/*
Build and run the correctness suite:
  clang++ -std=c++17 -O3 -mcpu=native -Wall -Wextra -Werror knapsack.cpp -o knapsack
  ./knapsack test

Run the benchmark:
  ./knapsack bench

Add -Rpass=loop-vectorize to inspect Clang's vectorization decisions, or
-fno-vectorize -fno-slp-vectorize to measure the non-vectorized kernels.
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
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
void shift_or_bounded(u64 *bits, int capacity, int &hi, int weight) {
    require(weight >= 0, "negative subset-sum weight");
    if (weight > capacity)
        return;

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

bool bit_is_set(const u64 *bits, int position) {
    return (bits[position / 64] >> (position % 64)) & 1;
}

// The scalar DP can retain one predecessor for each newly reached sum.
bool recover_subset_scalar(const int *weight, int n, int target,
                           std::vector<int> &picked) {
    std::vector<int> previous(target + 1, -1);
    std::vector<int> chosen(target + 1, -1);
    previous[0] = 0;
    int hi = 0;

    for (int i = 0; i < n; i++) {
        int w = weight[i];
        require(w >= 0, "negative subset-sum weight");
        if (w == 0 || w > target)
            continue;

        int next_hi = (hi > target - w ? target : hi + w);
        for (int s = next_hi; s >= w; s--) {
            if (previous[s] == -1 && previous[s - w] != -1) {
                previous[s] = s - w;
                chosen[s] = i;
            }
        }
        hi = next_hi;
    }

    picked.clear();
    if (previous[target] == -1)
        return false;

    for (int s = target; s != 0; s = previous[s]) {
        require(chosen[s] >= 0, "broken recovery chain");
        picked.push_back(chosen[s]);
    }
    std::reverse(picked.begin(), picked.end());
    return true;
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
    std::vector<u64> full(word_count(capacity));
    std::vector<u64> bounded(word_count(capacity));
    subset_sum_scalar(weight.data(), (int) weight.size(), capacity,
                      scalar.data());
    subset_sum_words_full(weight.data(), (int) weight.size(), capacity,
                          full.data());
    subset_sum_words_bounded(weight.data(), (int) weight.size(), capacity,
                             bounded.data());

    for (int s = 0; s <= capacity; s++) {
        require(bit_is_set(full.data(), s) == bool(scalar[s]),
                "full word DP disagrees with scalar DP");
        require(bit_is_set(bounded.data(), s) == bool(scalar[s]),
                "bounded word DP disagrees with scalar DP");
    }
    require((full.back() & ~last_word_mask(capacity)) == 0,
            "full word DP leaked bits past capacity");
    require((bounded.back() & ~last_word_mask(capacity)) == 0,
            "bounded word DP leaked bits past capacity");

    if (compare_brute) {
        std::vector<unsigned char> brute = brute_subset(weight, capacity);
        require(brute == scalar, "scalar subset DP disagrees with brute force");
    }
}

void check_recovery(const std::vector<int> &weight, int target,
                    bool expected) {
    std::vector<int> picked;
    bool found = recover_subset_scalar(weight.data(), (int) weight.size(),
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

    for (int test = 0; test < 5000; test++) {
        int target = int(rng() % 501);
        int n = int(rng() % 51);
        std::vector<int> weight(n);
        for (int &w : weight)
            w = int(rng() % 630);
        std::vector<unsigned char> reachable(target + 1);
        subset_sum_scalar(weight.data(), n, target, reachable.data());
        check_recovery(weight, target, reachable[target]);
    }

    for (int target = 0; target <= 512; target++) {
        std::vector<unsigned char> reachable(target + 1);
        subset_sum_scalar(edges.data(), (int) edges.size(), target,
                          reachable.data());
        check_recovery(edges, target, reachable[target]);
    }
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
    test_subset_sum(rng);
    test_value_knapsack(rng);
    std::puts("all knapsack/subset-sum tests passed");
}

volatile u64 benchmark_sink = 0;

double median(std::vector<double> sample) {
    std::sort(sample.begin(), sample.end());
    return sample[sample.size() / 2];
}

double time_scalar(const std::vector<int> &weight, int capacity, int repeats) {
    std::vector<unsigned char> reachable(capacity + 1);
    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        subset_sum_scalar(weight.data(), (int) weight.size(), capacity,
                          reachable.data());
        auto stop = std::chrono::steady_clock::now();
        benchmark_sink ^= reachable[capacity] + reachable[capacity / 2];
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(sample);
}

double time_words_full(const std::vector<int> &weight, int capacity, int repeats) {
    std::vector<u64> bits(word_count(capacity));
    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        subset_sum_words_full(weight.data(), (int) weight.size(), capacity,
                              bits.data());
        auto stop = std::chrono::steady_clock::now();
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    return median(sample);
}

double time_words_bounded(const std::vector<int> &weight, int capacity,
                          int repeats) {
    std::vector<u64> bits(word_count(capacity));
    std::vector<double> sample;
    for (int iteration = -2; iteration < repeats; iteration++) {
        auto start = std::chrono::steady_clock::now();
        subset_sum_words_bounded(weight.data(), (int) weight.size(), capacity,
                                 bits.data());
        auto stop = std::chrono::steady_clock::now();
        benchmark_sink ^= bit_is_set(bits.data(), capacity)
                        + bit_is_set(bits.data(), capacity / 2);
        if (iteration >= 0)
            sample.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
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

int main(int argc, char **argv) {
    if (argc == 1 || std::string(argv[1]) == "test") {
        run_tests();
        return 0;
    }
    if (std::string(argv[1]) == "bench") {
        run_benchmarks();
        return 0;
    }
    std::fprintf(stderr, "usage: %s [test|bench]\n", argv[0]);
    return 2;
}
