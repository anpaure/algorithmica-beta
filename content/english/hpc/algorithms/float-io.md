---
title: Reading and Writing Floating-Point Numbers
weight: 12
---

Reading an integer is a sequence of multiply-adds. Reading a floating-point number is a sequence of multiply-adds followed by a numerical-analysis problem.

The decimal number `0.1` is exactly $1/10$, but no finite binary fraction is equal to it. A correct parser has to choose the nearer of two adjacent binary values. A shortest formatter solves the reverse problem: it has to find the shortest decimal string that maps back to exactly the same binary value.

General algorithms such as Eisel–Lemire, Ryu, and Dragonbox are large because their contracts are large: signs, variable significands, exponents, subnormals, overflow, underflow, ties, and exact fallback paths all matter. We will not imitate them with an approximate multiply by a power of ten.

Instead, this case study adopts a fixed-point format narrow enough to implement and prove from scratch, then compares it honestly with the general C library.

## The Contract

Every token is exactly thirteen bytes:

```text
123456.789012
```

There are six integer digits, a decimal point, and six fractional digits. The integer part is between 100000 and 999999, so leading signs, zero padding, exponents, special values, and variable lengths do not occur. Parsing returns the correctly rounded binary64 value. Formatting accepts a value produced by this parser and recreates the same thirteen bytes.

The [complete program](../../../code/float_io_bench.cpp) generates $2^{20}$ fixed-seed tokens. It samples a twelve-digit scaled integer

$$
N \in [100000000000,999999999999]
$$

and inserts the decimal point six digits from the right. The [raw measurements](../../../code/float_io_m4_results.txt) contain the five-process aggregate, and the [Matplotlib script](../../../code/plot_float_io.py) turns them into the three figures below.

Measurements were taken on an Apple M4 Max with Apple Clang 17.0.0 using `-O3 -mcpu=native`, in the default C locale and rounding mode. Each process performs two warm-up runs followed by nine timed runs; the tables report the componentwise median of five independent processes.

The benchmark times a material conversion, not a loop-carried checksum. Every parser writes every binary64 result to a preallocated output array while the clock is running; a zero-instruction compiler fence keeps repeated small-array passes observable without serializing the processor. After the clock stops, the harness hashes the bit pattern of every output. Every formatter similarly writes the complete character range inside timing and has its full-buffer checksum computed afterward. Input generation, allocation, validation, and both checksum passes are outside the timed region.

## General Parsing Baseline

`strtod` expects a zero-terminated string, so the most isolated baseline copies each token to a fourteen-byte local buffer, appends zero, and calls it:

```c++
char token[14];
memcpy(token, input, 13);
token[13] = 0;

char *end;
double value = strtod(token, &end);
```

This takes **12.60 ns per value**. Calling `strtod` directly on the original range—where it stops at the newline and a zero still exists at the end of the complete allocation—takes 12.56 ns. Removing the small copy buys less than 0.3%; token movement is not the main bottleneck.

The library still has to recognize a general decimal grammar, consult its locale-dependent rules, determine the decimal exponent, and guarantee correct rounding. Our format has already ruled almost all of that work out.

Floating-point `std::from_chars` is not implemented by the libc++ shipped with this Apple Clang 17 installation, so it is not included under a misleading label. Availability and performance are properties of the standard-library version, not just the language mode.

## An Exact Fixed-Point Parser

Remove the point and parse the twelve digits as an integer:

```c++
uint64_t scaled = 0;

for (int i = 0; i < 13; i++)
    if (i != 6)
        scaled = 10 * scaled + (input[i] - '0');

double value = double(scaled) / 1000000.0;
```

This conversion is correctly rounded on the target for a simple reason. The largest `scaled` value is below $10^{12}<2^{40}$, so binary64 represents it exactly; it also represents the integer 1,000,000 exactly. The final hardware division is therefore one correctly rounded operation on the exact rational number $N/10^6$. Clang emits `fdiv`, not multiplication by an approximate reciprocal.

The scalar specialized parser takes **2.38 ns per value**, 5.29 times faster than the copied `strtod` baseline.

### The Tempting Wrong Optimization

Replacing division with multiplication looks harmless:

```c++
double value = double(scaled) * 0.000001;
```

It takes 2.29 ns, only about 4% less than exact scalar division. But the decimal constant `0.000001` is already rounded before the multiplication. In the benchmark corpus, **317,641 of 1,048,576 results—30.3%—differ by one bit or more** from `strtod` and the exact division.

The approximation is excluded from the speedup plot. A fast parser that silently changes almost one third of its outputs is not an optimization of the same function.

## Reducing the Digits with NEON

The twelve digit positions are fixed. We load sixteen addressable bytes, use `tbl` to remove the decimal point, and reduce the digits in a tree. Four leading digits form one integer and the remaining eight form another:

```c++
uint8x16_t ascii = vld1q_u8((const uint8_t*) input);
uint8x16_t numeric = vsubq_u8(ascii, vdupq_n_u8('0'));
uint8x16_t digits = vqtbl1q_u8(numeric, gather_indices);

uint32_t high = convert4(vget_low_u8(digits));
uint8x16_t shifted = vextq_u8(digits, digits, 4);
uint32_t low = convert8(vget_low_u8(shifted));

uint64_t scaled = uint64_t(high) * 100000000 + low;
double value = double(scaled) / 1000000.0;
```

`convert4` and `convert8` use widening multiplies with weights 10, 100, and 10000 followed by pairwise additions. The generated core contains `tbl.16b`, `umull`, and the same final `fdiv`. The last load needs two addressable padding bytes after the final record; the harness supplies sixteen, none of which belong to the logical token.

The NEON parser takes **1.13 ns per value**, another 2.11-fold improvement and **11.2 times faster than `strtod` plus copying**.

![](../img/float-io-parsing.svg)

This is not a general floating-point parser hidden behind a short code sample. It is an exact parser for one fixed-point domain. A sign, a seventh fractional digit, or an exponent violates its precondition and must be handled by a different path.

The generated inner loops also explain what changed. Clang completely unrolls the scalar digit loop into eleven dependent `madd` instructions, followed by `ucvtf`, `fdiv`, and the output store. The NEON loop replaces that serial decimal recurrence with `tbl`, widening `umull` operations, and pairwise reductions; it still ends with the same conversion, division, and store. We shortened the digit dependency chain rather than approximating the numerical step.

The NEON variant exists only in AArch64 builds. On another target the benchmark omits it at compile time instead of silently running the scalar parser under a SIMD label.

## Scaling with the Working Set

The main input occupies 14 bytes per value, including the newline, and the materialized result occupies another 8. The size sweep therefore crosses the 128 KiB L1 data cache between $2^{12}$ and $2^{13}$ values and the 16 MiB cluster L2 between $2^{19}$ and $2^{20}$:

![Fixed-format parsing by working-set size](../img/float-io-size.svg)

The scalar parser stays between 2.36 and 2.41 ns per value; the NEON parser stays between 1.14 and 1.15 ns. Neither cache boundary produces a step. At the largest point the NEON kernel streams 22 bytes per value at about 19.4 GB/s, and its time is still almost identical to the in-cache points. The dependency chain and arithmetic are the limiting resources here, not capacity misses.

## Fixed-Precision Formatting

The reverse contract is also narrower than shortest formatting: print exactly six digits on both sides of the point. The general baseline is

```c++
snprintf(output, 14, "%.6f", value);
```

which takes **126.36 ns per value**.

For values produced by our parser, we can recover the scaled integer:

```c++
uint64_t scaled = llround(value * 1000000.0);
```

This is exact over the stated range. Near $10^6$, one binary64 ulp is below $1.2\cdot10^{-10}$; after multiplication by $10^6$, the original parsing error remains far below one half. Rounding therefore recovers the original $N$.

The direct formatter repeatedly divides `scaled` by 10, writes twelve digits from right to left, and inserts the point at the known position. It takes **3.74 ns**, 33.8 times faster than `snprintf`.

There are only one hundred digit pairs. Using the same 200-byte table as the [integer formatter](../writing-integers), we divide by 100 six times and copy two digits per step:

```c++
for (int position : {11, 9, 7, 4, 2, 0}) {
    uint64_t q = scaled / 100;
    unsigned r = scaled - q * 100;
    memcpy(output + position, digit_pairs + 2 * r, 2);
    scaled = q;
}
output[6] = '.';
```

This final version takes **1.86 ns per value**, **67.9 times faster than `snprintf`** on the fixed format.

![](../img/float-io-formatting.svg)

## Correctness

The test mode generates one million independent tokens and then checks ten explicit boundaries: the minimum and maximum accepted values, their immediate neighbors, and carries across the decimal point near both ends of the range. Every exact custom parse is compared bit-for-bit with `strtod`; every formatter is compared byte-for-byte with the original token and with `snprintf`; and formatting after parsing must recover the original scaled integer. The same suite runs under AddressSanitizer and UndefinedBehaviorSanitizer.

The exactness argument is specific to the range. If $N$ no longer fits exactly in binary64, or if the denominator is introduced as an already-rounded binary approximation, the one-division proof fails. The harness includes the reciprocal multiplication specifically to demonstrate that failure.

The stored data and all three plots can be reproduced from the repository root:

```bash
clang++ -std=c++20 -O3 -mcpu=native \
  -Wall -Wextra -Wpedantic -Werror \
  static/code/float_io_bench.cpp -o float-io
./float-io test
python3 scripts/median-csv.py --runs 5 \
  --output static/code/float_io_m4_results.txt -- ./float-io bench
python3 static/code/plot_float_io.py \
  static/code/float_io_m4_results.txt \
  content/english/hpc/algorithms/img
```

## Final Comparison

| Parsing implementation | ns / value | Speedup | Correct over this contract |
|:--|--:|--:|:--:|
| `strtod`, copied and terminated | 12.60 | 1.00x | yes |
| `strtod`, original allocation | 12.56 | 1.00x | yes |
| fixed scalar digits + division | 2.38 | 5.29x | yes |
| fixed NEON digits + division | **1.13** | **11.2x** | yes |
| scalar digits + reciprocal multiply | 2.29 | 5.51x | **no** |

| Formatting implementation | ns / value | Speedup |
|:--|--:|--:|
| `snprintf("%.6f")` | 126.36 | 1.00x |
| fixed digits, `/10` | 3.74 | 33.8x |
| fixed digit pairs, `/100` | **1.86** | **67.9x** |

General conversion remains the harder and more important problem. Use a reviewed `from_chars`/`to_chars`, Ryu, Dragonbox, or Eisel–Lemire implementation when the input grammar is general. Specialization is justified only when the fixed format is a real interface, its proof is written down, and invalid inputs are rejected before entering the kernel.

## Further Reading

- Daniel Lemire, [*Number Parsing at a Gigabyte per Second*](https://arxiv.org/abs/2101.11408), describes the Eisel–Lemire parsing method.
- Ulf Adams, [*Ryu: Fast Float-to-String Conversion*](https://dl.acm.org/doi/10.1145/3192366.3192369), derives a fixed-width shortest formatter.
- William Clinger, [*How to Read Floating Point Numbers Accurately*](https://doi.org/10.1145/93542.93557), explains why exact fallback cases exist.
