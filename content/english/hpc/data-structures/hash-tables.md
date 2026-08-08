---
title: Hash Tables
weight: 8
---

A hash table is supposed to take constant time. This is true in the same way that fetching one value from memory takes constant time: the notation is correct, but it conceals almost everything that determines the running time.

## Hash Tables

<img src="https://upload.wikimedia.org/wikipedia/commons/7/7d/Hash_table_3_1_1_0_1_0_0_SP.svg" width="500" alt="Keys mapped by a hash function into an array of buckets">

A hash function maps each key to a bucket. What happens when several keys choose the same bucket is the first important layout decision.

### Chaining

<img src="https://upload.wikimedia.org/wikipedia/commons/d/d0/Hash_table_5_0_1_1_1_1_1_LL.svg" width="500" alt="A chained hash table with colliding keys stored in linked lists">

The standard chaining design stores a collection of linked lists or growable arrays, one for each bucket. Colliding keys remain in the same bucket and are distinguished by comparing the complete key.

### Open Addressing

<img src="https://upload.wikimedia.org/wikipedia/commons/b/bf/Hash_table_5_0_1_1_1_1_0_SP.svg" width="500" alt="An open-addressed hash table with colliding keys stored in later cells">

Open addressing instead keeps a fixed array of cells and uses a probe function $f_i(x)$ to decide where to look on the $i$-th step. A collision advances to another cell in the same array rather than following a separately allocated node.

Here is the original cyclic-array sketch, with four local repairs: the power-of-two size is a compile-time constant, every key slot is initialized, hashing and masking are unsigned, and probing terminates even if the table is full.

```cpp
struct hashmap {
    static constexpr size_t size = 1u << 24;
    static constexpr size_t mask = size - 1;
    static constexpr int empty = -1;

    int a[size], b[size];

    hashmap() { std::fill(a, a + size, empty); }

    static uint32_t h(uint32_t x) {
        return (x ^ 179u) * 7u;
    }

    bool add(int x, int y) {
        if (x == empty)
            return false;

        size_t k = h(static_cast<uint32_t>(x)) & mask;
        for (size_t probes = 0; probes < size; probes++) {
            if (a[k] == empty || a[k] == x) {
                a[k] = x;
                b[k] = y;
                return true;
            }
            k = (k + 1) & mask;
        }
        return false;
    }

    bool get(int x, int &y) const {
        if (x == empty)
            return false;

        size_t k = h(static_cast<uint32_t>(x)) & mask;
        for (size_t probes = 0; probes < size; probes++) {
            if (a[k] == empty)
                return false;
            if (a[k] == x) {
                y = b[k];
                return true;
            }
            k = (k + 1) & mask;
        }
        return false;
    }
};
```

The two layouts have the same expected asymptotic complexity, but this change alone produced a 2–3x difference in the motivating benchmark.

<img src="https://upload.wikimedia.org/wikipedia/commons/1/1c/Hash_table_average_insertion_time.png" width="500" alt="Average insertion time rising as an open-addressed hash table fills">

The immediate trade-off is capacity. As an open-addressed table fills, adjacent clusters merge and probe sequences grow quickly, so it needs to be rebuilt at a lower load factor. A dynamic table allocates a larger array and inserts every key again; copying cells to the same indices would be wrong because the capacity, mask, and probe sequences have changed. If capacity doubles each time, all earlier rebuilds move fewer than twice as many elements as the current table contains, preserving $O(1)$ amortized insertion.

The sketch still hides almost every decision that determines real performance. How full is the table allowed to become? What do the keys look like? Do we need deletion, stable pointers, and automatic growth? And, most importantly, which memory locations does a query have to fetch?

In this case study, we fix those choices and build a deliberately narrow integer hash table. Replacing nodes with a flat array makes dependent lookups much faster and cuts memory by roughly three times, but initially makes independent queries slower. Splitting keys from values recovers that throughput. Fingerprints then give us a deliberately untidy result: they do not win at the headline 75%-full point, and the ranking at high load changes non-monotonically with table size.

## Contract and Benchmark

We store a mapping from nonzero 32-bit integer keys to 32-bit integer values:

```c++
void add(uint32_t key, uint32_t value); // insert or replace
bool find(uint32_t key, uint32_t &value) const;
```

The maximum number of entries is known in advance. Capacity is a power of two, allocation happens before the timed region, and the table is single-threaded. We do not support deletion, stable references, iteration order, or adversarial keys. Those omissions are not incidental: they are the reason a specialized table can use a simpler layout than a general container.

All measurements in this article were taken on one performance core of an Apple M4 Max. Its L1 data cache is 128 KiB, its performance-cluster L2 cache is 16 MiB, and its cache lines are 128 bytes. The code was compiled with Apple Clang 17 using `-O3 -mcpu=native`.

For throughput, we issue 300,000 pre-generated independent queries, half successful and half unsuccessful. For latency, each successful lookup returns the key of the next lookup, forming a 300,000-step dependency chain. Each process takes the median of five runs; the published table is the componentwise median of five independent processes. The same integer mixer is used by every implementation. The complete [benchmark and tests](/code/hash_tables_bench.cpp), [raw measurements](/code/hash_tables_m4_results.txt), and [plot generator](/code/data_structures_plots.py) are available with the book.

### Hashing to a Cell

Making the capacity a power of two replaces division by a bit mask:

```c++
size_t i = hash(x) & (capacity - 1);
```

This makes the quality of the low hash bits important. The tiny hash in the introductory sketch is useful for showing the mechanics, but regular keys can make it form one enormous cluster. The measured tables all use the same unsigned 32-bit avalanche finalizer:

```c++
uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}
```

Unsigned multiplication wraps modulo $2^{32}$ by definition. This is not a cryptographic or keyed hash and must not be used for hostile input; its purpose here is to keep us from benchmarking an accidental pattern in the low bits.

## Node-Based Baseline

We start with `std::unordered_map`, set its maximum load factor, and call `rehash(capacity)` before insertion. On the measured libc++ this produces the requested power-of-two bucket count; the harness asserts that contract, so the reported capacity and load match the flat tables exactly. A conventional chained table first fetches a bucket pointer and then follows it to a separately allocated node. A collision may add another dependent pointer read.

This is costly in both time and space. At a capacity of $2^{22}$ cells and 75% load, `std::unordered_map` used 34.7 allocated bytes per live eight-byte key-value pair. An independent lookup took 21.3 ns, while a dependent successful lookup took 298 ns. Independent queries overlap many cache misses; the chain cannot, which is why the two numbers are so different.

The baseline is quite good while it is hot. With only 3,072 entries, it answers the mixed workload in 6.2 ns, faster than all of the flat tables below. Pointer chasing is a problem only after the pointers lead somewhere expensive.

## Flat Cells

Open addressing stores each key and value in the bucket array itself. A zero key marks an empty cell, and collisions advance linearly:

```c++
struct Cell {
    uint32_t key, value;
};

bool find(uint32_t x, uint32_t &value) const {
    size_t i = hash(x) & mask;
    while (cell[i].key != 0) {
        if (cell[i].key == x) {
            value = cell[i].value;
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}
```

Reaching an empty cell proves that the key is absent because insertion and lookup follow the same probe sequence. The implementation requires at least one empty cell; in practice, performance collapses much earlier as adjacent occupied cells merge into clusters.

At $2^{22}$ cells and 75% load, the flat array takes 10.7 bytes per key. Mixed lookup throughput actually *regresses* from 21.3 to 25.6 ns, but dependent-hit latency falls from 298 to 53.2 ns. This is the first important split in the result: contiguous storage removes dependent pointers, but a scalar linear-probing loop is not automatically a higher-throughput loop.

On small tables the same regression is even clearer. At 3,072 entries the flat table needs 10.5 ns per mixed lookup compared with 6.2 ns for `unordered_map`. The standard table is already cached, while linear probing adds its own loop and collision work.

## Separating Keys and Values

An unsuccessful lookup never needs a value. We can put the two fields in separate arrays:

```c++
alignas(128) uint32_t key[N];
alignas(128) uint32_t value[N];
```

The probe loop now reads only `key`; it touches `value` after finding a match. This is the same [structure-of-arrays](/hpc/cpu-cache/aos-soa/) transformation used in many streaming computations, but its benefit depends on the query result.

For the large 75%-full table, mixed throughput improves from 25.6 to 19.1 ns and finally beats the standard table's 21.3 ns. Dependent successful lookups regress from 53.2 to 67.5 ns because every hit now needs a second memory stream. SoA is not a universally better layout: it trades hit locality for miss bandwidth.

![Mean hash-table lookup cost as capacity grows](../img/hash-tables-size-m4.svg)

## Fingerprint Groups

Full keys are still four bytes each. For a negative query, we can first inspect a one-byte fragment of each hash and fetch a key only when its fingerprint matches.

The portable implementation uses groups of 16 control bytes. This is one NEON register on Arm and one SSE register on x86, although the measured version below intentionally uses ordinary scalar C++ and lets Apple Clang decide how to lower it:

```c++
static constexpr size_t B = 16;

uint8_t fingerprint(uint32_t h) {
    return uint8_t((h >> 25) + 1); // 1..128; zero means empty
}

bool find(uint32_t x, uint32_t &value) const {
    uint32_t h = hash(x);
    uint8_t fp = fingerprint(h);
    size_t base = (h & mask) & ~(B - 1);

    while (true) {
        bool empty = false;
        for (size_t j = 0; j < B; j++) {
            size_t i = base + j;
            empty |= control[i] == 0;
            if (control[i] == fp && key[i] == x) {
                value = values[i];
                return true;
            }
        }
        if (empty)
            return false;
        base = (base + B) & mask;
    }
}
```

At 75% load and $2^{22}$ cells, fingerprints do not improve mixed throughput: 19.4 ns is a nominal 1.5% regression from SoA's 19.1 ns. They cost one extra byte per cell, raising storage from 10.7 to 12.0 bytes per key, and dependent hits slow to 86.1 ns because a successful query reads metadata and then the full key/value arrays.

The more interesting result appears at 87.5% load. With $2^{20}$ cells, grouped probing is a regression: 23.4 ns versus 21.0 ns for scalar SoA. With $2^{22}$ cells, the order reverses: fingerprints take 30.9 ns while SoA takes 36.6 ns. The complete sweep is not monotone—groups also win at $2^{16}$ cells and lose at $2^{12}$—so these measurements do not establish one cache-size threshold or a universal winner. They show that the extra metadata and the avoided full-key reads trade places as the concrete probe stream and working set change. Neither layout beats `std::unordered_map` at the largest high-load point, and inspecting a group at once does not repair a table that is too full.

![Hash-table lookup cost as the table fills](../img/hash-tables-load-m4.svg)

## Comparison

These are the results at $2^{22}$ cells and 75% load (3,145,728 live entries):

| Implementation | Mixed lookup | Dependent hit | Bytes/key |
|:--|--:|--:|--:|
| `std::unordered_map` | 21.3 ns | 297.8 ns | 34.7 |
| flat AoS | 25.6 ns | **53.2 ns** | **10.7** |
| flat SoA | **19.1 ns** | 67.5 ns | **10.7** |
| fingerprint groups | 19.4 ns | 86.1 ns | 12.0 |

The fastest implementation depends on which column matters. AoS wins serialized successful latency, while SoA wins this large 75%-full independent workload. At small sizes the standard library wins outright; at high load it also beats all three specialized tables for independent queries. Fingerprints win among the open-addressed variants in two of the four measured high-load sizes, not in a single continuous size regime.

This table also explains why “lookups per second” is not a latency measurement. Independent queries let the processor keep several misses in flight through [memory-level parallelism](/hpc/cpu-cache/mlp/). Linking each query to the preceding result removes that overlap and exposes the true dependency cost.

## Limits

The zero sentinel excludes one key. Separate control bytes remove that restriction, but this benchmark keeps the same nonzero-key contract for every implementation. Deletion would require tombstones or backward shifting; growth would require allocating a new table and reinserting every key because the capacity mask changes. Neither is free, and both change the relevant query distribution.

A poor hash can overwhelm every layout above. Power-of-two masking makes the low bits particularly important, so all tests use the same unsigned avalanche finalizer. It is not a keyed hash and is unsuitable for hostile input.

The transferable lesson is narrower than “open addressing is faster.” Contiguous cells remove dependent pointers; SoA avoids fetching unused values; fingerprints avoid fetching most keys. Each transformation saves one kind of traffic and adds another cost. The workload decides which cost is real.

## Acknowledgements

The fingerprint layout is closely related to Google's [Swiss table design](https://abseil.io/about/design/swisstables). The 32-bit integer finalizer is taken from Chris Wellons' [Hash Prospector](https://github.com/skeeto/hash-prospector), which searches for small mixers with good avalanche properties. The introductory diagrams and insertion-time plot are from Wikimedia Commons.
