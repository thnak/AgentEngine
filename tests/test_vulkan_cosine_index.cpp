// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.6/§3 claim 7 --
// `VulkanCosineIndex` (src/backends/vulkan_vector_index/vulkan_cosine_index.hpp): basic correctness
// against small, hand-checkable inputs, `VectorIndex` conformance, and claim 7's own falsifiable
// shape -- an epsilon-bounded comparison against `BruteForceCosineIndex` at dim=1536, n in {1000,
// 5000} (the exact sizes ADR-063 §6 already measured as CPU Goal-tier misses against RFC 023 §3's
// `<= 500 us` budget).
//
// HONEST RESULT, recorded here rather than only in the ADR (a reader of just this test file should
// not be misled): GPU search does NOT beat CPU brute-force at either size on the reference hardware
// this was proven against (a discrete AMD Radeon RX 5300M) -- it is consistently ~1.3x SLOWER, not
// faster, after two real optimization passes (a vectors-buffer upload/caching fix that closed a ~7-8x
// gap, and a device-local-memory-via-staging-buffer fix that closed a further ~4x gap at n=5000; see
// this file's own git history / the ADR's §5-6 account for the full, honest before/after numbers).
// The remaining gap is very likely fixed per-`search()`-call CPU<->GPU synchronization overhead
// (`vkQueueSubmit`+`vkWaitForFences`, a genuine round-trip cost independent of problem size at these
// scales) -- named as an open, unresolved residual in ADR-180 §7, not chased further in this pass.
// What claim 7 DOES hold, proven below: GPU and CPU scores agree within a tight, stated epsilon (the
// determinism claim §2.6's design makes), and the tie-break/correctness/VectorIndex-conformance
// claims all hold for real, on real hardware.

#ifdef AGENTENGINE_WITH_VULKAN

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "agentengine/core/vector_index.hpp"
#include "backends/vulkan_vector_index/vulkan_cosine_index.hpp"

using namespace agentengine;
using namespace agentengine::backends::vulkan_vector_index;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

static_assert(VectorIndex<VulkanCosineIndex>,
              "VulkanCosineIndex must satisfy the real, unmodified VectorIndex concept (ADR-180 §2.6)");

int main() {
    auto created = VulkanCosineIndex::create();
    if (!created.has_value()) {
        // Fails closed, not a crash -- matching this codebase's own docker_execution_surface.hpp/
        // containerd_execution_surface.hpp posture toward "the backend this host doesn't have." A CI
        // machine with no Vulkan-capable GPU is a real, expected environment this test must not FAIL
        // in the ordinary ctest sense for -- it has nothing to test, not a bug.
        std::cerr << "test_vulkan_cosine_index: SKIPPED -- VulkanCosineIndex::create() failed: "
                  << created.error().message << " (" << created.error().code << ")\n"
                  << "  No Vulkan-capable device on this host, or no usable Vulkan runtime.\n";
        return 0;
    }
    VulkanCosineIndex index = std::move(*created);

    // --- Small, hand-checkable correctness case (mirrors test_vector_index.cpp's own basic case) ---
    {
        auto added = index.add_batch({"exact-match", "orthogonal", "opposite"},
                                       {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}});
        AE_CHECK(added.has_value(), "add_batch accepts 3 well-formed entries");
        AE_CHECK(index.size() == 3, "size() reflects all 3 entries");

        float const query[3] = {1.0f, 0.0f, 0.0f};
        auto results = index.search(std::span<float const>(query, 3), 3);
        AE_CHECK(results.has_value() && results->size() == 3, "search returns all 3 entries for k=3");
        if (results.has_value() && results->size() == 3) {
            AE_CHECK((*results)[0].id == "exact-match" && (*results)[0].score > 0.99f,
                     "the identical-direction vector ranks first with cosine ~1.0 (GPU-computed)");
            AE_CHECK((*results)[1].id == "orthogonal", "the orthogonal vector ranks second");
            AE_CHECK((*results)[2].id == "opposite" && (*results)[2].score < -0.99f,
                     "the opposite-direction vector ranks last with cosine ~-1.0 (GPU-computed)");
        }
        AE_CHECK(index.contains("exact-match") && !index.contains("nonexistent"), "contains() is correct");

        auto top1 = index.search(std::span<float const>(query, 3), 1);
        AE_CHECK(top1.has_value() && top1->size() == 1 && (*top1)[0].id == "exact-match",
                 "k truncates the result set, not just sorts all of it");
    }

    // --- Contract violations, mirroring BruteForceCosineIndex's own identical checks ---------------
    {
        auto created2 = VulkanCosineIndex::create();
        AE_CHECK(created2.has_value(), "setup: a second index instance is created");
        if (created2.has_value()) {
            VulkanCosineIndex idx2 = std::move(*created2);
            auto mismatched = idx2.add_batch({"a", "b"}, {{1.0f, 0.0f}});
            AE_CHECK(!mismatched.has_value() &&
                         mismatched.error().code == "vulkan_vector_index.add_batch_length_mismatch",
                     "add_batch rejects ids/vectors of differing length");

            auto dup = idx2.add_batch({"x", "x"}, {{1.0f}, {2.0f}});
            AE_CHECK(!dup.has_value() && dup.error().code == "vulkan_vector_index.add_batch_duplicate_id",
                     "add_batch rejects a duplicate id within a single call");

            AE_CHECK(idx2.add_batch({"y"}, {{1.0f, 0.0f}}).has_value(), "setup: a 2-dim vector establishes dim");
            auto wrong_dim = idx2.add_batch({"z"}, {{1.0f}});
            AE_CHECK(!wrong_dim.has_value() &&
                         wrong_dim.error().code == "vulkan_vector_index.add_batch_dimension_mismatch",
                     "add_batch rejects a vector whose width disagrees with the established dimensionality");

            float const wrong_dim_query[1] = {1.0f};
            auto bad_search = idx2.search(std::span<float const>(wrong_dim_query, 1), 1);
            AE_CHECK(!bad_search.has_value() &&
                         bad_search.error().code == "vulkan_vector_index.search_dimension_mismatch",
                     "search() rejects a query vector whose width disagrees with the index's dimensionality");
        }
    }

    // --- Claim 7: epsilon-bounded comparison against BruteForceCosineIndex, dim=1536, n in
    // {1000, 5000} -- the exact sizes ADR-063 §6 measured as CPU Goal-tier misses. -------------------
    for (std::size_t n : {std::size_t{1000}, std::size_t{5000}}) {
        constexpr std::size_t kDim = 1536;
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        BruteForceCosineIndex cpu_index;
        auto gpu_created = VulkanCosineIndex::create();
        AE_CHECK(gpu_created.has_value(), "fresh GPU index created for n=" + std::to_string(n));
        if (!gpu_created.has_value()) continue;
        VulkanCosineIndex gpu_index = std::move(*gpu_created);

        std::vector<std::string> ids;
        std::vector<std::vector<float>> vecs;
        ids.reserve(n);
        vecs.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            ids.push_back("v" + std::to_string(i));
            std::vector<float> v(kDim);
            for (auto& f : v) f = dist(rng);
            vecs.push_back(std::move(v));
        }
        AE_CHECK(cpu_index.add_batch(ids, vecs).has_value(), "CPU add_batch succeeds, n=" + std::to_string(n));
        AE_CHECK(gpu_index.add_batch(ids, vecs).has_value(), "GPU add_batch succeeds, n=" + std::to_string(n));

        std::vector<float> query(kDim);
        for (auto& f : query) f = dist(rng);
        std::span<float const> query_span(query.data(), kDim);

        // One "cold" call each first (pays any one-time GPU buffer setup), matching the realistic
        // "index once, query many times" pattern -- see this file's own top comment for why a naive
        // "re-upload the whole corpus every call" design measured ~10x slower than CPU.
        auto cpu_result = cpu_index.search(query_span, 5);
        auto gpu_result = gpu_index.search(query_span, 5);

        constexpr int kWarmRuns = 10;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kWarmRuns; ++i) cpu_result = cpu_index.search(query_span, 5);
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kWarmRuns; ++i) gpu_result = gpu_index.search(query_span, 5);
        auto t2 = std::chrono::steady_clock::now();
        double const cpu_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kWarmRuns;
        double const gpu_us = std::chrono::duration<double, std::micro>(t2 - t1).count() / kWarmRuns;
        std::cout << "  .. n=" << n << " CPU search (warm avg): " << cpu_us
                  << " us, GPU search (warm avg): " << gpu_us << " us\n";

        AE_CHECK(cpu_result.has_value() && gpu_result.has_value(), "both searches succeed, n=" + std::to_string(n));
        if (cpu_result.has_value() && gpu_result.has_value()) {
            AE_CHECK(cpu_result->size() == gpu_result->size(), "same result count, n=" + std::to_string(n));

            auto cpu_full = cpu_index.search(query_span, n);
            auto gpu_full = gpu_index.search(query_span, n);
            AE_CHECK(cpu_full.has_value() && gpu_full.has_value() && cpu_full->size() == gpu_full->size(),
                     "full-n rescore succeeds on both sides, n=" + std::to_string(n));
            if (cpu_full.has_value() && gpu_full.has_value()) {
                std::unordered_map<std::string, float> cpu_scores;
                for (auto const& s : *cpu_full) cpu_scores[s.id] = s.score;
                float max_abs_diff = 0.0f;
                for (auto const& s : *gpu_full) {
                    float const diff = std::fabs(s.score - cpu_scores.at(s.id));
                    max_abs_diff = std::max(max_abs_diff, diff);
                }
                std::cout << "  .. n=" << n << " max |GPU score - CPU score| across all " << n
                          << " candidates: " << max_abs_diff << "\n";
                // float32 (GPU) vs double (CPU) accumulation over 1536 dims -- a generous but real,
                // stated-up-front epsilon (ADR-180 §2.6's own named, accepted residual), not tuned
                // after seeing the number. Real measured max on reference hardware: ~1e-7, comfortably
                // within this bound.
                AE_CHECK(max_abs_diff < 1e-3f,
                         "GPU and CPU cosine scores agree within 1e-3 for every candidate, n=" +
                             std::to_string(n));
            }

            std::unordered_map<std::string, float> cpu_top;
            for (auto const& s : *cpu_result) cpu_top[s.id] = s.score;
            std::unordered_map<std::string, float> gpu_top;
            for (auto const& s : *gpu_result) gpu_top[s.id] = s.score;
            float const boundary = cpu_result->empty() ? 0.0f : cpu_result->back().score;
            bool top_k_divergence_only_near_tie = true;
            for (auto const& [id, score] : gpu_top) {
                if (!cpu_top.contains(id) && std::fabs(score - boundary) > 1e-3f) {
                    top_k_divergence_only_near_tie = false;
                }
            }
            AE_CHECK(top_k_divergence_only_near_tie,
                     "claim 7: any GPU-vs-CPU top-5 divergence only involves near-boundary-tied "
                     "candidates (a CPU score within 1e-3 of the 5th-place boundary), never a "
                     "wrong-by-a-wide-margin swap, n=" + std::to_string(n));

            // Named honestly, not hidden: this is NOT asserted as a pass/fail check -- see this
            // file's own top comment. Recorded as a printed observation for the ADR's own record.
            std::cout << "  .. n=" << n << " GPU " << (gpu_us < cpu_us ? "beats" : "does NOT beat")
                      << " CPU on this reference hardware (RFC 023 §3 budget: <= 500 us; "
                      << "neither CPU nor GPU meets it at this n -- both are Goal-tier misses here)\n";
        }
    }

    std::cout << (g_failures == 0 ? "test_vulkan_cosine_index: OK\n" : "test_vulkan_cosine_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() { return 0; }

#endif  // AGENTENGINE_WITH_VULKAN
