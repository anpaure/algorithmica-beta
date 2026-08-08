/*
Build:
  clang++ -std=c++20 -O3 -mcpu=native -Wall -Wextra -Werror tries_bench.cpp -o tries_bench

Run:
  ./tries_bench --test
  ./tries_bench --bench
*/

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile std::uint64_t benchmark_sink;

struct DenseTrie {
    struct Node {
        std::array<std::uint32_t, 26> child{};
        bool terminal = false;
    };
    std::vector<Node> nodes{1};

    void add(std::string_view s) {
        std::uint32_t v = 0;
        for (char ch : s) {
            unsigned c = static_cast<unsigned>(ch - 'a');
            assert(c < 26);
            std::uint32_t next = nodes[v].child[c];
            if (next == 0) {
                next = static_cast<std::uint32_t>(nodes.size());
                nodes[v].child[c] = next;
                nodes.push_back({});
            }
            v = next;
        }
        nodes[v].terminal = true;
    }

    bool contains(std::string_view s) const {
        std::uint32_t v = 0;
        for (char ch : s) {
            unsigned c = static_cast<unsigned>(ch - 'a');
            if (c >= 26) return false;
            v = nodes[v].child[c];
            if (v == 0) return false;
        }
        return nodes[v].terminal;
    }

    std::size_t bytes() const { return nodes.capacity() * sizeof(Node); }
};

struct PackedTrie {
    std::vector<std::uint32_t> mask, first;
    std::vector<std::uint8_t> terminal;

    explicit PackedTrie(const DenseTrie &dense) {
        std::size_t n = dense.nodes.size();
        mask.resize(n); first.resize(n); terminal.resize(n);
        std::vector<std::vector<std::uint32_t>> children(n);
        for (std::size_t v = 0; v < n; ++v) {
            terminal[v] = dense.nodes[v].terminal;
            for (unsigned c = 0; c < 26; ++c) {
                std::uint32_t u = dense.nodes[v].child[c];
                if (u) {
                    mask[v] |= 1U << c;
                    children[v].push_back(u);
                }
            }
        }
        // Renumber breadth-first so every node's children are consecutive.
        std::vector<std::uint32_t> old_to_new(n), new_to_old;
        new_to_old.reserve(n); new_to_old.push_back(0); old_to_new[0] = 0;
        for (std::size_t head = 0; head < new_to_old.size(); ++head) {
            std::uint32_t old = new_to_old[head];
            for (std::uint32_t child : children[old]) {
                old_to_new[child] = static_cast<std::uint32_t>(new_to_old.size());
                new_to_old.push_back(child);
            }
        }
        std::vector<std::uint32_t> new_mask(n), new_first(n);
        std::vector<std::uint8_t> new_terminal(n);
        for (std::size_t v = 0; v < n; ++v) {
            std::uint32_t old = new_to_old[v];
            new_mask[v] = mask[old]; new_terminal[v] = terminal[old];
            if (!children[old].empty())
                new_first[v] = old_to_new[children[old][0]];
        }
        mask.swap(new_mask); first.swap(new_first); terminal.swap(new_terminal);
    }

    bool contains(std::string_view s) const {
        std::uint32_t v = 0;
        for (char ch : s) {
            unsigned c = static_cast<unsigned>(ch - 'a');
            if (c >= 26) return false;
            std::uint32_t m = mask[v], bit = 1U << c;
            if ((m & bit) == 0) return false;
            v = first[v] + static_cast<unsigned>(__builtin_popcount(m & (bit - 1)));
        }
        return terminal[v] != 0;
    }

    std::size_t bytes() const {
        return (mask.capacity() + first.capacity()) * sizeof(std::uint32_t)
             + terminal.capacity() * sizeof(std::uint8_t);
    }
};

struct CompressedTrie {
    struct Edge {
        std::uint32_t label_offset, child;
        std::uint16_t label_length;
        std::uint16_t padding = 0;
    };
    struct Node {
        std::uint32_t mask = 0, first = 0;
        bool terminal = false;
    };
    struct TempEdge { std::string label; std::uint32_t child; };
    struct TempNode { bool terminal = false; std::vector<TempEdge> edges; };

    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::string labels;

    static int degree(const DenseTrie &dense, std::uint32_t v) {
        int result = 0;
        for (std::uint32_t x : dense.nodes[v].child) result += x != 0;
        return result;
    }

    static std::pair<unsigned, std::uint32_t> only_child(const DenseTrie &dense,
                                                         std::uint32_t v) {
        for (unsigned c = 0; c < 26; ++c)
            if (dense.nodes[v].child[c]) return {c, dense.nodes[v].child[c]};
        return {0, 0};
    }

    static std::uint32_t build_temp(const DenseTrie &dense, std::uint32_t v,
                                    std::vector<TempNode> &temp) {
        std::uint32_t index = static_cast<std::uint32_t>(temp.size());
        temp.push_back({dense.nodes[v].terminal, {}});
        for (unsigned c = 0; c < 26; ++c) {
            std::uint32_t u = dense.nodes[v].child[c];
            if (!u) continue;
            std::string label(1, static_cast<char>('a' + c));
            while (!dense.nodes[u].terminal && degree(dense, u) == 1) {
                auto [next_c, next] = only_child(dense, u);
                label.push_back(static_cast<char>('a' + next_c));
                u = next;
            }
            std::uint32_t child = build_temp(dense, u, temp);
            temp[index].edges.push_back({std::move(label), child});
        }
        return index;
    }

    explicit CompressedTrie(const DenseTrie &dense) {
        std::vector<TempNode> temp;
        build_temp(dense, 0, temp);
        nodes.resize(temp.size());
        for (std::size_t v = 0; v < temp.size(); ++v) {
            nodes[v].terminal = temp[v].terminal;
            nodes[v].first = static_cast<std::uint32_t>(edges.size());
            for (const TempEdge &te : temp[v].edges) {
                unsigned c = static_cast<unsigned>(te.label[0] - 'a');
                nodes[v].mask |= 1U << c;
                assert(te.label.size() <= std::numeric_limits<std::uint16_t>::max());
                Edge e{static_cast<std::uint32_t>(labels.size()), te.child,
                       static_cast<std::uint16_t>(te.label.size())};
                labels += te.label;
                edges.push_back(e);
            }
        }
        nodes.shrink_to_fit();
        edges.shrink_to_fit();
        labels.shrink_to_fit();
    }

    bool contains(std::string_view s) const {
        std::uint32_t v = 0;
        std::size_t p = 0;
        while (p < s.size()) {
            unsigned c = static_cast<unsigned>(s[p] - 'a');
            if (c >= 26) return false;
            std::uint32_t m = nodes[v].mask, bit = 1U << c;
            if ((m & bit) == 0) return false;
            const Edge &e = edges[nodes[v].first
                + static_cast<unsigned>(__builtin_popcount(m & (bit - 1)))];
            if (p + e.label_length > s.size()) return false;
            if (std::memcmp(s.data() + p, labels.data() + e.label_offset,
                            e.label_length) != 0) return false;
            p += e.label_length;
            v = e.child;
        }
        return nodes[v].terminal;
    }

    std::size_t bytes() const {
        return nodes.capacity() * sizeof(Node) + edges.capacity() * sizeof(Edge)
             + labels.capacity();
    }
};

struct TransparentHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const {
        return std::hash<std::string_view>{}(s);
    }
    std::size_t operator()(const std::string &s) const {
        return (*this)(std::string_view(s));
    }
};

struct TransparentEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

struct HashSet {
    std::unordered_set<std::string, TransparentHash, TransparentEqual> set;
    explicit HashSet(const std::vector<std::string> &keys) {
        set.reserve(keys.size() * 2);
        set.insert(keys.begin(), keys.end());
    }
    bool contains(std::string_view s) const { return set.find(s) != set.end(); }
    std::size_t bytes() const {
        std::size_t result = set.bucket_count() * sizeof(void*);
        for (const std::string &s : set) {
            result += sizeof(std::string) + 2 * sizeof(void*);
            std::uintptr_t data = reinterpret_cast<std::uintptr_t>(s.data());
            std::uintptr_t object = reinterpret_cast<std::uintptr_t>(&s);
            bool inline_storage = object <= data && data < object + sizeof(s);
            if (!inline_storage)
                result += s.capacity() + 1;
        }
        return result;
    }
};

static std::string base26(std::uint64_t x, int length) {
    std::string s(length, 'a');
    for (int i = length; i-- > 0;) {
        s[i] = static_cast<char>('a' + x % 26);
        x /= 26;
    }
    return s;
}

static std::vector<std::string> make_keys(std::size_t n, bool paths) {
    std::vector<std::string> result;
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!paths) {
            std::uint64_t x = i * 0x9e3779b97f4a7c15ULL + 12345;
            std::string s(16, 'a');
            for (char &c : s) { x ^= x >> 12; x ^= x << 25; x ^= x >> 27; c = 'a' + x % 26; }
            s.replace(8, 8, base26(i, 8)); // uniqueness without sorting the prefix
            result.push_back(std::move(s));
        } else {
            std::string group = base26(i / 64, 8);
            result.push_back("service" + group + "endpoint" + base26(i, 6));
        }
    }
    return result;
}

static DenseTrie make_dense(const std::vector<std::string> &keys) {
    DenseTrie trie;
    for (const std::string &s : keys) trie.add(s);
    trie.nodes.shrink_to_fit();
    return trie;
}

template<class Structure>
static double lookup_ns(const Structure &s, const std::vector<std::string> &queries,
                        bool batched, int repeats = 7) {
    std::vector<double> samples;
    for (int rep = 0; rep < repeats; ++rep) {
        std::uint64_t checksum = 0;
        auto begin = Clock::now();
        if (batched) {
            for (std::size_t i = 0; i < queries.size(); i += 4) {
                bool a = s.contains(queries[i]);
                bool b = s.contains(queries[i + 1]);
                bool c = s.contains(queries[i + 2]);
                bool d = s.contains(queries[i + 3]);
                checksum += a + 2 * b + 4 * c + 8 * d;
            }
        } else {
            for (const std::string &q : queries) checksum += s.contains(q);
        }
        auto end = Clock::now();
        benchmark_sink = checksum;
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count()
                          / queries.size());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template<class Structure>
static void emit(const char *dataset, const char *name, const Structure &s,
                 const std::vector<std::string> &keys,
                 const std::vector<std::string> &queries, double build_ms) {
    std::printf("%s,%s,%zu,%.3f,%.3f,%.3f,%.3f\n", dataset, name, keys.size(),
                lookup_ns(s, queries, false), lookup_ns(s, queries, true), build_ms,
                double(s.bytes()) / keys.size());
}

static std::vector<std::string> make_queries(const std::vector<std::string> &keys,
                                             std::size_t count) {
    assert(count % 2 == 0);
    std::vector<std::string> result(count);
    std::mt19937 rng(424242);
    std::size_t hits = 0, misses = 0;
    for (std::size_t i = 0; i < count; ++i) {
        result[i] = keys[rng() % keys.size()];
        if ((i & 1) == 0)
            ++hits;
        else {
            // All generated keys in one dataset have the same length, so no
            // stored key can equal this longer query.  Lookup still has to
            // traverse the complete stored key before rejecting the suffix.
            result[i].push_back('a');
            ++misses;
        }
    }
    std::unordered_set<std::string_view> stored;
    stored.reserve(keys.size() * 2);
    for (const std::string &key : keys) stored.insert(key);
    std::size_t actual_hits = 0;
    for (const std::string &query : result)
        actual_hits += stored.contains(query);
    assert(hits == count / 2 && misses == count / 2);
    assert(actual_hits == hits && count - actual_hits == misses);
    return result;
}

template<class Factory>
static auto timed_factory(Factory factory) {
    auto begin = Clock::now();
    auto value = factory();
    auto end = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return std::pair{std::move(value), ms};
}

static void run_tests() {
    for (bool paths : {false, true}) {
        auto keys = make_keys(1000, paths);
        DenseTrie dense = make_dense(keys);
        PackedTrie packed(dense); CompressedTrie compressed(dense); HashSet hash(keys);
        for (const std::string &s : keys) {
            assert(dense.contains(s)); assert(packed.contains(s));
            assert(compressed.contains(s)); assert(hash.contains(s));
            std::string miss = s + 'a';
            assert(!hash.contains(miss));
            assert(!dense.contains(miss));
            assert(!packed.contains(miss));
            assert(!compressed.contains(miss));
        }
    }
    std::puts("trie tests passed");
}

static void run_benchmarks() {
    std::puts("dataset,implementation,keys,lookup_ns,batch4_ns,build_ms,bytes_per_key");
    for (bool paths : {false, true}) {
        const char *dataset = paths ? "paths" : "random";
        for (std::size_t n : {std::size_t(1024), std::size_t(16384), std::size_t(131072)}) {
            auto keys = make_keys(n, paths);
            auto queries = make_queries(keys, 300000);
            auto [hash, hash_ms] = timed_factory([&] { return HashSet(keys); });
            auto [dense, dense_ms] = timed_factory([&] { return make_dense(keys); });
            auto [packed, packed_ms] = timed_factory([&] { return PackedTrie(dense); });
            auto [compressed, compressed_ms] = timed_factory([&] { return CompressedTrie(dense); });
            emit(dataset, "unordered", hash, keys, queries, hash_ms);
            emit(dataset, "dense", dense, keys, queries, dense_ms);
            emit(dataset, "packed", packed, keys, queries, dense_ms + packed_ms);
            emit(dataset, "compressed", compressed, keys, queries, dense_ms + compressed_ms);
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
