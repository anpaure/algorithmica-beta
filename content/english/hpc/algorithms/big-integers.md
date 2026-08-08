---
title: Big Integers and Karatsuba Multiplication
weight: 5
---

In 1960, Andrey Kolmogorov organized a seminar where he conjectured that multiplying two $n$-digit numbers necessarily takes $\Omega(n^2)$ time. The usual long multiplication had survived for a few thousand years, so this was not an unreasonable guess.

One week later, a 23-year-old student named Anatoly Karatsuba found a faster algorithm.

Apart from being a nice story, Karatsuba multiplication is a useful performance case study. The asymptotically faster method is not faster for small numbers, and most of the work needed to make it practical is not in the algebra but in representation, memory allocation, and choosing the right base case.

## Representing Large Integers

We can store a nonnegative integer as an array of digits in some base $B$:

$$
x=x_0+x_1B+x_2B^2+\cdots+x_{n-1}B^{n-1}.
$$

The least significant digit goes first because carries then move forward through memory. A decimal-friendly choice is $B=10^4$: every array element contains four decimal digits, and printing only requires padding all but the last group with zeros.

The usual multiplication algorithm is just a convolution of the digit arrays:

```c++
const int base = 10000;

vector<long long> schoolbook(const vector<long long> &a,
                             const vector<long long> &b) {
    vector<long long> c(a.size() + b.size());

    for (int i = 0; i < a.size(); i++)
        for (int j = 0; j < b.size(); j++)
            c[i + j] += a[i] * b[j];

    return c;
}
```

We postpone carrying until after the multiplication. This keeps the hot loop to one multiply and one add, and lets the compiler [unroll and vectorize](/hpc/simd/auto-vectorization) parts of it. The accumulator type must be wide enough: for $n$ base-$B$ digits, a coefficient can be as large as roughly $n(B-1)^2$.

The algorithm performs $nm$ digit products for operands of lengths $n$ and $m$. It is quadratic for equally sized inputs, but it has excellent locality and very little overhead. We will keep it — just not for every input size.

## Karatsuba's Trick

Assume for now that both arrays have the same even length $n=2k$. Split the corresponding numbers in half:

$$
x=x_0+B^kx_1,\qquad y=y_0+B^ky_1.
$$

Expanding the product in the obvious way needs four half-size multiplications:

$$
xy=x_0y_0+B^k(x_0y_1+x_1y_0)+B^{2k}x_1y_1.
$$

The middle term is the annoying one. Karatsuba observed that it can be recovered from one additional product:

$$
(x_0+x_1)(y_0+y_1)-x_0y_0-x_1y_1=x_0y_1+x_1y_0.
$$

So we only need three recursive multiplications instead of four. If additions take linear time, the recurrence becomes

$$
T(n)=3T(n/2)+O(n),
$$

which solves to

$$
T(n)=O(n^{\log_2 3})\approx O(n^{1.585}).
$$

Here is a direct implementation for polynomial coefficients. The input length must be a power of two; we will fix that in the wrapper.

```c++
const int cutoff = 32;

vector<long long> karatsuba(const vector<long long> &a,
                            const vector<long long> &b) {
    int n = a.size();

    if (n <= cutoff)
        return schoolbook(a, b);

    int k = n / 2;
    vector<long long> a0(a.begin(), a.begin() + k);
    vector<long long> a1(a.begin() + k, a.end());
    vector<long long> b0(b.begin(), b.begin() + k);
    vector<long long> b1(b.begin() + k, b.end());
    vector<long long> as(k), bs(k);

    for (int i = 0; i < k; i++) {
        as[i] = a0[i] + a1[i];
        bs[i] = b0[i] + b1[i];
    }

    vector<long long> low  = karatsuba(a0, b0);
    vector<long long> high = karatsuba(a1, b1);
    vector<long long> mid  = karatsuba(as, bs);

    vector<long long> c(2 * n);
    for (int i = 0; i < n; i++) {
        c[i]     += low[i];
        c[k + i] += mid[i] - low[i] - high[i];
        c[n + i] += high[i];
    }

    return c;
}
```

This is almost a literal translation of the formula. It also makes the main practical problem painfully visible: every recursive call slices four arrays and allocates several more.

## Turning Coefficients Back Into Digits

Before calling `karatsuba`, we pad both operands with zeroes to the same power-of-two length. Afterwards, we propagate the carries once:

```c++
vector<int> multiply(vector<int> a, vector<int> b) {
    if (a.empty() || b.empty())
        return {};

    int n = 1;
    while (n < max(a.size(), b.size()))
        n *= 2;

    vector<long long> x(n), y(n);
    copy(a.begin(), a.end(), x.begin());
    copy(b.begin(), b.end(), y.begin());

    vector<long long> c = karatsuba(x, y);

    for (int i = 0; i + 1 < c.size(); i++) {
        c[i + 1] += c[i] / base;
        c[i] %= base;
    }

    while (c.size() > 1 && c.back() == 0)
        c.pop_back();

    return vector<int>(c.begin(), c.end());
}
```

This code assumes that every input digit is in $[0,B)$ and that all intermediate polynomial coefficients fit in `long long`. These are representation constraints, not properties of Karatsuba itself. A real big-integer implementation selects the limb width together with a sufficiently wide accumulator — often 32-bit binary limbs with 64- or 128-bit products.

Padding to a power of two can almost double the input length. This is acceptable for a compact implementation, but large libraries use uneven splits and several multiplication algorithms so they do not spend most of their time multiplying zeroes.

## The Crossover

Recursing down to one digit is a classic asymptotic mistake. Karatsuba saves multiplications, but it adds array passes, function calls, temporary storage, and subtractions. For small inputs, the quadratic loop wins by a lot.

This is why the code switches to schoolbook multiplication at `cutoff`. The value 32 is only a starting point. The best threshold depends on the digit base, the schoolbook kernel, allocation strategy, compiler, and processor, and should be found by benchmarking a sweep of operand sizes rather than one conveniently large example.

Strongly unbalanced operands are another bad case. Splitting a very long number against a short one creates mostly empty recursive halves. It is usually better to use schoolbook multiplication or split the longer operand into blocks when the lengths are far apart.

## Removing the Allocations

The simple recursive version is useful for checking the algebra, but it should not be the final benchmark. All temporary arrays needed by one recursion branch can share a workspace, because the other branches are evaluated sequentially. A fast interface looks more like this:

```c++
void karatsuba(const long long *a, const long long *b,
               long long *c, long long *scratch, int n);
```

The wrapper allocates `c` and `scratch` once. Each call divides both buffers among its three products and the two arrays of sums. This removes allocator traffic and also avoids copying the low and high halves: they are already contiguous subarrays of `a` and `b`.

The remaining linear loops — forming `a0 + a1`, forming `b0 + b1`, and combining the three products — are independent elementwise operations and are good candidates for SIMD. The schoolbook loop is harder: each product contributes to a shifted output range, and carry propagation is a dependency chain. Optimizing this base case often moves the Karatsuba crossover *up*, because the allegedly slow algorithm has just become faster.

Other useful special cases include squaring, where the cross-products are symmetric, and multiplication by one limb. At still larger sizes, Toom–Cook and FFT-based methods continue the same hierarchy. There is no single “big integer multiplication algorithm”; there is a dispatcher between kernels that win in different size ranges.

## Testing and Benchmarking

Compare the result against an established arbitrary-precision library, not only against the same code with a different cutoff. Important inputs include zero, one, digits equal to `base - 1`, sizes just below and above powers of two, unequal lengths, and sizes around the crossover.

For benchmarks, separate decimal parsing and printing from multiplication. Report both operand lengths, their balance, the digit base, cutoff, and whether temporary memory is reused. The useful plot shows schoolbook and Karatsuba over a wide range of sizes: the crossing point is the result, not a magic constant to be copied into every program.
