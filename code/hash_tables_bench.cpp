#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using u32 = std::uint32_t;
using Clock = std::chrono::steady_clock;

static volatile u32 benchmark_sink;

static u32 mix32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ (x >> 16);
}

static std::size_t next_power_of_two(std::size_t x) {
    std::size_t p = 1;
    while (p < x)
        p <<= 1;
    return p;
}

struct FlatAoS {
    struct Cell { u32 key = 0, value = 0; };
    std::vector<Cell> cells;
    std::size_t mask;

    explicit FlatAoS(std::size_t capacity)
        : cells(capacity), mask(capacity - 1) {}

    void add(u32 key, u32 value) {
        std::size_t i = mix32(key) & mask;
        while (cells[i].key != 0 && cells[i].key != key)
            i = (i + 1) & mask;
        cells[i] = {key, value};
    }

    bool find(u32 key, u32 &value) const {
        std::size_t i = mix32(key) & mask;
        while (cells[i].key != 0) {
            if (cells[i].key == key) {
                value = cells[i].value;
                return true;
            }
            i = (i + 1) & mask;
        }
        return false;
    }

    std::size_t bytes() const { return cells.size() * sizeof(Cell); }
};

struct FlatSoA {
    std::vector<u32> keys, values;
    std::size_t mask;

    explicit FlatSoA(std::size_t capacity)
        : keys(capacity), values(capacity), mask(capacity - 1) {}

    void add(u32 key, u32 value) {
        std::size_t i = mix32(key) & mask;
        while (keys[i] != 0 && keys[i] != key)
            i = (i + 1) & mask;
        keys[i] = key;
        values[i] = value;
    }

    bool find(u32 key, u32 &value) const {
        std::size_t i = mix32(key) & mask;
        while (keys[i] != 0) {
            if (keys[i] == key) {
                value = values[i];
                return true;
            }
            i = (i + 1) & mask;
        }
        return false;
    }

    std::size_t bytes() const {
        return (keys.size() + values.size()) * sizeof(u32);
    }
};

// Portable scalar control-byte probing.  Sixteen control bytes are one NEON
// vector on Arm and one SSE vector on x86; compilers may vectorize these loops.
struct GroupTable {
    static constexpr std::size_t group = 16;
    std::vector<std::uint8_t> control;
    std::vector<u32> keys, values;
    std::size_t mask;

    explicit GroupTable(std::size_t capacity)
        : control(capacity), keys(capacity), values(capacity), mask(capacity - 1) {
        assert((capacity & (capacity - 1)) == 0 && capacity >= group);
    }

    static std::uint8_t fingerprint(u32 hash) {
        return static_cast<std::uint8_t>((hash >> 25) + 1); // 1..128; zero is empty
    }

    void add(u32 key, u32 value) {
        u32 h = mix32(key);
        std::uint8_t fp = fingerprint(h);
        std::size_t base = (h & mask) & ~(group - 1);
        while (true) {
            for (std::size_t j = 0; j < group; ++j) {
                std::size_t i = base + j;
                if (control[i] == fp && keys[i] == key) {
                    values[i] = value;
                    return;
                }
            }
            for (std::size_t j = 0; j < group; ++j) {
                std::size_t i = base + j;
                if (control[i] == 0) {
                    keys[i] = key;
                    values[i] = value;
                    control[i] = fp;
                    return;
                }
            }
            base = (base + group) & mask;
        }
    }

    bool find(u32 key, u32 &value) const {
        u32 h = mix32(key);
        std::uint8_t fp = fingerprint(h);
        std::size_t base = (h & mask) & ~(group - 1);
        while (true) {
            bool empty = false;
            for (std::size_t j = 0; j < group; ++j) {
                std::size_t i = base + j;
                std::uint8_t c = control[i];
                empty |= c == 0;
                if (c == fp && keys[i] == key) {
                    value = values[i];
                    return true;
                }
            }
            if (empty)
                return false;
            base = (base + group) & mask;
        }
    }

    std::size_t bytes() const {
        return control.size() + (keys.size() + values.size()) * sizeof(u32);
    }
};

struct SameHash {
    std::size_t operator()(u32 x) const { return mix32(x); }
};

static std::atomic<std::size_t> allocated_bytes{0};

template<class T>
struct CountingAllocator {
    using value_type = T;
    CountingAllocator() noexcept = default;
    template<class U> CountingAllocator(const CountingAllocator<U>&) noexcept {}
    T *allocate(std::size_t n) {
        allocated_bytes.fetch_add(n * sizeof(T), std::memory_order_relaxed);
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T *p, std::size_t n) noexcept {
        std::allocator<T>{}.deallocate(p, n);
    }
    template<class U> struct rebind { using other = CountingAllocator<U>; };
};

template<class T, class U>
bool operator==(const CountingAllocator<T>&, const CountingAllocator<U>&) { return true; }
template<class T, class U>
bool operator!=(const CountingAllocator<T>&, const CountingAllocator<U>&) { return false; }

using CountedMap = std::unordered_map<u32, u32, SameHash, std::equal_to<u32>,
                                      CountingAllocator<std::pair<const u32, u32>>>;

struct StdTable {
    CountedMap table;
    std::size_t allocated = 0;

    StdTable(std::size_t capacity, float max_load) {
        allocated_bytes.store(0, std::memory_order_relaxed);
        table.max_load_factor(max_load);
        table.rehash(capacity);
        assert(table.bucket_count() == capacity);
    }
    void finish_counting() { allocated = allocated_bytes.load(std::memory_order_relaxed); }
    void add(u32 key, u32 value) { table[key] = value; }
    bool find(u32 key, u32 &value) const {
        auto it = table.find(key);
        if (it == table.end())
            return false;
        value = it->second;
        return true;
    }
    std::size_t bytes() const { return allocated; }
};

template<class Table>
static double independent_ns(const Table &table, const std::vector<u32> &queries,
                             int repeats = 5) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        u32 checksum = 0, value = 0;
        auto begin = Clock::now();
        for (u32 key : queries)
            checksum += table.find(key, value) ? value : 0x9e3779b9U;
        auto end = Clock::now();
        benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / queries.size());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Table>
static double chained_ns(const Table &table, u32 first, std::size_t steps,
                         int repeats = 5) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        u32 key = first, next = 0;
        auto begin = Clock::now();
        for (std::size_t i = 0; i < steps; ++i) {
            bool ok = table.find(key, next);
            assert(ok);
            key = next;
        }
        auto end = Clock::now();
        benchmark_sink = key;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / steps);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Table>
static void fill_table(Table &table, const std::vector<u32> &keys) {
    const std::size_t stride = 65537;
    for (std::size_t i = 0; i < keys.size(); ++i)
        table.add(keys[i], keys[(i + stride) % keys.size()]);
}

template<class Table>
static void emit_row(const char *name, const Table &table, std::size_t capacity,
                     const std::vector<u32> &keys, const std::vector<u32> &queries) {
    double throughput = independent_ns(table, queries);
    double latency = chained_ns(table, keys[0], 300000);
    std::printf("%s,%zu,%zu,%.6f,%.3f,%.3f,%.3f\n", name, keys.size(), capacity,
                double(keys.size()) / capacity, throughput, latency,
                double(table.bytes()) / keys.size());
}

static std::vector<u32> make_keys(std::size_t n, u32 base) {
    std::vector<u32> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        result[i] = mix32(static_cast<u32>(i) + base);
        assert(result[i] != 0);
    }
    return result;
}

static void run_tests() {
    for (std::size_t n : {std::size_t(1), std::size_t(17), std::size_t(1000)}) {
        std::size_t capacity = next_power_of_two(std::max<std::size_t>(32, n * 2));
        auto keys = make_keys(n, 1);
        FlatAoS aos(capacity);
        FlatSoA soa(capacity);
        GroupTable group(capacity);
        for (std::size_t i = 0; i < n; ++i) {
            aos.add(keys[i], static_cast<u32>(i + 7));
            soa.add(keys[i], static_cast<u32>(i + 7));
            group.add(keys[i], static_cast<u32>(i + 7));
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (auto *tag : {"aos", "soa", "group"}) {
                u32 value = 0;
                bool found = false;
                if (std::strcmp(tag, "aos") == 0) found = aos.find(keys[i], value);
                if (std::strcmp(tag, "soa") == 0) found = soa.find(keys[i], value);
                if (std::strcmp(tag, "group") == 0) found = group.find(keys[i], value);
                assert(found && value == i + 7);
            }
        }
        for (u32 miss : make_keys(200, 1000000)) {
            u32 value = 0;
            assert(!aos.find(miss, value));
            assert(!soa.find(miss, value));
            assert(!group.find(miss, value));
        }
        if (n > 1) {
            aos.add(keys[0], 123); soa.add(keys[0], 123); group.add(keys[0], 123);
            u32 a = 0, b = 0, c = 0;
            assert(aos.find(keys[0], a) && soa.find(keys[0], b)
                   && group.find(keys[0], c));
            assert(a == 123 && b == 123 && c == 123);
        }
    }
    std::puts("hash table tests passed");
}

static void run_benchmarks() {
    std::puts("implementation,entries,capacity,load,throughput_ns,dependent_hit_ns,bytes_per_key");
    const std::size_t query_count = 300000;
    for (int log_capacity : {12, 16, 20, 22}) {
        std::size_t capacity = std::size_t(1) << log_capacity;
        for (double load : {0.50, 0.75, 0.875}) {
            std::size_t n = static_cast<std::size_t>(capacity * load);
            auto keys = make_keys(n, 1);
            auto misses = make_keys(query_count / 2, 0x80000000U);
            std::vector<u32> queries(query_count);
            std::mt19937 rng(1234567);
            for (std::size_t i = 0; i < query_count / 2; ++i) {
                queries[2 * i] = keys[rng() % keys.size()];
                queries[2 * i + 1] = misses[i];
            }

            {
                StdTable table(capacity, static_cast<float>(load + 0.03));
                fill_table(table, keys);
                table.finish_counting();
                emit_row("unordered", table, capacity, keys, queries);
            }
            {
                FlatAoS table(capacity); fill_table(table, keys);
                emit_row("flat_aos", table, capacity, keys, queries);
            }
            {
                FlatSoA table(capacity); fill_table(table, keys);
                emit_row("flat_soa", table, capacity, keys, queries);
            }
            {
                GroupTable table(capacity); fill_table(table, keys);
                emit_row("fingerprint_groups", table, capacity, keys, queries);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2 || (std::string(argv[1]) != "--test" && std::string(argv[1]) != "--bench")) {
        std::fprintf(stderr, "usage: %s --test|--bench\n", argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--test")
        run_tests();
    else
        run_benchmarks();
}
