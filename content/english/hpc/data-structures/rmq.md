---
title: Range Minimum Query
weight: 6
---

The *range minimum query* problem asks us to preprocess an array $a$ so that we can quickly find

$$
\min(a_l,a_{l+1},\ldots,a_{r-1})
$$

for any non-empty half-open range $[l,r)$.

If the array changes, we can use a [segment tree](../segment-trees/) and answer both updates and queries in $O(\log n)$. Here we will consider the static problem. Giving up updates lets us make each query both simpler and faster.

## Sparse Tables

For every $k$, precompute the minimum of all ranges of length $2^k$:

$$
t[k][i] = \min(a_i,a_{i+1},\ldots,a_{i+2^k-1}).
$$

Each level is built from the previous one:

$$
t[k][i] = \min(t[k-1][i],\;t[k-1][i+2^{k-1}]).
$$

Now take a query of length $L=r-l$ and let $k=\lfloor\log_2 L\rfloor$. The two intervals of length $2^k$ starting at $l$ and ending at $r$ cover the whole query, so

$$
\operatorname{rmq}(l,r)=\min(t[k][l],\;t[k][r-2^k]).
$$

The intervals normally overlap. This would be wrong for sums, but it is harmless for minima because

$$
\min(x,x)=x.
$$

This property is called *idempotence*. The same trick works for maximum, gcd, bitwise AND, and any other associative idempotent operation.

## Implementation

We store the table level by level so that construction reads and writes contiguous memory:

```c++
const int N = 1 << 20;
const int K = 21;

int n, a[N];
int t[K][N];

void build() {
    for (int i = 0; i < n; i++)
        t[0][i] = a[i];

    for (int k = 1; (1 << k) <= n; k++) {
        int half = 1 << (k - 1);
        int length = 2 * half;

        for (int i = 0; i + length <= n; i++)
            t[k][i] = min(t[k - 1][i], t[k - 1][i + half]);
    }
}

int rmq(int l, int r) {
    // The range is assumed to be non-empty.
    int k = 31 - __builtin_clz(r - l);
    return min(t[k][l], t[k][r - (1 << k)]);
}
```

`__builtin_clz` is compiled to a leading-zero-count instruction on modern targets, so the query consists of a few integer instructions and two independent table loads. The processor can issue both loads at the same time; if they miss cache, memory latency is almost the entire cost of the query.

The preprocessing takes $O(n\log n)$ time and the table occupies $O(n\log n)$ memory. Unlike most data structures in this chapter, the running time is already difficult to improve. The problem is its size.

## Blocking

For $n=10^8$, a full sparse table of 32-bit integers takes several gigabytes. We can spend a little computation to avoid most of it.

Split the array into blocks of $B$ elements, calculate one minimum for each block, and build a sparse table only over these $n/B$ values. A query is divided into three parts:

1. a suffix of the first block, which we scan directly;
2. some number of complete blocks, answered by the sparse table;
3. a prefix of the last block, also scanned directly.

The query performs at most $2B-2$ scalar comparisons in addition to the two table loads, while the auxiliary table becomes roughly $B$ times smaller. For a fixed $B$, this is still $O(1)$ time.

At first this sounds strictly worse: we execute more instructions to answer the same query. In practice, the smaller table is much more likely to stay in cache, and the edge scans are sequential and easy to [vectorize](/hpc/simd/auto-vectorization/). A block size around one or two cache lines is a reasonable starting point, but the right value depends on the array size and the distribution of query lengths.

Short queries deserve a special case. If the whole range fits in one or two blocks, scanning it may be cheaper than touching a distant sparse-table level at all.

## Linear-Space RMQ

It is possible to achieve $O(n)$ preprocessing, $O(n)$ space, and $O(1)$ queries at the same time.

The key reduction uses a *Cartesian tree*: its root is the position of the array minimum, and its two subtrees are built recursively from the parts to the left and right. The minimum on $[l,r)$ is the lowest common ancestor of positions $l$ and $r-1$ in this tree.

After an Euler tour, lowest-common-ancestor queries become RMQ on a special depth array in which neighboring values differ by exactly one. Small blocks of such an array have only a small number of possible shapes, so we can precompute all answers inside a block and use another structure for ranges spanning several blocks. This is the idea behind the Farach-Colton–Bender and Fischer–Heun structures.

Their asymptotic bounds are optimal, but they require more tables and indirections than the blocked sparse table. Whether they are faster is therefore a question for a benchmark, not for the big-O notation.

If we need the *position* of the minimum, we store indices instead of values and compare `a[i]`. Equal elements require one consistent rule — for example, always prefer the smaller index — at every level of the structure. Otherwise two overlapping blocks may agree on the value but disagree on which occurrence should be returned.
