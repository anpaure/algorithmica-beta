/*
Build:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror sorting.cpp -o sorting

Run:
  ./sorting test
  ./sorting bench
  ./sorting csv
*/

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using u32 = std::uint32_t;

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

// Stable four-pass LSD radix sort. Allocation is intentionally inside.
void radix8_alloc(std::vector<u32> &a) {
    std::vector<u32> buffer(a.size());

    for (int shift = 0; shift < 32; shift += 8) {
        std::size_t count[256] = {};
        std::size_t position[256];

        for (u32 x : a)
            count[(x >> shift) & 255]++;

        position[0] = 0;
        for (int digit = 1; digit < 256; digit++)
            position[digit] = position[digit - 1] + count[digit - 1];

        for (u32 x : a) {
            int digit = (x >> shift) & 255;
            buffer[position[digit]++] = x;
        }
        a.swap(buffer);
    }
}

// Same algorithm, but the caller owns and reuses the temporary array.
void radix8_reuse(std::vector<u32> &a, std::vector<u32> &buffer) {
    buffer.resize(a.size());

    for (int shift = 0; shift < 32; shift += 8) {
        std::size_t count[256] = {};
        std::size_t position[256];

        for (u32 x : a)
            count[(x >> shift) & 255]++;

        position[0] = 0;
        for (int digit = 1; digit < 256; digit++)
            position[digit] = position[digit - 1] + count[digit - 1];

        for (u32 x : a) {
            int digit = (x >> shift) & 255;
            buffer[position[digit]++] = x;
        }
        a.swap(buffer);
    }
}

// Three digits (11, 11, and 10 bits) reduce the number of full array passes.
void radix11(std::vector<u32> &a, std::vector<u32> &buffer) {
    constexpr int width[3] = {11, 11, 10};
    constexpr int shift[3] = {0, 11, 22};
    constexpr int max_buckets = 1 << 11;
    std::array<std::size_t, max_buckets> count{};
    std::array<std::size_t, max_buckets> position;
    buffer.resize(a.size());

    for (int pass = 0; pass < 3; pass++) {
        int buckets = 1 << width[pass];
        u32 mask = u32(buckets - 1);
        std::fill(count.begin(), count.begin() + buckets, 0);

        for (u32 x : a)
            count[(x >> shift[pass]) & mask]++;

        position[0] = 0;
        for (int digit = 1; digit < buckets; digit++)
            position[digit] = position[digit - 1] + count[digit - 1];

        for (u32 x : a) {
            u32 digit = (x >> shift[pass]) & mask;
            buffer[position[digit]++] = x;
        }
        a.swap(buffer);
    }
}

// Benchmark-only generic kernel for selecting a radix width. The caller owns
// both workspaces, so the sweep measures passes and histogram size, not malloc.
void radix_width(std::vector<u32> &a, std::vector<u32> &buffer,
                 std::vector<std::size_t> &count, int width) {
    buffer.resize(a.size());
    count.resize(std::size_t(1) << width);

    for (int shift = 0; shift < 32; shift += width) {
        int bits = std::min(width, 32 - shift);
        int buckets = 1 << bits;
        u32 mask = u32(buckets - 1);
        std::fill(count.begin(), count.begin() + buckets, 0);

        for (u32 x : a)
            count[(x >> shift) & mask]++;

        std::size_t sum = 0;
        for (int digit = 0; digit < buckets; digit++) {
            std::size_t frequency = count[digit];
            count[digit] = sum;
            sum += frequency;
        }

        for (u32 x : a) {
            u32 digit = (x >> shift) & mask;
            buffer[count[digit]++] = x;
        }
        a.swap(buffer);
    }
}

// Count all digits during one read. This saves scans, but four dependent
// histogram updates per element put more pressure on the load/store units.
void radix8_precount(std::vector<u32> &a, std::vector<u32> &buffer) {
    std::array<std::array<std::size_t, 256>, 4> count{};
    std::array<std::size_t, 256> position;
    buffer.resize(a.size());

    for (u32 x : a) {
        count[0][x & 255]++;
        count[1][(x >> 8) & 255]++;
        count[2][(x >> 16) & 255]++;
        count[3][x >> 24]++;
    }

    for (int pass = 0; pass < 4; pass++) {
        position[0] = 0;
        for (int digit = 1; digit < 256; digit++)
            position[digit] = position[digit - 1] + count[pass][digit - 1];

        int shift = 8 * pass;
        for (u32 x : a) {
            u32 digit = (x >> shift) & 255;
            buffer[position[digit]++] = x;
        }
        a.swap(buffer);
    }
}

enum class Algorithm {
    std_sort,
    radix8_allocating,
    radix8_workspace,
    radix11_workspace,
    radix8_all_counts,
};

const char *algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::std_sort: return "std_sort";
        case Algorithm::radix8_allocating: return "radix8_alloc";
        case Algorithm::radix8_workspace: return "radix8_reuse";
        case Algorithm::radix11_workspace: return "radix11";
        case Algorithm::radix8_all_counts: return "radix8_precount";
    }
    return "unknown";
}

void run_algorithm(Algorithm algorithm, std::vector<u32> &a,
                   std::vector<u32> &buffer) {
    switch (algorithm) {
        case Algorithm::std_sort:
            std::sort(a.begin(), a.end());
            break;
        case Algorithm::radix8_allocating:
            radix8_alloc(a);
            break;
        case Algorithm::radix8_workspace:
            radix8_reuse(a, buffer);
            break;
        case Algorithm::radix11_workspace:
            radix11(a, buffer);
            break;
        case Algorithm::radix8_all_counts:
            radix8_precount(a, buffer);
            break;
    }
}

enum class Distribution {
    random,
    sorted,
    reverse,
    low8,
    nearly_sorted,
};

const char *distribution_name(Distribution distribution) {
    switch (distribution) {
        case Distribution::random: return "random";
        case Distribution::sorted: return "sorted";
        case Distribution::reverse: return "reverse";
        case Distribution::low8: return "low8";
        case Distribution::nearly_sorted: return "nearly_sorted";
    }
    return "unknown";
}

std::vector<u32> make_input(std::size_t n, Distribution distribution,
                            std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<u32> a(n);

    if (distribution == Distribution::low8) {
        for (u32 &x : a)
            x = u32(rng() & 255);
        return a;
    }

    for (u32 &x : a)
        x = u32(rng());

    if (distribution == Distribution::sorted) {
        std::sort(a.begin(), a.end());
    } else if (distribution == Distribution::reverse) {
        std::sort(a.begin(), a.end(), std::greater<u32>());
    } else if (distribution == Distribution::nearly_sorted) {
        std::sort(a.begin(), a.end());
        std::size_t swaps = n / 100;
        for (std::size_t i = 0; i < swaps; i++) {
            std::size_t x = std::size_t(rng() % n);
            std::size_t y = std::size_t(rng() % n);
            std::swap(a[x], a[y]);
        }
    }
    return a;
}

volatile std::uint64_t benchmark_sink = 0;

double median(std::vector<double> sample) {
    std::sort(sample.begin(), sample.end());
    return sample[sample.size() / 2];
}

double benchmark(Algorithm algorithm, const std::vector<u32> &input,
                 int warmups = 2, int repeats = 7) {
    std::vector<u32> work;
    std::vector<u32> buffer(input.size());
    std::vector<double> sample;

    for (int iteration = -warmups; iteration < repeats; iteration++) {
        work = input; // restoring the input is deliberately outside timing
        auto start = std::chrono::steady_clock::now();
        run_algorithm(algorithm, work, buffer);
        auto stop = std::chrono::steady_clock::now();

        require(std::is_sorted(work.begin(), work.end()), "benchmark result is not sorted");
        if (!work.empty())
            benchmark_sink ^= std::uint64_t(work.front()) << 32 | work.back();
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(sample);
}

double benchmark_width(int width, const std::vector<u32> &input,
                       int warmups = 2, int repeats = 7) {
    std::vector<u32> work;
    std::vector<u32> buffer(input.size());
    std::vector<std::size_t> count(std::size_t(1) << width);
    std::vector<double> sample;

    for (int iteration = -warmups; iteration < repeats; iteration++) {
        work = input;
        auto start = std::chrono::steady_clock::now();
        radix_width(work, buffer, count, width);
        auto stop = std::chrono::steady_clock::now();
        require(std::is_sorted(work.begin(), work.end()),
                "radix-width result is not sorted");
        if (!work.empty())
            benchmark_sink ^= work[work.size() / 2];
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(sample);
}

struct PhaseTimes {
    double count_ms = 0;
    double prefix_ms = 0;
    double scatter_ms = 0;
};

PhaseTimes profile_radix11(const std::vector<u32> &input) {
    constexpr int width[3] = {11, 11, 10};
    constexpr int shift[3] = {0, 11, 22};
    std::array<std::size_t, 1 << 11> count{};
    std::array<std::size_t, 1 << 11> position;
    std::vector<u32> a = input;
    std::vector<u32> buffer(a.size());
    PhaseTimes result;

    for (int pass = 0; pass < 3; pass++) {
        int buckets = 1 << width[pass];
        u32 mask = u32(buckets - 1);

        auto start = std::chrono::steady_clock::now();
        std::fill(count.begin(), count.begin() + buckets, 0);
        for (u32 x : a)
            count[(x >> shift[pass]) & mask]++;
        auto after_count = std::chrono::steady_clock::now();

        position[0] = 0;
        for (int digit = 1; digit < buckets; digit++)
            position[digit] = position[digit - 1] + count[digit - 1];
        auto after_prefix = std::chrono::steady_clock::now();

        for (u32 x : a) {
            u32 digit = (x >> shift[pass]) & mask;
            buffer[position[digit]++] = x;
        }
        a.swap(buffer);
        auto after_scatter = std::chrono::steady_clock::now();

        result.count_ms += std::chrono::duration<double, std::milli>(after_count - start).count();
        result.prefix_ms += std::chrono::duration<double, std::milli>(after_prefix - after_count).count();
        result.scatter_ms += std::chrono::duration<double, std::milli>(after_scatter - after_prefix).count();
    }
    benchmark_sink ^= a.empty() ? 0 : a[a.size() / 2];
    return result;
}

void test_one(const std::vector<u32> &input) {
    std::vector<u32> expected = input;
    std::sort(expected.begin(), expected.end());
    std::vector<u32> buffer(input.size());

    for (Algorithm algorithm : {Algorithm::radix8_allocating,
                                Algorithm::radix8_workspace,
                                Algorithm::radix11_workspace,
                                Algorithm::radix8_all_counts}) {
        std::vector<u32> actual = input;
        run_algorithm(algorithm, actual, buffer);
        require(actual == expected, "radix sort disagrees with std::sort");
    }

    for (int width : {4, 8, 10, 11, 12, 16}) {
        std::vector<u32> actual = input;
        std::vector<u32> width_buffer(input.size());
        std::vector<std::size_t> count(std::size_t(1) << width);
        radix_width(actual, width_buffer, count, width);
        require(actual == expected, "generic radix width disagrees with std::sort");
    }
}

void run_tests() {
    std::mt19937_64 rng(0xa83091c5e4d672bfULL);
    test_one({});
    test_one({0});
    test_one({0, UINT32_MAX, 1, UINT32_MAX - 1, 0x80000000u, 0x7fffffffu});

    for (int test = 0; test < 5000; test++) {
        int n = int(rng() % 2000);
        std::vector<u32> input(n);
        for (u32 &x : input) {
            if ((test & 3) == 0)
                x = u32(rng() & 255);
            else
                x = u32(rng());
        }
        test_one(input);
    }
    std::puts("all sorting tests passed");
}

void print_case(std::size_t n, Distribution distribution) {
    std::vector<u32> input = make_input(n, distribution, 123456789);
    for (Algorithm algorithm : {Algorithm::std_sort,
                                Algorithm::radix8_allocating,
                                Algorithm::radix8_workspace,
                                Algorithm::radix11_workspace,
                                Algorithm::radix8_all_counts}) {
        double ms = benchmark(algorithm, input);
        std::printf("%-17s %-14s n=%9zu  %9.3f ms  %8.1f Mkeys/s\n",
                    algorithm_name(algorithm), distribution_name(distribution), n,
                    ms, double(n) / ms / 1000.0);
    }
}

void run_benchmarks() {
    std::puts("median kernel time; input restoration is excluded");
    print_case(1u << 20, Distribution::random);
    print_case(1u << 20, Distribution::low8);
    print_case(1u << 20, Distribution::sorted);

    std::vector<u32> input = make_input(1u << 22, Distribution::random, 123456789);
    PhaseTimes phase = profile_radix11(input);
    std::printf("radix11 phases n=%zu: count=%.3f ms prefix=%.3f ms scatter=%.3f ms\n",
                input.size(), phase.count_ms, phase.prefix_ms, phase.scatter_ms);

    input = make_input(1u << 20, Distribution::random, 0x51d3a117);
    for (int width : {4, 8, 10, 11, 12, 16})
        std::printf("radix width=%2d passes=%d histogram=%6zu KiB %8.3f ms\n",
                    width, (31 + width) / width,
                    ((std::size_t(1) << width) * sizeof(std::size_t)) >> 10,
                    benchmark_width(width, input));
    std::printf("benchmark sink: %llu\n", (unsigned long long) benchmark_sink);
}

void print_csv() {
    std::puts("kind,n,distribution,algorithm,milliseconds");
    {
        std::size_t n = 1u << 20;
        std::vector<u32> input =
            make_input(n, Distribution::random, 123456789);
        for (Algorithm algorithm : {Algorithm::std_sort,
                                    Algorithm::radix8_allocating,
                                    Algorithm::radix8_workspace,
                                    Algorithm::radix11_workspace,
                                    Algorithm::radix8_all_counts})
            std::printf("headline,%zu,random,%s,%.6f\n", n,
                        algorithm_name(algorithm), benchmark(algorithm, input));
    }

    {
        std::vector<u32> input =
            make_input(1u << 22, Distribution::random, 123456789);
        PhaseTimes phase = profile_radix11(input);
        std::printf("phase,%zu,count,radix11,%.6f\n",
                    input.size(), phase.count_ms);
        std::printf("phase,%zu,prefix,radix11,%.6f\n",
                    input.size(), phase.prefix_ms);
        std::printf("phase,%zu,scatter,radix11,%.6f\n",
                    input.size(), phase.scatter_ms);
    }

    for (int logn = 10; logn <= 23; logn++) {
        std::size_t n = std::size_t(1) << logn;
        std::vector<u32> input = make_input(n, Distribution::random, 123456789 + logn);
        for (Algorithm algorithm : {Algorithm::std_sort,
                                    Algorithm::radix8_workspace,
                                    Algorithm::radix11_workspace,
                                    Algorithm::radix8_all_counts}) {
            double ms = benchmark(algorithm, input, 1, 5);
            std::printf("size,%zu,random,%s,%.6f\n", n,
                        algorithm_name(algorithm), ms);
        }
    }

    std::size_t n = 1u << 20;
    for (Distribution distribution : {Distribution::random,
                                      Distribution::sorted,
                                      Distribution::reverse,
                                      Distribution::low8,
                                      Distribution::nearly_sorted}) {
        std::vector<u32> input = make_input(n, distribution, 987654321);
        for (Algorithm algorithm : {Algorithm::std_sort,
                                    Algorithm::radix8_workspace,
                                    Algorithm::radix11_workspace,
                                    Algorithm::radix8_all_counts}) {
            double ms = benchmark(algorithm, input, 1, 5);
            std::printf("distribution,%zu,%s,%s,%.6f\n", n,
                        distribution_name(distribution), algorithm_name(algorithm), ms);
        }
    }


    std::vector<u32> width_input =
        make_input(1u << 20, Distribution::random, 0x51d3a117);
    for (int width : {4, 8, 10, 11, 12, 16})
        std::printf("width,%zu,%d,radix_width,%.6f\n", width_input.size(),
                    width, benchmark_width(width, width_input, 1, 5));
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
