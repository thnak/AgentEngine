// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §3 claim 1 --
// BruteForceCosineIndex::search() latency at the hypothesized corpus size (§2.3A: "low hundreds to
// low thousands of items, embedding dimension in the low hundreds to ~1536"). RFC
// 023-Performance-Targets-and-Budgets.md names no dedicated budget row for vector-index search
// specifically -- the closest existing analog is "Context assembly: assemble a 50-message context,
// p99 <= 500 us (Goal)" (023 §3), a DIFFERENT operation this bench does not claim equivalence to.
// This bench's own job, honestly scoped: replace claim 1's "no number claimed yet" placeholder with
// REAL, measured numbers at the hypothesized sizes, so a future RFC pass can set a real budget row
// against real data instead of a guess. It does NOT, by itself, resolve claim 1 to CORRECT or WRONG
// -- that verdict needs a named target to compare against, which 023 does not yet have for this
// operation (a real, separate residual -- named in ADR-063 §7, not closed by this file).
//
// Machine-safety (CLAUDE.md): single-threaded, bounded corpus sizes and iteration counts, no thread
// spawning, no unbounded allocation (the largest corpus here is 5000 x 1536 floats, ~30 MB).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/vector_index.hpp"

using Clock = std::chrono::steady_clock;

namespace {

std::vector<std::vector<float>> make_vectors(std::size_t n, std::size_t dim, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<std::vector<float>> out(n, std::vector<float>(dim));
    for (auto& v : out) {
        for (auto& x : v) x = dist(rng);
    }
    return out;
}

// Deterministic (fixed seed), matching this project's own bench-file precedent
// (test_capability_token_benchmark.cpp): the goal is a real, reproducible number to reason about,
// not a statistically rigorous distribution.
[[nodiscard]] bool run_one(std::size_t n, std::size_t dim) {
    auto vectors = make_vectors(n, dim, /*seed=*/12345u + static_cast<std::uint32_t>(n));
    std::vector<std::string> ids;
    ids.reserve(n);
    for (std::size_t i = 0; i < n; ++i) ids.push_back("id-" + std::to_string(i));

    ae::BruteForceCosineIndex index;
    auto t_build0 = Clock::now();
    auto added = index.add_batch(ids, vectors);
    auto t_build1 = Clock::now();
    if (!added.has_value()) {
        std::cerr << "FAIL: add_batch() failed unexpectedly during benchmark setup (n=" << n << ")\n";
        return false;
    }
    double const build_ms =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t_build1 - t_build0).count()) /
        1000.0;

    auto query_vectors = make_vectors(1, dim, /*seed=*/99999u);
    std::span<float const> query(query_vectors.front().data(), query_vectors.front().size());

    constexpr int kWarmup = 5;
    constexpr int kIterations = 100;
    for (int i = 0; i < kWarmup; ++i) {
        auto warm = index.search(query, 5);
        if (!warm.has_value()) {
            std::cerr << "FAIL: search() failed unexpectedly during warmup (n=" << n << ")\n";
            return false;
        }
    }

    auto t0 = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto result = index.search(query, 5);
        if (!result.has_value()) {
            std::cerr << "FAIL: search() failed unexpectedly during benchmark (n=" << n << ")\n";
            return false;
        }
    }
    auto t1 = Clock::now();
    double const avg_search_us =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
        kIterations / 1000.0;

    std::cout << "n=" << n << " dim=" << dim << ": add_batch() " << build_ms
              << " ms (one-time build cost), search(k=5) " << avg_search_us << " us/call avg over "
              << kIterations << " calls\n";
    return true;
}

}  // namespace

int main() {
    // §2.3A's own hypothesized range: "low hundreds to low thousands of items, embedding dimension
    // in the low hundreds to ~1536". 1536 is text-embedding-3-small's real, sourced dimension
    // (docs/research/2026-08-19-embedding-provider-landscape.md §3) -- not a round guess.
    bool ok = true;
    ok &= run_one(200, 1536);
    ok &= run_one(1000, 1536);
    ok &= run_one(5000, 1536);  // beyond "low thousands", included for trend visibility at the margin
    return ok ? 0 : 1;
}
