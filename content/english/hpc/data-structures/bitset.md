---
title: Bitmaps
weight: 6
---

Suppose we need to store a subset of the integers $0,1,\ldots,N-1$. A hash table stores the elements that are present; a *bitmap* stores one bit for every possible element:

$$
b_i=[i\in S].
$$

This representation uses $N$ bits regardless of the size of $S$, so it is only practical when the universe is not too large. In exchange, it has no hashes, pointers, or metadata, and every set operation processes 64 elements with one instruction.

## Packing the Bits

Bit $i$ is stored in word $\lfloor i/64\rfloor$ at offset $i\bmod64$:

```c++
const int N = 1e7;
unsigned long long bits[(N + 63) / 64];

bool get(int i) {
    return bits[i >> 6] >> (i & 63) & 1;
}

void set(int i) {
    bits[i >> 6] |= 1ull << (i & 63);
}

void clear(int i) {
    bits[i >> 6] &= ~(1ull << (i & 63));
}
```

We assume that $0\le i<N$. The `ull` suffix is important: without it, the left operand of the shift would normally be a 32-bit integer, and `1 << 40` would not construct the fortieth bit of a 64-bit word.

If $N$ is not divisible by 64, the last physical word contains a few bits outside the logical universe. They are initially zero and should stay zero; after complementing a bitmap, for example, we need to mask them out again. Otherwise counting or iterating over set bits may return nonexistent elements.

## Set Operations

Intersection, union, difference, and symmetric difference are just Boolean operations on the underlying words:

```c++
const int W = (N + 63) / 64;

for (int i = 0; i < W; i++)
    result[i] = a[i] & b[i];       // intersection

for (int i = 0; i < W; i++)
    result[i] = a[i] | b[i];       // union

for (int i = 0; i < W; i++)
    result[i] = a[i] & ~b[i];      // difference
```

These loops are easy to [auto-vectorize](/hpc/simd/auto-vectorization/). For large bitmaps, however, they quickly become [memory-bandwidth bound](/hpc/cpu-cache/bandwidth/): there is only one cheap instruction for every two loaded words and one stored word.

This is why a compound expression such as

$$
R=(A\cap B)\cup(C\setminus D)
$$

should be evaluated in one pass:

```c++
for (int i = 0; i < W; i++)
    r[i] = (a[i] & b[i]) | (c[i] & ~d[i]);
```

Materializing the two intermediate sets would read and write the same amount of memory several times. For bitmaps, algebraic simplification is often a memory optimization.

## Counting and Iteration

The cardinality of a bitmap is the sum of the population counts of its words:

```c++
int count() {
    int s = 0;
    for (int i = 0; i < W; i++)
        s += __builtin_popcountll(bits[i]);
    return s;
}
```

On processors with `popcnt`, this compiles to one specialized instruction per word. If we only need the final count of an intersection, we should again avoid the temporary bitmap and accumulate `popcount(a[i] & b[i])` directly.

To enumerate a sparse bitmap, testing all $N$ positions would be wasteful. We can skip zero words and repeatedly remove the lowest set bit:

```c++
for (int i = 0; i < W; i++) {
    unsigned long long x = bits[i];

    while (x) {
        int k = __builtin_ctzll(x);
        visit(64 * i + k);
        x &= x - 1;
    }
}
```

The expression `x & (x - 1)` clears the lowest set bit, so the inner loop runs once per element rather than once per possible position. We must not call `ctz` on zero, which is why the loop condition is part of the algorithm.

## Rank and Select

Two common queries on static bitmaps are:

- $\operatorname{rank}(i)$: the number of set bits in $[0,i)$;
- $\operatorname{select}(k)$: the position of the zero-indexed $k$-th set bit.

A simple rank index stores the cumulative population count after every few words. To answer a query, we load the preceding counter and use `popcnt` on the remaining words and on a masked prefix of the final word. Select performs the reverse operation: find the counter interval containing $k$, then locate the required bit inside that block.

Storing one counter per word almost doubles the size of the bitmap. Succinct data structures use a hierarchy: a wide counter for a large *superblock* and smaller relative counters for its sub-blocks. Rank then needs only a few loads and one population count while the index occupies much less space than the data.

## Sparse Universes

A bitmap pays for the universe rather than for the set. If we store one million 64-bit identifiers scattered across the whole $[0,2^{64})$ range, a dense bitmap is impossible and a sorted array is far more sensible.

Real data often has mixed density: most of the universe is empty, but some regions are crowded. [Roaring bitmaps](https://arxiv.org/abs/1709.07821) split a 32-bit integer into two 16-bit halves and choose a representation separately for each non-empty high half. A sparse chunk is stored as a sorted array of low halves; a dense chunk is a $2^{16}$-bit bitmap; long consecutive regions can be stored as runs.

Operations then use the representation pair they encounter. Two dense chunks are combined with word-wise Boolean operations, two sparse chunks are merged as sorted arrays, and an array can probe a bitmap directly. The important idea is not the particular thresholds but the decision to measure density locally.

Bitmaps are fast when they turn a problem into long streams of predictable word operations. Compression is worthwhile when it avoids enough empty words without destroying that regularity.
