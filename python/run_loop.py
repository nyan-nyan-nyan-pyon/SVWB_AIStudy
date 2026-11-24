"""Generate self-play, train on recent replay, and evaluate each generation."""

import argparse
from collections import Counter
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import sys
import tempfile
import time

_ROOT_DIR = Path(__file__).resolve().parents[1]
_DEPS = _ROOT_DIR / "build/phase3-deps"
if _DEPS.is_dir() and str(_DEPS) not in sys.path:
    sys.path.append(str(_DEPS))
_MPL_DIR = _ROOT_DIR / "build/phase3-matplotlib"
if _MPL_DIR.is_dir():
    os.environ.setdefault("MPLCONFIGDIR", str(_MPL_DIR))

import torch

from plot_metrics import plot_metrics
from match_runner import invoke, seat_win_rates, win_counters
from run_storage import publish_checkpoint, replace_file
from train import (
    ATTN_WEIGHT_BYTES, WEIGHT_BYTES, generation_dir,
    initialize_attention_checkpoint, initialize_checkpoint, pick_device, train_generation,
)


ROOT = Path(__file__).resolve().parents[1]
LOG_FILES = ("val_log.bin", "pol_log.bin")


def weights_name(args):
    return "attn_weights.bin" if getattr(args, "model_type", "mlp") == "attention" else "weights.bin"


def wb_decks(args):
    flags = ["--rules", "wb"]
    if getattr(args, "model_type", "mlp") == "attention":
        flags.extend(("--model-type", "attention"))
    if args.perfect_information:
        flags.append("--perfect-information")
    for flag, deck in (("--deck-a", args.deck_a), ("--deck-b", args.deck_b)):
        if deck is not None:
            flags.extend((flag, deck))
    return flags


def evaluate(args, weights, opponent_weights=None):
    first_count = 2 * (args.eval_games // 4)
    assignments = (
        [(wb_decks(args), args.eval_games)]
        if args.rules == "wb"
        else [(["--deck-a", "A", "--deck-b", "B"], first_count),
              (["--deck-a", "B", "--deck-b", "A"], args.eval_games - first_count)]
    )
    opponent = ["--opponent-weights", opponent_weights] if opponent_weights else ["--random-opponent"]
    counts = Counter()
    eval_seed = (args.seed + 1_000_003) % 2**32
    for index, (deck_args, count) in enumerate(assignments):
        result = invoke(args.exe, [
            "--evaluate", "--weights", weights, *opponent, *deck_args,
            "--games", count, "--seed", (eval_seed + index) % 2**32, "--threads", args.threads,
            "--simulations", args.simulations,
        ])
        if int(result["games"]) != count or int(result["A_first"]) != count // 2:
            raise RuntimeError("Evaluation finished with an unexpected seat allocation")
        counts.update(win_counters(result))
    return counts


def converged(rows, minimum, rules="legacy"):
    latest = rows[-1]
    generation = latest["generation"]
    if generation < max(15, minimum) or generation % 5:
        return False
    if latest["delta_v"] > 0.035 or latest["delta_pi"] > 0.042:
        return False
    last_fifteen = rows[-15:]
    metric = "previous_win_rate" if rules == "wb" else "selfplay_deck_a_win_rate"
    means = [sum(row[metric] for row in last_fifteen[start:start + 5]) / 5
             for start in (0, 5, 10)]
    return all(abs(current - previous) <= 0.03 + 1e-12
               for previous, current in zip(means, means[1:]))


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                         encoding="utf-8")
    replace_file(temporary, path)


def publish_metrics(output, rows):
    write_json(output / "metrics.json", rows)
    csv_path = output / "metrics.csv"
    temporary = csv_path.with_suffix(".csv.tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        fields = dict.fromkeys(key for row in rows for key in row if key != "replay_generations")
        writer = csv.DictWriter(stream, fieldnames=list(fields), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    replace_file(temporary, csv_path)
    plot_metrics(csv_path, output)


def config_for(args):
    keys = ("games", "seconds", "threads", "simulations", "eval_games", "epochs", "batch_size",
            "replay_window", "seed", "minimum_convergence", "device", "rules", "snapshot_ratio")
    result = {key: getattr(args, key) for key in keys}
    if getattr(args, "model_type", "mlp") == "attention":
        result["model_type"] = "attention"
    if getattr(args, "learning_rate", None) is not None:
        result["learning_rate"] = args.learning_rate
    if getattr(args, "weight_decay", None) is not None:
        result["weight_decay"] = args.weight_decay
    if getattr(args, "dropout", 0.0):
        result["dropout"] = args.dropout
    if getattr(args, "value_label_smoothing", 0.0):
        result["value_label_smoothing"] = args.value_label_smoothing
    if args.rules == "wb":
        result["wb_actor_version"] = 2
    for key in ("deck_a", "deck_b", "baseline_weights"):
        path = getattr(args, key)
        if path is not None:
            result[key] = {"file": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    return result


def initialize_run(args, device):
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    config = output / "config.json"
    settings = config_for(args)
    if config.exists():
        if not args.resume:
            raise ValueError("Output already has a run. Use --resume or choose another output directory.")
        saved = json.loads(config.read_text(encoding="utf-8"))
        saved.setdefault("rules", "legacy")
        saved.setdefault("snapshot_ratio", 0.0)
        if args.rules == "wb" and saved.get("wb_actor_version") != settings["wb_actor_version"]:
            raise ValueError("This run uses an older WB feature schema. Start a new output directory for P0.")
        if saved != settings:
            raise ValueError("The run parameters changed. Use a new output directory for changed parameters.")
    elif args.resume:
        raise ValueError("Cannot resume an output directory without config.json")
    else:
        with config.open("x", encoding="utf-8") as stream:
            json.dump(settings, stream, indent=2)

    initial = generation_dir(output, 0)
    if not initial.exists():
        with tempfile.TemporaryDirectory(prefix=".initial-", dir=output, ignore_cleanup_errors=True) as staging:
            if getattr(args, "model_type", "mlp") == "attention":
                initialize_attention_checkpoint(Path(staging), args.seed)
            else:
                initialize_checkpoint(Path(staging), args.seed)
            publish_checkpoint(Path(staging), initial)
    verify_checkpoint(initial, 0, args.rules, getattr(args, "model_type", "mlp"))
    rows = []
    for directory in sorted(output.glob("generation_[0-9]*"), key=lambda path: int(path.name.split("_")[-1])):
        if directory == initial:
            continue
        row = json.loads((directory / "metrics.json").read_text(encoding="utf-8"))
        if row["generation"] != len(rows) + 1 or directory != generation_dir(output, row["generation"]):
            raise ValueError("Completed generation directories are not contiguous")
        verify_checkpoint(directory, row["generation"], args.rules, getattr(args, "model_type", "mlp"))
        rows.append(row)
    print(f"Training device: {torch.cuda.get_device_name(device) if device.type == 'cuda' else 'CPU'}",
          flush=True)
    return rows


def verify_checkpoint(directory, generation, rules, model_type="mlp"):
    required = ["model.pt"]
    if generation:
        required.extend(LOG_FILES)
        if generation == 1:
            required.append("evaluation.npz")
        if rules == "wb":
            required.append("matchup_features.json")
    for name in required:
        path = directory / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Incomplete checkpoint: {path}")
    if model_type == "attention":
        for name in ("attn_weights.bin", "weights.bin"):
            weights = directory / name
            if not weights.is_file() or weights.stat().st_size != ATTN_WEIGHT_BYTES:
                raise ValueError(f"Invalid attention checkpoint weights: {weights}")
    else:
        weights = directory / "weights.bin"
        if not weights.is_file() or weights.stat().st_size != WEIGHT_BYTES:
            raise ValueError(f"Invalid checkpoint weights: {weights}")


def snapshot_opponent(args, generation, baseline):
    if not args.snapshot_ratio:
        return [], None
    older = random.Random((args.seed << 32) | generation).randrange(max(1, generation - 1))
    weights = baseline if older == 0 else generation_dir(args.output_dir, older) / weights_name(args)
    source = "baseline" if older == 0 and args.baseline_weights else weights.parent.name
    return ["--snapshot-weights", weights, "--snapshot-bps", round(args.snapshot_ratio * 10_000)], source


def run_generation(args, generation, rows, device):
    started = time.perf_counter()
    output = args.output_dir
    previous = generation_dir(output, generation - 1)
    target = generation_dir(output, generation)
    seed = (args.seed + generation) % 2**32
    w_file = weights_name(args)
    baseline = args.baseline_weights or generation_dir(output, 0) / w_file
    snapshot_flags, snapshot_source = snapshot_opponent(args, generation, baseline)
    print(f"Generation {generation}: self-play", flush=True)
    with tempfile.TemporaryDirectory(prefix=".generation-", dir=output, ignore_cleanup_errors=True) as stage_name:
        stage = Path(stage_name)
        with tempfile.TemporaryDirectory(prefix="svwb-selfplay-", dir=args.work_dir, ignore_cleanup_errors=True) as work_name:
            work = Path(work_name)
            limits = (["--games", args.games] if args.games else []) + (
                ["--seconds", args.seconds] if args.seconds else [])
            deck_args = wb_decks(args) if args.rules == "wb" else ["--deck-a", "A", "--deck-b", "B"]
            counters = invoke(args.exe, [
                *limits, "--threads", args.threads, "--simulations", args.simulations, "--seed", seed,
                *deck_args, "--weights", previous / w_file, *snapshot_flags, "--log-dir", work,
            ])
            valid_schemas = ("wb-attention-v1",) if getattr(args, "model_type", "mlp") == "attention" else ("wb-mirror-v2", "wb-matchup-v2")
            if args.rules == "wb" and counters.get("feature_schema") not in valid_schemas:
                raise RuntimeError("The WB executable returned an unexpected feature schema")
            for filename in LOG_FILES:
                shutil.copyfile(work / filename, stage / filename)
            if (work / "matchup_features.json").is_file():
                shutil.copyfile(work / "matchup_features.json", stage / "matchup_features.json")

        trained_at = time.perf_counter()
        print(f"Generation {generation}: training on replay", flush=True)
        replay = [(number, generation_dir(output, number))
                  for number in range(max(1, generation - args.replay_window + 1), generation)]
        replay.append((generation, stage))
        reference = (stage if generation == 1 else generation_dir(output, 1)) / "evaluation.npz"
        losses = train_generation(replay, previous / "model.pt", stage, reference, generation,
                                  device=device, epochs=args.epochs, batch_size=args.batch_size, seed=seed,
                                  learning_rate=args.learning_rate,
                                  weight_decay=getattr(args, "weight_decay", None),
                                  dropout=getattr(args, "dropout", 0.0),
                                  value_label_smoothing=getattr(args, "value_label_smoothing", 0.0),
                                  model_type=getattr(args, "model_type", "mlp"))
        evaluating_at = time.perf_counter()
        print(f"Generation {generation}: evaluating", flush=True)
        weights = stage / w_file
        random_stats = evaluate(args, weights)
        previous_weights = previous / w_file
        previous_stats = evaluate(args, weights, previous_weights)
        baseline_stats = previous_stats if baseline == previous_weights else evaluate(args, weights, baseline)
        evaluated_at = time.perf_counter()
        selfplay_games = int(counters["games"])
        selfplay_stats = win_counters(counters)
        snapshot_games = int(counters["snapshot_games"]) if snapshot_flags else 0
        row = {
            "generation": generation, **losses,
            "evaluation_games": args.eval_games, "selfplay_games": selfplay_games,
            "snapshot_games": snapshot_games, "snapshot_source": snapshot_source,
            "selfplay_deck_a_win_rate": selfplay_stats["A_wins"] / selfplay_games,
            "information_mode": counters.get("information", "legacy"),
            **seat_win_rates(selfplay_stats, "selfplay"),
            "selfplay_seconds": float(counters["wall_seconds"]),
            "train_seconds": evaluating_at - trained_at,
            "evaluation_seconds": evaluated_at - evaluating_at,
            "generation_seconds": evaluated_at - started,
        }
        for name, stats in (("random", random_stats), ("previous", previous_stats), ("baseline", baseline_stats)):
            row[f"{name}_win_rate"] = stats["A_wins"] / stats["games"]
            row[f"{name}_wins"] = stats["A_wins"]
            row[f"{name}_draws"] = stats["draws"]
            row.update(seat_win_rates(stats, name))
        row["converged"] = converged([*rows, row], args.minimum_convergence, args.rules)
        write_json(stage / "metrics.json", row)
        publish_checkpoint(stage, target)
    return row


def parse_args():
    executables = [ROOT / "build/phase5c-p0-release/Release/svwb_ai.exe", ROOT / "build/Release/svwb_ai.exe",
                   ROOT / "build/phase5a-release/Release/svwb_ai.exe",
                   ROOT / "build/windows-release/Release/svwb_ai.exe", ROOT / "build/svwb_ai"]
    default_exe = next((path for path in executables if path.is_file()), executables[-1])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=default_exe)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--rules", choices=("legacy", "wb"))
    parser.add_argument("--deck-a", type=Path)
    parser.add_argument("--deck-b", type=Path)
    default_work_dir = ROOT / "build" if (ROOT / "build").is_dir() else Path(tempfile.gettempdir())
    parser.add_argument("--work-dir", type=Path, default=default_work_dir)
    parser.add_argument("--generations", type=int, default=10)
    parser.add_argument("--games", type=int, default=0, help="Self-play games per generation")
    parser.add_argument("--seconds", type=int, default=0, help="Self-play time limit per generation")
    parser.add_argument("--threads", type=int, default=14)
    parser.add_argument("--simulations", type=int, default=64)
    parser.add_argument("--eval-games", type=int, default=200, help="Games against each evaluation opponent")
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--learning-rate", "--lr", dest="learning_rate", type=float,
                        help="Override the checkpoint's Adam learning rate")
    parser.add_argument("--weight-decay", type=float, help="Override the checkpoint's weight decay")
    parser.add_argument("--dropout", type=float, default=0.0,
                        help="Training dropout probability for Attention model")
    parser.add_argument("--value-label-smoothing", type=float, default=0.0,
                        help="Label smoothing factor for value targets")
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--replay-window", type=int, default=5)
    parser.add_argument("--minimum-convergence", type=int, default=20)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--snapshot-ratio", type=float, default=0.2,
                        help="Fraction of self-play games against an older checkpoint (0 disables)")
    parser.add_argument("--baseline-weights", type=Path,
                        help="Fixed evaluation baseline (default: generation 0 of this run)")
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--perfect-information", action="store_true",
                        help="Use the previous WB search that sees all cards")
    parser.add_argument("--model-type", choices=("mlp", "attention"), default="mlp")
    args = parser.parse_args()
    custom_decks = args.deck_a is not None or args.deck_b is not None
    if custom_decks and args.rules == "legacy":
        parser.error("Deck JSON files require WB rules")
    if args.model_type == "attention" and args.rules == "legacy":
        parser.error("--model-type attention requires WB rules")
    args.rules = args.rules or ("wb" if (custom_decks or args.model_type == "attention") else "legacy")
    if args.perfect_information and args.rules != "wb":
        parser.error("--perfect-information requires WB rules")
    if custom_decks and args.output_dir is None:
        parser.error("Specify --output-dir for a custom deck run")
    for key in ("deck_a", "deck_b", "baseline_weights"):
        path = getattr(args, key)
        if path is not None:
            setattr(args, key, path.resolve(strict=True))
    if args.games < 0 or args.seconds < 0 or not 0 <= args.seed < 2**32:
        parser.error("Invalid self-play limits or seed")
    if not 0 <= args.snapshot_ratio <= 1:
        parser.error("--snapshot-ratio must be between 0 and 1")
    args.snapshot_ratio = round(args.snapshot_ratio, 4)
    if args.learning_rate is not None and (not math.isfinite(args.learning_rate) or args.learning_rate <= 0):
        parser.error("--learning-rate must be finite and positive")
    if args.weight_decay is not None and (not math.isfinite(args.weight_decay) or args.weight_decay < 0):
        parser.error("--weight-decay must be finite and non-negative")
    if not (math.isfinite(args.dropout) and 0.0 <= args.dropout < 1.0):
        parser.error("--dropout must be between 0.0 and 1.0")
    if not (math.isfinite(args.value_label_smoothing) and 0.0 <= args.value_label_smoothing < 0.5):
        parser.error("--value-label-smoothing must be between 0.0 and 0.5")
    if args.eval_games < 4 or args.eval_games % 2:
        parser.error("--eval-games must be even and at least four for balanced seats and decks")
    for key in ("generations", "threads", "simulations", "epochs", "batch_size", "replay_window", "minimum_convergence"):
        if getattr(args, key) < 1:
            parser.error(f"--{key.replace('_', '-')} must be positive")
    if not args.games and not args.seconds:
        args.games = 500
    args.exe = args.exe.resolve(strict=True)
    default_output = ROOT / ("data/phase5c-mirror" if args.rules == "wb" else "data/phase3")
    args.output_dir = (args.output_dir or default_output).resolve()
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
              f"baseline={row['baseline_win_rate']:.1%} snapshots={row['snapshot_games']}/{row['selfplay_games']} "
              f"elapsed={row['generation_seconds']:.2f}s", flush=True)
    print(f"Completed generations: {len(rows)}, results: {args.output_dir}", flush=True)


if __name__ == "__main__":
    main()
