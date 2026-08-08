---
title: Reading Decimal Integers
weight: 10
---

I wrote a new integer parsing algorithm that is ~35x faster than scanf.

(No, this is not an April Fools' joke — although it does sound ridiculous.)

Zen 2 @ 2GHz. The compiler is Clang 13.

Ridiculous — so let us see where all that time goes.

Before we start optimizing, we need to decide what exactly we are parsing. In the benchmark, the input consists of unsigned integers from $0$ to $10^8-1$, one per line, and the last integer is followed by a newline too:

```
7
31415926
0
42
```

The parser reads a known number of values and xor-sums them. The checksum makes the result observable without adding the cost of storing a large output array. Uniformly sampled numbers in this range occupy about 7.9 digits, or 8.9 bytes including the newline, on average.

This is a deliberately narrow format. There are no signs, spaces, empty lines, or integers longer than eight digits. These restrictions are not minor implementation details: later we will use every one of them. A general-purpose parser has to check its input; a benchmark parser is allowed to prove the checks unnecessary before it starts.

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

`scanf` has to interpret the format string and support the complete syntax of an unsigned integer. It was much faster than `std::cin` in this experiment, but it still spent over a hundred nanoseconds doing work for each number.

### Synchronization

By default, the C++ streams are synchronized with the C standard I/O streams so that calls to `printf` and `std::cout` can be safely interleaved. We do not need this, and we do not need `std::cin` to flush `std::cout` before every input operation either:

```c++
std::ios_base::sync_with_stdio(false);
std::cin.tie(nullptr);
```

This makes `std::cin` almost four times faster on this workload and, in this run, even faster than `scanf`. It still takes over a hundred nanoseconds per number, so synchronization was only one layer of the overhead.

### Getchar

We can throw away formatted input altogether and implement the decimal recurrence ourselves:

$$
x_{i+1} = 10x_i + d_i.
$$

Here $d_i$ is the numeric value of the next character, computed in C++ as `c - '0'`.

For the exact benchmark format, the entire parser is just this:

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

The accumulator lives across buffer refills, so an integer split between two blocks is handled normally. For the benchmark, where every integer is already known to have at most eight digits, we remove the overflow branch from the hot loop. The compiler also inlines the loop body, leaving roughly a load, a digit check, and a multiply-add per byte.

At this point input is no longer the expensive part. A 16 KiB buffer is large enough to amortize the `fread` bookkeeping and small enough to stay in cache; increasing it further has almost no effect.

### SIMD

The recurrence is serial within one integer: the next digit needs the value produced for the previous one. Decimal conversion can still be reorganized as a reduction tree. For an eight-digit number $\overline{abcdefgh}$,

$$
\begin{aligned}
\overline{abcdefgh}
    &= 10^6(10a+b) + 10^4(10c+d) \\
    &\quad + 10^2(10e+f) + (10g+h).
\end{aligned}
$$

After forming four two-digit numbers, we form two four-digit numbers and then one eight-digit number. SSSE3 provides the first pairwise multiply-add, and SSE4.1 provides the narrowing step needed before the last one:

```c++
typedef __m128i reg;

// The low 8 bytes contain 8 digits, zero-padded on the left.
uint32_t convert_8(reg x) {
    const reg mul10 = _mm_set1_epi16((1 << 8) + 10);
    x = _mm_maddubs_epi16(x, mul10);

    const reg mul100 = _mm_set1_epi32((1 << 16) + 100);
    x = _mm_madd_epi16(x, mul100);

    x = _mm_packus_epi32(x, x);

    const reg mul10000 = _mm_set1_epi32((1 << 16) + 10000);
    x = _mm_madd_epi16(x, mul10000);

    return (uint32_t) _mm_cvtsi128_si32(x);
}
```

The input starts as ASCII and has a variable length, so we still need to find the newline, subtract `'0'`, and right-align the digits before calling `convert_8`:

```c++
// Requires 16 readable bytes at p. Accepts 1..8 digits and a newline.
const char *parse_8(const char *p, uint32_t &value) {
    reg c = _mm_loadu_si128((const reg*) p);
    reg ge0 = _mm_cmpgt_epi8(c, _mm_set1_epi8('0' - 1));
    reg le9 = _mm_cmpgt_epi8(_mm_set1_epi8('9' + 1), c);
    unsigned digit_mask = (unsigned) _mm_movemask_epi8(
        _mm_and_si128(ge0, le9)
    );
    unsigned stops = (~digit_mask) & 0xffffu;

    if (stops == 0)
        return 0;

    int length = __builtin_ctz(stops);
    if (length == 0 || length > 8 || p[length] != '\n')
        return 0;

    reg digits = _mm_sub_epi8(c, _mm_set1_epi8('0'));
    reg shift = _mm_cvtsi32_si128(8 * (8 - length));
    digits = _mm_sll_epi64(digits, shift);

    value = convert_8(digits);
    return p + length + 1;
}
```

The helper is only legal when 16 bytes starting at `p` are addressable. The loop below stops early enough to guarantee this and leaves the remaining bytes to the scalar parser. Another valid design is to append readable padding, but those bytes are not part of the input and the scalar tail must still use the true buffer length. This is the same contract used by many high-performance string routines: over-reading addressable padding is fine; reading past the allocated object is not.

This pairwise reduction is also explained in Wojciech Mu&#322;a's note on [parsing decimal numbers with SIMD](http://0x80.pl/notesen/2014-10-12-parsing-decimal-numbers-part-1-swar.html).


### Serial

Using SIMD for the reduction removes most of the arithmetic, but `parse_8` still processes integers one at a time:

```c++
while (p + 16 <= end) {
    uint32_t x;
    const char *next = parse_8(p, x);
    if (next == 0)
        break;
    checksum ^= x;
    p = next;
}
```

The address of the next load depends on the position of the current newline. The critical path is now load, compare, `movemask`, `ctz`, variable shift, and pointer update. A 16-byte register often contains parts of two or three integers, but this loop only retires the first one. It is faster than the scalar digit loop, although not by the factor suggested by processing sixteen bytes at once.

### Transpose-based approach

To get independent work, split a large buffer into eight long streams and parse them concurrently. If we take one byte from each stream, the eight characters can be widened to the eight 32-bit lanes of an AVX2 register:

```
stream 0:  1 2 3 \n 7 \n ...
stream 1:  9 \n 4 2 \n ...
stream 2:  5 6 \n 8 \n ...
             ...

step 0:    [1 9 5 ...]
step 1:    [2 \n 6 ...]
step 2:    [3 4 \n ...]
```

Each lane now owns an ordinary decimal accumulator. A digit advances it; a newline contributes the completed value to the checksum and resets it:

```c++
typedef __m256i vec;

// The low 8 bytes contain one character from each of 8 streams.
// Every character is either a decimal digit or '\n'.
void update_8(__m128i chars, vec &x, vec &checksum) {
    vec c = _mm256_cvtepu8_epi32(chars);

    const vec zero_char = _mm256_set1_epi32('0');
    const vec zero = _mm256_setzero_si256();
    const vec ten = _mm256_set1_epi32(10);

    vec separator = _mm256_cmpgt_epi32(zero_char, c);
    vec digit = _mm256_sub_epi32(c, zero_char);
    vec next = _mm256_add_epi32(_mm256_mullo_epi32(x, ten), digit);

    checksum = _mm256_xor_si256(
        checksum,
        _mm256_and_si256(x, separator)
    );
    x = _mm256_blendv_epi8(next, zero, separator);
}
```

There is still only one multiplication per digit, but now there are eight independent multiplications in one instruction. More importantly, the dependency chain in one lane no longer prevents the other seven lanes from making progress.

The x86 instruction set has no useful byte gather, so loading the eight characters separately would give most of the speedup back. Instead, we load 32 consecutive bytes from every stream and transpose the resulting $8 \times 32$ byte tile with a network of `unpack` instructions. The columns of the transposed tile are exactly the `chars` arguments expected by `update_8`. The transpose looks expensive, but each loaded byte participates in only a few simple shuffles, all independent from decimal arithmetic.

The streams must begin and end between integers. The simplest solution is to place each tentative cut near one eighth of the buffer and move it to the next newline; the main loop processes the common part of all eight streams, and a scalar loop handles their short tails.

The benchmark implementation keeps the partitions equal and repairs every split record afterwards. Suppose a cut divides `123456\n` into `123|456\n`. The lane that begins at the cut emits the incorrect suffix `456`; the lane that ends there never emits its unfinished prefix `123`. The repair code xor-adds `456` once more, canceling the first copy, and then xor-adds the correctly reparsed `123456`. This trick relies on xor being its own inverse. For an ordered output, or for an aggregation that cannot undo a suffix, the cuts need to be aligned to newlines instead.

### Instruction-level parallelism

The same splitting trick helps even without SIMD. Four scalar states create four independent dependency chains that the out-of-order scheduler can interleave:

```c++
inline void update(unsigned char c, uint32_t &x, uint32_t &checksum) {
    if (c != '\n') {
        x = 10 * x + (c - '0');
    } else {
        checksum ^= x;
        x = 0;
    }
}

for (int i = 0; i < length; i++) {
    update(p0[i], x0, s0);
    update(p1[i], x1, s1);
    update(p2[i], x2, s2);
    update(p3[i], x3, s3);
}
```

This code has the same digit-or-newline precondition as `update_8`, and the four ranges must also start and end at record boundaries. It exposes more arithmetic parallelism, but it still executes four loads, four character tests, and roughly four times as many loop-body instructions as the vector version.


### Results and Limitations

Scalar instruction-level parallelism helps, but it still performs one copy of the parser for each stream. Transposition combines those streams into SIMD lanes and removes most of that duplicated work.

The measurements confirm this. Here is one run recorded by the original benchmark, with $10^8$ uniformly random integers from $0$ to $10^8-1$:

```
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

The last column uses the measured 2GHz clock and the average encoded length of the input. Exact numbers depend on the standard library, kernel, compiler, and input distribution, but the shape of the result is more important: removing formatted-input overhead buys the first order of magnitude, buffering roughly halves the remaining time, and parallelizing the recurrence buys the last large speedup.

There is one huge asterisk: the displayed kernel xor-reduces the integers because that is the cheapest way to verify the benchmark. We do get the integers, and we can run other computations on them — in every active lane of `separator`, the corresponding lane of `x` contains the actual parsed value. What we do *not* get for free is the original order: partitioning produces eight ordered subsequences, and merging them into one ordered output adds memory traffic and bookkeeping. The ~35x headline applies to the reduction benchmark, not to every possible `scanf` replacement.

One earlier end-to-end version, including transposition and boundary repair, ran at 1.75 cycles per byte. Later benchmark revisions got closer to the 1.20 cycles per byte in the last row of the table. At this point, the input distribution matters: shorter integers increase the frequency of separators, while long fixed-width records make the vector recurrence particularly efficient.

AVX-512 should improve the algorithm further, both because of the larger SIMD lane count and because mask instructions make filtering completed values cheaper. Sixteen 32-bit parser states fit in a register, but the structure of the algorithm does not change: transpose bytes into independent streams, update all Horner recurrences, and deal with the rare boundaries outside the hot loop.

The non-vectorized boundary and tail handling accounted for only ~2% of the measured running time, so elaborate optimization normally does not pay for itself. It can be reduced with special cleanup procedures or by padding the allocated buffer with arbitrary digits. Padding only makes a final vector load addressable: the logical end pointer must still stop the SIMD loop, and a partially accumulated value must only be committed when the real input ends. Treating padding as data would turn a memory-safety trick into a parsing bug.

### Future work

Next time, we will be *writing* integers.

The same pairwise multiply-add tree also works whenever a byte stream carries small symbols that need to be folded into a word. Decimal parsing is unusually convenient because the weights are fixed powers of ten, but the larger idea is to separate finding record boundaries from reducing the records.

The same multi-stream recurrence can create a string-searching algorithm by computing Rabin–Karp hashes in parallel. Unlike decimal conversion, though, a hash is allowed to collide, so this does not seem to yield an *exact* algorithm without an additional verification step.

## Acknowledgements

The pairwise SIMD conversion follows Wojciech Mu&#322;a's article on [parsing integer sequences with SIMD](http://0x80.pl/articles/simd-parsing-int-sequences.html). The byte-transpose network is based on Peter Cordes' explanation of [an AVX2 8-by-8 transpose](https://stackoverflow.com/questions/25622745/transpose-an-8x8-float-using-avx-avx2/25627536#25627536). The complete experimental programs and the benchmark notebook are available in the [`parsing` directory](https://github.com/sslotin/amh-code/tree/main/parsing) of the companion repository.
