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
// vectors live in a persistent, device-local GPU buffer rebuilt only after `add_batch()` changes the
// corpus (red-team pass 5, 2026-09-23: this comment previously still described the ORIGINAL
// "re-upload on every search() call" design, which ADR-180 §5-6's step-8 account replaced with a
// cache before any red-team pass ran -- stale documentation, corrected here).
//
// Input domain (red-team pass 5, 2026-09-23): every vector/query component must be FINITE -- NaN/Inf
// is rejected as a `contract` violation, never scored (a NaN score reaching the score-desc sort
// violates std::sort's strict-weak-ordering precondition). Finite inputs of ANY magnitude are scored
// correctly: each vector is rescaled by an exact power of two before upload so float32 accumulation
// in the shader can neither overflow (|x| > ~1.8e19 squared) nor underflow (|x| < ~1e-19 squared) --
// cosine similarity is scale-invariant, and power-of-two scaling is exact in IEEE arithmetic, so
// this changes no in-range result by even one ULP.
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
#include <cstdint>
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

    // Red-team pass 5 (2026-09-23): a MOVED-FROM instance (e.g. the `result<VulkanCosineIndex>` a
    // caller `std::move(*created)`-ed out of) is safe to destroy, to move-assign into, and to call
    // every method on -- `add_batch()`/`search()` return a typed `contract` error
    // (`vulkan_vector_index.moved_from`), `contains()` returns false, `size()` returns 0. Previously
    // every method dereferenced the null pimpl (an access violation, reproduced on the real GPU).
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

// Red-team pass 5 (2026-09-23), Real gap: the uint32 guard above is necessary but NOT sufficient.
// The flattened vectors buffer is bound as ONE storage-buffer descriptor with `VK_WHOLE_SIZE`, and the
// Vulkan spec requires that effective range to be <= `VkPhysicalDeviceLimits::maxStorageBufferRange`
// (VUID-VkWriteDescriptorSet-descriptorType-00333) -- whose spec-guaranteed minimum is only 2^27 bytes
// (128 MiB), the exact value the Vulkan SDK's own `VP_ANDROID_vulkan_profile_2021` baseline profile
// declares. At dim=1536 that is exceeded at n=21,846 -- a realistic corpus, not an adversarial one;
// reproduced under the Khronos validation layer + profiles layer emulating that baseline. Enforcing
// this bound ALSO closes the shader's own 32-bit index arithmetic (`uint base = i * pc.dimension`,
// cosine_similarity.comp): `maxStorageBufferRange` is itself a uint32, so n*dim*4 <= UINT32_MAX
// implies n*dim < 2^30 and that product can never wrap. The second bound closes red-team pass 3's
// (§4c finding 4) named-but-unfixed residual: the dispatch's workgroup count ((n + 63) / 64) must be
// <= `maxComputeWorkGroupCount[0]` (VUID-vkCmdDispatch-groupCountX-00386). Pure and Vulkan-free so it
// is unit-testable with synthetic limits, the same shape as the guard above.
[[nodiscard]] agentengine::result<void> check_gpu_device_limits(std::size_t n, std::size_t dim,
                                                                std::uint64_t max_storage_buffer_range,
                                                                std::uint32_t max_workgroup_count_x);

#ifdef AGENTENGINE_VULKAN_FAULT_INJECTION
// Red-team pass 5 (2026-09-23): a TEST-ONLY fault-injection seam, compiled in ONLY when the
// translation unit is built with AGENTENGINE_VULKAN_FAULT_INJECTION (tests/CMakeLists.txt builds a
// separate library variant for tests/test_vulkan_cosine_index_fault_injection.cpp; the production
// `agentengine::vulkan_vector_index` target never defines it, so none of this exists there). Every
// prior red-team pass (§4c/§4d) recorded that NONE of this file's checked-VkResult failure branches
// had ever been executed -- only reasoned about -- because a real device-lost/OOM cannot be induced
// safely on a dev box. This seam makes the Nth call of a named Vulkan entry point report failure
// (creation-style calls are SKIPPED, so nothing leaks; `vkWaitForFences` is called for real and its
// result then overridden to VK_ERROR_DEVICE_LOST, so no resource is ever torn down while pending).
enum class fault_point : std::uint8_t {
    create_instance, enumerate_physical_devices, create_device, create_command_pool,
    create_descriptor_set_layout, create_descriptor_pool, create_pipeline_layout, create_shader_module,
    create_compute_pipeline,
    create_buffer, allocate_memory, bind_buffer_memory, map_memory, allocate_command_buffers,
    begin_command_buffer, end_command_buffer, create_fence, queue_submit, wait_for_fences,
    reset_command_buffer, allocate_descriptor_sets,
    count_  // sentinel, not a real point
};
// Arms `point` to fail on its `nth` (1-based) hit after this call; resets every hit counter.
void arm_fault_injection(fault_point point, unsigned nth);
// Disarms any armed fault and resets every hit counter.
void disarm_fault_injection();
// How many times `point` was reached since the last arm/disarm call.
[[nodiscard]] unsigned fault_injection_hits(fault_point point);
#endif

}  // namespace detail

}  // namespace agentengine::backends::vulkan_vector_index
