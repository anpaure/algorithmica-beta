---
title: Hash Tables
weight: 8
---


## Hash Tables

<img src="https://upload.wikimedia.org/wikipedia/commons/7/7d/Hash_table_3_1_1_0_1_0_0_SP.svg" width="500" alt="">

----

### Chaining

<img src="https://upload.wikimedia.org/wikipedia/commons/d/d0/Hash_table_5_0_1_1_1_1_1_LL.svg" width="500" alt="">

The standard chaining design stores a collection of linked lists or growable arrays, one for each bucket. Colliding keys remain in the same bucket and are distinguished by comparing the complete key.

----

### Open Addressing

<img src="https://upload.wikimedia.org/wikipedia/commons/b/bf/Hash_table_5_0_1_1_1_1_0_SP.svg" width="500" alt="">

Open addressing instead keeps a fixed array of cells and uses a probe function $f_i(x)$ to decide where to look on the $i$-th step. A collision advances to another cell in the same array rather than following a separately allocated node.

----

Implementation with a cyclic array:

```cpp
struct hashmap {
    static constexpr int size = (1<<24);
    int a[size], b[size];

    hashmap() { std::fill(a, a + size, -1); }

    static inline unsigned h(unsigned x) { return (x^179)*7; }

    void add(int x, int y) {
        int k = h(x) % size;
        while (a[k] != -1 && a[k] != x)
            k = (k + 1) % size;
        a[k] = x, b[k] = y; 
    }

    int get(int x) {
        for (int k = h(x) % size; a[k] != -1; k = (k + 1) % size)
            if (a[k] == x)
                return b[k];
        return -1;
    }
};
```

The two layouts have the same expected asymptotic complexity, but this change alone produced a 2–3x difference in the motivating benchmark.

----

<img src="https://upload.wikimedia.org/wikipedia/commons/1/1c/Hash_table_average_insertion_time.png" width="500" alt="">

The immediate trade-off is capacity: as an open-addressed table fills, its probe sequences grow quickly, so it needs to be rebuilt at a lower load factor.

The code above is a useful sketch, but it also hides almost every decision that matters for performance. How full is the table allowed to become? What do real keys look like? Do we need deletion, stable pointers, and automatic growth? And, most importantly, which memory locations does a query have to fetch?

There is no universally fastest hash table. In this section, we will fix the problem first and then optimize for it: the keys and values are 32-bit integers, the maximum number of keys is known ahead of time, the table is single-threaded, and we only need insertion and lookup. This narrower contract lets us build a small integer table instead of a general-purpose container.

## The Memory Problem

A typical `std::unordered_map` keeps a bucket array and allocates every element in a separate node. This is necessary for parts of its interface, most notably keeping references to elements valid after rehashing, but it is an unfortunate layout for lookups:

1. fetch the bucket pointer;
2. follow it to a separately allocated node;
3. load the key and compare it;
4. possibly follow another pointer after a collision.

The second read depends on the first one, so the processor cannot start it early. The nodes are also unlikely to be adjacent, and each of them stores a pointer in addition to the useful key and value. This is the same [pointer-chasing](/hpc/cpu-cache/latency/) problem we encountered with search trees.

Open addressing stores the elements in the bucket array itself. A collision merely advances us to another cell:

```c++
constexpr unsigned N = 1 << 20;

alignas(64) uint32_t key[N]; // zero means that the cell is empty
alignas(64) uint32_t val[N];
```

We deliberately keep keys and values in separate arrays. An unsuccessful lookup only needs the keys, while the corresponding value is fetched after a match. This [structure-of-arrays layout](/hpc/cpu-cache/aos-soa/) therefore spends less memory bandwidth on the common negative-query path.

The zero sentinel means that this particular table cannot store the key zero. Reserving one key is often the simplest option for integer tables. If the full key range is needed, we can keep one byte of metadata per cell instead; we will return to that option later.

### Hashing to a Cell

Making the table size a power of two lets us replace `% N` with a bit mask:

```c++
unsigned k = hash(x) & (N - 1);
```

This makes the quality of the low hash bits important. The illustrative hash function from the first implementation does not have this property: for example, keys that differ by `N` map to the same initial cell. Sequential keys happen to work well, but a different regular input pattern can create one enormous cluster.

For 32-bit integer keys, we can use a short *finalizer* that makes every input bit affect every output bit:

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

The arithmetic is unsigned, so the multiplications wrap modulo $2^{32}$ by definition. This function is not cryptographic and should not be used for untrusted keys; in that setting, we need a keyed hash designed for adversarial input.

### A Flat Table

With these decisions made, the scalar implementation is almost as short as the original sketch:

```c++
bool find(uint32_t x, uint32_t &y) {
    unsigned k = hash(x) & (N - 1);
    while (key[k] != 0) {
        if (key[k] == x) {
            y = val[k];
            return true;
        }
        k = (k + 1) & (N - 1);
    }
    return false;
}

void add(uint32_t x, uint32_t y) {
    unsigned k = hash(x) & (N - 1);
    while (key[k] != 0 && key[k] != x)
        k = (k + 1) & (N - 1);
    key[k] = x;
    val[k] = y;
}
```

Both operations walk through exactly the same probe sequence. Consequently, reaching an empty cell proves that the key is not stored anywhere later in the sequence.

There is no full-table check in this code. The contract is that we allocate at least twice as many cells as the maximum number of keys and clear the key array before use. Apart from making an infinite probe impossible, the low load factor keeps clusters short. Open addressing becomes slow *before* it becomes completely full: after cells merge into long occupied runs, one unlucky hash value forces us to scan the whole run.

For a dynamic table, we keep the number of occupied cells and allocate a new array when the load factor crosses a chosen threshold. We then insert all keys into the new table again; copying the cells verbatim would be incorrect because the mask, and therefore the probe sequences, have changed. If the capacity doubles each time, the total number of elements moved by all previous rebuilds is less than twice the current table size, so rehashing still costs $O(1)$ per insertion on average.

Deletion needs one more state. Replacing an erased key with zero may disconnect the probe sequence of another key, making it unreachable. The usual solutions are to leave a *tombstone*, move the following keys backwards until the gap is safe, or periodically rebuild the table. Tombstones are easy, but a tombstone cannot terminate a lookup and too many of them eventually turn every query into a long scan.

## Probing a Cache Line at a Time

The scalar implementation has removed the pointers, but it still inspects the fetched data one integer at a time. A cache miss brings in 64 bytes — sixteen keys — regardless of whether we ask for one key or all of them. We can compare the entire cache line while it is here.

For AVX2, a 64-byte group takes two registers. We align every starting position to a group boundary, compare both vectors with the broadcasted search key, and turn the comparison results into one 16-bit mask:

```c++
constexpr unsigned N = 1 << 20;
constexpr unsigned B = 16;

alignas(64) uint32_t key[N];
alignas(64) uint32_t val[N];

uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}

unsigned match(__m256i x, __m256i a, __m256i b) {
    a = _mm256_cmpeq_epi32(x, a);
    b = _mm256_cmpeq_epi32(x, b);
    return (unsigned) _mm256_movemask_ps(_mm256_castsi256_ps(a))
         | ((unsigned) _mm256_movemask_ps(_mm256_castsi256_ps(b)) << 8);
}
```

`match` returns a mask whose $i$-th bit is set when key $i$ in the group is equal to `x`. We use it twice: once to look for the key and once to look for empty cells.

```c++
bool find(uint32_t x, uint32_t &y) {
    __m256i q = _mm256_set1_epi32((int) x);
    __m256i z = _mm256_setzero_si256();
    unsigned k = (hash(x) & (N - 1)) & ~(B - 1);

    while (true) {
        __m256i a = _mm256_load_si256((__m256i*) &key[k]);
        __m256i b = _mm256_load_si256((__m256i*) &key[k + 8]);

        unsigned found = match(q, a, b);
        if (found) {
            y = val[k + (unsigned) __builtin_ctz(found)];
            return true;
        }

        if (match(z, a, b))
            return false;

        k = (k + B) & (N - 1);
    }
}

void add(uint32_t x, uint32_t y) {
    __m256i q = _mm256_set1_epi32((int) x);
    __m256i z = _mm256_setzero_si256();
    unsigned k = (hash(x) & (N - 1)) & ~(B - 1);

    while (true) {
        __m256i a = _mm256_load_si256((__m256i*) &key[k]);
        __m256i b = _mm256_load_si256((__m256i*) &key[k + 8]);

        unsigned found = match(q, a, b);
        if (found) {
            val[k + (unsigned) __builtin_ctz(found)] = y;
            return;
        }

        unsigned empty = match(z, a, b);
        if (empty) {
            unsigned i = k + (unsigned) __builtin_ctz(empty);
            key[i] = x;
            val[i] = y;
            return;
        }

        k = (k + B) & (N - 1);
    }
}
```

This is still linear probing, except that its unit is a cache line instead of one cell. At a low load factor, most queries stop after the first group. A negative lookup then consists of two aligned loads and a handful of vector operations — and it never touches the value array.

The vectorization does not make memory latency disappear. Its purpose is to make full use of every cache line we had to fetch anyway and to replace a run of scalar comparisons and loop branches with a few instructions. Independent lookups can also overlap their cache misses through [memory-level parallelism](/hpc/cpu-cache/mlp), so throughput will usually be better than the latency of one isolated query suggests.

### Fingerprints

Comparing full keys works well because our keys are only four bytes. For strings or other large objects, loading sixteen complete keys would be wasteful. A more general design stores a short *fingerprint* of each hash in a separate metadata array. A vector comparison filters a group of fingerprints, and only the few matching candidates require loading and comparing their full keys.

This is the central lookup trick in [Swiss tables](https://abseil.io/about/design/swisstables): they store a 7-bit hash fragment together with the state of each cell and use SIMD to inspect a group of metadata bytes. The separate state also removes the need to reserve a sentinel key and naturally distinguishes empty cells from tombstones.

The price is another array, extra state transitions, and a more complicated insertion procedure. For a fixed table of 32-bit integers, comparing the keys directly is smaller and does the same filtering without fingerprints.

## Evaluation

Hash-table benchmarks are unusually easy to bias. Before comparing this implementation with `std::unordered_map`, we need to fix at least:

- the distribution of inserted keys;
- the ratio of successful and unsuccessful lookups;
- the ratio of lookups, insertions, and deletions;
- the load factor and the amount of memory reserved in advance;
- whether we measure dependent-query latency or independent-query throughput.

The hash function should also be the same on both sides. Otherwise, we may end up benchmarking two mixers rather than two table layouts. The queries should be generated before the timed region, the result should contribute to a checksum, and the table should get a warm-up pass. These are the same [benchmarking rules](/hpc/profiling/noise/) as elsewhere, but here changing just the miss ratio or load factor can reverse the result.

For this reason, there is no honest context-free number for how much faster a flat table is. The useful conclusion is structural: when pointer stability, deletion, and arbitrary key types are not required, a contiguous table can replace dependent pointer reads with sequential cache-line reads, and SIMD can test everything in those cache lines at once. Whether that saves 20% or 5x is a property of the workload and the machine, not of the name of the data structure.

## Further Improvements

There are several directions in which this implementation can be extended:

- Store one-byte fingerprints and probe 16 or 32 cells per vector when keys are large.
- Use a different probing rule or Robin Hood insertion to reduce the variance of probe lengths at higher load factors.
- Prefetch the next group when long probe sequences are common, although this spends additional bandwidth and is usually harmful when most queries stop in the first group.
- Batch independent queries explicitly so that hash calculation and memory accesses from different lookups overlap.
- Keep small tables entirely inline and switch to heap storage only after they grow.

Most of these improvements trade something away: insertion speed, deletion speed, iteration order, pointer stability, memory usage, or implementation simplicity. As with all performance engineering, the right next optimization is the one indicated by the workload.

### Acknowledgements

The SIMD group-probing idea is closely related to Google's [Swiss table design](https://abseil.io/about/design/swisstables). The 32-bit integer finalizer is taken from Chris Wellons' [Hash Prospector](https://github.com/skeeto/hash-prospector), which searches for small integer mixers with good avalanche properties.
