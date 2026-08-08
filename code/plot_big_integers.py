#!/usr/bin/env python3
"""Generate the figures used by the Big Integers case study."""

import csv
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


source, size_output, cutoff_output = sys.argv[1:]
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


size_rows = [row for row in rows if row["kind"] == "size"]
fig, ax = plt.subplots()
for algorithm, label, color in (
        ("schoolbook", "schoolbook", "#8b0000"),
        ("karatsuba_workspace", "Karatsuba, workspace", "#00008b")):
    selected = sorted((row for row in size_rows
                       if row["algorithm"] == algorithm),
                      key=lambda row: int(row["n"]))
    exponents = [int(row["n"]).bit_length() - 1 for row in selected]
    microseconds = [float(row["microseconds"]) for row in selected]
    ax.plot(exponents, microseconds, color=color, label=label)
ax.set_yscale("log")
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(4, 14))
ax.set_xlabel("Base-$10^4$ digits in each operand")
ax.set_ylabel("Multiplication time (microseconds)")
ax.set_title("Schoolbook and Karatsuba multiplication")
ax.legend()
fig.tight_layout()
fig.savefig(size_output, format="svg")
plt.close(fig)

cutoff_rows = sorted((row for row in rows if row["kind"] == "cutoff"),
                     key=lambda row: int(row["cutoff"]))
cutoffs = [int(row["cutoff"]) for row in cutoff_rows]
times = [float(row["microseconds"]) for row in cutoff_rows]
best = min(times)
slowdown = [value / best for value in times]
colors = ["#4c72b0", "#dd8452", "#55a868", "#c44e52",
          "#8172b3", "#937860", "#da8bc3", "#8c8c8c", "#ccb974"]

fig, ax = plt.subplots()
bars = ax.bar(range(len(cutoffs)), slowdown, color=colors)
for bar, value in zip(bars, slowdown):
    ax.text(bar.get_x() + bar.get_width() / 2, value + 0.025,
            f"{value:.2f}x", ha="center", va="bottom",
            fontsize=8, fontweight="bold")
ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
ax.set_xticks(range(len(cutoffs)), [str(value) for value in cutoffs])
ax.set_xlabel("Schoolbook cutoff (digits)")
ax.set_ylabel("Slowdown from the best result")
ax.set_title(r"Selecting the base case ($n=2^{12}$)")
ax.set_ylim(0, max(slowdown) * 1.15)
fig.tight_layout()
fig.savefig(cutoff_output, format="svg")
plt.close(fig)
