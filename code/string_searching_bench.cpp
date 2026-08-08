#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Stats {
    std::uint64_t candidates = 0;
    std::uint64_t fallbacks = 0;
};

std::vector<std::size_t> prefix_function(const std::uint8_t *pattern,
                                         std::size_t length) {
    std::vector<std::size_t> prefix(length);
    for (std::size_t i = 1; i < length; ++i) {
        std::size_t j = prefix[i - 1];
        while (j != 0 && pattern[i] != pattern[j])
            j = prefix[j - 1];
        if (pattern[i] == pattern[j])
            ++j;
        prefix[i] = j;
    }
    return prefix;
}

int kmp_from(const std::uint8_t *text, std::size_t length,
             const std::uint8_t *pattern, std::size_t pattern_length,
             const std::vector<std::size_t> &prefix, std::size_t start) {
    if (pattern_length == 0)
        return static_cast<int>(start);
    std::size_t matched = 0;
    for (std::size_t i = start; i < length; ++i) {
        while (matched != 0 && text[i] != pattern[matched])
            matched = prefix[matched - 1];
        if (text[i] == pattern[matched])
            ++matched;
        if (matched == pattern_length)
            return static_cast<int>(i + 1 - pattern_length);
    }
    return -1;
}

int naive_find(const std::uint8_t *text, std::size_t length,
               const std::uint8_t *pattern, std::size_t pattern_length,
               const std::vector<std::size_t> &, Stats *stats) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    std::size_t candidates = length - pattern_length + 1;
    for (std::size_t i = 0; i < candidates; ++i) {
        if (stats != nullptr) ++stats->candidates;
        if (std::memcmp(text + i, pattern, pattern_length) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

#if defined(__aarch64__)
unsigned neon_movemask(uint8x16_t bytes) {
    static const std::uint8_t bit_values[16] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128,
    };
    uint8x16_t weights = vld1q_u8(bit_values);
    uint8x16_t selected = vandq_u8(bytes, weights);
    uint16x8_t sums16 = vpaddlq_u8(selected);
    uint32x4_t sums32 = vpaddlq_u16(sums16);
    uint64x2_t sums64 = vpaddlq_u32(sums32);
    return static_cast<unsigned>(vgetq_lane_u64(sums64, 0)) |
           (static_cast<unsigned>(vgetq_lane_u64(sums64, 1)) << 8U);
}
#endif

unsigned equal_mask_16(const std::uint8_t *text, std::uint8_t value) {
#if defined(__aarch64__)
    uint8x16_t bytes = vld1q_u8(text);
    return neon_movemask(vceqq_u8(bytes, vdupq_n_u8(value)));
#else
    unsigned mask = 0;
    for (unsigned i = 0; i < 16; ++i)
        mask |= static_cast<unsigned>(text[i] == value) << i;
    return mask;
#endif
}

struct FilterPlan {
    std::size_t first = 0;
    std::size_t second = 0;
    bool repetitive = false;
};

FilterPlan choose_filter(const std::uint8_t *pattern, std::size_t length) {
    std::array<unsigned, 256> frequency{};
    for (std::size_t i = 0; i < length; ++i)
        ++frequency[pattern[i]];
    FilterPlan plan;
    if (length < 2) return plan;
    plan.first = 0;
    for (std::size_t i = 1; i < length; ++i)
        if (frequency[pattern[i]] < frequency[pattern[plan.first]])
            plan.first = i;
    plan.second = plan.first == 0 ? 1 : 0;
    for (std::size_t i = 0; i < length; ++i)
        if (i != plan.first &&
            frequency[pattern[i]] < frequency[pattern[plan.second]])
            plan.second = i;
    unsigned most_common = *std::max_element(frequency.begin(), frequency.end());
    plan.repetitive = 4U * most_common >= 3U * length;
    return plan;
}

int first_byte_find(const std::uint8_t *text, std::size_t length,
                    const std::uint8_t *pattern, std::size_t pattern_length,
                    const std::vector<std::size_t> &, Stats *stats) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    const std::size_t candidates = length - pattern_length + 1;
    std::size_t i = 0;
    for (; i + 16 <= candidates; i += 16) {
        unsigned mask = equal_mask_16(text + i, pattern[0]);
        while (mask != 0U) {
            unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i + bit, pattern, pattern_length) == 0)
                return static_cast<int>(i + bit);
            mask &= mask - 1U;
        }
    }
    for (; i < candidates; ++i) {
        if (text[i] == pattern[0]) {
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i, pattern, pattern_length) == 0)
                return static_cast<int>(i);
        }
    }
    return -1;
}

int two_byte_find(const std::uint8_t *text, std::size_t length,
                  const std::uint8_t *pattern, std::size_t pattern_length,
                  const std::vector<std::size_t> &, Stats *stats) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    if (pattern_length == 1) {
        const void *found = std::memchr(text, pattern[0], length);
        return found == nullptr
            ? -1
            : static_cast<int>(static_cast<const std::uint8_t *>(found) - text);
    }
    const std::size_t candidates = length - pattern_length + 1;
    std::size_t i = 0;
    for (; i + 16 <= candidates; i += 16) {
        unsigned first = equal_mask_16(text + i, pattern[0]);
        unsigned last = equal_mask_16(text + i + pattern_length - 1,
                                      pattern[pattern_length - 1]);
        unsigned mask = first & last;
        while (mask != 0U) {
            unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i + bit, pattern, pattern_length) == 0)
                return static_cast<int>(i + bit);
            mask &= mask - 1U;
        }
    }
    for (; i < candidates; ++i) {
        if (text[i] == pattern[0] &&
            text[i + pattern_length - 1] == pattern[pattern_length - 1]) {
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i, pattern, pattern_length) == 0)
                return static_cast<int>(i);
        }
    }
    return -1;
}

int rare_byte_find(const std::uint8_t *text, std::size_t length,
                   const std::uint8_t *pattern, std::size_t pattern_length,
                   const std::vector<std::size_t> &, Stats *stats) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    if (pattern_length == 1) {
        const void *found = std::memchr(text, pattern[0], length);
        return found == nullptr
            ? -1
            : static_cast<int>(static_cast<const std::uint8_t *>(found) - text);
    }
    FilterPlan plan = choose_filter(pattern, pattern_length);
    const std::size_t candidates = length - pattern_length + 1;
    std::size_t i = 0;
    for (; i + 16 <= candidates; i += 16) {
        unsigned first = equal_mask_16(text + i + plan.first,
                                       pattern[plan.first]);
        unsigned second = equal_mask_16(text + i + plan.second,
                                        pattern[plan.second]);
        unsigned mask = first & second;
        while (mask != 0U) {
            unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i + bit, pattern, pattern_length) == 0)
                return static_cast<int>(i + bit);
            mask &= mask - 1U;
        }
    }
    for (; i < candidates; ++i) {
        if (text[i + plan.first] == pattern[plan.first] &&
            text[i + plan.second] == pattern[plan.second]) {
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i, pattern, pattern_length) == 0)
                return static_cast<int>(i);
        }
    }
    return -1;
}

int adaptive_find(const std::uint8_t *text, std::size_t length,
                  const std::uint8_t *pattern, std::size_t pattern_length,
                  const std::vector<std::size_t> &prefix, Stats *stats) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    if (pattern_length == 1) {
        const void *found = std::memchr(text, pattern[0], length);
        return found == nullptr
            ? -1
            : static_cast<int>(static_cast<const std::uint8_t *>(found) - text);
    }
    constexpr std::uint64_t max_failed_candidates = 32;
    std::uint64_t failed = 0;
    FilterPlan plan = choose_filter(pattern, pattern_length);
    const std::size_t candidates = length - pattern_length + 1;
    std::size_t i = 0;
    for (; i + 16 <= candidates; i += 16) {
        unsigned first = equal_mask_16(text + i + plan.first,
                                       pattern[plan.first]);
        unsigned second = equal_mask_16(text + i + plan.second,
                                        pattern[plan.second]);
        unsigned mask = first & second;
        while (mask != 0U) {
            unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i + bit, pattern, pattern_length) == 0)
                return static_cast<int>(i + bit);
            if (++failed == max_failed_candidates && plan.repetitive) {
                if (stats != nullptr) ++stats->fallbacks;
                return kmp_from(text, length, pattern, pattern_length,
                                prefix, i + bit + 1U);
            }
            mask &= mask - 1U;
        }
    }
    for (; i < candidates; ++i) {
        if (text[i + plan.first] == pattern[plan.first] &&
            text[i + plan.second] == pattern[plan.second]) {
            if (stats != nullptr) ++stats->candidates;
            if (std::memcmp(text + i, pattern, pattern_length) == 0)
                return static_cast<int>(i);
            if (++failed == max_failed_candidates && plan.repetitive) {
                if (stats != nullptr) ++stats->fallbacks;
                return kmp_from(text, length, pattern, pattern_length,
                                prefix, i + 1U);
            }
        }
    }
    return -1;
}

int kmp_find(const std::uint8_t *text, std::size_t length,
             const std::uint8_t *pattern, std::size_t pattern_length,
             const std::vector<std::size_t> &prefix, Stats *) {
    return kmp_from(text, length, pattern, pattern_length, prefix, 0);
}

int standard_find(const std::uint8_t *text, std::size_t length,
                  const std::uint8_t *pattern, std::size_t pattern_length,
                  const std::vector<std::size_t> &, Stats *) {
    if (pattern_length == 0) return 0;
    if (pattern_length > length) return -1;
    const std::uint8_t *result = std::search(text, text + length,
                                             pattern, pattern + pattern_length);
    return result == text + length ? -1 : static_cast<int>(result - text);
}

using Finder = int (*)(const std::uint8_t *, std::size_t,
                       const std::uint8_t *, std::size_t,
                       const std::vector<std::size_t> &, Stats *);

struct Variant {
    std::string_view name;
    Finder finder;
};

#if defined(__aarch64__)
constexpr std::array<Variant, 7> variants{{
    {"naive", naive_find},
    {"first-byte-neon", first_byte_find},
    {"two-byte-neon", two_byte_find},
    {"rare-byte-neon", rare_byte_find},
    {"adaptive-kmp", adaptive_find},
    {"kmp", kmp_find},
    {"std-search", standard_find},
}};
#else
constexpr std::array<Variant, 7> variants{{
    {"naive", naive_find},
    {"first-byte-scalar16", first_byte_find},
    {"two-byte-scalar16", two_byte_find},
    {"rare-byte-scalar16", rare_byte_find},
    {"adaptive-kmp-scalar16", adaptive_find},
    {"kmp", kmp_find},
    {"std-search", standard_find},
}};
#endif

int reference_find(const std::vector<std::uint8_t> &text,
                   const std::vector<std::uint8_t> &pattern) {
    if (pattern.empty()) return 0;
    auto result = std::search(text.begin(), text.end(), pattern.begin(), pattern.end());
    return result == text.end() ? -1 : static_cast<int>(result - text.begin());
}

bool verify_case(const std::vector<std::uint8_t> &text,
                 const std::vector<std::uint8_t> &pattern) {
    int expected = reference_find(text, pattern);
    std::vector<std::size_t> prefix = prefix_function(pattern.data(), pattern.size());
    for (const Variant &variant : variants) {
        int actual = variant.finder(text.data(), text.size(), pattern.data(),
                                    pattern.size(), prefix, nullptr);
        if (actual != expected) {
            std::cerr << "mismatch," << variant.name << ',' << text.size()
                      << ',' << pattern.size() << ',' << expected << ',' << actual << '\n';
            return false;
        }
    }
    return true;
}

bool test() {
    for (unsigned n = 0; n <= 8; ++n) {
        for (unsigned text_bits = 0; text_bits < (1U << n); ++text_bits) {
            std::vector<std::uint8_t> text(n);
            for (unsigned i = 0; i < n; ++i)
                text[i] = static_cast<std::uint8_t>('a' + ((text_bits >> i) & 1U));
            for (unsigned m = 0; m <= 5; ++m) {
                for (unsigned pattern_bits = 0; pattern_bits < (1U << m); ++pattern_bits) {
                    std::vector<std::uint8_t> pattern(m);
                    for (unsigned i = 0; i < m; ++i)
                        pattern[i] = static_cast<std::uint8_t>(
                            'a' + ((pattern_bits >> i) & 1U));
                    if (!verify_case(text, pattern)) return false;
                }
            }
        }
    }
    std::mt19937 rng(0x7319a55dU);
    for (int trial = 0; trial < 100'000; ++trial) {
        std::size_t n = rng() % 257U;
        std::size_t m = rng() % 65U;
        std::vector<std::uint8_t> text(n), pattern(m);
        for (std::uint8_t &c : text) c = static_cast<std::uint8_t>('a' + rng() % 4U);
        for (std::uint8_t &c : pattern) c = static_cast<std::uint8_t>('a' + rng() % 4U);
        if (!verify_case(text, pattern)) return false;
    }
    std::cout << "tests,ok,exhaustive-binary-plus-100000-random\n";
    return true;
}

struct Workload {
    std::string name;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> pattern;
};

Workload random_absent(std::size_t length, std::size_t pattern_length,
                       std::uint32_t seed) {
    std::mt19937 rng(seed);
    Workload workload{"random-absent", std::vector<std::uint8_t>(length),
                      std::vector<std::uint8_t>(pattern_length)};
    for (std::uint8_t &c : workload.text) c = static_cast<std::uint8_t>(rng());
    do {
        for (std::uint8_t &c : workload.pattern) c = static_cast<std::uint8_t>(rng());
    } while (reference_find(workload.text, workload.pattern) != -1);
    return workload;
}

Workload alphabet4_absent(std::size_t length) {
    std::mt19937 rng(0xa1944U);
    Workload workload{"alphabet4-absent", std::vector<std::uint8_t>(length),
                      std::vector<std::uint8_t>(32)};
    for (std::uint8_t &c : workload.text) c = static_cast<std::uint8_t>('a' + rng() % 4U);
    do {
        for (std::uint8_t &c : workload.pattern)
            c = static_cast<std::uint8_t>('a' + rng() % 4U);
    } while (reference_find(workload.text, workload.pattern) != -1);
    return workload;
}

Workload match_at_end(std::size_t length) {
    Workload workload = random_absent(length, 32, 0xe11d5eedU);
    workload.name = "match-at-end";
    std::copy(workload.text.end() - 32, workload.text.end(), workload.pattern.begin());
    return workload;
}

Workload periodic_miss(std::size_t length) {
    Workload workload{"periodic-middle-miss",
                      std::vector<std::uint8_t>(length, 'a'),
                      std::vector<std::uint8_t>(129, 'a')};
    workload.pattern[64] = 'b';
    return workload;
}

Workload repetitive_window_miss(std::size_t length) {
    Workload workload{"repetitive-window-miss",
                      std::vector<std::uint8_t>(length),
                      std::vector<std::uint8_t>(129, 'a')};
    for (std::size_t i = 0; i < length; ++i)
        workload.text[i] = (i % 129U == 128U) ? 'b' : 'a';
    return workload;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

double measure(const Workload &workload, Finder finder,
               const std::vector<std::size_t> &prefix,
               std::size_t target_bytes, std::uint64_t &checksum) {
    std::size_t repeats = std::max<std::size_t>(1, target_bytes / workload.text.size());
    std::vector<double> samples;
    samples.reserve(9);
    for (int sample = -2; sample < 9; ++sample) {
        auto start = Clock::now();
        int result = -1;
        for (std::size_t repeat = 0; repeat < repeats; ++repeat)
            result ^= finder(workload.text.data(), workload.text.size(),
                             workload.pattern.data(), workload.pattern.size(),
                             prefix, nullptr);
        auto stop = Clock::now();
        checksum += static_cast<std::uint64_t>(result + 2);
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns /
                (static_cast<double>(workload.text.size()) * static_cast<double>(repeats)));
        }
    }
    return median(samples);
}

void print_measurement(std::string_view kind, const Workload &workload,
                       const Variant &variant, std::size_t target_bytes,
                       std::uint64_t &checksum) {
    std::vector<std::size_t> prefix = prefix_function(
        workload.pattern.data(), workload.pattern.size());
    Stats stats;
    int result = variant.finder(workload.text.data(), workload.text.size(),
                                workload.pattern.data(), workload.pattern.size(),
                                prefix, &stats);
    checksum += static_cast<std::uint64_t>(result + 2);
    double ns = measure(workload, variant.finder, prefix, target_bytes, checksum);
    double candidates = 1024.0 * static_cast<double>(stats.candidates) /
                        static_cast<double>(workload.text.size());
    std::cout << kind << ',' << workload.name << ',' << workload.text.size()
              << ',' << workload.pattern.size() << ',' << variant.name << ','
              << ns << ',' << candidates << ',' << stats.fallbacks << '\n';
}

void bench() {
    constexpr std::size_t mib = 1U << 20;
    std::vector<Workload> workloads;
    workloads.push_back(random_absent(mib, 16, 0x91827364U));
    workloads.push_back(alphabet4_absent(mib));
    workloads.push_back(match_at_end(mib));
    workloads.push_back(periodic_miss(1U << 18));
    workloads.push_back(repetitive_window_miss(1U << 18));

    std::uint64_t checksum = 0;
    std::cout << "kind,suite,size,pattern,variant,ns_per_byte,candidates_per_kib,fallbacks\n";
    for (const Workload &workload : workloads)
        for (const Variant &variant : variants)
            print_measurement("workload", workload, variant, 8U * mib, checksum);

    Workload large = random_absent(1U << 26, 16, 0x55aacc33U);
    constexpr std::array<std::size_t, 9> sizes{{
        1U << 10, 1U << 12, 1U << 14, 1U << 17, 1U << 20,
        1U << 22, 1U << 24, 1U << 25, 1U << 26,
    }};
    for (std::size_t size : sizes) {
        Workload view{"size-sweep",
                      std::vector<std::uint8_t>(large.text.begin(), large.text.begin() + size),
                      large.pattern};
        for (const Variant &variant : {variants[0], variants[2]})
            print_measurement("size", view, variant, 32U * mib, checksum);
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
