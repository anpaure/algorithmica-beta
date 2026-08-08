---
title: Writing Decimal Integers
weight: 11
---

Writing an integer is almost the reverse of [reading one](../reading-integers), except for one annoying detail: the easiest digit to compute is the last one.

For a positive integer $x$, we can peel off its last digit as

$$
d = x \bmod 10, \qquad x \leftarrow \lfloor x / 10 \rfloor.
$$

Repeating this gives all digits in reverse order. This is why most integer formatting algorithms write backwards into a small temporary buffer and then copy the result to its final destination.

In this section, we will focus on unsigned 32-bit integers. Signed integers only require writing a minus sign and taking the magnitude in the unsigned type; wider integers use exactly the same ideas but need a larger buffer.

## Scalar Baseline

Here is the direct implementation. The caller supplies a pointer to the end of a buffer with at least 10 bytes available before it, and the function returns the beginning of the decimal representation:

```cpp
char *write_u32(char *end, unsigned x) {
    char *p = end;

    do {
        unsigned q = x / 10;
        *--p = char('0' + x - q * 10);
        x = q;
    } while (x != 0);

    return p;
}
```

The `do` is important because zero also has one digit. A typical use looks like this:

```cpp
char buffer[10];
char *end = buffer + 10;
char *begin = write_u32(end, x);

fwrite(begin, 1, end - begin, stdout);
```

This loop looks like it executes one [integer division](/hpc/arithmetic/division) per digit. In reality, division by the constant 10 is replaced by multiplication by a precomputed reciprocal and a shift. You should normally leave `/ 10` in the source code and let the compiler pick the correct sequence for the target architecture.

The loop is still serial: we need the current quotient before we can compute the next one. A 10-digit number therefore goes through almost the same dependency chain as ten consecutive divisions by a constant.

## Two Digits at a Time

There are only one hundred pairs of decimal digits. We can store them in a 200-byte table and divide by 100 instead of 10:

```cpp
const char digits[] =
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

char *write_u32(char *end, unsigned x) {
    char *p = end;

    while (x >= 100) {
        unsigned q = x / 100;
        unsigned r = x - q * 100;
        p -= 2;
        memcpy(p, digits + 2 * r, 2);
        x = q;
    }

    if (x < 10) {
        *--p = char('0' + x);
    } else {
        p -= 2;
        memcpy(p, digits + 2 * x, 2);
    }

    return p;
}
```

This cuts the number of dependent iterations roughly in half. The last branch prevents a one-digit prefix from becoming, for example, `03`; all non-leading pairs keep their zero padding.

The same idea can be continued with groups of four or eight digits. We first split the number in base $10^4$ or $10^8$, write the highest nonzero group normally, and write every following group at a fixed width. Fixed-width groups are convenient because their internal digits can be generated independently.

There is a tradeoff. A table with all two-digit strings occupies 200 bytes and is effectively free after the first access. A table with all five-digit strings already needs roughly half a megabyte. It may remove arithmetic only to add a cache miss. Small tables and a little arithmetic usually make a better compromise.

## Writing Forward

Writing backwards requires a copy from the temporary buffer. Another approach is to determine the decimal length first and fill the final range from right to left.

The tempting formula

$$
\lfloor \log_{10} x \rfloor + 1
$$

is inconvenient because floating-point rounding can give a wrong answer near a power of ten. For 32-bit integers, a short table of thresholds is simpler:

```cpp
int digits10(unsigned x) {
    if (x < 10) return 1;
    if (x < 100) return 2;
    if (x < 1000) return 3;
    if (x < 10000) return 4;
    if (x < 100000) return 5;
    if (x < 1000000) return 6;
    if (x < 10000000) return 7;
    if (x < 100000000) return 8;
    if (x < 1000000000) return 9;
    return 10;
}
```

A production formatter normally turns this chain into a smaller decision tree or starts with the binary length and corrects the estimate with one comparison. Which version is faster depends on the input distribution: uniformly random 32-bit integers are almost always 9 or 10 digits long, while counters tend to spend most of their life in a much smaller range.

## Buffering

After integer conversion becomes fast, output itself is usually the larger problem. Calling `write`, `printf`, or even a buffered stream once per number adds bookkeeping that has nothing to do with decimal arithmetic.

The standard solution is to accumulate many numbers in one buffer:

```cpp
char output[1 << 16];
char *out = output;

for (int i = 0; i < n; i++) {
    if (output + sizeof output - out < 11) {
        fwrite(output, 1, out - output, stdout);
        out = output;
    }

    char temporary[10];
    char *end = temporary + 10;
    char *begin = write_u32(end, a[i]);

    int length = end - begin;
    memcpy(out, begin, length);
    out += length;
    *out++ = '\n';
}

fwrite(output, 1, out - output, stdout);
```

This is deliberately just the hot path: it assumes `stdout` remains writable and that the input values are unsigned. Real I/O code also has to handle short writes and errors, but those checks belong around the buffer flush, not inside the digit loop.

## SIMD

Computing decimal digits with SIMD is possible. The hard part is not division, but packing the results: eight integers generally produce eight strings of different lengths, and all of them have to be concatenated without gaps.

SIMD is much more attractive when the output width is fixed. Dates, timestamps, zero-padded identifiers, and fixed-width table columns already tell us where every digit must go. For general integers, scalar conversion with a small digit-pair table often leaves too little arithmetic for SIMD to save, especially after the cost of moving the variable-length results is included.

This distinction also matters in benchmarks. A program that computes a checksum of the digits measures decimal arithmetic; a program that stores the strings measures arithmetic and packing; a program that writes them to a file mostly measures buffering and I/O. These are three different experiments.

For ordinary C++ code, `std::to_chars` is a good reference implementation: it writes into caller-provided memory, performs no allocation, and does not involve the locale. If a custom formatter is supposed to be faster, compare the actual bytes against it around zero, every power of ten, and the largest value of the type.

The main optimization is less glamorous than a vector instruction: produce several digits per dependent step, and do not turn every small number into a separate I/O operation.
