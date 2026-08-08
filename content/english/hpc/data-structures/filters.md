---
title: Probabilistic Filters
weight: 10
---

Suppose that checking whether an object exists requires a disk read or a request to another machine. Most queries are misses, so we would like to reject them without performing the expensive lookup.

A *probabilistic filter* stores an approximate representation of a set and answers one of two things:

- the element is definitely not present;
- the element is probably present.

It has somewhat the inverse behavior of a cache. A cache hit is definitive and a miss tells us to look elsewhere; a filter miss is definitive and a hit tells us to look elsewhere. The real data structure remains the authority.

## Bloom Filters

A [*Bloom filter*](https://doi.org/10.1145/362686.362692) consists of an initially zero [bitmap](../bitset/) of $m$ bits and $k$ hash functions.

To insert an element $x$, we calculate $h_1(x),\ldots,h_k(x)$ and set all the corresponding bits. To query it, we check the same positions. If at least one bit is zero, $x$ was never inserted. If they are all one, the bits may either belong to $x$ or have been set by other elements, so we can only answer "probably present."

Assuming independent uniform hashes, after inserting $n$ elements, one particular bit remains zero with probability

$$
\left(1-\frac1m\right)^{kn}\approx e^{-kn/m}.
$$

Therefore, the false-positive probability is approximately

$$
p=\left(1-e^{-kn/m}\right)^k.
$$

For a fixed number of bits per element $b=m/n$, this expression is minimized when

$$
k=b\ln2,
$$

at which point roughly half of the bitmap is set and

$$
p\approx(0.6185)^b.
$$

For example, ten bits per element and seven probes give a false-positive probability close to one percent under the model. This is a sizing estimate, not a promise about a particular hash function or input distribution.

## Implementation

Computing $k$ unrelated hashes would often cost more than the bitmap probes. Instead, we can use *double hashing* and generate the positions as

$$
h_i(x)=h_1(x)+i\cdot h_2(x).
$$

Here is a small implementation for a power-of-two bitmap. We assume that `mix` is a suitable 64-bit hash for the keys we store:

```c++
typedef unsigned long long u64;

const int L = 24;
const int M = 1 << L;
const int K = 7;

u64 bits[M / 64];

void add(u64 x) {
    u64 h = mix(x);
    u64 step = mix(h) | 1; // odd, so it does not cycle early modulo M

    for (int i = 0; i < K; i++) {
        int p = h & (M - 1);
        bits[p >> 6] |= 1ull << (p & 63);
        h += step;
    }
}

bool maybe_contains(u64 x) {
    u64 h = mix(x);
    u64 step = mix(h) | 1;

    for (int i = 0; i < K; i++) {
        int p = h & (M - 1);
        if ((bits[p >> 6] >> (p & 63) & 1) == 0)
            return false;
        h += step;
    }

    return true;
}
```

Unsigned overflow in the hash sequence is intentional. Making the step odd ensures that the sequence can visit every residue modulo the power-of-two $M$.

We cannot delete an element by clearing its $k$ bits because some of them may also belong to other elements. A *counting Bloom filter* stores small counters instead of bits and decrements them on deletion, spending more memory and requiring us to handle counter overflow. If deletions are infrequent, rebuilding a normal filter is often simpler.

## Cache Behavior

The formula favors several probes, but a large Bloom filter may turn each of them into a separate cache miss. When the protected lookup is a network request this cost is insignificant; when it is a lookup in a nearby in-memory table, the filter itself may become slower than the work it saves.

A common modification is a *blocked Bloom filter*. One part of the hash selects a small block — usually one cache line — and the remaining bits select all $k$ positions inside it. The probes become correlated, slightly worsening the false-positive rate for the same number of bits, but a query normally fetches only one cache line. This is often a worthwhile exchange because it replaces random memory latency with a little extra arithmetic.

Queries should test the bits one at a time and return immediately on the first zero. Batched queries expose more [memory-level parallelism](/hpc/cpu-cache/mlp/) by keeping several independent filter probes in flight.

## Capacity and Correctness

The false-positive rate depends on the actual number of inserted elements. Inserting twice the planned capacity does not create false negatives, but it may set almost the whole bitmap and make nearly every query positive. A practical filter needs a capacity estimate and a rebuild policy.

The no-false-negative property also assumes that the filter and the real set are updated in the right order. If a key becomes visible in the database before its filter bits become visible, a concurrent query can incorrectly reject it. This is not a mathematical failure of the Bloom filter; it is an update protocol failure.

The array implementation above is single-threaded. Concurrent inserts or queries also require atomic bit updates or external synchronization; otherwise C++ data races and lost read-modify-write updates can themselves create false negatives.

There are other approximate membership structures. Cuckoo filters store short fingerprints in small candidate buckets and support deletion; quotient filters arrange fingerprints so that runs can be scanned sequentially. Their details differ, but the engineering question is the same: how much memory and query work should we spend to avoid one authoritative lookup?

The useful answer of a probabilistic filter is not "yes." It is the cheap and definitive "no."
