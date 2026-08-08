---
title: Models of Computation
weight: 3
---

In classical theoretical computer science, really exciting things stopped happening in the 70s; everything past that are just attempts to replace logarithms in the asymptotic with something slightly less than logarithms. If 50 years ago such algorithms had hope that eventually there will be enough computing power to process the large datasets for which they beat their asymptotically inferior, but practical counterparts, nowadays we know for certain that they never will.

This is what this book is about: accepting the reality and optimizing for the hardware you have, beyond just asymptotic complexity. You will probably not learn a single asymptotically faster algorithm here, but you will learn how to squeeze performance from all of non-exponentially-increasing transistors you have, which is a far more impactful skill.

Computers are still getting faster, but mostly in ways orthogonal to the classical computation model. A modern processor does not execute one abstract operation after another. It overlaps them, guesses which ones will be needed, moves data through several layers of memory, and applies one instruction to several values at once. New computers are not much *taller* than old ones; they are broader.

This does not make asymptotic analysis obsolete. It makes its assumptions visible.

## The RAM Model

The model normally implied by a Big-O analysis is the *random-access machine*. It gives the algorithm an infinite array of memory cells and charges one unit of time for reading or writing any cell and for performing an elementary arithmetic operation.

Under these rules, summing an array of $n$ integers costs $\Theta(n)$, and so does following a linked list of the same length. Binary search costs $\Theta(\log n)$ regardless of where the array is stored. Multiplication is one operation whether its operands occupy 8 bits or eight gigabytes.

These are deliberately unrealistic assumptions. The point of a model is not to reproduce a computer atom by atom, but to discard details that are irrelevant to the question being asked. The RAM model is excellent at separating insertion sort from merge sort and breadth-first search from an algorithm that inspects every pair of vertices. It is considerably less helpful for choosing between two linear scans.

There is also a small technical problem with giving each cell an arbitrary amount of information. If an unbounded integer still fits into one cell, it can encode an entire input, and a sufficiently creative "constant-time" operation can solve almost anything. The usual repair is the *word RAM* model.

### Word RAM

In the word RAM model, a memory cell contains a $w$-bit *word*, usually with $w = \Theta(\log n)$ so that an input position fits into one word. Arithmetic, comparisons, shifts, and bitwise operations on words cost $O(1)$; larger objects have to be represented by several words and processed accordingly.

This tiny change gives the machine a useful amount of parallelism. A single bitwise instruction can operate on $w$ booleans, so a bitset intersection of length $n$ takes $O(n / w)$ word operations rather than $O(n)$ scalar ones. Integer sorting, predecessor search, and many succinct data structures have bounds that cannot even be stated faithfully in the plain comparison model.

The word RAM is much closer to real hardware, but it still calls every word operation equally expensive. On a typical CPU, integer addition may have a latency of one cycle, division may take a few dozen, and a load may take anywhere from a few cycles to a few hundred depending on where the data happens to be. This difference is invisible to the model.

## Charging for Data Movement

For large computations, moving data is often more expensive than transforming it. The *external-memory model* makes this the only thing that costs anything.

It gives the machine a fast memory capable of holding $M$ words and an arbitrarily large slow memory. Data moves between them in blocks of $B$ consecutive words, and the complexity of an algorithm is the number of such block transfers. Computation on data already in fast memory is free.

This explains several facts that look strange in the RAM model:

- Scanning $n$ consecutive elements costs $\Theta(\lceil n / B \rceil)$ transfers, not $\Theta(n)$.
- Following $n$ unrelated pointers may cost $\Theta(n)$ transfers, even though it performs the same number of RAM accesses as the scan.
- A B-tree has a high branching factor because one node is chosen to occupy one block. Its search cost is $O(\log_B n)$ transfers.
- For $n \geq M \geq 2B$, comparison sorting can be done in $O(\left(n / B\right) \log_{M/B}\left(n / B\right))$ transfers by merging many runs at once; smaller inputs need only a scan.

Real machines have more than two memory levels, and their blocks are called cache lines, pages, and disk sectors depending on the boundary being crossed. The two-level model is nevertheless useful because the same analysis can be applied to each adjacent pair. [Cache-oblivious algorithms](/hpc/external-memory/oblivious/) go one step further: they are written without knowing $M$ or $B$, yet arrange their work so that the analysis holds at every level of the hierarchy.

The model also explains why layout is part of an algorithm. An array and a linked list may represent the same abstract sequence, but they induce radically different sequences of block transfers. Once moving data is charged separately, choosing a representation is no longer an implementation detail.

## Charging for Dependencies

Another family of models tries to capture parallelism. Suppose an algorithm performs $W$ operations in total, but the longest chain of operations that depend on one another has length $D$. These quantities are traditionally called *work* and *depth* (or *span*).

Even with $p$ processors, its running time cannot be lower than

$$
T_p \ge \max\left(\frac{W}{p}, D\right).
$$

The first term says that the work has to be done by somebody; the second says that adding processors cannot shorten a dependency chain. Subject to some scheduling assumptions, this lower bound is also achievable within a constant factor. This is the central idea behind PRAM models and the work-span analysis used by parallel runtimes.

The same reasoning applies *inside one CPU core*. A core has several execution units and can begin multiple independent instructions per cycle, but it cannot parallelize

```cpp
for (int i = 0; i < n; i++)
    x = f(x);
```

because every iteration needs the result of the previous one. If there are several independent accumulators, the processor can work on them simultaneously. In this book, we call this [instruction-level parallelism](/hpc/pipelining/), but mathematically it is the same distinction between work and depth.

Parallel machines also need to communicate. Depending on the problem, it may be more useful to count cache-line exchanges between cores, messages between computers, synchronization rounds, or the number of bits communicated. An algorithm with optimal work can still be useless if all processors contend for one memory location or exchange the entire input after every step.

## A Machine Has Several Prices

Each of these models normally charges for one scarce resource:

- the RAM model charges for operations;
- the external-memory model charges for block transfers;
- the work-span model charges for work and dependencies;
- communication models charge for messages, rounds, or transmitted bits.

A physical CPU charges for all of them at once. It has a finite instruction-decoding rate, a finite number of execution ports, a branch predictor of finite accuracy, several finite caches, and memory channels with finite bandwidth. These limits can overlap, so their costs cannot in general be added into one honest price per instruction.

Consider a loop that performs $a$ arithmetic operations and transfers $b$ bytes per element. Its *arithmetic intensity* is $I = a / b$. If the machine can perform at most $P$ operations per second and transfer at most $R$ bytes per second, then the loop cannot exceed

$$
\min(P, I R)
$$

operations per second. Increasing arithmetic throughput does nothing once the loop is limited by memory bandwidth; reducing traffic does nothing once it is limited by computation. This simple observation, usually called the *roofline model*, is more useful than assigning a fixed cost to each addition and load.

Latency and throughput need to be separated for the same reason. One cache miss may take a hundred nanoseconds, but ten independent misses do not necessarily take ten times as long: the memory system can service several at once. Conversely, a chain of ten dependent additions cannot use ten arithmetic units. The relevant cost depends not just on *which* operations the program performs, but on the dependency graph between them.

## Choosing a Model

There is no universally correct model of computation. A model is useful when it preserves the bottleneck you care about and removes everything else.

For a first design, the RAM model is usually the right tool. It prevents an accidental quadratic algorithm from surviving long enough for constant-factor optimization to matter. When the input stops fitting in cache, count cache lines or pages. When the work can overlap, count dependencies and communication. When two implementations have the same high-level costs, inspect the generated machine code and measure them.

This suggests a practical order of analysis:

1. Use asymptotic complexity to discard algorithms that do fundamentally too much work.
2. Choose a more specific model for the likely bottleneck: words, cache lines, dependencies, bandwidth, or something else.
3. Estimate the important constants using instruction tables and hardware parameters.
4. Benchmark the actual implementation and verify the hypothesis with profiling counters.

The last step is not an admission that theory failed. Measurements are simply another model—one that includes the compiler, operating system, and the particular piece of silicon on your desk, but says less about other inputs and other machines.

There are many more specialized frameworks. Cryptographic algorithms count expensive group operations and adversarial queries. Circuit designers count gates, wires, area, delay, and energy. Distributed algorithms count failures and synchronization rounds. Quantum algorithms count queries and qubits. Human-executed procedures even have to account for mistakes.

Throughout this book, we will switch models whenever the bottleneck changes. The important habit is not to pledge allegiance to one of them, but to ask what resource an algorithm is really consuming—and whether the machine has a way to consume several of them in parallel.
