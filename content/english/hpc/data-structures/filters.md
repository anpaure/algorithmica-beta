---
title: Probabilistic Filters
weight: 10
---

Suppose an authoritative membership lookup requires a disk read, a large hash-table probe, or a request to another machine. Most queries are misses. A *probabilistic filter* stores a cheaper approximation and answers one of two things:

- the key is definitely absent;
- the key is probably present, so we still have to consult the authority.

The useful answer is “no.” A false positive wastes an authoritative lookup; a false negative would be a correctness bug.

In this case study, we start with a conventional Bloom filter, remove redundant hash computations, and then force all probes into one 128-byte cache line. The blocked layout is faster once the bitmap leaves cache, but its correlated occupancy measurably increases the false-positive rate. That trade-off is the point of the data structure.

## Contract and Protocol

We store an append-only set of 64-bit keys:

```c++
void add(uint64_t key);
bool maybe_contains(uint64_t key) const;
```

Capacity is planned in advance. The benchmark uses seven probes, power-of-two bitmap sizes, deterministic non-adversarial keys, and a query stream containing 90% absent and 10% present keys. There is no deletion or concurrency. Every implementation is tested for false negatives before timing.

Measurements were made on an Apple M4 Max performance core with a 128 KiB L1 data cache, 16 MiB cluster L2, and 128-byte cache lines. Apple Clang 17 compiled the code with `-O3 -mcpu=native`. We time 300,000 independent queries and a separate 200,000-step serialized chain, reporting medians of five runs. The independent stream is the stated 90%-absent, 10%-present workload. The chain is a different diagnostic: its answers are overwhelmingly absent, each answer determines the next query, and one extra `mix64` generates that query. It is useful only for comparing serialization across layouts, not as standalone filter latency. The [complete harness](/code/filters_bench.cpp), [raw results](/code/filters_m4_results.txt), and [Matplotlib/Seaborn plot generator](/code/data_structures_plots.py) accompany the article.

The independent timings cover the filter only; the serialized diagnostic additionally includes the stated next-query `mix64`. Neither includes the authoritative lookup. The end-to-end value also depends on the miss ratio and the cost of the authority:

$$
C_{\text{total}}=C_{\text{filter}}
 + \Pr[\text{maybe}]\,C_{\text{authority}}.
$$

A filter that saves 2 ns but causes one additional network request is not faster.

## Bloom Filter Baseline

A Bloom filter is an initially zero [bitmap](../bitset/) of $m$ bits and $k$ hash functions. Insertion sets positions $h_1(x),\ldots,h_k(x)$. A query tests the same bits and stops at its first zero.

After inserting $n$ keys, a particular bit remains zero with probability approximately $e^{-kn/m}$. Under the independent uniform-hash model, the false-positive probability is

$$
p=\left(1-e^{-kn/m}\right)^k.
$$

Writing $b=m/n$ for bits per key, the optimum is near $k=b\ln2$. With ten bits per key, seven probes are close to optimal and the formula predicts about 0.82% false positives.

Our baseline actually computes seven separate hashes:

```c++
bool maybe_contains(uint64_t x) const {
    for (int i = 0; i < 7; i++) {
        uint64_t h = mix(x + C * uint64_t(i));
        size_t p = h & mask;
        if ((bits[p >> 6] >> (p & 63) & 1) == 0)
            return false;
    }
    return true;
}
```

For a 2 MiB filter at ten bits per key, the measured false-positive rate over 300,000 absent keys is 0.798%. The 90%-miss workload takes 13.2 ns per independent query. The separate serialized chain takes 17.8 ns per step, including the extra `mix64` that generates the next query. Early rejection keeps most negative queries from performing all seven probes.

## Double Hashing

Computing seven unrelated mixers is unnecessary. We can derive all positions from two hashes:

$$
h_i(x)=h_1(x)+i\,h_2(x).
$$

For a power-of-two bitmap, an odd step visits every residue before cycling:

```c++
uint64_t h = mix(x);
uint64_t step = mix(h) | 1;

for (int i = 0; i < 7; i++, h += step) {
    size_t p = h & mask;
    if ((bits[p >> 6] >> (p & 63) & 1) == 0)
        return false;
}
return true;
```

Unsigned overflow is intentional. The new filter has the same size and nearly the same observed error: 0.822% at ten bits per key. Independent query time improves from 13.2 to 12.7 ns. The serialized-chain cost improves from 17.8 to 15.6 ns per step, but these figures include next-query generation and should only be compared with each other.

The optimization is modest because most misses return before seven hashes. It helps positive queries and false positives more, since they execute the full loop.

## One Cache Line per Query

When the filter is large, seven positions may require seven unrelated cache lines. An M4 cache line holds 128 bytes, or 1,024 filter bits. A *blocked Bloom filter* uses disjoint parts of the hash to select one 1,024-bit block and the seven positions within it:

```c++
static constexpr size_t block_bits = 1024;

uint64_t h = mix(x);
uint64_t step = mix(h) | 1;
size_t block = ((h >> 10) & block_mask) * (block_bits / 64);

for (int i = 0; i < 7; i++, h += step) {
    size_t p = h & (block_bits - 1);
    if ((bits[block + (p >> 6)] >> (p & 63) & 1) == 0)
        return false;
}
return true;
```

Using the same low hash bits for both block selection and in-block positions would create severe correlations; the implementation deliberately splits them.

The bit array is explicitly aligned to 128 bytes, so a 1,024-bit block does not straddle two cache lines. At 2 MiB and ten bits per key, blocked lookup takes 12.0 ns independently. Its serialized-chain cost is 14.7 ns per step, including next-query generation. At a 16 MiB filter, it takes 15.5 ns per independent query compared with 15.9 ns for double hashing and 17.1 ns for seven independent hashes.

![Bloom-filter query cost across the M4 cache hierarchy](../img/filters-size-m4.svg)

Locality is not free. Keys are not distributed perfectly evenly among blocks, so an overloaded block sets more bits and contributes more false positives than an underloaded block removes. At ten bits per key, the blocked filter measures 0.977% false positives rather than 0.822%. At 16 bits per key, the gap is 0.132% versus 0.059%.

![Measured false-positive rates for standard and blocked layouts](../img/filters-fpr-m4.svg)

The dashed theoretical curve in the plot is $(1-e^{-7/b})^7$. The two conventional layouts follow it closely. The blocked layout does not satisfy the independent global-occupancy model, so its measured points sit above the curve.

## Comparison

For the 2 MiB filter with ten bits per inserted key:

| Layout | False positives | Independent 90/10 query | Serialized chain step* | Cache lines/query |
|:--|--:|--:|--:|:--|
| seven independent hashes | **0.798%** | 13.2 ns | 17.8 ns | up to 7 |
| double hashing | 0.822% | 12.7 ns | 15.6 ns | up to 7 |
| 128-byte blocked | 0.977% | **12.0 ns** | **14.7 ns** | normally 1 |

`*` Overwhelmingly absent answer-dependent queries, including one extra `mix64` per step. This column compares serialization across layouts; it is neither the 90/10 workload nor standalone filter latency.

The filter occupies exactly the same number of bits in all three rows. Double hashing changes computation; blocking changes address locality. The second transformation wins more time but also weakens the statistical result.

Whether 0.155 percentage points are acceptable depends on the authority. For a remote request, one extra false positive may cost more than the nanosecond saved by blocking. We can spend additional bits per key to restore the target rate: the measurements, not the standard Bloom formula, should size a blocked filter.

## Capacity and Correctness

The false-positive rate depends on the actual insertion count. Overfilling cannot create false negatives, but it drives the bitmap toward all ones until the filter rejects almost nothing. A practical implementation needs an insertion counter and a rebuild policy.

Clearing an element's bits is incorrect because other keys may share them. A counting filter replaces bits with counters to support deletion, spending more memory and introducing counter overflow. That is a different contract and needs a separate benchmark.

The no-false-negative property also depends on the update protocol. If a database key becomes visible before its filter bits, a concurrent reader may reject a real key. The arrays above are single-threaded; concurrent insertion requires atomic read-modify-write operations or external synchronization.

The engineering lesson is that a probabilistic filter has two performance outputs. Query time is one. The false-positive probability—which controls how much expensive work leaks through—is the other. Optimizing either in isolation can make the full system slower.
