---
title: Optimizing Logistic Regression
weight: 4
draft: false
---

In machine learning, perhaps the most popular way of doing black-box classification is logistic regression.

Say, we want to classify $28 \times 28$ black-and-white pictures of digits into one of 10 categories (0..9):

![Numbers that some Canadian students wrote on test blanks make MNIST dataset](../../arithmetic/img/mnist.png)

Computationally, it works like this. If we are working with $n$-dimensional data, which we need to classify into one of $m$ classes, then we multiply the input vector by a parameter matrix of size $n \times m$, and then apply a special "softmax" function to the output $m$-element vector:

$$
\operatorname{softmax}(x)_k = \frac{e^{x_k}}{\sum_{i=0}^{m-1} e^{x_i}}
$$

In other words, what this function does is it calculates elementwise exponent of its input vector and then normalizes it so that the elements add up to 1. Since the output has $m$ positive elements that add up to 1, it can be treated as a probability distribution that the sample belongs to a certain class. We can then look at the highest probability prediction and take it as the answer of the model.

We look at a large dataset of samples and fit our parameter matrix so that we get the most answers correct. We will not go into detail about how to fit it, but once we do, we need to run *inference*, that is, to feed it new data and get predictions.

This is pretty much all that a performance engineer needs to know about machine learning. For now, we are only concerned with the computational side of things.

```c++
float w[10][28*28];
// ten rows of 784 weights

int predict(const float *a) {
    float s[10] = {0};

    for (int k = 0; k < 10; k++)
        for (int i = 0; i < 28*28; i++)
            s[k] += a[i] * w[k][i];

    // subtracting the maximum keeps every exponent argument non-positive
    float mx = *std::max_element(s, s + 10);
    float sumexp = 0;
    for (int i = 0; i < 10; i++) {
        s[i] = std::exp(s[i] - mx);
        sumexp += s[i];
    }

    int argmax = 0;
    for (int i = 0; i < 10; i++) {
        s[i] /= sumexp;
        if (s[i] > s[argmax])
            argmax = i;
    }

    return argmax;
}
```

This isn't exactly worth optimizing, because that's just ~10k operations anyway, but our use case could be bigger. For example, neural networks are not fundamentally different: they just use longer chains of transformations, and not just matrix multiplication followed by a softmax.

This can also be used in a hot spot. For example, computer chess programs use similar models to determine the value of a position (the probability of winning). By the way, this is how the "1-3-3-5-9" heuristic arises: you can train a logistic regression on a large dataset of chess positions that are turned into piece count differences, and that's what weights are going to look like. Score in other games works in a similar way.

Nobody in their sane mind uses C++ for training ML models. We will only time inference.

## The Experiment

The picture above is illustrative; the benchmark does **not** use MNIST, and we will not report accuracy on it. The [complete program](../../../code/logistic_bench.cpp) deterministically creates a synthetic model with 784 features and 10 classes, together with $2^{13}$ synthetic inputs. Weights are uniform in $[-0.25,0.25]$, inputs are uniform in $[-1,1]$, there is no bias, and all random seeds are fixed. Its [five-process median output](../../../code/logistic_m4_results.txt) is rendered by the [Matplotlib plot script](../../../code/plot_logistic.py). Non-AArch64 builds omit the three NEON variants instead of timing the scalar kernel under SIMD labels.

This is a kernel benchmark with reproducible data, not a claim about a trained model. Its quality metric is the fraction of predictions that disagree with the floating-point argmax. Disagreement is not accuracy: either implementation may agree or disagree with a real label.

Measurements were taken on an Apple M4 Max with Apple Clang 17.0.0 using `-O3 -mcpu=native`. Each process performs two warm-up runs and nine timed runs, and the table reports the componentwise median of five processes. Model construction, quantization, and packing are outside the timed region; every implementation predicts the entire dataset and folds the class indices into an observable checksum.

The full floating-point implementation takes **3162 ns per sample**.

## Removing Softmax

The first thing we can notice is that we don't actually need to implement softmax when the caller only wants a class. The largest logit—this is what the pre-softmax numbers are called—will still be the largest after softmax, because the exponential function is strictly increasing and division by the same positive number does not change the order:

$$
\underset{k}{\operatorname{argmax}}\; \frac{e^{z_k}}{\sum_i e^{z_i}}
= \underset{k}{\operatorname{argmax}}\; z_k.
$$

This is an exact transformation, not an approximation. Probabilities, cross-entropy, and calibration are different interfaces and still need a numerically stable softmax.

Removing exponentiation, normalization, and their two short loops only improves the benchmark to **3042 ns**, or 3.8%. The matrix-vector product performs 7840 multiply-adds and is the actual bottleneck; optimizing ten exponentials was a good idea aimed at the wrong part of this particular workload.

## Quantization

Machine-learning models are often robust enough to tolerate reduced precision, but this is an empirical property, not a theorem. Quantization errors need not cancel. We will change the arithmetic and then measure how many predictions change.

Using lower precision has two potential advantages:

1. It takes less memory traffic to fetch the data.
2. SIMD instructions pack more values into each register.

We use symmetric per-tensor scales shared by all classes:

$$
q_i = \operatorname{round}(127x_i),
\qquad
r_{ki} = \operatorname{round}(508w_{ki}).
$$

Values are clipped to the signed-byte range. The positive common scale may be discarded for argmax, so the quantized score is simply

$$
\hat z_k = \sum_i q_i r_{ki}.
$$

The model and each input shrink from four bytes per element to one. The accumulator must remain wide: in the worst case,

$$
784 \cdot 127^2 = 12{,}645{,}136 < 2^{31}.
$$

More directly, the checked harness uses `static_assert(784 * 127 * 127 < INT32_MAX)` and a 32-bit sum. The scalar kernel is uncomplicated:

```c++
int32_t dot(const int8_t *a, const int8_t *b) {
    int32_t sum = 0;
    for (int i = 0; i < 784; i++)
        sum += int32_t(a[i]) * int32_t(b[i]);
    return sum;
}
```

This version takes **207 ns per sample**, already 15.2 times faster than the original. On the synthetic dataset, its predictions disagree with floating point on **0.598%** of samples. That number only describes this fixed random construction; a real model needs calibration on representative data and an accuracy evaluation after quantization.

There are $784 \times 10 = 7{,}840$ multiply-accumulates per prediction. Under the conventional two-operations-per-MAC accounting, that is 15,680 useful multiply/add operations. The 3042 ns floating-point argmax therefore sustains about

$$
\frac{15{,}680}{3042\ \mathrm{ns}} \approx 5.15\ \mathrm{GFLOPS}.
$$

The corresponding rate for the final 133 ns int8 kernel is about **118 billion integer multiply/add operations per second**. This is GOPS under the same useful-operation convention, not FLOPS and not a hardware-counter reading.

The float model occupies 31,360 bytes and the int8 model 7,840 bytes; both fit comfortably in the M4 performance core's 128 KiB L1 data cache. Each input similarly shrinks from 3136 to 784 bytes. Reduced traffic helps, but cache capacity alone does not explain a 23.8x result here—the instruction sequence and available independent accumulators dominate.

## What the Compiler Already Did

The source above says “scalar,” but the generated machine code does not. Apple Clang consumes 32 bytes from each input per iteration, multiplies the low and high byte halves with `smull` and `smull2`, and maintains eight independent vector accumulators:

```nasm
loop:
    ldp     q16, q17, [x8, #-16]
    ldp     q18, q19, [x9, #-16]
    smull.8h  v20, v18, v16
    smull2.8h v16, v18, v16
    smull.8h  v18, v19, v17
    smull2.8h v17, v19, v17
    saddw.4s  v0, v0, v20
    // seven other independent accumulators
    b.ne    loop
```

This is why a direct transcription into intrinsics is not automatically faster. Our first NEON version uses one 32-bit accumulator and pairwise-accumulates sixteen byte products into it:

```c++
int32x4_t sum = vdupq_n_s32(0);

for (int i = 0; i < 784; i += 16) {
    int8x16_t x = vld1q_s8(a + i);
    int8x16_t y = vld1q_s8(b + i);
    sum = vpadalq_s16(sum, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
    sum = vpadalq_s16(sum, vmull_high_s8(x, y));
}
```

It regresses to **290 ns**. Each iteration depends on the previous value of `sum`, while the compiler-generated loop had enough independent work to hide that latency.

## Independent Accumulators

We can repair the manual kernel by processing four 16-byte blocks in parallel, maintaining `sum0`, `sum1`, `sum2`, and `sum3`, and combining them only after the loop. This is the same [instruction-level parallelism](/hpc/pipelining/) technique used in ordinary reductions.

The four-accumulator version takes **133 ns per sample**: 23.8 times faster than the floating-point baseline and 1.56 times faster than the already auto-vectorized scalar int8 code. In operation-rate terms, the dependency-breaking change raises the measured int8 kernel from about 75.6 to 118 GOPS.

![](../img/logistic-stages.svg)

The graph also includes one failed batching experiment. We packed four inputs feature-major, loaded four independent feature values into a vector, and reused each weight across the four lanes. This removes horizontal reductions and reuses weights, but it widens every byte to 32 bits and executes an ordinary vector multiplication for every feature and class. Even with packing excluded, it takes **350 ns per sample**. Weight reuse does not compensate for throwing away the dense byte-dot-product kernel at a batch of only four.

For larger batches, this becomes a matrix-matrix multiplication problem. A production implementation should compare against a tuned GEMM or platform ML library and include packing in end-to-end latency. The batch-four experiment only establishes that this particular layout and kernel lose.

## Precision Is a Parameter

The int8 disagreement rate is not a universal constant. Keeping the same floating-point model and inputs, the harness repeats symmetric quantization with signed limits corresponding to 4 through 8 bits:

![](../img/logistic-disagreement.svg)

| Signed precision | Prediction disagreement |
|--:|--:|
| 4 bits | 8.47% |
| 5 bits | 4.05% |
| 6 bits | 2.26% |
| 7 bits | 1.14% |
| 8 bits | 0.598% |

This smooth curve is useful precisely because no label accuracy is attached to it. It shows that the arithmetic change is observable and must be part of the benchmark contract.

## Final Comparison

| Implementation | ns / sample | Speedup | Prediction semantics |
|:--|--:|--:|:--|
| float plus stable softmax | 3162 | 1.00x | probabilities, then argmax |
| float argmax only | 3042 | 1.04x | identical class |
| scalar-source int8 | 207 | 15.2x | 0.598% disagreement |
| one-accumulator NEON int8 | 290 | 10.9x | same int8 result |
| four-accumulator NEON int8 | **133** | **23.8x** | same int8 result |
| four-sample packed int8 | 350 | 9.0x | same int8 result |

The important transition is not “float instruction to integer instruction.” It is float model to quantized model, followed by a kernel that exposes enough independent work. The first transition changes predictions and needs a quality measurement; the second is exact with respect to the quantized integers and needs machine-code inspection.

The test mode verifies that stable softmax and float argmax select the same class, that scalar and both NEON dot products agree for every synthetic test sample, and that the packed batch produces exactly the same class sequence as scalar int8. It also runs under AddressSanitizer and UndefinedBehaviorSanitizer.

This implementation deliberately omits biases, per-channel scales, zero points, trained parameters, and packing time. Adding any of them changes both the arithmetic and the comparison rules. Those are necessary features of a production quantized model, not details to hide behind the 23.8x number.
