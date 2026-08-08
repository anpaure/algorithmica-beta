#!/usr/bin/env python3

import csv
import statistics
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


source, stage_source, size_output, stage_output, error_output = sys.argv[1:]
rows = list(csv.DictReader(open(source, newline="")))

plt.rcParams.update({
    "figure.figsize": (6.4, 4.8),
    "figure.facecolor": "white",
    "savefig.facecolor": "white",
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
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "lines.linewidth": 1.5,
    "lines.solid_capstyle": "round",
    "lines.dash_capstyle": "round",
    "patch.edgecolor": "white",
    "patch.force_edgecolor": True,
    "patch.linewidth": 1.0,
    "savefig.bbox": None,
    "svg.fonttype": "path",
    "text.color": "#262626",
    "xtick.color": "#262626",
    "ytick.color": "#262626",
    "xtick.labelsize": 11,
    "ytick.labelsize": 11,
    "legend.fontsize": 11,
    "legend.edgecolor": "#ccc",
    "legend.facecolor": "white",
    "legend.framealpha": 0.8,
})


def power_of_two(value, _position):
    return rf"$2^{{{int(round(value))}}}$"


size_rows = [row for row in rows if row["kind"] == "size"]
fig, ax = plt.subplots()
for algorithm, label, color in (
        ("recursive_alloc", "recursive, allocating", "#8b0000"),
        ("precomputed_roots", "iterative, root table", "#00008b")):
    selected = sorted((row for row in size_rows
                       if row["algorithm"] == algorithm),
                      key=lambda row: int(row["n"]))
    exponents = [int(row["n"]).bit_length() - 1 for row in selected]
    times = [float(row["milliseconds"]) for row in selected]
    ax.plot(exponents, times, color=color, label=label)
ax.set_yscale("log")
for exponent, label in ((13, "input: 128 KiB L1D"),
                        (20, "input: 16 MiB L2")):
    ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
    ax.text(exponent + 0.10, ax.get_ylim()[1] / 1.3, label,
            rotation=90, va="top", ha="left", fontsize=8)
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(8, 21, 2))
ax.set_xlabel("Complex samples")
ax.set_ylabel("Forward-transform time (milliseconds)")
ax.set_title("Radix-2 complex FFT")
ax.legend()
fig.tight_layout()
fig.savefig(size_output, format="svg")
plt.close(fig)

stage_algorithms = ["recursive_alloc", "iterative_recurrence",
                    "precomputed_roots", "planned"]
stage_labels = ["recursive", "iterative", "root table", "+ reversal table"]
audit_rows = list(csv.DictReader(open(stage_source, newline="")))
stage_times = {
    name: statistics.median(float(row[name]) for row in audit_rows)
    for name in stage_algorithms
}
baseline = stage_times["recursive_alloc"]
speedups = [baseline / stage_times[name] for name in stage_algorithms]
run_speedups = {
    name: [float(row["recursive_alloc"]) / float(row[name])
           for row in audit_rows]
    for name in stage_algorithms
}
lower_errors = [value - min(run_speedups[name])
                for name, value in zip(stage_algorithms, speedups)]
upper_errors = [max(run_speedups[name]) - value
                for name, value in zip(stage_algorithms, speedups)]
colors = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]

fig, ax = plt.subplots()
bars = ax.bar(range(len(stage_algorithms)), speedups, color=colors,
              edgecolor="white", linewidth=1.0,
              yerr=[lower_errors, upper_errors], capsize=3,
              error_kw={"color": "#262626", "linewidth": 0.8})
for bar, value in zip(bars, speedups):
    ax.text(bar.get_x() + bar.get_width() / 2, value + 0.12,
            f"{value:.2f}x", ha="center", va="bottom",
            fontsize=9, fontweight="bold")
ax.axhline(1, color="#262626", linestyle="--", linewidth=0.8)
ax.set_xticks(range(len(stage_labels)), stage_labels)
ax.set_ylabel("Speedup over recursive FFT")
ax.set_title(r"Optimization stages ($n=2^{18}$; five-process median)")
ax.set_ylim(0, max(speedups) * 1.15)
fig.tight_layout()
fig.savefig(stage_output, format="svg")
plt.close(fig)

error_rows = [row for row in rows if row["kind"] == "error"]
fig, ax = plt.subplots()
for algorithm, label, color in (
        ("iterative_recurrence", "recurrent twiddles", "#8b0000"),
        ("planned", "direct root table", "#00008b")):
    selected = sorted((row for row in error_rows
                       if row["algorithm"] == algorithm),
                      key=lambda row: int(row["n"]))
    exponents = [int(row["n"]).bit_length() - 1 for row in selected]
    errors = [float(row["error"]) for row in selected]
    ax.plot(exponents, errors, color=color, label=label)
ax.set_yscale("log")
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(8, 21, 2))
ax.set_xlabel("Complex samples")
ax.set_ylabel("Maximum forward/inverse error")
ax.set_title("Round-trip numerical error")
ax.legend()
fig.tight_layout()
fig.savefig(error_output, format="svg")
plt.close(fig)
