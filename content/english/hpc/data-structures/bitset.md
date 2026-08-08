---
title: Bitmaps
weight: 9
---

A bitmap stores one bit for every possible integer. It can test membership without hashing, combine 64 membership bits with one word operation, and enumerate set bits with `ctz`. Its weakness is equally simple: it pays for the universe rather than the set.

Real sets are often neither globally dense nor uniformly sparse. In this case study, we split 32-bit integers into 16-bit chunks and choose a representation separately for each chunk. The resulting adaptive structure is array-like in sparse regions and bitmap-like in dense ones. It is a small, directly implemented version of the central idea behind Roaring bitmaps—not a full Roaring implementation with its additional formats and tuning.

## Workload

The sets are immutable after construction and support three operations:

```c++
bool contains(uint32_t x) const;
uint64_t intersection_count(const Set &other) const;
void iterate(visitor) const;
```

Each benchmark set contains 262,144 integers. We vary *local density* by placing 32, 256, 2,048, 8,192, or 32,768 values in every occupied 16-bit chunk and changing the number of chunks to keep total cardinality fixed. Paired sets occupy the same chunks and share approximately half their values. Membership queries are pre-generated with exactly 50% hits and 50% misses. Every miss deliberately uses an absent low value in an occupied high-16-bit chunk, so all representations traverse the full directory path; these are not easy random-universe misses that stop at an empty directory entry.

Measurements use one Apple M4 Max performance core, Apple Clang 17, and `-O3 -mcpu=native`. The core has a 128 KiB L1 data cache, a shared 16 MiB L2, and 128-byte cache lines. Lookup and iteration numbers are medians of five runs; intersection is the median of seven complete intersections. Construction is outside the timed region. The [complete implementation and tests](/code/bitmaps_bench.cpp), [raw measurements](/code/bitmaps_m4_results.txt), and [plot generator](/code/data_structures_plots.py) are included.

## Sorted-Array Baseline

The baseline stores all values in a sorted `vector<uint32_t>`.

- Membership is binary search.
- Intersection merges two sorted arrays.
- Iteration is a contiguous scan.

It always uses four bytes per value. Membership takes 45–49 ns across our local-density sweep because total cardinality is fixed. Intersection processes 273–312 million stored input keys per second, and auto-vectorized iteration needs about 0.04 ns per value while the array is hot.

This is an important baseline. A sparse representation does not need metadata, bit decoding, or empty-word scans.

## Bitmap Chunks

Split a key into a high and low half:

$$
x=(h\ll16)\;|\;l.
$$

A 65,536-entry directory maps `h` to an optional 65,536-bit bitmap. Each allocated chunk occupies 8 KiB:

```c++
bool contains(uint32_t x) const {
    int offset = directory[x >> 16];
    if (offset < 0)
        return false;
    uint16_t low = uint16_t(x);
    return words[offset + (low >> 6)] >> (low & 63) & 1;
}
```

Membership now takes only one directory access and one bitmap load. It falls from roughly 49 ns for the sorted array to 3.03 ns with 32 values per chunk and close to 1 ns from 256 values per chunk onward.

The representation is disastrous when chunks are sparse. With 32 values in each allocated 8 KiB bitmap, it spends 257 bytes per key. Iteration takes 14.3 ns per returned value because it scans 2,048 empty bits for every set one. Its intersection throughput, 204 million stored input keys per second, is actually lower than merging the sorted arrays.

As density grows, the exact same loops become excellent. At 8,192 values per chunk, storage falls to two bytes per key and bitmap intersection reaches a logical rate of 56.7 billion stored input keys per second. At 32,768 values per chunk, storage is 1.25 bytes per key and the same normalized rate exceeds 240 billion input keys per second.

![Bitmap lookup latency versus local density](../img/bitmaps-contains-m4.svg)

## Adaptive Containers

The space crossover is easy to calculate. A sorted array of 16-bit lows uses $2c$ bytes for cardinality $c$; a bitmap always uses 8,192 bytes. They are equal at $c=4096$.

Our adaptive structure therefore stores each nonempty high-half chunk as either:

- a sorted array of `uint16_t` when $c\le4096$;
- a 1,024-word bitmap when $c>4096$.

The directory records a type, cardinality, and offset into one of two contiguous storage arenas:

```c++
bool contains(uint32_t x) const {
    uint16_t high = x >> 16, low = uint16_t(x);
    if (type[high] == empty)
        return false;

    size_t base = offset[high];
    if (type[high] == bitmap)
        return bitmaps[base + (low >> 6)] >> (low & 63) & 1;

    const uint16_t *first = arrays.data() + base;
    return binary_search(first, first + count[high], low);
}
```

Intersection dispatches on the representation pair:

- array–array performs a merge;
- array–bitmap probes each sparse value in the bitmap;
- bitmap–bitmap applies AND and `popcnt` word by word.

This is an algorithmic switch, not just compression. At 2,048 values per chunk, the adaptive structure behaves like sorted 16-bit arrays and intersects 271 million stored input keys per second. At 8,192, it switches to bitmaps and jumps to a normalized 56.2 billion.

![Intersection throughput across the representation switch](../img/bitmaps-intersection-m4.svg)

The adaptive structure avoids the sparse bitmap's space explosion. It uses 3.81, 3.76, and 3.75 bytes per key at 32, 256, and 2,048 values per chunk, compared with 257, 33, and 5 bytes for fixed bitmap chunks.

It does not minimize every column. Sparse adaptive membership uses binary search and slows from 6.4 ns at cardinality 32 to 18.1 ns at 2,048; a fixed bitmap remains close to 3 ns or below but spends much more memory. In dense chunks, the richer adaptive directory makes it slightly larger than the bitmap-only structure.

![Memory per value for fixed and adaptive chunk layouts](../img/bitmaps-memory-m4.svg)

## Counting and Iteration

Within a bitmap container, cardinality is the sum of word population counts. Set bits can be enumerated without testing all 65,536 positions:

```c++
for (int i = 0; i < 1024; i++) {
    uint64_t x = words[i];
    while (x) {
        unsigned bit = __builtin_ctzll(x);
        visit(64 * i + bit);
        x &= x - 1;
    }
}
```

The zero check is required because `ctz(0)` is undefined. The inner loop executes once per result, but the outer loop still executes once per physical word. This is why bitmap iteration is excellent when dense and terrible at 32 values per chunk. Array containers simply scan their stored lows.

## Comparison

Selected results from the fixed-cardinality sweep are:

| Values/chunk | Representation | Contains | Intersection input keys/s | Iteration | Bytes/key |
|--:|:--|--:|--:|--:|--:|
| 32 | sorted 32-bit array | 48.7 ns | 289 M/s | **0.04 ns/key** | 4.00 |
| 32 | bitmap chunks | **3.03 ns** | 204 M/s | 14.34 ns/key | 257.06 |
| 32 | adaptive | 6.42 ns | **295 M/s** | 0.19 ns/key | **3.81** |
| 2,048 | sorted 32-bit array | 45.3 ns | 301 M/s | **0.04 ns/key** | 4.00 |
| 2,048 | bitmap chunks | **0.87 ns** | **13,981 M/s** | 0.50 ns/key | 5.00 |
| 2,048 | adaptive | 18.1 ns | 271 M/s | 0.11 ns/key | **3.75** |
| 8,192 | sorted 32-bit array | 46.1 ns | 293 M/s | **0.04 ns/key** | 4.00 |
| 8,192 | bitmap chunks | 0.83 ns | **56,680 M/s** | 0.52 ns/key | **2.00** |
| 8,192 | adaptive | **0.80 ns** | 56,170 M/s | 0.50 ns/key | 2.75 |

“Intersection input keys/s” is defined by the harness as $|A|/t$: one input set's stored cardinality divided by the time for the complete intersection. It is a logical, workload-normalized throughput, not a count of bitmap positions, instructions, or physical values examined. The paired inputs have equal cardinality throughout this sweep, so the normalization is consistent across representations. Dense bitmap rates become enormous because a fixed 1,024-word chunk scan is amortized over many stored keys; they should not be read as a hardware instruction rate.

The adaptive structure is a compromise. It approaches sparse-array memory and dense-bitmap operations without winning every individual benchmark. If the workload is only membership and memory is plentiful, fixed bitmap chunks are faster. If it mostly iterates sparse sets, a sorted array is simpler and faster.

## Limits

The directory itself occupies hundreds of kilobytes even for an empty set. A production implementation can replace it with a sorted high-key directory when very few chunks are present, trading direct lookup for another search.

Long runs are another useful local representation. Full Roaring implementations also tune conversion thresholds, use vectorized container operations, and support mutation. We omit these features because the measured point is already visible with two container types: local density changes both the representation and the fastest algorithm.

Rank and select need additional indexes over population counts. They are important bitmap operations, but they constitute a separate data structure with their own space/latency trade-off; adding an unmeasured paragraph about them would not improve this case study.
