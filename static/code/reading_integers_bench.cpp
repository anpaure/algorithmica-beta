#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Dataset {
    std::string name;
    std::vector<char> bytes;
    std::size_t count = 0;
    std::array<std::size_t, 5> cuts4{};
};

void make_cuts(Dataset &data) {
    data.cuts4[0] = 0;
    data.cuts4[4] = data.bytes.size();
    for (std::size_t part = 1; part < 4; ++part) {
        std::size_t cut = data.bytes.size() * part / 4U;
        while (cut < data.bytes.size()
               && (cut == 0 || data.bytes[cut - 1] != '\n'))
            ++cut;
        data.cuts4[part] = cut;
    }
}

void append_number(std::vector<char> &bytes, std::uint32_t value) {
    char buffer[10];
    auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    bytes.insert(bytes.end(), buffer, result.ptr);
    bytes.push_back('\n');
}

Dataset make_uniform_numeric(std::size_t count, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::uint32_t> dist(0U, 99'999'999U);
    Dataset data{"uniform-numeric", {}, count, {}};
    data.bytes.reserve(count * 9U);
    for (std::size_t i = 0; i < count; ++i)
        append_number(data.bytes, dist(rng));
    make_cuts(data);
    return data;
}

Dataset make_fixed8(std::size_t count, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::uint32_t> dist(10'000'000U, 99'999'999U);
    Dataset data{"fixed-8-digit", {}, count, {}};
    data.bytes.reserve(count * 9U);
    for (std::size_t i = 0; i < count; ++i)
        append_number(data.bytes, dist(rng));
    make_cuts(data);
    return data;
}

Dataset make_uniform_digits(std::size_t count, std::uint32_t seed) {
    constexpr std::array<std::uint32_t, 8> lo{{
        0U, 10U, 100U, 1'000U, 10'000U, 100'000U, 1'000'000U, 10'000'000U,
    }};
    constexpr std::array<std::uint32_t, 8> hi{{
        9U, 99U, 999U, 9'999U, 99'999U, 999'999U, 9'999'999U, 99'999'999U,
    }};
    std::mt19937 rng(seed);
    Dataset data{"uniform-digits", {}, count, {}};
    data.bytes.reserve(count * 6U);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t digits = i % 8U;
        std::uniform_int_distribution<std::uint32_t> dist(lo[digits], hi[digits]);
        append_number(data.bytes, dist(rng));
    }
    make_cuts(data);
    return data;
}

inline void scalar_update(unsigned char c, std::uint32_t &value,
                          std::uint32_t &checksum) {
    if (c == '\n') {
        checksum ^= value;
        value = 0;
    } else {
        value = 10U * value + static_cast<std::uint32_t>(c - '0');
    }
}

std::uint32_t scalar_horner(const Dataset &data) {
    std::uint32_t value = 0;
    std::uint32_t checksum = 0;
    for (unsigned char c : data.bytes)
        scalar_update(c, value, checksum);
    return checksum;
}

std::uint32_t standard_from_chars(const Dataset &data) {
    if (data.bytes.empty()) return 0;
    const char *p = data.bytes.data();
    const char *end = p + data.bytes.size();
    std::uint32_t checksum = 0;
    while (p != end) {
        std::uint32_t value = 0;
        auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc() || result.ptr == end || *result.ptr != '\n')
            return std::numeric_limits<std::uint32_t>::max();
        checksum ^= value;
        p = result.ptr + 1;
    }
    return checksum;
}

#if defined(__aarch64__)
unsigned neon_movemask(uint8x16_t bytes) {
    static const std::uint8_t bit_values[16] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128,
    };
    uint8x16_t selected = vandq_u8(bytes, vld1q_u8(bit_values));
    uint16x8_t sums16 = vpaddlq_u8(selected);
    uint32x4_t sums32 = vpaddlq_u16(sums16);
    uint64x2_t sums64 = vpaddlq_u32(sums32);
    return static_cast<unsigned>(vgetq_lane_u64(sums64, 0)) |
           (static_cast<unsigned>(vgetq_lane_u64(sums64, 1)) << 8U);
}

const std::array<std::array<std::uint8_t, 16>, 9> &alignment_tables() {
    static const std::array<std::array<std::uint8_t, 16>, 9> tables = [] {
        std::array<std::array<std::uint8_t, 16>, 9> result{};
        for (std::size_t length = 0; length <= 8; ++length) {
            result[length].fill(0xffU);
            for (std::size_t i = 0; i < length; ++i)
                result[length][8U - length + i] = static_cast<std::uint8_t>(i);
        }
        return result;
    }();
    return tables;
}

std::uint32_t convert_8(uint8x8_t digits) {
    static const std::uint8_t pair_weights_data[8] = {10, 1, 10, 1, 10, 1, 10, 1};
    static const std::uint16_t quad_weights_data[4] = {100, 1, 100, 1};
    static const std::uint32_t full_weights_data[2] = {10000, 1};
    uint16x8_t products2 = vmull_u8(digits, vld1_u8(pair_weights_data));
    uint16x4_t pairs = vpadd_u16(vget_low_u16(products2), vget_high_u16(products2));
    uint32x4_t products4 = vmull_u16(pairs, vld1_u16(quad_weights_data));
    uint32x2_t quads = vpadd_u32(vget_low_u32(products4), vget_high_u32(products4));
    uint64x2_t products8 = vmull_u32(quads, vld1_u32(full_weights_data));
    return static_cast<std::uint32_t>(vaddvq_u64(products8));
}

const char *parse_neon_8(const char *p, std::uint32_t &value) {
    uint8x16_t bytes = vld1q_u8(reinterpret_cast<const std::uint8_t *>(p));
    unsigned newline_mask = neon_movemask(
        vceqq_u8(bytes, vdupq_n_u8(static_cast<std::uint8_t>('\n'))));
    if (newline_mask == 0U) return nullptr;
    unsigned length = static_cast<unsigned>(__builtin_ctz(newline_mask));
    if (length == 0U || length > 8U) return nullptr;
    uint8x16_t numeric = vsubq_u8(bytes, vdupq_n_u8(static_cast<std::uint8_t>('0')));
    uint8x16_t indices = vld1q_u8(alignment_tables()[length].data());
    uint8x16_t aligned = vqtbl1q_u8(numeric, indices);
    value = convert_8(vget_low_u8(aligned));
    return p + length + 1U;
}
#endif

#if defined(__aarch64__)
std::uint32_t serial_neon(const Dataset &data) {
    if (data.bytes.empty()) return 0;
    const char *p = data.bytes.data();
    const char *end = p + data.bytes.size();
    std::uint32_t checksum = 0;
    while (p + 16 <= end) {
        std::uint32_t value = 0;
        const char *next = parse_neon_8(p, value);
        if (next == nullptr) break;
        checksum ^= value;
        p = next;
    }
    std::uint32_t value = 0;
    while (p != end)
        scalar_update(static_cast<unsigned char>(*p++), value, checksum);
    return checksum;
}
#endif

std::uint32_t four_stream_scalar(const Dataset &data) {
    if (data.bytes.empty()) return 0;
    std::array<const char *, 4> p{};
    std::array<const char *, 4> end{};
    std::array<std::uint32_t, 4> value{};
    std::array<std::uint32_t, 4> checksum{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        p[lane] = data.bytes.data() + data.cuts4[lane];
        end[lane] = data.bytes.data() + data.cuts4[lane + 1U];
    }
    while (p[0] != end[0] && p[1] != end[1] &&
           p[2] != end[2] && p[3] != end[3]) {
        scalar_update(static_cast<unsigned char>(*p[0]++), value[0], checksum[0]);
        scalar_update(static_cast<unsigned char>(*p[1]++), value[1], checksum[1]);
        scalar_update(static_cast<unsigned char>(*p[2]++), value[2], checksum[2]);
        scalar_update(static_cast<unsigned char>(*p[3]++), value[3], checksum[3]);
    }
    for (std::size_t lane = 0; lane < 4; ++lane)
        while (p[lane] != end[lane])
            scalar_update(static_cast<unsigned char>(*p[lane]++),
                          value[lane], checksum[lane]);
    return checksum[0] ^ checksum[1] ^ checksum[2] ^ checksum[3];
}

#if defined(__aarch64__)
std::uint32_t four_stream_neon(const Dataset &data) {
    if (data.bytes.empty()) return 0;
    std::array<const char *, 4> p{};
    std::array<const char *, 4> end{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        p[lane] = data.bytes.data() + data.cuts4[lane];
        end[lane] = data.bytes.data() + data.cuts4[lane + 1U];
    }
    uint32x4_t value = vdupq_n_u32(0);
    uint32x4_t checksum = vdupq_n_u32(0);
    const uint32x4_t newline = vdupq_n_u32(static_cast<std::uint32_t>('\n'));
    const uint32x4_t ascii_zero = vdupq_n_u32(static_cast<std::uint32_t>('0'));
    while (p[0] != end[0] && p[1] != end[1] &&
           p[2] != end[2] && p[3] != end[3]) {
        std::uint32_t chars_data[4] = {
            static_cast<unsigned char>(*p[0]++), static_cast<unsigned char>(*p[1]++),
            static_cast<unsigned char>(*p[2]++), static_cast<unsigned char>(*p[3]++),
        };
        uint32x4_t chars = vld1q_u32(chars_data);
        uint32x4_t separator = vceqq_u32(chars, newline);
        uint32x4_t digit = vsubq_u32(chars, ascii_zero);
        uint32x4_t next = vmlaq_n_u32(digit, value, 10U);
        checksum = veorq_u32(checksum, vandq_u32(value, separator));
        value = vbslq_u32(separator, vdupq_n_u32(0), next);
    }
    alignas(16) std::uint32_t values[4], checksums[4];
    vst1q_u32(values, value);
    vst1q_u32(checksums, checksum);
    for (std::size_t lane = 0; lane < 4; ++lane)
        while (p[lane] != end[lane])
            scalar_update(static_cast<unsigned char>(*p[lane]++),
                          values[lane], checksums[lane]);
    return checksums[0] ^ checksums[1] ^ checksums[2] ^ checksums[3];
}
#endif

using Parser = std::uint32_t (*)(const Dataset &);

struct Variant {
    std::string_view name;
    Parser parser;
};

#if defined(__aarch64__)
constexpr std::array<Variant, 5> variants{{
    {"scalar-horner", scalar_horner},
    {"serial-neon", serial_neon},
    {"four-stream-scalar", four_stream_scalar},
    {"four-stream-neon", four_stream_neon},
    {"std-from-chars", standard_from_chars},
}};
#else
constexpr std::array<Variant, 3> variants{{
    {"scalar-horner", scalar_horner},
    {"four-stream-scalar", four_stream_scalar},
    {"std-from-chars", standard_from_chars},
}};
#endif

constexpr std::array<Variant, 2> size_variants{{
    {"scalar-horner", scalar_horner},
    {"four-stream-scalar", four_stream_scalar},
}};

bool verify(const Dataset &data) {
    std::uint32_t expected = standard_from_chars(data);
    for (const Variant &variant : variants) {
        std::uint32_t actual = variant.parser(data);
        if (actual != expected) {
            std::cerr << "mismatch," << data.name << ',' << variant.name << ','
                      << expected << ',' << actual << '\n';
            return false;
        }
    }
    return true;
}

bool test() {
    constexpr std::array<std::string_view, 4> tiny_inputs{{
        "", "0\n", "1\n2\n", "3\n4\n5\n",
    }};
    for (std::size_t i = 0; i < tiny_inputs.size(); ++i) {
        Dataset tiny{"tiny-0-to-3-records", {}, i, {}};
        tiny.bytes.assign(tiny_inputs[i].begin(), tiny_inputs[i].end());
        make_cuts(tiny);
        for (std::size_t part = 0; part < 4; ++part) {
            if (tiny.cuts4[part] > tiny.cuts4[part + 1U]
                || tiny.cuts4[part + 1U] > tiny.bytes.size())
                return false;
            std::size_t cut = tiny.cuts4[part + 1U];
            if (cut != 0 && cut != tiny.bytes.size()
                && tiny.bytes[cut - 1U] != '\n')
                return false;
        }
        if (!verify(tiny)) return false;
    }
    Dataset boundaries{"boundaries", {}, 0, {}};
    constexpr std::array<std::uint32_t, 11> values{{
        0U, 1U, 9U, 10U, 99U, 100U, 999U, 1'000U,
        9'999'999U, 10'000'000U, 99'999'999U,
    }};
    for (std::uint32_t value : values) append_number(boundaries.bytes, value);
    boundaries.count = values.size();
    make_cuts(boundaries);
    if (!verify(boundaries)) return false;
    for (std::uint32_t seed = 0; seed < 200; ++seed)
        if (!verify(make_uniform_numeric(1U + seed * 17U, seed))) return false;
    if (!verify(make_fixed8(10'003, 0x5511U))) return false;
    if (!verify(make_uniform_digits(10'003, 0x9911U))) return false;
    std::cout << "tests,ok,tiny-plus-boundaries-plus-200-partitioned-random\n";
    return true;
}

double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

double measure(const Dataset &data, Parser parser, std::uint64_t &checksum,
               std::size_t target_bytes) {
    std::size_t repeats = std::max<std::size_t>(1U, target_bytes / data.bytes.size());
    std::vector<double> samples;
    samples.reserve(9);
    for (int sample = -2; sample < 9; ++sample) {
        auto start = Clock::now();
        std::uint32_t result = 0;
        for (std::size_t repeat = 0; repeat < repeats; ++repeat)
            result ^= parser(data);
        auto stop = Clock::now();
        checksum += result;
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns / (static_cast<double>(repeats) * data.count));
        }
    }
    return median(samples);
}

void print_measurement(std::string_view kind, const Dataset &data,
                       const Variant &variant, std::uint64_t &checksum,
                       std::size_t target_bytes) {
    double ns = measure(data, variant.parser, checksum, target_bytes);
    double bytes_per_value = static_cast<double>(data.bytes.size()) /
                             static_cast<double>(data.count);
    std::cout << kind << ',' << data.name << ',' << data.bytes.size() << ','
              << data.count << ',' << variant.name << ',' << ns << ','
              << ns / bytes_per_value << '\n';
}

void bench() {
    constexpr std::size_t count = 1U << 20;
    std::vector<Dataset> datasets;
    datasets.push_back(make_uniform_numeric(count, 0x12345678U));
    datasets.push_back(make_fixed8(count, 0x23456789U));
    datasets.push_back(make_uniform_digits(count, 0x3456789aU));
    std::uint64_t checksum = 0;
    std::cout << "kind,suite,size,count,variant,ns_per_value,ns_per_byte\n";
    for (const Dataset &data : datasets)
        for (const Variant &variant : variants)
            print_measurement("workload", data, variant, checksum, 32U << 20);

    constexpr std::array<std::size_t, 9> counts{{
        128U, 512U, 2'048U, 16'384U, 131'072U,
        524'288U, 2'097'152U, 4'194'304U, 8'388'608U,
    }};
    for (std::size_t current : counts) {
        Dataset data = make_fixed8(current, static_cast<std::uint32_t>(current));
        for (const Variant &variant : size_variants)
            print_measurement("size", data, variant, checksum, 64U << 20);
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
