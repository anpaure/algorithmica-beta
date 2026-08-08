---
title: Memory Management
weight: 11
---

Allocating memory looks like one operation in C++:

```cpp
node *p = new node;
```

It is not one operation in the machine. `new` asks a user-space allocator for storage and then constructs an object there. The allocator usually serves the request from memory it already owns; only occasionally does it ask the operating system for more virtual pages. The operating system may postpone assigning physical memory until the program first writes to those pages.

This is why both of the following statements can be true:

- allocating a small object can be expensive without making a system call;
- reserving a huge virtual region can be cheap until it is touched.

For performance work, it is useful to separate object construction, allocation inside the process, virtual address space, and resident physical pages. They are related, but they are not the same resource.

## General-Purpose Allocation

Calling the kernel for every 24-byte object would be absurdly expensive and waste most of every page. A general-purpose allocator requests memory in large chunks and splits it into smaller blocks.

Small requests are normally rounded to one of several *size classes*. For example, a 25-byte object may occupy a 32-byte slot. Allocation then becomes removing one slot from a free list; deallocation puts it back. When a free list is empty, the allocator refills it from a larger pool.

Large requests are treated differently and may receive complete runs of pages. Adjacent free runs can later be merged. Multithreaded allocators also keep per-thread or per-CPU caches so that the common path does not need a global lock.

This hierarchy makes `malloc` fast enough for general use, but it has costs:

- rounding to a size class wastes space inside a slot;
- metadata occupies additional memory and cache lines;
- free memory may be split between incompatible sizes or different thread caches;
- a block freed by one thread may have to return to the cache of another;
- independently allocated objects are usually worse for locality than one contiguous array.

The last point is often more important than the allocation call itself. Replacing a million separately allocated tree nodes with a few contiguous arrays removes a million allocations, but it also removes pointer chasing and packs useful data into fewer cache lines and pages.

## Arenas

Many programs allocate objects one by one and destroy all of them at the same time. A compiler creates syntax nodes for one translation unit; a server creates temporary objects for one request; a graph algorithm builds scratch data for one run.

In this case, maintaining an independent lifetime for every object is unnecessary. An *arena allocator* advances one pointer through a large region and releases the whole region at once:

```cpp
const int M = 1 << 20;

alignas(64) char memory[M];
size_t used = 0;

void *allocate(size_t n, size_t alignment = 8) {
    used = (used + alignment - 1) & ~(alignment - 1);
    void *p = memory + used;
    used += n;
    return p;
}

void release_all() {
    used = 0;
}
```

This example intentionally exposes the whole algorithm. It assumes that `alignment` is a power of two, does not exceed 64, and that `used + n <= M`. A real arena checks or guarantees these conditions and obtains another large block when the current one is full.

Allocation now consists of an addition, a mask, and another addition. Individual deallocation does nothing. More importantly, objects created around the same time are placed near each other, which is often also how they are traversed.

An arena only allocates raw storage. C++ object lifetimes still apply: constructors must be run when objects are created, destructors must be run when required, and no pointer may be used after `release_all`. The speed comes from the stronger lifetime contract, not from pretending that these rules do not exist.

## Pools

If objects have independent lifetimes but all have the same size, a *pool allocator* is a better fit. A free block can store the pointer to the next free block in its own unused bytes:

```cpp
struct block {
    block *next;
};

block *free_list = 0;

void *allocate_block() {
    if (free_list == 0)
        free_list = allocate_page_of_blocks();

    block *p = free_list;
    free_list = free_list->next;
    return p;
}

void free_block(void *p) {
    block *q = (block*) p;
    q->next = free_list;
    free_list = q;
}
```

Here every block is assumed to be aligned for a pointer and large enough to store one. `allocate_page_of_blocks` divides a new page into blocks and links them together. With these preconditions, both operations are constant-time and touch only the first word of the block.

Separate pools can be used for several common sizes. Continue this far enough, add synchronization, page reclamation, and handling for large allocations, and we have reinvented the beginning of a general-purpose allocator. Specialization is useful only while the application provides a simpler contract.

## Fragmentation

There are two kinds of wasted space.

*Internal fragmentation* is unused memory inside allocated blocks. Serving a 25-byte request from a 32-byte slot wastes seven bytes, and alignment may add more.

*External fragmentation* is free memory divided into the wrong shapes. An allocator may own many free bytes but no large contiguous run. At the page level, one long-lived object can keep an otherwise empty page resident.

Allocation order matters because lifetime order matters. If one long-lived object is allocated between many short-lived ones on every page, freeing the short-lived objects may return almost no physical memory to the operating system. Grouping objects by lifetime often helps more than changing the free-list search algorithm.

Calling `free` also does not imply that resident memory immediately decreases. The block may remain in a thread cache or a partially occupied page so that the next allocation can reuse it. Releasing pages reduces memory footprint but makes future allocation pay for mapping and page faults again.

## Remove the Allocation

Before replacing the allocator, check whether the allocation is necessary at all. The usual improvements are simple:

- store objects by value when stable addresses are not required;
- use one contiguous array instead of separately allocated nodes;
- reserve capacity when the final size is approximately known;
- reuse buffers between iterations;
- store variable-sized data in one byte array and refer to it by offsets;
- allocate objects with the same lifetime from one arena.

These changes reduce allocator work and improve cache and TLB locality at the same time. Swapping one general-purpose allocator for another can help a particular workload, but it cannot make scattered data contiguous after the caller has requested scattered objects.

To find out what matters, measure the number and sizes of allocations, which threads allocate and free them, how long objects live, and how many bytes remain resident. Then measure the surrounding operation. An allocator that wins a microbenchmark can still make the program slower by increasing fragmentation or cache misses.

At the bottom of the hierarchy, memory is managed in pages. Touching one byte on every page creates page faults and translation entries; touching the same number of bytes contiguously may need only a few pages. Memory management is therefore not just the cost of `malloc`. It is the choice of layout and lifetime that determines how much of the [memory hierarchy](../hierarchy) the program has to use.
