"""Generate self-play, train on recent replay, and evaluate each generation."""

import argparse
import csv
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

import torch

from plot_metrics import plot_metrics
from train import generation_dir, initialize_checkpoint, pick_device, train_generation


ROOT = Path(__file__).resolve().parents[1]
LOG_FILES = ("val_log.bin", "pol_log.bin")


def invoke(executable: Path, arguments):
    command = [str(executable), *(str(value) for value in arguments)]
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(f"C++ match exited with {completed.returncode}: {completed.stderr.strip()}")
    counters = dict(re.findall(r"(?:^|\s)([A-Za-z_]+)=([^\s]+)", completed.stdout))
    required = ("games", "A_first", "A_second", "A_wins", "B_wins", "draws", "wall_seconds")
    if any(key not in counters for key in required):
        raise RuntimeError(f"C++ match did not emit counters: {completed.stdout.strip()}")
    games, first, second, wins, losses, draws = (int(counters[key]) for key in required[:-1])
    if not games or first + second != games or wins + losses + draws != games:
        raise RuntimeError("C++ match produced invalid game counts")
    return counters


def evaluate(args, weights, previous=None):
    first_count = 2 * (args.eval_games // 4)
    opponent = ["--opponent-weights", previous] if previous else ["--random-opponent"]
    wins = draws = 0
    eval_seed = (args.seed + 1_000_003) % 2**32
    for index, (deck_a, deck_b, count) in enumerate((
        ("A", "B", first_count), ("B", "A", args.eval_games - first_count),
    )):
        result = invoke(args.exe, [
            "--evaluate", "--weights", weights, *opponent, "--deck-a", deck_a, "--deck-b", deck_b,
            "--games", count, "--seed", (eval_seed + index) % 2**32, "--threads", args.threads,
            "--simulations", args.simulations,
        ])
        if int(result["games"]) != count or int(result["A_first"]) != count // 2:
            raise RuntimeError("Evaluation finished with an unexpected seat allocation")
        wins += int(result["A_wins"])
        draws += int(result["draws"])
    return wins, draws


def converged(rows, minimum):
    latest = rows[-1]
    generation = latest["generation"]
    if generation < max(15, minimum) or generation % 5:
        return False
    if latest["delta_v"] > 0.035 or latest["delta_pi"] > 0.042:
        return False
    last_fifteen = rows[-15:]
    means = [sum(row["selfplay_deck_a_win_rate"] for row in last_fifteen[start:start + 5]) / 5
             for start in (0, 5, 10)]
    return all(abs(current - previous) <= 0.03 for previous, current in zip(means, means[1:]))


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def publish_metrics(output, rows):
    write_json(output / "metrics.json", rows)
    csv_path = output / "metrics.csv"
    temporary = csv_path.with_suffix(".csv.tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            key for key in rows[0] if key != "replay_generations"
        ], extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(csv_path)
    plot_metrics(csv_path, output)


def config_for(args):
    keys = ("games", "seconds", "threads", "simulations", "eval_games", "epochs", "batch_size",
            "replay_window", "seed", "minimum_convergence", "device")
    return {key: getattr(args, key) for key in keys}


def initialize_run(args, device):
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    config = output / "config.json"
    settings = config_for(args)
    if config.exists():
        if not args.resume:
            raise ValueError("Output already has a run. Use --resume or choose another output directory.")
        if json.loads(config.read_text(encoding="utf-8")) != settings:
            raise ValueError("The run parameters changed. Use a new output directory for changed parameters.")
    elif args.resume:
        raise ValueError("Cannot resume an output directory without config.json")
    else:
        with config.open("x", encoding="utf-8") as stream:
            json.dump(settings, stream, indent=2)

    initial = generation_dir(output, 0)
    if not initial.exists():
        with tempfile.TemporaryDirectory(prefix=".initial-", dir=output) as staging:
            initialize_checkpoint(Path(staging), args.seed)
            Path(staging).rename(initial)
    rows = []
    for directory in sorted(output.glob("generation_[0-9]*"), key=lambda path: int(path.name.split("_")[-1])):
        if directory == initial:
            continue
        row = json.loads((directory / "metrics.json").read_text(encoding="utf-8"))
        if row["generation"] != len(rows) + 1 or directory != generation_dir(output, row["generation"]):
            raise ValueError("Completed generation directories are not contiguous")
        rows.append(row)
    print(f"Training device: {torch.cuda.get_device_name(device) if device.type == 'cuda' else 'CPU'}",
          flush=True)
    return rows


def run_generation(args, generation, rows, device):
    started = time.perf_counter()
    output = args.output_dir
    previous = generation_dir(output, generation - 1)
    target = generation_dir(output, generation)
    seed = (args.seed + generation) % 2**32
    print(f"Generation {generation}: self-play", flush=True)
    with tempfile.TemporaryDirectory(prefix=".generation-", dir=output) as stage_name:
        stage = Path(stage_name)
        with tempfile.TemporaryDirectory(prefix="svwb-selfplay-", dir=args.work_dir) as work_name:
            work = Path(work_name)
            limits = (["--games", args.games] if args.games else []) + (
                ["--seconds", args.seconds] if args.seconds else [])
            counters = invoke(args.exe, [
                *limits, "--threads", args.threads, "--simulations", args.simulations, "--seed", seed,
                "--deck-a", "A", "--deck-b", "B", "--weights", previous / "weights.bin", "--log-dir", work,
            ])
            for filename in LOG_FILES:
                shutil.copyfile(work / filename, stage / filename)

        trained_at = time.perf_counter()
        print(f"Generation {generation}: training on replay", flush=True)
        replay = [(number, generation_dir(output, number))
                  for number in range(max(1, generation - args.replay_window + 1), generation)]
        replay.append((generation, stage))
        reference = (stage if generation == 1 else generation_dir(output, 1)) / "evaluation.npz"
        losses = train_generation(replay, previous / "model.pt", stage, reference, generation,
                                  device=device, epochs=args.epochs, batch_size=args.batch_size, seed=seed)
        evaluating_at = time.perf_counter()
        print(f"Generation {generation}: evaluating", flush=True)
        weights = stage / "weights.bin"
        random_wins, random_draws = evaluate(args, weights)
        previous_wins, previous_draws = evaluate(args, weights, previous / "weights.bin")
        evaluated_at = time.perf_counter()
        selfplay_games = int(counters["games"])
        row = {
            "generation": generation, **losses,
            "random_win_rate": random_wins / args.eval_games,
            "previous_win_rate": previous_wins / args.eval_games,
            "random_wins": random_wins, "random_draws": random_draws,
            "previous_wins": previous_wins, "previous_draws": previous_draws,
            "evaluation_games": args.eval_games, "selfplay_games": selfplay_games,
            "selfplay_deck_a_win_rate": int(counters["A_wins"]) / selfplay_games,
            "selfplay_seconds": float(counters["wall_seconds"]),
            "train_seconds": evaluating_at - trained_at,
            "evaluation_seconds": evaluated_at - evaluating_at,
            "generation_seconds": evaluated_at - started,
        }
        row["converged"] = converged([*rows, row], args.minimum_convergence)
        write_json(stage / "metrics.json", row)
        stage.rename(target)
    return row


def parse_args():
    default_exe = (ROOT / "build/windows-release/Release/svwb_ai.exe"
                   if (ROOT / "build/windows-release/Release/svwb_ai.exe").exists()
                   else ROOT / "build/svwb_ai")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=default_exe)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "data/phase3")
    parser.add_argument("--work-dir", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--generations", type=int, default=10)
    parser.add_argument("--games", type=int, default=0, help="Self-play games per generation")
    parser.add_argument("--seconds", type=int, default=0, help="Self-play time limit per generation")
    parser.add_argument("--threads", type=int, default=14)
    parser.add_argument("--simulations", type=int, default=64)
    parser.add_argument("--eval-games", type=int, default=200, help="Games against each evaluation opponent")
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--replay-window", type=int, default=5)
    parser.add_argument("--minimum-convergence", type=int, default=20)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.games < 0 or args.seconds < 0 or not 0 <= args.seed < 2**32:
        parser.error("Invalid self-play limits or seed")
    if args.eval_games < 4 or args.eval_games % 2:
        parser.error("--eval-games must be even and at least four for balanced seats and decks")
    for key in ("generations", "threads", "simulations", "epochs", "batch_size", "replay_window", "minimum_convergence"):
        if getattr(args, key) < 1:
            parser.error(f"--{key.replace('_', '-')} must be positive")
    if not args.games and not args.seconds:
        args.games = 500
    args.exe = args.exe.resolve(strict=True)
    args.output_dir = args.output_dir.resolve()
    args.work_dir = args.work_dir.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    return args


def main():
    args = parse_args()
    device = pick_device(args.device)
    rows = initialize_run(args, device)
    if rows:
        publish_metrics(args.output_dir, rows)
    for generation in range(len(rows) + 1, args.generations + 1):
        if rows and rows[-1]["converged"]:
            break
        rows.append(run_generation(args, generation, rows, device))
        publish_metrics(args.output_dir, rows)
        row = rows[-1]
        print(f"Generation {generation}: value={row['val_loss']:.5f} policy={row['pol_loss']:.5f} "
              f"delta_v={row['delta_v']:.5f} delta_pi={row['delta_pi']:.5f} "
              f"random={row['random_win_rate']:.1%} previous={row['previous_win_rate']:.1%} "
              f"elapsed={row['generation_seconds']:.2f}s", flush=True)
    print(f"Completed generations: {len(rows)}, results: {args.output_dir}", flush=True)


if __name__ == "__main__":
    main()
