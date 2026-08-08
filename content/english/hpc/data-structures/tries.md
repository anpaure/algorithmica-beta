---
title: Tries
weight: 5
---

A trie reads a key one character at a time and uses each character to choose the next node. Its running time depends on key length rather than the number of stored strings. This sounds ideal until we remember that each character also creates a dependent memory access: we cannot locate the next node before the current one arrives.

This case study optimizes a static lowercase trie. Packing sparse nodes cuts memory by more than ten times but can make lookup slower. Compressing unary paths removes dependent steps and recovers much of the lost time. On our exact-membership workload, a transparent `std::unordered_set` still wins. A case study is more useful when it discovers that the specialized data structure is not the best answer.

## Workload

We build a set once and answer exact membership queries:

```c++
bool contains(string_view key) const;
```

Keys contain only `a` through `z`. There are no insertions after construction and no prefix enumeration. Half of the queries are present. Each absent query is a stored key with one lowercase character appended. Because every stored key in a dataset has the same length, the longer query is guaranteed to be absent, while the lookup must still traverse the complete stored key before rejecting the suffix. The harness asserts the exact 50/50 split.

Two deterministic synthetic datasets expose different structures:

- **random** keys are 16 characters long and have little prefix sharing;
- **paths** have the form `service<group>endpoint<id>`, are 29 characters long, and share long prefixes in groups of 64.

Synthetic data is not a substitute for an application corpus. It is useful here because it lets us control exactly the property being measured.

Measurements were taken on an Apple M4 Max performance core with 128 KiB of L1 data cache, 16 MiB of cluster L2, and 128-byte cache lines. Apple Clang 17 compiled the code as C++20 with `-std=c++20 -O3 -mcpu=native`. C++20 is required for the heterogeneous `unordered_set::find` baseline. Queries are pre-generated; allocation and construction are outside lookup timing; lookup numbers are medians of seven 300,000-query runs. The [test and benchmark harness](/code/tries_bench.cpp), [raw results](/code/tries_m4_results.txt), and [plot generator](/code/data_structures_plots.py) are included.

## Hash-Table Baseline

For exact membership, a hash table is the baseline a trie has to beat. We use heterogeneous lookup so `find(string_view)` does not allocate a temporary string, reserve twice the key count, and mutate only queries outside the timed region.

At 131,072 keys, `std::unordered_set` takes 10.13 ns per random-key query and 12.65 ns per path query. Hashing scans the query contiguously and then usually performs one table lookup. The memory estimates—56 and 88 bytes per key respectively—include the bucket array, each `string` object, heap character storage only when the string does not use the implementation's small-string buffer, and an estimated two pointers of node overhead. Allocator bookkeeping is not counted. Unlike the custom arrays below, these standard-container figures are estimates rather than allocator-traced totals.

## Dense Trie

The textbook lowercase trie stores 26 child indices in every node:

```c++
struct Node {
    uint32_t child[26] = {};
    bool terminal = false;
};

bool contains(string_view s) const {
    uint32_t v = 0;
    for (char ch : s) {
        v = node[v].child[ch - 'a'];
        if (v == 0)
            return false;
    }
    return node[v].terminal;
}
```

The transition is one indexed load, but transitions form a dependency chain. The node occupies 108 bytes after alignment in the measured implementation, even when it has one child.

Random keys create almost one node per remaining character after their short shared prefix. At 131,072 keys, the dense trie uses 1,404 bytes per stored key and needs 84.6 ns per lookup. The structure is hundreds of megabytes, so nearly every late transition is expensive.

Path keys share most nodes. The same implementation falls to 136 bytes per key and 55.8 ns per query. Layout cost is a property of the data, not just of the class definition.

## Bitmap-Packed Nodes

For a static trie, we can number each node's children consecutively. Store a 26-bit edge mask and the index of the first child. Edge $c$ exists when bit $c$ is set, and its child is

$$
\mathtt{first}[v]+\operatorname{popcount}
\left(\mathtt{mask}[v]\mathbin{\&}(2^c-1)\right).
$$

```c++
bool contains(string_view s) const {
    uint32_t v = 0;
    for (char ch : s) {
        unsigned c = ch - 'a';
        uint32_t m = mask[v], bit = 1u << c;
        if ((m & bit) == 0)
            return false;
        v = first[v] + __builtin_popcount(m & (bit - 1));
    }
    return terminal[v];
}
```

The arrays are stored separately. A transition reads `mask` and `first`; the terminal bitmap is touched only at the end. Breadth-first numbering guarantees that siblings are consecutive.

Packing is an enormous space win. For 131,072 random keys it reduces 1,404 bytes per key to 117; for path keys it reduces 136 to just 11.3.

Performance does not follow space automatically. Random-key lookup improves from 84.6 to 59.2 ns because the dense trie did not fit comfortably in cache. On path keys, packed lookup *regresses* from 55.8 to 80.4 ns. The dense nodes on shared prefixes are hot, so replacing a direct indexed load with mask tests and `popcnt` adds work without avoiding many misses.

![Trie lookup on deterministic random keys](../img/tries-random-m4.svg)

![Trie lookup on path-like keys with shared prefixes](../img/tries-paths-m4.svg)

## Compressing Paths

Most nodes in a trie have one child. If a nonterminal node has exactly one outgoing edge, it contributes a dependent load without making a decision. We collapse maximal unary chains into substring-labeled edges.

Each compressed node still has a child mask and rank calculation. Its edge stores an offset and length in one contiguous label pool:

```c++
struct Edge {
    uint32_t label_offset;
    uint32_t child;
    uint16_t label_length;
};

// after selecting edge e from the node mask
if (p + e.label_length > key.size())
    return false;
if (memcmp(key.data() + p, labels.data() + e.label_offset,
           e.label_length) != 0)
    return false;
p += e.label_length;
v = e.child;
```

This exchanges node transitions for contiguous byte comparisons. On 131,072 random keys, the compressed trie uses 43.2 bytes per key and takes 29.8 ns—roughly twice as fast as the bitmap-packed version. On path keys it uses 27.0 bytes per key and takes 37.5 ns, more than twice as fast as packed lookup.

Compression is not strictly smaller than the packed representation. On path keys, packed nodes need only 11.3 bytes per key because many keys share the same nodes; compressed edges additionally store offsets, lengths, and label bytes. The optimization targets dependency depth, not minimum space.

![Memory usage on the two controlled prefix distributions](../img/tries-memory-m4.svg)

## Attempting to Batch Queries

The processor cannot overlap successive nodes of one query, but it can overlap independent queries. We also timed a loop unrolled over four strings. This simple source-level batching did not produce a reliable improvement:

- random dense lookup changed from 84.6 to 83.7 ns;
- random compressed lookup changed from 29.8 to 31.0 ns;
- path packed lookup changed from 80.4 to 81.5 ns;
- path `unordered_set` changed from 12.6 to 14.3 ns.

Some versions improve slightly and others regress. Four ordinary calls do not guarantee that the compiler will interleave their internal state machines. A true batched trie would explicitly retain four node indices and advance them together. We leave that out rather than presenting an unimplemented optimization as a result.

## Comparison

At 131,072 keys:

| Dataset | Structure | Lookup | Bytes/key |
|:--|:--|--:|--:|
| random | `std::unordered_set` | **10.13 ns** | 56.0* |
| random | dense trie | 84.6 ns | 1,404.3 |
| random | bitmap-packed | 59.2 ns | 117.0 |
| random | path-compressed | 29.8 ns | **43.2** |
| paths | `std::unordered_set` | **12.65 ns** | 88.0* |
| paths | dense trie | 55.8 ns | 135.9 |
| paths | bitmap-packed | 80.4 ns | **11.3** |
| paths | path-compressed | 37.5 ns | 27.0 |

`*` Estimated for the standard container; custom-array sizes are exact.

The packed trie wins memory on shared-prefix data. The compressed trie is the fastest trie. The hash table wins exact-membership time on both datasets.

## When a Trie Is Still the Right Structure

This benchmark asks only exact membership. A trie also supports longest-prefix matching, enumerating all keys under a prefix, lexicographic traversal, and sharing key prefixes without storing every complete string. A hash table does not provide these operations directly.

The workload also uses misses that diverge only after the complete stored key. Random absent strings often fail near the root and make every trie look much faster. Real measurements need the distribution of successful-prefix lengths as well as average key length.

For a byte alphabet, the 26-bit mask becomes four 64-bit words and rank becomes more expensive. Adaptive radix trees use several node layouts for different degrees; that is a promising design, but it is not part of the measured lowercase contract above.

The main result is not that one trie representation wins. Dense nodes minimize transition arithmetic, packed nodes minimize the working set, and compressed paths minimize dependency depth. Those are separate objectives, and this experiment found a workload where each one matters.
