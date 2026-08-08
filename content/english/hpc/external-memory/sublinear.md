---
title: Sublinear Algorithms
weight: 10
---

To find the exact maximum of an arbitrary array, we have to inspect every element. If one position is left unread, an adversary can put a larger value there. The same argument applies to an exact sum, an exact number of distinct values, and many other basic statistics.

There are only two ways around this lower bound: assume something about the input or ask for less than an exact answer.

In this section, we will mostly use the second option. Instead of returning the exact value, a *sketch* returns an approximation together with a bound on its error probability.

Two different resources are commonly called sublinear:

- A *sublinear-time* algorithm has random access to the input and reads only some of its elements.
- A *streaming* algorithm reads the entire input once but stores much less than the entire input.

A streaming sketch still takes $O(N)$ time to process $N$ updates. What becomes sublinear is its memory, the size of the summary sent over a network, or the time of later queries.

## Sampling

Suppose that $x_1,\ldots,x_N$ are numbers between zero and one, and we want to estimate their mean

$$
\mu = \frac{1}{N}\sum_{i=1}^N x_i.
$$

Pick $k$ positions independently and uniformly and compute their sample mean $\hat\mu$. Hoeffding's inequality gives

$$
\Pr\left[|\hat\mu-\mu|\ge \varepsilon\right]
\le 2e^{-2k\varepsilon^2}.
$$

Therefore it is sufficient to take

$$
k \ge \frac{\ln(2/\delta)}{2\varepsilon^2}
$$

samples to obtain additive error at most $\varepsilon$ with probability at least $1-\delta$. Interestingly, $k$ does not depend on $N$.

This is a genuinely sublinear-time algorithm only if a random element can be accessed directly. Sampling a compressed file by first decoding it from the beginning is still a linear scan. The error is also additive: estimating an event with probability $10^{-9}$ to within $10^{-3}$ is mathematically correct and completely useless.

## Reservoir Sampling

If the input arrives as a stream and its length is not known beforehand, we cannot choose random indices in advance. *Reservoir sampling* maintains a uniform sample of $K$ elements in one pass:

```cpp
const int K = 1000;

int sample[K];
long long seen = 0;

void add(int x) {
    seen++;

    if (seen <= K) {
        sample[seen - 1] = x;
    } else {
        long long i = random(seen); // uniform integer in [0, seen)
        if (i < K)
            sample[i] = x;
    }
}
```

After seeing $n\ge K$ elements, every element is stored with probability $K/n$.

The proof is inductive. The new element is kept with probability $K/n$. An old sampled element is replaced only when the random index points to its particular slot, which happens with probability $1/n$, so it survives with probability

$$
\frac{K}{n-1}\left(1-\frac{1}{n}\right)=\frac{K}{n}.
$$

Reservoir sampling uses $O(K)$ memory, but it still reads all $n$ elements. Its advantage is that the sample is uniform without storing the stream or knowing its final length.

The `random(n)` operation in the example is assumed to be exactly uniform. Implementing it as `rand() % n` introduces modulo bias unless the generator's range happens to be divisible by $n$.

The `seen` counter is bounded too: the example assumes the stream length fits in a positive `long long`. Letting the signed counter overflow would be undefined behavior rather than a valid continuation of the sampling algorithm.

## Count–Min Sketch

Sampling is not ideal for estimating frequencies. An item occurring just below the sampling resolution may be absent from the sample entirely. A *Count–Min sketch* instead stores a small two-dimensional array of counters.

Choose $D$ independent hash functions, each mapping keys into $W$ counters. To add an item, increment one counter in every row. To estimate its frequency, return the smallest of these counters:

```cpp
unsigned count[D][W] = {};

void add(uint64_t x) {
    for (int row = 0; row < D; row++)
        count[row][hash(row, x) % W]++;
}

unsigned frequency(uint64_t x) {
    unsigned answer = UINT_MAX;
    for (int row = 0; row < D; row++)
        answer = min(answer, count[row][hash(row, x) % W]);
    return answer;
}
```

The code assumes insertion-only updates and counters wide enough not to overflow. The hash functions and their seeds are part of the data structure; using the same hash in every row destroys the probability argument.

Let $f_x$ be the true frequency of $x$ and let

$$
M = \sum_x f_x
$$

be the total number of inserted items. Collisions can only increase a counter, so the estimate $\hat f_x$ never underestimates $f_x$.

In one row, the expected contribution of all colliding items is at most $M/W$. If we choose

$$
W = \left\lceil \frac{e}{\varepsilon} \right\rceil,
$$

Markov's inequality says that the collision error exceeds $\varepsilon M$ with probability at most $1/e$. Taking the minimum of

$$
D = \left\lceil \ln \frac{1}{\delta} \right\rceil
$$

independent rows makes all rows bad with probability at most $\delta$. Thus, for any fixed queried key,

$$
f_x \le \hat f_x \le f_x + \varepsilon M
$$

with probability at least $1-\delta$.

The word *fixed* is important. If we inspect millions of estimates and then select the worst-looking one, a guarantee for one predetermined query does not automatically cover that selection. The failure probability has to account for the number and adaptivity of queries.

The sketch occupies $O(\varepsilon^{-1}\log(1/\delta))$ counters regardless of the number of distinct keys. An update performs $D$ hash calculations and $D$ mostly unrelated memory accesses, so making the table smaller does not necessarily make updates faster; once it stops fitting in cache, the memory layout matters again.

## Counting Distinct Values

Counting distinct values exactly requires remembering a set in the worst case. HyperLogLog uses a much smaller summary.

Hash each item to a uniformly distributed bit string. Some leading hash bits select one of $R$ registers; in the remaining bits, record one plus the number of leading zeroes — equivalently, the position of the first one bit. An all-zero suffix gets one more than the suffix length. Each register stores the largest value it has seen.

A prefix of $k$ zeroes appears with probability roughly $2^{-k}$. Seeing an unusually long zero prefix is therefore evidence that many distinct hashes have been observed. Combining the registers with a bias-corrected harmonic mean produces the cardinality estimate.

With ideal hashing and sufficiently large cardinalities, the standard deviation is approximately

$$
\frac{1.04}{\sqrt R}
$$

times the true count. This is a statistical standard deviation, not a deterministic bound. Practical implementations also need corrections for small and very large ranges and enough hash bits to make actual collisions negligible at the intended scale.

## Merging Sketches

One reason sketches are useful in distributed systems is that many of them can be merged without reading the original data:

- Count–Min sketches with the same dimensions and hash functions are added counter by counter.
- HyperLogLog sketches with the same register count and hash function are merged by taking register-wise maxima.
- Samples require a weighted merge based on the number of elements represented by each sample; simply concatenating two reservoirs is biased when their streams have different lengths.

The dimensions, hash functions, and random seeds are therefore part of a sketch's format. Merging two arrays that happen to have the same shape but use different hashes produces numbers with no useful guarantee.

Randomization also defines the adversary. The usual proofs assume that the data is independent of secret random choices. If an attacker knows a weak fixed hash function, they may deliberately create collisions. A probabilistic data structure is not automatically a security boundary.

Sublinear algorithms do not get something for nothing. They trade exactness for time, memory, or communication, and the error bound is part of the result. “Probably close” is not a specification; $\varepsilon$, $\delta$, the input model, and the queried quantity are.

## Further Reading

- Jeffrey Vitter's [reservoir sampling paper](https://doi.org/10.1145/3147.3165) develops faster variants that skip over stream elements.
- Cormode and Muthukrishnan's [Count–Min paper](https://doi.org/10.1016/j.jalgor.2003.12.001) gives frequency, range, and inner-product queries.
- The original [HyperLogLog analysis](https://doi.org/10.46298/dmtcs.3545) derives its estimator and error.
