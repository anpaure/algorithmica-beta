---
title: Reading and Writing Floating-Point Numbers
weight: 12
---

Reading an integer is a sequence of multiply-adds. Reading a floating-point number is a sequence of multiply-adds followed by a small numerical-analysis problem.

The decimal number `0.1` is exactly $1/10$, but no finite binary fraction is equal to it. A parser has to choose one of the two neighboring binary numbers, and a correct parser has to choose the nearer one. A formatter solves the reverse problem: it has to find a decimal number that will be rounded back to the same binary value.

This is why fast floating-point conversion is considerably harder than fast integer conversion. The loops are short; proving that their last bit is correct is not.

## The Conversion Problem

Ignoring special values for a moment, a decimal token has the form

$$
(-1)^s M \cdot 10^q,
$$

where $M$ is the integer formed by removing the decimal point and $q$ includes both the position of the point and the explicit exponent.

For example,

$$
1.234 \cdot 10^5 = 1234 \cdot 10^2.
$$

Scanning the sign, digits, decimal point, and exponent is not difficult. The difficult operation is converting $M\cdot10^q$ to the nearest representable number without first constructing that exact, potentially enormous, rational number.

A naive parser may look like this:

```cpp
double x = 0;

while (*s >= '0' && *s <= '9')
    x = 10 * x + (*s++ - '0');

if (*s == '.') {
    double p = 0.1;
    while (*++s >= '0' && *s <= '9') {
        x += p * (*s - '0');
        p *= 0.1;
    }
}
```

Apart from not handling signs and exponents, this code rounds after almost every arithmetic operation. The final value is close, but it is not guaranteed to be the correctly rounded value of the input. Replacing the last loop with a call to `pow` does not fix this; it only makes the approximate computation more expensive.

## A Fast Common Case

Fast parsers first collect digits in an integer. As long as the significand fits into 64 bits, this part is almost the same as [integer parsing](../reading-integers):

```cpp
uint64_t m = 0;

while (*s >= '0' && *s <= '9')
    m = 10 * m + (*s++ - '0');
```

Digits after the decimal point are accumulated in the same integer while decreasing the decimal exponent. If more digits arrive than fit, the parser keeps the useful prefix and remembers whether any discarded digit was nonzero. This last bit of information can decide which way a midpoint is rounded.

The conversion then uses

$$
10^q = 5^q 2^q.
$$

Multiplication by $2^q$ mostly changes the binary exponent. The remaining factor $5^q$ can be taken from a table and multiplied with $M$ using a wide integer product. From the high bits of that product, the parser obtains a candidate binary significand.

For most short inputs with moderate exponents, the known error bounds prove that this candidate is unambiguous. Inputs extremely close to a rounding boundary need a slower path with more precision. This is the important structure of algorithms such as Eisel–Lemire:

1. parse the syntax and a bounded integer significand;
2. multiply by a cached power using wide integer arithmetic;
3. accept the result when the error interval fits inside one rounding interval;
4. fall back to an exact method otherwise.

The last step may be rare, but it is part of the algorithm. Removing it creates a parser that is fast on benchmarks and wrong on carefully chosen decimals.

## Writing the Shortest Number

A binary floating-point value can be written as

$$
v = m \cdot 2^e.
$$

There are infinitely many decimal strings that round to $v$. For example, printing more and more trailing digits eventually stops adding information. The usual default format is therefore the *shortest round-tripping representation*: the shortest decimal string which a correct parser maps back to exactly $v$.

Every finite $v$ owns a rounding interval bounded by the midpoints to the previous and next representable values. Formatting becomes the following problem:

> Find the shortest decimal number inside that interval.

Modern algorithms such as Ryu and Dragonbox transform the interval with precomputed powers of five and use fixed-width integer arithmetic to generate its decimal digits. They stop as soon as the remaining interval contains a unique shortest candidate. The details around powers of two, subnormal values, and ties are exactly where most home-grown formatters fail.

Requested-precision formatting is a different problem. `%.2f` asks for rounding to two digits after the decimal point, not for a round trip. Printing `max_digits10` significant digits is sufficient for a round trip, but it is usually not the shortest representation.

## The C++ Interface

The locale-independent C++ interface is deliberately small. To read one token from a known byte range:

```cpp
double x;
auto [end, error] = std::from_chars(first, last, x);

if (error != std::errc() || end != last)
    /* invalid token */;
```

Checking `end` matters: otherwise a token such as `1.5ms` is silently accepted as the valid prefix `1.5`.

Writing is symmetric:

```cpp
char buffer[64];
auto [end, error] = std::to_chars(buffer, buffer + 64, x);

if (error == std::errc())
    fwrite(buffer, 1, end - buffer, stdout);
```

These functions do not allocate, do not require a terminating zero, and do not consult the locale. Floating-point overloads arrived later than the integer ones in some standard libraries, so both availability and speed depend on the library version; it is part of any benchmark result.

For a stream of numbers, conversion should operate on one large input buffer and write to one large output buffer. Copying every token to add `\0`, allocating one string per result, or making one system call per number can easily cost more than the conversion itself.

## Testing

Uniformly generated decimal strings mostly test the easy path. A useful test set also includes

- positive and negative zero;
- the smallest and largest subnormal numbers;
- the smallest normal and largest finite numbers;
- values adjacent to powers of two and ten;
- very long inputs whose discarded suffix changes rounding;
- decimal numbers exactly on both sides of a midpoint;
- exponent overflow and underflow;
- every malformed form the parser promises to reject.

For a formatter, generate floating-point values by choosing their bit patterns, write them, read them back with an independent implementation, and compare the bits. Testing a formatter and parser only against each other is insufficient: two implementations with the same bug can round-trip perfectly.

Unless floating-point conversion is demonstrably a bottleneck, use a reviewed implementation of `from_chars` and `to_chars`. If it is a bottleneck, the optimization target is still not "approximately parse the common inputs." It is "prove that the common input is easy, and keep an exact path for everything else."

## Further Reading

- Daniel Lemire, [*Number Parsing at a Gigabyte per Second*](https://arxiv.org/abs/2101.11408), describes the Eisel–Lemire parsing method.
- Ulf Adams, [*Ryu: Fast Float-to-String Conversion*](https://dl.acm.org/doi/10.1145/3192366.3192369), derives a fixed-width shortest formatter.
- William Clinger, [*How to Read Floating Point Numbers Accurately*](https://doi.org/10.1145/93542.93557), explains the exact slow cases.
