---
title: Reading Decimal Integers
weight: 10
---

I wrote a new integer parsing algorithm that is ~35x faster than `scanf`.

(No, this is not an April Fools' joke — although it does sound ridiculous.)

Zen 2 @ 2GHz. The compiler is Clang 13.

That headline belongs to the original end-to-end experiment: it compares formatted input through `scanf` with a specialized parser that owns the buffering, conversion, and final XOR reduction. It is not the ratio measured by the in-memory Apple M4 benchmark later in this article, and it is not a promise that arbitrary uses of `scanf` can be replaced at the same speedup.

Ridiculous — so let us see where all that time goes.

## The Workload

The input consists of unsigned integers from $0$ through $10^8-1$, one per line, including a newline after the final value:

```text
7
31415926
0
42
```

The parser reads a known number of values and XORs them. The checksum makes the result observable without adding the cost of storing a large output array. Uniformly sampled numbers in this range occupy about 7.9 digits, or 8.9 bytes including the newline, on average.

This is a deliberately narrow format. There are no signs, spaces, empty lines, or integers longer than eight digits. These restrictions are not minor implementation details: later we will use every one of them. A general-purpose parser has to check its input; a benchmark parser is allowed to prove the checks unnecessary before it starts.

## Removing I/O Overhead

### Iostream

The most direct C++ implementation uses formatted streams:

```c++
uint32_t read(int n) {
    uint32_t checksum = 0;

    for (int i = 0; i < n; i++) {
        uint32_t x;
        std::cin >> x;
        checksum ^= x;
    }

    return checksum;
}
```

There is much more going on in `operator>>` than multiplying by ten. It constructs a sentry object, consults the locale, skips whichever characters the locale calls whitespace, handles signs and stream states, and then goes through the stream buffer. This is useful when the input really is formatted text, but all that machinery is redundant when the format is fixed in advance.

### Scanf

The C equivalent is shorter internally, but not fundamentally simpler:

```c++
uint32_t read(int n) {
    uint32_t checksum = 0;

    for (int i = 0; i < n; i++) {
        uint32_t x;
        scanf("%u", &x);
        checksum ^= x;
    }

    return checksum;
}
```

`scanf` has to interpret the format string and support the complete syntax of an unsigned integer. It was much faster than `std::cin` in the original experiment, but it still spent over a hundred nanoseconds doing work for each number.

### Synchronization

By default, the C++ streams are synchronized with the C standard I/O streams so that calls to `printf` and `std::cout` can be safely interleaved. We do not need this, and we do not need `std::cin` to flush `std::cout` before every input operation either:

```c++
std::ios_base::sync_with_stdio(false);
std::cin.tie(nullptr);
```

This made `std::cin` almost four times faster on this workload and, in that run, even faster than `scanf`. It still took over a hundred nanoseconds per number, so synchronization was only one layer of the overhead.

### Getchar

We can throw away formatted input altogether and implement the decimal recurrence ourselves:

$$
x_{i+1}=10x_i+d_i,
$$

where $d_i$ is the numeric value of the next character. For the exact benchmark format, the entire parser is just this:

```c++
uint32_t read(int n) {
    uint32_t checksum = 0;

    for (int i = 0; i < n; i++) {
        uint32_t x = 0;

        for (int c; (c = getchar_unlocked()) != '\n'; )
            x = 10 * x + (c - '0');

        checksum ^= x;
    }

    return checksum;
}
```

The ordinary `getchar` locks `stdin` on every call because another thread may be reading the same stream. `getchar_unlocked` is the POSIX version without that lock. It is safe here because there is only one reader.

This kernel is correct only under the contract above. In particular, it does not treat `EOF` as a delimiter: an input with a missing final newline would never finish. It also omits an overflow check because an eight-digit number cannot overflow a 32-bit unsigned integer. If either property is not known beforehand, it must be checked rather than hoped for.

### Buffering

Removing the lock still leaves one function call and stream-buffer check per byte. We can amortize both by asking `fread` for a large block and walking over it ourselves.

Here is a checked version. It accepts a newline or the end of the file after the last value, rejects empty lines and all other characters, and detects overflow before performing it:

```c++
bool read(int n, uint32_t &checksum) {
    const int B = 1 << 14;
    char buffer[B];

    checksum = 0;
    uint32_t x = 0;
    bool have_digit = false;

    while (n > 0) {
        size_t length = fread(buffer, 1, B, stdin);

        if (length == 0) {
            if (ferror(stdin))
                return false;
            if (have_digit && n == 1) {
                checksum ^= x;
                return true;
            }
            return false;
        }

        for (size_t i = 0; i < length; i++) {
            unsigned c = (unsigned char) buffer[i];

            if (c >= '0' && c <= '9') {
                unsigned digit = c - '0';
                if (x > (UINT32_MAX - digit) / 10)
                    return false;
                x = 10 * x + digit;
                have_digit = true;
            } else if (c == '\n' && have_digit) {
                checksum ^= x;
                x = 0;
                have_digit = false;
                if (--n == 0)
                    return true;
            } else {
                return false;
            }
        }
    }

    return true;
}
```

The accumulator lives across buffer refills, so an integer split between two blocks is handled normally. In the specialized benchmark, where every integer is already known to have at most eight digits, the overflow branch can be removed from the hot loop. A 16 KiB buffer is enough to amortize the I/O bookkeeping; after that, decimal conversion becomes the interesting part.

The checked-in benchmark starts at this boundary. It receives an already-buffered byte range, so disk access, system calls, locale processing, and buffer refills are outside its timed interface. Mixing those costs into the following graph would attribute I/O improvements to arithmetic.

The [complete test and benchmark program](../../../code/reading_integers_bench.cpp) generates three fixed-seed datasets of $2^{20}$ values. The checked-in [five-process median data](../../../code/reading_integers_m4_results.txt) and [Matplotlib script](../../../code/plot_reading_integers.py) reproduce the figures:

- uniform numbers in $[0,10^8)$, averaging 8.89 bytes with the newline;
- exactly eight digits, taking nine bytes per record;
- an equal number of every decimal length from one through eight, averaging 5.5 bytes.

These measurements were taken on an Apple M4 Max with Apple Clang 17.0.0 using `-O3 -mcpu=native`. Each process performs two warm-up runs followed by nine timed runs; the table reports the componentwise median of five processes. Allocation, generation, and partitioning are outside the timed region. Non-AArch64 builds omit both NEON rows rather than relabeling scalar fallbacks.

The direct in-memory Horner loop takes **12.75 ns per integer**, or **1.43 ns per byte**, on uniform numbers. The multiply itself is cheap—the compiler uses a multiply-add—but every digit depends on the previous value of `x`. One record provides only one arithmetic dependency chain. For comparison, `std::from_chars` takes 6.71 ns per integer on the same range; it has a broader interface, reports errors and the stopping pointer, and serves as the correctness oracle.

## Parallelizing Decimal Conversion

### SIMD

The Horner recurrence is not the only way to evaluate eight digits. For

$$
\overline{abcdefgh},
$$

we can first form four two-digit values, then two four-digit values, and finally one eight-digit value:

$$
\begin{aligned}
p_0 &= 10a+b, & p_1 &= 10c+d,\\
p_2 &= 10e+f, & p_3 &= 10g+h,\\
q_0 &= 100p_0+p_1, & q_1 &= 100p_2+p_3,\\
x &= 10000q_0+q_1.
\end{aligned}
$$

The original x86 version performs this reduction with the SSSE3 pairwise multiply-add instructions. The measured Arm version first loads 16 readable bytes, finds the first newline, and uses `tbl` to right-align one through eight digits with leading zeroes. Its arithmetic is the same tree expressed with NEON widening multiplies and pairwise additions:

```c++
uint16x8_t products2 = vmull_u8(digits, weights10);
uint16x4_t pairs = vpadd_u16(
    vget_low_u16(products2), vget_high_u16(products2)
);

uint32x4_t products4 = vmull_u16(pairs, weights100);
uint32x2_t quads = vpadd_u32(
    vget_low_u32(products4), vget_high_u32(products4)
);

uint64x2_t products8 = vmull_u32(quads, weights10000);
uint32_t value = uint32_t(vaddvq_u64(products8));
```

The exact newline-mask reduction, shuffle tables, loop bound, and scalar tail are in the linked program. A vector load is issued only when 16 bytes remain addressable. Readable padding may make an over-read legal, but those bytes are not input and must never be committed as a value.

This pairwise reduction is also explained in Wojciech Mu&#322;a's note on [parsing decimal numbers with SIMD](http://0x80.pl/notesen/2014-10-12-parsing-decimal-numbers-part-1-swar.html).

### Serial

Reducing the digits with SIMD does not make the parser itself parallel. It still finds one newline, converts one number, and only then learns the address of the next load:

```c++
while (p + 16 <= end) {
    uint32_t x;
    const char *next = parse_8(p, x);
    if (next == nullptr)
        break;
    checksum ^= x;
    p = next;
}
```

The critical path is now load, compare, mask reduction, first-set-bit search, shuffle selection, and pointer update. A 16-byte register often contains parts of two or three integers, but this loop retires only the first one.

On the M4, the serial NEON tree takes **6.43 ns per uniform integer**, about twice as fast as scalar Horner and slightly faster than `from_chars`. It improves to **3.88 ns** on fixed eight-digit records, while the variable-length dataset takes 6.32 ns. Locating the newline and selecting a shuffle costs almost as much for a one-digit record as for an eight-digit record.

### Transpose-Based Approach

The original x86 experiment obtained independent work by splitting a large buffer into eight long streams. Taking one byte from each stream turns eight serial recurrences into the eight 32-bit lanes of an AVX2 register:

```text
stream 0:  1 2 3 \n 7 \n ...
stream 1:  9 \n 4 2 \n ...
stream 2:  5 6 \n 8 \n ...
             ...

step 0:    [1 9 5 ...]
step 1:    [2 \n 6 ...]
step 2:    [3 4 \n ...]
```

Each lane owns an ordinary decimal accumulator. A digit advances it; a newline contributes the completed value to the checksum and resets it:

```c++
typedef __m256i vec;

// The low 8 bytes contain one character from each of 8 streams.
void update_8(__m128i chars, vec &x, vec &checksum) {
    vec c = _mm256_cvtepu8_epi32(chars);

    const vec ascii_zero = _mm256_set1_epi32('0');
    const vec zero = _mm256_setzero_si256();
    const vec ten = _mm256_set1_epi32(10);

    vec separator = _mm256_cmpgt_epi32(ascii_zero, c);
    vec digit = _mm256_sub_epi32(c, ascii_zero);
    vec next = _mm256_add_epi32(_mm256_mullo_epi32(x, ten), digit);

    checksum = _mm256_xor_si256(
        checksum,
        _mm256_and_si256(x, separator)
    );
    x = _mm256_blendv_epi8(next, zero, separator);
}
```

There is no useful byte gather on AVX2, so loading eight individual characters would give much of the gain back. Instead, the parser loads a contiguous block from every stream and transposes the resulting byte tile with a network of unpack instructions. The columns of that tile are exactly the `chars` arguments expected by `update_8`.

The streams must begin and end between records. One approach moves every tentative cut to the next newline and parses the short unequal tails separately. The original XOR benchmark could instead keep equal partitions and repair each split record: XOR the erroneous suffix a second time to cancel it, then XOR the correctly reparsed whole value. That repair relies on XOR being its own inverse. An ordered output—or any aggregation that cannot undo a suffix—must align the cuts or merge stored lane outputs.

Here is one representative run from that end-to-end Zen 2 experiment, using $10^8$ uniformly random integers:

```text
implementation          ns / integer    cycles / byte
---------------------   ------------    -------------
iostream                    430.79            96.93
scanf                       166.37            37.43
iostream, no sync           109.70            24.68
getchar                      44.88            10.10
getchar_unlocked             26.84             6.04
buffered                     14.28             3.21
serial SIMD                  10.07             2.26
transpose SIMD                5.35             1.20
```

The exact ratio moved with the standard library, kernel, compiler, and random sample; this run is about 31 times faster than `scanf`, while the headline run was about 35 times faster. The shape matters more than either number: removing formatted-input overhead buys the first order of magnitude, buffering roughly halves what remains, and parallelizing the recurrence supplies the last large gain.

There is one huge asterisk. The kernel XOR-reduces the integers because this is the cheapest way to verify the benchmark. Every active lane does contain an actual parsed integer, but partitioning produces several ordered subsequences, not one ordered output sequence. Merging them adds memory traffic and bookkeeping. The headline applies to this reduction benchmark, not to every possible `scanf` replacement.

The current M4 harness does not contain a byte-transpose network, so it makes no current throughput claim for this stage. A transpose only earns a new data point when its shuffles, boundary policy, and cleanup path are all in the timed implementation.

### Instruction-Level Parallelism

The same splitting trick helps even without SIMD. We move four cuts to record boundaries and keep four scalar states:

```c++
while (p0 != e0 && p1 != e1 && p2 != e2 && p3 != e3) {
    update(*p0++, x0, s0);
    update(*p1++, x1, s1);
    update(*p2++, x2, s2);
    update(*p3++, x3, s3);
}
```

Each `update` is the scalar digit-or-newline operation. The four multiplies do not depend on one another, so the out-of-order scheduler can overlap them. Short unequal tails are parsed after the main loop, and because every cut lies between records, there is no split value to repair.

The result is **3.76 ns per integer** on uniform numbers, or 0.423 ns per input byte. That is 3.40 times faster than the scalar in-memory baseline and 1.79 times faster than this standard library's `from_chars`.

![](../img/reading-integers-stages.svg)

Packing these four accumulators into the four 32-bit lanes of a NEON register looks like the natural final step:

```c++
uint32x4_t separator = vceqq_u32(chars, newline);
uint32x4_t digit = vsubq_u32(chars, ascii_zero);
uint32x4_t next = vmlaq_n_u32(digit, value, 10);

checksum = veorq_u32(checksum, vandq_u32(value, separator));
value = vbslq_u32(separator, vdupq_n_u32(0), next);
```

The arithmetic becomes compact—Clang emits four-lane `mla`—but the machine has no byte gather. We still load four separate characters and pack them into `chars` on every iteration. This version regresses from 3.76 to **3.87 ns per uniform integer**. Vectorizing the cheap part did not remove the scattered loads.

## Record Length and Cache Size

![](../img/reading-integers-distributions.svg)

The complete in-memory measurements are:

| Implementation | Uniform numbers | Fixed 8 digits | Uniform length |
|:--|--:|--:|--:|
| scalar Horner | 12.75 ns | 13.00 ns | 7.95 ns |
| serial NEON tree | 6.43 ns | 3.88 ns | 6.32 ns |
| four scalar streams | **3.76 ns** | **3.79 ns** | **2.30 ns** |
| four NEON lanes | 3.87 ns | 3.98 ns | 2.42 ns |
| `std::from_chars` | 6.71 ns | 6.14 ns | 5.22 ns |

The four-stream parser does almost the same work per byte on all three distributions: 0.423, 0.421, and 0.419 ns. Its nanoseconds per integer change mostly because the records have different lengths.

We also swept fixed-width input from 1 KiB through 72 MiB. The performance-core L1 data cache is 128 KiB and the shared-per-cluster L2 cache is 16 MiB.

![](../img/reading-integers-size.svg)

Neither curve changes materially at a capacity boundary. Both kernels make one sequential pass with no data reuse, so their bottleneck is dependency and instruction throughput rather than cache capacity. The four-stream version remains near 0.42 ns per byte even after the input exceeds L2.

## Correctness and Limitations

The test mode checks decimal boundaries, irregular partition sizes, 200 fixed-seed random datasets, and every in-memory kernel against `std::from_chars`. AddressSanitizer and UndefinedBehaviorSanitizer verify the vector load bounds and tail handling.

The measured M4 contract still assumes valid digits, values below $10^8$, a newline after every record, and an XOR aggregation. Supporting signs, overflow detection, arbitrary delimiters, a missing final newline, or ordered output adds work and must be measured rather than described as free.

The result is narrow but reproducible end to end: the winning loop, partitioning, tails, tests, compiler command, and plots all correspond to the same checked-in program.

### Future Work

Next time, we will be *writing* integers.

A complete transpose-based implementation for the current benchmark remains an interesting continuation. The same pairwise multiply-add tree also applies when a byte stream carries other small symbols with fixed positional weights.

Multi-stream recurrences can also compute Rabin–Karp hashes in parallel. Such hashes are candidate filters, not proofs of equality: an exact string-searching algorithm must verify every reported match.

## Acknowledgements

The pairwise conversion follows Wojciech Mu&#322;a's articles on [parsing decimal numbers with SIMD](http://0x80.pl/notesen/2014-10-12-parsing-decimal-numbers-part-1-swar.html) and [parsing integer sequences with SIMD](http://0x80.pl/articles/simd-parsing-int-sequences.html). The byte-transpose discussion is based on Peter Cordes' explanation of [an AVX2 8-by-8 transpose](https://stackoverflow.com/questions/25622745/transpose-an-8x8-float-using-avx-avx2/25627536#25627536). The original experimental programs are in the [`parsing` directory](https://github.com/sslotin/amh-code/tree/main/parsing) of the companion repository; the current Arm benchmark is linked above.
