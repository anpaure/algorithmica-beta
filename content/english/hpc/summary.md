---
title: Summary
weight: 99
ignoreIndexing: true
---

Now we have enough information to summarize what we've learned.

Optimization is an experimental science. The machine is too complicated, and the compiler is too transformative, for source-level intuition to be reliable by itself. The central loop is therefore:

1. define the workload and the metric;
2. measure a baseline;
3. locate the bottleneck;
4. change one important thing;
5. verify correctness and measure again.

Every technique in this book belongs somewhere inside this loop. The difficult part is rarely knowing that SIMD or cache blocking exists. It is deciding which transformation the current bottleneck will reward.

## Start with the Problem

Before optimizing an implementation, make sure it is implementing the right algorithm and solving the right problem. Removing work dominates doing the same work faster.

- Avoid unnecessary computation, conversion, copying, and allocation.
- Choose an asymptotically appropriate algorithm.
- Batch operations when setup costs can be shared.
- Change the representation or interface if it removes an entire stage.
- Precompute results when the input domain and memory budget permit it.

Keep a simple reference implementation. It defines the behavior, generates test answers, and gives the benchmark a meaningful baseline. For many case studies in this book, the final optimized kernel is less obviously correct than the original by-definition loop; having both is a feature, not duplication.

## Let the Compiler Work

Compile optimized code before optimizing it by hand. For local experiments, `-O3 -march=native` is a reasonable starting point. Production binaries may need a portable target or several runtime-dispatched versions instead of `-march=native`.

Flags are contracts, not incantations. `-ffast-math` can enable important floating-point transformations, but it also permits the compiler to ignore NaNs, infinities, signed zero, rounding modes, and strict reassociation rules. Use it only when the algorithm allows those changes. Similarly, unrestricted aliasing, integer overflow, and alignment assumptions must be true in every execution, not merely in the benchmark.

Inspect the generated assembly when a source-level change has a surprising effect. It answers basic questions quickly: was the loop removed, inlined, unrolled, or vectorized? Did a division become a multiplication? Did an abstraction introduce a branch or an indirect call? If the compiler already produced the instruction sequence you intended to write, intrinsics will not make it faster by virtue of being less readable.

## Find the Limiting Resource

The running time is normally constrained by one of a few resources:

- total work or instruction throughput;
- a dependency chain and instruction latency;
- branch prediction;
- cache or memory latency;
- cache or memory bandwidth;
- front-end decoding and code size;
- allocation, system calls, or other runtime services.

A benchmark tells you *that* a version is slow. A profiler, hardware counters, and a small scaling experiment help explain *why*.

Vary the input size. A sharp transition when the working set leaves a cache points to the memory hierarchy. Compare useful bytes per second with sustainable bandwidth. Compare useful operations per cycle with the execution resources listed in instruction tables. Use [machine-code analyzers](/hpc/profiling/mca/) for compact compute kernels, but remember that they cannot predict cache misses or an input-dependent branch predictor.

The distinction between latency and throughput is fundamental. A dependent chain pays latency; independent operations can overlap and approach throughput. The same distinction applies to arithmetic instructions, cache misses, and even system calls.

## Transform the Bottleneck

Once the limiting resource is known, the candidate transformations become much narrower.

### Data movement

- Store together the data that is consumed together.
- Traverse memory sequentially and make the common path predictable to the hardware prefetcher.
- Use [blocking](/hpc/external-memory/locality/) so that reused data remains in the smallest practical cache.
- Prefer compact representations when they reduce traffic without adding more computation than they save.
- Choose [AoS or SoA](/hpc/cpu-cache/aos-soa/) according to the fields each operation actually needs.
- Replace pointers with indices, implicit layouts, or flat arrays when pointer chasing is the bottleneck.
- Add software prefetching only when accesses are predictable early enough, do not already prefetch well in hardware, and leave enough independent work to hide the latency.

Temporal locality is not just "using a cache." It is arranging the order of computation so that a value is reused before it is evicted. Spatial locality is arranging values so that most bytes fetched in a cache line are useful.

### Control flow

An unpredictable branch can be expensive because a misprediction discards speculative work. A predictable branch is usually cheap, and removing it can create extra instructions or memory traffic. Use branchless transformations when the branch is genuinely unpredictable or when straight-line code enables vectorization; do not remove branches as a ritual.

Separate common and rare cases when this makes the hot path simpler. Tables, masks, conditional moves, and arithmetic identities can replace control dependencies with data dependencies, but the new dependency chain still has a cost.

### Dependencies and instruction-level parallelism

Break long dependency chains by keeping several independent accumulators or processing several independent inputs together. Loop unrolling can expose this parallelism and amortize loop overhead, but excessive unrolling increases code size and register pressure. Compilers usually make a good first attempt; pragmas and manual unrolling are hypotheses to benchmark, not mandatory decorations.

Consult [instruction tables](/hpc/pipelining/tables/) for latency, reciprocal throughput, and port usage. A loop may be limited by a single saturated execution port even when many other units are idle. Changing an instruction is useful only if it moves work away from the resource that is full.

### SIMD

Vectorization is profitable when the same operation applies to independent, regularly arranged data. First make the scalar loop simple: remove hidden aliases, calls, complex control flow, and ambiguous dependencies. Check the compiler's vectorization report. Use intrinsics when the required data movement or instruction is difficult to express in portable source.

The nominal vector width is only an upper bound on speedup. Horizontal reductions, shuffles, masks, tails, conversions, cache bandwidth, and a scalar dependency chain all consume part of it. The best SIMD algorithm is often not a line-by-line translation of the best scalar algorithm.

### Arithmetic and representation

Use the narrowest type that represents every intermediate result—not merely every input. Narrow data can reduce memory traffic and fit more lanes in a vector, while a widened accumulator prevents overflow. Integer and floating-point transformations have contracts: prove the range, error, and exceptional cases before relying on them.

Division by constants, repeated transcendentals, modular reduction, and format conversion are common targets for reciprocal multiplication, lookup tables, approximation, or precomputation. The right question is not whether an operation is "slow," but whether it occupies the limiting resource and whether its replacement preserves the required result.

## Measure Without Lying to Yourself

Fast code is unusually easy to benchmark incorrectly. The compiler can delete an unused computation; the first run can include page faults and initialization; frequency scaling and other processes add noise; a synthetic input can make every branch predictable; an average can hide catastrophic outliers.

A trustworthy benchmark:

- uses inputs and distributions representative of the real workload;
- keeps observable results so the compiler cannot remove the work;
- separates setup from the operation being measured;
- warms up relevant code and data when the application would;
- repeats enough times to report a distribution;
- records the compiler, flags, CPU, and relevant environment;
- checks the optimized result against the reference implementation.

For small kernels, count cycles or nanoseconds per element rather than only total time. For memory-bound code, report useful bandwidth. For a service, tail latency may matter more than a microbenchmark median. Optimize the metric that belongs to the actual problem.

## A Practical Checklist

From highest leverage to lowest, ask:

1. Can the operation be avoided, cached, batched, or moved out of the hot path?
2. Is the algorithm appropriate for the input sizes and distributions?
3. Is optimized compilation enabled under the contracts the program can guarantee?
4. What does profiling identify as the dominant cost?
5. Is the working set limited by bandwidth, latency, computation, dependencies, branches, or the front end?
6. Can layout or traversal reduce data movement and improve locality?
7. Can independent work overlap to hide latency?
8. Can control flow be made predictable or vector-friendly without doing too much extra work?
9. Can the compiler vectorize the loop; if not, what precisely prevents it?
10. Can arithmetic or representation be simplified with a proved range and error bound?
11. Did the change improve the real metric on all important inputs?
12. Is the speedup worth the portability and maintenance cost?

Then start again. Optimization is iterative because removing one bottleneck reveals the next one. The final program is not the one containing the largest number of tricks; it is the simplest version for which the remaining bottlenecks are either fundamental or no longer worth fixing.
