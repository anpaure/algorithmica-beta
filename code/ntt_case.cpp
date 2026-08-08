#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 mod = 998244353;
constexpr u32 primitive_root = 3;
constexpr int max_log_n = 23;

volatile u64 benchmark_sink = 0;

u32 power(u32 a, u32 n) {
    u32 result = 1;
    while (n != 0) {
        if ((n & 1U) != 0)
            result = static_cast<u32>(u64(result) * a % mod);
        a = static_cast<u32>(u64(a) * a % mod);
        n >>= 1U;
    }
    return result;
}

bool valid_length(std::size_t n) {
    return n != 0 && (n & (n - 1)) == 0 && n <= (std::size_t{1} << max_log_n);
}

void check_input(const std::vector<u32>& a) {
    if (!valid_length(a.size()))
        throw std::invalid_argument("NTT length must be a power of two at most 2^23");
    if (std::any_of(a.begin(), a.end(), [](u32 x) { return x >= mod; }))
        throw std::invalid_argument("NTT coefficients must be reduced modulo q");
}

inline u32 add_mod(u32 a, u32 b) {
    u32 s = a + b;
    return s >= mod ? s - mod : s;
}

inline u32 sub_mod(u32 a, u32 b) {
    return a >= b ? a - b : a + mod - b;
}

__attribute__((noinline, used)) u32 multiply_remainder_probe(u32 a, u32 b) {
    return static_cast<u32>(u64(a) * b % mod);
}

struct Twiddle {
    u32 value;
    u32 shoup;
};

Twiddle make_twiddle(u32 value) {
    return {value, static_cast<u32>((u64(value) << 32U) / mod)};
}

inline u32 multiply_shoup(u32 a, const Twiddle& b) {
    u64 quotient = u64(a) * b.shoup >> 32U;
    u64 remainder = u64(a) * b.value - quotient * mod;
    if (remainder >= mod)
        remainder -= mod;
    return static_cast<u32>(remainder);
}

__attribute__((noinline, used)) u32 multiply_shoup_probe(u32 a, Twiddle b) {
    return multiply_shoup(a, b);
}

struct Plan {
    explicit Plan(std::size_t size)
        : n(size), forward(size), inverse(size) {
        if (!valid_length(n))
            throw std::invalid_argument("invalid NTT plan length");

        for (std::size_t len = 2; len <= n; len *= 2) {
            u32 step = power(primitive_root, static_cast<u32>((mod - 1) / len));
            u32 inverse_step = power(step, mod - 2);
            u32 w = 1;
            u32 inverse_w = 1;
            for (std::size_t j = 0; j < len / 2; ++j) {
                forward[len / 2 + j] = make_twiddle(w);
                inverse[len / 2 + j] = make_twiddle(inverse_w);
                w = static_cast<u32>(u64(w) * step % mod);
                inverse_w = static_cast<u32>(u64(inverse_w) * inverse_step % mod);
            }
        }
        inverse_n = make_twiddle(power(static_cast<u32>(n), mod - 2));
    }

    std::size_t n;
    std::vector<Twiddle> forward;
    std::vector<Twiddle> inverse;
    Twiddle inverse_n{};
};

void bit_reverse_online(std::vector<u32>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
}

// Textbook iterative NTT: it discovers the permutation, roots and consecutive
// powers again on every call. All three multiplication sites use C++ remainder.
void ntt_baseline(std::vector<u32>& a, bool invert) {
    assert(valid_length(a.size()));
    bit_reverse_online(a);

    const std::size_t n = a.size();
    for (std::size_t len = 2; len <= n; len *= 2) {
        u32 step = power(primitive_root, static_cast<u32>((mod - 1) / len));
        if (invert)
            step = power(step, mod - 2);

        for (std::size_t first = 0; first < n; first += len) {
            u32 w = 1;
            for (std::size_t j = 0; j < len / 2; ++j) {
                u32 u = a[first + j];
                u32 v = static_cast<u32>(u64(a[first + j + len / 2]) * w % mod);
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
                w = static_cast<u32>(u64(w) * step % mod);
            }
        }
    }

    if (invert) {
        u32 inverse_n = power(static_cast<u32>(n), mod - 2);
        for (u32& x : a)
            x = static_cast<u32>(u64(x) * inverse_n % mod);
    }
}

// Planning removes root exponentiation and the loop-carried w *= step chain,
// but deliberately leaves modular products as `% mod`.
void ntt_planned_remainder(std::vector<u32>& a, bool invert, const Plan& plan) {
    assert(a.size() == plan.n);
    bit_reverse_online(a);
    const auto& roots = invert ? plan.inverse : plan.forward;

    for (std::size_t len = 2; len <= a.size(); len *= 2) {
        for (std::size_t first = 0; first < a.size(); first += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                u32 u = a[first + j];
                u32 v = static_cast<u32>(u64(a[first + j + len / 2])
                                           * roots[len / 2 + j].value % mod);
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
            }
        }
    }

    if (invert)
        for (u32& x : a)
            x = static_cast<u32>(u64(x) * plan.inverse_n.value % mod);
}

void ntt_planned_shoup(std::vector<u32>& a, bool invert, const Plan& plan) {
    assert(a.size() == plan.n);
    bit_reverse_online(a);
    const auto& roots = invert ? plan.inverse : plan.forward;

    for (std::size_t len = 2; len <= a.size(); len *= 2) {
        for (std::size_t first = 0; first < a.size(); first += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                u32 u = a[first + j];
                u32 v = multiply_shoup(a[first + j + len / 2],
                                       roots[len / 2 + j]);
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
            }
        }
    }

    if (invert)
        for (u32& x : a)
            x = multiply_shoup(x, plan.inverse_n);
}

// DIF produces bit-reversed output. DIT consumes it, so a convolution can use
// this pair without ever materializing a permutation.
void ntt_forward_dif_remainder(std::vector<u32>& a, const Plan& plan) {
    assert(a.size() == plan.n);
    for (std::size_t len = a.size(); len >= 2; len /= 2) {
        for (std::size_t first = 0; first < a.size(); first += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                u32 u = a[first + j];
                u32 v = a[first + j + len / 2];
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = static_cast<u32>(
                    u64(sub_mod(u, v)) * plan.forward[len / 2 + j].value % mod);
            }
        }
    }
}

void ntt_inverse_dit_remainder(std::vector<u32>& a, const Plan& plan) {
    assert(a.size() == plan.n);
    for (std::size_t len = 2; len <= a.size(); len *= 2) {
        for (std::size_t first = 0; first < a.size(); first += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                u32 u = a[first + j];
                u32 v = static_cast<u32>(u64(a[first + j + len / 2])
                    * plan.inverse[len / 2 + j].value % mod);
                a[first + j] = add_mod(u, v);
                a[first + j + len / 2] = sub_mod(u, v);
            }
        }
    }
    for (u32& x : a)
        x = static_cast<u32>(u64(x) * plan.inverse_n.value % mod);
}

enum class Algorithm {
    baseline,
    planned_remainder,
    planned_shoup,
    permutation_free,
};

std::string_view algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::baseline: return "baseline";
    case Algorithm::planned_remainder: return "planned_remainder";
    case Algorithm::planned_shoup: return "planned_shoup";
    case Algorithm::permutation_free: return "permutation_free";
    }
    std::abort();
}

void forward_kernel(std::vector<u32>& a, Algorithm algorithm,
                    const Plan& plan) {
    switch (algorithm) {
    case Algorithm::baseline:
        ntt_baseline(a, false);
        return;
    case Algorithm::planned_remainder:
        ntt_planned_remainder(a, false, plan);
        return;
    case Algorithm::planned_shoup:
        ntt_planned_shoup(a, false, plan);
        return;
    case Algorithm::permutation_free:
        ntt_forward_dif_remainder(a, plan);
        return;
    }
    std::abort();
}

void inverse_kernel(std::vector<u32>& a, Algorithm algorithm,
                    const Plan& plan) {
    switch (algorithm) {
    case Algorithm::baseline:
        ntt_baseline(a, true);
        return;
    case Algorithm::planned_remainder:
        ntt_planned_remainder(a, true, plan);
        return;
    case Algorithm::planned_shoup:
        ntt_planned_shoup(a, true, plan);
        return;
    case Algorithm::permutation_free:
        ntt_inverse_dit_remainder(a, plan);
        return;
    }
    std::abort();
}

// Checked public boundaries. The benchmark performs the same validation once
// before timing and then calls the unchecked kernels above.
void forward_transform(std::vector<u32>& a, Algorithm algorithm,
                       const Plan& plan) {
    check_input(a);
    if (plan.n != a.size())
        throw std::invalid_argument("NTT plan length does not match the input");
    forward_kernel(a, algorithm, plan);
}

void inverse_transform(std::vector<u32>& a, Algorithm algorithm,
                       const Plan& plan) {
    check_input(a);
    if (plan.n != a.size())
        throw std::invalid_argument("NTT plan length does not match the input");
    inverse_kernel(a, algorithm, plan);
}

std::vector<u32> quadratic_convolution(const std::vector<u32>& a,
                                       const std::vector<u32>& b) {
    if (a.empty() || b.empty())
        return {};
    std::vector<u32> result(a.size() + b.size() - 1);
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j)
            result[i + j] = static_cast<u32>(
                (result[i + j] + u64(a[i]) * b[j]) % mod);
    return result;
}

std::vector<u32> transform_convolution(std::vector<u32> a,
                                       std::vector<u32> b,
                                       Algorithm algorithm) {
    if (a.empty() || b.empty())
        return {};
    std::size_t output_size = a.size() + b.size() - 1;
    std::size_t n = std::bit_ceil(output_size);
    if (!valid_length(n))
        throw std::invalid_argument("convolution does not fit this modulus");
    a.resize(n);
    b.resize(n);
    Plan plan(n);
    forward_transform(a, algorithm, plan);
    forward_transform(b, algorithm, plan);
    for (std::size_t i = 0; i < n; ++i)
        a[i] = static_cast<u32>(u64(a[i]) * b[i] % mod);
    inverse_transform(a, algorithm, plan);
    a.resize(output_size);
    return a;
}

void test() {
    std::mt19937_64 random(0x4e54545f43415345ULL);
    std::uniform_int_distribution<u32> residue(0, mod - 1);

    for (int repetition = 0; repetition < 1'000'000; ++repetition) {
        u32 a = residue(random);
        u32 b = residue(random);
        Twiddle twiddle = make_twiddle(b);
        u32 expected = static_cast<u32>(u64(a) * b % mod);
        if (multiply_shoup(a, twiddle) != expected)
            throw std::runtime_error("Shoup reduction mismatch");
    }

    constexpr std::array algorithms{
        Algorithm::baseline,
        Algorithm::planned_remainder,
        Algorithm::planned_shoup,
        Algorithm::permutation_free,
    };

    for (int exponent = 0; exponent <= 12; ++exponent) {
        std::size_t n = std::size_t{1} << exponent;
        Plan plan(n);
        for (int repetition = 0; repetition < 12; ++repetition) {
            std::vector<u32> input(n);
            std::generate(input.begin(), input.end(), [&] { return residue(random); });
            input.front() = 0;
            input.back() = mod - 1;
            for (Algorithm algorithm : algorithms) {
                std::vector<u32> transformed = input;
                forward_transform(transformed, algorithm, plan);
                inverse_transform(transformed, algorithm, plan);
                if (transformed != input)
                    throw std::runtime_error("round-trip mismatch");
            }
        }
    }

    std::uniform_int_distribution<int> small_size(0, 31);
    for (int repetition = 0; repetition < 400; ++repetition) {
        std::vector<u32> a(static_cast<std::size_t>(small_size(random)));
        std::vector<u32> b(static_cast<std::size_t>(small_size(random)));
        std::generate(a.begin(), a.end(), [&] { return residue(random); });
        std::generate(b.begin(), b.end(), [&] { return residue(random); });
        std::vector<u32> expected = quadratic_convolution(a, b);
        for (Algorithm algorithm : algorithms)
            if (transform_convolution(a, b, algorithm) != expected)
                throw std::runtime_error("convolution mismatch");
    }

    bool bad_length_rejected = false;
    try {
        Plan plan(4);
        std::vector<u32> invalid_length(3);
        forward_transform(invalid_length, Algorithm::planned_remainder, plan);
    } catch (const std::invalid_argument&) {
        bad_length_rejected = true;
    }
    if (!bad_length_rejected)
        throw std::runtime_error("invalid length was accepted");

    bool bad_residue_rejected = false;
    try {
        Plan plan(1);
        std::vector<u32> invalid_residue{mod};
        forward_transform(invalid_residue, Algorithm::planned_remainder, plan);
    } catch (const std::invalid_argument&) {
        bad_residue_rejected = true;
    }
    if (!bad_residue_rejected)
        throw std::runtime_error("unreduced coefficient was accepted");

    bool wrong_plan_rejected = false;
    try {
        Plan plan(2);
        std::vector<u32> input(4);
        forward_transform(input, Algorithm::planned_remainder, plan);
    } catch (const std::invalid_argument&) {
        wrong_plan_rejected = true;
    }
    if (!wrong_plan_rejected)
        throw std::runtime_error("mismatched plan was accepted");

    // Keep the two probe functions alive in optimized test builds.
    benchmark_sink = multiply_remainder_probe(1234567, 7654321)
                   + multiply_shoup_probe(1234567, make_twiddle(7654321));
    std::cerr << "all NTT differential tests passed\n";
}

double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

struct Measurement {
    double nanoseconds;
    int transforms;
};

Measurement measure(Algorithm algorithm, const std::vector<u32>& input,
                    const Plan& plan) {
    const double butterflies = static_cast<double>(input.size())
                             * std::log2(static_cast<double>(input.size())) / 2.0;
    int transforms = static_cast<int>(std::ceil(30'000'000.0
                                                / std::max(1.0, butterflies)));
    transforms = std::clamp(transforms, 3, 2000);
    std::vector<double> samples;
    samples.reserve(7);
    std::vector<u32> work = input;

    forward_kernel(work, algorithm, plan);
    benchmark_sink = benchmark_sink + work[input.size() / 3];

    for (int sample = 0; sample < 7; ++sample) {
        work = input;
        auto start = std::chrono::steady_clock::now();
        for (int repetition = 0; repetition < transforms; ++repetition)
            forward_kernel(work, algorithm, plan);
        auto stop = std::chrono::steady_clock::now();
        benchmark_sink = benchmark_sink + work[(sample * 104729U) % input.size()];
        samples.push_back(std::chrono::duration<double, std::nano>(stop - start).count()
                          / transforms);
    }
    return {median(samples), transforms};
}

void bench() {
    constexpr std::array algorithms{
        Algorithm::baseline,
        Algorithm::planned_remainder,
        Algorithm::planned_shoup,
        Algorithm::permutation_free,
    };
    std::mt19937 random(0x4e5454U);
    std::uniform_int_distribution<u32> residue(0, mod - 1);

    std::cout << "algorithm,n,log_n,median_ns,ns_per_butterfly,"
                 "million_butterflies_per_s,transforms_per_sample\n";
    for (int exponent = 8; exponent <= 20; ++exponent) {
        std::size_t n = std::size_t{1} << exponent;
        std::vector<u32> input(n);
        std::generate(input.begin(), input.end(), [&] { return residue(random); });
        check_input(input);
        Plan plan(n);
        double butterflies = static_cast<double>(n) * exponent / 2.0;
        for (Algorithm algorithm : algorithms) {
            Measurement result = measure(algorithm, input, plan);
            double ns_per_butterfly = result.nanoseconds / butterflies;
            double million_butterflies_per_second = 1000.0 / ns_per_butterfly;
            std::cout << algorithm_name(algorithm) << ',' << n << ',' << exponent
                      << ',' << result.nanoseconds << ',' << ns_per_butterfly
                      << ',' << million_butterflies_per_second << ','
                      << result.transforms << '\n';
        }
    }
    std::cerr << "benchmark checksum: " << benchmark_sink << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--test") {
            test();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--bench") {
            bench();
            return 0;
        }
        std::cerr << "usage: " << argv[0] << " --test|--bench\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
