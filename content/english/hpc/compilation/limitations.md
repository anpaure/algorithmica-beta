---
title: What Compilers Can and Can't Do
weight: 10
draft: false
---

Let's sum up this chapter with some general advice.

Writing optimizing compilers is very hard. The standard introductory textbook by Alfred Aho, Monica Lam, Ravi Sethi, and Jeffrey Ullman is [about 1000 pages long](https://www.amazon.com/Compilers-Principles-Techniques-Tools-2nd/dp/0321486811/), and it only begins to describe the subject.

GCC enables a long list of separate optimization passes at `-O3`. At a very high level, these optimizations improve performance by:

- doing a good job at fundamental algorithms like register allocation, scheduling, and dead-code elimination;
- eliminating unnecessary abstractions by inlining functions and removing temporary objects;
- knowing the CPU architecture very well — replacing divisions by powers of two with binary shifts, forming addressing operations with `lea`, and selecting suitable instructions;
- knowing lots and lots of "tricks" — specific peephole optimizations and loop transformations — and applying them whenever they are legal and expected to be profitable.

Unless you are *really* into compiler engineering, I wouldn't recommend going through the list and learning all of them. Instead, gradually build up a broader understanding of what compilers are capable of. Trial and error generally works well here: assume that everything generic and simple enough is already implemented, but always check the assembly output in case you are wrong.

### The As-If Rule

An optimizing compiler does not execute the source code literally. It may transform the program in any way that has the same observable behavior according to the language rules. This is known as the *as-if rule*.

It may calculate constants during compilation, remove unused objects, inline calls, reorder independent instructions, or replace a loop with a completely different instruction sequence. It does not need to preserve source-level operations that nobody can observe.

The inverse is just as important: an optimization is forbidden if some valid execution can observe a difference. The compiler has to respect possible pointer aliasing, integer and floating-point semantics, exceptions, `volatile` accesses, atomics, and calls whose definitions it cannot see. It does not know which inputs the programmer privately considers "reasonable."

Undefined behavior changes the premise. If signed overflow or an out-of-bounds access occurs, the language imposes no requirements on that execution, so the optimizer does not have to preserve an imagined wraparound or adjacent-object access. This freedom enables many transformations, but it also turns hidden bugs into surprising optimized code.

### Missing Information

The compiler can only use facts available at the point where it optimizes.

```c++
int twice(int x);

int answer() {
    return twice(21);
}
```

If `twice` is in a separately compiled library, the caller has to emit a real call. If its definition is visible and simply returns `2 * x`, inlining and constant propagation can reduce `answer()` to `return 42`.

The same limitation appears with an unknown virtual-call target, a load through a pointer that may alias a store, a loop bound read from mutable global state, or a target architecture that was never specified. Headers, `const`, `__restrict__`, [contracts](../contracts), `-march`, and [link-time optimization](../stages#interprocedural-optimization) are all ways of supplying missing information.

### Legality

An optimization may look obvious for the intended data and still be wrong for another input allowed by the function signature.

```c++
void add(int *a, const int *b, int n) {
    for (int i = 0; i < n; i++)
        a[i] += b[i];
}
```

Vectorizing this loop changes behavior when `a` and `b` overlap with a small offset. A compiler may generate a vector loop plus a run-time overlap check, but it cannot simply assume separate arrays. Adding valid `__restrict__` contracts removes the ambiguous case.

Floating-point reductions have a similar obstacle. Splitting one sum into several accumulators changes the order of rounding, while NaNs, infinities, signed zero, and exceptions introduce more observable cases. `-ffast-math` allows many useful transformations precisely because it changes these contracts; it is not merely a stronger `-O3`.

Signed integer overflow, shifts, division of negative values, and pointer provenance create other non-obvious legality conditions. When an expected transformation is missing, look for the input on which your proposed version differs from the specification.

### Profitability

A legal transformation is not necessarily faster.

Inlining removes a call and exposes more optimization, but duplicates machine code and may hurt the instruction cache. Unrolling removes loop overhead and exposes parallelism, but increases code size and register pressure. Vectorization processes several elements per instruction, but needs setup and remainder handling that may dominate a short loop.

The compiler estimates these trade-offs using a *cost model*. The estimate is based on the target CPU and whatever it can infer about trip counts, alignment, and branch probabilities. It does not know the production input distribution unless we provide it.

This is why optimization sometimes produces several versions of a loop with a run-time check selecting between them. It is also why [profile-guided optimization](../situational#profile-guided-optimization) can help: measured branch frequencies and loop counts replace guesses in the cost model.

### Implementation and Compile-Time Limits

Some legal and profitable transformations simply are not implemented. A pattern may be too rare, too architecture-specific, or too expensive to search for during every build. Optimization passes also run in a finite order, and one transformation may obscure the pattern another pass expected.

In general, when an optimization doesn't happen, it is usually for one of these reasons:

- The compiler doesn't have enough information to prove it legal.
- It cannot establish that the transformation will be beneficial.
- The relevant code is not visible in the same optimization unit.
- The transformation is not implemented, or finding it would consume too much compilation time.

Overly complicated source code can contribute to all four. Each abstraction layer has to be inlined, propagated, and simplified before the important loop or expression becomes recognizable. This is not an argument against abstractions, but it is a reason to inspect the hot path after compilation.

### Algorithmic Changes

Compilers are excellent at local transformations. They generally do not change the problem being solved.

They will not replace comparison sorting with radix sorting, choose a different hash-table layout, quantize a model because a small accuracy loss seems acceptable, or add a large lookup table with an application-specific memory trade-off. They also cannot decide to cache a result across API calls if mutation elsewhere is allowed to invalidate it.

These changes alter complexity, memory use, numerical guarantees, latency distribution, or public interfaces. The required information is outside the local code, and the decision belongs to the programmer.

A useful division of labor is:

- The programmer chooses the algorithm, representation, contracts, and approximation rules.
- The compiler performs instruction selection, register allocation, local algebra, loop transformations, and scheduling within those rules.
- The processor finds instruction-level parallelism dynamically within the resulting stream.

Expecting one layer to repair a poor decision made by the layer above it is rarely productive.

### Asking the Compiler

Assembly is the final answer, but compiler diagnostics often explain why it looks that way.

GCC can report successful and missed vectorization opportunities with

```
-fopt-info-vec-optimized -fopt-info-vec-missed
```

and Clang has analogous optimization remarks:

```
-Rpass=loop-vectorize -Rpass-missed=loop-vectorize
```

The messages commonly mention aliasing, an unknown trip count, unsupported control flow, or a cost-model decision. They are not infallible, but they provide a much better starting point than randomly rewriting syntax.

For a small function, [Compiler Explorer](https://godbolt.org/) is convenient. For a real program, compile with the exact production flags, inspect the binary rather than a simplified imitation, and use debug line information to connect hot instructions back to source.

### Checklist

Usually the right approach to performance is to think about how the main hot spots of the implementation should look in assembly, write high-level code that resembles them as much as possible, and then repeatedly ask the following questions:

0. **Is this really the hot path?** Measure on representative input before changing it.
1. **Did I request the intended optimization and target?** Check `-O3`, `-march`, link-time options, and floating-point policy.
2. **Is the transformation legal for every allowed input?** Check aliasing, overflow, alignment, exceptions, and numerical semantics.
3. **Can the compiler see the facts that prove it?** Use suitable types, `const`, `__restrict__`, assumptions, inlining, or LTO.
4. **Does the cost model know it is profitable?** Supply likely trip counts, branch hints, pragmas, or representative PGO data only when measurements justify them.
5. **Did the expected code appear, and is it actually faster?** Read the assembly and benchmark again.

If a small, generally useful transformation is missing despite clear legality and profitability, it may be worth reporting to GCC and Clang. If it is specific to one algorithm or changes its interface, implement it explicitly and test it like any other algorithmic optimization.

Compilers are powerful precisely because they preserve the contracts of the source language. High-performance code does not fight that constraint; it states narrower, truthful contracts and chooses representations that make the desired machine code a natural consequence.
