#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;
static volatile u64 benchmark_sink;

struct SortedSet {
    std::vector<u32> values;
    explicit SortedSet(std::vector<u32> input) : values(std::move(input)) {
        std::sort(values.begin(), values.end());
    }
    bool contains(u32 x) const {
        return std::binary_search(values.begin(), values.end(), x);
    }
    u64 intersection_count(const SortedSet &other) const {
        std::size_t i = 0, j = 0; u64 result = 0;
        while (i < values.size() && j < other.values.size()) {
            if (values[i] < other.values[j]) ++i;
            else if (other.values[j] < values[i]) ++j;
            else { ++result; ++i; ++j; }
        }
        return result;
    }
    template<class Visitor> void iterate(Visitor visit) const {
        for (u32 x : values) visit(x);
    }
    std::size_t size() const { return values.size(); }
    std::size_t bytes() const { return values.size() * sizeof(u32); }
};

struct ChunkBitmap {
    static constexpr int words_per_chunk = 1024;
    std::array<std::int32_t, 1 << 16> directory{};
    std::vector<u16> occupied;
    std::vector<u64> words;

    explicit ChunkBitmap(const std::vector<u32> &input) {
        directory.fill(-1);
        for (u32 x : input) {
            u16 high = static_cast<u16>(x >> 16);
            u16 low = static_cast<u16>(x);
            if (directory[high] < 0) {
                directory[high] = static_cast<std::int32_t>(words.size());
                occupied.push_back(high);
                words.resize(words.size() + words_per_chunk);
            }
            std::size_t base = static_cast<std::size_t>(directory[high]);
            words[base + (low >> 6)] |= 1ULL << (low & 63);
        }
        std::sort(occupied.begin(), occupied.end());
    }
    bool contains(u32 x) const {
        std::int32_t offset = directory[x >> 16];
        if (offset < 0) return false;
        u16 low = static_cast<u16>(x);
        return words[static_cast<std::size_t>(offset) + (low >> 6)] >> (low & 63) & 1;
    }
    u64 intersection_count(const ChunkBitmap &other) const {
        u64 result = 0;
        for (u16 high : occupied) {
            std::int32_t b = other.directory[high];
            if (b < 0) continue;
            std::size_t a = static_cast<std::size_t>(directory[high]);
            for (int i = 0; i < words_per_chunk; ++i)
                result += __builtin_popcountll(words[a + i]
                                             & other.words[static_cast<std::size_t>(b) + i]);
        }
        return result;
    }
    template<class Visitor> void iterate(Visitor visit) const {
        for (u16 high : occupied) {
            std::size_t base = static_cast<std::size_t>(directory[high]);
            for (int i = 0; i < words_per_chunk; ++i) {
                u64 x = words[base + i];
                while (x) {
                    unsigned bit = static_cast<unsigned>(__builtin_ctzll(x));
                    visit((u32(high) << 16) | static_cast<u32>(64 * i + bit));
                    x &= x - 1;
                }
            }
        }
    }
    std::size_t size() const {
        std::size_t result = 0;
        for (u64 x : words) result += __builtin_popcountll(x);
        return result;
    }
    std::size_t bytes() const {
        return sizeof(directory) + occupied.size() * sizeof(u16) + words.size() * sizeof(u64);
    }
};

struct AdaptiveSet {
    enum : std::uint8_t { empty = 0, array = 1, bitmap = 2 };
    static constexpr std::size_t array_limit = 4096;
    std::array<std::uint32_t, 1 << 16> offset{};
    std::array<std::uint16_t, 1 << 16> count{};
    std::array<std::uint8_t, 1 << 16> type{};
    std::vector<u16> occupied, arrays;
    std::vector<u64> bitmaps;

    explicit AdaptiveSet(const std::vector<u32> &sorted_input) {
        std::size_t begin = 0;
        while (begin < sorted_input.size()) {
            u16 high = static_cast<u16>(sorted_input[begin] >> 16);
            std::size_t end = begin + 1;
            while (end < sorted_input.size() && (sorted_input[end] >> 16) == high) ++end;
            std::size_t n = end - begin;
            occupied.push_back(high);
            count[high] = static_cast<u16>(n == 65536 ? 0 : n);
            if (n <= array_limit) {
                type[high] = array;
                offset[high] = static_cast<std::uint32_t>(arrays.size());
                for (std::size_t i = begin; i < end; ++i)
                    arrays.push_back(static_cast<u16>(sorted_input[i]));
            } else {
                type[high] = bitmap;
                offset[high] = static_cast<std::uint32_t>(bitmaps.size());
                bitmaps.resize(bitmaps.size() + 1024);
                for (std::size_t i = begin; i < end; ++i) {
                    u16 low = static_cast<u16>(sorted_input[i]);
                    bitmaps[offset[high] + (low >> 6)] |= 1ULL << (low & 63);
                }
            }
            begin = end;
        }
    }

    std::size_t cardinality(u16 high) const {
        return type[high] == bitmap && count[high] == 0 ? 65536 : count[high];
    }
    bool contains(u32 x) const {
        u16 high = static_cast<u16>(x >> 16), low = static_cast<u16>(x);
        if (type[high] == empty) return false;
        std::size_t base = offset[high];
        if (type[high] == bitmap)
            return bitmaps[base + (low >> 6)] >> (low & 63) & 1;
        const u16 *first = arrays.data() + base;
        return std::binary_search(first, first + cardinality(high), low);
    }
    u64 intersection_count(const AdaptiveSet &other) const {
        u64 result = 0;
        for (u16 high : occupied) {
            if (other.type[high] == empty) continue;
            std::size_t a = offset[high], b = other.offset[high];
            if (type[high] == bitmap && other.type[high] == bitmap) {
                for (int i = 0; i < 1024; ++i)
                    result += __builtin_popcountll(bitmaps[a + i] & other.bitmaps[b + i]);
            } else if (type[high] == array && other.type[high] == array) {
                std::size_t i = 0, j = 0, an = cardinality(high), bn = other.cardinality(high);
                while (i < an && j < bn) {
                    u16 x = arrays[a + i], y = other.arrays[b + j];
                    if (x < y) ++i; else if (y < x) ++j; else { ++result; ++i; ++j; }
                }
            } else {
                const AdaptiveSet *sparse = this, *dense = &other;
                std::size_t sparse_base = a, dense_base = b;
                if (type[high] == bitmap) {
                    sparse = &other; dense = this; sparse_base = b; dense_base = a;
                }
                for (std::size_t i = 0; i < sparse->cardinality(high); ++i) {
                    u16 low = sparse->arrays[sparse_base + i];
                    result += dense->bitmaps[dense_base + (low >> 6)] >> (low & 63) & 1;
                }
            }
        }
        return result;
    }
    template<class Visitor> void iterate(Visitor visit) const {
        for (u16 high : occupied) {
            std::size_t base = offset[high];
            if (type[high] == array) {
                for (std::size_t i = 0; i < cardinality(high); ++i)
                    visit((u32(high) << 16) | arrays[base + i]);
            } else {
                for (int i = 0; i < 1024; ++i) {
                    u64 x = bitmaps[base + i];
                    while (x) {
                        unsigned bit = static_cast<unsigned>(__builtin_ctzll(x));
                        visit((u32(high) << 16) | static_cast<u32>(64 * i + bit));
                        x &= x - 1;
                    }
                }
            }
        }
    }
    std::size_t size() const {
        std::size_t result = 0;
        for (u16 high : occupied) result += cardinality(high);
        return result;
    }
    std::size_t bytes() const {
        return sizeof(offset) + sizeof(count) + sizeof(type)
             + occupied.size() * sizeof(u16) + arrays.size() * sizeof(u16)
             + bitmaps.size() * sizeof(u64);
    }
};

static std::pair<std::vector<u32>, std::vector<u32>> make_pair_sets(
        std::size_t containers, std::size_t per_container) {
    assert(per_container <= 32768 && containers <= 65536);
    std::vector<u32> a, b;
    a.reserve(containers * per_container); b.reserve(containers * per_container);
    for (std::size_t j = 0; j < containers; ++j) {
        u16 high = static_cast<u16>((j * 40507) & 65535);
        std::vector<u16> low_a, low_b;
        low_a.reserve(per_container); low_b.reserve(per_container);
        for (std::size_t i = 0; i < per_container; ++i)
            low_a.push_back(static_cast<u16>((i * 40503) & 65535));
        for (std::size_t i = 0; i < per_container / 2; ++i)
            low_b.push_back(low_a[i]);
        for (std::size_t i = per_container; i < per_container + per_container / 2; ++i)
            low_b.push_back(static_cast<u16>((i * 40503) & 65535));
        if (per_container & 1) low_b.push_back(low_a.back());
        std::sort(low_a.begin(), low_a.end()); std::sort(low_b.begin(), low_b.end());
        for (u16 low : low_a) a.push_back((u32(high) << 16) | low);
        for (u16 low : low_b) b.push_back((u32(high) << 16) | low);
    }
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    return {std::move(a), std::move(b)};
}

static std::vector<u32> make_queries(const std::vector<u32> &values, std::size_t count) {
    std::vector<u32> queries(count);
    for (std::size_t i = 0; i < count; ++i) {
        u32 x = values[(i * 1000003) % values.size()];
        if ((i & 1) == 0) {
            queries[i] = x;
        } else {
            u32 high = x & 0xffff0000U;
            u16 low = static_cast<u16>(x ^ 0x8000U);
            u32 candidate = high | low;
            while (std::binary_search(values.begin(), values.end(), candidate)) {
                low = static_cast<u16>(low + 1);
                candidate = high | low;
            }
            queries[i] = candidate;
        }
    }
    return queries;
}

template<class Set>
static double contains_ns(const Set &set, const std::vector<u32> &queries) {
    std::vector<double> samples;
    for (int rep = 0; rep < 5; ++rep) {
        u64 checksum = 0; auto begin = Clock::now();
        for (u32 x : queries) checksum += set.contains(x);
        auto end = Clock::now(); benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / queries.size());
    }
    std::sort(samples.begin(), samples.end()); return samples[2];
}

template<class Set>
static double intersection_minput_keys(const Set &a, const Set &b) {
    std::vector<double> samples;
    for (int rep = 0; rep < 7; ++rep) {
        auto begin = Clock::now(); u64 result = a.intersection_count(b); auto end = Clock::now();
        benchmark_sink = result;
        double ns = std::chrono::duration<double, std::nano>(end - begin).count();
        samples.push_back(double(a.size()) * 1000.0 / ns);
    }
    std::sort(samples.begin(), samples.end()); return samples[3];
}

template<class Set>
static double iteration_ns(const Set &set) {
    std::vector<double> samples;
    for (int rep = 0; rep < 5; ++rep) {
        u64 checksum = 0; auto begin = Clock::now();
        set.iterate([&](u32 x) { checksum += x; });
        auto end = Clock::now(); benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / set.size());
    }
    std::sort(samples.begin(), samples.end()); return samples[2];
}

template<class Set>
static void emit(const char *name, const Set &a, const Set &b,
                 std::size_t containers, std::size_t per,
                 const std::vector<u32> &queries) {
    std::printf("%s,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f\n", name, containers, per,
                a.size(), contains_ns(a, queries), intersection_minput_keys(a, b),
                iteration_ns(a), double(a.bytes()) / a.size());
}

static void run_tests() {
    for (std::size_t per : {std::size_t(1), std::size_t(32), std::size_t(4096),
                            std::size_t(4097), std::size_t(32768)}) {
        auto [av, bv] = make_pair_sets(3, per);
        SortedSet as(av), bs(bv); ChunkBitmap ac(av), bc(bv); AdaptiveSet aa(av), ba(bv);
        assert(as.size() == ac.size() && as.size() == aa.size());
        assert(as.intersection_count(bs) == ac.intersection_count(bc));
        assert(as.intersection_count(bs) == aa.intersection_count(ba));
        for (u32 x : av) assert(ac.contains(x) && aa.contains(x));
        u64 x = 0, y = 0, z = 0;
        as.iterate([&](u32 v) { x += v; });
        ac.iterate([&](u32 v) { y += v; });
        aa.iterate([&](u32 v) { z += v; });
        assert(x == y && x == z);
    }
    std::puts("bitmap tests passed");
}

static void run_benchmarks() {
    std::puts("implementation,containers,per_container,entries,contains_ns,intersection_minput_keys_s,iteration_ns_per_key,bytes_per_key");
    for (std::size_t per : {std::size_t(32), std::size_t(256), std::size_t(2048),
                            std::size_t(8192), std::size_t(32768)}) {
        std::size_t containers = std::max<std::size_t>(1, 262144 / per);
        auto [av, bv] = make_pair_sets(containers, per);
        auto queries = make_queries(av, 200000);
        SortedSet as(av), bs(bv); ChunkBitmap ac(av), bc(bv); AdaptiveSet aa(av), ba(bv);
        emit("sorted", as, bs, containers, per, queries);
        emit("chunk_bitmap", ac, bc, containers, per, queries);
        emit("adaptive", aa, ba, containers, per, queries);
    }
}

int main(int argc, char **argv) {
    if (argc != 2 || (std::string(argv[1]) != "--test" && std::string(argv[1]) != "--bench")) {
        std::fprintf(stderr, "usage: %s --test|--bench\n", argv[0]); return 2;
    }
    if (std::string(argv[1]) == "--test") run_tests(); else run_benchmarks();
}
