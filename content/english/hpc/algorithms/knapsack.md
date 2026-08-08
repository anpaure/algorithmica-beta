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

This is usually presented as the optimization: replace one byte with one bit and process 64 states at a time. On the machine used for this case study, doing this carefully gives a 9.1-13.0x speedup over an already auto-vectorized byte dynamic program. This is nice, but it is also where the interesting part starts.

The bitset does not behave like a random array of bits. Its live prefix grows monotonically, and on many instances most of that prefix eventually becomes all ones. More importantly, constructing the *complete set* of reachable sums is a stronger problem than returning the best one. Exploiting these facts takes the running time on one dense million-state instance from 2.11 ms to 0.00188 ms — a 1123x speedup — while an adversarial instance stays within 1% of the ordinary word-parallel solver.

That does not mean the same machine instructions somehow became 1123 times faster. It is a sequence of progressively stronger observations about which instructions do not need to be executed at all. We will derive each of them, including two plausible optimizations that made the program slower.

## The Problem

We are given $n$ positive integers $c_0,c_1,\ldots,c_{n-1}$ and a nonnegative capacity $W$. Each integer may be used at most once, and we need the largest subset sum not exceeding $W$:

$$
\max_{S \subseteq \{0,1,\ldots,n-1\}}
\left\{\sum_{i\in S}c_i\;\middle|\;\sum_{i\in S}c_i\le W\right\}.
$$

This is the subset-sum specialization of 0/1 knapsack: the value of an item is equal to its weight. Repeated costs are allowed, and repeated items are still distinct by their positions. The algorithms below are exact for every such input; none of their correctness arguments relies on how the benchmark data is generated.

In the general weight-value problem, each item has a weight $w_i$ and an independent value $v_i$. For every total weight, we then need to remember the largest value achieved, and one bit is not enough. The representation trick in this article does not apply to that problem directly.

The [complete program](../../../code/knapsack.cpp) contains both implementations, fixed-seed workload generation, and differential tests against brute force. The [checked-in measurements](../../../code/knapsack_m4_results.txt) are rendered by the [plot script](../../../code/plot_knapsack.py), so every figure can be reproduced without scraping numbers from this page. The single-threaded measurements were taken on an Apple M4 Max using Apple Clang 17 and `-O3 -mcpu=native`. Every table is the componentwise median of five independent processes. Each process discards two warm-up samples and takes the median of 5, 7, or 9 measured samples. Even the fast kernels are run in calibrated batches lasting at least 3 ms before dividing by the batch size.

There are two timing boundaries, and we will keep them separate:

- The *kernel* benchmarks allocate the output before timing, but include clearing it. Sorting is included in the variants that sort.
- The *solver* benchmarks include validation, preprocessing, allocation, clearing, the dynamic program, and answer extraction. The very fast solvers are timed in calibrated batches lasting at least 3 ms.

This distinction is important. The first half of the article compares ways of constructing the complete reachable set. The second half only asks for the optimum and is allowed to stop as soon as it knows it.

## Scalar Dynamic Programming

Let $D_i[s]$ indicate whether sum $s$ can be formed using the first $i$ items. For the next item of cost $c_i$, either we omit it and preserve $s$, or include it and extend a subset whose sum was $s-c_i$:

$$
D_{i+1}[s] =
\begin{cases}
D_i[s], & s < c_i, \\
D_i[s] \lor D_i[s-c_i], & s \ge c_i.
\end{cases}
$$

Only the previous layer is needed, so we can collapse the item dimension and store one byte per sum:

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

The descending order is not a performance detail. If we go from left to right, `possible[x]` can be set from `possible[0]` and then immediately used to set `possible[2*x]` during the same iteration. We would silently solve *unbounded* knapsack, where the same item may be used repeatedly.

This implementation takes $O(nW)$ time and $O(W)$ bytes. Its inner loop reads two Boolean values, ORs them, and writes one back. There is almost no computation per byte moved, which makes the representation a more promising target than the instruction sequence.

## Packing the States

Let bit $s$ of one long binary integer $B$ indicate whether sum $s$ is reachable. Shifting this integer left by $x$ moves every reachable sum to the sum obtained after taking an item of cost $x$:

```text
reachable sums:       0  3  5  6
B:                 ...01101001
B << 4:            011010010000
                         ^  ^  ^  ^
new sums:                4  7  9 10
```

Keeping both the old and shifted bits performs the complete transition:

$$
B \gets B \lor (B \ll x).
$$

Every old set bit represents a subset that omits the new item; every shifted bit represents one that includes it. This is the scalar recurrence written vertically. On a 64-bit machine, it reduces the work to $O(n\lceil(W+1)/64\rceil)$ word operations and the storage to roughly $W/8$ bytes.

For a runtime capacity, we store the words ourselves. If bits $64k$ through $64k+63$ are in `bits[k]`, split an item cost into

$$
q=\left\lfloor\frac{x}{64}\right\rfloor,
\qquad
r=x\bmod64.
$$

When $r\ne0$, destination word $d$ receives pieces from two source words:

$$
(\text{bits}[d-q]\ll r)
\;\lor\;
(\text{bits}[d-q-1]\gg(64-r)).
$$

The first source provides the low part of the shifted word and the preceding source provides the bits crossing the word boundary. We still update from high to low, so every source word belongs to the state before this item:

```c++
void add_item(uint64_t *bits, int W, int x) {
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
}
```

The separate `r == 0` path avoids a right shift by 64, which is undefined in C++. The actual implementation also masks the unused bits in the last physical word.

This is the dynamic equivalent of `possible |= possible << x`, but it scans the full allocation for every item. On the sparse-frontier workload below, this first packed implementation took 7.371 ms, compared to 1.308 ms for the frontier-bounded byte program. Packing 64 states into a word had made it **5.6x slower** because it also made us scan millions of states known to be zero.

## Following the Frontier

After processing items with total usable cost $h$, no reachable sum can exceed $h$. We therefore maintain

$$
h_i=\min\left(W,\sum_{0\le j<i,\;c_j\le W}c_j\right)
$$

and visit only the words up to $h_i$. This is merely an upper bound on the largest reachable sum, but the loop needs no stronger fact.

If the highest source word is partial, its shift can spill into one extra destination word. Handling that spill separately leaves the hot loop with uniform bounds:

```c++
void add_item(uint64_t *bits, int W, int &hi, int x) {
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

    hi = next_hi;
}
```

Frontier tracking and bit packing attack different sources of waste. The first avoids known-zero states; the second performs the remaining transitions word-parallel. Applying both is consistently better than applying either alone:

| $n$ | $W$ | Cost distribution | Byte, full | Byte, bounded | Words, full | Words, bounded | Fair speedup |
|----:|----:|:------------------|-----------:|--------------:|------------:|---------------:|-------------:|
| 1,000 | 100,000 | uniform $[1,1000]$ | 1.902 ms | 1.703 ms | 0.229 ms | 0.187 ms | 9.1x |
| 2,000 | 1,000,000 | uniform $[1,1000]$ | 45.311 ms | 22.139 ms | 4.041 ms | 2.112 ms | 10.5x |
| 2,000 | 1,000,000 | uniform $[1,10^6]$ | 27.570 ms | 27.455 ms | 2.109 ms | 2.117 ms | 13.0x |
| 500 | 5,000,000 | uniform $[1,1000]$ | 56.584 ms | 1.308 ms | 7.371 ms | 0.133 ms | 9.9x |

![](../img/knapsack-stages.svg)

The last row intentionally asks for the complete reachable set even though the sum of all items is only 253,050. The full loops repeatedly visit almost five million impossible states. Once both representations get the same frontier bound, packed words are again about ten times faster.

To isolate this effect, we fixed $W=10^6$ and $n=512$, used equal positive costs, and varied their total from about $0.01W$ through $3W$. The full word loop remains near 1.05 ms because its work does not depend on the live prefix. The bounded loop grows from 0.0069 ms to 0.902 ms as the frontier expands:

![](../img/knapsack-frontier.svg)

This does not improve the worst-case complexity. After `hi` reaches $W$, the ordinary loop again scans almost the entire bitset for every remaining item. It only ensures that we reach that limit gradually instead of paying it from the first iteration.

## Reordering the Items

Constructing the complete reachable set is independent of item order. If we sort the costs in ascending order, then for every $k$, the first $k$ costs are the $k$ smallest ones. Their sum is no larger than the sum of any other $k$ items, so this ordering minimizes every prefix frontier among all permutations.

Sorting is not free, and minimizing the frontier does not formally minimize every detail of the word loop, but it often reduces the actual number of destination words updated. The timing includes copying and sorting the input:

| Workload, $W\approx10^6$ | Original time | Original destination words | Sorted time | Sorted destination words | Speedup |
|:--------------------------|--------------:|----------------------:|------------:|--------------------:|--------:|
| 2,000 dense costs | 2.116 ms | 15,796,861 | 1.432 ms | 10,500,431 | 1.48x |
| 2,000 wide costs | 2.090 ms | 15,736,484 | 2.022 ms | 15,094,606 | 1.03x |
| costs $2000,1999,\ldots,1$ | 3.550 ms | 26,915,850 | 2.252 ms | 16,505,250 | 1.58x |
| residue adversary | 2.533 ms | 27,411,551 | 2.517 ms | 27,411,551 | 1.01x |

This optimization is particularly clean when the complete set is required. For an optimum-only solver, reordering can destroy a lucky early exact fill in the caller's order. We will deal with that conflict later instead of pretending that sorting is unconditionally free.

## Looking at the Machine Code

At this point the inner loop is regular enough for the compiler to understand. Clang's vectorization report gives the 64-bit loop a vectorization width of 2 and an interleave count of 4 on the 128-bit SIMD hardware of the test machine. The generated loop contains an eight-word unrolled schedule of NEON loads, `ushl.2d` variable shifts, ORs, and stores. The byte baseline is also vectorized, with a width of 16, which is why packing bits gives an order-of-magnitude improvement rather than the nominal factor of 64.

Hand-written NEON was also tested. It was 2-5% slower than the compiler-generated loop on these workloads: the compiler had already found essentially the schedule we wanted. Intrinsics would have made the central idea harder to read without removing either source stream or the destination write, so the implementation stays as plain C++.

The profiler agrees with the compiler report. In a three-second Time Profiler run of the full-set solver on the dense million-state workload, 1665 of 1721 samples — **96.7%** — landed in `shift_or_bounded`. At this stage, optimizing preprocessing would have been pointless. We either had to execute fewer shifts or make each shift cheaper.

The packed state is also eight times smaller, which moves its cache boundaries. With 64 wide costs, the byte state crosses the 128 KiB L1 data-cache size around $W=2^{17}$, while the packed state reaches the same footprint around $W=2^{20}$. Both curves still grow roughly linearly because both kernels stream their live state:

![](../img/knapsack-size.svg)

## Combining Two Items

One tempting way to halve the number of passes is to process two items at once. In generating-function notation, adding costs $a$ and $b$ multiplies the reachable polynomial by

$$
(1+z^a)(1+z^b)=1+z^a+z^b+z^{a+b}.
$$

Therefore, from the old state $B$, one exact combined update is

$$
B\gets B\lor(B\ll a)\lor(B\ll b)\lor(B\ll(a+b)).
$$

It really does replace two shift calls with one and cut the counted destination-word writes roughly in half. It also turns one simple shifted source into three source streams, complicates the boundary checks, and prevents the compiler from using the same compact vector loop. The result is impressively bad:

| Workload | Separate updates | Destination words | Paired update | Destination words | Slowdown |
|:---------|-----------------:|-------------:|--------------:|-------------:|---------:|
| dense | 2.116 ms | 15,796,861 | 14.678 ms | 7,904,962 | 6.9x |
| wide costs | 2.090 ms | 15,736,484 | 13.992 ms | 10,538,245 | 6.7x |
| descending unique | 3.550 ms | 26,915,850 | 24.861 ms | 13,461,844 | 7.0x |
| residue adversary | 2.533 ms | 27,411,551 | 16.217 ms | 13,710,172 | 6.4x |

Counting loop iterations is useful only while the iterations have comparable cost. Here they do not. The paired kernel is retained in the benchmark as a failed experiment and rejected from the solver.

## Skipping Saturated Words

The profiler tells us where the time goes, but not why all those destinations still need to be updated. On dense instances, they mostly do not.

Once a 64-bit destination word becomes all ones, no later OR can change it. We could put an `if (bits[d] != ~0)` inside the loop, but that would still inspect every word, add a branch to a well-vectorized loop, and save only the shifts. We need a way to enumerate the unfinished words without scanning the finished ones.

We maintain two small bitmaps:

- `incomplete` has one bit per data word, set while that word is not all ones;
- `summary` has one bit per 64-bit word of `incomplete`, set while that group contains anything unfinished.

One summary bit therefore represents 64 data words, or 4096 subset-sum states. An indexed update enumerates set bits from high to low with `clz`, applies the ordinary shifted-word formula only there, and clears a marker permanently when its data word becomes full. The high-to-low order is still essential for 0/1 correctness.

Ignoring the range masks at the two ends, its central loop looks like this:

```c++
for (int si = last_summary; si >= first_summary; si--) {
    uint64_t groups = summary[si];

    while (groups) {
        int mb = 63 - __builtin_clzll(groups);
        groups ^= uint64_t(1) << mb;
        int m = 64 * si + mb;
        uint64_t todo = incomplete[m];

        while (todo) {
            int b = 63 - __builtin_clzll(todo);
            todo ^= uint64_t(1) << b;
            int d = 64 * m + b;

            bits[d] |= shifted_word(bits, d, weight);
            if (bits[d] == ~uint64_t(0))
                incomplete[m] &= ~(uint64_t(1) << b);
        }

        if (incomplete[m] == 0)
            summary[si] &= ~(uint64_t(1) << mb);
    }
}
```

`shifted_word` performs the same two-source shift derived earlier. Once a destination is removed, it never needs to return: all future transitions are ORs, and an all-one word is a fixed point of OR regardless of what happens to its sources.

The indexed loop is scalar and much more expensive per visited word than the dense SIMD loop, so switching early would be disastrous. The adaptive kernel starts dense and periodically samples at most 64 live words. It considers building the exact index only when:

- at least 95% of the sample is full;
- a full scan confirms that at most one live word in 16 is incomplete;
- the estimated remaining dense work is at least twice the size of the state.

After switching, if one update visits more than one eighth of the live words, the frontier has exposed too much fresh sparse state. The kernel immediately falls back to the dense vector loop and disables the index for the rest of the run. All these tests inspect the actual state; none assumes a distribution of weights.

The decision itself is intentionally less clever than the data structure:

```c++
if (sampled_full_fraction(bits, hi) >= 0.95) {
    int incomplete = build_index(bits, hi, index);
    if (16 * incomplete <= live_words)
        indexed = true;
}

if (indexed && visited_words * 8 > live_words) {
    indexed = false;
    indexing_disabled = true;
}
```

Sampling can only cause us to build or reject an index. It cannot affect which sums are set, so a bad prediction costs time but not correctness.

Here are the results for constructing the complete reachable set. “Switch” is the item after which the index was activated. The diagnostic destination-word count includes destinations actually processed, but not sampling or index construction, so time remains the final judge:

| Workload | Dense loop | Adaptive | Destination words | Switch | Sorted + adaptive | Destination words | Switch |
|:---------|-----------:|---------:|-------------:|-------:|------------------:|-------------:|-------:|
| dense | 2.116 ms | 0.0625 ms | 27,172 | 48 | 0.0762 ms | 20,408 | 137 |
| wide costs | 2.090 ms | 0.762 ms | 5,558,345 | 704 | 0.0757 ms | 340,953 | 64 |
| descending unique | 3.550 ms | 0.218 ms | 622,648 | 192 | 0.0647 ms | 18,907 | 91 |
| residue adversary | 2.533 ms | 2.476 ms | 27,411,551 | never | 2.478 ms | 27,411,551 | never |

![](../img/knapsack-kernel.svg)

The dense workload needs only 27 thousand destination-word visits instead of 15.8 million. For wide and reverse-ordered costs, sorting first creates a much denser reachable prefix early enough for the index to help. On the adversarial input the words never become full, the exact density test rejects the index, and the original SIMD loop keeps running at essentially its original speed.

This is a general pattern worth remembering: a monotone state often contains regions that have reached a fixed point. If such regions can be removed from a compact work list, later iterations need not rediscover that they are finished.

## Solving Only the Requested Problem

So far every variant has constructed the complete set of reachable sums, even when the caller asks for only one number. This was necessary for fair kernel comparisons, but it is not necessary for solving the stated problem.

Several exact exits cost almost nothing:

1. Ignore zero costs and costs larger than $W$. They cannot improve a positive optimum under the capacity.
2. If the total $T$ of all usable costs is at most $W$, take every usable item and return $T$.
3. During the dynamic program, if bit $W$ becomes set, return immediately. No legal answer can be larger.
4. Let $g$ be the greatest common divisor of all usable costs. Divide every cost by $g$ and replace $W$ by $\lfloor W/g\rfloor$; multiply the answer by $g$ afterwards.

The GCD reduction is exact because every reachable sum is a multiple of $g$. On the `gcd-64` workload, the capacity falls from 1,000,003 to 15,625. Direct early stopping without scaling takes 2.692 ms and visits 30,011,078 destination words; adding the reduction takes 0.00394 ms and visits 4,302 — about 684 times faster.

The total shortcut is even more dramatic on the five-million-capacity sparse-frontier workload: it replaces 967,150 destination-word visits with a linear pass over 500 integers. These are solver optimizations, not evidence that the underlying bitset shift became faster.

### The Complement Window

There is a stronger reduction when the total is only moderately larger than the capacity. After GCD scaling, let $C=\lfloor W/g\rfloor$, let $T>C$ be the sum of the scaled usable items, and define the excess

$$
L=T-C.
$$

Let $\mathcal R$ be the set of reachable sums. For every selected subset of sum $x$, its complement has sum $y=T-x$, which is also reachable. Consequently,

$$
\max\{x\in\mathcal R:x\le C\}
=T-\min\{y\in\mathcal R:y\ge L\}.
$$

This is not a heuristic symmetry. Complementing a subset is a bijection, so maximizing a feasible selected sum is exactly the same problem as finding the smallest reachable excluded sum at least $L$.

At first this seems to require a bitset up to $T$, which would not help. Let $M$ be the largest usable scaled item, and let $y^*$ be the minimum reachable sum with $y^*\ge L$. In a subset witnessing $y^*$, removing any chosen item $a$ must make the sum smaller than $L$; otherwise we would have found a smaller reachable sum still at least $L$. Therefore

$$
y^*-a<L
\quad\Longrightarrow\quad
y^*<L+a\le L+M.
$$

Since sums are integral, $y^*\le L+M-1$. It is sufficient to construct reachability only through

$$
U=L+M-1.
$$

We choose the complement orientation only when $U<C$ and otherwise retain the direct capacity. If bit $L$ appears, the scaled capacity $C$ — and hence the largest feasible multiple $gC$ in original units — is attainable, and we can stop; otherwise the first reachable bit above $L$ gives the optimum.

In code, the reduction surrounding the same bitset kernel is short:

```c++
int lower = total - capacity;
int complement_capacity = lower + maximum - 1;

if (complement_capacity < capacity) {
    subset_sum(cost, complement_capacity, bits);
    int excluded = first_sum_at_least(bits, lower);
    return total - excluded;
} else {
    subset_sum(cost, capacity, bits);
    return last_reachable(bits, capacity);
}
```

Here all quantities have already been divided by the GCD; the implementation multiplies the returned value back afterwards. It also stops either branch as soon as its exact target appears.

The useful window is smallest just above $T=W$. We generated 512 arbitrary positive costs with controlled totals and swept $T/W$, using the same fixed-seed proportions at every point so that the total is the main quantity changing. Below one, the total shortcut does no DP. At $1.05W$, complementing shrinks the effective capacity from one million to 54,216 and the time from 0.519 ms to 0.00157 ms. Near $2W$, the complement window approaches the direct capacity, and above the crossover the solver automatically stays direct:

![](../img/knapsack-symmetry.svg)

The bound depends only on $T$, $C$, and the largest item. It remains valid for duplicates, inconvenient residues, and adversarial input order.

### Putting the Solver Together

The remaining optimizations have conflicting preferences. Sorting makes the frontier small, but may postpone an exact fill that the caller's order would have found immediately. Grouping duplicates reduces the number of items, but costs another sort. The indexed kernel is spectacular on saturated states and useless on sparse ones.

The final solver uses a small portfolio, with exact checks at every branch:

1. In one pass, validate and filter the input, compute $T$, $M$, and $g$, and take the total shortcut when possible.
2. Scale by $g$ and select the smaller of the direct and proven complement windows.
3. Give the first $p=\min(64,n)$ items, in their original order, a chance to reach the exact target.
4. If $n\le4p$, continue the same bitset through the remaining items instead of restarting a short instance.
5. Otherwise, sort the weights and replace each run of $c$ equal copies of weight $x$ by groups of $1,2,4,\ldots$ copies. Every multiplicity from 0 through $c$ is still representable, while a long duplicate run becomes $O(\log c)$ shifts.
6. Restart with the bundled sequence and run the adaptive dense/indexed kernel, stopping at an exact target and falling back to the dense loop when saturation never appears.

The 64-item probe is deliberately small and fixed. It preserves cheap lucky exits without allowing an arbitrary input order to control the expensive remainder of the computation. Here $n$ counts the usable items left after filtering. When a restart does happen, $n>4p$, so recomputing the probed prefix duplicates fewer than 25% of the input shift calls. For 65 through 256 usable items, the solver simply finishes the probe state. Binary grouping is also exact: it changes the items used by the internal dynamic program, not the set of sums they represent.

Separating the last three stages shows which one is responsible for each result. All times include their preprocessing and allocation:

| Workload | Complement/GCD | + bundle | + original-order probe | + adaptive index |
|:---------|---------------:|---------:|-----------------------:|-----------------:|
| dense | 0.00187 ms | 0.0256 ms | 0.00190 ms | 0.00188 ms |
| wide costs | 0.0513 ms | 0.0640 ms | 0.0512 ms | 0.0514 ms |
| descending unique | 0.678 ms | 1.014 ms | 1.024 ms | 0.0783 ms |
| common divisor 64 | 0.00394 ms | 0.0282 ms | 0.00396 ms | 0.00396 ms |
| 2,000 copies of 500 | 0.00389 ms | 0.00533 ms | 0.00552 ms | 0.00559 ms |
| residue adversary | 2.484 ms | 2.492 ms | 2.493 ms | 2.496 ms |
| 65-item residue case | 0.0464 ms | 0.0471 ms | 0.0464 ms | 0.0463 ms |

The dense, wide-cost, and GCD workloads all find an exact target during the original-order probe, after 22, 43, and 34 shift calls respectively. They never invoke the saturation index. Conversely, the descending input survives the probe; here the index is the optimization that cuts the time from 1.024 ms to 0.0783 ms. Bundling reduces 2,000 equal items to 8 shifts, but its sorting and grouping overhead makes the already tiny duplicate case slightly slower than the unbundled complement stage. A portfolio need not win every internal race to remain robust end-to-end.

The table compares the complete-set solver, including its allocation and preprocessing, against the final optimum-only solver with the same timing boundary:

| Workload | Full-set time | Full destination visits | Final time | Final work (shifts / destination visits) | Main reason | Speedup |
|:---------|--------------:|-----------------------:|-----------:|:-----------|:------------|--------:|
| dense, $W=10^6$ | 2.112 ms | 15,796,861 | 0.00188 ms | 22 shifts / 2,054 words | complement + probe | 1123x |
| wide costs, $W=10^6$ | 2.104 ms | 15,736,484 | 0.0514 ms | 43 shifts / 359,588 words | original-order probe | 41.0x |
| sparse frontier, $W=5\cdot10^6$ | 0.1525 ms | 967,150 | 0.000305 ms | 0 shifts / 0 words | total shortcut | 500x |
| descending unique, $W=10^6$ | 3.546 ms | 26,915,850 | 0.0783 ms | 1,478 shifts / 81,321 words | sorting + saturation | 45.3x |
| common divisor 64 | 2.691 ms | 30,011,078 | 0.00396 ms | 34 shifts / 4,302 words | GCD + probe | 679x |
| 2,000 copies of 500 | 2.087 ms | 15,462,837 | 0.00559 ms | 72 shifts / 74 words | GCD + complement + grouping | 373x |
| residue adversary | 2.479 ms | 27,411,551 | 2.496 ms | 2,064 shifts / 27,453,279 words | dense fallback | 0.99x |
| 65-item residue case | 0.0465 ms | 478,361 | 0.0463 ms | 65 shifts / 478,361 words | continue probe state | 1.00x |

![](../img/knapsack-solver.svg)

The large speedups are possible because the full-set baseline performs work the question did not request. The adversarial rows are just as important. The large residue instance consists of one item of cost 1 and the costs $64,128,\ldots,64\cdot1999$, with $W=1,000,002$. Every reachable sum is congruent to 0 or 1 modulo 64, so the target, which is 2 modulo 64, is unreachable and no 64-bit word saturates. The solver tries its 64-item probe, rejects the index, performs the ordinary dense DP, and returns $W-1$. It is 0.7% slower than the full-set solver: the measured price of discovering that none of the fast paths applies.

The smaller adversary has only 65 items: one cost of 1 followed by 64 large multiples of 64. It crosses the probe boundary by exactly one item and never finds an exact fill. Restarting would duplicate almost all its work, so the $n\le4p$ rule continues the existing state for the last shift. The final solver performs exactly the same 65 shifts and visits the same 478,361 destination words as the full-set baseline, taking 0.0463 ms instead of 0.0465 ms.

The final version was profiled again before the last preprocessing cleanup. Only about 8% of samples remained in the shift loop; validation and repeated GCD work had become the bottleneck. In that isolated profile benchmark, fusing validation, filtering, the total, the maximum, and the GCD into one pass — and stopping GCD calls permanently once it becomes 1 — reduced the dense solver from about 3.8 microseconds to about 1.7 microseconds. This is why profiling must be repeated after a large optimization: the old hot loop may no longer matter.

## Finding the Answer

If the direct dynamic program completes without reaching $C$, scan its words from high to low. Once a nonzero word is found, `clz` locates its highest set bit:

```c++
int best_sum(const uint64_t *bits, int W) {
    for (int i = W / 64; i >= 0; i--)
        if (bits[i])
            return 64 * i + 63 - __builtin_clzll(bits[i]);
    return 0;
}
```

The production version masks the unused high bits in the last word. It checks for zero before calling `clz`, because `__builtin_clzll(0)` is undefined.

For the complement orientation, scan upward from $L$ instead and use `ctz` on the first nonzero masked word. If the first reachable excluded sum is $y$, the selected answer is $g(T-y)$. Both scans cost $O(U/64)$ once; an exact-target exit avoids them entirely.

## Recovering the Subset

The final bitset does not contain predecessor pointers, but retaining every intermediate bitset would be a particularly expensive way to fix that. We can recover a witness with two reusable bitsets and recomputation, in the same spirit as Hirschberg's linear-space reconstruction for longest common subsequence.

Suppose target $t$ is known to be reachable using items in $[l,r)$. Split the interval at $m$, construct a bitset $L$ for the left half and $R$ for the right half, both capped at $t$. Some split $x$ must satisfy

$$
L[x]\land R[t-x]=1.
$$

Then recursively recover $x$ on the left and $t-x$ on the right. A one-item interval is immediate:

```c++
bool recover(int l, int r, int t) {
    if (t == 0)
        return true;
    if (r - l == 1) {
        if (cost[l] != t)
            return false;
        answer.push_back(l);
        return true;
    }

    int m = (l + r) / 2;
    build_bitset(cost + l, m - l, t, left);
    build_bitset(cost + m, r - m, t, right);
    int x = compatible_split(left, right, t);

    return x >= 0
        && recover(l, m, x)
        && recover(m, r, t - x);
}
```

Searching for $x$ one bit at a time would throw away our word parallelism. For 64 consecutive positions of `left`, load the corresponding reversed window of `right` and intersect them:

```c++
for (int k = 0; k < words; k++) {
    int high = t - 64 * k;
    uint64_t reflected = reverse_bits(window(right, high - 63));
    uint64_t candidates = left[k] & reflected;
    if (candidates)
        return 64 * k + __builtin_ctzll(candidates);
}
```

Bit $j$ of `reflected` is $R[t-(64k+j)]$, so the intersection tests 64 compatible splits at once. The two parent bitsets are dead as soon as a split is found. Both recursive children deliberately overwrite the same two buffers; there are no stored forward layers, checkpoints, or predecessor arrays.

At a recursion node containing $k$ items and target $t$, rebuilding both halves takes at most $k\lceil(t+1)/64\rceil$ word updates. At each depth, the child targets sum to the parent target while item intervals halve. The non-rounded work therefore forms a geometric series, and rounding nonempty bitsets to whole words contributes at most $O(n\log n)$. The total is

$$
O\left(\frac{n(t+1)}{64}+n\log n\right)
$$

word operations, with two $\lceil(t+1)/64\rceil$-word buffers and an $O(\log n)$ recursion stack. The price of not retaining history is recomputation, not another factor of $n$ in memory.

Recovery is also integrated with the optimum-only solver. If the solver used the direct orientation, we reconstruct the selected sum. If it used the complement orientation, we reconstruct the usually smaller *excluded* sum and return all other usable item indices. GCD-scaled weights are used internally, but the reported indices always refer to the original items.

The timings below include both solving and reconstruction for the second column:

| Workload | Answer only | Answer and subset | Orientation | Reconstructed target |
|:---------|------------:|------------------:|:------------|---------------------:|
| dense, $W=100,000$ | 0.0216 ms | 0.325 ms | direct | 100,000 |
| dense, $W=1,000,000$ | 0.00153 ms | 0.109 ms | complement | 10,905 |
| residue adversary | 2.502 ms | 7.557 ms | direct | 1,000,001 |

The complement-aware case is the useful one: the forward DP has an effective capacity of 11,904, but the excluded witness itself sums to only 10,905, so reconstruction is capped there rather than at one million. The residue adversary, as intended, forces the general path and exposes the real recomputation cost.

## Limits

The final implementation is still pseudo-polynomial. The capacity occupies only $O(\log W)$ bits in the input, while the worst-case running time and memory depend on $W$. Saturation, complementing, GCD scaling, and exact exits can all fail simultaneously, as the residue workload demonstrates.

When $n$ is small and $W$ is enormous, meet-in-the-middle is usually a better axis of attack. When only a tiny number of sums is reachable, maintaining a sparse sorted set may beat scanning a bitset. General weight-value knapsack needs numeric states rather than reachability bits. There are also theoretically faster subset-sum algorithms in specialized regimes, but they solve a different engineering problem from the compact single-core implementation developed here.

The point of the case study is not that bitsets magically make knapsack fast. It is that the representation exposes structure — a frontier, fixed-point words, exact targets, and a complementary search window — and each of those gives us a new opportunity to stop doing work.
