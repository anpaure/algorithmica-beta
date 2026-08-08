---
title: Data Compression
weight: 8
---

Can we write a program that makes every file smaller?

There are $256^n$ byte strings of length $n$, but fewer than that many strings of length less than $n$:

$$
1+256+256^2+\cdots+256^{n-1}<256^n.
$$

If every input became shorter, at least two inputs would have to produce the same output, and the decompressor would not know which one to restore. Lossless compression can only work by making *likely* inputs shorter and sacrificing the unlikely ones.

Real data gives us plenty of opportunities. Source code repeats identifiers, logs repeat whole phrases, neighboring pixels are similar, and sorted integers often differ by a small amount. A compressor is mostly a machine for turning one of these observations into a cheaper representation.

## Information

If an event has probability $0 < p \le 1$, its information content is defined as

$$
I(p)=-\log_2p.
$$

An event that occurs half the time carries one bit of information. An event with probability $2^{-8}$ carries eight. For symbols with probabilities $p_i$, the average information per symbol is the *entropy*

$$
H=-\sum_i p_i\log_2p_i.
$$

The terms with $p_i=0$ contribute zero, following the limiting convention $0\log 0=0$.

This is a lower bound on the average number of bits needed under the assumed model, not a promise that every particular file becomes smaller. The model matters: if we assign short codes to events that rarely occur in the actual data, we have merely compressed the wrong distribution.

Most lossless compressors can be understood as a combination of three ideas:

1. transform the data so its predictable structure becomes obvious;
2. model which symbols or strings are likely;
3. encode likely choices with fewer bits.

Optimized formats fuse these stages together, but it is useful to keep them separate while designing the algorithm.

## Delta Coding and Variable-Length Integers

Consider a sorted sequence of timestamps:

```text
1000000, 1000003, 1000004, 1000011, ...
```

The values themselves need about 20 bits. Their differences are

```text
1000000, 3, 1, 7, ...
```

and almost all of them are tiny. Replacing values with differences is called *delta coding*. It does not save space by itself; it creates a distribution that another code can compress.

A simple code for nonnegative integers is a base-128 *varint*. Every byte stores seven payload bits, and its highest bit says whether another byte follows:

```c++
int encode_varint(uint64_t x, unsigned char *out) {
    int n = 0;

    while (x >= 128) {
        out[n++] = (x & 127) | 128;
        x >>= 7;
    }

    out[n++] = x;
    return n;
}

const unsigned char *decode_varint(const unsigned char *p,
                                   uint64_t &x) {
    x = 0;

    for (int shift = 0; ; shift += 7) {
        unsigned char byte = *p++;
        x |= uint64_t(byte & 127) << shift;
        if (byte < 128)
            return p;
    }
}
```

Numbers below $2^7$ use one byte, numbers below $2^{14}$ use two, and so on. Uniformly random 64-bit integers usually become larger, while deltas, lengths, and small identifiers often shrink considerably.

The decoder above is the small trusted-input kernel: it assumes that a complete canonical 64-bit varint is available. A decoder for files or network data also needs an end pointer, a ten-byte limit, a check that the last byte does not contain overflowing payload bits, and — when the format requires canonical encodings — rejection of an overlong encoding whose final payload group is zero. Keeping validation outside a proven-valid block can be faster, but omitting it entirely is not an optimization.

Signed residuals are usually mapped to unsigned numbers by interleaving them as

$$
0,-1,1,-2,2,\ldots \quad\longrightarrow\quad 0,1,2,3,4,\ldots
$$

so small values of either sign retain short encodings.

## Repeated Strings

Delta coding exploits numerical closeness. Dictionary methods exploit repeated byte strings.

In LZ77-style compression, the encoder emits either a literal or a pair `(distance, length)` referring to text that has already been decoded. A larger search window can find older repetitions and improve the ratio, but searching it costs time and memory. Decompression is simpler: copy literals or copy from a previous output position, allowing the source and destination to overlap.

The speed of the encoder depends heavily on how matches are found. Comparing every previous position is hopeless. Hash tables indexed by a few upcoming bytes provide likely candidates; chains, limits, and lazy matching trade compression ratio for search time. This is an algorithmic choice, not something a compiler can optimize away.

## Entropy Coding

Once a transform or dictionary has produced symbols, an entropy coder gives short representations to common symbols.

Huffman coding builds a prefix-free binary code with integer bit lengths. Prefix-free means that no codeword is the beginning of another one, so the decoder can recognize a symbol without a separator. Table-based decoders inspect several bits at once rather than walking a tree one branch at a time.

Arithmetic, range, and ANS coders can get closer to fractional-bit entropy because they represent a whole sequence using one changing state. Their performance is governed by state dependencies, table size, and how many independent streams can be interleaved. A theoretically denser coder may lose if it creates one long serial dependency chain.

General-purpose formats combine dictionary and entropy coding. Repeated strings become length-distance pairs, and then literals, lengths, and distances are themselves entropy-coded.

## Compression as a Performance Optimization

Compression adds instructions but removes bytes. It can therefore make an in-memory algorithm faster when the original representation is limited by memory bandwidth or cache capacity.

The relevant cost is not just encoding time:

$$
T=T_{encode}+T_{transfer}+T_{decode}.
$$

If data is written once and scanned many times, even an expensive encoder may pay for itself. If the data already fits in L1 cache, a serial decoder may be strictly worse than reading the uncompressed values.

Varints illustrate this trade-off nicely. Their size adapts to the values, but decoding includes a data-dependent branch per byte. Fixed-width bit packing has a slightly worse ratio when widths vary, yet it can decode many integers with regular shifts and masks. SIMD-friendly formats often divide values into blocks, store one width for the block, and unpack all lanes with the same instruction sequence.

Blocks also make streams seekable, parallel, and resistant to local corruption. Small blocks improve latency and random access; large blocks amortize headers and give the model more context. There is no universal block size for the same reason there is no universal cache block size: the surrounding workload decides what should be optimized.

When benchmarking a codec, report both ratio and throughput, separately for encoding and decoding. Include the block size, input distribution, warm or cold cache state, and whether checksums and allocation are timed. Testing only English text says little about integers, already-compressed media, or adversarial data.

Compression is representation engineering. First find the predictability, then expose it, and only then spend instructions turning it into fewer bits.
