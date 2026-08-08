#!/usr/bin/env python3
"""Generate the figures used by the Prime Number Sieves case study."""

import csv
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


source, size_output, segment_output, stage_output = sys.argv[1:]
rows = list(csv.DictReader(open(source, newline="")))

plt.style.use("default")
plt.rcParams.update({
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
})


def power_of_two(value, _position):
    return rf"$2^{{{int(round(value))}}}$"


def add_cache_line(ax, exponent, label):
    ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
    ax.text(exponent + 0.10, ax.get_ylim()[1] * 0.96, label,
            rotation=90, va="top", ha="left", fontsize=8)


size_rows = [row for row in rows if row["kind"] == "size"]
fig, ax = plt.subplots()
for algorithm, label, color in (
        ("full_bytes", "all integers, bytes", "#8b0000"),
        ("segmented_carry", "odd, segmented", "#00008b")):
    selected = sorted((row for row in size_rows
                       if row["algorithm"] == algorithm),
                      key=lambda row: int(row["n"]))
    exponents = [int(row["n"]).bit_length() - 1 for row in selected]
    throughput = [int(row["n"]) / float(row["milliseconds"]) / 1000
                  for row in selected]
    ax.plot(exponents, throughput, color=color, label=label)
add_cache_line(ax, 17, "full marker: 128 KiB L1D")
add_cache_line(ax, 24, "full marker: 16 MiB L2")
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(16, 28, 2))
ax.set_xlabel("Upper bound $n$")
ax.set_ylabel("Throughput (million integers/s)")
ax.set_title("Prime-counting throughput")
ax.legend()
fig.tight_layout()
fig.savefig(size_output, format="svg")
plt.close(fig)

segment_rows = sorted((row for row in rows if row["kind"] == "segment"),
                      key=lambda row: int(row["segment"]))
exponents = [int(row["segment"]).bit_length() - 1 for row in segment_rows]
throughput = [int(row["n"]) / float(row["milliseconds"]) / 1000
              for row in segment_rows]
fig, ax = plt.subplots()
ax.plot(exponents, throughput, color="#00008b")
add_cache_line(ax, 17, "128 KiB L1D")
add_cache_line(ax, 24, "16 MiB L2")
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(10, 25, 2))
ax.set_xlabel("Odd candidates per segment (bytes)")
ax.set_ylabel("Throughput (million integers/s)")
ax.set_title(r"Selecting the segment size ($n=2^{27}$)")
fig.tight_layout()
fig.savefig(segment_output, format="svg")
plt.close(fig)

stage_algorithms = ["full_bytes", "odd_bytes", "segmented_divide",
                    "segmented_carry"]
stage_labels = ["all bytes", "odd bytes", "segmented", "carried offsets"]
stage_rows = {row["algorithm"]: row for row in size_rows
              if int(row["n"]) == 1 << 27}
baseline = float(stage_rows["full_bytes"]["milliseconds"])
speedups = [baseline / float(stage_rows[name]["milliseconds"])
            for name in stage_algorithms]
colors = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]

fig, ax = plt.subplots()
bars = ax.bar(range(len(stage_algorithms)), speedups, color=colors)
for bar, value in zip(bars, speedups):
    ax.text(bar.get_x() + bar.get_width() / 2, value + 0.12,
            f"{value:.2f}x", ha="center", va="bottom",
            fontsize=9, fontweight="bold")
ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
ax.set_xticks(range(len(stage_labels)), stage_labels)
ax.set_ylabel("Speedup over the full byte sieve")
ax.set_title(r"Optimization stages ($n=2^{27}$)")
ax.set_ylim(0, max(speedups) * 1.15)
fig.tight_layout()
fig.savefig(stage_output, format="svg")
plt.close(fig)
