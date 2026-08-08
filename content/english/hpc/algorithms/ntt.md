---
title: Number-Theoretic Transform
weight: 7
---

The [fast Fourier transform](../fft/) only needs a few properties of complex numbers. We need an element $\omega$ whose powers cycle after exactly $n$ steps, we need to be able to add and multiply these powers, and we need to divide by $n$ for the inverse transform.

There are other number systems with the same properties. In particular, we can run the FFT butterfly using integers modulo a carefully chosen prime. The resulting algorithm is called the *number-theoretic transform* (NTT).

It is useful because all operations are exact. The price is that not every modulus supports every transform length, and the answer is a residue rather than an unrestricted integer.

## Modular Roots of Unity

For a prime $q$, the nonzero residues modulo $q$ form a multiplicative group of size $q-1$. If $g$ is a primitive root, then

$$
\omega_n=g^{(q-1)/n}\pmod q
$$

has order $n$ whenever $n$ divides $q-1$. This is our modular counterpart of $e^{-2\pi i/n}$.

A commonly used prime is

$$
q=998244353=119\cdot2^{23}+1.
$$

The number 3 is a primitive root modulo $q$, so this modulus supports every power-of-two transform length up to $2^{23}$. It does *not* support length $2^{24}$; primality alone is not enough.

We will need binary exponentiation for roots and modular inverses:

```c++
const int mod = 998244353;
const int primitive_root = 3;

int power(int a, int n) {
    int result = 1;
    while (n > 0) {
        if (n & 1)
            result = result * 1LL * a % mod;
        a = a * 1LL * a % mod;
        n >>= 1;
    }
    return result;
}
```

Since the modulus is prime, Fermat's theorem gives $x^{-1}=x^{q-2}\pmod q$ for every nonzero $x$.

## Reusing the FFT

The structure is exactly the same as the iterative radix-2 FFT: bit-reverse the input and then run stages of butterflies. Only complex multiplication and addition are replaced with modular arithmetic.

```c++
void ntt(vector<int> &a, bool inverse = false) {
    int n = a.size(); // power of two, n <= 2^23

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
        int step = power(primitive_root, (mod - 1) / len);
        if (inverse)
            step = power(step, mod - 2);

        for (int first = 0; first < n; first += len) {
            int w = 1;
            for (int i = 0; i < len / 2; i++) {
                int u = a[first + i];
                int v = a[first + i + len / 2] * 1LL * w % mod;

                a[first + i] = u + v;
                if (a[first + i] >= mod)
                    a[first + i] -= mod;

                a[first + i + len / 2] = u - v;
                if (a[first + i + len / 2] < 0)
                    a[first + i + len / 2] += mod;

                w = w * 1LL * step % mod;
            }
        }
    }

    if (inverse) {
        int inv_n = power(n, mod - 2);
        for (int &x : a)
            x = x * 1LL * inv_n % mod;
    }
}
```

The input coefficients are assumed to be in $[0,q)$, and the length is assumed to be a power of two not exceeding $2^{23}$. Under these conditions, every stage length divides $q-1$, so the requested root exists.

Notice that butterfly addition and subtraction need at most one correction. Both inputs are already reduced, so their sum is below $2q$ and their difference is above $-q$. Keeping this invariant saves a general remainder operation on two of the three arithmetic paths.

## Modular Convolution

The convolution wrapper is also almost identical to the floating-point version:

```c++
vector<int> multiply_mod(vector<int> a, vector<int> b) {
    if (a.empty() || b.empty())
        return {};

    int size = a.size() + b.size() - 1;
    int n = 1;
    while (n < size)
        n *= 2;

    a.resize(n);
    b.resize(n);

    ntt(a);
    ntt(b);

    for (int i = 0; i < n; i++)
        a[i] = a[i] * 1LL * b[i] % mod;

    ntt(a, true);
    a.resize(size);
    return a;
}
```

This returns the polynomial product *modulo* `998244353`. It is exact in that ring, which is not the same as being the exact ordinary integer product.

Suppose the input coefficients are nonnegative and bounded by $A$ and $B$. Each output coefficient is a sum of at most $\min(n,m)$ products, so

$$
c_k\le \min(n,m)AB.
$$

If this bound is smaller than $q$, the modular residue uniquely identifies the ordinary coefficient. Otherwise, the true value may have wrapped around the modulus. For larger coefficients, use several NTT-friendly primes and reconstruct the answer with the Chinese remainder theorem. The product of the moduli has to exceed the range of possible answers, and every chosen prime must support the padded transform length.

For signed coefficients, residues above $q/2$ can be interpreted as negative only when the absolute coefficient bound is below $q/2$. This is the NTT equivalent of the FFT's error bound: exact arithmetic still needs a range proof.

## Optimizing Modular Butterflies

Compared with a complex FFT, the NTT replaces floating-point operations with integer multiplication and modular reduction. Its memory access pattern — including bit reversal — is otherwise the same.

The `% mod` operations look like divisions, but `mod` is a compile-time constant. Optimizing compilers replace division by a constant with multiplication by a precomputed reciprocal and a few corrections. Explicit [Barrett or Montgomery reduction](/hpc/arithmetic/division) becomes useful when the modulus is not visible to the compiler, when reductions are being delayed, or when writing a SIMD kernel.

Roots can be precomputed once for each stage or, for repeated transforms, as a complete table. This removes exponentiation and some dependency chains at the cost of extra memory reads. As with the FFT, the right trade-off depends on transform size and reuse.

SIMD is slightly awkward because a 32-bit modular product needs the 64-bit halves of several lane products, and vector instruction sets do not always provide the exact widening operation we want. It is often easier to process several independent butterflies or transforms together than to force one recurrence into all lanes. Delaying reductions can help, but only after proving that the enlarged range cannot overflow.

## Checking the Preconditions

The most useful correctness test is an exact round trip:

```text
inverse(ntt(a)) == a
```

Also compare `multiply_mod` with the quadratic convolution on random small arrays. Test lengths 1 and powers of two around every application boundary, coefficients 0 and `mod - 1`, and the largest supported length.

For each stage root $w$ of order $n>1$, the two important identities are

$$
w^n=1,\qquad w^{n/2}\ne1\pmod q.
$$

The first identity alone is not enough: an element of a smaller order also satisfies it and produces a perfectly deterministic wrong transform.

Benchmark transforms separately from convolution, allocation, and Chinese-remainder reconstruction. Record the modulus, length, coefficient range, root-table strategy, and reduction method. An NTT is not “an FFT without errors”; it is the same butterfly operating under a stricter algebraic contract.
