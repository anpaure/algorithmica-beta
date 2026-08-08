#!/usr/bin/env python3
"""Generate the figures used by the Reading Decimal Integers case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "scalar-horner": "scalar Horner",
    "serial-neon": "serial NEON",
    "four-stream-scalar": "4-stream scalar",
    "four-stream-neon": "4-stream NEON",
    "std-from-chars": "std::from_chars",
}
DEEP = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8172b3"]


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
    selected = [
        row for row in rows
        if row["kind"] == "workload" and row["suite"] == "uniform-numeric"
    ]
    baseline = float(selected[0]["ns_per_value"])
    values = [baseline / float(row["ns_per_value"]) for row in selected]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    bars = ax.bar(np.arange(len(values)), values, color=DEEP)
    ax.set_xticks(
        np.arange(len(values)),
        [LABELS[row["variant"]] for row in selected],
        rotation=12,
    )
    ax.set_ylabel("Speedup over scalar Horner")
    ax.set_title("Parsing newline-delimited integers")
    ax.grid(axis="x", visible=False)
    ax.bar_label(
        bars,
        labels=[f"{value:.2f}×" for value in values],
        padding=3,
        fontsize=9,
        fontweight="bold",
    )
    ax.set_ylim(0, max(values) * 1.16)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def distributions(rows, output: Path):
    suites = ["uniform-numeric", "fixed-8-digit", "uniform-digits"]
    suite_labels = ["Uniform [0, 10⁸)", "Fixed 8 digits", "Uniform length"]
    variants = ["scalar-horner", "serial-neon", "four-stream-scalar", "std-from-chars"]
    by_key = {
        (row["suite"], row["variant"]): float(row["ns_per_value"])
        for row in rows if row["kind"] == "workload"
    }
    x = np.arange(len(suites))
    width = 0.19
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for index, variant in enumerate(variants):
        values = [by_key[(suite, variant)] for suite in suites]
        ax.bar(
            x + (index - 1.5) * width,
            values,
            width,
            label=LABELS[variant],
            color=DEEP[index],
            edgecolor="white",
            linewidth=1.0,
        )
    ax.set_xticks(x, suite_labels)
    ax.set_ylabel("Nanoseconds per integer (lower is better)")
    ax.set_title("Record length changes the SIMD tradeoff")
    ax.legend(frameon=True, ncols=2)
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def size_sweep(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "size"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in [("scalar-horner", "#8b0000"),
                           ("four-stream-scalar", "#00008b")]:
        current = sorted(
            (row for row in selected if row["variant"] == variant),
            key=lambda row: int(row["size"]),
        )
        ax.plot(
            [np.log2(int(row["size"])) for row in current],
            [float(row["ns_per_byte"]) for row in current],
            color=color,
            linewidth=1.5,
            label=LABELS[variant],
        )
    for exponent, label in [(17, "128 KiB L1D"), (24, "16 MiB L2")]:
        ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.9)
        ax.text(exponent + 0.15, ax.get_ylim()[1] * 0.82, label, rotation=90,
                va="top", fontsize=8)
    ticks = list(range(10, 27, 2))
    ax.set_xticks(ticks, [f"$2^{{{tick}}}$" for tick in ticks])
    ax.set_xlabel("Encoded input size (bytes)")
    ax.set_ylabel("Nanoseconds per input byte")
    ax.set_title("Four independent streams expose instruction-level parallelism")
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
    stages(rows, args.output_dir / "reading-integers-stages.svg")
    distributions(rows, args.output_dir / "reading-integers-distributions.svg")
    size_sweep(rows, args.output_dir / "reading-integers-size.svg")


if __name__ == "__main__":
    main()
