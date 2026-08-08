#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr std::size_t token_length = 13;
constexpr std::size_t record_length = 14;

struct Dataset {
    std::size_t count = 0;
    std::vector<std::uint64_t> scaled;
    std::vector<double> values;
    std::vector<char> input;
};

constexpr char digit_pairs[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

void write_token_from_scaled(char *out, std::uint64_t value) {
    for (int position = 12; position >= 0; --position) {
        if (position == 6) {
            out[position] = '.';
        } else {
            out[position] = static_cast<char>('0' + value % 10U);
            value /= 10U;
        }
    }
}

Dataset make_dataset(std::size_t count, std::uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::uint64_t> dist(
        100'000'000'000ULL, 999'999'999'999ULL);
    Dataset data;
    data.count = count;
    data.scaled.resize(count);
    data.values.resize(count);
    data.input.assign(count * record_length + 16U, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t scaled = dist(rng);
        data.scaled[i] = scaled;
        data.values[i] = static_cast<double>(scaled) / 1'000'000.0;
        char *record = data.input.data() + i * record_length;
        write_token_from_scaled(record, scaled);
        record[token_length] = '\n';
    }
    return data;
}

Dataset make_dataset(const std::vector<std::uint64_t> &scaled) {
    Dataset data;
    data.count = scaled.size();
    data.scaled = scaled;
    data.values.resize(data.count);
    data.input.assign(data.count * record_length + 16U, 0);
    for (std::size_t i = 0; i < data.count; ++i) {
        data.values[i] = static_cast<double>(scaled[i]) / 1'000'000.0;
        char *record = data.input.data() + i * record_length;
        write_token_from_scaled(record, scaled[i]);
        record[token_length] = '\n';
    }
    return data;
}

std::uint64_t bits_of(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t mix(std::uint64_t state, double value) {
    return (state ^ bits_of(value)) * 0x9e3779b97f4a7c15ULL;
}

using Parser = void (*)(const Dataset &, std::vector<double> &);

__attribute__((noinline)) void parse_strtod(const Dataset &data,
                                            std::vector<double> &output) {
    char token[token_length + 1];
    token[token_length] = 0;
    for (std::size_t i = 0; i < data.count; ++i) {
        std::memcpy(token, data.input.data() + i * record_length, token_length);
        char *end = nullptr;
        double value = std::strtod(token, &end);
        output[i] = end == token + token_length
                  ? value : std::numeric_limits<double>::quiet_NaN();
    }
}

__attribute__((noinline)) void parse_strtod_direct(
        const Dataset &data, std::vector<double> &output) {
    for (std::size_t i = 0; i < data.count; ++i) {
        const char *first = data.input.data() + i * record_length;
        char *end = nullptr;
        double value = std::strtod(first, &end);
        output[i] = end == first + token_length
                  ? value : std::numeric_limits<double>::quiet_NaN();
    }
}

std::uint64_t parse_scaled_scalar(const char *p) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < token_length; ++i)
        if (i != 6U)
            value = 10U * value + static_cast<unsigned>(p[i] - '0');
    return value;
}

__attribute__((noinline)) void parse_scalar_exact(
        const Dataset &data, std::vector<double> &output) {
    for (std::size_t i = 0; i < data.count; ++i) {
        std::uint64_t scaled = parse_scaled_scalar(
            data.input.data() + i * record_length);
        output[i] = static_cast<double>(scaled) / 1'000'000.0;
    }
}

__attribute__((noinline)) void parse_scalar_reciprocal(
        const Dataset &data, std::vector<double> &output) {
    for (std::size_t i = 0; i < data.count; ++i) {
        std::uint64_t scaled = parse_scaled_scalar(
            data.input.data() + i * record_length);
        output[i] = static_cast<double>(scaled) * 0.000001;
    }
}

#if defined(__aarch64__)
std::uint32_t convert4(uint8x8_t digits) {
    static const std::uint8_t w10_data[8] = {10, 1, 10, 1, 0, 0, 0, 0};
    static const std::uint16_t w100_data[4] = {100, 1, 0, 0};
    uint16x8_t p2 = vmull_u8(digits, vld1_u8(w10_data));
    uint16x4_t pairs = vpadd_u16(vget_low_u16(p2), vget_high_u16(p2));
    uint32x4_t p4 = vmull_u16(pairs, vld1_u16(w100_data));
    return vaddvq_u32(p4);
}

std::uint32_t convert8(uint8x8_t digits) {
    static const std::uint8_t w10_data[8] = {10, 1, 10, 1, 10, 1, 10, 1};
    static const std::uint16_t w100_data[4] = {100, 1, 100, 1};
    static const std::uint32_t w10000_data[2] = {10000, 1};
    uint16x8_t p2 = vmull_u8(digits, vld1_u8(w10_data));
    uint16x4_t pairs = vpadd_u16(vget_low_u16(p2), vget_high_u16(p2));
    uint32x4_t p4 = vmull_u16(pairs, vld1_u16(w100_data));
    uint32x2_t quads = vpadd_u32(vget_low_u32(p4), vget_high_u32(p4));
    uint64x2_t p8 = vmull_u32(quads, vld1_u32(w10000_data));
    return static_cast<std::uint32_t>(vaddvq_u64(p8));
}

std::uint64_t parse_scaled_neon(const char *p) {
    static const std::uint8_t gather_data[16] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12,
        0xff, 0xff, 0xff, 0xff,
    };
    uint8x16_t ascii = vld1q_u8(reinterpret_cast<const std::uint8_t *>(p));
    uint8x16_t numeric = vsubq_u8(ascii, vdupq_n_u8('0'));
    uint8x16_t digits = vqtbl1q_u8(numeric, vld1q_u8(gather_data));
    std::uint32_t high = convert4(vget_low_u8(digits));
    uint8x16_t shifted = vextq_u8(digits, digits, 4);
    std::uint32_t low = convert8(vget_low_u8(shifted));
    return static_cast<std::uint64_t>(high) * 100'000'000ULL + low;
}
__attribute__((noinline)) void parse_neon_exact(
        const Dataset &data, std::vector<double> &output) {
    for (std::size_t i = 0; i < data.count; ++i) {
        std::uint64_t scaled = parse_scaled_neon(
            data.input.data() + i * record_length);
        output[i] = static_cast<double>(scaled) / 1'000'000.0;
    }
}
#endif

struct ParseVariant {
    std::string_view name;
    Parser parser;
    bool exact;
};

#if defined(__aarch64__)
constexpr std::array<ParseVariant, 5> parse_variants{{
    {"strtod-copy", parse_strtod, true},
    {"strtod-direct", parse_strtod_direct, true},
    {"scalar-exact", parse_scalar_exact, true},
    {"neon-exact", parse_neon_exact, true},
    {"reciprocal-approx", parse_scalar_reciprocal, false},
}};
#else
constexpr std::array<ParseVariant, 4> parse_variants{{
    {"strtod-copy", parse_strtod, true},
    {"strtod-direct", parse_strtod_direct, true},
    {"scalar-exact", parse_scalar_exact, true},
    {"reciprocal-approx", parse_scalar_reciprocal, false},
}};
#endif

using Formatter = bool (*)(const Dataset &, std::vector<char> &);

std::uint64_t checksum_values(const std::vector<double> &output) {
    std::uint64_t checksum = output.size();
    for (double value : output)
        checksum = mix(checksum, value);
    return checksum;
}

std::uint64_t checksum_output(const std::vector<char> &output) {
    std::uint64_t checksum = output.size();
    for (char value : output)
        checksum = checksum * 257U + static_cast<unsigned char>(value);
    return checksum;
}

__attribute__((noinline)) bool format_snprintf(
        const Dataset &data, std::vector<char> &output) {
    for (std::size_t i = 0; i < data.count; ++i) {
        char *p = output.data() + i * token_length;
        int length = std::snprintf(p, token_length + 1U, "%.6f", data.values[i]);
        if (length != static_cast<int>(token_length)) return false;
    }
    return true;
}

std::uint64_t recover_scaled(double value) {
    return static_cast<std::uint64_t>(std::llround(value * 1'000'000.0));
}

__attribute__((noinline)) bool format_div10(
        const Dataset &data, std::vector<char> &output) {
    for (std::size_t i = 0; i < data.count; ++i)
        write_token_from_scaled(output.data() + i * token_length,
                                recover_scaled(data.values[i]));
    return true;
}

void write_token_pairs(char *out, std::uint64_t value) {
    constexpr std::array<int, 6> positions{{11, 9, 7, 4, 2, 0}};
    out[6] = '.';
    for (int position : positions) {
        std::uint64_t q = value / 100U;
        unsigned remainder = static_cast<unsigned>(value - q * 100U);
        std::memcpy(out + position, digit_pairs + 2U * remainder, 2);
        value = q;
    }
}

__attribute__((noinline)) bool format_pairs(
        const Dataset &data, std::vector<char> &output) {
    for (std::size_t i = 0; i < data.count; ++i)
        write_token_pairs(output.data() + i * token_length,
                          recover_scaled(data.values[i]));
    return true;
}

struct FormatVariant {
    std::string_view name;
    Formatter formatter;
};

constexpr std::array<FormatVariant, 3> format_variants{{
    {"snprintf", format_snprintf},
    {"div10-fixed", format_div10},
    {"pairs-fixed", format_pairs},
}};

bool check_dataset(const Dataset &data) {
    std::vector<double> expected(data.count);
    parse_strtod(data, expected);
    for (const ParseVariant &variant : parse_variants) {
        std::vector<double> actual(data.count);
        variant.parser(data, actual);
        if (variant.exact) {
            for (std::size_t i = 0; i < data.count; ++i)
                if (bits_of(actual[i]) != bits_of(expected[i])) {
                    std::cerr << "bitwise-parse-mismatch," << variant.name
                              << ',' << i << '\n';
                    return false;
                }
        }
    }
    for (const FormatVariant &variant : format_variants) {
        std::vector<char> actual(data.count * token_length + 1U);
        if (!variant.formatter(data, actual)) {
            std::cerr << "format-mismatch," << variant.name << '\n';
            return false;
        }
        for (std::size_t i = 0; i < data.count; ++i)
            if (std::memcmp(actual.data() + i * token_length,
                            data.input.data() + i * record_length,
                            token_length) != 0) {
                std::cerr << "format-mismatch," << variant.name << ',' << i << '\n';
                return false;
            }
    }
    for (std::size_t i = 0; i < data.count; ++i) {
        const char *token = data.input.data() + i * record_length;
        char *end = nullptr;
        double reference_value = std::strtod(token, &end);
        double scalar_value = static_cast<double>(parse_scaled_scalar(token)) /
                              1'000'000.0;
#if defined(__aarch64__)
        double neon_value = static_cast<double>(parse_scaled_neon(token)) /
                            1'000'000.0;
#endif
        if (end != token + token_length ||
            bits_of(reference_value) != bits_of(scalar_value)
#if defined(__aarch64__)
            || bits_of(reference_value) != bits_of(neon_value)
#endif
            ) {
            std::cerr << "bitwise-parse-mismatch," << i << '\n';
            return false;
        }
        if (recover_scaled(data.values[i]) != data.scaled[i]) {
            std::cerr << "roundtrip-mismatch," << i << '\n';
            return false;
        }
    }
    return true;
}

bool test() {
    Dataset random = make_dataset(1'000'000, 0xf10a710U);
    if (!check_dataset(random)) return false;

    struct BoundaryCase {
        std::uint64_t scaled;
        const char *token;
    };
    constexpr std::array boundaries{
        BoundaryCase{100'000'000'000ULL, "100000.000000"},
        BoundaryCase{100'000'000'001ULL, "100000.000001"},
        BoundaryCase{100'000'999'999ULL, "100000.999999"},
        BoundaryCase{100'001'000'000ULL, "100001.000000"},
        BoundaryCase{499'999'999'999ULL, "499999.999999"},
        BoundaryCase{500'000'000'000ULL, "500000.000000"},
        BoundaryCase{999'998'999'999ULL, "999998.999999"},
        BoundaryCase{999'999'000'000ULL, "999999.000000"},
        BoundaryCase{999'999'999'998ULL, "999999.999998"},
        BoundaryCase{999'999'999'999ULL, "999999.999999"},
    };
    std::vector<std::uint64_t> scaled;
    scaled.reserve(boundaries.size());
    for (const BoundaryCase &entry : boundaries)
        scaled.push_back(entry.scaled);
    Dataset boundary_data = make_dataset(scaled);
    for (std::size_t i = 0; i < boundaries.size(); ++i)
        if (std::memcmp(boundary_data.input.data() + i * record_length,
                        boundaries[i].token, token_length) != 0) {
            std::cerr << "boundary-token-mismatch," << i << '\n';
            return false;
        }
    if (!check_dataset(boundary_data)) return false;

    std::cout << "tests,ok,1000000-random-plus-10-boundary-cases\n";
    return true;
}

std::size_t reciprocal_mismatches(const Dataset &data) {
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < data.count; ++i) {
        const char *p = data.input.data() + i * record_length;
        std::uint64_t scaled = parse_scaled_scalar(p);
        double exact = static_cast<double>(scaled) / 1'000'000.0;
        double approximate = static_cast<double>(scaled) * 0.000001;
        mismatches += static_cast<std::size_t>(bits_of(exact) != bits_of(approximate));
    }
    return mismatches;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

double measure_parser(const Dataset &data, Parser parser, std::uint64_t &sink,
                      std::size_t repetitions = 1) {
    std::vector<double> output(data.count);
    std::vector<double> samples;
    for (int sample = -2; sample < 9; ++sample) {
        auto start = Clock::now();
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
            parser(data, output);
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        auto stop = Clock::now();
        sink ^= checksum_values(output);
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns / static_cast<double>(data.count * repetitions));
        }
    }
    return median(samples);
}

double measure_formatter(const Dataset &data, Formatter formatter,
                         std::uint64_t &sink) {
    std::vector<char> output(data.count * token_length + 1U);
    std::vector<double> samples;
    for (int sample = -2; sample < 9; ++sample) {
        auto start = Clock::now();
        bool valid = formatter(data, output);
        auto stop = Clock::now();
        if (!valid) std::abort();
        sink ^= checksum_output(output);
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns / static_cast<double>(data.count));
        }
    }
    return median(samples);
}

void bench() {
    Dataset data = make_dataset(1U << 20, 0xdec1a15U);
    std::uint64_t sink = 0;
    std::cout << "kind,variant,count,working_set_bytes,value\n";
    for (const ParseVariant &variant : parse_variants)
        std::cout << "parse," << variant.name << ',' << data.count << ','
                  << data.count * (record_length + sizeof(double)) << ','
                  << measure_parser(data, variant.parser, sink) << '\n';
    for (const FormatVariant &variant : format_variants)
        std::cout << "format," << variant.name << ',' << data.count << ','
                  << data.count * (token_length + sizeof(double)) << ','
                  << measure_formatter(data, variant.formatter, sink) << '\n';

    for (int exponent = 8; exponent <= 20; ++exponent) {
        std::size_t count = std::size_t{1} << exponent;
        Dataset sweep = make_dataset(count, 0x51ae000U + exponent);
        std::size_t repetitions = std::max<std::size_t>(1, (1U << 20) / count);
        for (const ParseVariant &variant : parse_variants) {
            if (variant.name != "scalar-exact"
#if defined(__aarch64__)
                && variant.name != "neon-exact"
#endif
                ) continue;
            std::cout << "parse-size," << variant.name << ',' << count << ','
                      << count * (record_length + sizeof(double)) << ','
                      << measure_parser(sweep, variant.parser, sink,
                                        repetitions) << '\n';
        }
    }
    std::size_t wrong = reciprocal_mismatches(data);
    std::cout << "correctness,reciprocal-mismatch-count," << data.count
              << ",0," << wrong << '\n';
    std::cout << "correctness,total-values," << data.count << ",0,"
              << data.count << '\n';
    std::cerr << "checksum," << sink << '\n';
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
