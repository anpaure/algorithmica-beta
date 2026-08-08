---
title: Preface
weight: -1
ignoreIndexing: true
---

There are two influential textbooks on computer science: one was written 30 years ago, and the other was written 50 years ago. Computers were very different back then.

Their central abstraction—counting elementary operations and ignoring how a machine performs them—remains one of the most useful ideas in computer science. It is also no longer sufficient by itself. Modern processors get much of their speed from parallelism, speculation, vector instructions, and a deep memory hierarchy. Two implementations that perform the same number of abstract operations can differ by orders of magnitude because only one of them fits the hardware.

This book is about that difference. It studies the design and implementation of fast algorithms on modern computers, starting with a single CPU core. The objective is not to collect isolated tricks, but to build a model that lets you predict what will be slow, verify the prediction, and improve it without relying on folklore.

A lot of this book discusses hardware. I am a software developer with no formal training in hardware, so on some occasions I might be misleading or wrong. All I am trying to do is to help you build a useful mental model.

The model is intentionally simpler than a processor manual. We will introduce hardware details when they explain a measurable effect and omit them when they do not change how an algorithm should be written. Whenever the model and the measurement disagree, trust the measurement—and then find out which assumption was wrong.

## Prerequisites

The intended audience ranges from performance engineers and practical algorithm researchers to undergraduate computer science students who have finished an algorithms course and want to learn more practical ways to speed up a program than going from $O(n \log n)$ to $O(n \log \log n)$.

You should be comfortable with basic algorithms and data structures, asymptotic notation, and systems programming in C or C++. Some examples use x86-64 assembly and SIMD intrinsics, but neither is a prerequisite: the relevant instructions are introduced as they appear. Familiarity with operating systems, computer architecture, probability, and linear algebra is helpful in individual chapters, but the necessary parts are reviewed along the way.

Most examples target Linux, GCC or Clang, and a recent x86-64 processor. The underlying ideas—data locality, dependencies, throughput, vectorization—are not specific to that platform. Exact instruction names, counter events, and benchmark results are. You should rerun the experiments on the machine you care about rather than treating the printed numbers as physical constants.

### How to Read This Book

There are many forward references I could not get rid of. Performance effects interact: vectorization changes memory traffic, layout changes vectorization, and compilation changes both. The book is ordered so that the main path introduces dependencies before they are needed, but it is also meant to be followed non-linearly.

Chapter 1 is a "why you should care" sort of read.

Chapter 2 is an introduction to computer architectures from the perspective of performance. There is a high chance that you already know it from a college course, but I still advise to read it to get into context, as we will cover assembly-level optimization techniques there.

Chapter 3 is where experienced programmers should start from.

Chapter 4 discusses compilation with the example of C++ and GCC/Clang. Chapter 5 discusses language-agnostic profiling methods. You are free to skip both.

Chapter 6 discusses arithmetic, and Chapter 7 discusses modular arithmetic and its applications. They also act as a reference for algorithms in the case studies.

Chapter 8 introduces the external memory model and how the memory system works. Chapter 9 follows up with experimental studies of how it can affect performance.

Chapter 10 discusses SIMD programming, which is a major part of the book. It is not *that* intertwined with the previous chapters, and if you are already comfortable with architecture and memory, you can start there to learn powerful techniques right away.

The first five chapters build a general understanding of performance. Chapters 6–10 develop specific tools involving arithmetic, number theory, memory, and SIMD. Chapters 11 and 12 contain extended case studies of algorithms and data structures. Performance engineering is a practical field, and the case studies are where the techniques stop being independent tricks and start becoming a design process.

If you are reading for a course, follow the chapters in order and reproduce the benchmarks. If you are solving a concrete performance problem, begin with [profiling](/hpc/profiling/), read the chapter corresponding to the bottleneck, and then find a similar case study.

## Experiments and Code

Small code fragments in the text are designed to expose one idea and may omit error handling or portability machinery. Complete benchmark programs are kept in the [code repository](https://github.com/sslotin/scmm-code). Build them with the documented compiler and flags, pin or otherwise isolate the benchmark when possible, and check the generated assembly before attributing a result to the source code.

Performance numbers are observations, not promises. They depend on the processor, compiler, input distribution, alignment, temperature, operating system, and what else the machine happens to be doing. A useful experiment states enough of these conditions to be reproduced and reports the distribution of repeated measurements rather than one unusually pleasing run.

Correctness comes before timing. Optimized kernels are checked against a simple reference implementation, including edge cases that do not occur in the headline benchmark. Compiler flags such as `-ffast-math` deliberately change language guarantees; when they are used, the changed contract is part of the algorithm.

## Scope

In the first part, we are mainly concerned with single-core CPU programming. Operating systems, networking, GPUs, multicore synchronization, and distributed computing appear only where they affect that subject. The focus is on algorithms and data structures rather than application tuning, although the same methods apply to both.

The book does not try to replace an architecture manual, a compiler reference, or a numerical-analysis textbook. It aims to connect the parts of those subjects that determine the performance of real programs.

## Acknowledgements

A lot of illustrations are borrowed from other places on the internet. They are meant to be temporary placeholders.

Many readers have reported errors, reproduced experiments on new hardware, and contributed corrections. Their work is recorded in the repository history. Any remaining mistakes are mine.

## How to Support

All materials are hosted on GitHub, with code in a [separate repository](https://github.com/sslotin/scmm-code). This is not a collaboratively authored project, but corrections, reproducible counterexamples, and other feedback are very welcome. The most useful way to support it is to read critically, report what is wrong, and share the parts that helped you.
