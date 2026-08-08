---
title: B-Trees
weight: 9
---

A balanced binary search tree performs $O(\log_2 N)$ comparisons. This is optimal up to a constant factor if comparisons are what we pay for. It is quite bad if every node is stored on a different disk page: we read a whole block, use one key and two pointers from it, and then issue another random read.

A B-tree uses the block we have already paid for. Each node stores many sorted keys and many children, so one block read makes much more progress down the tree.

## Layout

A B-tree node with $B$ keys has up to $B+1$ children:

$$
\underbrace{c_0}_{x \lt k_0},\, k_0,\,
\underbrace{c_1}_{k_0 \le x \lt k_1},\, k_1,\, \ldots,\, k_{B-1},\,
\underbrace{c_B}_{k_{B-1} \le x}.
$$

The keys split the ordered set into $B+1$ ranges. Once a node has been read, we find the range containing the query and continue with the corresponding child.

In a simplified fixed-size implementation, the in-memory representation could look like this:

```cpp
const int B = 255;

struct node {
    int size;
    int leaf;
    uint64_t key[B];
    uint64_t child[B + 1];
};

uint64_t select_child(node *v, uint64_t x) {
    int i = 0;
    while (i < v->size && x >= v->key[i])
        i++;
    return v->child[i];
}
```

Here `child[i]` is a page number, not a machine pointer. The buffer manager turns it into either a cached address or a block read. A real page also contains its type, checksum, free-space offsets, and other bookkeeping, so $B$ has to be chosen to make the complete node fit in one page.

We used linear search inside a node because computation is free in the [external memory model](../model). In real code, binary search, interpolation, or SIMD may be faster depending on the number and size of keys. This choice changes CPU time but not the number of I/O operations.

## Height

If every internal node has $B+1$ children, a tree of height $h$ contains on the order of

$$
(B+1)^h
$$

keys. Therefore a lookup takes

$$
O(\log_B N)
$$

block reads instead of $O(\log_2 N)$.

The tree is not always perfectly full. The usual invariant requires every non-root node to be at least half full. If the minimum number of children is $t$, then a nonempty tree of height $h$ contains at least

$$
2t^h-1
$$

keys, and hence

$$
h \le \log_t \frac{N+1}{2}.
$$

The root is allowed to contain fewer keys. This exception is harmless because there is only one root, and it is usually cached anyway.

For a 4KB page, 8-byte keys, and 8-byte child identifiers, the fanout is on the order of a few hundred after accounting for metadata. A tree holding billions of records then has only a few levels. More importantly, the upper levels are small enough to remain in RAM, so a lookup may need external I/O only for its last one or two nodes.

## Insertion

Searching is easy; updates are the reason B-trees have empty space.

To insert a key, we descend to its leaf and place it in sorted order. If the leaf overflows, we split it into two roughly equal nodes and insert a new separator into the parent. The parent may now overflow too, in which case the split continues upward. Splitting the root creates a new root and increases the height by one.

```text
before:       [ 1  3  5  7  9 ]

insert 6:     [ 1  3  5  6  7  9 ]

after split:  [ 1  3  5 ]  [ 6  7  9 ]
                         ^
                 separator copied to parent
```

Only the nodes on one root-to-leaf path can split, so insertion performs $O(\log_B N)$ I/O operations in the worst case. Most insertions modify just one leaf.

Deletion is symmetric. If a node becomes too empty, it first borrows keys from a sibling; if that is impossible, the siblings merge and a separator is removed from the parent. The half-full invariant prevents the tree from degenerating into a linked list of nearly empty pages.

Leaving pages half empty is not always the best practical policy. Random insertions need some free space to avoid immediate splits, while a read-mostly index benefits from denser pages. With sorted input, we can *bulk load* the tree: fill leaves from left to right and build each parent level after its children. Once the input is sorted, this takes only $O(N/B)$ block writes.

## B+ Trees

Most database indexes use a closely related structure called a *B+ tree*:

- internal nodes contain only separators and child page numbers;
- complete records, or pointers to them, are stored only in the leaves;
- leaves are linked in sorted order.

Keeping records out of internal nodes increases their fanout. Linked leaves also make range queries simple: find the first key with one root-to-leaf lookup and then scan consecutive leaf pages.

Returning $K$ consecutive records takes

$$
O\left(\log_B N + \frac{K}{B}\right)
$$

I/O operations. The first term locates the range, and the second reads it. This is why ordered database indexes are useful even when a hash table could answer isolated point queries.

The separator stored in an internal node does not always need to contain the full key. If adjacent child ranges begin with long common prefixes, a shorter distinguishing prefix may be enough. Shorter separators increase fanout, but the comparison rule becomes part of the persistent file format and has to agree exactly with the ordering used in the leaves.

## Page Layout

The fixed C++ struct above is useful for understanding the algorithm, but it is rarely a good disk format. Keys may have variable length, compiler padding is not stable, and pointers do not survive reopening the file.

A common *slotted page* stores a small header and an array of offsets at the beginning of the block, while variable-sized key data grows backwards from the end. Moving a key then changes an offset rather than every following record. Compaction can reclaim the holes left by deletions.

Persistent updates also need a recovery protocol. A split changes at least two children and their parent; a crash between these writes must not leave half of the update visible. Databases solve this with write-ahead logging or copy-on-write pages. This machinery is important, but it is separate from the B-tree's main algorithmic idea.

That idea can be summarized in one sentence: make one search-tree node the size of the block that the memory system transfers anyway. A binary tree minimizes comparisons; a B-tree minimizes expensive memory transfers.

## Further Reading

Bayer and McCreight introduced B-trees in [*Organization and Maintenance of Large Ordered Indexes*](https://doi.org/10.1007/BF00288683). The [SQLite file format](https://sqlite.org/fileformat.html) is a readable example of how interior and leaf pages are represented in a real database.
