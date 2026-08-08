---
title: When to Optimize
weight: 4
---

Why do companies like Google ask to whiteboard-solve algorithm problems in their interviews?

The charitable explanation is that algorithm design is a compact proxy for several useful skills: turning an imprecise problem into a precise one, finding the part that determines scale, and writing code while explaining why it works. These abilities are particularly valuable in organizations with thousands of engineers and a great deal of in-house software. An asymptotic mistake copied into a widely used library can be expensive.

This does not mean that employees spend their afternoons reversing linked lists on whiteboards. Most computer science knowledge is abstracted away by languages and libraries, which is exactly why a well-designed abstraction has so much leverage. Algorithm design is the kind of skill that you may use rarely, but when it matters, it can determine whether a system is feasible at all.

There is also a lot of bias in hiring. Some companies do not ask algorithmic questions; others seem to be staffed entirely by former competitive programmers. People who believe a skill is undervalued naturally hire people who have it, and the process reinforces itself.

In any case, Big-O notation is not quite what companies want. They mostly want engineers who avoid solutions that become catastrophically slow as the input grows. Compute keeps getting cheaper, and paying for twice as many servers can be more sensible than spending six months replacing working software.

This can be frustrating if you have competitive-programming experience. The asymptotically optimal algorithm often already exists in a library or a paper. The remaining job is to choose a representation, adapt it to the workload, and optimize the constant factor. That requires another set of skills, and only a handful of universities teach them systematically.

## The Levels of Optimization

As a deliberately imprecise classification, programmers can be put at several "levels" according to the cost model they instinctively use:

0. *New programmer*. Gets the program to work and does not think about performance yet. Most software can—and should—be written at this level most of the time.
1. *Undergraduate student*. Knows Big-O notation, standard data structures, and the usual algorithmic techniques. Can tell a scalable solution from one that will collapse on a large input.
2. *Graduate student*. Knows that not all operations are created equal and can use other theoretical models: the word RAM for bitsets, the external-memory model for B-trees and sorting, or work and depth for parallel algorithms.
3. *Professional developer*. Knows approximate real costs. Understands that a cache miss and an integer addition are not comparable, that branch mispredictions are costly, and that memory is transferred in cache lines rather than individual variables.
4. *Performance engineer*. Can connect source code to what the hardware does. Distinguishes latency from throughput, understands execution ports and memory-level parallelism, reads assembly, uses profilers, and writes SIMD when the compiler cannot.
5. *CPU engineer*. Knows undocumented or microarchitecture-specific details that normal programmers should not have to depend on.

In this book, we expect that the average reader is somewhere around stage 1, and hopefully by the end of it will get to 4.

You should also go through these levels when designing an algorithm. First make it correct. Then choose a reasonably asymptotically efficient approach. Then consider its data movement and available parallelism—even a single core is a parallel machine internally. Only then should you tune instructions. Starting at level 4 is not advanced optimization; it is an elaborate way to make the wrong algorithm fast.

## When Performance Matters

For many applications, efficiency is not a product feature. A program that runs once a day for two seconds instead of one does not need an AVX-512 implementation. The optimized version would take longer to write, be harder to review, and probably contain more bugs.

Performance matters when it changes one of four things:

- *Feasibility*: the program must fit within a hard limit, such as a frame deadline, a device's memory, or the duration of an experiment.
- *Latency*: a person or another system is waiting for the result.
- *Throughput and cost*: the operation is repeated often enough that CPU time, memory, power, or servers become material expenses.
- *Quality*: saved computation can be reinvested into a larger search, a better model, a finer simulation, or more game entities.

These cases overlap, but they lead to different optimizations. A web request may have abundant total compute and still need a low tail latency. A batch job may tolerate hours of latency but cost enough at scale that a ten-percent throughput improvement pays for weeks of engineering. An embedded device may be limited by energy rather than either one.

In many online systems, adding machines improves throughput but not the latency of one inherently sequential request. Parallelism can reduce latency only when there is independent work to distribute, and it introduces communication and synchronization costs of its own. This part of the book is concerned mainly with single-core efficiency, which often improves both throughput and latency before a distributed solution is needed.

There are also domains where performance directly buys quality:

- Search and recommendation systems use cascades of progressively slower models. A cheaper stage can pass more candidates to the next one.
- Games and simulations can spend spare cycles on a larger world, better physics, or stronger AI.
- Machine-learning systems can train or serve a larger model under the same time and power budget.
- Compression and databases can trade computation for fewer bytes transferred or stored.

An optimization that makes an infeasible idea feasible can create its own demand. People rarely build on a primitive that takes a minute per call; make it take a millisecond, and suddenly it becomes part of an inner loop.

## Estimating the Impact

Before changing code, estimate the upper bound on the improvement. If a fraction $p$ of the running time can be accelerated by a factor of $s$, Amdahl's law gives the total speedup

$$
S = \frac{1}{(1 - p) + p / s}.
$$

Making a component infinitely fast does not help by more than $1 / (1-p)$. If parsing accounts for 5% of a request, replacing it with a parser that is ten times faster improves the request by only about 4.7%. This is why [profiling](/hpc/profiling/) comes before heroic optimization.

The estimate should include how often the code runs and how long the optimization will remain useful. Saving a nanosecond in a loop executed $10^{15}$ times is a different proposition from saving a second in a migration that runs once. It should also include engineering costs: implementation, validation, portability, code size, and future maintenance.

Sometimes the right optimization is in the calling layer. A faster JSON parser is useful, but not serializing the data as text in the first place can remove parsing altogether. A packed binary format may reduce both CPU work and memory traffic. Caching can remove repeated computation; batching can amortize overhead; changing an API can make copies unnecessary.

The usual order of leverage is:

1. Avoid doing the work.
2. Do less work by changing the algorithm.
3. Arrange the work to move less data and expose parallelism.
4. Make the remaining operations cheaper.

The lower levels of the machine are attractive because they produce satisfying benchmark numbers, but the largest wins usually come from the top of this list.

## An Optimization Loop

A reliable optimization process is short and repetitive:

1. Define a representative workload and a metric.
2. Measure a trustworthy baseline.
3. Profile and form one hypothesis about the bottleneck.
4. Make the smallest change that tests it.
5. Validate correctness, measure again, and keep the change only if it helps.

The metric is part of the specification. Average latency, 99th-percentile latency, throughput, memory footprint, startup time, and energy are different objectives. Optimizing an easy-to-measure proxy can make the actual product worse.

You also need a stopping condition: a performance budget, a deadline, or the point at which the expected saving no longer justifies the added complexity. It is perfectly fine that most software is inefficient. Good engineering is not the production of the fastest possible program; it is spending limited attention where it has the largest effect.

Compiler optimizations, allocators, databases, and widely used libraries are unusually high-leverage targets because their cost is inherited by everything built on top of them. The case studies later in this book—factorization, sorting, search trees—are not all things you will reimplement at work. They are controlled environments for learning techniques that transfer to the code where the stakes are real.
