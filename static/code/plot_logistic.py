#!/usr/bin/env python3
"""Generate the figures used by the Optimizing Logistic Regression case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "float-softmax": "float + softmax",
    "float-argmax": "float argmax",
    "scalar-int8": "scalar int8",
    "neon-int8": "NEON int8",
    "neon4-int8": "4-way NEON int8",
    "batch4-int8": "batch-4 int8",
}
DEEP = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8172b3", "#937860"]


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


def performance(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "performance"]
    baseline = float(selected[0]["value"])
    speedups = [baseline / float(row["value"]) for row in selected]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    bars = ax.bar(np.arange(len(speedups)), speedups, color=DEEP)
    ax.set_xticks(
        np.arange(len(speedups)),
        [LABELS[row["variant"]] for row in selected],
        rotation=14,
    )
    ax.set_ylabel("Speedup over float + softmax")
    ax.set_title("Synthetic 784×10 inference: measured implementation ladder")
    ax.grid(axis="x", visible=False)
    ax.bar_label(
        bars,
        labels=[f"{value:.2f}×" for value in speedups],
        padding=3,
        fontsize=9,
        fontweight="bold",
    )
    ax.set_ylim(0, max(speedups) * 1.15)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def quality(rows, output: Path):
    selected = sorted(
        (row for row in rows if row["kind"] == "disagreement"),
        key=lambda row: int(row["variant"]),
    )
    bits = [int(row["variant"]) for row in selected]
    disagreement = [float(row["value"]) for row in selected]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    ax.plot(bits, disagreement, color="#00008b", linewidth=1.5)
    ax.set_xticks(bits)
    ax.set_xlabel("Signed quantization bits")
    ax.set_ylabel("Prediction disagreement with float argmax (%)")
    ax.set_title("Quantization changes predictions on the synthetic dataset")
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
    performance(rows, args.output_dir / "logistic-stages.svg")
    quality(rows, args.output_dir / "logistic-disagreement.svg")


if __name__ == "__main__":
    main()
