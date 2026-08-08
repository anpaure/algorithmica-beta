#!/usr/bin/env python3
"""Regenerate the Algorithmica data-structure case-study figures.

Run the five C++ harnesses with --bench first and save their CSV output beside
this script using the checked-in *_m4_results.txt names. Then run this file with
Matplotlib, pandas, and Seaborn installed.
"""

from pathlib import Path
import math

import matplotlib as mpl
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns


HERE = Path(__file__).resolve().parent
OUT = HERE.parents[1] / "content/english/hpc/data-structures/img"

sns.set_theme(style="whitegrid", palette="deep")
mpl.rcParams.update({
    "figure.figsize": (6.4, 4.8),
    "figure.facecolor": "white",
    "savefig.facecolor": "white",
    "savefig.bbox": None,
    "svg.fonttype": "path",
    "axes.facecolor": "white",
    "axes.edgecolor": "#ccc",
    "axes.labelcolor": "#262626",
    "axes.titlecolor": "#262626",
    "axes.linewidth": 1.25,
    "axes.titlesize": 12,
    "axes.labelsize": 12,
    "axes.grid": True,
    "axes.axisbelow": True,
    "font.family": "DejaVu Sans",
    "font.size": 10,
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
})


def save(fig: plt.Figure, name: str) -> None:
    fig.tight_layout()
    fig.savefig(OUT / name, format="svg")
    plt.close(fig)


def power_ticks(ax: plt.Axes, values) -> None:
    ticks = sorted(set(float(x) for x in values))
    ax.set_xscale("log", base=2)
    ax.set_xticks(ticks)
    ax.set_xticklabels([rf"$2^{{{round(math.log2(x))}}}$" for x in ticks])


def label_bars(ax: plt.Axes, fmt: str = "{:.1f}") -> None:
    for container in ax.containers:
        ax.bar_label(container, fmt=fmt, padding=3, fontsize=8, fontweight="bold")


def hash_plots() -> None:
    df = pd.read_csv(HERE / "hash_tables_m4_results.txt")
    names = {
        "unordered": "std::unordered_map",
        "flat_aos": "Flat AoS",
        "flat_soa": "Flat SoA",
        "fingerprint_groups": "Fingerprint groups",
    }
    df["Implementation"] = df.implementation.map(names)
    size = df[df.load.between(0.749, 0.751)]
    fig, ax = plt.subplots()
    sns.lineplot(data=size, x="capacity", y="throughput_ns", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    power_ticks(ax, size.capacity.unique())
    ax.set(title="Mean hash-table lookup cost at 75% load",
           xlabel="Table cells", ylabel="Nanoseconds per lookup (50% misses)")
    save(fig, "hash-tables-size-m4.svg")

    load = df[df.capacity == 1048576]
    fig, ax = plt.subplots()
    sns.lineplot(data=load, x="load", y="throughput_ns", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    ax.set(title="Open addressing becomes expensive before it becomes full",
           xlabel="Load factor", ylabel="Nanoseconds per lookup (50% misses)")
    save(fig, "hash-tables-load-m4.svg")


def rmq_plots() -> None:
    df = pd.read_csv(HERE / "rmq_m4_results.txt")
    names = {
        "scan": "Direct scan", "segment": "Segment tree",
        "sparse": "Sparse table", "blocked_scan": "Blocked + edge scan",
        "blocked_prefix": "Blocked + prefix/suffix",
    }
    df["Implementation"] = df.implementation.map(names)
    length = df[df.n == 1048576].drop_duplicates(["implementation", "length"])
    fig, ax = plt.subplots()
    sns.lineplot(data=length, x="length", y="query_ns", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    ticks = sorted(length.length.unique())
    ax.set_xscale("log", base=2)
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"{value:,}" for value in ticks])
    ax.set_yscale("log", base=2)
    ax.set(title="RMQ cost depends on query length",
           xlabel="Range length", ylabel="Nanoseconds per query")
    save(fig, "rmq-length-m4.svg")

    memory = df[(df.n == 1048576) & (df.length == 1001)].copy()
    fig, ax = plt.subplots()
    sns.barplot(data=memory, x="Implementation", y="bytes_per_element",
                color="#4c72b0", saturation=1, edgecolor="white",
                linewidth=1.0, ax=ax)
    ax.tick_params(axis="x", rotation=18)
    ax.set(title="Owned RMQ representation size for $2^{20}$ integers",
           xlabel="", ylabel="Owned bytes per input element")
    label_bars(ax)
    save(fig, "rmq-memory-m4.svg")


def filter_plots() -> None:
    df = pd.read_csv(HERE / "filters_m4_results.txt")
    names = {"independent": "Independent hashes", "double": "Double hashing",
             "blocked": "128-byte blocked"}
    df["Implementation"] = df.implementation.map(names)
    density = df[df.phase == "density"]
    fig, ax = plt.subplots()
    sns.lineplot(data=density, x="bits_per_key", y="false_positive_rate",
                 hue="Implementation", linewidth=1.5, markers=False, ax=ax)
    theory_x = [6 + i / 20 for i in range(201)]
    theory_y = [(1 - math.exp(-7 / b)) ** 7 for b in theory_x]
    ax.plot(theory_x, theory_y, color="#262626", linestyle="--", linewidth=0.8,
            label=r"Independent model $(1-e^{-7/b})^7$")
    ax.legend()
    ax.set_yscale("log")
    ax.set(title="Locality has a false-positive cost",
           xlabel="Bits per inserted key", ylabel="Measured false-positive rate")
    save(fig, "filters-fpr-m4.svg")

    size = df[df.phase == "size"].copy()
    size["bytes"] = size.bits / 8
    fig, ax = plt.subplots()
    sns.lineplot(data=size, x="bytes", y="throughput_ns", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    power_ticks(ax, size.bytes.unique())
    for x, label in [(128 * 1024, "L1D"), (16 * 1024 * 1024, "L2")]:
        ax.axvline(x, color="#262626", linestyle="--", linewidth=0.8)
        ax.text(x, ax.get_ylim()[1], label, ha="right", va="top", fontsize=8)
    ax.set(title="Bloom-filter lookup cost on Apple M4 Max",
           xlabel="Filter size (bytes)", ylabel="Nanoseconds per query (90% misses)")
    save(fig, "filters-size-m4.svg")


def trie_plots() -> None:
    df = pd.read_csv(HERE / "tries_m4_results.txt")
    names = {"unordered": "std::unordered_set", "dense": "Dense trie",
             "packed": "Bitmap-packed trie", "compressed": "Compressed trie"}
    df["Implementation"] = df.implementation.map(names)
    for dataset, title, filename in [
        ("random", "Deterministic random 16-byte keys", "tries-random-m4.svg"),
        ("paths", "Deterministic path-like keys", "tries-paths-m4.svg"),
    ]:
        part = df[df.dataset == dataset]
        fig, ax = plt.subplots()
        sns.lineplot(data=part, x="keys", y="lookup_ns", hue="Implementation",
                     linewidth=1.5, markers=False, ax=ax)
        power_ticks(ax, part["keys"].unique())
        ax.set(title=title, xlabel="Stored strings", ylabel="Nanoseconds per lookup (50% misses)")
        save(fig, filename)

    memory = df[df["keys"] == 131072]
    fig, ax = plt.subplots()
    sns.barplot(data=memory, x="Implementation", y="bytes_per_key", hue="dataset",
                palette="deep", saturation=1, edgecolor="white",
                linewidth=1.0, ax=ax)
    ax.set_yscale("log", base=2)
    ax.tick_params(axis="x", rotation=18)
    ax.set(title="Trie memory depends on prefix sharing",
           xlabel="", ylabel="Bytes per stored key")
    save(fig, "tries-memory-m4.svg")


def bitmap_plots() -> None:
    df = pd.read_csv(HERE / "bitmaps_m4_results.txt")
    names = {"sorted": "Sorted array", "chunk_bitmap": "Bitmap chunks",
             "adaptive": "Adaptive containers"}
    df["Implementation"] = df.implementation.map(names)
    fig, ax = plt.subplots()
    sns.lineplot(data=df, x="per_container", y="contains_ns", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    power_ticks(ax, df.per_container.unique())
    ax.set_yscale("log", base=2)
    ax.set(title="Membership lookup versus local density",
           xlabel="Values per 16-bit chunk", ylabel="Nanoseconds per lookup (50% misses)")
    save(fig, "bitmaps-contains-m4.svg")

    fig, ax = plt.subplots()
    sns.lineplot(data=df, x="per_container", y="intersection_minput_keys_s",
                 hue="Implementation", linewidth=1.5, markers=False, ax=ax)
    power_ticks(ax, df.per_container.unique())
    ax.set_yscale("log", base=2)
    ax.set(title="Intersection changes algorithm at the container boundary",
           xlabel="Values per 16-bit chunk", ylabel="Million stored input keys per second")
    save(fig, "bitmaps-intersection-m4.svg")

    fig, ax = plt.subplots()
    sns.lineplot(data=df, x="per_container", y="bytes_per_key", hue="Implementation",
                 linewidth=1.5, markers=False, ax=ax)
    power_ticks(ax, df.per_container.unique())
    ax.set_yscale("log", base=2)
    ax.set(title="The cost of paying for empty bits",
           xlabel="Values per 16-bit chunk", ylabel="Bytes per stored value")
    save(fig, "bitmaps-memory-m4.svg")


if __name__ == "__main__":
    hash_plots()
    rmq_plots()
    filter_plots()
    trie_plots()
    bitmap_plots()
