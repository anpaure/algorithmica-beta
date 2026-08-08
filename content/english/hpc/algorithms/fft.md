---
title: Fast Fourier Transform
weight: 6
---

The fast Fourier transform is one of the most important algorithms of the twentieth century. It is used in signal processing, physics, image compression, and many other places where mentioning all the applications would take longer than explaining the algorithm.

We will approach it from a slightly unusual direction: as a fast way to multiply polynomials.

## Multiplication by Evaluation

A polynomial of degree less than $n$ is uniquely determined by its values at any $n$ distinct points. If two polynomials are already represented by their values at the same points, multiplying them is easy:

```c++
for (int i = 0; i < n; i++)
    C[i] = A[i] * B[i];
```

Afterwards, interpolation recovers the coefficients of the product. The whole plan is therefore:

1. evaluate both polynomials at sufficiently many points;
2. multiply the values pairwise;
3. interpolate the result.

For arbitrary points, evaluation and interpolation are too expensive. The trick is to choose points with enough symmetry that both operations can be performed in $O(n\log n)$ time.

## Roots of Unity

The $n$ complex roots of unity are

$$
1,\ \omega_n,\ \omega_n^2,\ldots,\omega_n^{n-1},
\qquad
\omega_n=e^{-2\pi i/n}.
$$

The *discrete Fourier transform* evaluates the coefficient sequence $a_0,\ldots,a_{n-1}$ at these points:

$$
A_k=\sum_{j=0}^{n-1}a_j\omega_n^{jk}.
$$

The inverse transform has almost exactly the same form:

$$
a_j=\frac1n\sum_{k=0}^{n-1}A_k\omega_n^{-jk}.
$$

So one routine can perform both operations: reverse the sign of the angle for the inverse transform and divide every result by $n$ at the end.

Computing the definition directly takes $n^2$ operations. The useful property of our evaluation points is

$$
\omega_n^2=\omega_{n/2}.
$$

Split the coefficients into those with even and odd indices. If $E_k$ and $O_k$ are the transforms of these two halves, then

$$
\begin{aligned}
A_k       &=E_k+\omega_n^kO_k,\\
A_{k+n/2}&=E_k-\omega_n^kO_k.
\end{aligned}
$$

The pair of additions around one multiplication is called a *butterfly*. We recursively perform two transforms of size $n/2$ and then $n/2$ butterflies, giving

$$
T(n)=2T(n/2)+O(n)=O(n\log n).
$$

## A Direct Recursive Version

The derivation translates almost word for word into code:

```c++
using ftype = complex<double>;

void fft_recursive(vector<ftype> &a, ftype root) {
    int n = a.size();
    if (n == 1)
        return;

    vector<ftype> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; i++) {
        even[i] = a[2 * i];
        odd[i] = a[2 * i + 1];
    }

    fft_recursive(even, root * root);
    fft_recursive(odd, root * root);

    ftype w = 1;
    for (int i = 0; i < n / 2; i++) {
        ftype t = w * odd[i];
        a[i] = even[i] + t;
        a[i + n / 2] = even[i] - t;
        w *= root;
    }
}
```

For a forward transform, the initial root is `polar(1.0, -2 * pi / n)`. For an inverse transform, we use the positive angle and divide the output by $n$.

This version is short and useful as a reference, but it allocates two vectors at every recursion node and copies the entire input on every level. The arithmetic is $O(n\log n)$, and so is the amount of copying, except the copying has a much less interesting constant.

## Removing Recursion and Allocations

Follow one input element through the recursive splitting. First its lowest index bit decides whether it goes into the even or odd half, then the next bit does the same, and so on. At the bottom, the element originally at index `i` ends up at the index obtained by reversing the bits of `i`.

We can perform this *bit-reversal permutation* once and then apply the butterflies bottom-up:

```c++
const double pi = acos(-1.0);

void fft(vector<ftype> &a, bool inverse = false) {
    int n = a.size(); // nonzero power of two

    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;

        if (i < j)
            swap(a[i], a[j]);
    }

    for (int len = 2; len <= n; len *= 2) {
        double angle = 2 * pi / len * (inverse ? 1 : -1);
        ftype step = polar(1.0, angle);

        for (int first = 0; first < n; first += len) {
            ftype w = 1;
            for (int i = 0; i < len / 2; i++) {
                ftype u = a[first + i];
                ftype v = w * a[first + i + len / 2];
                a[first + i] = u + v;
                a[first + i + len / 2] = u - v;
                w *= step;
            }
        }
    }

    if (inverse)
        for (ftype &x : a)
            x /= n;
}
```

The routine assumes that the size is a nonzero power of two. This is the natural contract for a compact radix-2 implementation; checking it in a benchmark's inner path would not make the transform any more general.

The order of operations is now explicit. First we combine adjacent one-element transforms, then blocks of four, then eight, until the entire array becomes one transform. No allocation happens inside the routine, and each stage scans contiguous blocks.

## Polynomial Multiplication

If polynomials have $n$ and $m$ coefficients, their product has $n+m-1$. We pad both arrays with zeroes to a power of two at least that large, transform them, multiply pointwise, and apply the inverse transform:

```c++
vector<long long> multiply(const vector<int> &a,
                           const vector<int> &b) {
    if (a.empty() || b.empty())
        return {};

    int size = a.size() + b.size() - 1;
    int n = 1;
    while (n < size)
        n *= 2;

    vector<ftype> x(n), y(n);
    for (int i = 0; i < a.size(); i++) x[i] = a[i];
    for (int i = 0; i < b.size(); i++) y[i] = b[i];

    fft(x);
    fft(y);

    for (int i = 0; i < n; i++)
        x[i] *= y[i];

    fft(x, true);

    vector<long long> result(size);
    for (int i = 0; i < size; i++)
        result[i] = llround(x[i].real());

    return result;
}
```

Padding is not an optional implementation detail. Without it, the transform computes *cyclic* convolution and coefficients that run past the end wrap around to the beginning.

Rounding is valid only while floating-point error stays below one half. For small integer coefficients and moderate lengths, `double` usually leaves a comfortable margin, but “usually” is not an arithmetic guarantee. Splitting coefficients into smaller pieces reduces the error at the cost of more transforms. When an exact modular answer is sufficient, the [number-theoretic transform](../ntt/) removes roundoff entirely.

## Where the Time Goes

At each stage there are $n/2$ independent butterflies. The additions are cheap; complex multiplication, moving the data, and producing the twiddle factors are the main work.

The code computes one sine and cosine per stage and obtains the rest of the roots by repeatedly multiplying by `step`. Calling `sin` and `cos` inside every butterfly would be much slower. Precomputing all roots can be faster when many transforms of the same size are reused, but the table also consumes cache bandwidth. Large FFT libraries therefore create a *plan* for a particular size instead of committing to one strategy forever.

The butterfly has plenty of SIMD parallelism, although `complex<double>` stores real and imaginary parts interleaved. One can process several butterflies with the same layout, or keep the real and imaginary components in separate arrays. Higher radices combine several stages and save some twiddle multiplications, at the cost of a larger and more specialized kernel.

Bit reversal is only $O(n)$ and disappears from the asymptotic expression, but its accesses are irregular. It matters for small transforms and one-shot calls. Stockham variants replace the explicit permutation with regular out-of-place passes; whether that wins depends on the cost of the extra buffer.

## Numerical Error and Benchmarking

Every butterfly rounds its result, and the recurrence `w *= step` also drifts slightly away from the unit circle. Useful correctness checks are:

- transform followed by inverse transform;
- comparison with the $O(n^2)$ definition for small arrays;
- convolution compared with the quadratic algorithm;
- the size of the imaginary residue when a real answer is expected.

Use an error relative to the scale of the input rather than one absolute epsilon for every test. Compiler options such as `-ffast-math` may change reassociation, contraction, and exceptional-value behavior, so they are part of both the performance and numerical contract.

When benchmarking, distinguish a planned repeated transform from a one-shot convolution that includes allocation, padding, and root setup. Also state the direction, size, precision, in-place or out-of-place layout, and whether the input is real or complex. FFT performance is mostly the art of matching the same butterfly to different memory hierarchies; the formula is the easy part.
