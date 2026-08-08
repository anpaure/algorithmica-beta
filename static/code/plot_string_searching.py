#!/usr/bin/env python3
"""Generate the figures used by the String Searching case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "naive": "naive memcmp",
    "first-byte-neon": "one-byte NEON",
    "two-byte-neon": "two-byte NEON",
    "rare-byte-neon": "rare-byte NEON",
    "adaptive-kmp": "adaptive + KMP",
    "kmp": "KMP",
    "std-search": "std::search",
}
DEEP = [
    "#4c72b0", "#dd8452", "#55a868", "#c44e52",
    "#8172b3", "#937860", "#da8bc3",
]


def load(path: Path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


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


def stage_plot(rows, output: Path):
    selected = [
        row for row in rows
        if row["kind"] == "workload" and row["suite"] == "random-absent"
    ]
    baseline = float(next(row["ns_per_byte"] for row in selected if row["variant"] == "naive"))
    values = [baseline / float(row["ns_per_byte"]) for row in selected]
    labels = [LABELS[row["variant"]] for row in selected]

    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    bars = ax.bar(np.arange(len(values)), values, color=DEEP[: len(values)])
    ax.set_xticks(np.arange(len(values)), labels, rotation=14)
    ax.set_ylabel("Speedup over naive search")
    ax.set_title("Filtering candidate positions")
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


def workload_plot(rows, output: Path):
    suites = [
        "random-absent",
        "alphabet4-absent",
        "periodic-middle-miss",
        "repetitive-window-miss",
    ]
    labels = ["random bytes", "4-symbol", "rare middle byte", "repetitive windows"]
    variants = ["naive", "rare-byte-neon", "adaptive-kmp"]
    by_key = {
        (row["suite"], row["variant"]): float(row["ns_per_byte"])
        for row in rows if row["kind"] == "workload"
    }
    x = np.arange(len(suites))
    width = 0.24
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for index, variant in enumerate(variants):
        values = [by_key[(suite, variant)] for suite in suites]
        ax.bar(
            x + (index - 1) * width,
            values,
            width,
            label=LABELS[variant],
            color=DEEP[index],
        )
    ax.set_xticks(x, labels, rotation=8)
    ax.set_ylabel("Nanoseconds per haystack byte (lower is better)")
    ax.set_title("The best filter depends on the byte distribution")
    ax.legend(frameon=True)
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def size_plot(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "size"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in [("naive", "#8b0000"), ("two-byte-neon", "#00008b")]:
        current = sorted(
            (row for row in selected if row["variant"] == variant),
            key=lambda row: int(row["size"]),
        )
        x = [np.log2(int(row["size"])) for row in current]
        y = [float(row["ns_per_byte"]) for row in current]
        ax.plot(x, y, color=color, linewidth=1.5, label=LABELS[variant])
    for exponent, label in [(17, "128 KiB L1D"), (24, "16 MiB L2")]:
        ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
        ax.text(exponent + 0.15, ax.get_ylim()[1] * 0.82, label, rotation=90,
                va="top", fontsize=8)
    ticks = list(range(10, 27, 2))
    ax.set_xticks(ticks, [f"$2^{{{tick}}}$" for tick in ticks])
    ax.set_xlabel("Haystack size (bytes)")
    ax.set_ylabel("Nanoseconds per haystack byte")
    ax.set_title("Search throughput across the memory hierarchy")
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
    stage_plot(rows, args.output_dir / "string-searching-stages.svg")
    workload_plot(rows, args.output_dir / "string-searching-workloads.svg")
    size_plot(rows, args.output_dir / "string-searching-size.svg")


if __name__ == "__main__":
    main()
