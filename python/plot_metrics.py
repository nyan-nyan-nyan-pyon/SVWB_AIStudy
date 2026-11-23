"""Render the loss, model drift, and evaluation curves as PNGs."""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
from matplotlib import pyplot as plt
from matplotlib.ticker import MaxNLocator, PercentFormatter


VALUE_COLOR = "#2563a5"
POLICY_COLOR = "#b65f29"


def plot_metrics(metrics: Path, output: Path):
    with metrics.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError("At least one completed generation is required")
    generations = [int(row["generation"]) for row in rows]
    output.mkdir(parents=True, exist_ok=True)

    def curve(ax, key, label, color):
        ax.plot(generations, [float(row[key]) for row in rows], color=color,
                marker="o", markersize=4, linewidth=2, label=label)

    def save(fig, filename):
        for ax in fig.axes:
            ax.set_xlabel("Generation")
            ax.xaxis.set_major_locator(MaxNLocator(integer=True))
            ax.spines[["top", "right"]].set_visible(False)
            ax.grid(axis="y", color="#e3e8ef")
            ax.set_axisbelow(True)
            ax.legend(frameon=False)
        target = output / filename
        temporary = target.with_suffix(".png.tmp")
        fig.savefig(temporary, dpi=150, format="png")
        plt.close(fig)
        temporary.replace(target)

    fig, axes = plt.subplots(1, 2, figsize=(8.6, 3.9), layout="constrained")
    fig.suptitle("Training losses", x=0.04, ha="left", fontsize=15, weight="bold")
    curve(axes[0], "val_loss", "Value BCE", VALUE_COLOR)
    curve(axes[1], "pol_loss", "Policy cross-entropy", POLICY_COLOR)
    for ax in axes:
        ax.set_ylabel("Loss")
    save(fig, "losses.png")

    fig, ax = plt.subplots(figsize=(7.5, 4.2), layout="constrained")
    ax.set_title("Change from the previous model", loc="left", weight="bold")
    curve(ax, "delta_v", "ΔV", VALUE_COLOR)
    curve(ax, "delta_pi", "Δπ", POLICY_COLOR)
    ax.axhline(0.035, color=VALUE_COLOR, linestyle="--", alpha=0.55, label="ΔV threshold")
    ax.axhline(0.042, color=POLICY_COLOR, linestyle=":", alpha=0.65, label="Δπ threshold")
    ax.set_ylabel("Mean change")
    ax.set_ylim(bottom=0)
    save(fig, "drift.png")

    fig, ax = plt.subplots(figsize=(7.5, 4.2), layout="constrained")
    ax.set_title("Evaluation win rates", loc="left", weight="bold")
    curve(ax, "random_win_rate", "Against random moves", VALUE_COLOR)
    curve(ax, "previous_win_rate", "Against previous model", POLICY_COLOR)
    ax.axhline(0.90, color="#64748b", linestyle="--", label="90% target")
    ax.set_ylabel("Wins / games")
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0))
    ax.set_ylim(0, 1.025)
    save(fig, "win_rates.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metrics", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    plot_metrics(args.metrics, args.output_dir or args.metrics.parent)


if __name__ == "__main__":
    main()
