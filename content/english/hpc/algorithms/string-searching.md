---
title: String Searching
weight: 13
---

**Problem.** Given a byte string $s$ of length $n$ and a pattern $p$ of length $m$, find the first position $i$ such that

$$
s[i..i+m) = p[0..m).
$$

Searching Unicode text with case folding and normalization is a different problem; UTF-8 does not make it disappear by being stored in a `char` array. We will search arbitrary bytes, return zero for an empty pattern, and return $-1$ when no match exists.

The main idea of this case study is simple: proving that a position *cannot* match is much cheaper than comparing the whole pattern. On the workload below, a 16-byte NEON filter makes an exact search 18.7 times faster than the direct implementation. On another perfectly valid workload, the same filter makes it slower. We will keep both results.

## The Experiment

The [complete program](../../../code/string_searching_bench.cpp) tests and benchmarks six exact custom algorithms plus `std::search`. Its [five-process median CSV](../../../code/string_searching_m4_results.txt) is rendered by the [Matplotlib script](../../../code/plot_string_searching.py). All searches return the first match and never read beyond either input range. On non-AArch64 targets the same 16-position filters are labeled `scalar16`, not NEON.

The principal workloads are:

- 1 MiB of uniformly random bytes and an absent 16-byte pattern;
- 1 MiB over a four-symbol alphabet and an absent 32-byte pattern;
- a random 1 MiB haystack with a 32-byte match at its end;
- 256 KiB of `a` searched for `a...ab...a`, whose unusual middle byte defeats an endpoint filter;
- 256 KiB of 128 `a` bytes followed by one `b`, searched for 129 `a` bytes.

Inputs and patterns are generated from fixed seeds. The machine is an Apple M4 Max, and the program is compiled with Apple Clang 17.0.0 using `-O3 -mcpu=native`. Each process performs two warm-up runs and nine timed runs; the tables report the componentwise median of five independent processes. Pattern construction and input generation are outside the timed region. The KMP prefix table is also precomputed, while the cheap choice of two filter offsets is conservatively included in the filtered searches.

The metric is nanoseconds per haystack byte. Every timed search consumes at least 8 MiB in aggregate, and the returned index contributes to a checksum.

## Direct Search

The direct algorithm checks the pattern at every possible position:

```c++
int find(const unsigned char *s, size_t n,
         const unsigned char *p, size_t m) {
    if (m == 0)
        return 0;
    if (m > n)
        return -1;

    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, p, m) == 0)
            return int(i);

    return -1;
}
```

Its worst-case running time is $O(nm)$. It is not always bad in practice: on random bytes, almost every `memcmp` rejects the position after its first load. Even so, we call it at approximately 1024 candidate positions per KiB, and the benchmark takes **1.57 ns per haystack byte**.

The profiler does not reveal one slow arithmetic instruction. The overhead is structural: a call or inlined comparison, a branch, and loop bookkeeping are repeated at every position even though almost none can match.

## Filtering One Byte

Before comparing all $m$ bytes, the first byte has to agree:

$$
s[i]=p[0].
$$

NEON compares this condition for 16 positions at once. A small reduction converts the sixteen comparison bytes into a bit mask, and we visit only its set bits:

```c++
uint8x16_t bytes = vld1q_u8(s + i);
uint8x16_t equal = vceqq_u8(bytes, vdupq_n_u8(p[0]));
unsigned mask = neon_movemask(equal);

while (mask != 0) {
    unsigned j = __builtin_ctz(mask);
    if (memcmp(s + i + j, p, m) == 0)
        return int(i + j);
    mask &= mask - 1;
}
```

On AArch64 there is no single `movemask` instruction. Clang emits `cmeq.16b`, three pairwise `uaddlp` reductions, and then uses `rbit` plus `clz` for the first set bit. The arithmetic looks more elaborate than the source condition, but it replaces sixteen independently controlled candidate checks.

For random bytes, only 4.06 positions per KiB reach `memcmp`. The time falls to **0.063 ns per byte**, about 25 times faster than the baseline.

## Filtering Two Bytes

One condition is not special. We can also require the last byte to agree:

$$
s[i] = p[0]
\qquad\text{and}\qquad
s[i+m-1] = p[m-1].
$$

```c++
unsigned first = equal_mask_16(s + i, p[0]);
unsigned last  = equal_mask_16(s + i + m - 1, p[m - 1]);
unsigned mask = first & last;
```

The vector-loop bound guarantees that both 16-byte loads remain inside the haystack. An intrinsic does not make an out-of-range load legal.

For independent random bytes, this second test is usually unnecessary: the candidate rate falls from 4.06 to 0.023 per KiB, but the extra load, comparison, and mask reduction make the search regress from 0.063 to **0.076 ns per byte**. On a four-symbol alphabet, where the first-byte filter passes roughly one position in four, the same change improves the time from 0.74 to **0.52 ns per byte**.

This is why a single “random string” benchmark is not enough. The optimization trades a fixed amount of filter work for fewer full comparisons, and the alphabet controls the exchange rate.

![](../img/string-searching-stages.svg)

## Better Filter Positions

The first and last positions are convenient, not optimal. Consider a pattern made almost entirely of `a` with one `b` in its middle, searched in a long run of `a`. Both endpoints pass everywhere, and nearly every `memcmp` reads half of the pattern before failing.

We choose two offsets whose byte values occur least often *inside the pattern*. This is only a cheap heuristic—we do not scan the haystack to build a frequency table—but it finds the unusual middle byte in this example. The rest of the loop is unchanged:

```c++
FilterPlan plan = choose_filter(p, m);

unsigned x = equal_mask_16(s + i + plan.first,  p[plan.first]);
unsigned y = equal_mask_16(s + i + plan.second, p[plan.second]);
unsigned mask = x & y;
```

On the middle-byte workload, the endpoint filter takes 2.65 ns per byte and passes essentially every position. The selected-offset filter passes none and takes **0.084 ns per byte**. On random bytes it is slightly slower than the endpoint version because selecting offsets and addressing internal positions add work to an already easy search.

## The Remaining Bad Case

A filter changes constants, not worst-case complexity. The last workload contains only runs of 128 `a` bytes, while the pattern asks for 129. No pair of positions can reject most windows: many candidates resemble a match for a long time.

KMP solves this problem in $O(n+m)$ time by remembering how much of the pattern remains useful after a mismatch. Its prefix table is built once for the pattern. Our adaptive implementation uses the selected-byte filter first and, for a pattern where one byte value occupies at least three quarters of the positions, switches to KMP after 32 failed full comparisons.

This heuristic improves the repetitive-window workload from 2.99 to **1.77 ns per byte**. It does not claim a global worst-case bound: an adversary can construct a non-dominant pattern that also defeats the filter. When a linear bound is part of the interface, use KMP or the Two-Way algorithm from the beginning. The benchmark includes KMP separately so that this tradeoff is visible.

![](../img/string-searching-workloads.svg)

## Memory Size

The random-byte filter executes little work per 16-byte block. We therefore swept the haystack from $2^{10}$ through $2^{26}$ bytes. The machine has a 128 KiB L1 data cache and a 16 MiB L2 cache shared by a performance-core cluster.

![](../img/string-searching-size.svg)

Both curves are almost flat across the cache boundaries because they make sequential passes and reuse almost no haystack data. The direct search remains compute-bound by per-position control; the filtered search is close to the bandwidth of a simple sequential scan. Cache capacity matters much less here than it does for a random-access data structure.

## Final Comparison

The table reports nanoseconds per haystack byte. Lower is better.

| Implementation | Random absent | Four-symbol absent | Rare middle byte | Repetitive windows |
|:--|--:|--:|--:|--:|
| naive `memcmp` | 1.57 | 1.28 | 2.42 | 2.69 |
| one-byte NEON | **0.063** | 0.74 | 2.69 | 2.93 |
| endpoint NEON | 0.076 | 0.52 | 2.65 | 2.91 |
| selected-offset NEON | 0.084 | 0.54 | **0.084** | 2.99 |
| adaptive filter + KMP | 0.084 | **0.50** | 0.085 | 1.77 |
| KMP | 0.55 | 3.09 | 0.77 | **1.63** |
| `std::search` | 0.29 | 1.75 | 16.88 | 20.59 |

There is no row that wins every column. The one-byte filter is best when candidate bytes are rare. A second byte helps a small alphabet. Selecting an internal byte fixes an important periodic-looking case. A classical linear algorithm is less spectacular on friendly data and much less fragile on hostile data.

## Correctness and Limits

The harness exhaustively enumerates all haystacks through length eight and patterns through length five over a two-byte alphabet, then checks another 100,000 fixed-seed random pairs against `std::search`. The same suite runs under AddressSanitizer and UndefinedBehaviorSanitizer, which is important here: a vector search can return the right answer while still reading beyond the allocation.

The special cases are part of the implementation. Empty patterns return zero, one-byte patterns use `memchr`, short tails are scalar, and all arithmetic uses `size_t` until the benchmark-sized result is converted to `int`.

For many patterns, use an algorithm such as Aho–Corasick rather than scanning the haystack independently for each one. For one pattern that is reused many times, more preprocessing—such as a full Two-Way searcher or a haystack-informed filter—may pay. Those are different workload contracts, and their setup cost should not be hidden inside this benchmark.
