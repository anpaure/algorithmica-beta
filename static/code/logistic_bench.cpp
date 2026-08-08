#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr int features = 28 * 28;
constexpr int classes = 10;

struct Problem {
    std::size_t samples = 0;
    std::vector<float> weights;
    std::vector<float> input;
    std::vector<std::int8_t> qweights;
    std::vector<std::int8_t> qinput;
    std::vector<std::int8_t> packed4;
};

std::int8_t quantize(float x, float scale, int limit = 127) {
    int value = static_cast<int>(std::lrint(x / scale));
    value = std::max(-limit, std::min(limit, value));
    return static_cast<std::int8_t>(value);
}

Problem make_problem(std::size_t samples, std::uint32_t seed,
                     int quantization_limit = 127) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> weight_dist(-0.25F, 0.25F);
    std::uniform_real_distribution<float> input_dist(-1.0F, 1.0F);
    Problem problem;
    problem.samples = samples;
    problem.weights.resize(classes * features);
    problem.input.resize(samples * features);
    for (float &weight : problem.weights) weight = weight_dist(rng);
    for (float &value : problem.input) value = input_dist(rng);

    float weight_scale = 0.25F / static_cast<float>(quantization_limit);
    float input_scale = 1.0F / static_cast<float>(quantization_limit);
    problem.qweights.resize(problem.weights.size());
    problem.qinput.resize(problem.input.size());
    for (std::size_t i = 0; i < problem.weights.size(); ++i)
        problem.qweights[i] = quantize(problem.weights[i], weight_scale,
                                       quantization_limit);
    for (std::size_t i = 0; i < problem.input.size(); ++i)
        problem.qinput[i] = quantize(problem.input[i], input_scale,
                                     quantization_limit);

    std::size_t groups = (samples + 3U) / 4U;
    problem.packed4.assign(groups * features * 4U, 0);
    for (std::size_t group = 0; group < groups; ++group)
        for (int i = 0; i < features; ++i)
            for (std::size_t lane = 0; lane < 4; ++lane) {
                std::size_t sample = 4U * group + lane;
                if (sample < samples)
                    problem.packed4[(group * features + static_cast<std::size_t>(i)) * 4U + lane]
                        = problem.qinput[sample * features + static_cast<std::size_t>(i)];
            }
    return problem;
}

int predict_float_softmax_one(const Problem &problem, const float *input) {
    float score[classes]{};
    for (int k = 0; k < classes; ++k)
        for (int i = 0; i < features; ++i)
            score[k] += input[i] * problem.weights[k * features + i];
    float shift = *std::max_element(score, score + classes);
    float sum = 0;
    for (float &value : score) {
        value = std::exp(value - shift);
        sum += value;
    }
    int best = 0;
    for (int k = 0; k < classes; ++k) {
        score[k] /= sum;
        if (score[k] > score[best]) best = k;
    }
    return best;
}

int predict_float_argmax_one(const Problem &problem, const float *input) {
    float best_value = -std::numeric_limits<float>::infinity();
    int best = 0;
    for (int k = 0; k < classes; ++k) {
        float score = 0;
        for (int i = 0; i < features; ++i)
            score += input[i] * problem.weights[k * features + i];
        if (score > best_value) {
            best_value = score;
            best = k;
        }
    }
    return best;
}

__attribute__((noinline)) std::int32_t dot_scalar(const std::int8_t *a,
                                                  const std::int8_t *b) {
    std::int32_t sum = 0;
    for (int i = 0; i < features; ++i)
        sum += static_cast<std::int32_t>(a[i]) * static_cast<std::int32_t>(b[i]);
    return sum;
}

#if defined(__aarch64__)
__attribute__((noinline)) std::int32_t dot_neon(const std::int8_t *a,
                                                const std::int8_t *b) {
    int32x4_t sum = vdupq_n_s32(0);
    for (int i = 0; i < features; i += 16) {
        int8x16_t x = vld1q_s8(a + i);
        int8x16_t y = vld1q_s8(b + i);
        sum = vpadalq_s16(sum, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
        sum = vpadalq_s16(sum, vmull_high_s8(x, y));
    }
    return vaddvq_s32(sum);
}

__attribute__((noinline)) std::int32_t dot_neon4(const std::int8_t *a,
                                                 const std::int8_t *b) {
    int32x4_t sum0 = vdupq_n_s32(0), sum1 = sum0, sum2 = sum0, sum3 = sum0;
    int i = 0;
    for (; i + 63 < features; i += 64) {
        int8x16_t a0 = vld1q_s8(a + i);
        int8x16_t b0 = vld1q_s8(b + i);
        int8x16_t a1 = vld1q_s8(a + i + 16);
        int8x16_t b1 = vld1q_s8(b + i + 16);
        int8x16_t a2 = vld1q_s8(a + i + 32);
        int8x16_t b2 = vld1q_s8(b + i + 32);
        int8x16_t a3 = vld1q_s8(a + i + 48);
        int8x16_t b3 = vld1q_s8(b + i + 48);
        sum0 = vpadalq_s16(sum0, vmull_s8(vget_low_s8(a0), vget_low_s8(b0)));
        sum0 = vpadalq_s16(sum0, vmull_high_s8(a0, b0));
        sum1 = vpadalq_s16(sum1, vmull_s8(vget_low_s8(a1), vget_low_s8(b1)));
        sum1 = vpadalq_s16(sum1, vmull_high_s8(a1, b1));
        sum2 = vpadalq_s16(sum2, vmull_s8(vget_low_s8(a2), vget_low_s8(b2)));
        sum2 = vpadalq_s16(sum2, vmull_high_s8(a2, b2));
        sum3 = vpadalq_s16(sum3, vmull_s8(vget_low_s8(a3), vget_low_s8(b3)));
        sum3 = vpadalq_s16(sum3, vmull_high_s8(a3, b3));
    }
    int32x4_t sum = vaddq_s32(vaddq_s32(sum0, sum1), vaddq_s32(sum2, sum3));
    for (; i < features; i += 16) {
        int8x16_t x = vld1q_s8(a + i);
        int8x16_t y = vld1q_s8(b + i);
        sum = vpadalq_s16(sum, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
        sum = vpadalq_s16(sum, vmull_high_s8(x, y));
    }
    return vaddvq_s32(sum);
}
#endif

using Dot = std::int32_t (*)(const std::int8_t *, const std::int8_t *);

int predict_quantized_one(const Problem &problem, const std::int8_t *input, Dot dot) {
    std::int32_t best_value = std::numeric_limits<std::int32_t>::min();
    int best = 0;
    for (int k = 0; k < classes; ++k) {
        std::int32_t score = dot(input, problem.qweights.data() + k * features);
        if (score > best_value) {
            best_value = score;
            best = k;
        }
    }
    return best;
}

#if defined(__aarch64__)
int32x4_t load4_i8(const std::int8_t *p) {
    std::int32_t packed = 0;
    std::memcpy(&packed, p, 4);
    int8x8_t bytes = vreinterpret_s8_s32(vdup_n_s32(packed));
    int16x8_t wide16 = vmovl_s8(bytes);
    return vmovl_s16(vget_low_s16(wide16));
}
#endif

std::uint64_t predict_float_softmax(const Problem &problem) {
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        checksum = checksum * 11U + static_cast<unsigned>(
            predict_float_softmax_one(problem, problem.input.data() + sample * features));
    return checksum;
}

std::uint64_t predict_float_argmax(const Problem &problem) {
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        checksum = checksum * 11U + static_cast<unsigned>(
            predict_float_argmax_one(problem, problem.input.data() + sample * features));
    return checksum;
}

std::uint64_t predict_scalar_i8(const Problem &problem) {
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        checksum = checksum * 11U + static_cast<unsigned>(
            predict_quantized_one(problem, problem.qinput.data() + sample * features,
                                  dot_scalar));
    return checksum;
}

#if defined(__aarch64__)
std::uint64_t predict_neon_i8(const Problem &problem) {
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        checksum = checksum * 11U + static_cast<unsigned>(
            predict_quantized_one(problem, problem.qinput.data() + sample * features,
                                  dot_neon));
    return checksum;
}

std::uint64_t predict_neon4_i8(const Problem &problem) {
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        checksum = checksum * 11U + static_cast<unsigned>(
            predict_quantized_one(problem, problem.qinput.data() + sample * features,
                                  dot_neon4));
    return checksum;
}

std::uint64_t predict_batch4_i8(const Problem &problem) {
    std::uint64_t checksum = 0;
    std::size_t groups = (problem.samples + 3U) / 4U;
    for (std::size_t group = 0; group < groups; ++group) {
        int32x4_t score[classes];
        for (int k = 0; k < classes; ++k) score[k] = vdupq_n_s32(0);
        const std::int8_t *input = problem.packed4.data() + group * features * 4U;
        for (int i = 0; i < features; ++i) {
            int32x4_t x = load4_i8(input + static_cast<std::size_t>(i) * 4U);
            for (int k = 0; k < classes; ++k)
                score[k] = vmlaq_n_s32(score[k], x,
                    static_cast<std::int32_t>(problem.qweights[k * features + i]));
        }
        alignas(16) std::int32_t lane_score[classes][4];
        for (int k = 0; k < classes; ++k) vst1q_s32(lane_score[k], score[k]);
        for (std::size_t lane = 0; lane < 4 && 4U * group + lane < problem.samples; ++lane) {
            int best = 0;
            for (int k = 1; k < classes; ++k)
                if (lane_score[k][lane] > lane_score[best][lane]) best = k;
            checksum = checksum * 11U + static_cast<unsigned>(best);
        }
    }
    return checksum;
}
#endif

using Predictor = std::uint64_t (*)(const Problem &);

struct Variant {
    std::string_view name;
    Predictor predictor;
    bool quantized;
};

#if defined(__aarch64__)
constexpr std::array<Variant, 6> variants{{
    {"float-softmax", predict_float_softmax, false},
    {"float-argmax", predict_float_argmax, false},
    {"scalar-int8", predict_scalar_i8, true},
    {"neon-int8", predict_neon_i8, true},
    {"neon4-int8", predict_neon4_i8, true},
    {"batch4-int8", predict_batch4_i8, true},
}};
#else
constexpr std::array<Variant, 3> variants{{
    {"float-softmax", predict_float_softmax, false},
    {"float-argmax", predict_float_argmax, false},
    {"scalar-int8", predict_scalar_i8, true},
}};
#endif

std::vector<int> predictions_float(const Problem &problem) {
    std::vector<int> result(problem.samples);
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        result[sample] = predict_float_argmax_one(
            problem, problem.input.data() + sample * features);
    return result;
}

std::vector<int> predictions_quantized(const Problem &problem, Dot dot) {
    std::vector<int> result(problem.samples);
    for (std::size_t sample = 0; sample < problem.samples; ++sample)
        result[sample] = predict_quantized_one(
            problem, problem.qinput.data() + sample * features, dot);
    return result;
}

double disagreement(const std::vector<int> &a, const std::vector<int> &b) {
    std::size_t different = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        different += static_cast<std::size_t>(a[i] != b[i]);
    return 100.0 * static_cast<double>(different) / static_cast<double>(a.size());
}

bool test() {
    Problem problem = make_problem(257, 0x10a1571cU);
    for (std::size_t sample = 0; sample < problem.samples; ++sample) {
        const float *input = problem.input.data() + sample * features;
        if (predict_float_softmax_one(problem, input) !=
            predict_float_argmax_one(problem, input)) {
            std::cerr << "float-softmax-argmax-mismatch," << sample << '\n';
            return false;
        }
        const std::int8_t *qinput = problem.qinput.data() + sample * features;
        int scalar = predict_quantized_one(problem, qinput, dot_scalar);
#if defined(__aarch64__)
        int neon = predict_quantized_one(problem, qinput, dot_neon);
        int neon4 = predict_quantized_one(problem, qinput, dot_neon4);
        if (scalar != neon || scalar != neon4) {
            std::cerr << "quantized-mismatch," << sample << '\n';
            return false;
        }
#else
        (void) qinput;
        (void) scalar;
#endif
    }
#if defined(__aarch64__)
    if (predict_scalar_i8(problem) != predict_batch4_i8(problem)) {
        std::cerr << "batch-mismatch\n";
        return false;
    }
#endif
    constexpr std::int32_t max_sum = features * 127 * 127;
    static_assert(max_sum < std::numeric_limits<std::int32_t>::max());
#if defined(__aarch64__)
    std::cout << "tests,ok,257-samples-all-kernels\n";
#else
    std::cout << "tests,ok,257-samples-portable-kernels\n";
#endif
    return true;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

double measure(const Problem &problem, Predictor predictor,
               std::uint64_t &checksum) {
    std::vector<double> samples;
    samples.reserve(9);
    for (int sample = -2; sample < 9; ++sample) {
        auto start = Clock::now();
        std::uint64_t result = predictor(problem);
        auto stop = Clock::now();
        checksum ^= result;
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns / static_cast<double>(problem.samples));
        }
    }
    return median(samples);
}

void bench() {
    Problem problem = make_problem(1U << 13, 0x5eed1234U);
    std::vector<int> reference = predictions_float(problem);
    std::vector<int> quantized = predictions_quantized(problem, dot_scalar);
    double int8_disagreement = disagreement(reference, quantized);
    std::uint64_t checksum = 0;
    std::cout << "kind,variant,value\n";
    for (const Variant &variant : variants) {
        double ns = measure(problem, variant.predictor, checksum);
        std::cout << "performance," << variant.name << ',' << ns << '\n';
    }
    std::cout << "quality,int8-disagreement-percent," << int8_disagreement << '\n';
    for (int bits = 4; bits <= 8; ++bits) {
        int limit = (1 << (bits - 1)) - 1;
        Problem reduced = make_problem(problem.samples, 0x5eed1234U, limit);
        double rate = disagreement(reference, predictions_quantized(reduced, dot_scalar));
        std::cout << "disagreement," << bits << ',' << rate << '\n';
    }
    std::cerr << "checksum," << checksum << '\n';
}

}  // namespace

int main(int argc, char **argv) {
    std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "test") return test() ? 0 : 1;
    if (mode == "bench") {
        bench();
        return 0;
    }
    if (mode == "all") {
        if (!test()) return 1;
        bench();
        return 0;
    }
    std::cerr << "usage: " << argv[0] << " [test|bench|all]\n";
    return 2;
}
