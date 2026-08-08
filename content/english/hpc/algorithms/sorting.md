---
title: Sorting
weight: 14
---

There is no fastest sorting algorithm in the same sense that there is no fastest vehicle.

If the elements are arbitrary objects and we can only compare them, we have one problem. If they are 32-bit integers, we have another. Stability, input distribution, record size, available memory, and what we do with the sorted data afterwards can all change the answer.

For general C++ objects, `std::sort` is a very strong baseline. Instead of trying to reproduce an entire standard-library sort, we will look at the one piece of information it cannot use: the bits of an integer key.

## Comparison Sorting

A comparison gives us at most one bit of information. To distinguish all $n!$ permutations of $n$ distinct values, a comparison sort needs at least

$$
\log_2(n!)=n\log_2n-O(n)
$$

comparisons in the worst case.

Quicksort is usually fast because its partitioning pass is sequential and in-place, but careless pivot selection gives it a quadratic worst case. Heapsort has a guaranteed $O(n\log n)$ bound but poor locality. Insertion sort is quadratic and nevertheless excellent for tiny arrays. Most practical implementations combine ideas like these rather than remaining faithful to one textbook algorithm.

This already gives us two useful rules:

1. Compare specialized code against `std::sort`, not against a naive quicksort.
2. If we want to escape $n\log n$, we need to use more information than comparisons reveal.

Fixed-width integers give us all their bits.

## Counting One Digit

Counting sort works when keys come from a small range. Count how many elements have each key, calculate where every bucket begins, and then place the elements into their buckets.

A 32-bit integer has too many possible values for one counting array, but each of its bytes has only 256. We can stably counting-sort by one byte at a time. This is *radix sort*.

Suppose we first sort by the lowest byte, then by the next byte. After the second pass, values are ordered by their lowest 16 bits — but only if the second pass preserves the order of equal second bytes. This is why every pass has to be stable.

```c++
void radix_sort(vector<uint32_t> &a) {
    vector<uint32_t> buffer(a.size());

    for (int shift = 0; shift < 32; shift += 8) {
        size_t count[256] = {};
        size_t position[256];

        for (uint32_t x : a)
            count[(x >> shift) & 255]++;

        position[0] = 0;
        for (int digit = 1; digit < 256; digit++)
            position[digit] = position[digit - 1] + count[digit - 1];

        for (uint32_t x : a) {
            int digit = (x >> shift) & 255;
            buffer[position[digit]++] = x;
        }

        a.swap(buffer);
    }
}
```

There are four passes, each linear, so for 32-bit keys the running time is $O(n)$. More honestly, it is $O(4n+4\cdot256)$ with one auxiliary array of $n$ integers.

The four swaps do not move the elements — a `vector` swap only exchanges the two buffers. After each swap, `a` owns the output of the latest pass; after an even number of passes it even owns its original allocation again.

### Signed integers

Sorting the bit patterns of signed integers as unsigned places all negative numbers after the positive ones. On the usual two's-complement representation, flipping the sign bit fixes the order:

```c++
uint32_t key(int32_t x) {
    return uint32_t(x) ^ 0x80000000u;
}
```

Use this transformed key only for selecting the digit; keep the original value as the payload. Floating-point numbers need a related but slightly more complicated transformation, plus an explicit policy for NaNs and signed zero.

## Choosing the Radix

Eight bits per pass is not sacred. With $r$ bits per digit, we perform roughly $32/r$ passes and need $2^r$ counters.

- Small digits mean more complete passes over the input.
- Large digits mean a larger histogram that is slower to clear and may not fit in L1 cache.
- A larger radix also creates more simultaneous output streams during distribution, which is harder for the cache and write-combining machinery.

For example, a 16-bit digit reduces the algorithm to two passes but needs 65,536 counters. That histogram alone occupies hundreds of kilobytes, compared with just a few kilobytes for 256 `size_t` counters. Depending on the array size and processor, saving two passes may or may not compensate for losing the small hot histogram.

The count loop repeatedly increments locations in this histogram. A popular low-level optimization is to unroll the loop and maintain several independent histograms, merging them afterwards. This breaks the dependency chain when consecutive keys have the same digit, at the cost of more cache space and a final reduction. As always, it is useful only after the count phase has actually appeared in a profile.

## The Memory Cost

Radix sort avoids comparisons, but it moves a lot of data. Each pass reads the input once to build the histogram, reads it again to distribute the values, and writes an entire output array. For a large array, the algorithm is mostly a memory-bandwidth problem.

This is why radix sort is not automatically faster than `std::sort`:

- small arrays do not amortize clearing the histograms;
- large records are expensive to copy on every pass;
- allocating another full-size array may be unacceptable;
- comparison sorting can be in-place and may finish in fewer memory passes;
- an expensive comparison or a cheap integer key strongly favors radix sorting.

For records, we can sort compact indices or key-pointer pairs instead of moving the entire payload. This reduces traffic during sorting but introduces an extra indirection when the result is consumed. Whether that is a win depends on the next stage of the program, not just the sort benchmark.

## Stability and Records

The implementation above is stable because it scans the input from left to right and advances each bucket position in the same order. For plain integers this property is invisible, but it matters for records with equal keys.

If stability is not required, other radix layouts can partition in place and save the auxiliary array. They usually become more complicated: we need to follow cycles between buckets, deal with small subranges, and retain good locality. The stable out-of-place version is a better starting point because every pass is a pair of simple streaming loops.

Comparison sorting has the opposite default in the C++ library: `std::sort` is not stable, while `std::stable_sort` is. Adding the original index as a tie-breaker changes both the key and the amount of data moved; stability is not free just because the final order can be described that way.

## Benchmarking Sorting

Never benchmark a sort by repeatedly sorting the same array. After the first iteration, the input distribution has changed. Generate or restore the data outside the timed region and verify that the result is ordered after every implementation.

At minimum, test:

- uniformly random full-width keys;
- already sorted and reverse-sorted arrays;
- many equal keys and a very small key range;
- nearly sorted data;
- the real distribution and record type from the application.

Sweep the size across the cache hierarchy. Report whether allocation and copying the input are included, whether stability is required, and the key and record widths. “Elements per second” without these details is not a sorting result.

For correctness, compare the radix output with `std::sort` on random arrays and on values around `0`, `UINT32_MAX`, `INT32_MIN`, and `INT32_MAX`. To test a stable record sort, attach the original position and check that positions remain increasing within each equal-key group.

The lesson is not that radix sort beats comparison sorting. It is that the representation of the key is part of the algorithm. Once we are allowed to inspect a byte rather than ask a yes-or-no question, the complexity changes — and the bottleneck moves from branches and comparisons to cache lines and memory traffic.
