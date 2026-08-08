---
title: Big Integers and Karatsuba Multiplication
weight: 5
---

In 1960, Andrey Kolmogorov organized a seminar where he conjectured that multiplying two $n$-digit numbers necessarily takes $\Omega(n^2)$ time. The usual long multiplication had survived for a few thousand years, so this was not an unreasonable guess.

One week later, a 23-year-old student named Anatoly Karatsuba found a faster algorithm.

The algebra fits on a napkin. Making it fast takes rather more work: a direct translation allocates thousands of temporary arrays, recursion to single digits loses badly, and the machine-dependent crossover matters more than the asymptotic bound at ordinary sizes.

## The Experiment

We need a narrow contract before measuring anything. An operand in this case study is a nonnegative, little-endian polynomial of exactly $n$ base-$10^4$ digits, where $n$ is a power of two and both operands have the same length. Benchmark digits are uniform in $[0,9999]$, with a nonzero most-significant digit. The timed kernel produces the $2n$ *uncarried convolution coefficients* in signed 64-bit integers. Decimal conversion, padding, and the final carry pass are deliberately excluded.

The largest benchmark has $n=8192$ and uses a base-case cutoff of 32. At the deepest middle-product node, a coefficient is bounded by

$$
\frac{n^2}{32}(B-1)^2 < 2.1\cdot 10^{14},
$$

so this workload fits safely in `int64_t`. This is a property of the stated range, not a general promise for arbitrary limb counts.

All measurements were made on one performance core of an Apple M4 Max with Apple Clang 17 and `-O3 -mcpu=native`. Inputs, output, and the reusable scratch array are allocated before timing. Output zeroing is included. The intentionally allocating implementation includes all of its recursive allocations. Headline values are medians of seven runs after two warmups, except at $n=8192$, where five runs are used. For sub-microsecond cases, each timed sample performs

$$
\max(1,\lfloor 2^{22}/n^2\rfloor)
$$

products and reports the time per product. The same non-inlined base-case kernel is shared by all variants, preventing the compiler from silently specializing one version differently. The [complete test and benchmark program](../../../code/big_integers.cpp) contains the fixed seeds and the CSV mode. Its [raw output](../../../code/big_integers_m4_results.txt) is rendered by the [Matplotlib plot script](../../../code/plot_big_integers.py).

## Long Multiplication

Writing

$$
x=x_0+x_1B+x_2B^2+\cdots+x_{n-1}B^{n-1}
$$

turns multiplication into a convolution. We postpone carrying so the hot loop contains only multiply-adds:

```c++
void schoolbook(const long long *a, const long long *b,
                long long *c, int n) {
    fill(c, c + 2 * n, 0);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            c[i + j] += a[i] * b[j];
}
```

This performs exactly $n^2$ digit products. It is quadratic, but contiguous, compact, and almost free of setup. At 64 digits it takes 0.921 microseconds. Any recursive method has to beat this kernel after paying for its additions and calls.

## Three Multiplications Instead of Four

Split equally sized even-length operands in half:

$$
x=x_0+B^kx_1,\qquad y=y_0+B^ky_1.
$$

The direct expansion needs four half-size products. Karatsuba recovers both cross-products from one:

$$
(x_0+x_1)(y_0+y_1)-x_0y_0-x_1y_1=x_0y_1+x_1y_0.
$$

Therefore

$$
xy=x_0y_0+B^k\left((x_0+x_1)(y_0+y_1)-x_0y_0-x_1y_1\right)
   +B^{2k}x_1y_1,
$$

and the recurrence

$$
T(n)=3T(n/2)+O(n)
$$

gives $T(n)=O(n^{\log_2 3})\approx O(n^{1.585})$.

A literal implementation slices `a0`, `a1`, `b0`, and `b1`, builds two sums, recursively returns three products, and allocates a result at every node. It is pleasingly close to the formula and an excellent correctness reference. It is not an excellent memory allocator benchmark.

At 64 digits, this version takes 1.084 microseconds and is 18% *slower* than schoolbook. It only reaches parity around 128 digits. The asymptotic saving exists, but the first implementation manages to hide it under object construction and copying.

The allocation counter makes the bottleneck concrete. Multiplying two 4096-digit operands with cutoff 32 asks for 9,838 vectors containing 798,848 elements in total. That is 6.10 MiB of cumulative `int64_t` storage requests for a result containing only 8192 coefficients. The allocator can recycle memory, so this is not the peak footprint; it is the amount of allocation and initialization work requested by the source.

## One Workspace

The three recursive branches run sequentially, so they need not own their temporary arrays simultaneously. We write the low and high products directly into their final ranges and reserve part of one scratch array for the two sums and middle product:

```c++
void karatsuba(const long long *a, const long long *b,
               long long *c, long long *scratch,
               int n, int cutoff) {
    if (n <= cutoff) {
        schoolbook(a, b, c, n);
        return;
    }

    int k = n / 2;
    karatsuba(a,     b,     c,     scratch, k, cutoff);
    karatsuba(a + k, b + k, c + n, scratch, k, cutoff);

    long long *as = scratch;
    long long *bs = as + k;
    long long *middle = bs + k;
    long long *next = middle + n;

    for (int i = 0; i < k; i++) {
        as[i] = a[i] + a[k + i];
        bs[i] = b[i] + b[k + i];
    }
    karatsuba(as, bs, middle, next, k, cutoff);

    for (int i = 0; i < n; i++)
        middle[i] -= c[i] + c[n + i];
    for (int i = 0; i < n; i++)
        c[k + i] += middle[i];
}
```

At one level, `as`, `bs`, and `middle` consume $2n$ scratch elements. Their recursive call has length $n/2$ and, inductively, needs another $4(n/2)=2n$. Thus $4n$ elements suffice for the whole recursion. The low and high calls finish before this level uses its temporaries and may reuse the same region.

At 4096 digits and cutoff 32, the allocating version takes 0.985 ms while the workspace version takes 0.572 ms. Removing the recursive vectors is a 1.72-fold improvement. This change does not alter the recurrence, base case, or arithmetic; it removes allocation, copying, and repeated initialization alone.

## Finding the Base Case

Recursing down to one digit is the other classic asymptotic mistake. Each Karatsuba level saves multiplications but adds two sum passes, two combine passes, and three calls. We swept the cutoff on the same 4096-digit operands. The plot reports slowdown relative to the fastest measured cutoff; lower is better.

![Karatsuba base-case cutoff sweep](../img/big-integers-cutoff.svg)

A cutoff of one is 4.78 times slower than the best result. Moving the cutoff from 16 to 32 still helps slightly, while moving it to 64 loses 21%. Beyond that, too much work returns to the quadratic kernel. For this compiler, representation, and processor, 32 is the measured choice—not a constant inherited from the algorithm.

## The Crossover Curve

With the workspace and cutoff fixed, we sweep balanced operands from 16 to 8192 digits. Each line uses the same inputs and includes output initialization.

![Schoolbook and Karatsuba multiplication time](../img/big-integers-size.svg)

Some representative headline medians are:

| Digits per operand | schoolbook | allocating Karatsuba | workspace Karatsuba |
|---:|---:|---:|---:|
| 32 | **0.242 µs** | 0.269 µs | 0.242 µs |
| 128 | 3.638 µs | 3.609 µs | **2.271 µs** |
| 512 | 59.477 µs | 35.042 µs | **21.089 µs** |
| 2048 | 0.921 ms | 0.327 ms | **0.189 ms** |
| 8192 | 15.144 ms | 3.112 ms | **1.757 ms** |

At the largest size, the final kernel is 8.62 times faster than schoolbook and 1.77 times faster than the allocation-heavy translation. The gap grows with $n$, as the exponents predict, but the constant factors decide when that growth becomes useful.

## Carrying and Representation Limits

For nonnegative coefficients, carrying is a final linear pass:

```c++
for (int i = 0; i + 1 < c.size(); i++) {
    c[i + 1] += c[i] / base;
    c[i] %= base;
}
```

Real big integers need more policy than this benchmark. Unequal lengths must be padded or split without wasting almost half the work; signs need normalization; binary limbs are usually more convenient than decimal ones; and the limb width, accumulator width, and maximum recursion depth must be selected together to avoid overflow. Padding both operands to one power of two is particularly bad for strongly unbalanced multiplication. Carrying also becomes subtler when an implementation allows negative intermediate coefficients.

The checked-in harness differentially tests every power-of-two size through 512 digits, including zero, all-9999 inputs, and 40 fixed-seed random pairs per size. It compares both Karatsuba implementations and seven workspace cutoffs against schoolbook, then checks that carrying leaves every digit in $[0,B)$. The same suite passes with AddressSanitizer and UndefinedBehaviorSanitizer.

At larger sizes, Toom–Cook and FFT-based multiplication continue the hierarchy. The important lesson arrives earlier: Karatsuba's three-product identity is only the beginning. The practical algorithm is the identity plus a shared workspace, a measured base case, and an explicit representation contract.
