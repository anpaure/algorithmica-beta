/*
Build:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror \
      big_integers.cpp -o big_integers

Run:
  ./big_integers test
  ./big_integers bench
  ./big_integers csv
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using i64 = std::int64_t;
using u64 = std::uint64_t;

constexpr i64 base = 10000;

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

__attribute__((noinline))
void schoolbook_accumulate(const i64 *__restrict a,
                           const i64 *__restrict b,
                           i64 *__restrict c, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            c[i + j] += a[i] * b[j];
}

void schoolbook(const i64 *__restrict a, const i64 *__restrict b,
                i64 *__restrict c, int n) {
    std::fill(c, c + 2 * n, 0);
    schoolbook_accumulate(a, b, c, n);
}

std::vector<i64> schoolbook_vector(const std::vector<i64> &a,
                                   const std::vector<i64> &b) {
    int n = int(a.size());
    std::vector<i64> c(2 * n);
    schoolbook_accumulate(a.data(), b.data(), c.data(), n);
    return c;
}

struct AllocationStats {
    u64 vectors = 0;
    u64 elements = 0;
};

// A literal vector implementation. Track=false compiles the counters away in
// the timed kernel; Track=true counts source-level vector allocation requests.
template <bool Track>
std::vector<i64> karatsuba_allocating(const std::vector<i64> &a,
                                      const std::vector<i64> &b,
                                      int cutoff,
                                      AllocationStats *stats) {
    int n = int(a.size());
    if (n <= cutoff) {
        if constexpr (Track) {
            stats->vectors++;
            stats->elements += u64(2 * n);
        }
        return schoolbook_vector(a, b);
    }

    int k = n / 2;
    std::vector<i64> a0(a.begin(), a.begin() + k);
    std::vector<i64> a1(a.begin() + k, a.end());
    std::vector<i64> b0(b.begin(), b.begin() + k);
    std::vector<i64> b1(b.begin() + k, b.end());
    std::vector<i64> as(k), bs(k);
    if constexpr (Track) {
        stats->vectors += 6;
        stats->elements += u64(6 * k);
    }

    for (int i = 0; i < k; i++) {
        as[i] = a0[i] + a1[i];
        bs[i] = b0[i] + b1[i];
    }

    std::vector<i64> low =
        karatsuba_allocating<Track>(a0, b0, cutoff, stats);
    std::vector<i64> high =
        karatsuba_allocating<Track>(a1, b1, cutoff, stats);
    std::vector<i64> middle =
        karatsuba_allocating<Track>(as, bs, cutoff, stats);

    std::vector<i64> c(2 * n);
    if constexpr (Track) {
        stats->vectors++;
        stats->elements += u64(2 * n);
    }
    for (int i = 0; i < n; i++) {
        c[i] += low[i];
        c[k + i] += middle[i] - low[i] - high[i];
        c[n + i] += high[i];
    }
    return c;
}

// c has 2*n elements, scratch has 4*n elements, and all four buffers are
// disjoint. n is a power of two.
void karatsuba_workspace(const i64 *a, const i64 *b, i64 *c,
                         i64 *scratch, int n, int cutoff) {
    if (n <= cutoff) {
        schoolbook(a, b, c, n);
        return;
    }

    int k = n / 2;
    karatsuba_workspace(a, b, c, scratch, k, cutoff);
    karatsuba_workspace(a + k, b + k, c + n, scratch, k, cutoff);

    i64 *as = scratch;
    i64 *bs = as + k;
    i64 *middle = bs + k;
    i64 *next = middle + n;
    for (int i = 0; i < k; i++) {
        as[i] = a[i] + a[k + i];
        bs[i] = b[i] + b[k + i];
    }
    karatsuba_workspace(as, bs, middle, next, k, cutoff);

    for (int i = 0; i < n; i++)
        middle[i] -= c[i] + c[n + i];
    for (int i = 0; i < n; i++)
        c[k + i] += middle[i];
}

std::vector<i64> carry(std::vector<i64> coefficients) {
    for (std::size_t i = 0; i + 1 < coefficients.size(); i++) {
        require(coefficients[i] >= 0, "negative final coefficient");
        coefficients[i + 1] += coefficients[i] / base;
        coefficients[i] %= base;
    }
    require(coefficients.empty() || coefficients.back() < base,
            "the fixed output array was too short for the final carry");
    while (coefficients.size() > 1 && coefficients.back() == 0)
        coefficients.pop_back();
    return coefficients;
}

std::vector<i64> make_operand(int n, u64 seed) {
    std::mt19937_64 rng(seed);
    std::vector<i64> result(n);
    for (i64 &digit : result)
        digit = i64(rng() % base);
    if (!result.empty() && result.back() == 0)
        result.back() = 1;
    return result;
}

void test_case(const std::vector<i64> &a, const std::vector<i64> &b) {
    int n = int(a.size());
    require(n > 0 && (n & (n - 1)) == 0, "test length is not a power of two");
    require(b.size() == a.size(), "unbalanced test operands");

    std::vector<i64> expected(2 * n);
    schoolbook(a.data(), b.data(), expected.data(), n);

    AllocationStats unused;
    std::vector<i64> allocated =
        karatsuba_allocating<false>(a, b, 8, &unused);
    require(allocated == expected, "allocating Karatsuba disagrees with schoolbook");

    for (int cutoff : {1, 2, 4, 8, 16, 32, 64}) {
        std::vector<i64> actual(2 * n);
        std::vector<i64> scratch(4 * n);
        karatsuba_workspace(a.data(), b.data(), actual.data(),
                            scratch.data(), n, cutoff);
        require(actual == expected, "workspace Karatsuba disagrees with schoolbook");
    }

    std::vector<i64> digits = carry(allocated);
    require(!digits.empty(), "carry removed the whole result");
    for (i64 digit : digits)
        require(0 <= digit && digit < base, "carry produced an invalid digit");
}

void run_tests() {
    test_case({0}, {0});
    test_case({1}, {1});
    test_case({base - 1}, {base - 1});

    std::mt19937_64 rng(0x24d82ea91bff706dULL);
    for (int logn = 0; logn <= 9; logn++) {
        int n = 1 << logn;
        std::vector<i64> high(n, base - 1);
        test_case(high, high);
        for (int test = 0; test < 40; test++) {
            std::vector<i64> a(n), b(n);
            for (int i = 0; i < n; i++) {
                a[i] = i64(rng() % base);
                b[i] = i64(rng() % base);
            }
            test_case(a, b);
        }
    }
    std::puts("all big-integer tests passed");
}

enum class Algorithm { schoolbook, allocating, workspace };

const char *algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::schoolbook: return "schoolbook";
        case Algorithm::allocating: return "karatsuba_alloc";
        case Algorithm::workspace: return "karatsuba_workspace";
    }
    return "unknown";
}

volatile u64 benchmark_sink = 0;

inline void escape(const void *pointer) {
    asm volatile("" : : "r"(pointer) : "memory");
}

double median(std::vector<double> sample) {
    std::sort(sample.begin(), sample.end());
    return sample[sample.size() / 2];
}

double benchmark(Algorithm algorithm, const std::vector<i64> &a,
                 const std::vector<i64> &b, int cutoff,
                 int warmups, int repeats) {
    int n = int(a.size());
    u64 quadratic_work = u64(n) * u64(n);
    int batch = int(std::max<u64>(1, (u64(1) << 22) / quadratic_work));
    std::vector<i64> output(2 * n);
    std::vector<i64> scratch(4 * n);
    std::vector<double> samples;
    AllocationStats unused;

    for (int iteration = -warmups; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        for (int invocation = 0; invocation < batch; invocation++) {
            if (algorithm == Algorithm::schoolbook) {
                schoolbook(a.data(), b.data(), output.data(), n);
            } else if (algorithm == Algorithm::allocating) {
                output = karatsuba_allocating<false>(a, b, cutoff, &unused);
            } else {
                karatsuba_workspace(a.data(), b.data(), output.data(),
                                    scratch.data(), n, cutoff);
            }
            escape(output.data());
        }
        auto stop = std::chrono::steady_clock::now();
        benchmark_sink ^= u64(output[std::size_t(iteration + warmups) % output.size()]);
        if (iteration >= 0)
            samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count()
                              / batch);
    }
    return median(samples);
}

void print_benchmark(int n) {
    std::vector<i64> a = make_operand(n, 0x12345678 + u64(n));
    std::vector<i64> b = make_operand(n, 0x87654321 + u64(n));
    int repeats = n >= 8192 ? 5 : 7;
    for (Algorithm algorithm : {Algorithm::schoolbook,
                                Algorithm::allocating,
                                Algorithm::workspace}) {
        double us = benchmark(algorithm, a, b, 32, 2, repeats);
        std::printf("%-20s n=%5d cutoff=%2d %10.3f us\n",
                    algorithm_name(algorithm), n, 32, us);
    }
}

void run_benchmarks() {
    std::puts("median multiplication time; equal power-of-two base-10000 operands");
    for (int n : {32, 128, 512, 2048, 8192})
        print_benchmark(n);

    int n = 4096;
    std::vector<i64> a = make_operand(n, 0x31415926);
    std::vector<i64> b = make_operand(n, 0x27182818);
    for (int cutoff : {1, 2, 4, 8, 16, 32, 64, 128, 256}) {
        double us = benchmark(Algorithm::workspace, a, b, cutoff, 2, 7);
        std::printf("workspace cutoff sweep n=%d cutoff=%3d %10.3f us\n",
                    n, cutoff, us);
    }

    AllocationStats stats;
    (void) karatsuba_allocating<true>(a, b, 32, &stats);
    std::printf("allocating n=%d cutoff=32 requests %llu vectors / %llu elements\n",
                n, (unsigned long long) stats.vectors,
                (unsigned long long) stats.elements);
    std::printf("benchmark sink: %llu\n", (unsigned long long) benchmark_sink);
}

void print_csv() {
    std::puts("kind,n,algorithm,cutoff,microseconds");
    for (int logn = 4; logn <= 13; logn++) {
        int n = 1 << logn;
        std::vector<i64> a = make_operand(n, 0x12345678 + u64(n));
        std::vector<i64> b = make_operand(n, 0x87654321 + u64(n));
        int repeats = n >= 8192 ? 5 : 7;
        for (Algorithm algorithm : {Algorithm::schoolbook,
                                    Algorithm::allocating,
                                    Algorithm::workspace}) {
            double us = benchmark(algorithm, a, b, 32, 2, repeats);
            std::printf("size,%d,%s,32,%.6f\n", n,
                        algorithm_name(algorithm), us);
        }
    }

    int n = 4096;
    std::vector<i64> a = make_operand(n, 0x31415926);
    std::vector<i64> b = make_operand(n, 0x27182818);
    for (int cutoff : {1, 2, 4, 8, 16, 32, 64, 128, 256}) {
        double us = benchmark(Algorithm::workspace, a, b, cutoff, 2, 7);
        std::printf("cutoff,%d,karatsuba_workspace,%d,%.6f\n",
                    n, cutoff, us);
    }
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "test";
    if (mode == "test") {
        run_tests();
    } else if (mode == "bench") {
        run_benchmarks();
    } else if (mode == "csv") {
        print_csv();
    } else {
        std::fprintf(stderr, "usage: %s [test|bench|csv]\n", argv[0]);
        return 2;
    }
}
