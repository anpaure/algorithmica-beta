---
title: Prime Number Sieves
weight: 2
---

If we need to check whether one large number is prime, we use a primality test. If we need *all* primes up to some limit $n$, testing every number separately is a terrible idea: we would rediscover the same small divisors over and over again.

The sieve of Eratosthenes does it the other way around. It starts by assuming that every number is prime and then crosses out the multiples of every prime it finds:

```c++
vector<char> sieve(int n) {
    // Assume n >= 2.
    vector<char> is_prime(size_t(n) + 1, true);
    is_prime[0] = is_prime[1] = false;

    for (int p = 2; p <= n / p; p++)
        if (is_prime[p])
            for (long long k = 1LL * p * p; k <= n; k += p)
                is_prime[k] = false;

    return is_prime;
}
```

We start the inner loop at $p^2$ rather than $2p$, because every smaller multiple of $p$ has already been crossed out by another prime. We also stop looking for new divisors after $\sqrt n$: every composite number not exceeding $n$ has at least one prime factor not exceeding $\sqrt n$.

The slightly strange condition `p <= n / p` means the same thing as `p * p <= n`, but does not overflow before the comparison.

## How Much Work Does It Do?

For every prime $p$, the sieve writes to roughly $n/p$ locations. The total number of writes is therefore

$$
n\sum_{p\le n}\frac1p=O(n\log\log n).
$$

This bound is almost linear, but it does not tell us how fast the program runs. For a large sieve, the expensive part is moving through the marker array. The division in the outer loop is executed only $O(\sqrt n)$ times; the millions of scattered stores in the inner loops matter much more.

There are two easy improvements we should make before trying anything sophisticated.

## Removing the Even Numbers

After handling 2 separately, we only need to store odd candidates. Let slot $i$ represent the number $2i+1$. For an odd prime $p$, its odd multiples are

$$
p^2,\ p^2+2p,\ p^2+4p,\ldots
$$

and their indices in the compressed array differ by $p$:

```c++
vector<int> primes(int n) {
    vector<int> result;

    if (n < 2)
        return result;

    vector<char> composite(n / 2 + 1, false);
    result.push_back(2);

    for (long long p = 3; p <= n; p += 2) {
        if (composite[p / 2])
            continue;

        result.push_back((int) p);

        if (p <= n / p)
            for (long long k = 1LL * p * p; k <= n; k += 2 * p)
                composite[k / 2] = true;
    }

    return result;
}
```

This cuts the array in half and removes the densest crossing-out pass. More generally, we can omit multiples of $2$, $3$, $5$, and so on using a *wheel*, but the indexing becomes progressively less pleasant. Wheels are useful, although the first factor of two is by far the cheapest one.

### Bytes or bits?

`vector<char>` spends one byte per candidate. A packed bitmap spends one bit, so eight times more candidates fit in the same cache. On the other hand, changing one bit requires loading a word, masking it, and writing it back, while changing a byte is just a store.

This is a common memory optimization trade-off: the denser representation executes more instructions but transfers fewer cache lines. A byte sieve is often preferable while both versions fit in cache; a bitmap becomes more attractive when only the packed version fits. There is no need to guess — benchmark both around the cache-size boundaries of the target machine.

## Segmented Sieving

If $n$ is large, even one bit per odd number eventually falls out of cache. The standard fix is to split the range into blocks and sieve them independently.

First, we find the primes up to $\sqrt n$. Then, for each interval $[L,R)$, we use these *base primes* to cross out composites in a small temporary array:

```c++
vector<int> segmented_primes(int n) {
    vector<int> result;

    if (n < 2)
        return result;

    result.push_back(2);

    int root = sqrt(n);
    while ((root + 1LL) * (root + 1) <= n) root++;
    while (1LL * root * root > n) root--;
    vector<int> base = primes(root);

    const int S = 1 << 15; // number of odd candidates in one block
    vector<char> composite(S);

    for (long long low = 3; low <= n; low += 2LL * S) {
        long long high = min(low + 2LL * S, n + 1LL);
        int count = (high - low + 1) / 2;
        fill(composite.begin(), composite.begin() + count, false);

        for (int p : base) {
            if (p == 2)
                continue;

            long long first = max(1LL * p * p,
                                  (low + p - 1) / p * p);
            if (first % 2 == 0)
                first += p;

            for (long long k = first; k < high; k += 2LL * p)
                composite[(k - low) / 2] = true;
        }

        for (int i = 0; i < count; i++)
            if (!composite[i])
                result.push_back(low + 2LL * i);
    }

    return result;
}
```

The segment contains only odd numbers, so it covers twice as large a numerical interval as its byte size suggests. The `max` in the starting position is important: without $p^2$, the segment containing $p$ would cross out the prime itself.

The block size should be chosen so that the marker array and the frequently used part of the base-prime array fit in cache. Making blocks tiny increases loop setup and the number of divisions used to find the first multiple. Making them huge brings back the cache problem we were trying to solve.

Small primes write regularly and are easy for the prefetcher. Large primes usually hit a block only a few times, so calculating their starting positions can cost more than crossing them out. Very large sieves keep the next position for each prime or place future hits into buckets, but segmentation is the main optimization: it turns repeated RAM traffic into repeated cache traffic without changing the asymptotic algorithm.

## What About the Linear Sieve?

There is another algorithm that marks every composite exactly once and therefore works in $O(n)$ time. It keeps the smallest prime divisor of every number and a growing list of primes.

Its asymptotic bound is better, but for plain prime enumeration it usually performs more bookkeeping and stores a full integer per number instead of a bit or byte. The linear sieve is valuable when we also need all smallest prime factors; it is not automatically a faster Eratosthenes sieve.

## Benchmarking

There are several different operations people call “running a sieve”:

- building the marker array;
- counting the primes;
- collecting them into another array;
- printing them.

These should be timed separately. Printing decimal integers can easily dominate the sieve itself. Also record the candidate representation, segment size, whether base-prime generation is included, and the compiler and machine used.

Test the boundaries around prime squares and segment endings, and compare small results with trial division. The most interesting performance graph sweeps $n$ across the cache hierarchy: that is where the byte, bitmap, and segmented variants stop looking like the same algorithm.
