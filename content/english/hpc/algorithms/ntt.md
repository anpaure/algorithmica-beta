---
title: Number-Theoretic Transform
weight: 7
---

The fast Fourier transform is usually introduced over the complex numbers, but the butterfly itself does not care about angles. It needs addition, multiplication, an $n$-th root of unity, and a way to divide by $n$ at the end. We can supply all of these operations with integers modulo a prime.

The result is the *number-theoretic transform* (NTT). It computes polynomial convolutions without floating-point error, and its inner loop contains enough modular arithmetic to make a good optimization case study.

We will begin with the textbook iterative transform, measure it, remove one bottleneck at a time, and end with a forward transform that is 6.1 times faster on the benchmark problem. One attempted optimization will make it slower. That failure is important: modular arithmetic that looks expensive in C++ is not necessarily expensive in machine code.

## The Algebraic Contract

Let $q$ be prime. The nonzero residues modulo $q$ form a multiplicative group of order $q-1$, so if $g$ is a primitive root and $n$ divides $q-1$, then

$$
\omega_n=g^{(q-1)/n}\pmod q
$$

has order $n$. Its powers play the same role as the equally spaced points on the complex unit circle.

We will use

$$
q=998244353=119\cdot2^{23}+1,\qquad g=3.
$$

This choice gives us a primitive root for every power-of-two length up to $2^{23}$. It does not give us one for $2^{24}$. Primality alone is not enough; the transform length has to divide $q-1$.

The transform in this article therefore has a strict contract:

1. $n$ is a nonzero power of two and $n\le2^{23}$;
2. every input coefficient is already in $[0,q)$;
3. the result is interpreted modulo $q$.

The [complete program](../../../code/ntt_case.cpp) checks these conditions at its public boundary. It also checks that the supplied plan has the same length as the input. The benchmark performs those checks once before timing and then calls the unchecked kernel:

```c++
void forward_transform(vector<uint32_t>& a,
                       Algorithm algorithm,
                       const Plan& plan) {
    check_input(a);
    if (plan.n != a.size())
        throw invalid_argument("plan length mismatch");
    forward_kernel(a, algorithm, plan);
}
```

The modular butterfly is

$$
(u,v)\mapsto(u+\omega v,\ u-\omega v)\pmod q.
$$

If $u,v<q$, the sum is below $2q$ and the difference is above $-q$. Addition and subtraction therefore need at most one correction each:

```c++
uint32_t add_mod(uint32_t a, uint32_t b) {
    uint32_t s = a + b;       // s < 2q < 2^31
    return s >= q ? s - q : s;
}

uint32_t sub_mod(uint32_t a, uint32_t b) {
    return a >= b ? a - b : a + q - b;
}
```

We only need a general reduction for multiplication.

## A Textbook Transform

Binary exponentiation supplies the stage roots and the inverse of $n$:

```c++
uint32_t power(uint32_t a, uint32_t k) {
    uint32_t r = 1;
    while (k != 0) {
        if (k & 1)
            r = uint64_t(r) * a % q;
        a = uint64_t(a) * a % q;
        k >>= 1;
    }
    return r;
}
```

An iterative decimation-in-time transform first puts the input into bit-reversed order and then runs stages of length $2,4,8,\ldots,n$:

```c++
void ntt(vector<uint32_t>& a, bool inverse = false) {
    int n = a.size();
    assert(n && (n & (n - 1)) == 0 && n <= (1 << 23));

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
        uint32_t step = power(3, (q - 1) / len);
        if (inverse)
            step = power(step, q - 2);

        for (int first = 0; first < n; first += len) {
            uint32_t w = 1;
            for (int j = 0; j < len / 2; j++) {
                uint32_t u = a[first + j];
                uint32_t v = uint64_t(a[first + j + len / 2]) * w % q;
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
                w = uint64_t(w) * step % q;
            }
        }
    }

    if (inverse) {
        uint32_t inv_n = power(n, q - 2);
        for (uint32_t& x : a)
            x = uint64_t(x) * inv_n % q;
    }
}
```

There are $n\log_2n/2$ butterflies. A forward transform of length $2^{18}$ has 2,359,296 of them.

### Benchmarking Contract

All measurements below were made by a single-threaded process on an Apple M4 Max with Apple Clang 17.0.0 and

```text
-std=c++20 -O3 -mcpu=native
```

Inputs are fixed-seed uniformly distributed residues in $[0,q)$. Plan construction, allocation, input generation, and copying are outside the timed region. Each process reports the median of seven samples; a sample targets about 30 million butterflies, capped at 2,000 transforms and floored at three. The plotted point is the componentwise median of five independent processes. One warm transform precedes the samples. The output is consumed after every sample. The harness does not request CPU affinity, which macOS does not expose as a stable public interface.

The plots time a *forward transform*. Inverse normalization and the pointwise products needed by a convolution are not included. This separation matters because otherwise a faster transform could be hidden by allocation or by the caller.

At $n=2^{18}$, the textbook implementation takes 6.248 ms, or 2.648 ns per butterfly. That is 378 million butterflies per second.

There are no floating-point operations here, so a GFLOPS number would describe nothing. A modular butterfly is the useful unit. The baseline performs two modular products per butterfly: one for $\omega v$ and another to advance $\omega$.

## Finding the First Bottleneck

The remainder operator is an obvious suspect. The inner loop contains two expressions of the form

```c++
uint64_t(a) * b % 998244353;
```

but the generated code contains no integer division. Because the modulus is a compile-time constant, Clang replaces the remainder with multiplication by a reciprocal:

```asm
umull   x8, w1, w0
umulh   x9, x8, magic
lsr     x9, x9, #29
msub    w0, w9, q, w8
```

The constants are hoisted out of the transform loop. This is still several instructions, but the more damaging line is the second product:

```c++
w = uint64_t(w) * step % q;
```

Every value of `w` depends on the previous one. The processor cannot begin the next update until it has finished the current reduction. Worse, this recurrence exists only because we rediscover the same powers of the same roots on every transform.

This is the first bottleneck to remove.

## Planning the Transform

For repeated transforms of the same length, we build a plan once. Roots for a stage of length `len` occupy the range `[len / 2, len)`. The plan allocates $n$ slots, leaves slot zero unused, and fills the other $n-1$ slots:

```c++
for (int len = 2; len <= n; len *= 2) {
    uint32_t step = power(3, (q - 1) / len);
    uint32_t w = 1;
    for (int j = 0; j < len / 2; j++) {
        root[len / 2 + j] = w;
        w = uint64_t(w) * step % q;
    }
}
```

To isolate this experiment, we do **not** change the permutation. Both the textbook transform and the planned transform compute the bit-reversed indices online with the same loop. Only root generation and the loop-carried `w` update disappear. The butterfly loop becomes

```c++
for (int len = 2; len <= n; len *= 2)
    for (int first = 0; first < n; first += len)
        for (int j = 0; j < len / 2; j++) {
            uint32_t u = a[first + j];
            uint32_t v = uint64_t(a[first + j + len / 2])
                       * root[len / 2 + j] % q;
            a[first + j] = add_mod(u, v);
            a[first + j + len / 2] = sub_mod(u, v);
        }
```

The loop now has one modular product per butterfly instead of two, and consecutive butterflies are independent. We traded computation for one sequential root-table load.

At $n=2^{18}$, this change reduces the time from 6.248 ms to 1.645 ms: a 3.80x speedup. Throughput rises from 378 million to 1.434 billion butterflies per second.

The exponentiations were never the important part. There are only $\log_2n$ of them. The win comes from removing $n\log_2n/2$ recurrent root updates.

Planning is not free. The benchmark deliberately excludes it because the intended workload reuses a length for multiple transforms: one convolution already performs two forward transforms and one inverse transform. A one-shot transform of a tiny vector may be better off with a smaller plan or no plan at all.

## A Reduction That Loses

We still perform one `% q` per butterfly. A standard way to avoid it when one operand is fixed is Shoup multiplication. For every root $w$, precompute

$$
\widehat w=\left\lfloor\frac{w2^{32}}q\right\rfloor.
$$

For $0\le x,w<q<2^{31}$, estimate the quotient and correct once:

```c++
struct Twiddle {
    uint32_t value;
    uint32_t shoup;
};

uint32_t multiply(uint32_t x, Twiddle w) {
    uint64_t quotient = uint64_t(x) * w.shoup >> 32;
    uint64_t remainder = uint64_t(x) * w.value - quotient * q;
    if (remainder >= q)
        remainder -= q;
    return remainder;
}
```

The estimate is either the true quotient or one smaller, so `remainder` is in $[0,2q)$ and one correction is sufficient. The test program compares one million random Shoup products with the definition using `% q`.

This looks like an optimization. On this processor it is not one.

![Constant remainder versus Shoup reduction](../img/ntt-reduction.svg)

At $n=2^{18}$, replacing the compiler's constant remainder with Shoup multiplication increases the time from 1.645 ms to 2.040 ms. The planned transform is now only 3.06 times faster than the baseline.

The reason is visible in the generated instruction sequences. The constant remainder becomes a low product followed by `umulh`, a shift, and `msub`. The Shoup probe computes its quotient product, then follows it with `mul`, `umaddl`, a comparison, and `csel`. That is a longer dependent chain on this core.

There is deliberately no table-size explanation hiding in this comparison. Both kernels read the same eight-byte `Twiddle {value, shoup}` entry; the remainder kernel merely ignores the second word. The measured difference therefore belongs to the arithmetic and its dependencies, not to different root traffic. On another ISA, with vector lanes, or with a runtime modulus, the balance can reverse. Here it does not.

We keep the losing version in the benchmark and the plot because deleting negative results from an optimization diary teaches the wrong lesson. We do not keep it in the final kernel: the next step returns to the compiler's faster constant remainder.

## Removing the Permutation

After planning, the arithmetic loop is short enough that the bit-reversal pass matters. Computing the indices online costs integer work, and the resulting swaps still move distant coefficients. The cleanest improvement is not to compute the permutation faster, but to stop requiring it.

A convolution does not require either forward transform to be in natural order. It only requires corresponding frequency coefficients to have the same order before pointwise multiplication.

Decimation in frequency (DIF) changes the butterfly to

$$
(u,v)\mapsto(u+v,\ (u-v)\omega),
$$

runs the stage lengths in the order $n,n/2,\ldots,2$, and produces bit-reversed output. Decimation in time (DIT) consumes bit-reversed input and produces natural-order output. We can therefore use DIF for both forward transforms, multiply their bit-reversed outputs element by element, and use inverse DIT afterward. No permutation is performed anywhere.

The forward kernel is direct:

```c++
void forward_dif(vector<uint32_t>& a, const Plan& p) {
    for (int len = a.size(); len >= 2; len /= 2)
        for (int first = 0; first < int(a.size()); first += len)
            for (int j = 0; j < len / 2; j++) {
                uint32_t u = a[first + j];
                uint32_t v = a[first + j + len / 2];
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] =
                    uint64_t(sub_mod(u, v)) * p.root[len / 2 + j] % q;
            }
}
```

The inverse runs the DIT stages upward with inverse roots and multiplies every coefficient by $n^{-1}$ at the end:

```c++
void inverse_dit(vector<uint32_t>& a, const Plan& p) {
    for (int len = 2; len <= int(a.size()); len *= 2)
        for (int first = 0; first < int(a.size()); first += len)
            for (int j = 0; j < len / 2; j++) {
                uint32_t u = a[first + j];
                uint32_t v = uint64_t(a[first + j + len / 2])
                           * p.inverse_root[len / 2 + j] % q;
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
            }

    for (uint32_t& x : a)
        x = uint64_t(x) * p.inverse_n % q;
}
```

This version never constructs or reads a reversal table. At $n=2^{18}$ it takes 1.024 ms. It is 6.10 times faster than the textbook transform and 1.61 times faster than the planned natural-order transform.

![Speedup after each NTT optimization](../img/ntt-stages.svg)

This is why bottlenecks have to be removed in sequence. Shoup loses 24% against the compiler reduction, so we revert it. Removing the permutation then cuts another 38% from the faster planned kernel. Keeping the old layout and polishing the modular multiply would have optimized the smaller problem.

## Scaling with Transform Length

The size sweep tells us whether the headline point is representative:

![NTT time per butterfly by transform length](../img/ntt-size.svg)

The textbook implementation stays between 2.30 and 2.77 ns per butterfly. The permutation-free implementation stays between 0.43 and 0.48 ns per butterfly over the entire range from $2^8$ to $2^{20}$.

For a forward transform, the active coefficient array occupies $4n$ bytes and the active forward-root table occupies $8n$ bytes. Their combined 12-byte-per-element working set outgrows the M4 Max's 128 KiB L1 data cache between $2^{13}$ and $2^{14}$, as marked on the plot. There is no corresponding cliff: the stages stream through contiguous coefficient blocks and root ranges, and the 16 MiB cluster L2 still holds the active data through $2^{20}$.

That is the *active* memory, not the whole plan allocation. The benchmark plan owns separate forward and inverse `Twiddle` arrays: $n$ eight-byte slots each, with slot zero unused in both, plus one eight-byte inverse of $n$. Its total twiddle payload is therefore $16n+8$ bytes, while a forward transform reads only one of the two arrays. A final remainder-only implementation can store 32-bit roots instead, reducing the active root table to $4n$ bytes and both directional tables together to $8n$ bytes.

At $n=2^{20}$, the baseline takes 29.035 ms and the permutation-free kernel takes 4.523 ms, a 6.42x speedup. The latter executes 2.319 billion butterflies per second. Algorithmically, that is 2.319 billion modular multiplications and 4.637 billion modular additions or subtractions per second, although each constant remainder expands to several machine instructions.

The permutation-free version wins throughout this sweep, but it returns a different order. A library still needs a natural-order transform when the caller asks for one; convolution should use the faster path because its pointwise product does not care about the permutation.

## Exact Convolution

The optimized convolution pads both inputs to a supported power of two and keeps the intermediate order bit-reversed:

```c++
vector<uint32_t> multiply_mod(vector<uint32_t> a,
                              vector<uint32_t> b) {
    if (a.empty() || b.empty())
        return {};

    int output_size = a.size() + b.size() - 1;
    int n = bit_ceil(unsigned(output_size));
    assert(n <= (1 << 23));
    a.resize(n);
    b.resize(n);

    Plan plan(n);
    forward_dif(a, plan);
    forward_dif(b, plan);
    for (int i = 0; i < n; i++)
        a[i] = uint64_t(a[i]) * b[i] % q;
    inverse_dit(a, plan);

    a.resize(output_size);
    return a;
}
```

The result is exact in $\mathbb Z/q\mathbb Z$. That is not automatically the exact product over the ordinary integers.

If the inputs are nonnegative and bounded by $A$ and $B$, then each output coefficient satisfies

$$
c_k\le\min(|a|,|b|)AB.
$$

When this bound is below $q$, the residue identifies the ordinary coefficient. If it is not, the coefficient may have wrapped around even though every NTT operation was exact. Larger ranges require several NTT-friendly primes followed by Chinese remainder reconstruction; their product must exceed the range of the answer, and every prime must support the padded transform length.

For signed coefficients, the symmetric interpretation of residues is unique only when the absolute result is below $q/2$.

## Testing and Reproduction

Round trips alone are not sufficient tests: a forward and inverse transform can contain matching mistakes. The program checks all of the following:

- one million random Shoup products against 64-bit `% q`;
- forward/inverse round trips for all four kernels and every power of two through $2^{12}$;
- random convolutions against the quadratic definition;
- the boundary residues 0 and $q-1$;
- checked-wrapper rejection of non-power-of-two lengths, unreduced coefficients, and mismatched plans.

The same suite passes both warnings-as-errors and AddressSanitizer/UndefinedBehaviorSanitizer builds:

```bash
clang++ -std=c++20 -O3 -mcpu=native \
  -Wall -Wextra -Wpedantic -Werror static/code/ntt_case.cpp -o ntt
./ntt --test
python3 scripts/median-csv.py --runs 5 \
  --output static/code/ntt_m4_results.txt -- ./ntt --bench

python3 static/code/plot_ntt.py static/code/ntt_m4_results.txt \
  content/english/hpc/algorithms/img/ntt-size.svg \
  content/english/hpc/algorithms/img/ntt-reduction.svg \
  content/english/hpc/algorithms/img/ntt-stages.svg

clang++ -std=c++20 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
  static/code/ntt_case.cpp -o ntt-sanitized
./ntt-sanitized --test
```

The [raw measurements](../../../code/ntt_m4_results.txt) and [plotting script](../../../code/plot_ntt.py) are stored with the article. Plan construction is intentionally outside the timing, and the measurements belong to this CPU and compiler. In particular, the failed Shoup result should not be generalized to a machine whose constant-remainder sequence, multiply throughput, vector ISA, or cache hierarchy is different.

The final lesson is not that every NTT should use these exact instructions. It is that the most expensive-looking operator was not the main bottleneck. Reusing algebraic structure removed a dependency chain, and changing the representation of the intermediate result removed an entire memory pass. Those are much larger optimizations than spelling modular reduction by hand.
