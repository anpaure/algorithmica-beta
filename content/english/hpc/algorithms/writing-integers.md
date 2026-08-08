---
title: Writing Decimal Integers
weight: 11
---

Writing an integer is almost the reverse of [reading one](../reading-integers), except for one annoying detail: the easiest digit to compute is the last one.

For a positive integer $x$, we can peel off its last digit as

$$
d = x \bmod 10, \qquad x \leftarrow \lfloor x / 10 \rfloor.
$$

Repeating this gives all digits in reverse order. The obvious implementation is short, correct, and serial. In this case study, we will make it 2.8 times faster by reducing the number of dependent quotient operations and then removing an unnecessary copy.

## The Experiment

We will format unsigned 32-bit integers into a caller-provided memory range. The function returns the first unwritten byte; it does not append a newline or a terminating zero. The output range always has room for ten bytes, and allocation and I/O are outside the timed region.

This narrow contract matters. A benchmark that computes a checksum of the digits measures arithmetic; a benchmark that stores the strings measures arithmetic and packing; a benchmark that writes them to a file mostly measures buffering and the kernel. We measure the second problem.

The [complete test and benchmark program](../../../code/writing_integers_bench.cpp) uses three deterministic input distributions, each containing $2^{20}$ values. Its [five-process median CSV](../../../code/writing_integers_m4_results.txt) is rendered by the [Matplotlib plot script](../../../code/plot_writing_integers.py).

- uniformly random 32-bit bit patterns;
- values divided as evenly as possible among every decimal length from 1 through 10;
- the counter sequence $0,1,\ldots,2^{20}-1$.

The measurements below were taken on an Apple M4 Max with Apple Clang 17.0.0 using `-O3 -mcpu=native`. Each process performs two warm-up runs followed by nine timed runs; the reported value is the median from five independent processes. Every implementation writes the complete decimal representation into a 10 MiB output buffer, and a checksum of its length and boundary bytes makes the work observable.

## One Digit at a Time

Most direct implementations write backwards into a small temporary buffer and then copy the result to its destination:

```c++
char *write_div10(char *out, uint32_t x) {
    char temporary[10];
    char *p = temporary + 10;

    do {
        uint32_t q = x / 10;
        *--p = char('0' + x - q * 10);
        x = q;
    } while (x != 0);

    size_t length = temporary + 10 - p;
    memcpy(out, p, length);
    return out + length;
}
```

The `do` is important because zero also has one digit. On uniformly random 32-bit values, this baseline takes **8.54 ns per value**.

It looks like the loop executes an integer division on every iteration. It does not. Division by the constant 10 is replaced by multiplication by a precomputed reciprocal and a shift. Here is the central part of the generated AArch64 loop:

```nasm
loop:
    umull   x11, w1, w8     // multiply by the reciprocal of 10
    lsr     x11, x11, #35   // q = x / 10
    madd    w12, w11, w9, w1
    strb    w12, [x13, #9]
    mov     x1, x11         // the next iteration needs q
    b.hi    loop
```

The compiler removed the expensive instruction, but it could not remove the dependency: the next quotient needs the previous quotient. A ten-digit number traverses this chain ten times before the copy can begin.

## Two Digits at a Time

There are only one hundred pairs of decimal digits. We can store them in a 200-byte table and divide by 100 instead of 10:

```c++
const char digit_pairs[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

char *write_pairs(char *out, uint32_t x) {
    char temporary[10];
    char *p = temporary + 10;

    while (x >= 100) {
        uint32_t q = x / 100;
        uint32_t r = x - q * 100;
        p -= 2;
        memcpy(p, digit_pairs + 2 * r, 2);
        x = q;
    }

    if (x < 10) {
        *--p = char('0' + x);
    } else {
        p -= 2;
        memcpy(p, digit_pairs + 2 * x, 2);
    }

    size_t length = temporary + 10 - p;
    memcpy(out, p, length);
    return out + length;
}
```

The last branch prevents a one-digit prefix from becoming, for example, `03`; all non-leading pairs keep their zero padding. This is one structural change: the temporary buffer and final copy are still there, but the dependent chain is half as long.

The time falls from 8.54 to **6.41 ns**. It is not a twofold improvement because the table loads, loop control, prefix branch, and final copy did not become twice as cheap.

## Removing the Copy

Writing backwards only requires a temporary buffer because we do not initially know where the representation begins. If we determine its decimal length first, we can place the end pointer inside the final output and keep the same backwards loop:

```c++
char *write_pairs_direct(char *out, uint32_t x) {
    char *end = out + decimal_length(x);
    char *p = end;

    while (x >= 100) {
        uint32_t q = x / 100;
        uint32_t r = x - q * 100;
        p -= 2;
        memcpy(p, digit_pairs + 2 * r, 2);
        x = q;
    }

    if (x < 10)
        *--p = char('0' + x);
    else {
        p -= 2;
        memcpy(p, digit_pairs + 2 * x, 2);
    }

    return end;
}
```

For 32-bit integers, `decimal_length` can be a short decision tree of comparisons with powers of ten. A floating-point `log10` is slower and, more importantly, needs correction near exact powers of ten.

The extra length calculation costs less than copying the completed string: the result improves to **5.64 ns**.

## Four-Digit Groups

We can shorten the quotient chain once more without constructing a huge table. Split the number into base-$10^4$ groups, write the leading group without zero padding, and write every following group as two digit pairs:

```c++
void write_fixed4(char *out, uint32_t x) {
    uint32_t hi = x / 100;
    uint32_t lo = x - hi * 100;
    memcpy(out,     digit_pairs + 2 * hi, 2);
    memcpy(out + 2, digit_pairs + 2 * lo, 2);
}

char *write_groups4(char *out, uint32_t x) {
    if (x < 10000)
        return write_small(out, x);

    if (x < 100000000) {
        uint32_t hi = x / 10000;
        uint32_t lo = x - hi * 10000;
        char *p = write_small(out, hi);
        write_fixed4(p, lo);
        return p + 4;
    }

    uint32_t hi = x / 100000000;
    uint32_t rest = x - hi * 100000000;
    uint32_t mid = rest / 10000;
    uint32_t lo = rest - mid * 10000;
    char *p = write_small(out, hi);
    write_fixed4(p, mid);
    write_fixed4(p + 4, lo);
    return p + 8;
}
```

`write_small` handles a value below $10^4$ with the same pair table. There is no four-digit table: the only lookup data remains 200 bytes.

On uniformly random 32-bit values, almost every input takes the same long branch and the formatter finishes in **3.10 ns**, a 2.75-fold improvement over the baseline. It also narrowly beats this standard library's `std::to_chars`, which takes 3.20 ns on the same workload.

![](../img/writing-integers-ladder.svg)

The graph includes `std::to_chars` as a reference, not as a step in our implementation. Its algorithm is an implementation detail of the version of libc++ shipped with Apple Clang 17.

## The Distribution Is Part of the Problem

The four-digit version has more branches than the pair loop. Uniform random 32-bit values almost always have nine or ten digits, so those branches are highly predictable. A mixture divided as evenly as possible among all decimal lengths is different.

![](../img/writing-integers-distributions.svg)

Here are the complete measurements in nanoseconds per value:

| Implementation | Uniform `uint32_t` | Uniform digit length | Counter |
|:--|--:|--:|--:|
| `/10`, then copy | 8.54 | 11.56 | 5.44 |
| two digits, then copy | 6.41 | 9.91 | 3.81 |
| two digits, direct | 5.64 | **6.61** | 2.96 |
| four-digit groups, direct | **3.10** | 7.09 | **1.54** |
| `std::to_chars` | 3.20 | 7.50 | 1.75 |

The last optimization regresses from 6.61 to 7.09 ns on the mixed-length input. It wins when long values dominate and on the short counter sequence, but the branchy fixed-group dispatch is not universally better. There is no context-free “fastest integer formatter”; the digit-length distribution is part of its interface to the hardware.

## Correctness

Decimal boundaries are exactly where length and prefix bugs hide. The harness compares every implementation byte-for-byte with `std::to_chars` for one million fixed-seed random values, zero, `UINT32_MAX`, and the neighbors of every representable power of ten. The same tests are run under AddressSanitizer and UndefinedBehaviorSanitizer.

Signed integers require a little care. Write the minus sign first and form the magnitude in the corresponding unsigned type. Converting the negative value to unsigned and subtracting it from zero is defined; evaluating `-INT_MIN` in the signed type is not.

Wider integers use the same grouping idea with a larger maximum output range. The constants and the number of groups change, so they need their own overflow and boundary tests rather than a cast through `uint32_t`.

## Buffering and Limits

This benchmark deliberately stops at memory. Calling `write`, `printf`, or even `fwrite` once per value adds stream and system-call bookkeeping unrelated to decimal conversion. A program that writes many values should append each representation and its delimiter to a large output buffer and flush only when there is no room for the next worst-case record.

Variable-length SIMD formatting is possible, but digit arithmetic is not the difficult part. Several lanes produce strings of different lengths, and packing them without gaps adds shuffles and bookkeeping. Fixed-width dates, timestamps, and zero-padded identifiers avoid that problem and deserve a separate benchmark; we do not claim a SIMD speedup here without implementing and measuring the packing step.

For ordinary C++ code, `std::to_chars` remains a strong default: it accepts a caller-provided range, allocates nothing in the hot path, and has a much broader contract than our unsigned 32-bit formatter. The custom version is useful when the narrow contract is real, the distribution is known, and three nanoseconds per value is important enough to own the tests.
