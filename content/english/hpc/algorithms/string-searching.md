---
title: String Searching
weight: 13
---

**Problem.** Given a string $s$ of length $n$ and a pattern $p$ of length $m$, find the first position $i$ such that

$$
s[i..i+m) = p[0..m).
$$

We will search byte strings. Searching Unicode characters with case folding and normalization is a different problem; UTF-8 does not make it disappear by being stored in `char` arrays.

## Naive Search

The direct algorithm checks the pattern at every possible position:

```cpp
int find(const char *s, int n, const char *p, int m) {
    if (m == 0)
        return 0;

    for (int i = 0; i + m <= n; i++)
        if (memcmp(s + i, p, m) == 0)
            return i;

    return -1;
}
```

Its worst-case running time is $O(nm)$. Searching for `aaaaab` in a long sequence of `a` compares almost the whole pattern at almost every position.

On ordinary text, it often behaves much better. Most positions fail after comparing the first byte, and `memcmp` is itself highly optimized. This is a recurring problem in string searching: an algorithm with a strong worst-case bound can lose to a much simpler one on short, non-periodic patterns.

Classical algorithms improve one of these cases:

- Knuth–Morris–Pratt remembers borders of the pattern and guarantees $O(n+m)$ comparisons.
- Boyer–Moore compares from the end and can skip many positions after a mismatch.
- The Two-Way algorithm uses the period of the pattern to obtain linear worst-case time with constant extra memory.

Libraries normally do not pick one algorithm for every pattern length. The empty and one-byte cases need almost no setup, short patterns favor small filters, and long or periodic patterns need stronger skipping guarantees.

## Filtering Candidate Positions

Before comparing all $m$ bytes, we can test two necessary conditions:

$$
s[i] = p[0]
\qquad\text{and}\qquad
s[i+m-1] = p[m-1].
$$

If either endpoint differs, position $i$ cannot be a match. AVX2 lets us perform these two tests for 32 positions at once:

```cpp
int find(const char *s, int n, const char *p, int m) {
    if (m == 0)
        return 0;
    if (m > n)
        return -1;

    if (m == 1) {
        const char *q = (const char*) memchr(s, p[0], n);
        return q ? int(q - s) : -1;
    }

    __m256i first = _mm256_set1_epi8(p[0]);
    __m256i last  = _mm256_set1_epi8(p[m - 1]);

    int candidates = n - m + 1;
    int i = 0;

    for (; i + 32 <= candidates; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i*) (s + i));
        __m256i b = _mm256_loadu_si256((const __m256i*) (s + i + m - 1));

        __m256i x = _mm256_cmpeq_epi8(a, first);
        __m256i y = _mm256_cmpeq_epi8(b, last);
        unsigned mask = unsigned(_mm256_movemask_epi8(_mm256_and_si256(x, y)));

        while (mask) {
            int j = __builtin_ctz(mask);
            if (memcmp(s + i + j, p, m) == 0)
                return i + j;
            mask &= mask - 1;
        }
    }

    for (; i < candidates; i++)
        if (s[i] == p[0] && s[i + m - 1] == p[m - 1]
            && memcmp(s + i, p, m) == 0)
            return i;

    return -1;
}
```

The vector loop condition is part of the algorithm. It guarantees that both 32-byte loads remain inside the string; reading past the end is not made legal by using an intrinsic.

`movemask` turns the result of 32 byte comparisons into a 32-bit integer. The `mask &= mask - 1` trick removes its lowest set bit, so the inner loop visits only candidate positions. `__builtin_ctz` is called only when the mask is nonzero.

For independent uniformly distributed bytes, the first-byte test accepts one position out of 256, and testing two different bytes accepts roughly one out of $256^2$. Text is neither uniform nor independent, but two bytes still tend to reject far more candidates than one.

The first and last bytes are only the easiest choice. If the pattern starts and ends with common characters, we can select two rare internal bytes instead. The offsets are found once while preprocessing the pattern and then added to the two vector loads.

## The Bad Case

The filter changes the constant, not the worst-case complexity. Searching for a long sequence of `a` inside another long sequence of `a` passes both endpoint tests at every position and calls `memcmp` each time.

There are two reasonable ways to handle this:

1. inspect the pattern before searching and use a linear algorithm when it is highly periodic;
2. start with the SIMD filter, count failed full comparisons, and switch to a linear algorithm when there are too many.

The second approach preserves the very cheap common path while preventing adversarial input from keeping the algorithm in its bad regime. The fallback threshold is a performance parameter, not a correctness parameter.

Rolling hashes provide another filter. Rabin–Karp updates the hash of an $m$-byte window in constant time and checks the bytes when the hash agrees. Unless collisions are impossible by construction, the final comparison is still necessary for an exact search. A hash match is a candidate, not a proof.

## Special Cases

The one-byte case deserves its own path. `memchr` implementations already use vector instructions, architecture dispatch, and careful page-boundary handling. Replacing them with another hand-written AVX2 loop usually adds code without adding useful work.

For two- and three-byte patterns, a specialized loop may also be better than setting up a general string-search algorithm. At the other extreme, searching for many patterns independently wastes memory bandwidth. Algorithms such as Aho–Corasick combine all patterns into one automaton and scan the input once.

## Benchmarking

String-search benchmarks are easy to rig accidentally. Performance depends on

- the pattern length and period;
- whether a match exists and where the first one occurs;
- the alphabet size and byte frequencies;
- the number of candidates that reach `memcmp`;
- whether preprocessing is reused for many searches;
- whether the input is in cache.

Absent random patterns measure the SIMD filter's easiest case. Also test matches at the beginning and end, one-byte patterns, long common prefixes, and low-alphabet periodic strings.

Correctness testing is simpler. Compare the result with a reference implementation for random byte strings, and exhaustively enumerate all short strings over a two-letter alphabet. The latter catches most off-by-one errors in the vector tail. Run the same tests with AddressSanitizer, because a search can return the right position while still reading beyond the allocation.

String searching is fast when we spend almost no work on positions that cannot match. SIMD rejects many positions at once; classical string algorithms prove that some positions do not need to be visited at all. A practical implementation uses both ideas.

## Further Reading

- Boyer and Moore's [original paper](https://doi.org/10.1145/359842.359859) develops right-to-left matching and skip tables.
- Crochemore and Perrin's [Two-Way algorithm](https://doi.org/10.1145/79147.79151) gives a constant-space linear search.
- The [glibc `strstr` implementation](https://sourceware.org/git/?p=glibc.git;a=blob;f=string/strstr.c) shows how short-pattern filters and a linear fallback are combined in a real library.
