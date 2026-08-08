---
title: Arithmetic Optimizations
weight: 10
---

Programmers are taught many little arithmetic tricks: replace multiplication by a power of two with a shift, replace `x % 2` with `x & 1`, move divisions out of loops, and so on.

Most of these tricks are useful for understanding the machine, but not for outsmarting the compiler. Modern compilers already know thousands of algebraic identities, along with the awkward corner cases that decide whether each identity is legal.

The productive question is usually not “how do I rewrite this expression?” but “what prevents the compiler from rewriting it?”

## Constant Folding

Arithmetic with compile-time constants is normally performed during compilation:

```c++
int f(int x) {
    int width = 7 * 9;
    return x * width + 3 - 3;
}
```

The generated code only needs to calculate `x * 63`. If the caller also supplies a constant `x` and the function is visible after inlining, the entire computation may disappear.

This is a combination of *constant folding*, *constant propagation*, inlining, and dead-code elimination. `constexpr` can require evaluation during compilation in certain contexts, but it is not necessary for ordinary optimization. A value only needs to be provably constant.

The important word is “provably.” A separately compiled function, a mutable global, or a load through a pointer may hide the value. Templates, inline functions, and [link-time optimization](../stages) help mostly because they expose more information to the optimizer.

## Strength Reduction

Replacing an expensive operation with a cheaper equivalent is called *strength reduction*. The textbook examples are:

```c++
unsigned mul8(unsigned x) { return x * 8; }
unsigned div8(unsigned x) { return x / 8; }
unsigned mod8(unsigned x) { return x % 8; }
```

These become a left shift (or an equivalent address calculation), a right shift, and an `and` with 7. You should still write the operations you mean. The compiler knows the target, while source code such as `x / 8` keeps the intended arithmetic obvious if the type or divisor later changes.

Division by a non-power-of-two constant is optimized too:

```c++
unsigned div10(unsigned x) {
    return x / 10;
}
```

There is normally no division instruction here. The compiler multiplies by a precomputed fixed-point reciprocal and shifts the high part of the product. The [integer division](/hpc/arithmetic/division) chapter derives this transformation in detail.

This only works for a divisor known during compilation. If the same runtime divisor is used many times, libraries such as `libdivide` precompute the reciprocal once and reuse it — an optimization that changes the interface and therefore cannot always be invented locally by the compiler.

## Loops and Induction Variables

Consider this loop:

```c++
for (int i = 0; i < n; i++)
    sum += a[i * stride];
```

The source contains a multiplication on every iteration, but the machine code can keep a pointer and add `stride * sizeof(*a)` to it. The sequence `i * stride` is an *induction variable*: its next value is cheaper to obtain from the previous one than to recalculate from scratch.

The same idea works for more complicated affine expressions, loop counters, and addresses. Replacing the index loop manually with pointer arithmetic usually produces identical code, so use whichever version is clearer and inspect the result before changing it.

Expressions that do not change between iterations can be *hoisted* out of the loop:

```c++
void normalize(float *a, int n, float low, float high) {
    for (int i = 0; i < n; i++)
        a[i] = (a[i] - low) / (high - low);
}
```

The subtraction `high - low` only needs to happen once. Loads can be hoisted too, but only when the compiler can prove that no store in the loop changes the same memory. This is one reason [aliasing contracts](../contracts#memory-aliasing) matter.

Replacing every floating-point division with multiplication by one reciprocal is a different transformation: it changes rounding. Under strict floating-point rules the compiler may hoist the denominator but still retain division in the loop.

## Algebra Has Preconditions

Some identities from school mathematics are not identities of C++ arithmetic.

For unsigned integers, overflow wraps modulo a power of two. For signed integers, overflow is undefined. Consequently,

```c++
(x + 1) > x
```

can be simplified to `true` when `x` is signed — every defined execution satisfies it — but not when `x` is unsigned, because `UINT_MAX + 1` becomes zero.

This sometimes surprises programmers who expected the compiler to preserve the behavior of overflowing signed arithmetic. There is no such behavior to preserve. If wraparound is part of the algorithm, use an unsigned type. If overflow is impossible because of an input bound, make that bound clear and test it.

Signed division has another corner case. For nonnegative integers, `x / 8` equals `x >> 3`. For negative values, C++ division rounds toward zero, while an arithmetic right shift commonly rounds toward negative infinity. The compiler inserts a correction unless it can prove that `x` is nonnegative.

The [contract programming](../contracts) chapter discusses how types, assertions, and assumptions can communicate facts like these. Most failed arithmetic optimizations are really missing preconditions.

## Floating-Point Reassociation

Real-number addition is associative. Floating-point addition is not:

$$
(a+b)+c\ne a+(b+c)
$$

in general, because both additions round. This matters for reductions:

```c++
float sum(float *a, int n) {
    float s = 0;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}
```

Every iteration depends on the previous one. Splitting the sum into several accumulators would expose [instruction-level parallelism](/hpc/pipelining/throughput), and using vector accumulators would expose [SIMD parallelism](/hpc/simd/reduction), but both change the order of additions.

With strict IEEE semantics, the compiler has to preserve the relevant ordering. Flags such as `-ffast-math` permit reassociation and make vectorization much easier, while also changing assumptions about NaNs, infinities, signed zero, and exceptions. This is not a generic “make floating point faster” switch; it is a different numerical contract.

Sometimes reassociation improves the error, for example by creating a balanced summation tree. Sometimes it destroys a carefully stabilized formula. The compiler cannot decide which numerical meaning the application intended.

## Fused Operations

Many processors have a fused multiply-add instruction that calculates

$$
a\cdot b+c
$$

with one final rounding instead of rounding the multiplication and addition separately. It is often both faster and more accurate.

```c++
float linear(float x, float a, float b) {
    return a * x + b;
}
```

Whether this contracts into one instruction depends on the target and floating-point options. `std::fma(a, x, b)` explicitly requests fused semantics; ordinary `a * x + b` does not always do so. These two expressions are mathematically equal but can differ by one rounding in machine arithmetic.

Compilers also recognize rotations, population-count idioms, absolute values, minima, and many other instruction-shaped expressions. Writing the straightforward form usually gives the optimizer more freedom than spelling out a long sequence of hand-made bit tricks.

## What the Compiler Will Not Do

Compilers are excellent at local algebra. They do not usually:

- replace an unstable numerical formula with a stable one;
- choose a different representation for a big integer;
- introduce a lookup table with a nontrivial cache trade-off;
- decide that an approximate answer is acceptable;
- replace a quadratic algorithm with an FFT.

These transformations change memory usage, error guarantees, preprocessing costs, or the algorithm's interface. They require information that is not present in one expression.

A practical workflow is therefore:

1. write the clearest expression with the correct types;
2. compile with the intended optimization level and target;
3. inspect the hot loop's assembly;
4. if an expensive operation remains, look for the corner case or missing invariant that makes it necessary;
5. change the algorithm only when local simplification is not enough.

The compiler has already memorized the arithmetic tricks. Our job is to give it the facts that make them true — and to recognize when the real optimization lives one level above arithmetic.
