/*
Build:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror \
      prime_sieves.cpp -o prime_sieves

Run:
  ./prime_sieves test
  ./prime_sieves bench
  ./prime_sieves csv
*/

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

u32 integer_sqrt(u32 n) {
    u32 root = u32(std::sqrt(static_cast<long double>(n)));
    while (u64(root + 1) * (root + 1) <= n)
        root++;
    while (u64(root) * root > n)
        root--;
    return root;
}

struct Workspace {
    std::vector<std::uint8_t> full;
    std::vector<std::uint8_t> odd;
    std::vector<std::uint8_t> segment;
    std::vector<std::uint8_t> base_marker;
    std::vector<u32> base_primes;
    std::vector<u64> next_multiple;

    void reserve(u32 n, std::size_t segment_size) {
        full.resize(std::size_t(n) + 1);
        odd.resize(std::size_t(n) / 2 + 1);
        segment.resize(segment_size);
        u32 root = integer_sqrt(n);
        base_marker.resize(std::size_t(root) / 2 + 1);
        base_primes.reserve(std::size_t(root) / 8 + 16);
        next_multiple.reserve(std::size_t(root) / 8 + 16);
    }
};

u64 count_full(u32 n, Workspace &workspace) {
    std::vector<std::uint8_t> &composite = workspace.full;
    composite.resize(std::size_t(n) + 1);
    std::fill(composite.begin(), composite.end(), 0);
    if (n < 2)
        return 0;
    composite[0] = composite[1] = 1;

    for (u32 p = 2; p <= n / p; p++)
        if (!composite[p])
            for (u64 multiple = u64(p) * p; multiple <= n; multiple += p)
                composite[std::size_t(multiple)] = 1;

    u64 count = 0;
    for (u32 value = 2; value <= n; value++)
        count += !composite[value];
    return count;
}

u64 count_odd(u32 n, Workspace &workspace) {
    if (n < 2)
        return 0;
    std::size_t slots = std::size_t(n) / 2 + 1;
    std::vector<std::uint8_t> &composite = workspace.odd;
    composite.resize(slots);
    std::fill(composite.begin(), composite.end(), 0);

    for (u32 p = 3; p <= n / p; p += 2)
        if (!composite[p / 2])
            for (u64 multiple = u64(p) * p; multiple <= n;
                 multiple += u64(2) * p)
                composite[std::size_t(multiple / 2)] = 1;

    u64 count = 1; // prime 2
    for (u32 value = 3; value <= n; value += 2)
        count += !composite[value / 2];
    return count;
}

void make_base_primes(u32 root, Workspace &workspace) {
    std::size_t slots = std::size_t(root) / 2 + 1;
    std::vector<std::uint8_t> &composite = workspace.base_marker;
    composite.resize(slots);
    std::fill(composite.begin(), composite.end(), 0);
    workspace.base_primes.clear();

    for (u32 p = 3; p <= root / p; p += 2)
        if (!composite[p / 2])
            for (u64 multiple = u64(p) * p; multiple <= root;
                 multiple += u64(2) * p)
                composite[std::size_t(multiple / 2)] = 1;

    for (u32 value = 3; value <= root; value += 2)
        if (!composite[value / 2])
            workspace.base_primes.push_back(value);
}

struct PhaseStats {
    double base_ms = 0;
    double clear_ms = 0;
    double mark_ms = 0;
    double count_ms = 0;
    u64 segments = 0;
};

enum class StartMode { divide, carry };

template <bool Profile>
u64 count_segmented(u32 n, std::size_t segment_size,
                    StartMode mode, Workspace &workspace,
                    PhaseStats *stats) {
    if (n < 2)
        return 0;

    Clock::time_point phase_start;
    if constexpr (Profile)
        phase_start = Clock::now();
    make_base_primes(integer_sqrt(n), workspace);
    if constexpr (Profile) {
        auto stop = Clock::now();
        stats->base_ms +=
            std::chrono::duration<double, std::milli>(stop - phase_start).count();
    }

    std::vector<u64> &next = workspace.next_multiple;
    if (mode == StartMode::carry) {
        next.resize(workspace.base_primes.size());
        for (std::size_t i = 0; i < workspace.base_primes.size(); i++) {
            u64 p = workspace.base_primes[i];
            next[i] = p * p;
        }
    }

    std::vector<std::uint8_t> &composite = workspace.segment;
    composite.resize(segment_size);
    u64 answer = 1; // prime 2
    u64 limit = u64(n) + 1;

    for (u64 low = 3; low < limit; low += u64(2) * segment_size) {
        u64 high = std::min(limit, low + u64(2) * segment_size);
        std::size_t slots = std::size_t((high - low + 1) / 2);

        if constexpr (Profile)
            phase_start = Clock::now();
        std::fill(composite.begin(), composite.begin() + slots, 0);
        if constexpr (Profile) {
            auto stop = Clock::now();
            stats->clear_ms +=
                std::chrono::duration<double, std::milli>(stop - phase_start).count();
            phase_start = stop;
            stats->segments++;
        }

        for (std::size_t i = 0; i < workspace.base_primes.size(); i++) {
            u64 p = workspace.base_primes[i];
            if (p * p >= high)
                break;

            u64 first;
            if (mode == StartMode::divide) {
                first = std::max(p * p, (low + p - 1) / p * p);
                if ((first & 1) == 0)
                    first += p;
            } else {
                first = next[i];
                if (first < low) {
                    u64 steps = (low - first + 2 * p - 1) / (2 * p);
                    first += steps * 2 * p;
                }
            }

            u64 multiple = first;
            for (; multiple < high; multiple += 2 * p) {
                composite[std::size_t((multiple - low) / 2)] = 1;
            }
            if (mode == StartMode::carry)
                next[i] = multiple;
        }

        if constexpr (Profile) {
            auto stop = Clock::now();
            stats->mark_ms +=
                std::chrono::duration<double, std::milli>(stop - phase_start).count();
            phase_start = stop;
        }
        for (std::size_t i = 0; i < slots; i++)
            answer += !composite[i];
        if constexpr (Profile) {
            auto stop = Clock::now();
            stats->count_ms +=
                std::chrono::duration<double, std::milli>(stop - phase_start).count();
        }
    }
    return answer;
}

struct ActivityStats {
    u64 prime_visits = 0;
    u64 start_divisions = 0;
    u64 stores = 0;
};

ActivityStats count_activity(u32 n, std::size_t segment_size,
                             Workspace &workspace) {
    make_base_primes(integer_sqrt(n), workspace);
    ActivityStats stats;
    u64 limit = u64(n) + 1;
    for (u64 low = 3; low < limit; low += u64(2) * segment_size) {
        u64 high = std::min(limit, low + u64(2) * segment_size);
        for (u32 p32 : workspace.base_primes) {
            u64 p = p32;
            if (p * p >= high)
                break;
            u64 first = std::max(p * p, (low + p - 1) / p * p);
            if ((first & 1) == 0)
                first += p;
            stats.prime_visits++;
            stats.start_divisions++;
            if (first < high)
                stats.stores += (high - 1 - first) / (2 * p) + 1;
        }
    }
    return stats;
}

enum class Algorithm { full, odd, segmented_divide, segmented_carry };

const char *algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::full: return "full_bytes";
        case Algorithm::odd: return "odd_bytes";
        case Algorithm::segmented_divide: return "segmented_divide";
        case Algorithm::segmented_carry: return "segmented_carry";
    }
    return "unknown";
}

u64 run_algorithm(Algorithm algorithm, u32 n, std::size_t segment_size,
                  Workspace &workspace) {
    switch (algorithm) {
        case Algorithm::full:
            return count_full(n, workspace);
        case Algorithm::odd:
            return count_odd(n, workspace);
        case Algorithm::segmented_divide:
            return count_segmented<false>(n, segment_size, StartMode::divide,
                                          workspace, nullptr);
        case Algorithm::segmented_carry:
            return count_segmented<false>(n, segment_size, StartMode::carry,
                                          workspace, nullptr);
    }
    return 0;
}

void test_n(u32 n) {
    Workspace workspace;
    workspace.reserve(n, 4096);
    u64 expected = count_full(n, workspace);
    require(count_odd(n, workspace) == expected, "odd sieve disagrees with full sieve");
    for (std::size_t segment_size : {std::size_t(1), std::size_t(2),
                                     std::size_t(3), std::size_t(7),
                                     std::size_t(31), std::size_t(256),
                                     std::size_t(4096)}) {
        require(count_segmented<false>(n, segment_size, StartMode::divide,
                                       workspace, nullptr) == expected,
                "division segmented sieve disagrees with full sieve");
        require(count_segmented<false>(n, segment_size, StartMode::carry,
                                       workspace, nullptr) == expected,
                "carried-offset segmented sieve disagrees with full sieve");
    }
}

void run_tests() {
    for (auto [n, expected] : {std::pair<u32, u64>{0, 0}, {1, 0}, {2, 1},
                               {3, 2}, {10, 4}, {100, 25}, {1000, 168},
                               {1000000, 78498}}) {
        Workspace workspace;
        workspace.reserve(n, 4096);
        require(count_full(n, workspace) == expected, "known prime count failed");
    }

    for (u32 n = 0; n <= 200; n++)
        test_n(n);
    std::mt19937 rng(0x7d413f2b);
    for (int test = 0; test < 200; test++)
        test_n(rng() % 200000);
    std::puts("all prime-sieve tests passed");
}

volatile u64 benchmark_sink = 0;

double median(std::vector<double> sample) {
    std::sort(sample.begin(), sample.end());
    return sample[sample.size() / 2];
}

double benchmark(Algorithm algorithm, u32 n, std::size_t segment_size,
                 int warmups, int repeats) {
    Workspace workspace;
    workspace.reserve(n, segment_size);
    std::vector<double> samples;
    for (int iteration = -warmups; iteration < repeats; iteration++) {
        auto start = Clock::now();
        u64 result = run_algorithm(algorithm, n, segment_size, workspace);
        auto stop = Clock::now();
        benchmark_sink ^= result;
        if (iteration >= 0)
            samples.push_back(
                std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(samples);
}

PhaseStats profile_segmented(u32 n, std::size_t segment_size,
                             int warmups, int repeats) {
    Workspace workspace;
    workspace.reserve(n, segment_size);
    std::vector<double> base_samples;
    std::vector<double> clear_samples;
    std::vector<double> mark_samples;
    std::vector<double> count_samples;
    u64 segments = 0;

    for (int iteration = -warmups; iteration < repeats; iteration++) {
        PhaseStats stats;
        u64 result = count_segmented<true>(n, segment_size, StartMode::carry,
                                           workspace, &stats);
        benchmark_sink ^= result;
        if (iteration >= 0) {
            base_samples.push_back(stats.base_ms);
            clear_samples.push_back(stats.clear_ms);
            mark_samples.push_back(stats.mark_ms);
            count_samples.push_back(stats.count_ms);
            segments = stats.segments;
        }
    }

    return {median(base_samples), median(clear_samples), median(mark_samples),
            median(count_samples), segments};
}

void run_benchmarks() {
    constexpr u32 n = 1u << 27;
    constexpr std::size_t segment_size = std::size_t(1) << 17;
    std::puts("median prime-count time; all marker initialization is included");
    for (Algorithm algorithm : {Algorithm::full, Algorithm::odd,
                                Algorithm::segmented_divide,
                                Algorithm::segmented_carry}) {
        double ms = benchmark(algorithm, n, segment_size, 2, 7);
        std::printf("%-20s n=%u segment=%zu %9.3f ms\n",
                    algorithm_name(algorithm), n, segment_size, ms);
    }

    PhaseStats stats = profile_segmented(n, segment_size, 2, 7);
    Workspace workspace;
    workspace.reserve(n, segment_size);
    ActivityStats activity = count_activity(n, segment_size, workspace);
    std::printf("profile segmented_carry: base=%.3f clear=%.3f mark=%.3f "
                "count=%.3f ms, segments=%llu prime-visits=%llu stores=%llu\n",
                stats.base_ms, stats.clear_ms, stats.mark_ms, stats.count_ms,
                (unsigned long long) stats.segments,
                (unsigned long long) activity.prime_visits,
                (unsigned long long) activity.stores);
    std::printf("division-start variant executes %llu start divisions\n",
                (unsigned long long) activity.start_divisions);
    std::printf("benchmark sink: %llu\n", (unsigned long long) benchmark_sink);
}

void print_csv() {
    std::puts("kind,n,algorithm,segment,milliseconds,base_ms,clear_ms,mark_ms,"
              "count_ms,segments,prime_visits,division_mode_start_divisions,"
              "stores");
    constexpr std::size_t segment_size = std::size_t(1) << 17;
    for (int logn = 16; logn <= 27; logn++) {
        u32 n = u32(1) << logn;
        for (Algorithm algorithm : {Algorithm::full, Algorithm::odd,
                                    Algorithm::segmented_divide,
                                    Algorithm::segmented_carry}) {
            double ms = benchmark(algorithm, n, segment_size, 1, 5);
            std::printf("size,%u,%s,%zu,%.6f,,,,,,,,\n", n,
                        algorithm_name(algorithm), segment_size, ms);
        }
    }

    constexpr u32 n = 1u << 27;
    for (int logs = 10; logs <= 24; logs++) {
        std::size_t size = std::size_t(1) << logs;
        double ms = benchmark(Algorithm::segmented_carry, n, size, 1, 5);
        std::printf("segment,%u,segmented_carry,%zu,%.6f,,,,,,,,\n",
                    n, size, ms);
    }

    PhaseStats stats = profile_segmented(n, segment_size, 2, 7);
    Workspace workspace;
    workspace.reserve(n, segment_size);
    ActivityStats activity = count_activity(n, segment_size, workspace);
    std::printf("profile,%u,segmented_carry,%zu,,%.6f,%.6f,%.6f,%.6f,"
                "%llu,%llu,%llu,%llu\n",
                n, segment_size, stats.base_ms, stats.clear_ms, stats.mark_ms,
                stats.count_ms, (unsigned long long) stats.segments,
                (unsigned long long) activity.prime_visits,
                (unsigned long long) activity.start_divisions,
                (unsigned long long) activity.stores);
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
