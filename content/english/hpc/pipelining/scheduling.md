---
title: Instruction Scheduling
weight: 4
draft: false
---

Let's dive a bit deeper.

### Superscalar Processors

The CPI of a perfectly pipelined scalar processor should tend to one, but it can actually be even lower than one.

It is very common for programs to have groups of logically independent operations that can be processed by different execution units. To improve their utilization, we can widen the pipeline so that more than one instruction is processed at a time and, if possible, schedule these instructions on different parts of the ALU. Architectures capable of executing more than one instruction per cycle are called *superscalar*, and most modern CPUs are.

<!-- Pipeline of a superscalar CPU with the width of 2 img/superscalar.png -->

Interleaving the stages of execution is a general idea in digital electronics, and it is applied not only in the main CPU pipeline, but also on the level of separate instructions and [memory](/hpc/cpu-cache/mlp). Most execution units have their own little pipelines and can accept another operation before the previous one has finished. If a certain operation is frequent, it also makes sense to duplicate its execution unit. This is why a modern core has several integer ALUs, load units, and vector units rather than one universal execution unit.

Widening the hardware creates a coordination problem. A four-wide front-end is useful only if the back-end can find four operations whose inputs are ready and whose required execution units are available. The source program rarely presents them in exactly that order.

### Micro-Operations

An x86 instruction is not necessarily what the back-end executes. The decoder translates it into one or more simpler internal operations called *micro-operations*, usually shortened to *micro-ops* or *uops*.

A register-register `add` is normally one uop. An instruction such as

```nasm
add eax, DWORD PTR [rdi]
```

looks like one instruction to the programmer, but the back-end still has to load a value and add it. Depending on the microarchitecture, the two pieces may be tracked separately or kept together as one fused uop for part of the pipeline.

Most common instructions decode into a small fixed number of uops. Rare, complicated instructions are handled by a *microcode sequencer* that emits a longer sequence. Microcode is also patchable, which lets CPU vendors correct some processor bugs without changing the instruction set visible to software.

Each microarchitecture has its own collection of execution *ports*. A port accepts certain kinds of uops and connects them to suitable execution units. An integer addition may be accepted by several ports, while a division or a particular shuffle may have only one possible destination. The [instruction tables](../tables) summarize these mappings as throughput and latency, and [machine-code analyzers](/hpc/profiling/mca) model the contention directly.

### Instruction Scheduling

This poses some additional challenges in coordinating which instructions to execute and in which order. The real schedulers are very complex, but the following mental model is good enough most of the time.

Modern processors don’t actually execute instructions one-by-one, but maintain a *pipeline* of pending instructions so that two independent operations can be executed concurrently without waiting for each other to finish.

A simplified out-of-order pipeline works as follows:

1. The front-end predicts branches, fetches machine code, decodes instructions into uops, and preserves their original order.
2. The rename stage assigns their outputs to physical registers and allocates entries in a *reorder buffer*.
3. Uops wait in scheduling queues until their input values and a suitable execution port are available.
4. Ready uops are issued to execution units, possibly long before older stalled uops.
5. Completed uops retire in the original program order.

A bit more precisely, the CPU will look at the instruction stream up to some distance in the future. If there are branches, it will do branch prediction to produce a sequential stream of instructions. It then determines which uops are ready for execution. If a future uop only uses values that are already available, it is safe to start as soon as a compatible execution unit is free, even if an older independent uop is still waiting.

Consider two independent multiplication chains:

```nasm
mov  eax, DWORD PTR [rdi]
imul eax, eax
mov  edx, DWORD PTR [rsi]
imul edx, edx
add  eax, edx
```

The final `add` must wait for both multiplications, but neither multiplication needs the other. Their loads and multiplications can overlap on a superscalar processor. The source order does not grant or forbid this parallelism; data dependencies do.

### Register Renaming

Architectural register names create dependencies that do not always exist in the computation. If one instruction reads `eax` and a later independent instruction overwrites `eax`, executing the write first would appear to destroy the old value. This is a *write-after-read* dependency. Two writes to the same name similarly create a *write-after-write* dependency.

The processor removes these false dependencies by mapping architectural registers to a larger set of physical registers. Each write gets a new physical destination, while earlier reads keep their mapping to the old value. The mapping is changed, not the data itself.

Among register-name dependencies, only *read-after-write* dependencies remain fundamental. In this chain,

```nasm
imul eax, eax
imul eax, eax
imul eax, eax
```

each multiplication genuinely needs the result of the previous one. No number of execution ports can make the chain shorter than three multiplication latencies. To expose more parallelism, the algorithm has to provide independent chains, as we did with multiple accumulators in the [throughput](../throughput) chapter.

Memory dependencies are harder because addresses may not be known when operations enter the scheduler. Loads are often allowed to pass older stores whose addresses appear unrelated. If the guess was wrong, the load and its dependent work are replayed. Stores are kept in a buffer and do not become globally visible until it is safe to commit them.

### In-Order Retirement

Execution is out of order, but retirement is in order. This distinction gives the processor *precise exceptions*.

Suppose a younger addition finishes while an older load causes a page fault. The addition may have produced a physical-register value, but it has not retired, so the processor can discard it and present the kernel with a state corresponding exactly to the faulting load. The same mechanism discards speculative work after a branch misprediction.

The reorder buffer therefore serves two purposes: it provides a window in which independent work can be found, and it keeps enough information to restore the architectural state. Once the oldest uop has completed without an exception, its result can retire and its buffer entry can be reused.

All of this happens in the hardware, all the time, fully automatically. The only thing that the programmer needs to do is to make sure there are sufficiently many independent instructions always available for execution. The magic takes place inside the CPU. The compiler just produces machine language instructions, without any special annotation that indicates whether or not these instructions can be executed in parallel. The CPU will then automatically figure out which of the instructions can be executed in parallel.

### The Scheduling Window

You can schedule independent instructions separately, but only up to some extent. The reorder buffer, scheduling queues, physical registers, and load and store buffers are all finite. The out-of-order window is large — typically hundreds of uops — but it is still not enough to hide an arbitrarily long chain or an unlimited number of main-memory accesses.

Several events can prevent useful work from reaching the scheduler:

- A branch misprediction discards younger speculative uops.
- A cache miss can occupy load-buffer and reorder-buffer entries for a long time.
- Too many in-flight destinations exhaust physical registers and stall the rename stage.
- Too many source-level live values create register pressure, making the compiler add spill loads and stores.
- A large loop may be limited by fetch and decode before the back-end fills.
- A narrow dependency chain may offer no other ready uops at all.

This is why unrolling sometimes helps: it places operations from several iterations in the window at once. Excessive unrolling eventually hurts because it increases code size and register pressure. The useful amount is just enough to expose independent work and saturate the bottleneck identified by the [instruction throughputs](../tables).

Hardware scheduling does not make compiler scheduling irrelevant. The compiler chooses instructions, registers, and loop structure, and these choices decide which dependencies the hardware sees. But manually rearranging two adjacent independent statements is rarely the important part. The productive task is to change the dependency graph — split an accumulator, start independent loads earlier, or process several inputs together — and let the processor schedule the resulting uops.
