/*
Build and run the correctness suite:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror knapsack.cpp -o knapsack
  ./knapsack test

Run the benchmark:
  ./knapsack bench
  ./knapsack bench-csv

Add -Rpass=loop-vectorize to inspect Clang's vectorization decisions, or
-fno-vectorize -fno-slp-vectorize to measure the non-vectorized kernels.
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

using u64 = std::uint64_t;

struct Item {
    int weight;
    int value;
};

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

// General 0/1 knapsack: maximize value with total weight at most capacity.
long long knapsack_value_scalar(const Item *item, int n, int capacity,
                                long long *dp) {
    const long long unreachable = std::numeric_limits<long long>::min() / 4;
    std::fill(dp, dp + capacity + 1, unreachable);
    dp[0] = 0;

    for (int i = 0; i < n; i++) {
        int w = item[i].weight;
        int v = item[i].value;
        require(w >= 0, "negative item weight");
        if (w > capacity)
            continue;

        for (int s = capacity; s >= w; s--) {
            if (dp[s - w] != unreachable)
                dp[s] = std::max(dp[s], dp[s - w] + v);
        }
    }

    return *std::max_element(dp, dp + capacity + 1);
}

// Subset sum: mark every exactly reachable sum in [0, capacity].
void subset_sum_scalar_full(const int *weight, int n, int capacity,
                            unsigned char *reachable) {
    std::fill(reachable, reachable + capacity + 1, 0);
    reachable[0] = 1;

    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w > capacity)
            continue;
        for (int s = capacity; s >= w; s--)
            reachable[s] |= reachable[s - w];
    }
}

// The same byte DP, bounded by the sum of the processed usable weights.
void subset_sum_scalar(const int *weight, int n, int capacity,
                       unsigned char *reachable) {
    std::fill(reachable, reachable + capacity + 1, 0);
    reachable[0] = 1;
    int hi = 0;

    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w > capacity)
            continue;

        int next_hi = (hi > capacity - w ? capacity : hi + w);
        for (int s = next_hi; s >= w; s--)
            reachable[s] |= reachable[s - w];
        hi = next_hi;
    }
}

int word_count(int capacity) {
    return capacity / 64 + 1;
}

u64 last_word_mask(int capacity) {
    int used = capacity % 64 + 1;
    return used == 64 ? ~u64(0) : (u64(1) << used) - 1;
}

// Straight word-parallel form of B |= B << w. It scans the whole bitset.
void subset_sum_words_full(const int *weight, int n, int capacity, u64 *bits) {
    int words = word_count(capacity);
    std::fill(bits, bits + words, 0);
    bits[0] = 1;

    for (int k = 0; k < n; k++) {
        int w = weight[k];
        require(w >= 0, "negative subset-sum weight");
        if (w > capacity)
            continue;

        int q = w / 64;
        int r = w % 64;
        if (r == 0) {
            for (int i = words - 1; i >= q; i--)
                bits[i] |= bits[i - q];
        } else {
            for (int i = words - 1; i > q; i--)
                bits[i] |= (bits[i - q] << r)
                         | (bits[i - q - 1] >> (64 - r));
            bits[q] |= bits[0] << r;
        }
        bits[words - 1] &= last_word_mask(capacity);
    }
}

// Shift the live prefix of an existing bitset in place. The descending order
// is what keeps every source word from the pre-update state.
int shift_or_bounded(u64 *bits, int capacity, int &hi, int weight) {
    require(weight >= 0, "negative subset-sum weight");
    if (weight > capacity)
        return 0;

    int old_hi = hi;
    int next_hi = (old_hi > capacity - weight ? capacity : old_hi + weight);
    int old_last = old_hi / 64;
    int next_last = next_hi / 64;
    int q = weight / 64;
    int r = weight % 64;

    if (r == 0) {
        int top = std::min(next_last, old_last + q);
        for (int d = top; d >= q; d--)
            bits[d] |= bits[d - q];
    } else {
        int spill = old_last + q + 1;
        if (spill <= next_last)
            bits[spill] |= bits[old_last] >> (64 - r);

        int top = std::min(next_last, old_last + q);
        for (int d = top; d > q; d--)
            bits[d] |= (bits[d - q] << r)
                     | (bits[d - q - 1] >> (64 - r));
        bits[q] |= bits[0] << r;
    }

    bits[word_count(capacity) - 1] &= last_word_mask(capacity);
    hi = next_hi;
    return next_last - q + 1;
}

u64 shifted_word(const u64 *bits, int destination, int old_last, int shift) {
    int q = shift / 64;
    int r = shift % 64;
    int source = destination - q;
    u64 word = 0;
    if (0 <= source && source <= old_last)
        word |= bits[source] << r;
    if (r != 0 && 0 <= source - 1 && source - 1 <= old_last)
        word |= bits[source - 1] >> (64 - r);
    return word;
}

// A deliberately tempting experiment: compose two updates algebraically and
// touch every destination only once.
//
// (1 + z^a)(1 + z^b) = 1 + z^a + z^b + z^(a+b).
//
// It is exact, but the three shifted source streams make it slower on the
// benchmark machine.  We keep it as a measured failed optimization.
int shift_or_pair(u64 *bits, int capacity, int &hi, int a, int b) {
    require(a >= 0 && b >= 0, "negative paired shift");
    if (a <= 0 || b <= 0 || a > capacity || b > capacity) {
        int visited = 0;
        if (a > 0)
            visited += shift_or_bounded(bits, capacity, hi, a);
        if (b > 0)
            visited += shift_or_bounded(bits, capacity, hi, b);
        return visited;
    }

    int old_last = hi / 64;
    long long combined = (long long) a + b;
    long long high = (long long) hi + combined;
    int next_hi = high > capacity ? capacity : int(high);
    int next_last = next_hi / 64;
    int bottom = std::min(a, b) / 64;
    for (int d = next_last; d >= bottom; d--) {
        u64 word = bits[d]
                 | shifted_word(bits, d, old_last, a)
                 | shifted_word(bits, d, old_last, b);
        if (combined <= capacity)
            word |= shifted_word(bits, d, old_last, int(combined));
        bits[d] = word;
    }
    bits[word_count(capacity) - 1] &= last_word_mask(capacity);
    hi = next_hi;
    return next_last - bottom + 1;
}

void subset_sum_words_bounded(const int *weight, int n, int capacity,
                              u64 *bits) {
    int words = word_count(capacity);
    std::fill(bits, bits + words, 0);
    bits[0] = 1;
    int hi = 0;

    for (int i = 0; i < n; i++)
        shift_or_bounded(bits, capacity, hi, weight[i]);
}

void subset_sum_words_paired(const int *weight, int n, int capacity,
                             u64 *bits) {
    int words = word_count(capacity);
    std::fill(bits, bits + words, 0);
    bits[0] = 1;
    int hi = 0;
    int i = 0;
    for (; i + 1 < n; i += 2)
        shift_or_pair(bits, capacity, hi, weight[i], weight[i + 1]);
    if (i < n)
        shift_or_bounded(bits, capacity, hi, weight[i]);
}

// One bit per data word: a set bit means that the corresponding 64 reachable
// states are not all one yet.  The summary has one bit per marker word, so an
// empty 4096-state region can be skipped without even loading its marker.
struct IncompleteIndex {
    std::vector<u64> word;
    std::vector<u64> summary;
};

int marker_word_count(int entries) {
    return (entries + 63) / 64;
}

int build_incomplete_index(const u64 *bits, int capacity, int live_words,
                           IncompleteIndex &index) {
    int words = word_count(capacity);
    index.word.assign(marker_word_count(words), 0);
    int incomplete = 0;
    for (int d = 0; d < words; d++) {
        u64 mask = d + 1 == words ? last_word_mask(capacity) : ~u64(0);
        if ((bits[d] & mask) != mask) {
            index.word[d / 64] |= u64(1) << (d % 64);
            if (d < live_words)
                incomplete++;
        }
    }

    index.summary.assign(marker_word_count((int) index.word.size()), 0);
    for (int i = 0; i < (int) index.word.size(); i++)
        if (index.word[i])
            index.summary[i / 64] |= u64(1) << (i % 64);
    return incomplete;
}

// Apply one shift only to incomplete destination words.  Words disappear from
// the index permanently when all of their logical bits become one.
int shift_or_indexed(u64 *bits, int capacity, int &hi, int weight,
                     IncompleteIndex &index) {
    require(weight > 0, "nonpositive indexed shift");
    if (weight > capacity)
        return 0;

    int words = word_count(capacity);
    int next_hi = hi > capacity - weight ? capacity : hi + weight;
    int lo = weight / 64;
    int top = next_hi / 64;
    int q = weight / 64;
    int r = weight % 64;
    int first_marker = lo / 64;
    int last_marker = top / 64;
    int first_summary = first_marker / 64;
    int last_summary = last_marker / 64;
    int visited = 0;

    for (int si = last_summary; si >= first_summary; si--) {
        u64 marker_words = index.summary[si];
        if (si == last_summary && last_marker % 64 != 63)
            marker_words &= (u64(1) << (last_marker % 64 + 1)) - 1;
        if (si == first_summary)
            marker_words &= ~u64(0) << (first_marker % 64);

        while (marker_words) {
            int marker_bit = 63 - __builtin_clzll(marker_words);
            u64 marker_word_bit = u64(1) << marker_bit;
            marker_words ^= marker_word_bit;
            int mi = 64 * si + marker_bit;
            u64 candidates = index.word[mi];
            if (mi == last_marker && top % 64 != 63)
                candidates &= (u64(1) << (top % 64 + 1)) - 1;
            if (mi == first_marker)
                candidates &= ~u64(0) << (lo % 64);

            while (candidates) {
                int bit = 63 - __builtin_clzll(candidates);
                u64 marker = u64(1) << bit;
                candidates ^= marker;
                int d = 64 * mi + bit;
                visited++;
                u64 incoming;
                if (r == 0) {
                    incoming = bits[d - q];
                } else {
                    int source = d - q;
                    incoming = bits[source] << r;
                    if (source > 0)
                        incoming |= bits[source - 1] >> (64 - r);
                }

                u64 mask = d + 1 == words
                         ? last_word_mask(capacity) : ~u64(0);
                bits[d] = (bits[d] | incoming) & mask;
                if (bits[d] == mask)
                    index.word[mi] &= ~marker;
            }

            if (index.word[mi] == 0)
                index.summary[si] &= ~marker_word_bit;
        }
    }
    hi = next_hi;
    return visited;
}

double sampled_full_fraction(const u64 *bits, int hi) {
    int complete_words = int((std::int64_t(hi) + 1) / 64);
    if (complete_words == 0)
        return 0;
    int samples = std::min(64, complete_words);
    int full = 0;
    for (int i = 0; i < samples; i++) {
        int d = int((std::int64_t(i) * complete_words) / samples);
        full += bits[d] == ~u64(0);
    }
    return double(full) / samples;
}

struct AdaptiveStats {
    int updates = 0;
    int switch_after = -1;
    int incomplete_at_switch = -1;
    long long word_updates = 0;
    long long indexed_word_updates = 0;
    bool fell_back = false;
};

// Start with the compiler-vectorized dense loop.  Periodically take a small
// sample, then build the exact index only if at least 95% of sampled words are
// full and enough work remains to amortize the scan.
bool subset_sum_words_adaptive(const int *weight, int n, int capacity,
                               int exact_target, u64 *bits,
                               AdaptiveStats *stats = nullptr,
                               bool initialize = true) {
    require(exact_target >= -1 && exact_target <= capacity,
            "invalid exact target");
    int words = word_count(capacity);
    if (stats)
        *stats = AdaptiveStats{};
    if (initialize)
        std::fill(bits, bits + words, 0);
    bits[0] = 1;
    if (exact_target == 0)
        return true;

    int hi = 0;
    bool indexed = false;
    bool indexing_disabled = false;
    std::int64_t next_probe = 15;
    IncompleteIndex index;
    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w == 0 || w > capacity)
            continue;

        int visited = 0;
        if (indexed) {
            visited = shift_or_indexed(bits, capacity, hi, w, index);
            if (stats)
                stats->indexed_word_updates += visited;

            // A rapidly growing frontier can expose many fresh incomplete
            // words after a profitable switch.  Pay for at most one such
            // scalar update, then return to the vectorized dense loop.
            int live_words = hi / 64 + 1;
            if (visited * 8 > live_words) {
                indexed = false;
                indexing_disabled = true;
                index = IncompleteIndex{};
                if (stats)
                    stats->fell_back = true;
            }
        } else {
            visited = shift_or_bounded(bits, capacity, hi, w);
        }
        if (stats) {
            stats->updates++;
            stats->word_updates += visited;
        }

        if (exact_target >= 0
                && ((bits[exact_target / 64] >> (exact_target % 64)) & 1))
            return true;

        int remaining = n - i - 1;
        int live_words = hi / 64 + 1;
        bool enough_work = std::int64_t(remaining) * live_words
                         >= 2LL * words;
        if (!indexed && !indexing_disabled && enough_work && i >= next_probe
                && hi >= 4096) {
            next_probe = std::int64_t(i) + 16;
            if (sampled_full_fraction(bits, hi) >= 0.95) {
                int incomplete = build_incomplete_index(
                    bits, capacity, live_words, index);
                // One indexed word update costs considerably more than one
                // lane of the dense SIMD loop.  Switch only after an exact
                // scan shows that at least 15/16 of the live words can be
                // skipped.
                if (16LL * incomplete <= live_words) {
                    indexed = true;
                    if (stats) {
                        stats->switch_after = i + 1;
                        stats->incomplete_at_switch = incomplete;
                    }
                } else {
                    index = IncompleteIndex{};
                    next_probe = std::int64_t(i) + 64;
                }
            }
        }
    }
    return false;
}

bool bit_is_set(const u64 *bits, int position) {
    return (bits[position / 64] >> (position % 64)) & 1;
}

// Return the largest reachable sum represented by a completed bitset.
int best_sum(const u64 *bits, int capacity) {
    for (int i = capacity / 64; i >= 0; i--) {
        u64 word = bits[i];
        if (i == capacity / 64)
            word &= last_word_mask(capacity);
        if (word)
            return 64 * i + 63 - __builtin_clzll(word);
    }
    return 0;
}

int first_sum_at_least(const u64 *bits, int lower, int capacity) {
    require(0 <= lower && lower <= capacity, "invalid lower sum bound");
    int first_word = lower / 64;
    u64 word = bits[first_word] & (~u64(0) << (lower % 64));
    if (first_word == capacity / 64)
        word &= last_word_mask(capacity);
    if (word)
        return 64 * first_word + __builtin_ctzll(word);

    for (int i = first_word + 1; i <= capacity / 64; i++) {
        word = bits[i];
        if (i == capacity / 64)
            word &= last_word_mask(capacity);
        if (word)
            return 64 * i + __builtin_ctzll(word);
    }
    return -1;
}

struct SolverStats {
    long long usable_total = 0;
    int usable_items = 0;
    int dp_items = 0;
    int divisor = 1;
    int effective_capacity = 0;
    int exact_target = 0;
    int witness_sum_scaled = 0;
    bool complement = false;
    const char *exit = "scan";
    AdaptiveStats adaptive;
};

// Replace c equal items of weight x by groups of 1, 2, 4, ... copies.  Every
// count from 0 through c still has a representation, so this preserves the
// complete set of subset sums while reducing a long run of duplicates to
// O(log c) shifts.  Groups heavier than the DP cap cannot participate in a
// represented sum and are omitted.
std::vector<int> bundle_equal_weights(std::vector<int> item, int capacity) {
    std::sort(item.begin(), item.end());
    std::vector<int> bundled;
    for (int begin = 0; begin < (int) item.size();) {
        int end = begin + 1;
        while (end < (int) item.size() && item[end] == item[begin])
            end++;

        int count = end - begin;
        std::int64_t block = 1;
        while (count > 0) {
            int take = int(std::min<std::int64_t>(block, count));
            std::int64_t weight = std::int64_t(item[begin]) * take;
            if (weight <= capacity)
                bundled.push_back(int(weight));
            count -= take;
            block *= 2;
        }
        begin = end;
    }
    std::sort(bundled.begin(), bundled.end());
    return bundled;
}

// Return the optimum rather than materializing a stronger-than-requested full
// reachability set.  The complement orientation is exact for every positive
// 0/1 instance: if T is the usable total and C the scaled capacity, maximizing
// x <= C is equivalent to minimizing a reachable y >= T-C.  A minimum such y
// is smaller than T-C+M, where M is the maximum item, because every selected
// item in a minimum witness is essential.
int subset_sum_best_impl(const int *weight, int n, int capacity,
                         bool use_complement, bool use_bundles,
                         bool use_adaptive, bool use_probe,
                         SolverStats *stats) {
    require(n >= 0, "negative item count");
    require(capacity >= 0, "negative capacity");
    SolverStats local;
    SolverStats &out = stats ? *stats : local;
    out = SolverStats{};
    if (capacity <= 0) {
        for (int i = 0; i < n; i++)
            require(weight[i] >= 0, "negative subset-sum weight");
        out.exit = "zero";
        return 0;
    }

    std::vector<int> item;
    item.reserve(n);
    long long total = 0;
    int divisor = 0;
    int maximum = 0;
    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w == 0 || w > capacity)
            continue;
        item.push_back(w);
        total += w;
        maximum = std::max(maximum, w);
        if (divisor != 1)
            divisor = std::gcd(divisor, w);
    }
    out.usable_total = total;
    out.usable_items = (int) item.size();
    if (total <= capacity) {
        out.exit = "total";
        return int(total);
    }

    require(divisor > 0, "nontrivial instance has no usable item");
    out.divisor = divisor;
    int scaled_capacity = capacity / divisor;
    if (divisor > 1) {
        for (int &w : item)
            w /= divisor;
        total /= divisor;
        maximum /= divisor;
    }

    long long lower = total - scaled_capacity;
    long long complement_capacity = lower + maximum - 1;
    bool complement = use_complement
                   && complement_capacity < scaled_capacity;
    int effective_capacity = complement
                           ? int(complement_capacity) : scaled_capacity;
    int exact_target = complement ? int(lower) : scaled_capacity;
    out.complement = complement;
    out.effective_capacity = effective_capacity;
    out.exact_target = exact_target;

    // Reordering and bundling reduce the later frontier work, but can destroy
    // a lucky early exact fill in the caller's order.  The final solver gives
    // that order a small fixed budget before rebuilding the representation.
    if (use_probe) {
        std::vector<u64> probe_bits(word_count(effective_capacity), 0);
        probe_bits[0] = 1;
        int hi = 0;
        int probe_items = std::min(64, (int) item.size());
        for (int i = 0; i < probe_items; i++) {
            int visited = shift_or_bounded(
                probe_bits.data(), effective_capacity, hi, item[i]);
            if (stats) {
                out.adaptive.word_updates += visited;
                out.adaptive.updates++;
            }
            if (bit_is_set(probe_bits.data(), exact_target)) {
                out.dp_items = (int) item.size();
                out.exit = "exact";
                out.witness_sum_scaled = exact_target;
                return divisor * scaled_capacity;
            }
        }
        // Do not restart a short instance just because it barely crossed the
        // probe boundary.  Continuing through 4p items bounds any duplicated
        // prefix work on restarted instances by less than 25% of their item
        // count.
        if ((int) item.size() <= 4 * probe_items) {
            for (int i = probe_items; i < (int) item.size(); i++) {
                int visited = shift_or_bounded(
                    probe_bits.data(), effective_capacity, hi, item[i]);
                if (stats) {
                    out.adaptive.word_updates += visited;
                    out.adaptive.updates++;
                }
                if (bit_is_set(probe_bits.data(), exact_target)) {
                    out.dp_items = (int) item.size();
                    out.exit = "exact";
                    out.witness_sum_scaled = exact_target;
                    return divisor * scaled_capacity;
                }
            }
            out.dp_items = (int) item.size();
            if (!complement) {
                int selected = best_sum(probe_bits.data(), scaled_capacity);
                out.witness_sum_scaled = selected;
                return divisor * selected;
            }
            int excluded = first_sum_at_least(
                probe_bits.data(), int(lower), effective_capacity);
            require(excluded >= 0, "probe complement bound failed");
            out.witness_sum_scaled = excluded;
            return divisor * int(total - excluded);
        }
    }

    if (use_bundles)
        item = bundle_equal_weights(std::move(item), effective_capacity);
    out.dp_items = (int) item.size();

    std::vector<u64> bits(word_count(effective_capacity));
    bool exact = false;
    if (use_adaptive) {
        AdaptiveStats main_stats;
        exact = subset_sum_words_adaptive(
            item.data(), (int) item.size(), effective_capacity, exact_target,
            bits.data(), stats ? &main_stats : nullptr, false);
        if (stats) {
            int offset = out.adaptive.updates;
            out.adaptive.updates += main_stats.updates;
            out.adaptive.word_updates += main_stats.word_updates;
            out.adaptive.indexed_word_updates +=
                main_stats.indexed_word_updates;
            out.adaptive.fell_back |= main_stats.fell_back;
            if (main_stats.switch_after >= 0) {
                out.adaptive.switch_after = offset + main_stats.switch_after;
                out.adaptive.incomplete_at_switch =
                    main_stats.incomplete_at_switch;
            }
        }
    } else {
        bits[0] = 1;
        int hi = 0;
        for (int w : item) {
            int visited = shift_or_bounded(
                bits.data(), effective_capacity, hi, w);
            if (stats) {
                out.adaptive.word_updates += visited;
                out.adaptive.updates++;
            }
            if (bit_is_set(bits.data(), exact_target)) {
                exact = true;
                break;
            }
        }
    }
    if (exact) {
        out.exit = "exact";
        out.witness_sum_scaled = exact_target;
        return divisor * scaled_capacity;
    }
    if (!complement) {
        int selected = best_sum(bits.data(), scaled_capacity);
        out.witness_sum_scaled = selected;
        return divisor * selected;
    }

    int excluded = first_sum_at_least(bits.data(), int(lower),
                                      effective_capacity);
    require(excluded >= 0, "complement witness bound failed");
    out.witness_sum_scaled = excluded;
    return divisor * int(total - excluded);
}

int subset_sum_best_scaled_direct(const int *weight, int n, int capacity,
                                  SolverStats *stats = nullptr) {
    return subset_sum_best_impl(weight, n, capacity, false, false, false,
                                false, stats);
}

int subset_sum_best_symmetric(const int *weight, int n, int capacity,
                              SolverStats *stats = nullptr) {
    return subset_sum_best_impl(weight, n, capacity, true, false, false,
                                false, stats);
}

int subset_sum_best_bundled(const int *weight, int n, int capacity,
                            SolverStats *stats = nullptr) {
    return subset_sum_best_impl(weight, n, capacity, true, true, false,
                                false, stats);
}

int subset_sum_best_probed(const int *weight, int n, int capacity,
                           SolverStats *stats = nullptr) {
    return subset_sum_best_impl(weight, n, capacity, true, true, false,
                                true, stats);
}

int subset_sum_best_direct(const int *weight, int n, int capacity,
                           SolverStats *stats = nullptr) {
    require(n >= 0, "negative item count");
    require(capacity >= 0, "negative capacity");
    SolverStats local;
    SolverStats &out = stats ? *stats : local;
    out = SolverStats{};
    if (capacity <= 0) {
        for (int i = 0; i < n; i++)
            require(weight[i] >= 0, "negative subset-sum weight");
        out.exit = "zero";
        return 0;
    }

    std::vector<int> item;
    item.reserve(n);
    long long total = 0;
    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w == 0 || w > capacity)
            continue;
        item.push_back(w);
        total += w;
    }
    out.usable_total = total;
    out.usable_items = (int) item.size();
    if (total <= capacity) {
        out.exit = "total";
        return int(total);
    }
    out.dp_items = (int) item.size();
    out.effective_capacity = capacity;
    out.exact_target = capacity;

    std::vector<u64> bits(word_count(capacity));
    bits[0] = 1;
    int hi = 0;
    for (int w : item) {
        int visited = shift_or_bounded(bits.data(), capacity, hi, w);
        if (stats) {
            out.adaptive.word_updates += visited;
            out.adaptive.updates++;
        }
        if (bit_is_set(bits.data(), capacity)) {
            out.exit = "exact";
            out.witness_sum_scaled = capacity;
            return capacity;
        }
    }
    int answer = best_sum(bits.data(), capacity);
    out.witness_sum_scaled = answer;
    return answer;
}

// A timing-boundary-compatible baseline for the optimum-only solvers.  It
// performs preprocessing and allocation inside the call, constructs the
// complete reachable set, and scans it for the answer.
int subset_sum_best_full(const int *weight, int n, int capacity,
                         SolverStats *stats = nullptr) {
    require(n >= 0, "negative item count");
    require(capacity >= 0, "negative capacity");
    SolverStats local;
    SolverStats &out = stats ? *stats : local;
    out = SolverStats{};
    out.exit = "full";
    out.effective_capacity = capacity;
    if (capacity == 0) {
        for (int i = 0; i < n; i++)
            require(weight[i] >= 0, "negative subset-sum weight");
        return 0;
    }

    std::vector<int> item;
    item.reserve(n);
    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w == 0 || w > capacity)
            continue;
        item.push_back(w);
        out.usable_total += w;
    }
    out.usable_items = (int) item.size();
    out.dp_items = (int) item.size();

    std::vector<u64> bits(word_count(capacity), 0);
    bits[0] = 1;
    int hi = 0;
    for (int w : item) {
        int visited = shift_or_bounded(bits.data(), capacity, hi, w);
        if (stats) {
            out.adaptive.word_updates += visited;
            out.adaptive.updates++;
        }
    }
    int answer = best_sum(bits.data(), capacity);
    out.witness_sum_scaled = answer;
    return answer;
}

int subset_sum_best(const int *weight, int n, int capacity,
                    SolverStats *stats = nullptr) {
    return subset_sum_best_impl(weight, n, capacity, true, true, true,
                                true, stats);
}

u64 reverse_bits(u64 x) {
    x = ((x >> 1) & 0x5555555555555555ULL)
      | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL)
      | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0f0f0f0f0f0f0f0fULL)
      | ((x & 0x0f0f0f0f0f0f0f0fULL) << 4);
    return __builtin_bswap64(x);
}

// Load source[low..low+63] from a masked bitset, zero-padding a negative low.
u64 bit_window(const u64 *source, int capacity, int low) {
    require(-63 <= low && low <= capacity, "bit window is out of range");
    int words = word_count(capacity);
    if (low < 0)
        return source[0] << -low;

    int q = low / 64;
    int r = low % 64;
    u64 window = source[q] >> r;
    if (r != 0 && q + 1 < words)
        window |= source[q + 1] << (64 - r);
    return window;
}

// Find x with left[x] and right[target-x] both set. Reversing one 64-bit
// window turns the test for 64 consecutive x values into one intersection.
int compatible_split(const u64 *left, const u64 *right, int target) {
    int words = word_count(target);
    for (int k = 0; k < words; k++) {
        int high = target - 64 * k;
        u64 reflected = reverse_bits(bit_window(right, target, high - 63));
        u64 candidates = left[k] & reflected;
        if (k == words - 1)
            candidates &= last_word_mask(target);
        if (candidates)
            return 64 * k + __builtin_ctzll(candidates);
    }
    return -1;
}

struct RecoveryWorkspace {
    std::vector<u64> left;
    std::vector<u64> right;

    explicit RecoveryWorkspace(int target)
        : left(word_count(target)), right(word_count(target)) {}
};

void subset_sum_range(const int *weight, int begin, int end, int target,
                      u64 *bits) {
    int words = word_count(target);
    std::fill(bits, bits + words, 0);
    bits[0] = 1;
    int hi = 0;
    for (int i = begin; i < end; i++)
        shift_or_bounded(bits, target, hi, weight[i]);
}

bool recover_subset_rec(const int *weight, int begin, int end, int target,
                        RecoveryWorkspace &workspace,
                        std::vector<int> &picked) {
    if (target == 0)
        return true;
    if (end - begin == 0)
        return false;
    if (end - begin == 1) {
        if (weight[begin] != target)
            return false;
        picked.push_back(begin);
        return true;
    }

    int middle = begin + (end - begin) / 2;
    subset_sum_range(weight, begin, middle, target, workspace.left.data());
    subset_sum_range(weight, middle, end, target, workspace.right.data());
    int left_target = compatible_split(workspace.left.data(),
                                       workspace.right.data(), target);
    if (left_target < 0)
        return false;

    // The parent bitsets are dead now. Both recursive calls deliberately reuse
    // the same two buffers instead of retaining a DP layer for traceback.
    if (!recover_subset_rec(weight, begin, middle, left_target,
                            workspace, picked))
        return false;
    return recover_subset_rec(weight, middle, end, target - left_target,
                              workspace, picked);
}

// Recover a witness by recomputing two capped bitset DPs at every split. Apart
// from the output indices, this stores only two target-sized working bitsets.
bool recover_subset_words(const int *weight, int n, int target,
                          std::vector<int> &picked) {
    require(n >= 0, "negative item count");
    require(target >= 0, "negative subset-sum target");
    for (int i = 0; i < n; i++)
        require(weight[i] >= 0, "negative subset-sum weight");

    picked.clear();
    RecoveryWorkspace workspace(target);
    return recover_subset_rec(weight, 0, n, target, workspace, picked);
}

// Solve and recover an optimum without retaining the forward DP.  When the
// solver chose the complement orientation, reconstruct the smaller excluded
// subset and return all other usable items.
int recover_optimal_subset(const int *weight, int n, int capacity,
                           std::vector<int> &picked) {
    SolverStats stats;
    int answer = subset_sum_best(weight, n, capacity, &stats);
    picked.clear();

    std::vector<int> index;
    std::vector<int> scaled;
    index.reserve(n);
    scaled.reserve(n);
    for (int i = 0; i < n; i++) {
        require(weight[i] >= 0, "negative subset-sum weight");
        if (weight[i] == 0 || weight[i] > capacity)
            continue;
        index.push_back(i);
        scaled.push_back(weight[i] / stats.divisor);
    }

    if (stats.usable_total <= capacity) {
        picked = std::move(index);
        return answer;
    }
    if (answer == 0)
        return 0;

    std::vector<int> witness;
    bool found = recover_subset_words(
        scaled.data(), (int) scaled.size(), stats.witness_sum_scaled,
        witness);
    require(found, "optimum recovery target is unreachable");

    if (!stats.complement) {
        for (int position : witness)
            picked.push_back(index[position]);
        return answer;
    }

    std::vector<unsigned char> excluded(index.size(), 0);
    for (int position : witness)
        excluded[position] = 1;
    for (int i = 0; i < (int) index.size(); i++)
        if (!excluded[i])
            picked.push_back(index[i]);
    return answer;
}

std::vector<unsigned char> brute_subset(const std::vector<int> &weight,
                                        int capacity) {
    require(weight.size() <= 20, "brute-force subset too large");
    std::vector<unsigned char> answer(capacity + 1, 0);
    std::uint64_t subsets = u64(1) << weight.size();
    for (std::uint64_t mask = 0; mask < subsets; mask++) {
        int sum = 0;
        for (int i = 0; i < (int) weight.size(); i++) {
            if ((mask >> i) & 1) {
                if (weight[i] > capacity - sum) {
                    sum = capacity + 1;
                    break;
                }
                sum += weight[i];
            }
        }
        if (sum <= capacity)
            answer[sum] = 1;
    }
    return answer;
}

long long brute_knapsack(const std::vector<Item> &item, int capacity) {
    require(item.size() <= 20, "brute-force knapsack too large");
    long long answer = 0;
    std::uint64_t subsets = u64(1) << item.size();
    for (std::uint64_t mask = 0; mask < subsets; mask++) {
        int total_weight = 0;
        long long total_value = 0;
        for (int i = 0; i < (int) item.size(); i++) {
            if ((mask >> i) & 1) {
                total_weight += item[i].weight;
                total_value += item[i].value;
            }
        }
        if (total_weight <= capacity)
            answer = std::max(answer, total_value);
    }
    return answer;
}

void compare_subset_kernels(const std::vector<int> &weight, int capacity,
                            bool compare_brute) {
    std::vector<unsigned char> scalar(capacity + 1);
    std::vector<unsigned char> scalar_full(capacity + 1);
    std::vector<u64> full(word_count(capacity));
    std::vector<u64> bounded(word_count(capacity));
    std::vector<u64> paired(word_count(capacity));
    std::vector<u64> adaptive(word_count(capacity));
    std::vector<u64> bundled_bits(word_count(capacity));
    subset_sum_scalar(weight.data(), (int) weight.size(), capacity,
                      scalar.data());
    subset_sum_scalar_full(weight.data(), (int) weight.size(), capacity,
                           scalar_full.data());
    subset_sum_words_full(weight.data(), (int) weight.size(), capacity,
                          full.data());
    subset_sum_words_bounded(weight.data(), (int) weight.size(), capacity,
                             bounded.data());
    subset_sum_words_paired(weight.data(), (int) weight.size(), capacity,
                            paired.data());
    subset_sum_words_adaptive(weight.data(), (int) weight.size(), capacity,
                              -1, adaptive.data());
    std::vector<int> bundled = bundle_equal_weights(weight, capacity);
    subset_sum_words_bounded(bundled.data(), (int) bundled.size(), capacity,
                             bundled_bits.data());

    for (int s = 0; s <= capacity; s++) {
        require(bool(scalar_full[s]) == bool(scalar[s]),
                "full byte DP disagrees with bounded byte DP");
        require(bit_is_set(full.data(), s) == bool(scalar[s]),
                "full word DP disagrees with scalar DP");
        require(bit_is_set(bounded.data(), s) == bool(scalar[s]),
                "bounded word DP disagrees with scalar DP");
        require(bit_is_set(paired.data(), s) == bool(scalar[s]),
                "paired word DP disagrees with scalar DP");
        require(bit_is_set(adaptive.data(), s) == bool(scalar[s]),
                "adaptive word DP disagrees with scalar DP");
        require(bit_is_set(bundled_bits.data(), s) == bool(scalar[s]),
                "binary-bundled DP disagrees with scalar DP");
    }
    require((full.back() & ~last_word_mask(capacity)) == 0,
            "full word DP leaked bits past capacity");
    require((bounded.back() & ~last_word_mask(capacity)) == 0,
            "bounded word DP leaked bits past capacity");
    require((paired.back() & ~last_word_mask(capacity)) == 0,
            "paired word DP leaked bits past capacity");
    require((adaptive.back() & ~last_word_mask(capacity)) == 0,
            "adaptive word DP leaked bits past capacity");
    require((bundled_bits.back() & ~last_word_mask(capacity)) == 0,
            "binary-bundled DP leaked bits past capacity");

    int expected_best = capacity;
    while (expected_best > 0 && !scalar[expected_best])
        expected_best--;
    require(best_sum(full.data(), capacity) == expected_best,
            "best sum from full word DP is wrong");
    require(best_sum(bounded.data(), capacity) == expected_best,
            "best sum from bounded word DP is wrong");
    require(best_sum(adaptive.data(), capacity) == expected_best,
            "best sum from adaptive word DP is wrong");
    require(subset_sum_best(weight.data(), (int) weight.size(), capacity)
            == expected_best, "optimum-only solver is wrong");
    require(subset_sum_best_direct(weight.data(), (int) weight.size(), capacity)
            == expected_best, "direct optimum-only solver is wrong");
    require(subset_sum_best_symmetric(weight.data(), (int) weight.size(),
                                      capacity) == expected_best,
            "symmetric optimum-only solver is wrong");
    require(subset_sum_best_scaled_direct(weight.data(), (int) weight.size(),
                                          capacity) == expected_best,
            "scaled direct solver is wrong");
    require(subset_sum_best_bundled(weight.data(), (int) weight.size(),
                                    capacity) == expected_best,
            "bundled optimum-only solver is wrong");
    require(subset_sum_best_probed(weight.data(), (int) weight.size(),
                                   capacity) == expected_best,
            "probed optimum-only solver is wrong");
    require(subset_sum_best_full(weight.data(), (int) weight.size(), capacity)
            == expected_best, "full-set solver wrapper is wrong");

    if (compare_brute) {
        std::vector<unsigned char> brute = brute_subset(weight, capacity);
        require(brute == scalar, "scalar subset DP disagrees with brute force");
    }
}

void check_recovery(const std::vector<int> &weight, int target,
                    bool expected) {
    std::vector<int> picked;
    bool found = recover_subset_words(weight.data(), (int) weight.size(),
                                      target, picked);
    require(found == expected, "recovery reachability mismatch");
    if (found) {
        int sum = 0;
        int previous_index = -1;
        for (int index : picked) {
            require(index > previous_index, "recovery reused or reordered item");
            previous_index = index;
            sum += weight[index];
        }
        require(sum == target, "recovered subset has wrong sum");
    }
}

void check_optimal_recovery(const std::vector<int> &weight, int capacity) {
    std::vector<int> picked;
    int answer = recover_optimal_subset(
        weight.data(), (int) weight.size(), capacity, picked);
    int sum = 0;
    int previous = -1;
    for (int index : picked) {
        require(index > previous, "optimum recovery reordered an item");
        require(weight[index] <= capacity,
                "optimum recovery selected an unusable item");
        previous = index;
        sum += weight[index];
    }
    require(sum == answer, "optimum recovery returned the wrong sum");
    require(answer == subset_sum_best_full(
        weight.data(), (int) weight.size(), capacity),
        "optimum recovery did not return an optimum");
}

void test_compatible_split(std::mt19937_64 &rng) {
    for (int target = 0; target <= 320; target++) {
        int words = word_count(target);
        for (int test = 0; test < 200; test++) {
            std::vector<u64> left(words);
            std::vector<u64> right(words);
            for (u64 &word : left)
                word = rng();
            for (u64 &word : right)
                word = rng();
            left.back() &= last_word_mask(target);
            right.back() &= last_word_mask(target);

            int expected = -1;
            for (int x = 0; x <= target; x++) {
                if (bit_is_set(left.data(), x)
                    && bit_is_set(right.data(), target - x)) {
                    expected = x;
                    break;
                }
            }
            require(compatible_split(left.data(), right.data(), target)
                    == expected, "word-parallel split search mismatch");
        }
    }
}

void test_recovery_exhaustive() {
    // All short sequences over {0,1,2,3}: this covers empty inputs, zeroes,
    // duplicates, and many targets with more than one possible witness.
    for (int n = 0; n <= 7; n++) {
        int sequences = 1;
        for (int i = 0; i < n; i++)
            sequences *= 4;
        for (int code = 0; code < sequences; code++) {
            int value = code;
            std::vector<int> weight(n);
            int sum = 0;
            for (int &w : weight) {
                w = value % 4;
                value /= 4;
                sum += w;
            }
            std::vector<unsigned char> reachable = brute_subset(weight,
                                                                 sum + 3);
            for (int target = 0; target <= sum + 3; target++)
                check_recovery(weight, target, reachable[target]);
        }
    }
}

void test_recovery_random(std::mt19937_64 &rng) {
    // Small random instances are checked against exhaustive enumeration.
    for (int test = 0; test < 2000; test++) {
        int n = int(rng() % 16);
        int capacity = int(rng() % 193);
        std::vector<int> weight(n);
        for (int &w : weight)
            w = int(rng() % 33);
        std::vector<unsigned char> reachable = brute_subset(weight, capacity);
        int target = int(rng() % (capacity + 1));
        check_recovery(weight, target, reachable[target]);
        check_optimal_recovery(weight, capacity);
    }

    // Larger instances exercise many-word splits without using the recovery
    // implementation itself to decide whether a target should be reachable.
    for (int test = 0; test < 5000; test++) {
        int target = int(rng() % 2049);
        int n = int(rng() % 81);
        std::vector<int> weight(n);
        for (int &w : weight)
            w = int(rng() % 2300);
        std::vector<unsigned char> reachable(target + 1);
        subset_sum_scalar(weight.data(), n, target, reachable.data());
        check_recovery(weight, target, reachable[target]);
        if (test < 1000)
            check_optimal_recovery(weight, target);
    }
}

void test_shift_primitive(std::mt19937_64 &rng) {
    for (int test = 0; test < 30000; test++) {
        int capacity = int(rng() % 513);
        int hi = int(rng() % (capacity + 1));
        int shift = int(rng() % (capacity + 130));
        std::vector<unsigned char> before(capacity + 1, 0);
        std::vector<unsigned char> expected(capacity + 1, 0);
        std::vector<u64> bits(word_count(capacity), 0);

        for (int s = 0; s <= hi; s++) {
            before[s] = (rng() & 3) == 0;
            if (before[s])
                bits[s / 64] |= u64(1) << (s % 64);
        }
        std::vector<u64> indexed_bits = bits;
        IncompleteIndex index;
        build_incomplete_index(indexed_bits.data(), capacity, hi / 64 + 1,
                               index);
        expected = before;
        if (shift <= capacity) {
            for (int s = 0; s <= hi && s <= capacity - shift; s++)
                expected[s + shift] |= before[s];
        }

        int reported_hi = hi;
        shift_or_bounded(bits.data(), capacity, reported_hi, shift);
        int expected_hi = shift > capacity
                        ? hi
                        : (hi > capacity - shift ? capacity : hi + shift);
        require(reported_hi == expected_hi, "bounded shift reported wrong frontier");
        for (int s = 0; s <= capacity; s++)
            require(bit_is_set(bits.data(), s) == bool(expected[s]),
                    "bounded shift primitive mismatch");
        require((bits.back() & ~last_word_mask(capacity)) == 0,
                "bounded shift leaked bits past capacity");
        if (shift > 0) {
            int indexed_hi = hi;
            shift_or_indexed(indexed_bits.data(), capacity, indexed_hi,
                             shift, index);
            require(indexed_hi == expected_hi,
                    "indexed shift reported wrong frontier");
            for (int s = 0; s <= capacity; s++)
                require(bit_is_set(indexed_bits.data(), s) == bool(expected[s]),
                        "indexed shift primitive mismatch");
            require((indexed_bits.back() & ~last_word_mask(capacity)) == 0,
                    "indexed shift leaked bits past capacity");
        }
    }
}

void test_subset_sum(std::mt19937_64 &rng) {
    for (int n = 0; n <= 5; n++) {
        int sequences = 1;
        for (int i = 0; i < n; i++)
            sequences *= 6;
        for (int code = 0; code < sequences; code++) {
            int value = code;
            std::vector<int> weight(n);
            for (int i = 0; i < n; i++) {
                weight[i] = value % 6;
                value /= 6;
            }
            for (int capacity = 0; capacity <= 24; capacity++)
                compare_subset_kernels(weight, capacity, true);
        }
    }

    for (int test = 0; test < 5000; test++) {
        int capacity = int(rng() % 601);
        int n = int(rng() % 51);
        std::vector<int> weight(n);
        for (int &w : weight)
            w = int(rng() % (capacity + 130));
        compare_subset_kernels(weight, capacity, n <= 20);
    }

    const std::vector<int> edges = {
        0, 1, 1, 2, 31, 32, 63, 64, 65, 127, 128, 129, 511, 512, 513
    };
    for (int capacity : {0, 1, 63, 64, 65, 127, 128, 129, 511, 512})
        compare_subset_kernels(edges, capacity, true);

    for (int target = 0; target <= 512; target++) {
        std::vector<unsigned char> reachable(target + 1);
        subset_sum_scalar(edges.data(), (int) edges.size(), target,
                          reachable.data());
        check_recovery(edges, target, reachable[target]);
    }
}

void test_indexed_dp(std::mt19937_64 &rng) {
    for (int test = 0; test < 1000; test++) {
        int capacity = 4096 + int(rng() % 8193);
        int n = 32 + int(rng() % 129);
        std::vector<int> weight(n);
        for (int &w : weight)
            w = int(rng() % (capacity + 257));

        std::vector<u64> expected(word_count(capacity));
        std::vector<u64> actual(word_count(capacity));
        subset_sum_words_bounded(weight.data(), n, capacity, expected.data());

        // Force the indexed representation after the first 16 items, even on
        // sparse instances where the production heuristic would reject it.
        std::fill(actual.begin(), actual.end(), 0);
        actual[0] = 1;
        int hi = 0;
        int split = std::min(16, n);
        for (int i = 0; i < split; i++)
            shift_or_bounded(actual.data(), capacity, hi, weight[i]);
        IncompleteIndex index;
        build_incomplete_index(actual.data(), capacity, hi / 64 + 1, index);
        for (int i = split; i < n; i++) {
            if (weight[i] > 0)
                shift_or_indexed(actual.data(), capacity, hi, weight[i], index);
        }
        require(actual == expected, "indexed subset-sum DP mismatch");
    }
}

void test_index_boundaries() {
    for (int capacity : {4095, 4096, 4097, 262143, 262144, 262145}) {
        int words = word_count(capacity);
        std::vector<u64> expected(words, ~u64(0));
        expected.back() &= last_word_mask(capacity);
        int hole = capacity >= 262144 ? 262144 : capacity / 2;
        expected[hole / 64] &= ~(u64(1) << (hole % 64));
        std::vector<u64> actual = expected;

        int expected_hi = capacity;
        shift_or_bounded(expected.data(), capacity, expected_hi, 1);
        int actual_hi = capacity;
        IncompleteIndex index;
        build_incomplete_index(actual.data(), capacity, words, index);
        shift_or_indexed(actual.data(), capacity, actual_hi, 1, index);
        require(actual == expected, "indexed marker boundary mismatch");
        require(actual_hi == expected_hi, "indexed marker boundary frontier");
        for (u64 summary : index.summary)
            require(summary == 0, "completed marker remained in summary");
    }
}

void test_adaptive_transition() {
    constexpr int capacity = 300000;
    std::vector<int> weight;
    for (int repetition = 0; repetition < 40; repetition++)
        for (int w = 1; w <= 100; w++)
            weight.push_back(w);
    weight.insert(weight.begin() + 2000, 150000);

    std::vector<u64> expected(word_count(capacity));
    std::vector<u64> actual(word_count(capacity));
    subset_sum_words_bounded(weight.data(), (int) weight.size(), capacity,
                             expected.data());
    AdaptiveStats stats;
    subset_sum_words_adaptive(weight.data(), (int) weight.size(), capacity,
                              -1, actual.data(), &stats);
    require(actual == expected, "production adaptive transition mismatch");
    require(stats.switch_after >= 0, "adaptive index never activated");
    require(stats.fell_back, "adaptive index never exercised its fallback");
}

void test_solver_paths() {
    SolverStats stats;
    const std::vector<int> total = {0, 5, 7, 100};
    require(subset_sum_best(total.data(), (int) total.size(), 12, &stats)
            == 12, "total shortcut answer");
    require(std::string(stats.exit) == "total" && stats.usable_items == 2,
            "total shortcut path");

    const std::vector<int> gcd = {64, 128, 192};
    require(subset_sum_best_scaled_direct(
        gcd.data(), (int) gcd.size(), 200, &stats) == 192,
        "gcd-scaled answer");
    require(stats.divisor == 64 && stats.effective_capacity == 3,
            "gcd-scaled path");

    const std::vector<int> complement = {6, 7, 8};
    require(subset_sum_best_symmetric(
        complement.data(), (int) complement.size(), 15, &stats) == 15,
        "complement answer");
    require(stats.complement && stats.effective_capacity == 13,
            "complement path");

    const std::vector<int> duplicate(100, 5);
    require(subset_sum_best_bundled(
        duplicate.data(), (int) duplicate.size(), 450, &stats) == 450,
        "bundled answer");
    require(stats.dp_items < stats.usable_items,
            "duplicate bundling did not reduce the item count");

    const std::vector<int> exact = {4, 6, 9};
    require(subset_sum_best_direct(
        exact.data(), (int) exact.size(), 10, &stats) == 10,
        "exact-fill answer");
    require(std::string(stats.exit) == "exact",
            "exact-fill path");

    const std::vector<int> recovery = {6, 12, 18, 24};
    std::vector<int> picked;
    require(recover_optimal_subset(
        recovery.data(), (int) recovery.size(), 49, picked) == 48,
        "complement/gcd recovery answer");
    int recovered_sum = 0;
    for (int i : picked)
        recovered_sum += recovery[i];
    require(recovered_sum == 48, "complement/gcd recovery witness");
}

void test_value_knapsack(std::mt19937_64 &rng) {
    for (int test = 0; test < 1200; test++) {
        int capacity = int(rng() % 61);
        int n = int(rng() % 17);
        std::vector<Item> item(n);
        for (Item &x : item) {
            x.weight = int(rng() % 25);
            x.value = int(rng() % 81) - 30;
        }
        std::vector<long long> dp(capacity + 1);
        long long actual = knapsack_value_scalar(item.data(), n, capacity,
                                                  dp.data());
        long long expected = brute_knapsack(item, capacity);
        require(actual == expected, "value knapsack disagrees with brute force");
    }
}

void run_tests() {
    std::mt19937_64 rng(0x8f3b7a92d4c61e05ULL);
    test_shift_primitive(rng);
    test_compatible_split(rng);
    test_subset_sum(rng);
    test_indexed_dp(rng);
    test_index_boundaries();
    test_adaptive_transition();
    test_solver_paths();
    test_recovery_exhaustive();
    test_recovery_random(rng);
    test_value_knapsack(rng);
    std::puts("all knapsack/subset-sum tests passed");
}

volatile u64 benchmark_sink = 0;

double median(std::vector<double> sample) {
    std::sort(sample.begin(), sample.end());
    return sample[sample.size() / 2];
}

template<class Function>
double time_batched(int repeats, Function run) {
    int batch = 1;
    for (;;) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++)
            run();
        auto stop = std::chrono::steady_clock::now();
        double milliseconds = std::chrono::duration<double, std::milli>(
            stop - start).count();
        if (milliseconds >= 3.0 || batch >= (1 << 20))
            break;
        batch *= 2;
    }

    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++)
            run();
        auto stop = std::chrono::steady_clock::now();
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(
                stop - start).count() / batch);
    }
    return median(sample);
}

double time_scalar(const std::vector<int> &weight, int capacity, int repeats) {
    std::vector<unsigned char> reachable(capacity + 1);
    auto run = [&] {
        subset_sum_scalar(weight.data(), (int) weight.size(), capacity,
                          reachable.data());
        benchmark_sink ^= reachable[capacity] + reachable[capacity / 2];
    };
    return time_batched(repeats, run);
}

double time_scalar_full(const std::vector<int> &weight, int capacity,
                        int repeats) {
    std::vector<unsigned char> reachable(capacity + 1);
    auto run = [&] {
        subset_sum_scalar_full(weight.data(), (int) weight.size(), capacity,
                               reachable.data());
        benchmark_sink ^= reachable[capacity] + reachable[capacity / 2];
    };
    return time_batched(repeats, run);
}

double time_words_full(const std::vector<int> &weight, int capacity, int repeats) {
    std::vector<u64> bits(word_count(capacity));
    auto run = [&] {
        subset_sum_words_full(weight.data(), (int) weight.size(), capacity,
                              bits.data());
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
    };
    return time_batched(repeats, run);
}

double time_words_bounded(const std::vector<int> &weight, int capacity,
                          int repeats) {
    std::vector<u64> bits(word_count(capacity));
    auto run = [&] {
        subset_sum_words_bounded(weight.data(), (int) weight.size(), capacity,
                                 bits.data());
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
    };
    return time_batched(repeats, run);
}

double time_words_paired(const std::vector<int> &weight, int capacity,
                         int repeats) {
    std::vector<u64> bits(word_count(capacity));
    auto run = [&] {
        subset_sum_words_paired(weight.data(), (int) weight.size(), capacity,
                                bits.data());
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
    };
    return time_batched(repeats, run);
}

double time_words_adaptive(const std::vector<int> &weight, int capacity,
                           int repeats, bool sort_items) {
    std::vector<u64> bits(word_count(capacity));
    auto run = [&] {
        if (sort_items) {
            std::vector<int> ordered = weight;
            std::sort(ordered.begin(), ordered.end());
            subset_sum_words_adaptive(ordered.data(), (int) ordered.size(),
                                      capacity, -1, bits.data());
        } else {
            subset_sum_words_adaptive(weight.data(), (int) weight.size(),
                                      capacity, -1, bits.data());
        }
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
    };
    return time_batched(repeats, run);
}

double time_words_sorted(const std::vector<int> &weight, int capacity,
                         int repeats) {
    std::vector<u64> bits(word_count(capacity));
    auto run = [&] {
        std::vector<int> ordered = weight;
        std::sort(ordered.begin(), ordered.end());
        subset_sum_words_bounded(ordered.data(), (int) ordered.size(),
                                 capacity, bits.data());
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
    };
    return time_batched(repeats, run);
}

using Solver = int (*)(const int *, int, int, SolverStats *);

int calibrate_solver_batch(Solver solver, const std::vector<int> &weight,
                           int capacity) {
    for (int batch = 1;; batch *= 2) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++)
            benchmark_sink ^= u64(solver(
                weight.data(), (int) weight.size(), capacity, nullptr));
        auto stop = std::chrono::steady_clock::now();
        double milliseconds = std::chrono::duration<double, std::milli>(
            stop - start).count();
        if (milliseconds >= 3.0 || batch >= (1 << 20))
            return batch;
    }
}

double time_solver(Solver solver, const std::vector<int> &weight,
                   int capacity, int repeats) {
    int batch = calibrate_solver_batch(solver, weight, capacity);
    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++)
            benchmark_sink ^= u64(solver(
                weight.data(), (int) weight.size(), capacity, nullptr));
        auto stop = std::chrono::steady_clock::now();
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(
                stop - start).count() / batch);
    }
    return median(sample);
}

int calibrate_recovery_batch(const std::vector<int> &weight, int capacity) {
    std::vector<int> picked;
    for (int batch = 1;; batch *= 2) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++) {
            int answer = recover_optimal_subset(
                weight.data(), (int) weight.size(), capacity, picked);
            benchmark_sink ^= u64(answer) + picked.size();
        }
        auto stop = std::chrono::steady_clock::now();
        double milliseconds = std::chrono::duration<double, std::milli>(
            stop - start).count();
        if (milliseconds >= 3.0 || batch >= (1 << 20))
            return batch;
    }
}

double time_optimal_recovery(const std::vector<int> &weight, int capacity,
                             int repeats) {
    int batch = calibrate_recovery_batch(weight, capacity);
    std::vector<int> picked;
    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; i++) {
            int answer = recover_optimal_subset(
                weight.data(), (int) weight.size(), capacity, picked);
            benchmark_sink ^= u64(answer) + picked.size();
        }
        auto stop = std::chrono::steady_clock::now();
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(
                stop - start).count() / batch);
    }
    return median(sample);
}

void benchmark_case(const char *name, int n, int capacity, int min_weight,
                    int max_weight, int repeats, std::mt19937 &rng) {
    std::uniform_int_distribution<int> distribution(min_weight, max_weight);
    std::vector<int> weight(n);
    for (int &w : weight)
        w = distribution(rng);

    std::vector<unsigned char> scalar(capacity + 1);
    std::vector<u64> full(word_count(capacity));
    std::vector<u64> bounded(word_count(capacity));
    subset_sum_scalar(weight.data(), n, capacity, scalar.data());
    subset_sum_words_full(weight.data(), n, capacity, full.data());
    subset_sum_words_bounded(weight.data(), n, capacity, bounded.data());
    for (int s = 0; s <= capacity; s++) {
        require(bit_is_set(full.data(), s) == bool(scalar[s]),
                "benchmark full result mismatch");
        require(bit_is_set(bounded.data(), s) == bool(scalar[s]),
                "benchmark bounded result mismatch");
    }

    double scalar_ms = time_scalar(weight, capacity, repeats);
    double full_ms = time_words_full(weight, capacity, repeats);
    double bounded_ms = time_words_bounded(weight, capacity, repeats);
    long long sum = 0;
    for (int w : weight)
        sum += w;

    std::printf("%-24s n=%4d C=%8d sum=%10lld  scalar=%9.3f ms  "
                "words=%8.3f ms (%6.1fx)  bounded=%8.3f ms (%6.1fx)\n",
                name, n, capacity, sum, scalar_ms, full_ms,
                scalar_ms / full_ms, bounded_ms, scalar_ms / bounded_ms);
}

void run_benchmarks() {
    std::mt19937 rng(123456789);
    std::puts("median kernel time; initialization is included");
    benchmark_case("dense-100k", 1000, 100000, 1, 1000, 9, rng);
    benchmark_case("dense-1m", 2000, 1000000, 1, 1000, 7, rng);
    benchmark_case("wide-weights-1m", 2000, 1000000, 1, 1000000, 7, rng);
    benchmark_case("sparse-frontier-5m", 500, 5000000, 1, 1000, 9, rng);
    std::printf("benchmark sink: %llu\n",
                (unsigned long long) benchmark_sink);
}

void print_csv_row(const char *kind, const char *suite, int n, int capacity,
                   const char *variant, double milliseconds,
                   double total_ratio, int effective_capacity = -1,
                   int dp_items = -1, int shift_calls = -1,
                   long long destination_words = -1, int switch_after = -1,
                   int answer = -1, int divisor = -1,
                   const char *side = "none", const char *exit = "full") {
    std::printf("%s,%s,%d,%d,%s,%.9f,%.6f,%d,%d,%d,%lld,%d,%d,%d,%s,%s\n",
                kind, suite, n, capacity, variant, milliseconds, total_ratio,
                effective_capacity, dp_items, shift_calls, destination_words,
                switch_after, answer, divisor, side, exit);
}

long long usable_total(const std::vector<int> &weight, int capacity) {
    long long sum = 0;
    for (int w : weight)
        if (0 < w && w <= capacity)
            sum += w;
    return sum;
}

void benchmark_case_csv(const char *kind, const char *name,
                        const std::vector<int> &weight, int capacity,
                        int repeats, bool all_variants) {
    long long sum = usable_total(weight, capacity);
    double frontier_ratio = static_cast<double>(sum) / capacity;
    if (all_variants) {
        print_csv_row(kind, name, (int) weight.size(), capacity, "byte-full",
                      time_scalar_full(weight, capacity, repeats), frontier_ratio);
        print_csv_row(kind, name, (int) weight.size(), capacity, "byte-bounded",
                      time_scalar(weight, capacity, repeats), frontier_ratio);
        print_csv_row(kind, name, (int) weight.size(), capacity, "words-full",
                      time_words_full(weight, capacity, repeats), frontier_ratio);
    }
    print_csv_row(kind, name, (int) weight.size(), capacity, "words-bounded",
                  time_words_bounded(weight, capacity, repeats), frontier_ratio);
}

std::vector<int> random_weights(int n, int lo, int hi, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> distribution(lo, hi);
    std::vector<int> weight(n);
    for (int &w : weight)
        w = distribution(rng);
    return weight;
}

std::vector<int> residue_adversary() {
    std::vector<int> weight;
    weight.reserve(2000);
    weight.push_back(1);
    for (int i = 1; i < 2000; i++)
        weight.push_back(64 * i);
    return weight;
}

std::vector<int> descending_unique_weights() {
    std::vector<int> weight(2000);
    for (int i = 0; i < (int) weight.size(); i++)
        weight[i] = (int) weight.size() - i;
    return weight;
}

std::vector<int> weights_with_total(int n, int total, std::uint32_t seed) {
    require(n > 0, "controlled item count must be positive");
    require(total >= n, "controlled total is too small");
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> distribution(1, 10000);
    std::vector<int> raw(n);
    long long raw_sum = 0;
    for (int &x : raw) {
        x = distribution(rng);
        raw_sum += x;
    }
    require(raw_sum > 0, "controlled raw weights have zero total");

    std::vector<int> weight(n, 1);
    int remaining = total - n;
    int assigned = 0;
    for (int i = 0; i < n; i++) {
        int extra = int(std::int64_t(remaining) * raw[i] / raw_sum);
        weight[i] += extra;
        assigned += extra;
    }
    for (int i = 0; assigned < remaining; i = (i + 1) % n) {
        weight[i]++;
        assigned++;
    }
    require(usable_total(weight, total) == total,
            "controlled weights have wrong total");
    int divisor = 0;
    for (int w : weight)
        divisor = std::gcd(divisor, w);
    require(divisor == 1, "controlled weights unexpectedly share a divisor");
    return weight;
}

std::vector<int> small_residue_adversary() {
    std::vector<int> weight;
    weight.push_back(1);
    for (int i = 1; i < 65; i++)
        weight.push_back(64 * (8000 + i));
    return weight;
}

struct KernelStats {
    int item_updates = 0;
    long long word_updates = 0;
    int switch_after = -1;
    int answer = 0;
};

KernelStats diagnose_kernel(const std::vector<int> &weight, int capacity,
                            const std::string &variant) {
    std::vector<int> ordered;
    const std::vector<int> *input = &weight;
    if (variant == "words-sorted" || variant == "adaptive-sorted") {
        ordered = weight;
        std::sort(ordered.begin(), ordered.end());
        input = &ordered;
    }

    std::vector<u64> bits(word_count(capacity), 0);
    bits[0] = 1;
    KernelStats stats;
    int hi = 0;
    if (variant == "words-bounded" || variant == "words-sorted") {
        for (int w : *input) {
            stats.word_updates += shift_or_bounded(
                bits.data(), capacity, hi, w);
            stats.item_updates++;
        }
    } else if (variant == "words-paired") {
        int i = 0;
        for (; i + 1 < (int) input->size(); i += 2) {
            stats.word_updates += shift_or_pair(
                bits.data(), capacity, hi, (*input)[i], (*input)[i + 1]);
            stats.item_updates++;
        }
        if (i < (int) input->size()) {
            stats.word_updates += shift_or_bounded(
                bits.data(), capacity, hi, (*input)[i]);
            stats.item_updates++;
        }
    } else {
        require(variant == "words-adaptive" || variant == "adaptive-sorted",
                "unknown kernel benchmark variant");
        AdaptiveStats adaptive;
        subset_sum_words_adaptive(input->data(), (int) input->size(),
                                  capacity, -1, bits.data(), &adaptive);
        stats.item_updates = adaptive.updates;
        stats.word_updates = adaptive.word_updates;
        stats.switch_after = adaptive.switch_after;
    }
    stats.answer = best_sum(bits.data(), capacity);
    return stats;
}

void benchmark_kernel_case(const char *name, const std::vector<int> &weight,
                           int capacity, int repeats) {
    const char *variant[] = {
        "words-bounded", "words-sorted", "words-paired",
        "words-adaptive", "adaptive-sorted"
    };
    int expected = -1;
    double ratio = double(usable_total(weight, capacity)) / capacity;
    for (const char *v : variant) {
        KernelStats stats = diagnose_kernel(weight, capacity, v);
        if (expected < 0)
            expected = stats.answer;
        require(stats.answer == expected, "kernel benchmark answer mismatch");

        double milliseconds;
        if (std::string(v) == "words-bounded")
            milliseconds = time_words_bounded(weight, capacity, repeats);
        else if (std::string(v) == "words-sorted")
            milliseconds = time_words_sorted(weight, capacity, repeats);
        else if (std::string(v) == "words-paired")
            milliseconds = time_words_paired(weight, capacity, repeats);
        else
            milliseconds = time_words_adaptive(
                weight, capacity, repeats,
                std::string(v) == "adaptive-sorted");

        print_csv_row("kernel", name, (int) weight.size(), capacity, v,
                      milliseconds, ratio, capacity, (int) weight.size(),
                      stats.item_updates, stats.word_updates,
                      stats.switch_after, stats.answer, 1, "none", "full");
    }
}

const char *solver_side(const SolverStats &stats) {
    if (std::string(stats.exit) == "total"
            || std::string(stats.exit) == "zero")
        return "none";
    return stats.complement ? "complement" : "direct";
}

int print_solver_row(const char *kind, const char *name,
                     const std::vector<int> &weight, int capacity,
                     const char *variant, Solver solver, int repeats) {
    SolverStats stats;
    int answer = solver(weight.data(), (int) weight.size(), capacity, &stats);
    double ratio = double(usable_total(weight, capacity)) / capacity;
    print_csv_row(kind, name, (int) weight.size(), capacity, variant,
                  time_solver(solver, weight, capacity, repeats), ratio,
                  stats.effective_capacity, stats.dp_items,
                  stats.adaptive.updates, stats.adaptive.word_updates,
                  stats.adaptive.switch_after, answer, stats.divisor,
                  solver_side(stats), stats.exit);
    return answer;
}

void benchmark_solver_case(const char *name, const std::vector<int> &weight,
                           int capacity, int repeats) {
    int expected = print_solver_row(
        "solver", name, weight, capacity, "full-set",
        subset_sum_best_full, repeats);
    const struct {
        const char *name;
        Solver solver;
    } stage[] = {
        {"direct-stop", subset_sum_best_direct},
        {"scaled-direct", subset_sum_best_scaled_direct},
        {"symmetric", subset_sum_best_symmetric},
        {"bundled", subset_sum_best_bundled},
        {"probe-bundled", subset_sum_best_probed},
        {"final-adaptive", subset_sum_best},
    };
    for (const auto &current : stage) {
        int answer = print_solver_row(
            "solver", name, weight, capacity, current.name,
            current.solver, repeats);
        require(answer == expected, "solver benchmark stage disagrees");
    }
}

void benchmark_symmetry_case(double requested_ratio, std::uint32_t seed) {
    constexpr int capacity = 1000000;
    constexpr int n = 512;
    int total = int(requested_ratio * capacity + 0.5);
    std::vector<int> weight = weights_with_total(n, total, seed);
    char name[32];
    std::snprintf(name, sizeof(name), "total-%.2fW", requested_ratio);
    int expected = subset_sum_best_full(
        weight.data(), (int) weight.size(), capacity);
    int direct = print_solver_row(
        "symmetry", name, weight, capacity, "scaled-direct",
        subset_sum_best_scaled_direct, 7);
    int symmetric = print_solver_row(
        "symmetry", name, weight, capacity, "symmetric",
        subset_sum_best_symmetric, 7);
    int final = print_solver_row(
        "symmetry", name, weight, capacity, "final-adaptive",
        subset_sum_best, 7);
    require(direct == expected && symmetric == expected && final == expected,
            "symmetry benchmark stage disagrees");
}

void benchmark_recovery_case(const char *name,
                             const std::vector<int> &weight,
                             int capacity, int repeats) {
    SolverStats stats;
    int answer = subset_sum_best(
        weight.data(), (int) weight.size(), capacity, &stats);
    double ratio = double(usable_total(weight, capacity)) / capacity;
    print_csv_row("recovery", name, (int) weight.size(), capacity,
                  "answer-only",
                  time_solver(subset_sum_best, weight, capacity, repeats),
                  ratio, stats.effective_capacity, stats.dp_items,
                  stats.adaptive.updates, stats.adaptive.word_updates,
                  stats.adaptive.switch_after, answer, stats.divisor,
                  solver_side(stats), stats.exit);

    std::vector<int> picked;
    int recovered = recover_optimal_subset(
        weight.data(), (int) weight.size(), capacity, picked);
    require(recovered == answer, "recovery benchmark answer mismatch");
    print_csv_row("recovery", name, (int) weight.size(), capacity,
                  "answer-and-subset",
                  time_optimal_recovery(weight, capacity, repeats), ratio,
                  stats.witness_sum_scaled, stats.usable_items, -1, -1, -1,
                  answer, stats.divisor, solver_side(stats), "witness");
}

void run_csv_benchmarks() {
    std::puts("kind,suite,n,capacity,variant,milliseconds,total_ratio,"
              "effective_capacity,dp_items,shift_calls,destination_words,"
              "switch_after,answer,divisor,side,exit");
    benchmark_case_csv("stage", "dense-100k",
        random_weights(1000, 1, 1000, 101), 100000, 9, true);
    benchmark_case_csv("stage", "dense-1m",
        random_weights(2000, 1, 1000, 102), 1000000, 7, true);
    benchmark_case_csv("stage", "wide-weights-1m",
        random_weights(2000, 1, 1000000, 103), 1000000, 7, true);
    benchmark_case_csv("stage", "sparse-frontier-5m",
        random_weights(500, 1, 1000, 104), 5000000, 9, true);

    for (int exponent : {10, 12, 14, 16, 17, 18, 20, 22, 24}) {
        int capacity = 1 << exponent;
        std::vector<int> weight = random_weights(
            64, std::max(1, capacity / 128), std::max(2, capacity / 16),
            200U + static_cast<std::uint32_t>(exponent));
        long long usable_sum = 0;
        for (int w : weight)
            if (w <= capacity)
                usable_sum += w;
        double frontier_ratio = static_cast<double>(usable_sum) / capacity;
        int repeats = exponent <= 18 ? 9 : (exponent <= 22 ? 7 : 5);
        print_csv_row("size", "wide-size-sweep", (int) weight.size(), capacity,
                      "byte-bounded", time_scalar(weight, capacity, repeats),
                      frontier_ratio);
        print_csv_row("size", "wide-size-sweep", (int) weight.size(), capacity,
                      "words-bounded", time_words_bounded(weight, capacity, repeats),
                      frontier_ratio);
    }

    constexpr int capacity = 1000000;
    constexpr int n = 512;
    for (double ratio : {0.01, 0.03, 0.1, 0.3, 1.0, 3.0}) {
        int weight_value = std::max(1, int(ratio * capacity / n));
        std::vector<int> weight(n, weight_value);
        double actual_ratio = static_cast<double>(weight_value) * n / capacity;
        print_csv_row("frontier", "constant-weights", n, capacity, "words-full",
                      time_words_full(weight, capacity, 9), actual_ratio);
        print_csv_row("frontier", "constant-weights", n, capacity, "words-bounded",
                      time_words_bounded(weight, capacity, 9), actual_ratio);
    }

    std::vector<int> dense = random_weights(2000, 1, 1000, 102);
    std::vector<int> wide = random_weights(2000, 1, 1000000, 103);
    std::vector<int> descending = descending_unique_weights();
    std::vector<int> residue = residue_adversary();
    benchmark_kernel_case("dense-1m", dense, 1000000, 7);
    benchmark_kernel_case("wide-weights-1m", wide, 1000000, 7);
    benchmark_kernel_case("unique-desc-1m", descending, 1000000, 7);
    benchmark_kernel_case("residue-adversary", residue, 1000002, 7);

    benchmark_solver_case("dense-1m", dense, 1000000, 7);
    benchmark_solver_case("wide-weights-1m", wide, 1000000, 7);
    benchmark_solver_case("sparse-frontier-5m",
        random_weights(500, 1, 1000, 104), 5000000, 7);
    benchmark_solver_case("unique-desc-1m", descending, 1000000, 7);
    std::vector<int> gcd = random_weights(2000, 1, 1000, 105);
    for (int &w : gcd)
        w *= 64;
    benchmark_solver_case("gcd-64-1m", gcd, 1000003, 7);
    benchmark_solver_case("duplicates-900k", std::vector<int>(2000, 500),
                          900000, 7);
    benchmark_solver_case("residue-adversary", residue, 1000002, 7);
    benchmark_solver_case("small-residue", small_residue_adversary(),
                          1000002, 7);

    for (double ratio : {0.50, 0.95, 1.05, 1.25, 1.50,
                         1.75, 1.95, 2.05, 3.00})
        benchmark_symmetry_case(ratio, 300);

    benchmark_recovery_case("dense-100k",
        random_weights(1000, 1, 1000, 101), 100000, 7);
    benchmark_recovery_case("dense-1m", dense, 1000000, 7);
    benchmark_recovery_case("residue-adversary", residue, 1000002, 7);
    std::fprintf(stderr, "benchmark sink: %llu\n",
                 (unsigned long long) benchmark_sink);
}

void run_profile(const std::string &variant) {
    std::vector<int> weight = random_weights(2000, 1, 1000, 102);
    Solver solver = variant == "full" ? subset_sum_best_full
                                      : subset_sum_best;
    require(variant == "full" || variant == "final",
            "profile variant must be full or final");
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(3);
    long long calls = 0;
    do {
        int batch = variant == "full" ? 64 : 1024;
        for (int i = 0; i < batch; i++)
            benchmark_sink ^= u64(solver(
                weight.data(), (int) weight.size(), 1000000, nullptr));
        calls += batch;
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "profiled %lld calls; sink=%llu\n", calls,
                 (unsigned long long) benchmark_sink);
}

int main(int argc, char **argv) {
    if (argc == 1 || std::string(argv[1]) == "test") {
        run_tests();
        return 0;
    }
    if (std::string(argv[1]) == "bench") {
        run_benchmarks();
        return 0;
    }
    if (std::string(argv[1]) == "bench-csv") {
        run_csv_benchmarks();
        return 0;
    }
    if (std::string(argv[1]) == "profile" && argc == 3) {
        run_profile(argv[2]);
        return 0;
    }
    std::fprintf(stderr,
                 "usage: %s [test|bench|bench-csv|profile full|final]\n",
                 argv[0]);
    return 2;
}
