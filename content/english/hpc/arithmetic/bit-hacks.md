---
title: Bit Manipulation
weight: 7
---

This article is largely based on [Bit Twiddling Hacks](https://graphics.stanford.edu/~seander/bithacks.html) by Sean Eron Anderson. Some methods were added, and some removed due to being solved by hardware. Most of these are already optimized by compilers.

A lot of it became obsolete once compilers learned these identities and processors gained instructions such as `popcnt`, `lzcnt`, and `cmov`. This is good news: we can write the operation we mean and let the compiler choose an implementation for the target architecture.

The remaining recipes are still useful for three reasons. Some represent genuinely word-parallel algorithms, some help us recognize compiler output, and all of them are exercises in stating the exact range and type for which an identity is true.

## Basic Operations

When the promoted type of an integer is unsigned, `x << k` shifts the bit pattern left and fills the low positions with zeroes; `x >> k` shifts it right and fills the high positions with zeroes. The shift count must be nonnegative and smaller than the width of the promoted left operand. Shifting a 32-bit value by 32 is not a clever way to obtain zero—it is undefined behavior.

Unsigned left shift is arithmetic modulo $2^w$ when the result is interpreted as an unsigned $w$-bit value. Narrow integer types are promoted before shifting, so an `unsigned char` may still be shifted as a signed `int`. Before C++20, left shift of a negative signed value was undefined and right shift was implementation-defined. Since C++20, left shift is defined by congruence modulo $2^w$, while right shift rounds toward negative infinity, which is the familiar *arithmetic shift* that copies the sign bit. Portable bit manipulation is still much easier when the bit pattern lives in an unsigned type.

A *rotation* moves the bits shifted out of one end back into the other. C++20 provides `std::rotl` and `std::rotr`; compilers recognize the usual rotate expression and use `rol` or `ror` when the target has them.

GCC and Clang expose common bit operations as builtins:

- `__builtin_popcount(x)` returns the number of set bits and normally maps to `popcnt` when available.
- `__builtin_parity(x)` returns that number modulo two, which is useful in simple [error-detecting codes](/hpc/number-theory/error-correction/).
- `__builtin_ffs(x)` returns one plus the index of the lowest set bit, or zero when `x` is zero.
- `__builtin_ctz(x)` and `__builtin_clz(x)` count trailing and leading zeroes. Their result is undefined when `x` is zero.
- `__builtin_clrsb(x)` counts the redundant leading sign bits of a signed integer, excluding the sign bit itself.

The suffix is part of the type: use `popcountll`, `ctzll`, and `clzll` for `unsigned long long`. C++20 standardizes type-safe versions such as `std::popcount`, `std::countr_zero`, and `std::countl_zero`; unlike the compiler builtins, the two counting functions return the full width for zero.

These are semantic operations, not macros for one particular instruction. On a target without the corresponding instruction, the compiler emits an equivalent short sequence or a library call.

## Recipes

### Sign of an integer

Use `x < 0` when a Boolean answer is needed. On a two's-complement machine with an arithmetic right shift, `x >> 31` produces `0` for a nonnegative 32-bit integer and `-1` for a negative one, but the comparison states the intent and does not hard-code the width.

### Check if two integers have the same sign

For fixed-width two's-complement signed integers, `(x ^ y) < 0` says that the sign bits differ. The direct comparison `(x < 0) == (y < 0)` is portable and compiles just as well.

### Absolute value of an integer

For a 32-bit `int` on a two's-complement machine with arithmetic right shift, extract the sign mask: `int mask = x >> 31`. This is `-1` for negative numbers and `0` for nonnegative ones.

XOR it with the initial number and subtract the mask:

```c++
int absolute(int x) {
    int mask = x >> 31;
    return (x ^ mask) - mask;
}
```

For a nonnegative input, this returns `x`. For a negative input, XOR performs the one's complement and subtracting `-1` adds one, producing the two's-complement negation. Just like `std::abs`, the signed result is not representable for `INT_MIN`; evaluating that case has undefined behavior. If the full magnitude range is required, convert and negate in the corresponding unsigned type:

```c++
unsigned magnitude(int x) {
    unsigned u = (unsigned) x;
    return x < 0 ? 0u - u : u;
}
```

### Get last 1-bit

For unsigned `x`, `x & -x` isolates the lowest set bit. Unsigned negation is defined modulo $2^w$, and the result is zero when `x` is zero.

### Remove the last 1-bit of an integer

For unsigned `x`, `x & (x - 1)` clears the lowest set bit. Repeating it therefore takes one iteration per set bit:

```c++
int popcount(unsigned x) {
    int count = 0;
    while (x != 0) {
        x &= x - 1;
        count++;
    }
    return count;
}
```

This was a useful software implementation when population count was absent from the instruction set. With `popcnt`, use the builtin and let the compiler decide.

### Checking for power of two

An unsigned integer is a power of two exactly when it has one set bit:

```c++
bool is_power_of_two(unsigned x) {
    return x != 0 && (x & (x - 1)) == 0;
}
```

The `x != 0` check is essential because the bit-clearing expression is also zero for zero. C++20 calls this operation `std::has_single_bit`.

### Reversing bits

Clang provides `__builtin_bitreverse8`, `16`, `32`, and `64`. A portable 32-bit fallback successively swaps adjacent 1-, 2-, 4-, 8-, and 16-bit fields:

```c++
uint32_t reverse_bits(uint32_t x) {
    x = (x >> 1  & 0x55555555u) | (x & 0x55555555u) << 1;
    x = (x >> 2  & 0x33333333u) | (x & 0x33333333u) << 2;
    x = (x >> 4  & 0x0f0f0f0fu) | (x & 0x0f0f0f0fu) << 4;
    x = (x >> 8  & 0x00ff00ffu) | (x & 0x00ff00ffu) << 8;
    return (x >> 16) | (x << 16);
}
```

Each line exchanges equally sized neighboring fields. The operations within a line are independent, so the algorithm has logarithmic depth rather than moving all 32 bits one at a time. Use a builtin when available: some architectures have a dedicated bit-reversal instruction, and the compiler is better placed to select it.

### Swapping numbers with xor

You've probably heard of this one.

```c++
a ^= b;
b ^= a;
a ^= b;
```

This only works when `a` and `b` are distinct objects of the same integer type. If they alias, all three names become zero. It also creates an unnecessary dependency chain. A temporary variable or `std::swap` is clearer and normally becomes a few register moves; a memory `xchg` instruction is not inherently faster.

### Modulus a power of two

For an unsigned integer and $m=2^k$, `x % m` is the same as `x & (m - 1)`. Compilers perform this replacement automatically when `m` is constant. It is not equivalent for a negative signed `x`, because signed remainder keeps the sign of the dividend.

## Masks

The usual operations on a bit position `k` are:

```c++
bool test(unsigned x, int k)   { return (x >> k) & 1u; }
unsigned set(unsigned x, int k)    { return x |  (1u << k); }
unsigned clear(unsigned x, int k)  { return x & ~(1u << k); }
unsigned toggle(unsigned x, int k) { return x ^  (1u << k); }
```

They require $0 \le k < 32$ for a 32-bit `unsigned`. Writing `1u` rather than `1` makes the shifted value unsigned and avoids running out of signed range at bit 31.

### Brute forcing

A mask can represent a subset of $n$ objects, with bit $i$ indicating whether object $i$ is present. Enumerating all masks gives the brute-force $O(2^n)$ solution to the subset-sum version of knapsack:

```c++
int ans = 0;
for (unsigned mask = 0; mask < (1u << n); mask++) {
    int s = 0;
    for (int i = 0; i < n; i++)
        if (mask >> i & 1)
            s += a[i];
    if (s <= C)
        ans = max(ans, s);
}
```

Here we require $n<32$. The branch is not necessarily expensive: for each fixed `i`, its outcome follows a regular pattern, and the compiler may replace it with a conditional move. More importantly, this implementation repeats almost all of its work. Enumerating masks in Gray-code order changes one membership bit at a time, while meet-in-the-middle reduces the exponent for larger $n$. Bit manipulation does not make an exponential algorithm stop being exponential.

### Subsets of all subsets

To enumerate all nonempty submasks of one mask:

```c++
for (unsigned submask = mask; submask != 0; submask = (submask - 1) & mask) {
    // ...
}
```

The expression subtracts one and then removes every bit that is not present in `mask`. If the zero submask is also needed, process it separately; using the same update after zero would start the sequence again.

One mask has $2^{\operatorname{popcount}(mask)}$ submasks. If we enumerate submasks for *all* $n$-bit masks, the total number of `(mask, submask)` pairs is $3^n$. Each bit has three possible states: outside `mask`, inside `mask` but outside `submask`, or inside both.

## When a Trick Is an Optimization

A bit identity is not faster merely because it is shorter in source code. The compiler already recognizes most scalar recipes in this chapter, including rotations, absolute value, power-of-two tests, and clearing the lowest bit. Rewriting them by hand is useful only when it changes the algorithm or communicates an operation the language otherwise cannot express.

The major wins come from packing independent Boolean values into one word. Intersecting two 64-bit masks performs 64 logical ANDs at once; a bitmap scan uses `popcnt` to reduce 64 predicates to a count; dynamic programs over small sets use one integer as an entire state. This is [word-level parallelism](/hpc/complexity/models/#word-ram), not a peephole trick.

As always, inspect the generated code and benchmark the surrounding loop. A branchless expression can lengthen a dependency chain, a lookup table can replace arithmetic with a cache miss, and a specialized instruction can have different throughput on different CPUs. The identity tells us that a transformation is correct under its preconditions; the machine decides whether it is useful.
