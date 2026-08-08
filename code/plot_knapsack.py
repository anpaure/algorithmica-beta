#!/usr/bin/env python3
"""Generate the figures used by the Knapsack with Bitsets case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "byte-full": "byte DP, full",
    "byte-bounded": "byte DP, bounded",
    "words-full": "packed words, full",
    "words-bounded": "packed words, bounded",
}
DEEP = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]


def configure():
    plt.style.use("default")
    plt.rcParams.update(
        {
            "figure.figsize": (6.4, 4.8),
            "figure.facecolor": "white",
            "savefig.facecolor": "white",
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.facecolor": "white",
            "axes.edgecolor": "#ccc",
            "axes.labelcolor": "#262626",
            "axes.titlecolor": "#262626",
            "axes.linewidth": 1.25,
            "axes.titlesize": 12,
            "axes.labelsize": 12,
            "axes.grid": True,
            "axes.axisbelow": True,
            "grid.color": "#ccc",
            "grid.linewidth": 0.8,
            "lines.linewidth": 1.5,
            "lines.solid_capstyle": "round",
            "lines.dash_capstyle": "round",
            "patch.edgecolor": "white",
            "patch.force_edgecolor": True,
            "patch.linewidth": 1.0,
            "text.color": "#262626",
            "xtick.color": "#262626",
            "ytick.color": "#262626",
            "xtick.labelsize": 11,
            "ytick.labelsize": 11,
            "legend.fontsize": 11,
            "legend.edgecolor": "#ccc",
            "legend.facecolor": "white",
            "legend.framealpha": 0.8,
            "savefig.bbox": None,
            "svg.fonttype": "path",
        }
    )


def load(path: Path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def stages(rows, output: Path):
    suites = ["dense-100k", "dense-1m", "wide-weights-1m", "sparse-frontier-5m"]
    titles = ["Dense, W=10⁵", "Dense, W=10⁶", "Wide weights", "Sparse frontier"]
    by_key = {
        (row["suite"], row["variant"]): float(row["milliseconds"])
        for row in rows if row["kind"] == "stage"
    }
    variants = ["byte-full", "byte-bounded", "words-full", "words-bounded"]
    fig, axes = plt.subplots(2, 2, figsize=(6.4, 4.8), sharey=False)
    for ax, suite, title in zip(axes.flat, suites, titles):
        baseline = by_key[(suite, "byte-full")]
        speedups = [baseline / by_key[(suite, variant)] for variant in variants]
        bars = ax.bar(np.arange(4), speedups, color=DEEP)
        ax.set_xticks(np.arange(4), ["byte\nfull", "byte\nbounded", "words\nfull", "words\nbounded"])
        ax.set_title(title)
        ax.grid(axis="x", visible=False)
        ax.bar_label(
            bars,
            labels=[f"{value:.1f}×" for value in speedups],
            padding=2,
            fontsize=7.5,
            fontweight="bold",
        )
        ax.set_ylim(0, max(speedups) * 1.2)
    axes[0, 0].set_ylabel("Speedup over full byte DP")
    axes[1, 0].set_ylabel("Speedup over full byte DP")
    fig.suptitle("Packing and frontier bounds solve different bottlenecks", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(output, format="svg")
    plt.close(fig)


def frontier(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "frontier"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in [("words-full", "#8b0000"),
                           ("words-bounded", "#00008b")]:
        current = sorted(
            (row for row in selected if row["variant"] == variant),
            key=lambda row: float(row["frontier_ratio"]),
        )
        ax.plot(
            [float(row["frontier_ratio"]) for row in current],
            [float(row["milliseconds"]) for row in current],
            color=color,
            linewidth=1.5,
            label=LABELS[variant],
        )
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Total usable weight / capacity")
    ax.set_ylabel("Milliseconds")
    ax.set_title("Do not scan states that are known to be zero")
    ax.legend(frameon=True)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def size_sweep(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "size"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in [("byte-bounded", "#8b0000"),
                           ("words-bounded", "#00008b")]:
        current = sorted(
            (row for row in selected if row["variant"] == variant),
            key=lambda row: int(row["capacity"]),
        )
        ax.plot(
            [np.log2(int(row["capacity"])) for row in current],
            [float(row["milliseconds"]) for row in current],
            color=color,
            linewidth=1.5,
            label=LABELS[variant],
        )
    for exponent, label in [
        (17, "byte L1D"),
        (20, "word L1D"),
        (24, "byte L2"),
    ]:
        ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
        ax.text(exponent + 0.12, ax.get_ylim()[1] * 0.55, label, rotation=90,
                va="top", fontsize=7.5)
    ax.set_yscale("log")
    ticks = list(range(10, 25, 2))
    ax.set_xticks(ticks, [f"$2^{{{tick}}}$" for tick in ticks])
    ax.set_xlabel("Capacity W")
    ax.set_ylabel("Milliseconds")
    ax.set_title("Packed and byte states across the M4 memory hierarchy")
    ax.legend(frameon=True)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    configure()
    rows = load(args.csv)
    stages(rows, args.output_dir / "knapsack-stages.svg")
    frontier(rows, args.output_dir / "knapsack-frontier.svg")
    size_sweep(rows, args.output_dir / "knapsack-size.svg")


if __name__ == "__main__":
    main()
