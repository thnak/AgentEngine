// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.6/§3 claim 7 --
// `VulkanCosineIndex` (src/backends/vulkan_vector_index/vulkan_cosine_index.hpp): basic correctness
// against small, hand-checkable inputs, `VectorIndex` conformance, and claim 7's own falsifiable
// shape -- an epsilon-bounded comparison against `BruteForceCosineIndex` at dim=1536, n in {1000,
// 5000} (the exact sizes ADR-063 §6 already measured as CPU Goal-tier misses against RFC 023 §3's
// `<= 500 us` budget).
//
// PERFORMANCE, recorded here rather than only in the ADR: the first shipped version measured GPU
// search ~1.3x SLOWER than CPU brute force on the reference hardware (a discrete AMD Radeon RX 5300M),
// and this comment used to guess the cause was fixed per-call submit/wait overhead. That guess was
// WRONG. A per-phase profile (ADR-180 §10, 2026-09-23) showed the kernel itself was the cost: the
// row-major vectors layout left adjacent GPU lanes 6 KB apart, so no read was ever coalesced.
// Switching to a dimension-major layout plus top-k selection instead of a full sort made GPU search
// ~5x faster than CPU at n=5000 and ~15x at n=20000. A fixed ~1.3-1.6 ms submit/wait floor per call
// is real, which puts the crossover near n=1000 on this machine. Speed is still printed, not asserted
// (timing on shared hardware is not a stable pass/fail signal). What IS asserted: GPU and CPU scores
// agree within a tight, stated epsilon (the determinism claim §2.6's design makes), the layout is
// correct across batch growth and non-multiple-of-64 sizes, and the tie-break/correctness/
// VectorIndex-conformance claims all hold for real, on real hardware.

#ifdef AGENTENGINE_WITH_VULKAN

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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
    // --- Red-team pass 3 (2026-09-22) finding 3: `detail::check_gpu_dispatch_size_plausible()` is a
    // pure, Vulkan-free predicate -- runs even on a machine with no Vulkan-capable device at all
    // (unlike every other block below), since it needs no real GPU resource, only synthetic size_t
    // values standing in for a candidate count / dimensionality this backend's uint32 push constants
    // cannot represent without silently wrapping. ---------------------------------------------------
    {
        auto ok_small = detail::check_gpu_dispatch_size_plausible(5000, 1536);
        AE_CHECK(ok_small.has_value(), "dispatch-size guard accepts a realistic n/dim pair");

        auto over_n = detail::check_gpu_dispatch_size_plausible(
            std::size_t{std::numeric_limits<std::uint32_t>::max()} + 1, 8);
        AE_CHECK(!over_n.has_value() &&
                     over_n.error().code == "vulkan_vector_index.dispatch_size_exceeds_uint32",
                 "dispatch-size guard rejects a candidate count beyond uint32 range");

        auto over_dim = detail::check_gpu_dispatch_size_plausible(
            8, std::size_t{std::numeric_limits<std::uint32_t>::max()} + 1);
        AE_CHECK(!over_dim.has_value() &&
                     over_dim.error().code == "vulkan_vector_index.dispatch_size_exceeds_uint32",
                 "dispatch-size guard rejects a dimensionality beyond uint32 range");

        // dim == 0 must not divide-by-zero in the second (multiplication-overflow) guard -- reachable
        // in principle via a not-yet-populated index, even though search() itself never calls this
        // helper with dim == 0 today (the zero-dimension invariant check above it already excludes
        // that). Tested here anyway since this is a general-purpose predicate, not a hidden detail
        // of that one call site.
        auto zero_dim = detail::check_gpu_dispatch_size_plausible(5000, 0);
        AE_CHECK(zero_dim.has_value(), "dispatch-size guard does not divide by zero for dim == 0");

        // NOTE: the largest n*dim product representable by two in-range uint32 values
        // (UINT32_MAX * UINT32_MAX =~ 1.8446744e19) is itself just BELOW SIZE_MAX on a 64-bit
        // size_t (~1.8446744e19 + a margin) -- so the uint32 ceiling above already makes the
        // second, independent size_t-multiplication-overflow guard structurally unreachable in
        // practice from any pair that passes it. It is kept anyway as a second, independent layer
        // (defense-in-depth, not merely inferred safe from the first check alone) -- named here
        // honestly rather than claiming a test exercises a path that cannot actually be reached.
    }

    // --- Red-team pass 5 (2026-09-23), Real gap: the DEVICE's own limits, not just uint32
    // representability. Pure predicate, synthetic limits -- runs with or without a GPU. The 2^27
    // figure is the Vulkan spec's required minimum maxStorageBufferRange, and the exact value the
    // Vulkan SDK's own VP_ANDROID_vulkan_profile_2021 baseline profile declares.
    {
        constexpr std::uint64_t kSpecMinStorageRange = std::uint64_t{1} << 27;
        constexpr std::uint32_t kSpecMinWorkgroups = 65535;
        std::size_t const dim = 1536;
        std::size_t const n_fits = kSpecMinStorageRange / (dim * sizeof(float));  // 21845
        AE_CHECK(detail::check_gpu_device_limits(n_fits, dim, kSpecMinStorageRange, kSpecMinWorkgroups).has_value(),
                 "device-limit guard accepts the largest corpus whose vectors buffer fits a 2^27-byte "
                 "maxStorageBufferRange exactly (n=21845, dim=1536)");
        auto over = detail::check_gpu_device_limits(n_fits + 1, dim, kSpecMinStorageRange, kSpecMinWorkgroups);
        AE_CHECK(!over.has_value() && over.error().code == "vulkan_vector_index.exceeds_device_storage_buffer_range" &&
                     over.error().klass == failure_class::resource,
                 "device-limit guard rejects n=21846 x dim=1536 (one vector over a 2^27-byte "
                 "maxStorageBufferRange) as a resource limit, before any descriptor write -- previously an "
                 "invalid descriptor range (VUID-VkWriteDescriptorSet-descriptorType-00333) on such a device");
        auto groups = detail::check_gpu_device_limits(std::size_t{64} * 65536, 1, std::uint64_t{1} << 40,
                                                      kSpecMinWorkgroups);
        AE_CHECK(!groups.has_value() && groups.error().code == "vulkan_vector_index.exceeds_device_workgroup_count",
                 "device-limit guard rejects a dispatch needing 65536 workgroups on a device offering the "
                 "spec-minimum 65535 (closes red-team pass 3's named maxComputeWorkGroupCount residual)");
        AE_CHECK(detail::check_gpu_device_limits(std::size_t{64} * 65535, 1, std::uint64_t{1} << 40,
                                                 kSpecMinWorkgroups)
                     .has_value(),
                 "device-limit guard accepts exactly 65535 workgroups");
        // The shader's own `uint base = i * pc.dimension` cannot wrap once the range bound holds:
        // maxStorageBufferRange is itself a uint32, so the largest accepted n*dim is < 2^30.
        std::uint64_t const max_u32_range = (std::numeric_limits<std::uint32_t>::max)();
        AE_CHECK(!detail::check_gpu_device_limits(std::size_t{1} << 20, std::size_t{1} << 10, max_u32_range,
                                                  (std::numeric_limits<std::uint32_t>::max)())
                      .has_value(),
                 "n*dim = 2^30 (4 GiB of floats) is rejected even against the largest representable "
                 "maxStorageBufferRange -- so the shader's uint32 index arithmetic can never wrap");
    }

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

            // Red-team pass 3 finding 5 (Real gap): the CODE already checks
            // `impl_->entries.contains(id)` for a duplicate ACROSS separate add_batch() calls (not
            // just within one call), mirroring BruteForceCosineIndex::add_batch() exactly -- but no
            // test had ever exercised that specific branch for the GPU conformer. "y" was already
            // committed by idx2's earlier add_batch({"y"}, ...) above.
            auto cross_call_dup = idx2.add_batch({"y"}, {{9.0f, 9.0f}});
            AE_CHECK(!cross_call_dup.has_value() &&
                         cross_call_dup.error().code == "vulkan_vector_index.add_batch_duplicate_id",
                     "add_batch rejects an id that was already committed by an EARLIER, separate "
                     "add_batch call (not just a duplicate within one call)");
        }
    }

    // --- Red-team pass 4 (2026-09-22) finding 3 (Real gap): a zero-dimensional vector used to
    // establish `impl_->dimension == 0` on a non-empty index, which search() then classified as
    // `failure_class::fatal` ("internal invariant violated") -- the wrong severity for reachable,
    // caller-supplied degenerate input, and a code path this test file never exercised. add_batch()
    // now rejects it outright, at its actual origin, as an ordinary `contract` violation. ------------
    {
        auto created_zd = VulkanCosineIndex::create();
        AE_CHECK(created_zd.has_value(), "setup: a fifth index instance is created for the "
                                          "zero-dimension-vector rejection case");
        if (created_zd.has_value()) {
            VulkanCosineIndex idx_zd = std::move(*created_zd);
            auto zero_dim_first = idx_zd.add_batch({"z0"}, {std::vector<float>{}});
            AE_CHECK(!zero_dim_first.has_value() &&
                         zero_dim_first.error().code == "vulkan_vector_index.add_batch_zero_dimension",
                     "add_batch rejects a zero-dimensional vector when it would establish the index's "
                     "dimensionality (no prior add_batch call), classified as an ordinary contract "
                     "violation, not a fatal internal-invariant error");
            AE_CHECK(idx_zd.size() == 0,
                     "the rejected zero-dimension add_batch call left the index untouched");

            // The SAME zero-sized-vector input, but AFTER a real dimensionality is already
            // established, must fall through to the pre-existing, differently-coded dimension-
            // mismatch rejection instead -- confirms the new check does not shadow or change that
            // already-covered path.
            AE_CHECK(idx_zd.add_batch({"real"}, {{1.0f, 2.0f}}).has_value(),
                     "setup: idx_zd's dimensionality is established at 2 by a real vector");
            auto zero_dim_after = idx_zd.add_batch({"z1"}, {std::vector<float>{}});
            AE_CHECK(!zero_dim_after.has_value() &&
                         zero_dim_after.error().code == "vulkan_vector_index.add_batch_dimension_mismatch",
                     "a zero-dimensional vector added AFTER dimensionality is already established "
                     "falls through to the pre-existing dimension-mismatch rejection, not the new "
                     "zero-dimension-establishing one");

            // A fully empty add_batch() call (ids=[], vectors=[]) is a legitimate no-op, distinct from
            // the zero-dimensional-VECTOR case above -- must NOT be rejected by the new check.
            AE_CHECK(idx_zd.add_batch({}, {}).has_value(),
                     "add_batch with fully empty ids/vectors (a genuine no-op) is still accepted, not "
                     "confused with a zero-dimensional vector");
        }
    }

    // --- Red-team pass 4 (2026-09-22): adversarial/boundary inputs beyond §4c's own coverage -- a
    // single-vector (n=1), single-dimension (dim=1) corpus, k exactly equal to corpus size, and a
    // back-to-back no-op add_batch() (empty ids/vectors) sandwiched between two real ones on the same
    // instance (distinct from the growth-cycle test above, which only ever adds real entries). -------
    {
        auto created_edge = VulkanCosineIndex::create();
        AE_CHECK(created_edge.has_value(), "setup: a sixth index instance is created for n=1/dim=1 "
                                            "boundary coverage");
        if (created_edge.has_value()) {
            VulkanCosineIndex idx_edge = std::move(*created_edge);

            AE_CHECK(idx_edge.add_batch({}, {}).has_value(),
                     "a no-op add_batch (empty ids/vectors) on a fresh, never-populated index succeeds "
                     "and establishes nothing");
            AE_CHECK(idx_edge.size() == 0, "the no-op add_batch left the index empty");

            AE_CHECK(idx_edge.add_batch({"only"}, {{3.0f}}).has_value(),
                     "add_batch accepts a single dim=1 vector, establishing an n=1 corpus");
            AE_CHECK(idx_edge.size() == 1, "size() reflects the single n=1 entry");

            // A second no-op add_batch() call AFTER real data exists -- distinct from the
            // never-populated case above, and distinct from every "real growth" cycle the earlier
            // three-cycle test already covers: this call changes nothing, but the implementation
            // still marks the GPU vectors-buffer cache dirty (an accepted, unbenched, performance-only
            // cost, not a correctness bug -- named here rather than silently exercised and ignored),
            // and the NEXT search() must still return the single, unchanged, correct entry.
            AE_CHECK(idx_edge.add_batch({}, {}).has_value(),
                     "a second no-op add_batch, now with real data already present, also succeeds");

            float const q1[1] = {3.0f};
            auto r_n1 = idx_edge.search(std::span<float const>(q1, 1), 1);
            AE_CHECK(r_n1.has_value() && r_n1->size() == 1 && (*r_n1)[0].id == "only" &&
                         (*r_n1)[0].score > 0.99f,
                     "search() on an n=1/dim=1 corpus (k=1, exactly equal to corpus size) returns the "
                     "single entry, unaffected by the intervening no-op add_batch calls");

            auto r_k_over_one = idx_edge.search(std::span<float const>(q1, 1), 5);
            AE_CHECK(r_k_over_one.has_value() && r_k_over_one->size() == 1,
                     "search(k > 1) on an n=1 corpus still returns exactly the 1 entry present, not an "
                     "error and not padded");
        }
    }

    // --- Red-team pass 3 finding 5 (Real gap): VectorIndex contract behaviors BruteForceCosineIndex's
    // own test suite exercises that this conformer's test never had -- empty-index search, k == 0,
    // and k > corpus size. Implementation already mirrors BruteForceCosineIndex's identical logic for
    // all three (confirmed by code reading); this closes the coverage gap so that claim is proven, not
    // merely asserted. ------------------------------------------------------------------------------
    {
        auto created3 = VulkanCosineIndex::create();
        AE_CHECK(created3.has_value(), "setup: a third, never-populated index instance is created");
        if (created3.has_value()) {
            VulkanCosineIndex idx3 = std::move(*created3);

            float const any_query[2] = {1.0f, 0.0f};
            auto empty_search = idx3.search(std::span<float const>(any_query, 2), 5);
            AE_CHECK(empty_search.has_value() && empty_search->empty(),
                     "search() on a never-populated (empty) index succeeds with an empty result, "
                     "not an error");

            AE_CHECK(idx3.add_batch({"p", "q", "r"}, {{1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}})
                         .has_value(),
                     "setup: idx3 populated with 3 entries");

            auto k_zero = idx3.search(std::span<float const>(any_query, 2), 0);
            AE_CHECK(k_zero.has_value() && k_zero->empty(),
                     "search(k=0) succeeds with an empty result, not an error");

            auto k_over = idx3.search(std::span<float const>(any_query, 2), 100);
            AE_CHECK(k_over.has_value() && k_over->size() == 3,
                     "search(k > corpus size) returns every entry, not an error, and does not pad "
                     "with placeholders");

            // Red-team pass 4 (2026-09-22): k exactly equal to corpus size, distinct from both k == 0
            // and k > corpus size above -- the boundary itself, not just either side of it.
            auto k_exact = idx3.search(std::span<float const>(any_query, 2), 3);
            AE_CHECK(k_exact.has_value() && k_exact->size() == 3,
                     "search(k == corpus size exactly) returns every entry, the same as k > corpus "
                     "size, not truncated to fewer");
        }
    }

    // --- Red-team pass 3 finding 1 (Critical, partially closed): the persistent GPU vectors-buffer
    // cache (Impl's own field comment: "rebuilt only when the corpus actually changes") destroys the
    // OLD device-local buffer/memory only AFTER the new one is fully built and uploaded, right before
    // reassigning `impl_->vectors_buffer`/`vectors_memory` -- exercised here across THREE separate
    // add_batch()-then-search() cycles on the SAME instance (the existing test suite only ever
    // populated an instance once before searching it, so the cache-rebuild-on-GROWTH path, and
    // specifically the old-buffer-destroy-then-reassign sequence, had never actually run more than
    // once). Correctness (each search reflects the FULL, current corpus, not a stale snapshot) is the
    // behavioral proof available here; true leak-freedom across repeated rebuilds is additionally
    // supported by this file's own AddressSanitizer run (this whole binary, including this block, run
    // clean under /fsanitize=address -- see the ADR's own account for the caveat that Windows ASan has
    // no LeakSanitizer, so this is a host-side-corruption check, not an automated leak-detector run).
    {
        auto created4 = VulkanCosineIndex::create();
        AE_CHECK(created4.has_value(), "setup: a fourth index instance is created for repeated-growth "
                                        "cache-rebuild coverage");
        if (created4.has_value()) {
            VulkanCosineIndex idx4 = std::move(*created4);
            float const q[2] = {1.0f, 0.0f};

            AE_CHECK(idx4.add_batch({"g0"}, {{1.0f, 0.0f}}).has_value(), "growth cycle 1: add_batch");
            auto r0 = idx4.search(std::span<float const>(q, 2), 10);
            AE_CHECK(r0.has_value() && r0->size() == 1,
                     "growth cycle 1: search reflects the 1 entry added so far (first cache build)");

            AE_CHECK(idx4.add_batch({"g1", "g2"}, {{0.0f, 1.0f}, {-1.0f, 0.0f}}).has_value(),
                     "growth cycle 2: add_batch (triggers cache_dirty, forcing a cache REBUILD)");
            auto r1 = idx4.search(std::span<float const>(q, 2), 10);
            AE_CHECK(r1.has_value() && r1->size() == 3,
                     "growth cycle 2: search reflects all 3 entries -- the rebuilt buffer is NOT a "
                     "stale snapshot from cycle 1");

            AE_CHECK(idx4.add_batch({"g3"}, {{0.5f, 0.5f}}).has_value(),
                     "growth cycle 3: add_batch (a SECOND cache rebuild on the same instance)");
            auto r2 = idx4.search(std::span<float const>(q, 2), 10);
            AE_CHECK(r2.has_value() && r2->size() == 4,
                     "growth cycle 3: search reflects all 4 entries after a second consecutive "
                     "cache-rebuild cycle on the same instance");
            if (r2.has_value()) {
                bool has_all = true;
                for (auto const& id : {"g0", "g1", "g2", "g3"}) {
                    bool found = false;
                    for (auto const& s : *r2) found = found || (s.id == id);
                    has_all = has_all && found;
                }
                AE_CHECK(has_all, "growth cycle 3: every id ever added is present, none dropped or "
                                   "duplicated across repeated cache rebuilds");
            }
        }
    }

    // --- Red-team pass 5 (2026-09-23), Real gap: finite inputs outside float32's squared range. The
    // shader used to square RAW components in float32: {1e20, 1e20} (|x|^2 = 1e40 > FLT_MAX) scored 0
    // against query {1, 1} (CPU, in double: 1.0) and ranked LAST; {1e-25, 1e-25} underflowed and
    // scored 0 too; a {1e20, 1e20} query produced NaN. Now every row and the query are rescaled by an
    // exact power of two before upload, so the GPU agrees with BruteForceCosineIndex. -------------------
    {
        auto created_r = VulkanCosineIndex::create();
        AE_CHECK(created_r.has_value(), "setup: an index instance for the magnitude-range case");
        if (created_r.has_value()) {
            VulkanCosineIndex gpu = std::move(*created_r);
            BruteForceCosineIndex cpu;
            std::vector<std::string> const ids{"big-exact", "ordinary", "tiny-exact", "zero"};
            std::vector<std::vector<float>> const vecs{
                {1e20f, 1e20f}, {1.0f, 0.2f}, {1e-25f, 1e-25f}, {0.0f, 0.0f}};
            AE_CHECK(gpu.add_batch(ids, vecs).has_value() && cpu.add_batch(ids, vecs).has_value(),
                     "setup: both indices accept finite large/tiny/zero-norm components");
            for (float const scale : {1.0f, 1e20f, 1e-25f}) {
                float const q[2] = {scale, scale};
                auto g = gpu.search(std::span<float const>(q, 2), 4);
                auto c = cpu.search(std::span<float const>(q, 2), 4);
                bool agree = g.has_value() && c.has_value() && g->size() == 4 && c->size() == 4;
                if (agree) {
                    std::unordered_map<std::string, float> cpu_scores;
                    for (auto const& s : *c) cpu_scores[s.id] = s.score;
                    for (auto const& s : *g) {
                        agree = agree && std::isfinite(s.score) && std::fabs(s.score - cpu_scores.at(s.id)) < 1e-5f;
                    }
                    // Exact matches (score 1) rank above "ordinary" (0.83); the zero vector scores 0 on
                    // both sides (shared zero-norm guard) and ranks last.
                    agree = agree && (*g)[3].id == "zero" && (*g)[3].score == 0.0f && (*g)[2].id == "ordinary";
                }
                AE_CHECK(agree, "GPU matches CPU (within 1e-5, all finite, same ranking) for a query of "
                                "magnitude " + std::to_string(scale) +
                                " against stored vectors of magnitude 1e20, 1, 1e-25, and 0");
            }
        }
    }

    // --- Red-team pass 5 (2026-09-23), Real gap: non-finite components used to be accepted, and the
    // resulting NaN scores reached std::sort under a comparator that is not a strict weak ordering over
    // NaN (undefined behavior; observed as out-of-order results). Rejected at the origin now. ----------
    {
        auto created_nf = VulkanCosineIndex::create();
        AE_CHECK(created_nf.has_value(), "setup: an index instance for the non-finite case");
        if (created_nf.has_value()) {
            VulkanCosineIndex idx = std::move(*created_nf);
            float const nan = std::numeric_limits<float>::quiet_NaN();
            float const inf = std::numeric_limits<float>::infinity();
            for (float const bad : {nan, inf, -inf}) {
                auto r = idx.add_batch({"ok", "bad"}, {{1.0f, 0.0f}, {bad, 1.0f}});
                AE_CHECK(!r.has_value() && r.error().code == "vulkan_vector_index.add_batch_non_finite" &&
                             r.error().klass == failure_class::contract,
                         "add_batch rejects a vector with a non-finite component (" + std::to_string(bad) +
                             ") as a contract violation");
            }
            AE_CHECK(idx.size() == 0 && !idx.contains("ok"),
                     "a rejected non-finite batch commits NOTHING -- not even its finite members");
            AE_CHECK(idx.add_batch({"ok"}, {{1.0f, 0.0f}}).has_value(), "setup: one finite vector");
            float const bad_query[2] = {nan, 1.0f};
            auto s = idx.search(std::span<float const>(bad_query, 2), 1);
            AE_CHECK(!s.has_value() && s.error().code == "vulkan_vector_index.search_non_finite",
                     "search rejects a query with a non-finite component");
        }
    }

    // --- Red-team pass 5 (2026-09-23), Real gap: a moved-from instance used to dereference its null
    // pimpl on every method call (an access violation, reproduced). Now safe and typed. ---------------
    {
        auto created_mv = VulkanCosineIndex::create();
        AE_CHECK(created_mv.has_value(), "setup: an index instance for the moved-from case");
        if (created_mv.has_value()) {
            VulkanCosineIndex source = std::move(*created_mv);
            AE_CHECK(source.add_batch({"x"}, {{1.0f, 0.0f}}).has_value(), "setup: populate before moving");
            VulkanCosineIndex target = std::move(source);
            float const q[2] = {1.0f, 0.0f};
            // NOLINTBEGIN(bugprone-use-after-move) -- the moved-from contract IS the thing under test.
            AE_CHECK(source.size() == 0 && !source.contains("x"), "moved-from size()/contains() are 0/false");
            auto s = source.search(std::span<float const>(q, 2), 1);
            AE_CHECK(!s.has_value() && s.error().code == "vulkan_vector_index.moved_from",
                     "moved-from search() returns a typed error, not a crash");
            auto a = source.add_batch({"y"}, {{1.0f, 0.0f}});
            AE_CHECK(!a.has_value() && a.error().code == "vulkan_vector_index.moved_from",
                     "moved-from add_batch() returns a typed error, not a crash");
            source = std::move(target);  // move-assign back INTO the moved-from object
            // NOLINTEND(bugprone-use-after-move)
            auto back = source.search(std::span<float const>(q, 2), 1);
            AE_CHECK(back.has_value() && back->size() == 1 && (*back)[0].id == "x",
                     "a moved-from object is a valid move-assignment target and works again afterwards");
        }
    }

    // --- ADR-180 §10: dimension-major layout regression. The vectors buffer's stride is `n`, so a
    // layout bug (stale stride after growth, wrong index arithmetic, a shader/upload mismatch) would
    // hand every candidate some OTHER candidate's components. Grows the index across three batches
    // whose sizes are not multiples of the 64-lane workgroup, with an odd dimension, searching between
    // batches (so a cached buffer built for the old `n` must be rebuilt), and checks EVERY candidate's
    // score against BruteForceCosineIndex by id -- not just the top result. Positive control: run
    // against the pre-§10 row-major shader with this dimension-major upload, it fails. ---------------
    {
        constexpr std::size_t kDim = 17;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        BruteForceCosineIndex cpu_index;
        auto gpu_created = VulkanCosineIndex::create();
        AE_CHECK(gpu_created.has_value(), "layout test: GPU index created");
        if (gpu_created.has_value()) {
            VulkanCosineIndex gpu_index = std::move(*gpu_created);
            std::vector<float> query(kDim);
            for (auto& f : query) f = dist(rng);
            std::span<float const> query_span(query.data(), kDim);
            std::size_t total = 0;
            for (std::size_t batch : {std::size_t{37}, std::size_t{64}, std::size_t{131}}) {
                std::vector<std::string> ids;
                std::vector<std::vector<float>> vecs;
                for (std::size_t i = 0; i < batch; ++i) {
                    ids.push_back("L" + std::to_string(total + i));
                    std::vector<float> v(kDim);
                    for (auto& f : v) f = dist(rng);
                    vecs.push_back(std::move(v));
                }
                total += batch;
                AE_CHECK(cpu_index.add_batch(ids, vecs).has_value() && gpu_index.add_batch(ids, vecs).has_value(),
                         "layout test: add_batch succeeds on both, n=" + std::to_string(total));
                auto cpu_full = cpu_index.search(query_span, total);
                auto gpu_full = gpu_index.search(query_span, total);
                bool ok = cpu_full.has_value() && gpu_full.has_value() && gpu_full->size() == total &&
                          cpu_full->size() == total;
                float max_abs_diff = 0.0f;
                if (ok) {
                    std::unordered_map<std::string, float> cpu_scores;
                    for (auto const& s : *cpu_full) cpu_scores[s.id] = s.score;
                    for (auto const& s : *gpu_full) {
                        auto it = cpu_scores.find(s.id);
                        if (it == cpu_scores.end()) { ok = false; break; }
                        max_abs_diff = std::max(max_abs_diff, std::fabs(s.score - it->second));
                    }
                }
                AE_CHECK(ok && max_abs_diff < 1e-5f,
                         "layout test: every candidate's GPU score matches CPU within 1e-5 after growing to n=" +
                             std::to_string(total) + " (max diff " + std::to_string(max_abs_diff) + ")");
                auto gpu_top = gpu_index.search(query_span, 3);
                auto cpu_top = cpu_index.search(query_span, 3);
                AE_CHECK(gpu_top.has_value() && cpu_top.has_value() && gpu_top->size() == 3 &&
                             (*gpu_top)[0].id == (*cpu_top)[0].id,
                         "layout test: top-k selection returns exactly k, same winner as CPU, n=" +
                             std::to_string(total));
            }
        }
    }

    // --- Claim 7: epsilon-bounded comparison against BruteForceCosineIndex, dim=1536, n in
    // {1000, 5000, 20000} -- the sizes ADR-063 §6 measured as CPU Goal-tier misses, plus one larger
    // size where the GPU's advantage is unambiguous (ADR-180 §10). --------------------------------
    for (std::size_t n : {std::size_t{1000}, std::size_t{5000}, std::size_t{20000}}) {
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
