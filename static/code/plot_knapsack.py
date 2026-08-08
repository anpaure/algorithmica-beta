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
    "words-sorted": "sort ascending",
    "words-paired": "pair two shifts",
    "words-adaptive": "skip full words",
    "adaptive-sorted": "sort + skip",
    "direct-stop": "stop at an exact fill",
    "scaled-direct": "+ divide by the gcd",
    "symmetric": "+ solve the complement",
    "bundled": "+ bundle duplicates",
    "probe-bundled": "+ probe original order",
    "final-adaptive": "+ skip full words",
}
DEEP = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]
DEEP7 = DEEP + ["#8172b3", "#937860", "#da8bc3"]


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


def normalize_svg(path: Path):
    """Keep generated SVGs reproducible and clean under git diff --check."""
    lines = path.read_text().splitlines()
    path.write_text("\n".join(line.rstrip() for line in lines) + "\n")


def ratio(row):
    """Read the frontier ratio from both the old and expanded CSV schemas."""
    return float(row.get("total_ratio") or row["frontier_ratio"])


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
    fig.supylabel("Speedup over full byte DP", fontsize=12)
    fig.suptitle("Packing and frontier bounds solve different bottlenecks", fontsize=12)
    fig.tight_layout(rect=(0.025, 0, 1, 0.96))
    fig.savefig(output, format="svg")
    plt.close(fig)


def frontier(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "frontier"]
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    for variant, color in [("words-full", "#8b0000"),
                           ("words-bounded", "#00008b")]:
        current = sorted(
            (row for row in selected if row["variant"] == variant),
            key=ratio,
        )
        ax.plot(
            [ratio(row) for row in current],
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


def kernel(rows, output: Path):
    suites = [
        "dense-1m",
        "wide-weights-1m",
        "unique-desc-1m",
        "residue-adversary",
    ]
    suite_labels = [
        "Small costs",
        "Wide costs",
        "Descending\n1..2000",
        "Residue\nadversary",
    ]
    variants = [
        "words-sorted",
        "words-paired",
        "words-adaptive",
        "adaptive-sorted",
    ]
    by_key = {
        (row["suite"], row["variant"]): float(row["milliseconds"])
        for row in rows if row["kind"] == "kernel"
    }

    x = np.arange(len(suites))
    width = 0.19
    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    containers = []
    for index, variant in enumerate(variants):
        values = [
            by_key[(suite, variant)] / by_key[(suite, "words-bounded")]
            for suite in suites
        ]
        offset = (index - (len(variants) - 1) / 2) * width
        containers.append(
            ax.bar(
                x + offset,
                values,
                width,
                label=LABELS[variant],
                color=DEEP[index],
            )
        )

    # The last workload is deliberately hostile to the saturation index.  Its
    # bars would otherwise merge visually with the 1x reference on a log axis.
    for container in containers:
        value = container[-1].get_height()
        ax.annotate(
            f"{value:.2f}×",
            (container[-1].get_x() + container[-1].get_width() / 2, value),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            rotation=90,
            fontsize=7,
        )

    ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
    ax.set_yscale("log")
    ax.set_ylim(0.012, 30)
    ax.set_xticks(x, suite_labels)
    ax.set_ylabel("Time / bounded-word time (lower is better)")
    ax.set_title("Exact transformations can still lose")
    ax.legend(ncols=2, frameon=True)
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(output, format="svg")
    plt.close(fig)


def speedup_label(value):
    if value >= 1000:
        return f"{value / 1000:.1f}k×"
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def solver(rows, output: Path):
    suites = [
        "dense-1m",
        "wide-weights-1m",
        "sparse-frontier-5m",
        "unique-desc-1m",
        "gcd-64-1m",
        "residue-adversary",
    ]
    titles = [
        "Small costs",
        "Wide costs",
        "Total < W",
        "Descending 1..2000",
        "gcd = 64",
        "Residue adversary",
    ]
    variants = [
        "full-set",
        "direct-stop",
        "scaled-direct",
        "symmetric",
        "bundled",
        "probe-bundled",
        "final-adaptive",
    ]
    stage_labels = ["full", "stop", "gcd", "sym.", "bundle", "probe", "index"]
    by_key = {
        (row["suite"], row["variant"]): float(row["milliseconds"])
        for row in rows if row["kind"] == "solver"
    }

    panel_values = {}
    maximum = 1
    for suite in suites:
        baseline = by_key[(suite, "full-set")]
        speedups = [
            baseline / by_key[(suite, variant)]
            for variant in variants
        ]
        panel_values[suite] = speedups
        maximum = max(maximum, max(speedups))

    upper = 10 ** np.ceil(np.log10(maximum * 2))
    fig, axes = plt.subplots(2, 3, figsize=(6.4, 4.8), sharey=True)
    positions = np.arange(len(variants))
    for panel, (ax, suite, title) in enumerate(zip(axes.flat, suites, titles)):
        speedups = panel_values[suite]
        bars = ax.bar(positions, speedups, color=DEEP7)
        ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
        ax.set_yscale("log")
        ax.set_ylim(0.72, upper)
        ax.set_title(title, fontsize=9.5)
        ax.grid(axis="x", visible=False)
        if panel < 3:
            ax.set_xticks(positions, [])
        else:
            ax.set_xticks(
                positions,
                stage_labels,
                rotation=55,
                ha="right",
                rotation_mode="anchor",
                fontsize=6.5,
            )

        # Labels identify every near-baseline result and the final policy.
        # This keeps genuine regressions visible even beside 1000x wins.
        for index, (bar, value) in enumerate(zip(bars, speedups)):
            if suite == "residue-adversary" or value <= 1.05 \
                    or index == len(variants) - 1:
                ax.annotate(
                    speedup_label(value),
                    (bar.get_x() + bar.get_width() / 2, value),
                    xytext=(0, 2),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    rotation=90,
                    fontsize=5.5,
                )

    fig.supylabel("Speedup over full-set DP", fontsize=12)
    fig.suptitle("One bottleneck at a time", fontsize=12)
    fig.tight_layout(rect=(0.025, 0, 1, 0.95), pad=0.8)
    fig.savefig(output, format="svg")
    plt.close(fig)


def symmetry(rows, output: Path):
    selected = [row for row in rows if row["kind"] == "symmetry"]
    variants = ["scaled-direct", "symmetric", "final-adaptive"]
    colors = ["#8b0000", "#00008b", "#006400"]
    labels = {
        "scaled-direct": "direct",
        "symmetric": "smaller of subset/complement",
        "final-adaptive": "final policy",
    }
    by_variant = {
        variant: sorted(
            (row for row in selected if row["variant"] == variant),
            key=ratio,
        )
        for variant in variants
    }

    fig, axes = plt.subplots(
        2,
        1,
        figsize=(6.4, 4.8),
        sharex=True,
        gridspec_kw={"height_ratios": [1, 1]},
    )
    direct = by_variant["scaled-direct"]
    complement = by_variant["symmetric"]
    for current, label, color in [
        (direct, labels["scaled-direct"], colors[0]),
        (complement, labels["symmetric"], colors[1]),
    ]:
        axes[0].plot(
            [ratio(row) for row in current],
            [int(row["effective_capacity"]) / int(row["capacity"]) for row in current],
            color=color,
            label=label,
        )

    for variant, color in zip(variants, colors):
        current = by_variant[variant]
        axes[1].plot(
            [ratio(row) for row in current],
            [float(row["milliseconds"]) for row in current],
            color=color,
            label=labels[variant] if variant == "final-adaptive" else None,
        )

    for ax in axes:
        ax.axvline(1, color="#262626", linestyle="--", linewidth=0.8)
        ax.axvline(2, color="#262626", linestyle="--", linewidth=0.8)
    axes[0].set_ylim(-0.04, 1.08)
    axes[0].set_ylabel("DP capacity / W")
    axes[0].legend(ncols=2, frameon=True, fontsize=9)
    axes[1].set_yscale("log")
    axes[1].set_xlabel("Total usable weight / capacity")
    axes[1].set_ylabel("Milliseconds")
    axes[1].legend(loc="lower right", frameon=True, fontsize=9)
    fig.suptitle("Complementing the subset shrinks the useful DP", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
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
    kernel(rows, args.output_dir / "knapsack-kernel.svg")
    solver(rows, args.output_dir / "knapsack-solver.svg")
    symmetry(rows, args.output_dir / "knapsack-symmetry.svg")
    for name in [
        "knapsack-stages.svg",
        "knapsack-frontier.svg",
        "knapsack-size.svg",
        "knapsack-kernel.svg",
        "knapsack-solver.svg",
        "knapsack-symmetry.svg",
    ]:
        normalize_svg(args.output_dir / name)


if __name__ == "__main__":
    main()
