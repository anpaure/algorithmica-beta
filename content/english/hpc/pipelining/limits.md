---
title: Theoretical Performance Limits
weight: 5
draft: false
---

A very good optimization model is to identify the bottleneck and then work around it.

Before changing code, it is useful to ask a stronger question: how fast could this computation possibly run on the target machine? A theoretical limit does not predict the exact running time, but it can rule out impossible goals and tell us which resource has to be improved.

For a steady-state loop, several independent lower bounds apply at once:

$$
T \ge \max(T_\text{front-end},\; T_\text{ports},\; T_\text{dependencies},\; T_\text{bandwidth},\; T_\text{latency}).
$$

The largest term is the current bottleneck. Optimizing a smaller term cannot improve the total, although it may become important after the largest one is reduced.

These are *bounds*, not promises. They assume ideal overlap, warm caches, predictable control flow, and no interference from the operating system. Real execution can always be slower.

### Front-End Width

The [front-end](/hpc/architecture/layout) has a finite fetch and decode width. If a loop needs $U$ decoded uops per iteration and the front-end supplies at most $D$ uops per cycle, then

$$
T_\text{front-end} \ge \frac{U}{D}
$$

cycles per iteration.

Machine instructions and uops are not the same unit. This AVX2 loop adds two arrays and writes into a third:

```c++
for (int i = 0; i < n; i += 8) {
    __m256i x = _mm256_loadu_si256((__m256i*) (a + i));
    __m256i y = _mm256_loadu_si256((__m256i*) (b + i));
    __m256i z = _mm256_add_epi32(x, y);
    _mm256_storeu_si256((__m256i*) (c + i), z);
}
```

The important part of its assembly may contain only three instructions:

```nasm
vmovdqu ymm0, YMMWORD PTR [rdi+rax]
vpaddd  ymm0, ymm0, YMMWORD PTR [rsi+rax]
vmovdqu YMMWORD PTR [rdx+rax], ymm0
```

but the memory-source addition still needs both a load operation and an arithmetic operation in the back-end. Conversely, adjacent machine instructions are sometimes fused before or during decoding. Count the units used by the particular microarchitecture, not just source statements or assembly lines.

Small hot loops may also be supplied by a decoded-uop cache instead of the ordinary decoders. Large unrolled loops can fall out of it and become front-end-bound even though their arithmetic did not change. This is one reason that blindly increasing the unroll factor eventually stops helping.

### Execution-Port Throughput

Each uop can execute on a particular set of [ports](../scheduling#micro-operations). If one iteration needs $u_p$ operations that must use port $p$, and that port accepts $c_p$ such operations per cycle, then

$$
T_\text{ports} \ge \max_p \frac{u_p}{c_p}.
$$

The real calculation is slightly more complicated because many uops may choose between several ports. A scheduler can balance flexible additions across them, but it cannot move a shuffle onto a load-only port. Tools such as [llvm-mca](/hpc/profiling/mca) solve this small resource-allocation problem more reliably than counting mnemonics by hand.

As a simpler example, incrementing an aligned integer array can use just two instructions per 8 elements — a read-fused addition and a write, assuming `ymm1` contains eight ones:

```asm
vpaddd  ymm0, ymm1, YMMWORD PTR [rax]
vmovdqa YMMWORD PTR [rax], ymm0
```

If the target can execute one 256-bit store per cycle, the loop cannot update more than eight 32-bit values per cycle, regardless of how many addition units it has. The corresponding per-second limit is this rate multiplied by the sustained clock frequency. It should be reported as integer operations or processed elements per second, not GFLOPS.

The "port" and "L1 bandwidth" views often describe the same physical limit. Loads and stores reach L1 through load/store execution units, so a shortage of those ports can be the reason an in-cache loop cannot consume more bytes.

### Dependency Latency

Throughput numbers assume independent operations. A true dependency forms a path through the computation, and the sum of instruction latencies along the longest such path is another lower bound.

Consider a scalar reduction:

```c++
int sum = 0;
for (int i = 0; i < n; i++)
    sum += a[i];
```

Every addition needs the previous value of `sum`. If addition latency is $L$, the loop needs at least $nL$ cycles along this dependency chain, even if several addition ports are idle.

Using several accumulators changes the graph:

```c++
int s0 = 0, s1 = 0;
for (int i = 0; i < n; i += 2) {
    s0 += a[i];
    s1 += a[i + 1];
}
int sum = s0 + s1;
```

The two chains can overlap. More generally, an instruction with latency $L$ and throughput $R$ operations per cycle needs about $L \cdot R$ independent chains to reach that throughput, assuming enough registers and no other bottleneck.

A branch misprediction creates a control dependency of a different kind. The processor may have executed many younger uops, but all of them are discarded. If a branch is mispredicted with probability $p$ and recovery costs $C$ cycles, then $pC$ cycles per branch is a useful first-order bound — provided that the misses do not overlap with another bottleneck.

### Memory Bandwidth

A loop that performs little arithmetic per byte is often limited by the rate at which a cache or main memory can transfer data.

Let $W$ be the number of useful operations and $Q$ the number of transferred bytes. Their ratio

$$
I = \frac{W}{Q}
$$

is called *arithmetic intensity*. If the relevant memory level supplies $B$ bytes per second and the processor can perform at most $P$ operations per second, then useful performance is bounded by

$$
\operatorname{performance} \le \min(P,\; I B).
$$

This is the basic *roofline model*. At low intensity, performance grows with bandwidth. At high intensity, more bandwidth no longer helps because arithmetic becomes the roof.

The difficult word is "relevant." An array may come from L1, a shared last-level cache, or RAM depending on its size and reuse. Byte counts also need to include stores, possible write allocation, and any data fetched speculatively. A single-pass SIMD loop is frequently bandwidth-bound for large inputs, but an expensive operation such as division can still make it compute-bound.

Blocking improves arithmetic intensity by reusing data while it is in a fast cache. The classic example is [matrix multiplication](/hpc/algorithms/matmul): its operation count does not change, but a blocked algorithm performs many multiply-adds for each loaded cache line.

### Memory Latency and Parallelism

Bandwidth limits a stream of many transfers. Latency limits a transfer whose address is needed before the next one can even be requested.

Pointer chasing is the extreme case:

```c++
Node *p = head;
for (int i = 0; i < n; i++)
    p = p->next;
```

If every load misses in the relevant cache and takes $L$ cycles, the dependency chain is at least $nL$ cycles long. Prefetching cannot request `p->next->next` before `p->next` is known.

Independent random reads are different:

```c++
for (int i = 0; i < n; i++)
    sum += a[index[i]];
```

The processor can keep several misses in flight. If at most $M$ requests can overlap, latency alone gives the approximate bound

$$
T_\text{latency} \ge \frac{nL}{M},
$$

while memory bandwidth supplies another bound. The actual concurrency is limited by the out-of-order window, load buffers, translation lookaside buffers, and the memory controller. Increasing [memory-level parallelism](/hpc/cpu-cache/mlp) helps until one of those resources or bandwidth is saturated.

### Algorithmic Limits

Hardware bounds apply after an algorithm has been chosen. Sometimes the strongest limit comes from the problem itself.

**Comparison-based sorting.** A comparison has two outcomes, while sorting $n$ distinct elements must distinguish between $n!$ possible orders. A decision tree therefore needs depth at least

$$
\left\lceil \log_2(n!) \right\rceil = n\log_2 n - O(n)
$$

in the worst case. Improving comparison throughput cannot make a comparison sort linear. Integer radix sorting escapes the bound by using more information than a pairwise comparison.

**Linear algebra.** Dense linear algebra is commonly compared with the machine's FMA peak. A vector FMA performs one multiplication and one addition in every lane, conventionally counted as two floating-point operations. If a vector contains $V$ elements, the core starts $F$ such instructions per cycle, and its frequency is $f$, then

$$
P_\text{FMA} = 2VFf.
$$

This is the number usually advertised as peak FLOPS. Reaching it requires enough independent accumulators to hide FMA latency, enough register and cache bandwidth to feed the units, and a problem with sufficiently high arithmetic intensity. A dot product on data streamed once from RAM and a blocked matrix multiplication have the same instruction available, but only one is likely to approach the arithmetic peak.

### Using the Bounds

For a hot loop, a useful procedure is:

1. Compile for the actual target and inspect the steady-state assembly.
2. Count uops, port demand, transferred bytes, and true dependency chains per unit of useful work.
3. Calculate a lower bound from each resource using measured or documented machine parameters.
4. Compare the bounds with the measured running time.
5. Change the algorithm or layout that feeds the largest term, then repeat.

If the measurement is far below every calculated ceiling, the model is missing something: a cache level, a branch miss, an aliasing check, front-end delivery, or operating-system noise. If it is close to one ceiling, optimizing a different resource is wishful thinking.

Performance engineering is largely the process of moving bottlenecks. Vectorization may turn an arithmetic-bound loop into a bandwidth-bound one; blocking may turn it back into a compute-bound one; excessive unrolling may then make it front-end-bound. There is no single "CPU speed" to approach — only a collection of limits, one of which is tight for the program in front of you.
