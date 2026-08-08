#!/usr/bin/env python3

import csv
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


source, size_output, distribution_output, width_output = sys.argv[1:]
rows = list(csv.DictReader(open(source, newline="")))

labels = {
    "std_sort": "std::sort",
    "radix8_reuse": "radix-8",
    "radix11": "radix-11",
    "radix8_precount": "radix-8, all histograms",
}

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

colors = {
    "std_sort": "#8b0000",
    "radix8_reuse": "#c47f00",
    "radix11": "#00008b",
    "radix8_precount": "#4b7f52",
}


def power_of_two(value, _position):
    exponent = int(round(value))
    return rf"$2^{{{exponent}}}$"

size_rows = [row for row in rows if row["kind"] == "size"]
fig, ax = plt.subplots()
for algorithm in ("std_sort", "radix11"):
    selected = sorted((row for row in size_rows
                       if row["algorithm"] == algorithm),
                      key=lambda row: int(row["n"]))
    count = [int(row["n"]) for row in selected]
    exponents = [int(value).bit_length() - 1 for value in count]
    throughput = [int(row["n"]) / float(row["milliseconds"]) / 1000
                  for row in selected]
    ax.plot(exponents, throughput, label=labels[algorithm],
            color=colors[algorithm])
for exponent, name in ((14, "128 KiB L1D"), (21, "16 MiB L2")):
    ax.axvline(exponent, color="#262626", linestyle="--", linewidth=0.8)
    ax.text(exponent + 0.12, ax.get_ylim()[1] * 0.96, name,
            rotation=90, va="top", ha="left", fontsize=8)
ax.xaxis.set_major_formatter(FuncFormatter(power_of_two))
ax.set_xticks(range(10, 24, 2))
ax.set_xlabel("Number of random 32-bit keys")
ax.set_ylabel("Throughput (million keys/s)")
ax.set_title("32-bit sorting throughput")
ax.legend(ncol=2)
fig.tight_layout()
fig.savefig(size_output, format="svg")
plt.close(fig)

distribution_rows = [row for row in rows if row["kind"] == "distribution"]
times = {(row["distribution"], row["algorithm"]):
         float(row["milliseconds"]) for row in distribution_rows}
distributions = ["random", "sorted", "reverse", "low8", "nearly_sorted"]
display_names = ["Random", "Sorted", "Reverse", "8-bit range", "1% swaps"]
algorithms = ["radix8_reuse", "radix11", "radix8_precount"]
bar_colors = ["#4c72b0", "#dd8452", "#55a868",
              "#c44e52", "#8172b3", "#937860"]

fig, ax = plt.subplots()
width = 0.24
for index, algorithm in enumerate(algorithms):
    speedup = [times[(distribution, "std_sort")] /
               times[(distribution, algorithm)]
               for distribution in distributions]
    position = [i + (index - 1) * width
                for i in range(len(distributions))]
    bars = ax.bar(position, speedup, width=width, label=labels[algorithm],
                  color=bar_colors[index], edgecolor="white", linewidth=1.0)
    for bar, value in zip(bars, speedup):
        ax.text(bar.get_x() + bar.get_width() / 2, value + 0.25,
                f"{value:.2f}x", ha="center", va="bottom",
                fontsize=7, fontweight="bold", rotation=90)
ax.axhline(1, color="#262626", linewidth=1, linestyle="--")
ax.set_xticks(range(len(distributions)), display_names)
ax.set_ylabel("Speedup over std::sort")
ax.set_title("Effect of the input distribution ($n=2^{20}$)")
ax.set_ylim(0, 20.5)
ax.legend(ncol=2)
fig.tight_layout()
fig.savefig(distribution_output, format="svg")
plt.close(fig)

width_rows = sorted((row for row in rows if row["kind"] == "width"),
                    key=lambda row: int(row["distribution"]))
widths = [int(row["distribution"]) for row in width_rows]
width_throughput = [int(row["n"]) / float(row["milliseconds"]) / 1000
                    for row in width_rows]
passes = [(31 + width) // width for width in widths]
histogram_kib = [(1 << width) * 8 / 1024 for width in widths]

fig, ax = plt.subplots()
bars = ax.bar(range(len(widths)), width_throughput,
              color=bar_colors[:len(widths)], edgecolor="white", linewidth=1.0)
for bar, value in zip(bars, width_throughput):
    ax.text(bar.get_x() + bar.get_width() / 2, value + 5,
            f"{value:.0f}", ha="center", va="bottom",
            fontsize=8, fontweight="bold")
tick_labels = [f"{width} bits\n{count} passes\n{size:g} KiB"
               for width, count, size in zip(widths, passes, histogram_kib)]
ax.set_xticks(range(len(widths)), tick_labels)
ax.set_xlabel("Digit width, pass count, and largest histogram")
ax.set_ylabel("Throughput (million keys/s)")
ax.set_title(r"Choosing the radix width ($n=2^{20}$)")
ax.set_ylim(0, max(width_throughput) * 1.16)
fig.tight_layout()
fig.savefig(width_output, format="svg")
plt.close(fig)
