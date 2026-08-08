#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Writer = char *(*)(char *, std::uint32_t);

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

int decimal_length(std::uint32_t x) {
    if (x < 10U) return 1;
    if (x < 100U) return 2;
    if (x < 1'000U) return 3;
    if (x < 10'000U) return 4;
    if (x < 100'000U) return 5;
    if (x < 1'000'000U) return 6;
    if (x < 10'000'000U) return 7;
    if (x < 100'000'000U) return 8;
    if (x < 1'000'000'000U) return 9;
    return 10;
}

char *write_div10(char *out, std::uint32_t x) {
    char temporary[10];
    char *p = temporary + sizeof temporary;
    do {
        std::uint32_t q = x / 10U;
        *--p = static_cast<char>('0' + (x - q * 10U));
        x = q;
    } while (x != 0U);
    std::size_t length = static_cast<std::size_t>(temporary + sizeof temporary - p);
    std::memcpy(out, p, length);
    return out + length;
}

char *write_pairs_copy(char *out, std::uint32_t x) {
    char temporary[10];
    char *p = temporary + sizeof temporary;
    while (x >= 100U) {
        std::uint32_t q = x / 100U;
        std::uint32_t r = x - q * 100U;
        p -= 2;
        std::memcpy(p, digit_pairs + 2U * r, 2);
        x = q;
    }
    if (x < 10U) {
        *--p = static_cast<char>('0' + x);
    } else {
        p -= 2;
        std::memcpy(p, digit_pairs + 2U * x, 2);
    }
    std::size_t length = static_cast<std::size_t>(temporary + sizeof temporary - p);
    std::memcpy(out, p, length);
    return out + length;
}

char *write_pairs_direct(char *out, std::uint32_t x) {
    char *end = out + decimal_length(x);
    char *p = end;
    while (x >= 100U) {
        std::uint32_t q = x / 100U;
        std::uint32_t r = x - q * 100U;
        p -= 2;
        std::memcpy(p, digit_pairs + 2U * r, 2);
        x = q;
    }
    if (x < 10U) {
        *--p = static_cast<char>('0' + x);
    } else {
        p -= 2;
        std::memcpy(p, digit_pairs + 2U * x, 2);
    }
    return end;
}

char *write_small(char *out, std::uint32_t x) {
    if (x < 10U) {
        *out++ = static_cast<char>('0' + x);
        return out;
    }
    if (x < 100U) {
        std::memcpy(out, digit_pairs + 2U * x, 2);
        return out + 2;
    }
    if (x < 1'000U) {
        *out++ = static_cast<char>('0' + x / 100U);
        x %= 100U;
        std::memcpy(out, digit_pairs + 2U * x, 2);
        return out + 2;
    }
    std::uint32_t hi = x / 100U;
    std::uint32_t lo = x - hi * 100U;
    std::memcpy(out, digit_pairs + 2U * hi, 2);
    std::memcpy(out + 2, digit_pairs + 2U * lo, 2);
    return out + 4;
}

void write_fixed4(char *out, std::uint32_t x) {
    std::uint32_t hi = x / 100U;
    std::uint32_t lo = x - hi * 100U;
    std::memcpy(out, digit_pairs + 2U * hi, 2);
    std::memcpy(out + 2, digit_pairs + 2U * lo, 2);
}

char *write_groups4(char *out, std::uint32_t x) {
    if (x < 10'000U)
        return write_small(out, x);
    if (x < 100'000'000U) {
        std::uint32_t hi = x / 10'000U;
        std::uint32_t lo = x - hi * 10'000U;
        char *p = write_small(out, hi);
        write_fixed4(p, lo);
        return p + 4;
    }
    std::uint32_t hi = x / 100'000'000U;
    std::uint32_t rest = x - hi * 100'000'000U;
    std::uint32_t mid = rest / 10'000U;
    std::uint32_t lo = rest - mid * 10'000U;
    char *p = write_small(out, hi);
    write_fixed4(p, mid);
    write_fixed4(p + 4, lo);
    return p + 8;
}

char *write_to_chars(char *out, std::uint32_t x) {
    auto result = std::to_chars(out, out + 10, x);
    return result.ptr;
}

struct Variant {
    std::string_view name;
    Writer writer;
};

constexpr std::array<Variant, 5> variants{{
    {"div10-copy", write_div10},
    {"pairs-copy", write_pairs_copy},
    {"pairs-direct", write_pairs_direct},
    {"groups4-direct", write_groups4},
    {"std-to-chars", write_to_chars},
}};

std::vector<std::uint32_t> uniform32(std::size_t n) {
    std::mt19937 rng(0x7a17b39dU);
    std::vector<std::uint32_t> values(n);
    for (std::uint32_t &x : values)
        x = rng();
    return values;
}

std::vector<std::uint32_t> uniform_digits(std::size_t n) {
    constexpr std::array<std::uint32_t, 10> lo{{
        0U, 10U, 100U, 1'000U, 10'000U, 100'000U, 1'000'000U,
        10'000'000U, 100'000'000U, 1'000'000'000U,
    }};
    constexpr std::array<std::uint32_t, 10> hi{{
        9U, 99U, 999U, 9'999U, 99'999U, 999'999U, 9'999'999U,
        99'999'999U, 999'999'999U, std::numeric_limits<std::uint32_t>::max(),
    }};
    std::mt19937 rng(0x0ddc0ffeU);
    std::vector<std::uint32_t> values(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t d = i % 10U;
        std::uniform_int_distribution<std::uint32_t> dist(lo[d], hi[d]);
        values[i] = dist(rng);
    }
    std::shuffle(values.begin(), values.end(), rng);
    return values;
}

std::vector<std::uint32_t> counters(std::size_t n) {
    std::vector<std::uint32_t> values(n);
    for (std::size_t i = 0; i < n; ++i)
        values[i] = static_cast<std::uint32_t>(i);
    return values;
}

bool verify_value(std::uint32_t x) {
    char reference[16];
    auto expected = std::to_chars(reference, reference + sizeof reference, x);
    std::size_t expected_length = static_cast<std::size_t>(expected.ptr - reference);
    for (const Variant &variant : variants) {
        char actual[16]{};
        char *end = variant.writer(actual, x);
        std::size_t actual_length = static_cast<std::size_t>(end - actual);
        if (actual_length != expected_length ||
            std::memcmp(actual, reference, expected_length) != 0) {
            std::cerr << "mismatch," << variant.name << ',' << x << '\n';
            return false;
        }
    }
    return true;
}

bool test() {
    std::vector<std::uint32_t> special{0U, 1U, 9U, 10U,
        std::numeric_limits<std::uint32_t>::max()};
    for (std::uint32_t p = 10U; p <= 1'000'000'000U; p *= 10U) {
        special.push_back(p - 1U);
        special.push_back(p);
        special.push_back(p + 1U);
    }
    for (std::uint32_t x : special)
        if (!verify_value(x)) return false;

    std::mt19937 rng(0x51a7eU);
    for (int i = 0; i < 1'000'000; ++i)
        if (!verify_value(rng())) return false;
    std::cout << "tests,ok,1000000-random-plus-boundaries\n";
    return true;
}

double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

double measure(const std::vector<std::uint32_t> &values, Writer writer,
               std::vector<char> &output, std::uint64_t &checksum) {
    std::vector<double> samples;
    samples.reserve(11);
    for (int sample = -2; sample < 9; ++sample) {
        char *p = output.data();
        auto start = Clock::now();
        for (std::uint32_t x : values)
            p = writer(p, x);
        auto stop = Clock::now();
        checksum += static_cast<unsigned char>(output[0]);
        checksum += static_cast<unsigned char>(p[-1]);
        checksum += static_cast<std::uint64_t>(p - output.data());
        if (sample >= 0) {
            double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(ns / static_cast<double>(values.size()));
        }
    }
    return median(samples);
}

void benchmark_suite(std::string_view suite,
                     const std::vector<std::uint32_t> &values,
                     std::uint64_t &checksum) {
    std::vector<char> output(values.size() * 10U);
    for (const Variant &variant : variants) {
        double ns = measure(values, variant.writer, output, checksum);
        std::cout << suite << ',' << variant.name << ',' << ns << '\n';
    }
}

void bench() {
    constexpr std::size_t n = 1U << 20;
    std::uint64_t checksum = 0;
    std::cout << "suite,variant,ns_per_value\n";
    benchmark_suite("uniform32", uniform32(n), checksum);
    benchmark_suite("uniform-digits", uniform_digits(n), checksum);
    benchmark_suite("counters", counters(n), checksum);
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
