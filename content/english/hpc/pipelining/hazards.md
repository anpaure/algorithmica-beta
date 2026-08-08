---
title: Pipeline Hazards
weight: 1
published: true
---

[Pipelining](../) lets you hide the latencies of instructions by running them concurrently, but also creates some potential obstacles of its own — characteristically called *pipeline hazards*, that is, situations when the next instruction cannot execute on the following clock cycle.

There are multiple ways this may happen:

* A *structural hazard* happens when two or more instructions need the same part of CPU (e.g., an execution unit).
* A *data hazard* happens when you have to wait for an operand to be computed from some previous step.
* A *control hazard* happens when a CPU can't tell which instructions it needs to execute next.

When the hardware cannot avoid or hide a hazard, it causes a *pipeline stall*: younger instructions stop advancing until the cause of congestion is gone. This creates *bubbles* in the pipeline — analogous with air bubbles in fluid pipes — a time-propagating condition when execution units are idling and no useful work is done.

![Pipeline stall on the execution stage](../img/bubble.png)

Different hazards have different penalties:

- In structural hazards, you have to wait (usually one more cycle) until the required execution unit is ready. The amount of each hardware resource is a fundamental throughput limit, so you have to engineer around it.
- In data hazards, you have to wait for the required data to be computed (the latency of the *critical path*) unless forwarding, register renaming, or out-of-order execution can hide the dependency. At the algorithm level, they are addressed by restructuring computations so that the critical path is shorter.
- In control hazards, a misprediction generally forces the processor to discard the younger speculative work, often wasting 15–20 cycles on a modern desktop CPU. They are addressed by either removing branches completely or making them predictable so that the CPU can effectively *speculate* on what is going to be executed next.

As they have very different impacts on performance, we are going to go in the reversed order and start with the more grave ones.
