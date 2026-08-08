---
title: Knapsack with Bitsets
weight: 21
---

We have already used the knapsack problem as a small example when discussing [memory locality](/hpc/external-memory/locality/#dynamic-programming). We ended that example with a transition that fits in two lines:

```c++
bitset<W + 1> possible;
possible[0] = 1;
for (int x : cost)
    possible |= possible << x;
```

This looks suspiciously easier than the dynamic program it replaces. In this case the short code is not hiding a complicated library algorithm: the shift and the OR *are* the algorithm. In this section, we will derive it, write a runtime-sized version from scratch, and optimize it until the compiler processes more than a hundred states at a time.

On the machine used for the benchmarks below, the final implementation is 9–13 times faster than an already auto-vectorized byte dynamic program. More importantly, it uses one-eighth as much memory and exposes the actual limits of the computation.

## The Problem

We are given $n$ positive integers $c_0,c_1,\ldots,c_{n-1}$ and a nonnegative capacity $W$. Each integer may be used at most once, and we need the largest subset sum that does not exceed $W$:

$$
\max_{S \subseteq \{0,1,\ldots,n-1\}}
\left\{\sum_{i\in S}c_i\;\middle|\;\sum_{i\in S}c_i\le W\right\}.
$$

This is the subset-sum specialization of 0/1 knapsack: the value of an item is equal to its weight. Repeated costs are allowed; the items are distinct by their positions. The distinction matters. In the general problem, an item has an independent weight $w_i$ and value $v_i$, and for each total weight we need to store the largest value achieved. One bit is not enough for that problem, so the optimization in this article does not apply to it directly.

We will also only compute the optimal value for now. Recovering the actual subset requires retaining some history, which we will discuss near the end.

## Scalar Dynamic Programming

Let $D_i[s]$ indicate whether sum $s$ can be formed using the first $i$ items. For the next item of cost $c_i$, there are two possibilities: either we do not take it and keep the sum $s$, or we take it and extend a subset whose sum was $s-c_i$:

$$
D_{i+1}[s] =
\begin{cases}
D_i[s], & s < c_i, \\
D_i[s] \lor D_i[s-c_i], & s \ge c_i.
\end{cases}
$$

Only the previous layer is needed, so the item dimension can be collapsed. The direct implementation stores one byte per sum:

```c++
void subset_sum(const int *cost, int n, int W, unsigned char *possible) {
    fill(possible, possible + W + 1, 0);
    possible[0] = 1;

    for (int i = 0; i < n; i++) {
        int x = cost[i];
        if (x > W)
            continue;
        for (int s = W; s >= x; s--)
            possible[s] |= possible[s - x];
    }
}
```

The descending order is part of the algorithm. If we go from left to right, `possible[x]` can be set from `possible[0]` and then immediately used to set `possible[2*x]` during the same iteration. That silently changes the problem into *unbounded* knapsack, where one item may be taken any number of times.

This implementation takes $O(nW)$ time and $O(W)$ bytes of memory. The inner loop does very little useful work: it reads two Boolean values, ORs them, and writes one back. This is precisely the kind of computation where packing the data changes more than its storage size.

## Packing the States

Instead of storing each predicate in a byte, let bit $s$ of one long binary integer $B$ indicate whether sum $s$ is reachable. Shifting this integer left by $x$ moves every reachable sum to the sum obtained after taking an item of cost $x$:

```text
reachable sums:       0  3  5  6
B:                 ...01101001
B << 4:            011010010000
                         ^  ^  ^  ^
new sums:                4  7  9 10
```

Keeping both the old and shifted bits therefore performs the entire dynamic-programming transition:

$$
B \gets B \lor (B \ll x).
$$

The proof is the same as for the scalar recurrence. Every old set bit represents a subset that omits the new item, and every shifted set bit represents a subset that includes it. Since the shift reads the previous bit vector as one value, the new item is still used at most once.

On a 64-bit machine, this reduces the work to $O(n\lceil (W+1)/64\rceil)$ word operations and the memory to roughly $W/8$ bytes. This is [word-level parallelism](/hpc/complexity/models/#word-ram), not a shorter spelling of the same scalar loop.

The `std::bitset` version is useful when $W$ is a compile-time constant. For a runtime capacity, we need to store the words ourselves. This also lets us fuse the shift and OR instead of materializing a full temporary bitset.

## Shifting an Array of Words

We store bits $64k$ through $64k+63$ in `bits[k]`. For an item of cost $x$, split the shift into a whole-word part and an intra-word part:

$$
q = \left\lfloor\frac{x}{64}\right\rfloor,
\qquad
r = x \bmod 64.
$$

When $r\ne0$, destination word $d$ receives pieces from two source words:

$$
(\text{bits}[d-q] \ll r)
\;\lor\;
(\text{bits}[d-q-1] \gg (64-r)).
$$

The first source provides the low part of the shifted word, while the preceding source provides the bits that cross the word boundary. As in the byte implementation, we update the words from high to low so that all source words still belong to the previous state.

```c++
using u64 = uint64_t;

u64 last_word_mask(int W) {
    int used = W % 64 + 1;
    return used == 64 ? ~u64(0) : (u64(1) << used) - 1;
}

void add_item(u64 *bits, int W, int x) {
    if (x > W)
        return;

    int words = W / 64 + 1;
    int q = x / 64;
    int r = x % 64;

    if (r == 0) {
        for (int d = words - 1; d >= q; d--)
            bits[d] |= bits[d - q];
    } else {
        for (int d = words - 1; d > q; d--)
            bits[d] |= (bits[d - q] << r)
                     | (bits[d - q - 1] >> (64 - r));
        bits[q] |= bits[0] << r;
    }

    bits[words - 1] &= last_word_mask(W);
}
```

The separate `r == 0` path is necessary. Combining the two cases would evaluate a right shift by 64, which is undefined in C++. The final mask clears physical bits that lie beyond the logical capacity when $W+1$ is not divisible by 64.

To initialize the dynamic program, allocate `W / 64 + 1` zeroed words and set bit zero:

```c++
vector<u64> bits(W / 64 + 1);
bits[0] = 1;

for (int i = 0; i < n; i++)
    add_item(bits.data(), W, cost[i]);
```

This is already the dynamic equivalent of `b |= b << x`, but it scans the whole destination range from word `q` through word `W / 64`, regardless of which source words can be nonzero. In the kernel stress test described below, this version took 7.244 ms while the byte dynamic program took only 1.309 ms. Packing the states had made the program slower because it also made us scan millions of impossible states.

## Do Not Scan Known Zeroes

After processing items with total cost $h$, no reachable sum can be larger than $h$. We can track the upper bound

$$
h_i=\min\left(W,\sum_{\substack{0\le j<i \\ c_j\le W}}c_j\right)
$$

and only touch words that may contain reachable states. It is an upper bound, not necessarily the largest reachable sum, but that is all the loop needs.

There is one small complication. If the highest live source word is only partially full, shifting it may create one additional destination word. Handling that spill separately gives the hot loop uniform bounds and, as a useful side effect, makes it much easier for the compiler to vectorize.

```c++
void add_item(u64 *bits, int W, int &hi, int x) {
    if (x > W)
        return;

    int next_hi = hi > W - x ? W : hi + x;
    int old_last = hi / 64;
    int next_last = next_hi / 64;
    int q = x / 64;
    int r = x % 64;

    if (r == 0) {
        int top = min(next_last, old_last + q);
        for (int d = top; d >= q; d--)
            bits[d] |= bits[d - q];
    } else {
        int spill = old_last + q + 1;
        if (spill <= next_last)
            bits[spill] |= bits[old_last] >> (64 - r);

        int top = min(next_last, old_last + q);
        for (int d = top; d > q; d--)
            bits[d] |= (bits[d - q] << r)
                     | (bits[d - q - 1] >> (64 - r));
        bits[q] |= bits[0] << r;
    }

    bits[W / 64] &= last_word_mask(W);
    hi = next_hi;
}
```

Initialize `hi` together with the bitset and pass it through every update:

```c++
int hi = 0;
for (int i = 0; i < n; i++)
    add_item(bits.data(), W, hi, cost[i]);
```

The expression for `next_hi` avoids overflowing `hi + x`: after the `x > W` check, both operands of `W - x` are valid. We assume, as stated in the problem contract, that all costs are positive. A cost of zero changes no reachable sum and may simply be ignored.

Frontier tracking does not change the worst-case bound. Once `hi` reaches $W$, almost the full bitset may be visited for every remaining item. It saves work while the processed prefix is still much smaller than $W$; if the total never reaches $W$ and only the optimum is needed, the total-sum shortcut below bypasses the dynamic program completely.

Two other reductions are almost free:

- If the sum of all usable costs is at most $W$, that sum is the answer and no dynamic program is needed.
- If bit $W$ becomes set after an update, we can stop immediately because no larger legal answer exists.

If all costs have a common divisor $g$, divide every cost by $g$ and replace $W$ with $\lfloor W/g\rfloor$. All reachable sums are multiples of $g$, so the smaller problem has exactly the same answer after multiplying it by $g$. This can shrink both the running time and memory by a large factor on structured inputs.

## Finding the Answer

After all updates, scan the words from high to low. Once a nonzero word is found, `clz` gives the position of its highest set bit:

```c++
int best_sum(const u64 *bits, int W) {
    for (int i = W / 64; i >= 0; i--) {
        u64 word = bits[i];
        if (i == W / 64 && W % 64 != 63)
            word &= (u64(1) << (W % 64 + 1)) - 1;
        if (word)
            return 64 * i + 63 - __builtin_clzll(word);
    }
    return 0;
}
```

The zero check is required because `__builtin_clzll(0)` is undefined. This final scan takes $O(W/64)$ time once, rather than once per item.

## What the Compiler Does

The inner loop now performs two shifted reads, a few Boolean operations, and one read-modify-write for every 64 states. It is also regular enough to [auto-vectorize](/hpc/simd/auto-vectorization/).

For the benchmark build, `-Rpass=loop-vectorize` reports a vectorization width of 16 for the byte loop and a width of 2 for the 64-bit loop on the 128-bit SIMD hardware of the test machine. Cross-compiling the same loops for AVX2 produces four 64-bit lanes using variable packed shifts such as `vpsllvq` and `vpsrlvq`.

This is why we will not add a page of intrinsics. A manual AVX2 implementation would restate the same source loads and shifts while adding scalar boundary code, and it would not reduce memory traffic. The plain loop already communicates the operation well enough for the compiler.

It also explains why the real speedup is much smaller than 64. The scalar baseline is not actually processing one byte at a time: the compiler updates 16 byte states per vector instruction. With vectorization disabled, the bounded word kernel is about 45x faster in the same benchmark, but disabling a valid optimization is not a useful baseline.

## Benchmark

The [complete validation and benchmark program](../../../code/knapsack.cpp) is available alongside the article. The following measurements were taken on an Apple M4 Max using Apple Clang 17 with `-O3 -mcpu=native`. Inputs are generated from a fixed seed and allocation is outside the timed region, while zero-initialization is included. Each process discards two warm-up runs and takes 9 timed samples for the 100,000- and 5,000,000-capacity cases and 7 for the others. The table reports the componentwise median of five independent process runs. All three implementations produce the complete set of reachable sums rather than stopping early at an exact fill.

The scalar implementation uses the same `hi` bound as the final bitset version. “Full words” is the first `uint64_t` implementation, which ignores the live frontier and scans the full destination range, and “bounded words” is the final implementation.

| $n$ | $W$ | Cost distribution | Byte DP | Full words | Bounded words | Speedup |
|----:|----:|:------------------|--------:|-----------:|--------------:|--------:|
| 1,000 | 100,000 | uniform $[1,1000]$ | 1.652 ms | 0.202 ms | 0.184 ms | 9.0x |
| 2,000 | 1,000,000 | uniform $[1,1000]$ | 20.973 ms | 4.010 ms | 2.073 ms | 10.1x |
| 2,000 | 1,000,000 | uniform $[1,10^6]$ | 26.149 ms | 2.065 ms | 2.064 ms | 12.7x |
| 500 | 5,000,000 | uniform $[1,1000]$; kernel stress test | 1.309 ms | 7.244 ms | 0.136 ms | 9.6x |

The last row isolates the kernels by deliberately disabling the total-sum shortcut. The sum of all 500 costs is only 255,908, so an optimum-only solver should simply return that number without running any dynamic program. If the complete set of reachable sums is required, the full bitset loop repeatedly scans almost five million impossible states. It is **5.5x slower** than the byte dynamic program despite doing 64 states per word, while the bounded loop only visits the live prefix.

For wide weights, both word implementations take essentially the same time because the live frontier reaches the capacity almost immediately. The remaining kernel becomes a sequence of cache-friendly passes. As $W$ grows beyond the caches, it eventually becomes [memory-bandwidth bound](/hpc/cpu-cache/bandwidth/), and additional arithmetic parallelism helps less and less.

The exact ratios are not universal. Small costs, wide costs, repeated costs, a common divisor, and the position of the first exact fill all change how many words are visited. Knapsack benchmarks need to state their input distribution; quoting one context-free speedup is particularly misleading here.

## Recovering the Subset

The final bitset tells us which sums are reachable, but not how they were reached. The simplest way to retain a witness is to use the scalar dynamic program and remember the first predecessor of every new sum:

```c++
vector<int> previous(W + 1, -1);
vector<int> chosen(W + 1, -1);
previous[0] = 0;

for (int i = 0; i < n; i++) {
    int x = cost[i];
    if (x <= 0 || x > W)
        continue;
    for (int s = W; s >= x; s--) {
        if (previous[s] == -1 && previous[s - x] != -1) {
            previous[s] = s - x;
            chosen[s] = i;
        }
    }
}
```

Starting from the best reachable sum, repeatedly take `chosen[s]` and replace `s` with `previous[s]` until `s == 0`. Descending updates ensure that the resulting indices are distinct.

We could store one full bitset after every item and reconstruct the choices backwards, but that would increase memory from $O(W)$ bits to $O(nW)$ bits. Checkpointing some layers and recomputing the gaps trades time for space. There is no free reconstruction hidden in the final packed state: discarding provenance was part of how we made it small.

## Limits

The bitset implementation is fast, but it is still *pseudo-polynomial*. The capacity occupies only $O(\log W)$ bits in the input while the algorithm performs work proportional to $W$. A capacity of $10^9$ is large even if it is written with only ten decimal digits.

Different parameter ranges need different algorithms:

- When $n$ is small and $W$ is enormous, meet-in-the-middle enumerates two groups of subset sums in roughly $O(2^{n/2})$ time without allocating $W$ states.
- When only a few sums are reachable, maintaining a sorted sparse set can beat scanning a mostly empty bitset.
- General weight-value knapsack needs numeric states and usually returns to the descending `max` recurrence.

There are also asymptotically faster algorithms for subset sum in specialized regimes, including [Bringmann's near-linear pseudopolynomial algorithm](https://epubs.siam.org/doi/10.1137/1.9781611974782.69) and more recent work on [beating Bellman's algorithm](https://epubs.siam.org/doi/10.1137/1.9781611978322.157). They are algorithmically much more involved. For the capacities that fit comfortably in memory, the classical dynamic program remains a useful example of a broader principle: changing the representation can expose parallelism that was already present in the state transition.
