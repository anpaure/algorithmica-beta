#!/usr/bin/env python3
"""Generate the figures used by the floating-point I/O case study."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter


COLORS = ["#8b0000", "#00008b", "#006400", "#b8860b"]
LABELS = {
    "strtod-copy": "strtod + copy",
    "strtod-direct": "strtod direct",
    "scalar-exact": "fixed scalar",
    "neon-exact": "fixed NEON",
    "snprintf": "snprintf",
    "div10-fixed": "fixed /10",
    "pairs-fixed": "fixed /100 pairs",
}


def configure():
    plt.style.use("default")
    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.titlesize": 11,
            "axes.labelsize": 10,
            "axes.grid": True,
            "axes.axisbelow": True,
            "axes.facecolor": "white",
            "axes.edgecolor": "#ccc",
            "axes.labelcolor": "#262626",
            "axes.linewidth": 1.25,
            "figure.facecolor": "white",
            "grid.color": "#ccc",
            "grid.linewidth": 0.8,
            "lines.solid_capstyle": "round",
            "patch.edgecolor": "white",
            "patch.linewidth": 1.0,
            "savefig.bbox": None,
            "svg.fonttype": "path",
            "text.color": "#262626",
            "xtick.color": "#262626",
            "ytick.color": "#262626",
        }
    )


def load(path: Path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def speedup_plot(rows, kind, variants, title, output):
    by_name = {
        row["variant"]: float(row["value"])
        for row in rows if row["kind"] == kind
    }
    baseline = by_name[variants[0]]
    speedups = [baseline / by_name[variant] for variant in variants]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    bars = ax.bar(
        np.arange(len(variants)), speedups, color=COLORS[: len(variants)],
        edgecolor="white", linewidth=1.0,
    )
    ax.set_xticks(np.arange(len(variants)), [LABELS[variant] for variant in variants])
    ax.set_ylabel("Speedup over general baseline")
    ax.set_title(title)
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


def power_of_two(value, _position):
    return rf"$2^{{{int(round(value))}}}$"


def size_plot(rows, output):
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in (("scalar-exact", "#8b0000"),
                           ("neon-exact", "#00008b")):
        data = sorted(
            (row for row in rows
             if row["kind"] == "parse-size" and row["variant"] == variant),
            key=lambda row: int(row["count"]),
        )
        if not data:
            continue
        ax.plot(
            [np.log2(int(row["count"])) for row in data],
            [float(row["value"]) for row in data],
            color=color, linewidth=1.5, label=LABELS[variant],
        )

    # The timed parser reads 14 bytes and writes 8 bytes per value.
    for exponent, label in ((np.log2((128 * 1024) / 22), "128 KiB L1D"),
                            (np.log2((16 * 1024 * 1024) / 22), "16 MiB L2")):
        ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
        ax.text(exponent + 0.08, 0.96, label, transform=ax.get_xaxis_transform(),
                rotation=90, va="top", ha="left", fontsize=8)
    ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
    ax.set_xticks(range(8, 21, 2))
    ax.set_ylim(bottom=0)
    ax.set_xlabel("Number of values")
    ax.set_ylabel("Nanoseconds per value")
    ax.set_title("Fixed-format parsing by working-set size")
    ax.legend()
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
    speedup_plot(
        rows,
        "parse",
        ["strtod-copy", "strtod-direct", "scalar-exact", "neon-exact"],
        "Correctly rounded fixed-format parsing",
        args.output_dir / "float-io-parsing.svg",
    )
    speedup_plot(
        rows,
        "format",
        ["snprintf", "div10-fixed", "pairs-fixed"],
        "Fixed six-decimal formatting",
        args.output_dir / "float-io-formatting.svg",
    )
    size_plot(rows, args.output_dir / "float-io-size.svg")


if __name__ == "__main__":
    main()
