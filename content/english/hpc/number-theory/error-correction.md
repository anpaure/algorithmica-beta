---
title: Error Correction
weight: 6
---

If we store one bit and later read the opposite value, there is no way to tell whether the original bit was zero or one. If we store it three times, there is: after one bit flip, `000` becomes one of `001`, `010`, or `100`, all of which are still closer to `000` than to `111`.

This is the main idea of *error-correcting codes*: we deliberately add redundancy so that valid messages are far apart. The amount and shape of that redundancy determines which failures we can detect and correct.

## Hamming Distance

The *Hamming distance* $d(x,y)$ is the number of positions in which two equal-length words differ. If the minimum distance between any two valid codewords is $d_{\min}$, then the code can:

- detect up to $d_{\min}-1$ changed symbols;
- correct up to $\lfloor(d_{\min}-1)/2\rfloor$ changed symbols.

The first claim follows because fewer than $d_{\min}$ changes cannot turn one valid word into another. For correction, the received word must be strictly closer to the original codeword than to every other one.

The threefold repetition code has minimum distance 3 and therefore corrects one bit, but it spends three physical bits for every useful bit. Better codes protect many data bits together.

The distance model needs a fault model. A radio link may flip individual symbols, a scratched disk may corrupt one long burst, and a failed storage server produces an *erasure* whose location is already known. These are different problems even when the number of missing bits is the same.

## Detecting Errors

The simplest error-detecting code appends one parity bit — the XOR of all data bits. Any odd number of flips changes the parity, while an even number remains undetected.

A *cyclic redundancy check* keeps more information. It interprets a bit string as a polynomial over $\mathbb F_2$, where addition and subtraction are both XOR, multiplies it by $x^r$, and appends the remainder modulo a generator polynomial $G(x)$ of degree $r$. The resulting codeword is divisible by $G$, so a non-zero remainder at the receiver proves that an error occurred.

Polynomial division can be implemented with shifts and XORs, but modern processors also have carryless-multiplication instructions that process many polynomial bits at once. This is why CRCs can protect high-bandwidth streams without executing a bit-by-bit long division.

A CRC only detects accidental corruption. It does not protect against an adversary, who can modify the message and calculate a new checksum. That requires a keyed message authentication code.

## Locating One Bad Bit

A [Hamming $(7,4)$ code](https://doi.org/10.1002/j.1538-7305.1950.tb00463.x) stores four data bits and three parity bits. Number the seven bit positions starting from one. Positions 1, 2, and 4 contain parity; every other position contains data.

Each parity check covers the positions whose binary index has the corresponding bit set:

- check 1 covers positions `1, 3, 5, 7`;
- check 2 covers positions `2, 3, 6, 7`;
- check 4 covers positions `4, 5, 6, 7`.

Every position participates in a unique subset of the three checks. If position 6 is flipped, checks 2 and 4 fail, producing the binary number `110`, which is 6. The error tells us its own location.

```c++
int encode(int x) {
    // Put the four low bits of x into positions 3, 5, 6, and 7.
    int c = 0;
    c |= (x & 1) << 2;
    c |= (x & 2) << 3;
    c |= (x & 4) << 3;
    c |= (x & 8) << 3;

    // Choose the parity bits so that every check has even parity.
    c |= __builtin_parity(c & 0b1010101);
    c |= __builtin_parity(c & 0b1100110) << 1;
    c |= __builtin_parity(c & 0b1111000) << 3;
    return c;
}

int correct(int c) {
    int syndrome = 0;
    syndrome |= __builtin_parity(c & 0b1010101);
    syndrome |= __builtin_parity(c & 0b1100110) << 1;
    syndrome |= __builtin_parity(c & 0b1111000) << 2;

    if (syndrome)
        c ^= 1 << (syndrome - 1);
    return c;
}

int decode(int c) {
    c = correct(c);
    return ((c >> 2) & 1)
         | ((c >> 3) & 2)
         | ((c >> 3) & 4)
         | ((c >> 3) & 8);
}
```

The decoder assumes that at most one of the seven bits changed. Two flips can produce the syndrome of a third position, causing it to "correct" an unbroken bit. Adding one overall parity bit increases the minimum distance to 4 and gives the common SECDED code: single-error correction and double-error detection.

## Reed–Solomon Codes

Hamming codes work naturally with bits. Storage systems often lose larger symbols — bytes, packets, or entire disk shards. [*Reed–Solomon codes*](https://doi.org/10.1137/0108018) handle this by doing arithmetic in a [finite field](../finite/).

Treat $k$ data symbols as the coefficients of a polynomial

$$
P(x)=m_0+m_1x+\ldots+m_{k-1}x^{k-1}
$$

and store its values at $n$ different field points. Two polynomials of degree less than $k$ can agree at most $k-1$ points, so the resulting code has distance

$$
d_{\min}=n-k+1.
$$

It can recover from $s$ known erasures and $e$ unknown errors whenever

$$
2e+s\le n-k.
$$

An erasure costs half as much because its position is already known. In particular, if only erasures occur, any $k$ surviving evaluations determine the original polynomial.

Practical implementations normally use a *systematic* form: the original data symbols are stored unchanged and the encoder calculates only the parity symbols. Encoding and recovery then become matrix operations over a small finite field, often $GF(2^8)$.

The arithmetic can be implemented with lookup tables, shifts and XORs, bitslicing, or specialized instructions such as carryless multiplication and GFNI. Independent symbols and shards are easy to process with [SIMD](/hpc/simd/), while interleaving can spread one physical burst across several codewords.

The code should be chosen for the failures that actually happen. Two parity shards recover two missing shards, but they do not necessarily correct two shards that silently return wrong data: unknown errors spend twice as much distance. Error correction is not a promise that nothing will fail; it is a precise agreement about which failures can be repaired.
