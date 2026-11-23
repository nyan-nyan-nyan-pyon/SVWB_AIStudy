"""Evaluate all ordered WB deck pairs and export the row deck's win rate."""

import argparse
import csv
import json
from pathlib import Path

from match_runner import invoke


def load_decks(paths):
    decks = {}
    for path in paths:
        path = path.resolve(strict=True)
        name = json.loads(path.read_text(encoding="utf-8-sig")).get("name", path.stem)
        if not isinstance(name, str) or not name.strip() or "\n" in name or name in decks:
            raise ValueError(f"Deck names must be unique and fit on one line: {path}")
        decks[name] = path
    return decks


def load_pair_models(path, decks):
    if path is None:
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    models = {}
    for entry in document["matchups"]:
        pair = entry["deck_a"], entry["deck_b"]
        if not all(name in decks for name in pair) or pair in models:
            raise ValueError(f"Unknown or duplicate matchup in model manifest: {pair}")
        first = (path.parent / entry["weights"]).resolve(strict=True)
        second = (path.parent / entry.get("opponent_weights", entry["weights"])).resolve(strict=True)
        models[pair] = ["--weights", first, "--opponent-weights", second]
    if len(models) != len(decks) ** 2:
        raise ValueError("The model manifest must cover all ordered pairs")
    return models


def evaluate(args, decks, models):
    rows = []
    for name_a, deck_a in decks.items():
        row = []
        for name_b, deck_b in decks.items():
            result = invoke(args.exe, [
                "--rules", "wb", "--deck-a", deck_a, "--deck-b", deck_b, "--evaluate",
                "--games", args.games, "--threads", args.threads, "--seed", args.seed,
                "--simulations", args.simulations, *models.get((name_a, name_b), ()),
            ])
            if int(result["A_first"]) != args.games // 2 or int(result["A_second"]) != args.games // 2:
                raise RuntimeError(f"Unbalanced seats for {name_a} vs {name_b}")
            row.append((int(result["A_wins"]), int(result["B_wins"]), int(result["draws"])))
            print(f"{name_a} vs {name_b} {row[-1]}", flush=True)
        rows.append(row)
    return rows


def export(directory, names, counts, games, uses_weights):
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import font_manager, pyplot as plt

    fonts = {font.name for font in font_manager.fontManager.ttflist}
    plt.rcParams["font.family"] = next((font for font in ("Noto Sans CJK JP", "Yu Gothic", "Meiryo") if font in fonts),
                                      "DejaVu Sans")
    rates = [[a / games for a, _, _ in row] for row in counts]
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "win_rates.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["deck", *names])
        for name, row in zip(names, rates):
            writer.writerow([name, *(f"{value:.6f}" for value in row)])

    escaped = [name.replace("\\", "\\\\").replace("|", "\\|") for name in names]
    lines = ["# Matchup win rates", "",
             f"{games} games per ordered pair with equal first and second seats.",
             "Each cell gives the row deck's wins as a share of all games.",
             "Models are supplied per pair." if uses_weights else "Baseline seeded random weights are used for every pair.",
             "", "| Deck | " + " | ".join(escaped) + " |", "| --- | " + " | ".join("---:" for _ in names) + " |"]
    lines.extend(f"| {name} | " + " | ".join(f"{value:.1%}" for value in row) + " |"
                 for name, row in zip(escaped, rates))
    lines.extend(["", "| Deck A | Deck B | A wins | B wins | Both lose |", "| --- | --- | ---: | ---: | ---: |"])
    for name_a, row in zip(escaped, counts):
        lines.extend(f"| {name_a} | {name_b} | {a} | {b} | {both} |"
                     for name_b, (a, b, both) in zip(escaped, row))
    (directory / "win_rates.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    extent = max(5, 0.65 * len(names) + 3)
    fig, ax = plt.subplots(figsize=(extent + 1.4, extent), layout="constrained")
    heatmap = ax.imshow(rates, cmap="RdYlGn", vmin=0, vmax=1)
    ax.set_xticks(range(len(names)), names, rotation=35, ha="right")
    ax.set_yticks(range(len(names)), names)
    ax.set(xlabel="Opponent deck", ylabel="Row deck", title="Matchup win rates")
    fig.colorbar(heatmap, ax=ax, label="Row deck win rate")
    if len(names) <= 16:
        for i, row in enumerate(rates):
            for j, rate in enumerate(row):
                ax.text(j, i, f"{rate:.0%}", va="center", ha="center",
                        color="white" if rate < 0.16 or rate > 0.84 else "#1b1b1b")
    fig.savefig(directory / "heatmap.png", dpi=160)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decks", nargs="+", type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--models", type=Path, help="Optional JSON manifest with weights for each ordered matchup")
    parser.add_argument("--games", type=int, default=200)
    parser.add_argument("--threads", type=int, default=14)
    parser.add_argument("--simulations", type=int, default=64)
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    if args.games < 2 or args.games % 2 or args.threads < 1 or args.simulations < 1 or not 0 <= args.seed < 2**32:
        parser.error("Use an even game count, positive thread and simulation counts, and a uint32 seed")
    args.exe = args.exe.resolve(strict=True)
    decks = load_decks(args.decks)
    models = load_pair_models(args.models, decks)
    export(args.output_dir, list(decks), evaluate(args, decks, models), args.games, bool(models))


if __name__ == "__main__":
    main()
