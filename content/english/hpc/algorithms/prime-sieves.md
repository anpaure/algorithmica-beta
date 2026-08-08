---
title: Prime Number Sieves
weight: 2
---

If we need to check whether one large number is prime, we use a primality test. If we need every prime up to some limit $n$, testing every number separately rediscovers the same small divisors over and over again.

The sieve of Eratosthenes reverses the work. It assumes the candidates are prime, then crosses out multiples of every prime it discovers. The asymptotic complexity is already close to linear; this case study is about making the marker stores land in the right part of the memory hierarchy.

## What We Measure

Every kernel returns only

$$
\pi(n)=|\{p\le n:p\text{ is prime}\}|.
$$

It does not materialize or print the primes. Workspace allocation is outside timing, while clearing markers, generating base primes for segmented variants, marking composites, and counting the surviving candidates are included. The algorithms use one byte per represented candidate, so bit packing is not part of this experiment.

Measurements were taken on one performance core of an Apple M4 Max with Apple Clang 17 and `-O3 -mcpu=native`. The machine has 128-byte cache lines, a 128 KiB L1 data cache, and a 16 MiB shared-per-cluster L2. Each sweep point is the median of five runs after one warmup. The phase row is the componentwise median of seven instrumented traversals after two warmups; its integer activity counts come from a separate deterministic arithmetic pass. The [complete test and benchmark program](../../../code/prime_sieves.cpp) emits the [raw CSV](../../../code/prime_sieves_m4_results.txt) used by the [Matplotlib plot script](../../../code/plot_prime_sieves.py) for all three figures.

## A Byte for Every Integer

The direct sieve is short:

```c++
uint64_t count_primes(uint32_t n) {
    vector<uint8_t> composite(size_t(n) + 1);

    for (uint32_t p = 2; p <= n / p; p++)
        if (!composite[p])
            for (uint64_t k = uint64_t(p) * p; k <= n; k += p)
                composite[k] = 1;

    uint64_t count = 0;
    for (uint32_t x = 2; x <= n; x++)
        count += !composite[x];
    return count;
}
```

Starting at $p^2$ is safe because every smaller composite multiple of $p$ has a smaller prime factor. The condition `p <= n / p` avoids overflowing the loop bound.

For each prime $p$, we perform roughly $n/p$ marker stores. Their total is

$$
n\sum_{p\le n}\frac1p=O(n\log\log n).
$$

At $n=2^{27}$, this implementation takes 395.484 ms. Its 128 MiB marker array is much larger than cache, and small primes repeatedly stream across it with different strides.

## Throwing Away the Evens

After accounting for prime 2, no even candidate needs a marker. Slot $i$ represents the odd number $2i+1$, and the odd multiples of an odd prime are

$$
p^2,\ p^2+2p,\ p^2+4p,\ldots
$$

Their compressed indices differ by $p$:

```c++
for (uint32_t p = 3; p <= n / p; p += 2)
    if (!composite[p / 2])
        for (uint64_t k = uint64_t(p) * p; k <= n; k += 2 * p)
            composite[k / 2] = 1;
```

This one representation change halves both the marker footprint and the densest marking work. At $2^{27}$ it reduces the time from 395.484 ms to 153.840 ms, a 2.57-fold speedup—slightly more than the factor of two suggested by capacity alone.

More elaborate wheels can omit multiples of 3, 5, and so on, while a bitmap can reduce capacity by another factor of eight. Both also make indexing and individual updates more expensive. They are useful alternatives, but they were not implemented or measured here.

## Keeping the Markers in Cache

The odd marker array still occupies 64 MiB at our largest input. Instead of revisiting all of it for each small prime, we sieve a short interval completely before moving on.

First generate the odd primes through $\lfloor\sqrt n\rfloor$. For a segment $[L,R)$, find the first odd multiple of each base prime and mark within one reusable array:

```c++
for (uint64_t low = 3; low <= n; low += 2 * S) {
    uint64_t high = min<uint64_t>(uint64_t(n) + 1, low + 2 * S);
    size_t slots = (high - low + 1) / 2;
    fill(composite.begin(), composite.begin() + slots, 0);

    for (uint64_t p : base_primes) {
        if (p * p >= high)
            break;

        uint64_t first = max(p * p, (low + p - 1) / p * p);
        if (first % 2 == 0)
            first += p;

        for (uint64_t k = first; k < high; k += 2 * p)
            composite[(k - low) / 2] = 1;
    }

    for (size_t i = 0; i < slots; i++)
        answer += !composite[i];
}
```

`S` counts odd candidates and bytes, so one segment covers a numerical interval of length $2S$. The `max` with $p^2$ prevents the segment containing $p$ from crossing out the prime itself.

With $S=2^{17}$, segmentation takes 51.671 ms at $n=2^{27}$, another 2.98-fold improvement over the odd full-array sieve. It is not free at small sizes: at $n=2^{18}$, the odd array already fits in 128 KiB and takes 0.062 ms, while segmentation takes 0.075 ms. The extra base sieve and segment setup cause a 21% regression before locality becomes a problem.

The full size sweep makes the transition visible. The cache lines in the figure refer to the uncompressed baseline marker; the segmented marker remains fixed at 128 KiB.

![Prime-counting throughput by upper bound](../img/prime-sieves-size.svg)

The baseline loses most of its throughput after its marker leaves L2. The segmented curve declines much more slowly because increasing $n$ changes the number of cache-resident blocks rather than the active marker size.

## Selecting the Segment Size

“Fits in cache” is not a complete tuning rule. Tiny segments repeat loop setup and scan the base-prime list too often; large segments bring back the cache misses. We swept every power of two from 1 KiB through 16 MiB on the same $n=2^{27}$ workload.

![Segment-size sweep](../img/prime-sieves-segment.svg)

The best measured point is 128 KiB: 49.114 ms, or 2.73 billion integers of the sieved range per second. A 1 KiB segment needs 174.143 ms because it performs far too many short segment passes. Increasing the marker once beyond L1D to 256 KiB raises the time to 59.940 ms. At 16 MiB it takes 72.962 ms. The optimum landing exactly on the nominal L1 capacity should not be generalized blindly—the base primes and other live state also compete for that cache—but it gives us the right value for this machine.

## Removing the Start Divisions

The direct segmented loop divides once for every base prime that can mark each block. At $n=2^{27}$ and $S=2^{17}$, the 512 segments produce 497,411 such prime visits and therefore 497,411 start divisions. Consecutive segments let us replace them with state: after marking a prime, retain the first multiple beyond the current segment and resume there in the next one.

```c++
vector<uint64_t> next(base_primes.size());
for (size_t i = 0; i < base_primes.size(); i++)
    next[i] = uint64_t(base_primes[i]) * base_primes[i];

// In each segment:
uint64_t k = next[i];
for (; k < high; k += 2 * p)
    composite[(k - low) / 2] = 1;
next[i] = k;
```

This removes all 497,411 start divisions, but the time only falls from 51.671 to 49.528 ms: a 4.1% improvement. A phase profile explains why. In separate phase-timed runs whose hot loops contain no counters, base-prime generation takes 0.007 ms, clearing segments 0.764 ms, marking 39.194 ms, and counting 8.451 ms. The arithmetic pass counts 130,353,348 byte stores. Division was visible, but it was never the dominant cost.

The complete progression at the largest input is:

![Prime-sieve optimization stages](../img/prime-sieves-stages.svg)

The first two changes attack memory traffic and locality and produce most of the 7.99-fold final speedup. Carrying offsets is still correct and measurable, but it is deliberately the smallest bar-to-bar step in the diary.

## Boundaries and Alternatives

The segment loop still visits base primes from the beginning for every block. More advanced sieves bucket future hits of large primes instead of scanning them repeatedly. Packed bitmaps trade extra masking instructions for eight times less marker storage. A wheel changes the mapping again. None of these is a free extension of the measured byte kernel, so none is included in its performance claim.

The linear sieve is another different contract. It marks each composite once and can provide the smallest prime factor of every integer, but stores and updates much more metadata. Its $O(n)$ bound does not imply that it beats this $O(n\log\log n)$ byte sieve for prime counting.

The harness checks the known values through $\pi(10^6)=78{,}498$, every boundary through 200, and 200 fixed-seed random limits below 200,000. For every limit it compares the full, odd, division-start, and carried-offset variants across segment sizes from one byte to 4096 bytes. The suite also passes with AddressSanitizer and UndefinedBehaviorSanitizer.

The final lesson is not merely “use segmentation.” Removing evens changes the amount of work, segmentation changes where the work happens, and retained offsets remove an instruction that profiling shows to be secondary. The asymptotic algorithm never changed; the memory traffic did.
