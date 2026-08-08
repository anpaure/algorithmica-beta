#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;
static volatile u64 benchmark_sink;

static u64 mix64(u64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct IndependentBloom {
    static constexpr int probes = 7;
    std::vector<u64> words;
    std::size_t mask;
    explicit IndependentBloom(std::size_t bit_count)
        : words(bit_count / 64), mask(bit_count - 1) {
        assert(bit_count >= 64 && (bit_count & (bit_count - 1)) == 0);
    }
    void add(u64 x) {
        for (int i = 0; i < probes; ++i) {
            u64 h = mix64(x + 0x9e3779b97f4a7c15ULL * static_cast<u64>(i));
            std::size_t p = h & mask;
            words[p >> 6] |= 1ULL << (p & 63);
        }
    }
    bool maybe_contains(u64 x) const {
        for (int i = 0; i < probes; ++i) {
            u64 h = mix64(x + 0x9e3779b97f4a7c15ULL * static_cast<u64>(i));
            std::size_t p = h & mask;
            if ((words[p >> 6] >> (p & 63) & 1) == 0)
                return false;
        }
        return true;
    }
    std::size_t bytes() const { return words.size() * sizeof(u64); }
};

struct DoubleBloom {
    static constexpr int probes = 7;
    std::vector<u64> words;
    std::size_t mask;
    explicit DoubleBloom(std::size_t bit_count)
        : words(bit_count / 64), mask(bit_count - 1) {
        assert(bit_count >= 64 && (bit_count & (bit_count - 1)) == 0);
    }
    void add(u64 x) {
        u64 h = mix64(x), step = mix64(h) | 1;
        for (int i = 0; i < probes; ++i, h += step) {
            std::size_t p = h & mask;
            words[p >> 6] |= 1ULL << (p & 63);
        }
    }
    bool maybe_contains(u64 x) const {
        u64 h = mix64(x), step = mix64(h) | 1;
        for (int i = 0; i < probes; ++i, h += step) {
            std::size_t p = h & mask;
            if ((words[p >> 6] >> (p & 63) & 1) == 0)
                return false;
        }
        return true;
    }
    std::size_t bytes() const { return words.size() * sizeof(u64); }
};

struct BlockedBloom {
    static constexpr int probes = 7;
    // One M4 cache line: 128 bytes = 16 words = 1024 bits.
    static constexpr std::size_t block_bits = 1024;
    struct Free { void operator()(u64 *p) const { std::free(p); } };
    std::unique_ptr<u64[], Free> words;
    std::size_t word_count;
    std::size_t block_mask;
    explicit BlockedBloom(std::size_t bit_count)
        : word_count(bit_count / 64), block_mask(bit_count / block_bits - 1) {
        assert(bit_count >= block_bits && (bit_count & (bit_count - 1)) == 0);
        std::size_t bytes = word_count * sizeof(u64);
        auto *p = static_cast<u64 *>(std::aligned_alloc(128, bytes));
        if (!p) throw std::bad_alloc();
        assert(reinterpret_cast<std::uintptr_t>(p) % 128 == 0);
        std::memset(p, 0, bytes);
        words.reset(p);
    }
    void add(u64 x) {
        u64 h = mix64(x), step = mix64(h) | 1;
        // Use disjoint hash bits for the block and for positions inside it.
        std::size_t block = ((h >> 10) & block_mask) * (block_bits / 64);
        for (int i = 0; i < probes; ++i, h += step) {
            std::size_t p = h & (block_bits - 1);
            words[block + (p >> 6)] |= 1ULL << (p & 63);
        }
    }
    bool maybe_contains(u64 x) const {
        u64 h = mix64(x), step = mix64(h) | 1;
        std::size_t block = ((h >> 10) & block_mask) * (block_bits / 64);
        for (int i = 0; i < probes; ++i, h += step) {
            std::size_t p = h & (block_bits - 1);
            if ((words[block + (p >> 6)] >> (p & 63) & 1) == 0)
                return false;
        }
        return true;
    }
    std::size_t bytes() const { return word_count * sizeof(u64); }
};

template<class Filter>
static void populate(Filter &filter, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        filter.add(mix64(i + 1));
}

template<class Filter>
static double false_positive_rate(const Filter &filter, std::size_t trials) {
    std::size_t positives = 0;
    for (std::size_t i = 0; i < trials; ++i)
        positives += filter.maybe_contains(mix64(0x8000000000000000ULL + i));
    return double(positives) / trials;
}

template<class Filter>
static double throughput_ns(const Filter &filter, const std::vector<u64> &queries,
                            int repeats = 5) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        u64 checksum = 0;
        auto begin = Clock::now();
        for (u64 x : queries)
            checksum += filter.maybe_contains(x);
        auto end = Clock::now();
        benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / queries.size());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Filter>
static double serialized_chain_ns(const Filter &filter, std::size_t steps,
                                  int repeats = 5) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        u64 x = 0x123456789abcdef0ULL;
        auto begin = Clock::now();
        for (std::size_t i = 0; i < steps; ++i)
            x = mix64(x + static_cast<u64>(filter.maybe_contains(x)));
        auto end = Clock::now();
        benchmark_sink = x;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / steps);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Filter>
static void emit(const char *phase, const char *name, std::size_t bits,
                 std::size_t inserted, const std::vector<u64> &queries) {
    Filter filter(bits);
    populate(filter, inserted);
    double fpr = false_positive_rate(filter, 300000);
    std::printf("%s,%s,%zu,%zu,%.3f,%.6f,%.3f,%.3f\n", phase, name, bits,
                inserted, double(bits) / inserted, fpr,
                throughput_ns(filter, queries), serialized_chain_ns(filter, 200000));
}

static std::vector<u64> make_queries(std::size_t inserted, std::size_t count) {
    std::vector<u64> queries(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 10 == 0)
            queries[i] = mix64(i % inserted + 1); // 10% present
        else
            queries[i] = mix64(0x8000000000000000ULL + i);
    }
    return queries;
}

static void run_tests() {
    for (std::size_t bits : {std::size_t(1024), std::size_t(1 << 17)}) {
        std::size_t n = std::max<std::size_t>(1, bits / 10);
        IndependentBloom independent(bits);
        DoubleBloom doubled(bits);
        BlockedBloom blocked(bits);
        populate(independent, n); populate(doubled, n); populate(blocked, n);
        for (std::size_t i = 0; i < n; ++i) {
            u64 x = mix64(i + 1);
            assert(independent.maybe_contains(x));
            assert(doubled.maybe_contains(x));
            assert(blocked.maybe_contains(x));
        }
    }
    std::puts("filter tests passed");
}

static void run_benchmarks() {
    std::puts("phase,implementation,bits,inserted,bits_per_key,false_positive_rate,throughput_ns,serialized_chain_ns");
    {
        std::size_t bits = std::size_t(1) << 24;
        for (int b : {6, 8, 10, 12, 16}) {
            std::size_t inserted = bits / static_cast<unsigned>(b);
            auto q = make_queries(inserted, 300000);
            emit<IndependentBloom>("density", "independent", bits, inserted, q);
            emit<DoubleBloom>("density", "double", bits, inserted, q);
            emit<BlockedBloom>("density", "blocked", bits, inserted, q);
        }
    }
    for (int log_bits : {17, 20, 23, 26, 27}) {
        std::size_t bits = std::size_t(1) << log_bits;
        std::size_t inserted = bits / 10;
        auto q = make_queries(inserted, 300000);
        emit<IndependentBloom>("size", "independent", bits, inserted, q);
        emit<DoubleBloom>("size", "double", bits, inserted, q);
        emit<BlockedBloom>("size", "blocked", bits, inserted, q);
    }
}

int main(int argc, char **argv) {
    if (argc != 2 || (std::string(argv[1]) != "--test" && std::string(argv[1]) != "--bench")) {
        std::fprintf(stderr, "usage: %s --test|--bench\n", argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--test") run_tests();
    else run_benchmarks();
}
