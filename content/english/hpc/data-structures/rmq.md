---
title: Range Minimum Query
weight: 6
---

The *range minimum query* problem asks us to preprocess an array $a$ so that we can quickly find

$$
\min(a_l,a_{l+1},\ldots,a_{r-1})
$$

for a nonempty half-open range $[l,r)$. If the array changes, a [segment tree](../segment-trees/) is appropriate. Here the array is immutable, and we only return the minimum value.

Static RMQ presents an unusually clean space-time trade-off. A sparse-table representation answers a query with two loads but occupies 84 bytes per four-byte input element at $n=2^{20}$. A blocked representation needs only five bytes per element and is slower. Adding prefix and suffix minima reduces its query work, but triples the raw-array storage. None of these choices dominates for every query length or query count.

## Benchmark Contract

The input is a deterministic array of uniformly generated 32-bit integers. Construction happens once, followed by pre-generated independent queries. We test range lengths of 13, 97, 1,001, and $3n/8+1$ while sweeping $n$ from $2^{12}$ to $2^{20}$. These lengths are deliberately not powers of two. In a sparse-table query, the two starting positions are separated by 5, 33, 489, and $n/8+1$ entries, respectively, instead of collapsing to the same entry.

Measurements were taken on an Apple M4 Max performance core using Apple Clang 17 with `-O3 -mcpu=native`. The machine has a 128 KiB L1 data cache, a 16 MiB performance-cluster L2, and 128-byte cache lines. Each process times every query loop five times and reports its middle sample; the published table is the component-wise median of five fresh process executions. The harness also checks every timed sparse-table query before measurement and aborts if its two table positions coincide. Allocation is included in the separately reported construction time but excluded from query timing. Every space figure is the storage owned by the queried representation, including its private copy of the input or the input values stored in its leaves or first table level. It excludes the caller's source vector used during construction. The [complete harness](/code/rmq_bench.cpp), [raw measurements](/code/rmq_m4_results.txt), and [plot script](/code/data_structures_plots.py) are provided.

## Baseline: Scan the Range

The baseline is difficult to simplify:

```c++
int rmq(int l, int r) {
    int result = a[l];
    for (int i = l + 1; i < r; i++)
        result = min(result, a[i]);
    return result;
}
```

The loop is contiguous and Apple Clang auto-vectorizes it. On the $2^{20}$-element array it takes 5.0 ns for a 13-element range, 6.4 ns for 97 elements, 53.7 ns for 1,001 elements, and 15.2 μs for 393,217 elements. A theoretically inferior scan is therefore a serious baseline for short queries.

A bottom-up segment tree avoids scanning the range and uses eight bytes per input element after power-of-two padding. It takes 11.1 ns, 18.4 ns, 28.0 ns, and 40.9 ns on the same four lengths. It loses to scanning below a few hundred elements and wins decisively on long ranges.

## Full Sparse Table

For every $k$, precompute minima of all ranges of length $2^k$:

$$
t[k][i]=\min(a_i,a_{i+1},\ldots,a_{i+2^k-1}).
$$

One level is constructed from the preceding one:

$$
t[k][i]=\min(t[k-1][i],t[k-1][i+2^{k-1}]).
$$

For a query of length $L=r-l$, let $k=\lfloor\log_2L\rfloor$. Two intervals of length $2^k$, one beginning at $l$ and one ending at $r$, cover the query:

$$
\operatorname{rmq}(l,r)=\min(t[k][l],t[k][r-2^k]).
$$

They may overlap, which is safe because minimum is *idempotent*: $\min(x,x)=x$.

```c++
int rmq(int l, int r) {
    int k = 31 - __builtin_clz(unsigned(r - l));
    const int *level = table + size_t(k) * n;
    return min(level[l], level[r - (1 << k)]);
}
```

Once the endpoints are known, the two table loads are data-independent. The non-power-of-two workload also makes their addresses distinct, although the two values for the shortest range can still reside in one cache line. At $n=2^{20}$, the measured cost stays between 0.94 and 1.24 ns for all four lengths. This is throughput, not serialized DRAM latency: the loop has many unrelated queries in flight.

The cost is size. We allocate one `n`-element row for every level, including the first row that copies the input and unused cells at the row ends to keep addressing simple. The resulting owned representation uses 84 bytes per input element and takes roughly 5–6 ms to build at $n=2^{20}$. If we only need a few queries, the scan completes before preprocessing has paid for itself.

## Sparse Table over Blocks

The full table stores an answer starting at every array position. We can instead divide the array into blocks of $B=64$ elements, calculate one minimum per block, and build the sparse table over only those minima.

A query has at most three parts:

1. a suffix of its first block;
2. zero or more complete blocks;
3. a prefix of its last block.

The complete blocks still take two sparse-table loads. We scan the two edges directly:

```c++
int rmq(int l, int r) {
    int first = l / B, last = (r - 1) / B;
    if (first == last)
        return scan(l, r);

    int result = min(scan(l, (first + 1) * B),
                     scan(last * B, r));
    if (first + 1 < last)
        result = min(result, macro.rmq(first + 1, last));
    return result;
}
```

The owned representation shrinks to five bytes per element at $n=2^{20}$—four bytes for its private input copy plus about one byte for block metadata—and builds in about 2.1 ms. The regression is query time: edge scanning costs 17.3 ns for a 97-element range and 13.7–17.5 ns for the two longer ranges. It is still far faster than a long direct scan, but about 13–16 times slower than the full sparse table on these cross-block queries.

This is a useful negative result. Making the block metadata compact does not guarantee a win when the original query required only two table loads.

## Prefix and Suffix Minima

We can remove the edge scans by storing, for every position, its minimum to the beginning and end of its block:

$$
p_i=\min(a_{B\lfloor i/B\rfloor},\ldots,a_i),
\qquad
s_i=\min(a_i,\ldots,a_{B\lfloor i/B\rfloor+B-1}).
$$

For a query spanning multiple blocks, `suffix[l]` answers the left edge and `prefix[r-1]` answers the right edge. The macro sparse table handles the middle. A same-block query still scans directly because the prefix and suffix values extend outside the query.

```c++
int rmq(int l, int r) {
    int first = l / B, last = (r - 1) / B;
    if (first == last)
        return scan(l, r);

    int result = min(suffix[l], prefix[r - 1]);
    if (first + 1 < last)
        result = min(result, macro.rmq(first + 1, last));
    return result;
}
```

For the million-element array, this version takes 3.72 ns for length 97, 1.70 ns for length 1,001, and 1.52 ns for length 393,217. The length-13 query remains a direct scan and takes 5.81 ns. Storage rises to 13 bytes per element because the prefix and suffix arrays duplicate two integers per input value.

![RMQ query cost across range lengths](../img/rmq-length-m4.svg)

## Space, Construction, and Query Count

![Memory occupied by each RMQ representation](../img/rmq-memory-m4.svg)

The $n=2^{20}$ comparison is:

| Structure | Owned bytes/element | Build | Length 13 | Length 1,001 | Length 393,217 |
|:--|--:|--:|--:|--:|--:|
| direct scan | **4** | 0.2 ms | 5.03 ns | 53.7 ns | 15.18 μs |
| segment tree | 8 | 0.7 ms | 11.1 ns | 28.0 ns | 40.9 ns |
| sparse table | 84 | 5.2 ms | **0.94 ns** | **1.10 ns** | **1.06 ns** |
| blocked, edge scan | 5 | 2.1 ms | 7.55 ns | 17.5 ns | 13.7 ns |
| blocked, prefix/suffix | 13 | 1.2 ms | 5.81 ns | 1.70 ns | 1.52 ns |

The build measurements are small enough to be noisy and come from one construction in each benchmark case; the query medians are the primary result. More importantly, preprocessing is only rational when it is amortized. For one short query, scanning is both smaller and faster end to end. For millions of queries, the full table can justify its 84-byte owned representation. Between these extremes, the prefix/suffix block layout retains most of the sparse table's throughput with a much smaller working set.

## Limits

The experiment fixes $B=64$. Smaller blocks shrink the same-block scan but enlarge the macro table; larger blocks do the opposite. The optimum depends on the query-length distribution, not merely on cache-line size.

If we need the *position* of the minimum, every level must store indices and use one consistent tie rule, such as preferring the lowest index. Operations such as sum are not idempotent, so overlapping sparse-table intervals no longer work.

There are linear-space, constant-query-time RMQ structures based on Cartesian trees and small-block lookup tables. Their asymptotic bounds are stronger, but they introduce more indirections and tables than the structures above. They belong in the final comparison only after they have been implemented and measured; big-O notation cannot choose this trade-off for us.
