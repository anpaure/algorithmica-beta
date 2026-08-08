/*
Build:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror fft_case.cpp -o fft_case

Run:
  ./fft_case test
  ./fft_case bench
  ./fft_case csv
*/

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using Complex = std::complex<double>;
using Clock = std::chrono::steady_clock;

constexpr double pi = 3.141592653589793238462643383279502884;

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

void fft_recursive_impl(std::vector<Complex> &a, Complex root) {
    int n = int(a.size());
    if (n == 1)
        return;
    std::vector<Complex> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; i++) {
        even[i] = a[2 * i];
        odd[i] = a[2 * i + 1];
    }
    fft_recursive_impl(even, root * root);
    fft_recursive_impl(odd, root * root);
    Complex w = 1;
    for (int i = 0; i < n / 2; i++) {
        Complex value = w * odd[i];
        a[i] = even[i] + value;
        a[i + n / 2] = even[i] - value;
        w *= root;
    }
}

void fft_recursive(std::vector<Complex> &a, bool inverse = false) {
    double angle = (inverse ? 2 : -2) * pi / double(a.size());
    fft_recursive_impl(a, std::polar(1.0, angle));
    if (inverse)
        for (Complex &value : a)
            value /= double(a.size());
}

void bit_reverse_dynamic(std::vector<Complex> &a) {
    int n = int(a.size());
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
}

void fft_iterative(std::vector<Complex> &a, bool inverse = false) {
    int n = int(a.size());
    bit_reverse_dynamic(a);
    for (int length = 2; length <= n; length *= 2) {
        double angle = (inverse ? 2 : -2) * pi / length;
        Complex step = std::polar(1.0, angle);
        for (int first = 0; first < n; first += length) {
            Complex w = 1;
            for (int i = 0; i < length / 2; i++) {
                Complex left = a[first + i];
                Complex right = w * a[first + i + length / 2];
                a[first + i] = left + right;
                a[first + i + length / 2] = left - right;
                w *= step;
            }
        }
    }
    if (inverse)
        for (Complex &value : a)
            value /= double(n);
}

struct Plan {
    int n;
    std::vector<int> reversed;
    std::vector<Complex> roots;

    explicit Plan(int size, bool make_reversal = true)
        : n(size), reversed(make_reversal ? size : 0), roots(size / 2) {
        if (make_reversal) {
            int bits = 0;
            while ((1 << bits) < n)
                bits++;
            for (int i = 0; i < n; i++) {
                unsigned value = unsigned(i);
                unsigned result = 0;
                for (int bit = 0; bit < bits; bit++) {
                    result = (result << 1) | (value & 1);
                    value >>= 1;
                }
                reversed[i] = int(result);
            }
        }
        for (int i = 0; i < n / 2; i++)
            roots[i] = std::polar(1.0, -2 * pi * i / double(n));
    }
};

void fft_roots(std::vector<Complex> &a, const Plan &plan,
               bool inverse = false) {
    int n = int(a.size());
    bit_reverse_dynamic(a);
    for (int length = 2; length <= n; length *= 2) {
        int stride = n / length;
        for (int first = 0; first < n; first += length)
            for (int i = 0; i < length / 2; i++) {
                Complex root = plan.roots[i * stride];
                if (inverse)
                    root = std::conj(root);
                Complex left = a[first + i];
                Complex right = root * a[first + i + length / 2];
                a[first + i] = left + right;
                a[first + i + length / 2] = left - right;
            }
    }
    if (inverse)
        for (Complex &value : a)
            value /= double(n);
}

void fft_planned(std::vector<Complex> &a, const Plan &plan,
                 bool inverse = false) {
    int n = int(a.size());
    for (int i = 0; i < n; i++)
        if (i < plan.reversed[i])
            std::swap(a[i], a[plan.reversed[i]]);
    for (int length = 2; length <= n; length *= 2) {
        int stride = n / length;
        for (int first = 0; first < n; first += length)
            for (int i = 0; i < length / 2; i++) {
                Complex root = plan.roots[i * stride];
                if (inverse)
                    root = std::conj(root);
                Complex left = a[first + i];
                Complex right = root * a[first + i + length / 2];
                a[first + i] = left + right;
                a[first + i + length / 2] = left - right;
            }
    }
    if (inverse)
        for (Complex &value : a)
            value /= double(n);
}

std::vector<Complex> naive_dft(const std::vector<Complex> &a) {
    int n = int(a.size());
    std::vector<Complex> result(n);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < n; j++)
            result[k] += a[j] * std::polar(1.0, -2 * pi * j * k / double(n));
    return result;
}

double max_error(const std::vector<Complex> &a,
                 const std::vector<Complex> &b) {
    require(a.size() == b.size(), "error vectors have different sizes");
    double result = 0;
    for (std::size_t i = 0; i < a.size(); i++)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}

std::vector<Complex> make_input(int n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> distribution(-1, 1);
    std::vector<Complex> result(n);
    for (Complex &value : result)
        value = {distribution(rng), distribution(rng)};
    return result;
}

void run_tests() {
    for (int logn = 0; logn <= 6; logn++) {
        int n = 1 << logn;
        std::vector<Complex> input = make_input(n, 0x93117 + n);
        std::vector<Complex> expected = naive_dft(input);
        Plan roots(n, false);
        Plan plan(n);
        for (int algorithm = 0; algorithm < 4; algorithm++) {
            std::vector<Complex> actual = input;
            if (algorithm == 0) fft_recursive(actual);
            if (algorithm == 1) fft_iterative(actual);
            if (algorithm == 2) fft_roots(actual, roots);
            if (algorithm == 3) fft_planned(actual, plan);
            require(max_error(actual, expected) < 2e-10,
                    "FFT disagrees with direct DFT");
        }
    }

    for (int logn = 0; logn <= 16; logn++) {
        int n = 1 << logn;
        std::vector<Complex> input = make_input(n, 0x715ab3 + n);
        Plan plan(n);
        for (int algorithm = 0; algorithm < 2; algorithm++) {
            std::vector<Complex> actual = input;
            if (algorithm == 0) {
                fft_iterative(actual);
                fft_iterative(actual, true);
            } else {
                fft_planned(actual, plan);
                fft_planned(actual, plan, true);
            }
            require(max_error(actual, input) < 2e-9,
                    "FFT round trip exceeded error bound");
        }
    }
    std::puts("all FFT tests passed");
}

enum class Algorithm { recursive, iterative, roots, planned };

const char *algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::recursive: return "recursive_alloc";
        case Algorithm::iterative: return "iterative_recurrence";
        case Algorithm::roots: return "precomputed_roots";
        case Algorithm::planned: return "planned";
    }
    return "unknown";
}

volatile double benchmark_sink = 0;

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double benchmark(Algorithm algorithm, const std::vector<Complex> &input,
                 const Plan &plan, int warmups, int repeats) {
    std::vector<Complex> work;
    std::vector<double> samples;
    for (int iteration = -warmups; iteration < repeats; iteration++) {
        work = input;
        auto start = Clock::now();
        if (algorithm == Algorithm::recursive) fft_recursive(work);
        if (algorithm == Algorithm::iterative) fft_iterative(work);
        if (algorithm == Algorithm::roots) fft_roots(work, plan);
        if (algorithm == Algorithm::planned) fft_planned(work, plan);
        auto stop = Clock::now();
        benchmark_sink += work[std::size_t(iteration + warmups) % work.size()].real();
        if (iteration >= 0)
            samples.push_back(
                std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(samples);
}

struct PhaseTimes { double permutation_ms; double butterflies_ms; };

PhaseTimes profile_roots(const std::vector<Complex> &input,
                         const Plan &plan) {
    std::vector<Complex> a = input;
    int n = int(a.size());
    auto start = Clock::now();
    bit_reverse_dynamic(a);
    auto middle = Clock::now();
    for (int length = 2; length <= n; length *= 2) {
        int stride = n / length;
        for (int first = 0; first < n; first += length)
            for (int i = 0; i < length / 2; i++) {
                Complex left = a[first + i];
                Complex right = plan.roots[i * stride] *
                                a[first + i + length / 2];
                a[first + i] = left + right;
                a[first + i + length / 2] = left - right;
            }
    }
    auto stop = Clock::now();
    benchmark_sink += a[n / 3].real();
    return {std::chrono::duration<double, std::milli>(middle - start).count(),
            std::chrono::duration<double, std::milli>(stop - middle).count()};
}

PhaseTimes profile_planned(const std::vector<Complex> &input,
                           const Plan &plan) {
    std::vector<Complex> a = input;
    int n = int(a.size());
    auto start = Clock::now();
    for (int i = 0; i < n; i++)
        if (i < plan.reversed[i])
            std::swap(a[i], a[plan.reversed[i]]);
    auto middle = Clock::now();
    for (int length = 2; length <= n; length *= 2) {
        int stride = n / length;
        for (int first = 0; first < n; first += length)
            for (int i = 0; i < length / 2; i++) {
                Complex left = a[first + i];
                Complex right = plan.roots[i * stride] *
                                a[first + i + length / 2];
                a[first + i] = left + right;
                a[first + i + length / 2] = left - right;
            }
    }
    auto stop = Clock::now();
    benchmark_sink += a[n / 3].real();
    return {std::chrono::duration<double, std::milli>(middle - start).count(),
            std::chrono::duration<double, std::milli>(stop - middle).count()};
}

void run_benchmarks() {
    int n = 1 << 18;
    std::vector<Complex> input = make_input(n, 0x2819bca7);
    Plan roots(n, false);
    Plan plan(n);
    std::puts("median forward-transform time; input restoration and planning excluded");
    for (Algorithm algorithm : {Algorithm::recursive, Algorithm::iterative,
                                Algorithm::roots, Algorithm::planned})
        std::printf("%-22s n=%d %9.3f ms\n", algorithm_name(algorithm), n,
                    benchmark(algorithm, input,
                              algorithm == Algorithm::planned ? plan : roots,
                              2, 7));
    PhaseTimes phase = profile_roots(input, roots);
    std::printf("precomputed-roots phases n=%d: permutation=%.3f butterflies=%.3f ms\n",
                n, phase.permutation_ms, phase.butterflies_ms);
    phase = profile_planned(input, plan);
    std::printf("planned-reversal phases n=%d: permutation=%.3f butterflies=%.3f ms\n",
                n, phase.permutation_ms, phase.butterflies_ms);
    std::printf("benchmark sink: %.9f\n", benchmark_sink);
}

void print_csv() {
    std::puts("kind,n,algorithm,milliseconds,error");
    for (int logn = 8; logn <= 20; logn++) {
        int n = 1 << logn;
        std::vector<Complex> input = make_input(n, 0xa19c70 + n);
        Plan roots(n, false);
        Plan plan(n);
        for (Algorithm algorithm : {Algorithm::recursive, Algorithm::iterative,
                                    Algorithm::roots, Algorithm::planned})
            std::printf("size,%d,%s,%.6f,0\n", n, algorithm_name(algorithm),
                        benchmark(algorithm, input,
                                  algorithm == Algorithm::planned ? plan : roots,
                                  1, 5));

        std::vector<Complex> recurrence = input;
        fft_iterative(recurrence);
        fft_iterative(recurrence, true);
        std::printf("error,%d,iterative_recurrence,0,%.17g\n", n,
                    max_error(recurrence, input));
        std::vector<Complex> planned = input;
        fft_planned(planned, plan);
        fft_planned(planned, plan, true);
        std::printf("error,%d,planned,0,%.17g\n", n,
                    max_error(planned, input));
    }
}

void print_stage_csv() {
    int n = 1 << 18;
    std::vector<Complex> input = make_input(n, 0x2819bca7);
    Plan roots(n, false);
    Plan plan(n);
    double recursive = benchmark(Algorithm::recursive, input, roots, 2, 7);
    double iterative = benchmark(Algorithm::iterative, input, roots, 2, 7);
    double direct_roots = benchmark(Algorithm::roots, input, roots, 2, 7);
    double planned = benchmark(Algorithm::planned, input, plan, 2, 7);
    PhaseTimes root_phase = profile_roots(input, roots);
    PhaseTimes planned_phase = profile_planned(input, plan);
    std::puts(
        "recursive_alloc,iterative_recurrence,precomputed_roots,planned,"
        "root_permutation_ms,root_butterflies_ms,"
        "planned_permutation_ms,planned_butterflies_ms"
    );
    std::printf("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                recursive, iterative, direct_roots, planned,
                root_phase.permutation_ms, root_phase.butterflies_ms,
                planned_phase.permutation_ms, planned_phase.butterflies_ms);
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "test";
    if (mode == "test") run_tests();
    else if (mode == "bench") run_benchmarks();
    else if (mode == "csv") print_csv();
    else if (mode == "stage-csv") print_stage_csv();
    else {
        std::fprintf(stderr, "usage: %s [test|bench|csv|stage-csv]\n", argv[0]);
        return 2;
    }
}
