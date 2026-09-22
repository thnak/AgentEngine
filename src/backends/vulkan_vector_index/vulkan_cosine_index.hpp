#pragma once
// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.6 --
// `VulkanCosineIndex`: a GPU-accelerated, exact-brute-force `VectorIndex` conformer
// (core/vector_index.hpp), CONVENTIONS.md Tier 2 seam backend (`src/backends/*`, one dependency --
// the Vulkan LOADER, dynamically loaded, gated behind `AGENTENGINE_WITH_VULKAN`, never linked unless
// selected). ONE generic, cross-vendor GPU conformer -- a deliberate scope decision (ADR-180 §2.7):
// anyone wanting a CUDA/Metal/ROCm-specific backend writes their own `VectorIndex` conformer against
// the same seam; this is not a gap in this design, it is the design.
//
// Algorithm: exact brute-force cosine similarity, GPU-PARALLEL ACROSS CANDIDATES, SEQUENTIAL WITHIN
// each candidate's own dot product -- `src/backends/vulkan_vector_index/cosine_similarity.comp`'s own
// top comment has the full determinism rationale (§3.5/§2.3B's concern, and what it does NOT
// eliminate: float32 GPU accumulation vs. `BruteForceCosineIndex`'s own `double` accumulation is a
// named, bounded, accepted residual, the identical I5 posture `Embedder` already carries per ADR-063
// §2.2A). Storage (`add_batch`/`contains`/`size`) is pure host-side bookkeeping, mirroring
// `BruteForceCosineIndex` exactly -- only `search()` actually dispatches to the GPU; the stored
// vectors are re-uploaded to a GPU buffer on every `search()` call (simplicity over incremental-
// buffer-management performance, an accepted, named residual for this first pass -- see this file's
// own §5-6 account in the ADR for the real bench numbers this tradeoff costs).
//
// Satisfies the EXISTING, UNMODIFIED `VectorIndex` concept (core/vector_index.hpp) -- no
// `EffectContext`, no capability gating, no async: GPU dispatch is local compute, not a network
// effect, the same reasoning that already lets `BruteForceCosineIndex`/`RecursiveChunker` stay
// synchronous. Reuses `BruteForceCosineIndex`'s exact score-desc/id-asc tie-break after reading GPU
// scores back to the host (ADR-063 §4 finding 7's own total order, not re-derived here).
//
// Fails closed: `create()` returns a real `result<...>` error (never a crash, never a partially-
// constructed object) when no Vulkan-capable device is present or a required feature is missing --
// matching `docker_execution_surface.hpp`/`containerd_execution_surface.hpp`'s own posture toward
// "the backend this host doesn't have."
//
// Pimpl: every `<vulkan/vulkan.h>` detail lives in the .cpp, not here -- a Tier 1 caller composing
// `VectorRagContextProvider<..., VulkanCosineIndex, ...>` should not need the Vulkan SDK headers
// merely to NAME this type; only the one translation unit that actually links `Vulkan::Vulkan` does.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/vector_index.hpp"

namespace agentengine::backends::vulkan_vector_index {

// ae-naming-lint: allow VulkanCosineIndex — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
class VulkanCosineIndex {
public:
    // The ONLY way to construct a real instance -- a constructor cannot return `result<T>`, and "fail
    // closed with a real, typed error when no Vulkan device exists" needs exactly that channel, not a
    // thrown exception (CONVENTIONS.md: no exceptions for control flow) or a silently broken object.
    [[nodiscard]] static agentengine::result<VulkanCosineIndex> create();

    VulkanCosineIndex(VulkanCosineIndex&&) noexcept;
    VulkanCosineIndex& operator=(VulkanCosineIndex&&) noexcept;
    VulkanCosineIndex(VulkanCosineIndex const&) = delete;
    VulkanCosineIndex& operator=(VulkanCosineIndex const&) = delete;
    ~VulkanCosineIndex();

    [[nodiscard]] agentengine::result<void> add_batch(std::vector<std::string> const& ids,
                                                        std::vector<std::vector<float>> const& vectors);
    [[nodiscard]] agentengine::result<std::vector<agentengine::ScoredId>> search(
        std::span<float const> query, std::size_t k) const;
    [[nodiscard]] bool contains(std::string const& id) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Impl;
    explicit VulkanCosineIndex(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

static_assert(agentengine::VectorIndex<VulkanCosineIndex>,
              "VulkanCosineIndex must satisfy the real, unmodified VectorIndex concept (ADR-180 §2.6)");

namespace detail {

// Red-team pass 3 (2026-09-22) finding 3 (Real gap): `search()`'s push constants are `uint32_t
// count`/`uint32_t dimension` (cosine_similarity.comp's own PushConstants block), but `n`
// (`impl_->order.size()`) and `dim` (`impl_->dimension`) are `std::size_t` (64-bit). A candidate
// count or dimensionality that fits in `size_t` but exceeds `UINT32_MAX` would silently WRAP at the
// `static_cast<std::uint32_t>(...)` call sites in `search()` -- the GPU would then dispatch against
// a wrong, wrapped `count`/`dimension`, either under- or over-reading the flattened vectors buffer
// relative to what the host actually allocated (`n * dim * sizeof(float)` bytes, computed in
// `size_t`/`VkDeviceSize`, which does NOT wrap at the same boundary). Reject, don't silently
// truncate -- the same "reject-not-coerce on any structural inconsistency" posture
// `BruteForceCosineIndex::restore()`'s own red-team-pass-1 fix already established for this ADR
// family (§4 finding 1). A pure, Vulkan-free predicate so it is unit-testable with synthetic
// `size_t` values directly, without needing to actually allocate (or hold) billions of real
// vectors -- exactly how `ByteReader::check_header_plausible()` is tested against a crafted header
// rather than a genuinely oversized blob.
[[nodiscard]] agentengine::result<void> check_gpu_dispatch_size_plausible(std::size_t n, std::size_t dim);

}  // namespace detail

}  // namespace agentengine::backends::vulkan_vector_index
