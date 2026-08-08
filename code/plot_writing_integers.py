#!/usr/bin/env python3
"""Generate the figures used by the Writing Decimal Integers case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "div10-copy": "/10 + copy",
    "pairs-copy": "/100 pairs + copy",
    "pairs-direct": "pairs, direct",
    "groups4-direct": "4-digit groups",
    "std-to-chars": "std::to_chars",
}


def load(path: Path):
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    return {
        (row["suite"], row["variant"]): float(row["ns_per_value"])
        for row in rows
    }


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


def ladder(data, output: Path):
    variants = [
        "div10-copy",
        "pairs-copy",
        "pairs-direct",
        "groups4-direct",
        "std-to-chars",
    ]
    times = [data[("uniform32", variant)] for variant in variants]
    values = [times[0] / value for value in times]
    colors = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8172b3"]

    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    positions = np.arange(len(variants))
    bars = ax.bar(positions, values, color=colors)
    ax.set_xticks(positions, [LABELS[variant] for variant in variants], rotation=12)
    ax.set_ylabel("Speedup over /10 + copy")
    ax.set_title("Integer formatting: one change at a time")
    ax.grid(axis="x", visible=False)
    labels = [f"{value:.2f}×" for value in values]
    ax.bar_label(bars, labels=labels, padding=3, fontsize=9, fontweight="bold")
    ax.set_ylim(0, max(values) * 1.16)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def distributions(data, output: Path):
    suites = ["uniform32", "uniform-digits", "counters"]
    suite_labels = ["Uniform uint32", "Uniform digit length", "Counter"]
    variants = [
        "div10-copy",
        "pairs-direct",
        "groups4-direct",
        "std-to-chars",
    ]
    x = np.arange(len(suites))
    width = 0.19

    colors = ["#4c72b0", "#55a868", "#c44e52", "#8172b3"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for index, variant in enumerate(variants):
        values = [data[(suite, variant)] for suite in suites]
        offset = (index - (len(variants) - 1) / 2) * width
        ax.bar(x + offset, values, width, label=LABELS[variant], color=colors[index])
    ax.set_xticks(x, suite_labels)
    ax.set_ylabel("Nanoseconds per value (lower is better)")
    ax.set_title("Input distribution changes the winner")
    ax.legend(ncols=2, frameon=True)
    ax.grid(axis="x", visible=False)
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
    data = load(args.csv)
    ladder(data, args.output_dir / "writing-integers-ladder.svg")
    distributions(data, args.output_dir / "writing-integers-distributions.svg")


if __name__ == "__main__":
    main()
