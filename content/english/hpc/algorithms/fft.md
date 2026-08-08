---
title: Fast Fourier Transform
weight: 6
---

The fast Fourier transform is one of the most important algorithms of the twentieth century. The formula is elegant; the implementation is mostly an argument about permutations, temporary arrays, and how we obtain the same roots of unity millions of times.

We will derive it as a fast way to multiply polynomials, then optimize one precise kernel: an in-place forward transform of $n$ `complex<double>` values, where $n$ is a nonzero power of two.

## Evaluation Instead of Multiplication

A polynomial of degree less than $n$ is uniquely determined by its values at $n$ distinct points. In value representation, multiplication is pointwise:

```c++
for (int i = 0; i < n; i++)
    C[i] = A[i] * B[i];
```

The complex $n$-th roots of unity give evaluation points with the symmetry we need:

$$
\omega_n=e^{-2\pi i/n},\qquad
A_k=\sum_{j=0}^{n-1}a_j\omega_n^{jk}.
$$

The inverse changes the sign of the exponent and divides by $n$:

$$
a_j=\frac1n\sum_{k=0}^{n-1}A_k\omega_n^{-jk}.
$$

Because $\omega_n^2=\omega_{n/2}$, the even and odd coefficients form two half-size transforms. If they produce $E_k$ and $O_k$, then

$$
\begin{aligned}
A_k       &=E_k+\omega_n^kO_k,\\
A_{k+n/2}&=E_k-\omega_n^kO_k.
\end{aligned}
$$

This pair is a *butterfly*. Two recursive transforms and $n/2$ butterflies give

$$
T(n)=2T(n/2)+O(n)=O(n\log n).
$$

## Benchmark Contract

The inputs in this article have independently generated real and imaginary parts uniform in $[-1,1]$, from a fixed `mt19937_64` seed. Each measurement performs one forward transform. Restoring the input and constructing the optional root and reversal tables are outside timing; the bit-reversal permutation and all butterflies are inside. This is therefore the repeated-transform kernel of a planned FFT, not a one-shot convolution benchmark.

Measurements were made on one performance core of an Apple M4 Max with Apple Clang 17 and `-O3 -mcpu=native`; `-ffast-math` is not used. For the $2^{18}$-sample optimization ladder, we launched five fresh processes and took the median of their results; each process itself reports the median of seven transforms after two warmups. Size sweeps use five runs after one warmup. The [complete test and benchmark program](../../../code/fft_case.cpp) contains all four kernels. Its [full sweep output](../../../code/fft_m4_results.txt), [five process-level stage runs](../../../code/fft_m4_stage_results.txt), and [Matplotlib generator](../../../code/plot_fft.py) reproduce every figure.

## The Literal Recursion

The derivation translates directly into code:

```c++
void fft_recursive(vector<complex<double>> &a, complex<double> root) {
    int n = a.size();
    if (n == 1)
        return;

    vector<complex<double>> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; i++) {
        even[i] = a[2 * i];
        odd[i] = a[2 * i + 1];
    }

    fft_recursive(even, root * root);
    fft_recursive(odd, root * root);

    complex<double> w = 1;
    for (int i = 0; i < n / 2; i++) {
        auto value = w * odd[i];
        a[i] = even[i] + value;
        a[i + n / 2] = even[i] - value;
        w *= root;
    }
}
```

This is our measured baseline, not our final implementation. It allocates two vectors at every non-leaf node and copies every sample once per recursion level. At $n=2^{18}$, it takes 22.989 ms.

The arithmetic is $O(n\log n)$, but so is the copying. Big-O notation has correctly hidden the part we need to remove.

## Turning the Recursion Inside Out

Follow an input index through the even/odd splits. Its lowest bit chooses the first half, then the next bit chooses the next half, and so on. The leaf order is the order obtained by reversing the index bits.

We can perform this permutation once and execute the butterfly levels bottom-up:

```c++
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

for (int length = 2; length <= n; length *= 2) {
    complex<double> step = polar(1.0, -2 * pi / length);
    for (int first = 0; first < n; first += length) {
        complex<double> w = 1;
        for (int i = 0; i < length / 2; i++) {
            auto left = a[first + i];
            auto right = w * a[first + i + length / 2];
            a[first + i] = left + right;
            a[first + i + length / 2] = left - right;
            w *= step;
        }
    }
}
```

No allocation remains in the transform, and each stage scans contiguous blocks. At $2^{18}$ samples the time falls to 5.918 ms, a 3.88-fold speedup. This is our largest single improvement.

## Removing a Multiplication Chain

The iterative kernel still obtains each twiddle as `w *= step`. That costs almost one extra complex multiplication per butterfly and creates a dependency chain: the next root cannot exist until the previous multiplication finishes. It also accumulates roundoff along each block.

For repeated transforms of the same length, we can construct a root table once:

```c++
for (int k = 0; k < n / 2; k++)
    roots[k] = polar(1.0, -2 * pi * k / n);

for (int length = 2; length <= n; length *= 2) {
    int stride = n / length;
    for (int first = 0; first < n; first += length)
        for (int i = 0; i < length / 2; i++) {
            auto right = roots[i * stride] * a[first + i + length / 2];
            // The same two butterfly additions.
        }
}
```

At $2^{18}$ samples this changes 5.918 ms to 3.249 ms, another 1.82-fold improvement. Across the same five fresh processes, the median phase-timed run spends 0.905 ms in bit reversal and 2.446 ms in butterflies: the irregular permutation is about 27% of their sum, while the repeated arithmetic still dominates.

One radix-2 stage contains $n/2$ butterflies, and there are $\log_2 n$ stages. Counting a complex multiplication as six real operations and the two complex add/subtract results as four gives ten real floating-point operations per butterfly, or approximately

$$
5n\log_2n
$$

for the root-table kernel. At $n=2^{18}$, 3.249 ms therefore corresponds to an effective 7.26 GFLOP/s. This is an algorithmic operation-rate calculation, not a hardware-counter measurement of retired floating-point instructions.

The same count exposes the data movement. Each stage logically reads and writes the 4 MiB transform once, for 144 MiB across 18 stages, and performs another 36 MiB of root-table loads. Completing those butterflies in 2.446 ms corresponds to roughly 77 GB/s of logical traffic through the cache hierarchy. It is not a DRAM-bandwidth figure—the array and roots are repeatedly reused from cache—but it explains why reducing arithmetic does not make the stage free.

The root table contains $n/2$ complex numbers and occupies $8n$ bytes. The transform array itself occupies $16n$ bytes. The cache markers in the following plot refer only to the input array; a planned transform has the additional table footprint.

![FFT time by transform size](../img/fft-size.svg)

The final kernel remains about six to seven times faster than the allocating recursion throughout the large-size range. Both curves bend as the transform and its repeatedly accessed tables leave cache, but the algorithmic work remains $n\log n$.

## A Reversal Table That Did Not Win

Since roots benefited from precomputation, precomputing every bit-reversed index seems natural. The arithmetic that updates `j` disappears; each iteration instead loads `reversed[i]` and performs the same irregular swap.

The fresh five-process audit does not establish a large-size improvement. At $2^{18}$, the process medians are 3.249 ms with dynamic reversal and 3.344 ms with the table—rough parity at this level of run-to-run variation. The per-process ranges overlap: 3.163–3.325 ms and 3.213–3.395 ms respectively. The earlier single-series regression claim was too strong a conclusion from benchmark noise, and the size sweep does not show a sustained win in either direction. We retain the simpler dynamic update because precomputation has not demonstrated a benefit, not because these timings prove a particular microarchitectural cause.

The whole progression is shown below. Planning remains excluded from every bar; the whiskers span the five independently launched process medians.

![FFT optimization stages](../img/fft-stages.svg)

## Numerical Error Is Part of the Result

The direct root table changes more than speed. Repeated `w *= step` slowly moves the twiddle away from the unit circle, whereas each table entry is computed directly. We measured a forward transform followed by the corresponding inverse and recorded

$$
\max_i |\hat a_i-a_i|.
$$

![FFT forward-inverse error](../img/fft-error.svg)

At $n=2^{20}$, recurrent twiddles produce a maximum error of $5.24\cdot10^{-11}$; direct roots produce $1.94\cdot10^{-15}$. The latter stays close to machine precision across the sweep. These values belong to the fixed random input and compiler settings, not a general worst-case error bound, but the causal difference is clear: the optimization removed a long floating-point recurrence.

The test mode also compares all four forward transforms with the $O(n^2)$ definition through size 64 and checks forward/inverse round trips through $2^{16}$. It passes with AddressSanitizer and UndefinedBehaviorSanitizer.

## Returning to Polynomial Multiplication

Polynomials with $r$ and $s$ coefficients require at least $r+s-1$ samples. We pad to the next power of two, transform both inputs, multiply pointwise, and apply the inverse transform. Padding is essential: without it, coefficients beyond the transform length wrap around and produce cyclic convolution.

Rounding the final real parts to integers is valid only while the accumulated error is below one half. Coefficient splitting can reduce the error at the cost of more transforms. If an exact modular result is suitable, the [number-theoretic transform](../ntt/) replaces floating-point roots with roots in a finite field.

This article does not claim one-shot convolution time, real-input specialization, or comparison with a third-party FFT library. Plan construction, two forward transforms, pointwise multiplication, inverse scaling, padding, and allocation would all belong to that different contract. Nor does the final kernel implement Stockham ordering, higher radices, or a split real/imaginary layout.

Within the measured contract, the result is precise: removing recursive storage gives 3.88 times, direct twiddles give another 1.82 times and improve numerical accuracy, while the apparently obvious reversal table remains in rough parity and has not justified its extra planning state.
