#!/usr/bin/env python3

import csv
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


source, size_output, reduction_output, stages_output = sys.argv[1:]
with open(source, newline="", encoding="utf-8") as source_file:
    rows = list(csv.DictReader(source_file))

plt.style.use("default")
plt.rcParams.update({
    "figure.figsize": (6.4, 4.8),
    "figure.facecolor": "white",
    "axes.facecolor": "white",
    "axes.edgecolor": "#ccc",
    "axes.labelcolor": "#262626",
    "axes.linewidth": 1.25,
    "axes.grid": True,
    "axes.axisbelow": True,
    "grid.color": "#ccc",
    "grid.linewidth": 0.8,
    "font.family": "DejaVu Sans",
    "lines.solid_capstyle": "round",
    "patch.edgecolor": "white",
    "patch.linewidth": 1.0,
    "svg.fonttype": "path",
    "text.color": "#262626",
    "xtick.color": "#262626",
    "ytick.color": "#262626",
})


def power_of_two(value, _position):
    return rf"$2^{{{int(round(value))}}}$"


def selected(algorithm):
    return sorted((row for row in rows if row["algorithm"] == algorithm),
                  key=lambda row: int(row["n"]))


def add_cache_line(ax, exponent, label):
    ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
    ax.text(exponent + 0.10, 0.96, label,
            transform=ax.get_xaxis_transform(), rotation=90,
            va="top", ha="left", fontsize=8)


fig, ax = plt.subplots()
for algorithm, label, color in (
        ("baseline", "textbook", "#8b0000"),
        ("permutation_free", "planned, no permutation", "#00008b")):
    data = selected(algorithm)
    ax.plot([int(row["log_n"]) for row in data],
            [float(row["ns_per_butterfly"]) for row in data],
            color=color, label=label)
ax.set_ylim(bottom=0)
add_cache_line(ax, 13.42, "data + roots: 128 KiB L1D")
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(8, 21, 2))
ax.set_xlabel("Transform length")
ax.set_ylabel("Nanoseconds per butterfly")
ax.set_title("Forward number-theoretic transform")
ax.legend()
fig.tight_layout()
fig.savefig(size_output, format="svg")
plt.close(fig)


fig, ax = plt.subplots()
for algorithm, label, color in (
        ("planned_remainder", "constant C++ remainder", "#8b0000"),
        ("planned_shoup", "Shoup reduction", "#00008b")):
    data = selected(algorithm)
    ax.plot([int(row["log_n"]) for row in data],
            [float(row["ns_per_butterfly"]) for row in data],
            color=color, label=label)
ax.set_ylim(bottom=0)
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(8, 21, 2))
ax.set_xlabel("Transform length")
ax.set_ylabel("Nanoseconds per butterfly")
ax.set_title("The hand-written reduction loses")
ax.legend()
fig.tight_layout()
fig.savefig(reduction_output, format="svg")
plt.close(fig)


stage_algorithms = ["baseline", "planned_remainder", "planned_shoup",
                    "permutation_free"]
stage_labels = ["textbook", "planned", "+ Shoup", "DIF/DIT + %"]
stage_rows = {row["algorithm"]: row for row in rows
              if int(row["n"]) == 1 << 18}
baseline = float(stage_rows["baseline"]["median_ns"])
speedups = [baseline / float(stage_rows[name]["median_ns"])
            for name in stage_algorithms]

fig, ax = plt.subplots()
bars = ax.bar(range(len(stage_algorithms)), speedups,
              color=["#8b0000", "#00008b", "#006400", "#b8860b"],
              edgecolor="white", linewidth=1.0)
for bar, value in zip(bars, speedups):
    ax.text(bar.get_x() + bar.get_width() / 2, value + 0.10,
            f"{value:.2f}x", ha="center", va="bottom", fontsize=9)
ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
ax.set_xticks(range(len(stage_labels)), stage_labels)
ax.set_ylabel("Speedup over the textbook transform")
ax.set_title(r"Optimization stages ($n=2^{18}$)")
ax.set_ylim(0, max(speedups) * 1.15)
fig.tight_layout()
fig.savefig(stages_output, format="svg")
plt.close(fig)
