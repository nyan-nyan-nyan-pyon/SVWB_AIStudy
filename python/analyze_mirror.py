"""Run the final noise-free mirror and export the analysis bundle."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import time

from run_loop import ROOT, invoke, write_json
from train import generation_dir


TRACE_FILES = ("first_win_trace.json", "second_win_trace.json")


def match_summary(counters, expected):
    fields = ("games", "A_first", "A_second", "A_wins", "B_wins", "first_wins", "second_wins",
              "both_lose", "turns")
    summary = {key: int(counters[key]) for key in fields}
    if (summary["games"] != expected or summary["A_first"] != expected // 2
            or summary["A_second"] != expected // 2
            or summary["first_wins"] + summary["second_wins"] + summary["both_lose"] != expected):
        raise RuntimeError("Final evaluation finished with unexpected game counts")
    summary.update({key: float(counters[key]) for key in ("avg_turns", "avg_rounds", "wall_seconds")})
    summary["first_win_rate"] = summary["first_wins"] / expected
    summary["second_win_rate"] = summary["second_wins"] / expected
    summary["both_lose_rate"] = summary["both_lose"] / expected
    summary["competitor_a_win_rate"] = summary["A_wins"] / expected
    return summary


def verify_artifacts(path, mirror):
    with (path / "opening_win_rates.csv").open(encoding="utf-8", newline="") as stream:
        table = list(csv.DictReader(stream))
    by_seat = {seat: [row for row in table if row["seat"] == seat] for seat in ("first", "second", "combined")}
    if len(table) != 45 or any(len(rows) != 15 for rows in by_seat.values()):
        raise RuntimeError("Expected 15 opening-card rows for each of the three seat groups")
    for seat, rows in by_seat.items():
        expected = mirror["games"] if seat == "combined" else mirror["games"] // 2
        for row in rows:
            for with_key, without_key in (("kept", "not_kept"), ("present", "absent")):
                if int(row[f"{with_key}_games"]) + int(row[f"{without_key}_games"]) != expected:
                    raise RuntimeError(f"Opening counts do not match {seat}: {row['card_name']}")
    for index, filename in enumerate(TRACE_FILES, 1):
        wins = mirror["first_wins" if index == 1 else "second_wins"]
        if wins:
            trace = json.loads((path / filename).read_text(encoding="utf-8"))
            if trace["winner_seat"] != index or not trace["turns"]:
                raise RuntimeError(f"Invalid trace {filename}")


def report(summary):
    mirror = summary["mirror"]
    baseline = summary["random_opponent"]
    no_winner = mirror["both_lose"]
    status = "収束条件を満たした" if summary["converged"] else "実行上限時点の"
    traces = "・".join(
        f"[{label}]({name})"
        for label, name, wins in zip(("先攻勝利の手順", "後攻勝利の手順"), TRACE_FILES,
                                     (mirror["first_wins"], mirror["second_wins"]))
        if wins
    )
    windows = "、".join(
        f"第{item['first']}〜{item['last']}世代 {item['rate']:.2%}"
        for item in summary["previous_win_rate_windows"]
    )
    stability = f"対前世代の5世代平均は {windows}。" if windows else ""
    return f"""# SVWB 確定ミラー解析

{status}第{summary["generation"]}世代のモデルを評価した。確定40枚デッキを両側で使用し、探索ノイズと温度サンプリングを無効にした。

| 指標 | 結果 |
| --- | ---: |
| ミラー試合数 | {mirror["games"]:,} |
| 先攻勝率 | {mirror["first_win_rate"]:.2%}（{mirror["first_wins"]:,}勝） |
| 後攻勝率 | {mirror["second_win_rate"]:.2%}（{mirror["second_wins"]:,}勝） |
| 引き分け・両者敗北率 | {mirror["both_lose_rate"]:.2%}（両者敗北{no_winner}件） |
| 平均手番数 | {mirror["avg_turns"]:.2f} |
| 平均決着ラウンド | {mirror["avg_rounds"]:.2f} |
| 対ランダム勝率 | {baseline["competitor_a_win_rate"]:.2%}（{baseline["A_wins"]}/{baseline["games"]}） |
| 最終 ΔV | {summary["delta_v"]:.5f} |
| 最終 Δπ | {summary["delta_pi"]:.5f} |
| 学習用自己対戦 | {summary["training_games"]:,}試合 |
| 世代処理時間の合計 | {summary["generation_seconds"]:.2f}秒 |

{stability}

競技者Aをミラーの先攻・後攻に{mirror["A_first"]:,}試合ずつ割り当てた。初手CSVはA視点で先攻・後攻・合算を集計した。保持は交換前の4枚から残した現物、初手存在は交換後かつ第1ドロー前の4枚にあることを指す。差は観測された勝率の差で、単位はパーセントポイント。比較群が空なら空欄とした。

手順ログには選んだマリガン、実行した各ターンの行動、MCTSの上位候補を収録した。評価シードはミラーが `{summary["mirror_seed"]}`、対ランダムが `{summary["random_seed"]}`。

- [初手カード別集計](opening_win_rates.csv)
- {traces or "勝利手順の該当試合なし"}
- [損失](losses.png)・[ΔV / Δπ](drift.png)・[勝率](win_rates.png)
- [全世代の指標](metrics.csv)・[最終評価の集計](evaluation.json)
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "data/phase4b-mirror")
    parser.add_argument("--work-dir", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--threads", type=int, default=14)
    parser.add_argument("--mirror-games", type=int, default=2000)
    parser.add_argument("--random-games", type=int, default=200)
    parser.add_argument("--seed", type=int, default=722026)
    args = parser.parse_args()
    if any(count < 2 or count % 2 for count in (args.mirror_games, args.random_games)):
        parser.error("Game counts must be positive and even for balanced seats")
    if not 0 <= args.seed < 2**32 - 1:
        parser.error("The seed and its successor must fit in uint32")
    default_exe = ROOT / "build/windows-release/Release/svwb_ai.exe"
    if not default_exe.exists():
        default_exe = ROOT / "build/svwb_ai"
    executable = (args.exe or default_exe).resolve(strict=True)
    output = args.output_dir.resolve()
    rows = json.loads((output / "metrics.json").read_text(encoding="utf-8"))
    config = json.loads((output / "config.json").read_text(encoding="utf-8"))
    if not rows or config.get("rules") != "wb":
        raise ValueError("A completed WB mirror training run is required")
    latest = rows[-1]
    gen = latest["generation"]
    weights = generation_dir(output, gen) / "weights.bin"
    args.work_dir.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="svwb-final-", dir=args.work_dir) as scratch:
        common = ["--rules", "wb", "--evaluate", "--weights", weights,
                  "--threads", args.threads, "--simulations", config["simulations"]]
        print(f"Evaluating generation {gen}: {args.mirror_games} mirrors", flush=True)
        mirror = match_summary(invoke(executable, [
            *common, "--games", args.mirror_games, "--seed", args.seed, "--analysis-dir", scratch,
        ]), args.mirror_games)
        print(f"Evaluating generation {gen}: {args.random_games} games against random actions", flush=True)
        baseline = match_summary(invoke(executable, [
            *common, "--games", args.random_games, "--seed", args.seed + 1, "--random-opponent",
        ]), args.random_games)
        verify_artifacts(Path(scratch), mirror)
        for name in ("opening_win_rates.csv", *TRACE_FILES):
            source = Path(scratch) / name
            if source.is_file():
                temporary = output / f"{name}.tmp"
                shutil.copyfile(source, temporary)
                temporary.replace(output / name)
    windows = []
    if len(rows) >= 15:
        recent = rows[-15:]
        for start in (0, 5, 10):
            batch = recent[start:start + 5]
            windows.append({
                "first": batch[0]["generation"], "last": batch[-1]["generation"],
                "rate": sum(row["previous_win_rate"] for row in batch) / len(batch),
            })
    summary = {
        "generation": gen, "converged": latest["converged"],
        "delta_v": latest["delta_v"], "delta_pi": latest["delta_pi"],
        "weights_sha256": hashlib.sha256(weights.read_bytes()).hexdigest(),
        "previous_win_rate_windows": windows,
        "simulations": config["simulations"], "mirror_seed": args.seed, "random_seed": args.seed + 1,
        "training_games": sum(row["selfplay_games"] for row in rows),
        "generation_seconds": sum(row["generation_seconds"] for row in rows),
        "mirror": mirror, "random_opponent": baseline, "elapsed_seconds": time.perf_counter() - started,
    }
    write_json(output / "evaluation.json", summary)
    (output / "report.md").write_text(report(summary), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
