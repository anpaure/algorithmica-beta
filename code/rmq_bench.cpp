#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile int benchmark_sink;

struct ScanRMQ {
    std::vector<int> a;
    explicit ScanRMQ(const std::vector<int> &input) : a(input) {}
    int query(int l, int r) const {
        int result = a[l];
        for (int i = l + 1; i < r; ++i)
            result = std::min(result, a[i]);
        return result;
    }
    std::size_t bytes() const { return a.size() * sizeof(int); }
};

struct SegmentRMQ {
    int base = 1;
    std::vector<int> tree;
    explicit SegmentRMQ(const std::vector<int> &a) {
        while (base < static_cast<int>(a.size())) base <<= 1;
        tree.assign(2 * base, std::numeric_limits<int>::max());
        std::copy(a.begin(), a.end(), tree.begin() + base);
        for (int i = base - 1; i; --i)
            tree[i] = std::min(tree[2 * i], tree[2 * i + 1]);
    }
    int query(int l, int r) const {
        int result = std::numeric_limits<int>::max();
        for (l += base, r += base; l < r; l >>= 1, r >>= 1) {
            if (l & 1) result = std::min(result, tree[l++]);
            if (r & 1) result = std::min(result, tree[--r]);
        }
        return result;
    }
    std::size_t bytes() const { return tree.size() * sizeof(int); }
};

struct SparseRMQ {
    int n = 0, levels = 0;
    std::vector<int> table;
    SparseRMQ() = default;
    explicit SparseRMQ(const std::vector<int> &a) { build(a); }
    void build(const std::vector<int> &a) {
        n = static_cast<int>(a.size());
        if (n == 0) return;
        levels = 32 - __builtin_clz(static_cast<unsigned>(n));
        table.assign(static_cast<std::size_t>(n) * levels,
                     std::numeric_limits<int>::max());
        std::copy(a.begin(), a.end(), table.begin());
        for (int k = 1; k < levels; ++k) {
            int half = 1 << (k - 1), length = 2 * half;
            int *dst = table.data() + static_cast<std::size_t>(k) * n;
            const int *src = table.data() + static_cast<std::size_t>(k - 1) * n;
            for (int i = 0; i + length <= n; ++i)
                dst[i] = std::min(src[i], src[i + half]);
        }
    }
    int query(int l, int r) const {
        int k = 31 - __builtin_clz(static_cast<unsigned>(r - l));
        const int *level = table.data() + static_cast<std::size_t>(k) * n;
        return std::min(level[l], level[r - (1 << k)]);
    }
    std::size_t bytes() const { return table.size() * sizeof(int); }
};

struct BlockScanRMQ {
    static constexpr int block = 64;
    std::vector<int> a, minima;
    SparseRMQ macro;
    explicit BlockScanRMQ(const std::vector<int> &input) : a(input) {
        int blocks = (static_cast<int>(a.size()) + block - 1) / block;
        minima.resize(blocks, std::numeric_limits<int>::max());
        for (int i = 0; i < static_cast<int>(a.size()); ++i)
            minima[i / block] = std::min(minima[i / block], a[i]);
        macro.build(minima);
    }
    int query(int l, int r) const {
        int first = l / block, last = (r - 1) / block;
        int result = std::numeric_limits<int>::max();
        if (first == last) {
            for (int i = l; i < r; ++i) result = std::min(result, a[i]);
            return result;
        }
        int first_end = std::min(static_cast<int>(a.size()), (first + 1) * block);
        for (int i = l; i < first_end; ++i) result = std::min(result, a[i]);
        int last_begin = last * block;
        for (int i = last_begin; i < r; ++i) result = std::min(result, a[i]);
        if (first + 1 < last)
            result = std::min(result, macro.query(first + 1, last));
        return result;
    }
    std::size_t bytes() const {
        return (a.size() + minima.size()) * sizeof(int) + macro.bytes();
    }
};

struct BlockPrefixRMQ {
    static constexpr int block = 64;
    std::vector<int> a, prefix, suffix, minima;
    SparseRMQ macro;
    explicit BlockPrefixRMQ(const std::vector<int> &input)
        : a(input), prefix(input.size()), suffix(input.size()) {
        int n = static_cast<int>(a.size());
        int blocks = (n + block - 1) / block;
        minima.resize(blocks, std::numeric_limits<int>::max());
        for (int b = 0; b < blocks; ++b) {
            int begin = b * block, end = std::min(n, begin + block);
            prefix[begin] = a[begin];
            for (int i = begin + 1; i < end; ++i)
                prefix[i] = std::min(prefix[i - 1], a[i]);
            suffix[end - 1] = a[end - 1];
            for (int i = end - 1; i-- > begin;)
                suffix[i] = std::min(suffix[i + 1], a[i]);
            minima[b] = prefix[end - 1];
        }
        macro.build(minima);
    }
    int query(int l, int r) const {
        int first = l / block, last = (r - 1) / block;
        if (first == last) {
            int result = a[l];
            for (int i = l + 1; i < r; ++i) result = std::min(result, a[i]);
            return result;
        }
        int result = std::min(suffix[l], prefix[r - 1]);
        if (first + 1 < last)
            result = std::min(result, macro.query(first + 1, last));
        return result;
    }
    std::size_t bytes() const {
        return (a.size() + prefix.size() + suffix.size() + minima.size()) * sizeof(int)
             + macro.bytes();
    }
};

template<class Structure>
static std::pair<Structure, double> build_timed(const std::vector<int> &a) {
    auto begin = Clock::now();
    Structure structure(a);
    auto end = Clock::now();
    return {std::move(structure),
            std::chrono::duration<double, std::milli>(end - begin).count()};
}

template<class Structure>
static double query_ns(const Structure &s,
                       const std::vector<std::pair<int, int>> &queries,
                       int repeats = 5) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        int checksum = 0;
        auto begin = Clock::now();
        for (auto [l, r] : queries)
            checksum ^= s.query(l, r);
        auto end = Clock::now();
        benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / queries.size());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Structure>
static void emit(const char *name, const std::vector<int> &a, int length,
                 const std::vector<std::pair<int, int>> &queries) {
    auto [structure, build_ms] = build_timed<Structure>(a);
    std::printf("%s,%zu,%d,%.3f,%.3f,%.3f\n", name, a.size(), length,
                query_ns(structure, queries), build_ms,
                double(structure.bytes()) / a.size());
}

static std::vector<std::pair<int, int>> make_queries(int n, int length,
                                                     std::size_t count) {
    std::mt19937 rng(987654321);
    std::vector<std::pair<int, int>> q(count);
    for (auto &[l, r] : q) {
        l = static_cast<int>(rng() % static_cast<unsigned>(n - length + 1));
        r = l + length;
    }
    return q;
}

static void require_distinct_sparse_endpoints(
        const std::vector<std::pair<int, int>> &queries) {
    for (auto [l, r] : queries) {
        int k = 31 - __builtin_clz(static_cast<unsigned>(r - l));
        if (l == r - (1 << k)) {
            std::fprintf(stderr,
                         "timed sparse-table query reuses one table entry: [%d, %d)\n",
                         l, r);
            std::abort();
        }
    }
}

static void run_tests() {
    std::mt19937 rng(12345);
    for (int n = 1; n <= 130; ++n) {
        std::vector<int> a(n);
        for (int &x : a) x = static_cast<int>(rng());
        ScanRMQ scan(a); SegmentRMQ segment(a); SparseRMQ sparse(a);
        BlockScanRMQ blocked(a); BlockPrefixRMQ prefix(a);
        for (int l = 0; l < n; ++l) {
            for (int r = l + 1; r <= n; ++r) {
                int expected = scan.query(l, r);
                assert(segment.query(l, r) == expected);
                assert(sparse.query(l, r) == expected);
                assert(blocked.query(l, r) == expected);
                assert(prefix.query(l, r) == expected);
            }
        }
    }
    std::puts("rmq tests passed");
}

static void run_benchmarks() {
    std::puts("implementation,n,length,query_ns,build_ms,bytes_per_element");
    std::mt19937 rng(31415926);
    for (int log_n : {12, 16, 18, 20}) {
        int n = 1 << log_n;
        std::vector<int> a(n);
        for (int &x : a) x = static_cast<int>(rng());
        const std::vector<int> lengths = {13, 97, 1001, 3 * n / 8 + 1};
        for (int length : lengths) {
            std::size_t count = std::min<std::size_t>(200000,
                std::max<std::size_t>(2000, 30000000ULL / static_cast<unsigned>(length)));
            auto queries = make_queries(n, length, count);
            require_distinct_sparse_endpoints(queries);
            emit<ScanRMQ>("scan", a, length, queries);
            emit<SegmentRMQ>("segment", a, length, queries);
            emit<SparseRMQ>("sparse", a, length, queries);
            emit<BlockScanRMQ>("blocked_scan", a, length, queries);
            emit<BlockPrefixRMQ>("blocked_prefix", a, length, queries);
        }
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
