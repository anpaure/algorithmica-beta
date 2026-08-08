---
title: Tries
weight: 5
---

A comparison-based search structure gets information about the key by comparing it with other keys. A *trie* does something simpler: it reads the key one character at a time and uses each character to choose the next node.

For a set of strings, this makes the running time proportional to the length of the query rather than to the number of stored strings. But the textbook implementation also performs one dependent memory access per character, which is not a particularly good way to use a modern CPU.

In this article, we will keep the algorithm and try to fix its memory layout.

## The Textbook Implementation

Assume for now that the strings consist only of lowercase English letters. We can store an array of 26 child indices in every node:

```c++
const int A = 26;
const int N = 1e6; // maximum number of nodes

int child[N][A];
bool terminal[N];
int nodes = 1;     // node 0 is the root; index 0 also means "no child"

void insert(const char *s) {
    int v = 0;
    while (*s) {
        int c = *s++ - 'a';
        if (child[v][c] == 0)
            child[v][c] = nodes++;
        v = child[v][c];
    }
    terminal[v] = true;
}

bool find(const char *s) {
    int v = 0;
    while (*s) {
        int c = *s++ - 'a';
        v = child[v][c];
        if (v == 0)
            return false;
    }
    return terminal[v];
}
```

The code assumes that the input is valid and that at most $N$ nodes are created. This is intentional: checks that are unrelated to the data structure only obscure the part we want to study.

Lookup performs one indexed load per character, but these loads form a dependency chain: we do not know where to read the next node until the current load finishes. [Out-of-order execution](/hpc/pipelining/) can overlap different queries, but it cannot look far ahead in one query.

The representation also wastes a lot of space. A node takes at least $26 \times 4=104$ bytes even when it has only one child, which is the common case for long strings. For a byte alphabet, the same idea would need 256 pointers per node and become almost unusable.

## Packing Static Tries

When the set of strings does not change, we can build the trie in any convenient representation and then pack it.

For each node, store a 26-bit mask of the existing edges and place all its children consecutively, in alphabetic order. If `first[v]` is the index of the first child, the edge labeled $c$ leads to

$$
\mathtt{first}[v] + \operatorname{popcount}(\mathtt{mask}[v] \bmod 2^c).
$$

In code, the complete transition procedure is:

```c++
unsigned mask[N]; // bit c is set iff the edge labeled c exists
int first[N];     // children are stored consecutively in increasing c

int next(int v, int c) {
    unsigned m = mask[v];

    if ((m >> c & 1) == 0)
        return -1;

    unsigned lower = m & ((1u << c) - 1);
    return first[v] + __builtin_popcount(lower);
}
```

The shift is safe because $0 \le c < 26$. The packing pass can assign the nodes breadth-first: when processing a node, append all its children and remember where that interval begins.

Compared to the dense layout, a transition now needs a `popcnt`, but the node metadata shrinks from 104 bytes to roughly a dozen. This is usually a good exchange when the trie is too large for cache: arithmetic is cheap, while every extra cache line participates in the dependency chain.

For a byte alphabet, the mask becomes four 64-bit words. We first select the word containing $c$ and add the population counts of the preceding words. The idea stays the same, but high-degree nodes become more expensive to search.

## Different Nodes Need Different Layouts

No single representation is best for every degree.

- A node with one child only needs the character and one index.
- A small sorted array of `(character, index)` pairs is compact and can be searched with a short linear scan.
- A bitmap plus `popcnt` works well for medium-degree nodes.
- A full table is wasteful for sparse nodes but ideal for nodes with almost every possible child.

[Adaptive radix trees](https://db.in.tum.de/~leis/papers/ART.pdf) use exactly this observation, switching between several node formats as the degree grows. This adds a type check to each transition but greatly reduces the working set.

Another important transformation is *path compression*. If several consecutive nodes all have one child, we replace them with one edge labeled by the whole substring. This turns several dependent node loads into one load followed by a contiguous string comparison. A trie with compressed paths is usually called a *radix tree*; PATRICIA is its bitwise variant.

## Finding Parallelism

Even after packing, a single lookup still has an unavoidable dependency between consecutive characters. To get memory-level parallelism, we can search for several strings at once, interleaving their transitions. The chains are independent, so the processor can have several node loads in flight. This improves throughput, although the latency of one lookup stays almost unchanged.

The same layout decision applies to prefix queries. Breadth-first packing keeps siblings together, which is good for choosing the next edge. If we mostly enumerate all strings with a given prefix, a depth-first layout is better because it keeps the whole subtree contiguous. The correct layout is determined not only by the data structure but also by the query that moves the most memory.
