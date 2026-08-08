---
title: Random Number Generation
weight: 9
---

A computer is a deterministic machine: if we run the same instructions from the same state, we get the same result. Most programs therefore do not generate new randomness. They expand a small random *seed* into a long sequence that merely looks random for the problem at hand.

This is a useful distinction. Simulations, randomized algorithms, and tests usually want a fast and reproducible sequence. Cryptographic keys and session tokens need something an adversary cannot predict. These two requirements lead to different generators.

## Engines and Distributions

A pseudorandom number generator maintains some state and produces a fixed-width word on every call. A *distribution* turns these words into what the program actually needs: an integer in $[0,n)$, a floating-point number, a random permutation, and so on.

Keeping the two operations separate matters. A generator may produce all 32-bit integers uniformly while `random() % n` introduces bias. It may pass statistical tests while remaining trivial to predict after observing a few outputs.

The relevant properties of a generator are not limited to its period:

- the size of its state;
- statistical quality of the output;
- latency and throughput of one step;
- the cost of seeding and creating independent streams;
- and, when security matters, resistance to state recovery and prediction.

## Linear Generators

The classical *linear congruential generator* updates

$$
x_{i+1}=a x_i+c\pmod m.
$$

When $m=2^w$, unsigned integer overflow performs the reduction for free. With suitable $a$ and $c$, the state visits all $2^w$ values before repeating.

A long period does not imply random-looking output. The low bits of an LCG have short cycles, and points formed from several consecutive outputs lie on a relatively small number of hyperplanes. The transition is useful because it is cheap, but exposing its state directly is usually a bad idea.

A common modern design uses a simple state transition and applies a separate permutation to produce the output. [PCG](https://www.pcg-random.org/), the *permuted congruential generator*, is one such family.

## A Small Generator

The following is the 64-bit-state, 32-bit-output PCG-XSH-RR variant. It is suitable for ordinary randomized algorithms, but not for cryptography.

```c++
typedef __uint32_t u32;
typedef __uint64_t u64;

u64 state, increment;

u32 random_u32() {
    u64 old = state;
    state = old * 6364136223846793005ull + increment;

    u32 x = u32(((old >> 18) ^ old) >> 27);
    int r = int(old >> 59);
    return (x >> r) | (x << ((-r) & 31));
}

void seed(u64 value, u64 stream) {
    state = 0;
    increment = (stream << 1) | 1;
    random_u32();
    state += value;
    random_u32();
}
```

The state follows an LCG, while XOR, shifts, and rotation hide its most obvious linear structure. The increment is odd, which is required for the full-period transition; different increments select different streams.

Only the low 63 bits of the `stream` argument survive `(stream << 1) | 1`, selecting one of $2^{63}$ odd increments. Two stream arguments that differ only in their highest bit select the same stream.

Seeding is part of the algorithm. Two generators initialized with the same seed and stream deliberately produce the same sequence, making a failed randomized test reproducible. Changing the constants or skipping the two initialization steps produces a different generator, not a harmless variation of this one.

## Mapping to a Range

The obvious way to obtain a number in $[0,n)$ is

```c++
random_u32() % n
```

but this is biased unless $n$ divides $2^{32}$. If $n=6$, for example, some remainders have one more 32-bit preimage than others.

We can remove the bias using [multiply-and-reject range reduction](https://arxiv.org/abs/1805.10941):

```c++
u32 uniform(u32 n) {
    // n is assumed to be positive.
    u64 m = (u64) random_u32() * n;
    u32 low = u32(m);

    if (low < n) {
        u32 threshold = -n % n;
        while (low < threshold) {
            m = (u64) random_u32() * n;
            low = u32(m);
        }
    }

    return u32(m >> 32);
}
```

The upper half of the product maps the input almost evenly to the target interval. Rejection discards exactly the values that prevent it from being perfectly uniform. The slow path is entered rarely for most bounds, and the division used to compute `threshold` is outside the rejection loop.

Floating-point conversion has a similar boundary trap: dividing by the maximum 32-bit value can produce exactly 1. To generate a `float` in $[0,1)$, we can take 24 random bits and scale by $2^{-24}$:

```c++
float uniform_float() {
    return (random_u32() >> 8) * 0x1p-24f;
}
```

## Parallel Streams

Giving thread $i$ the seed `seed + i` does not by itself prove that the streams are independent. A generator needs a documented way to select streams or jump ahead in its sequence.

There is another approach that avoids mutable chains entirely. A *counter-based generator* computes something resembling a keyed hash of `(counter, stream)`. Different workers own disjoint counter ranges, and any output can be regenerated directly without replaying all previous states. This is especially convenient for parallel simulations and accelerators.

The distinction also matters for performance. One PCG stream is a dependency chain because every state depends on the previous one. Generating from several independent states lets the processor overlap their multiplications. Counter-based generators expose parallelism even more directly and are easy to vectorize.

## Randomness for Security

PCG, xorshift, Mersenne Twister, and ordinary LCGs are not suitable for keys, password-reset links, or nonces that must be unpredictable. Statistical tests only look for patterns; they do not model an adversary who observes outputs and tries to reconstruct the state.

For these tasks, request bytes from the operating system or use a vetted cryptographic library. On Linux, the relevant interface is [`getrandom`](https://man7.org/linux/man-pages/man2/getrandom.2.html); other systems provide equivalent APIs. A cryptographically secure generator can then expand this entropy with a specified construction such as ChaCha20.

Timestamps, process identifiers, and memory addresses are not secret seeds. They may be different from run to run while still having very little entropy.

When benchmarking a generator, measure the value the application consumes rather than only the raw engine. Range reduction may reject, a large state may stop fitting in cache when thousands of workers each own one, and a generator with high throughput may still have poor latency for one dependent stream.

There is no universally best random number generator. First decide whether the goal is reproducibility, statistical simulation, parallel generation, or adversarial unpredictability; only then choose the engine and the way its output is transformed.
