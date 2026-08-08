---
title: Finite Fields
weight: 5
---

Residues modulo a prime are not the only mathematical objects that support the familiar arithmetic operations. Depending on which laws are preserved, these objects form *groups*, *rings*, and *fields*.

The terminology may seem needlessly abstract, but it lets us reuse algorithms. [Binary exponentiation](../exponentiation/) does not really need integers; it only needs an associative multiplication and an identity element. Gaussian elimination does not really need real numbers; it needs addition, multiplication, and division by non-zero elements. Once we identify the required structure, the same algorithm works on every instance of it.

## Algebraic Structures

### Groups

A *group* is a set with one closed operation that is associative, has an identity element, and gives every element an inverse.

Permutations are a useful non-numeric example. We define the product of two permutations as applying one after the other. Composition is associative, the permutation that changes nothing is the identity, and every permutation can be undone.

This means we can use binary exponentiation to compute the $n$-th power of a permutation in $O(\log n)$ compositions:

![](../../arithmetic/img/permutation.png)

The order of the operands still matters: permutation composition is generally not commutative. Binary exponentiation never required commutativity, so the algorithm remains valid without it.

### Rings and Fields

A *ring* has addition and multiplication. Addition forms a commutative group, multiplication is associative, and multiplication distributes over addition. The integers and the residues modulo any positive $m$ are rings.

A *field* is a commutative ring in which every non-zero element has a multiplicative inverse. The real and complex numbers are fields. The residues modulo $m$ form a field exactly when $m$ is prime: modulo $6$, for example, $2 \cdot 3 = 0$ even though neither factor is zero, so neither 2 nor 3 can have an inverse.

The distinction tells us which algorithms are legal. We can add and multiply matrices over any ring, but ordinary Gaussian elimination needs a field because it divides by pivots.

## Roots of Unity

Complex numbers are numbers of the form $a+bi$, where $a$ and $b$ are real and $i^2=-1$.

It is most convenient to think about complex numbers geometrically.

The *modulus* of a complex number is the real number $r = \sqrt{a^2+b^2}$. Geometrically, this is the length of the vector $(a,b)$.

For a non-zero complex number, the *argument* is the angle $\phi$ of that vector. It is normally computed as $\operatorname{atan2}(b,a)$ rather than from $\tan \phi=b/a$, because the tangent alone cannot distinguish opposite quadrants and is undefined when $a=0$.

This way we can represent a complex number using polar coordinates:

$$
a + bi = r \cdot ( \cos \phi + i \sin \phi )
$$

![](../../arithmetic/img/complex-plane.png)

The reason this representation is useful is that if we need to multiply two complex numbers, it can be shown with high school trigonometry that instead of doing binomial expansion we just need to multiply their moduli and add their arguments:

$$
\begin{aligned}
(a + bi) \cdot (c + di)
   &= r_1 \cdot ( \cos \phi_1 + i \sin \phi_1 ) \cdot r_2 \cdot ( \cos \phi_2 + i \sin \phi_2 )
\\ &= r_1 \cdot r_2 \cdot (\cos \phi_1 \cos \phi_2 - \sin \phi_1 \sin \phi_2 + i \cos \phi_1 \sin \phi_2 + i \cos \phi_2 \sin \phi_1 )
\\ &= r_1 \cdot r_2 \cdot (\cos (\phi_1 + \phi_2) + i \sin (\phi_1 + \phi_2) )
\end{aligned}
$$

Euler's formula gives a compact notation for points on the unit circle:

$$
e^{i\phi}=\cos \phi+i\sin \phi.
$$

It behaves exactly as the exponential notation suggests: multiplying two unit complex numbers adds their angles.

An $n$-th *root of unity* is a complex number $w$ satisfying $w^n=1$. It turns out that there are exactly $n$ of them, and they are equally spaced. Precisely, these are the numbers

$$
w_k=e^{i\tau\frac{k}{n}}, \qquad 0 \le k < n,
$$

where $\tau$ means $2\pi$ (a [modern notation](https://tauday.com/tau-manifesto)).

![](../../arithmetic/img/roots.png)

To see why the list is complete, write an arbitrary complex number as $r e^{i\phi}$. If its $n$-th power is 1, then $r^n=1$, so $r=1$, and $n\phi$ must be an integer multiple of $\tau$. This gives precisely the angles above.

The root $w_1$ is a *primitive* $n$-th root of unity. Its powers generate all the others and return to the identity after exactly $n$ steps:

$$
w_1^k=w_k \quad (0\le k<n),
\qquad
w_1^n=e^{i\tau}=1=w_0.
$$

The roots of unity form a finite cyclic **group** under multiplication. They do not form a ring: addition is not closed, since the sum of two roots is usually not another root. This group structure is what makes the [fast Fourier transform](/hpc/algorithms/fft/) work. The [number-theoretic transform](/hpc/algorithms/ntt/) uses analogous roots inside a finite field.

## Finite Fields

Finite fields are fields with finitely many elements. The residues modulo a prime $p$ form the simplest one, denoted $\mathbb F_p$ or $GF(p)$.

It turns out that you can also construct fields of size $p^k$ for any prime $p$ and positive integer $k$, and there are no finite fields of any other size. This is particularly useful for computers: choosing $p=2$ and $k=8$ produces a field with exactly 256 elements, one for every byte value.

### Polynomial Representation

To construct $GF(p^k)$, we choose an irreducible polynomial $P(x)$ of degree $k$ over $\mathbb F_p$. The field elements are polynomials of degree less than $k$; we add and multiply them normally and reduce the result modulo $P(x)$.

For a byte field, the coefficients are bits. The byte

```text
10110010
```

represents the polynomial

$$
x^7+x^5+x^4+x.
$$

Because coefficients are reduced modulo 2, addition and subtraction are both XOR. Multiplication is carryless polynomial multiplication followed by reduction.

AES uses $GF(2^8)$ with the irreducible polynomial

$$
P(x)=x^8+x^4+x^3+x+1,
$$

represented as `0x11b`. When a left shift produces an $x^8$ term, reducing by $P$ means XORing the remaining low terms, `0x1b`:

```c++
uint8_t add(uint8_t a, uint8_t b) {
    return a ^ b;
}

uint8_t sub(uint8_t a, uint8_t b) {
    return a ^ b;
}

uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            r ^= a;
        bool high = a & 0x80;
        a <<= 1;
        if (high)
            a ^= 0x1b;
        b >>= 1;
    }
    return r;
}
```

The loop is just eight steps, but we can still use field structure to implement division. The 255 non-zero elements form a multiplicative group, so $a^{255}=1$ and $a^{-1}=a^{254}$:

```c++
uint8_t power(uint8_t a, unsigned n) {
    uint8_t r = 1;
    while (n) {
        if (n & 1)
            r = mul(r, a);
        a = mul(a, a);
        n >>= 1;
    }
    return r;
}

uint8_t inverse(uint8_t a) {
    assert(a != 0);
    return power(a, 254);
}
```

Zero has no multiplicative inverse. This is not an implementation corner case but part of the field definition, and every division routine needs to handle it explicitly.

### Logarithm Tables

The multiplicative group of a finite field is cyclic. If $g$ is a primitive element, every non-zero value has the form $g^i$, and multiplication turns into addition of exponents:

$$
g^i \cdot g^j=g^{i+j \bmod 255}.
$$

For the AES polynomial, `0x03` is a primitive element. We can enumerate its powers once and build logarithm and exponential tables:

```c++
uint8_t logarithm[256];
uint8_t exponential[510];

void init_tables() {
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        exponential[i] = x;
        logarithm[x] = (uint8_t) i;
        x = mul(x, 3);
    }
    for (int i = 255; i < 510; i++)
        exponential[i] = exponential[i - 255];
}

uint8_t multiply(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0)
        return 0;
    return exponential[logarithm[a] + logarithm[b]];
}

uint8_t divide(uint8_t a, uint8_t b) {
    assert(b != 0);
    if (a == 0)
        return 0;
    return exponential[logarithm[a] + 255 - logarithm[b]];
}
```

Duplicating the exponential table removes a remainder operation from the hot path. The arrays use unsigned bytes: a plain `char` may be signed, and then values above 127 and negative table indices silently break the implementation. The explicit zero checks are equally important because $\log 0$ is undefined.

Table lookup is not automatically the fastest approach. The direct loop has a dependency chain, while tables add data loads; for bulk work, bitslicing, carryless multiplication, GFNI, or SIMD over independent field elements may be better. The right implementation depends on how much parallel work is available and whether the tables stay hot in cache.

Finite-field arithmetic appears in the AES substitution layer, [Reed–Solomon error correction](../error-correction/#reedsolomon-codes), polynomial hashes, and many cryptographic protocols. It is not a special encoding applied to all internet traffic: it is one small algebraic tool reused in many otherwise unrelated algorithms.

### Acknowledgements

The byte-field representation and AES polynomial follow the [Advanced Encryption Standard](https://csrc.nist.gov/pubs/fips/197/final). The standard also gives a direct `XTIME` formulation equivalent to the multiplication loop above.
