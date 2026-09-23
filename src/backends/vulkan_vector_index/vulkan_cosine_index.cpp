// Implements vulkan_cosine_index.hpp's own design -- see that file's top comment for the full
// rationale. This translation unit is the ONLY place `<vulkan/vulkan.h>` appears in this backend.

#include "vulkan_cosine_index.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "vulkan_cosine_similarity_spv.h"  // generated at CMake configure time from the checked-in .spv

namespace agentengine::backends::vulkan_vector_index {

namespace {

// Same tie-break every index/fusion function in the ADR-180 family uses (ADR-063 §4 finding 7):
// score desc, then id asc -- a genuine deterministic total order, reused here verbatim rather than
// re-derived, so a caller mixing VulkanCosineIndex and BruteForceCosineIndex never observes a
// DIFFERENT tie-break rule between the two.
//
// Red-team pass 5 (2026-09-23), Real gap: the comparator used to be exactly
// BruteForceCosineIndex's (`a.score != b.score ? a.score > b.score : a.id < b.id`), which is NOT a
// strict weak ordering once any score is NaN (NaN compares "equivalent" to every score, so
// equivalence is not transitive) -- violating std::sort's precondition, i.e. undefined behavior, not
// merely a wrong answer. Reproduced on the real GPU before the fix: 29 NaN scores among 200 produced
// 11 descending-order violations among the FINITE scores. add_batch()/search() now reject non-finite
// input, so a NaN score should be unreachable; this comparator is ALSO made total over NaN anyway
// (every NaN orders after every non-NaN score, NaNs among themselves by id) so the sort's own
// precondition never depends on that upstream reasoning staying true.
void sort_and_truncate(std::vector<agentengine::ScoredId>& scored, std::size_t k) {
    std::sort(scored.begin(), scored.end(),
              [](agentengine::ScoredId const& a, agentengine::ScoredId const& b) {
                  bool const a_nan = std::isnan(a.score);
                  bool const b_nan = std::isnan(b.score);
                  if (a_nan != b_nan) return b_nan;
                  if (!a_nan && a.score != b.score) return a.score > b.score;
                  return a.id < b.id;
              });
    if (scored.size() > k) scored.resize(k);
}

[[nodiscard]] agentengine::error vulkan_error(std::string message, std::string code) {
    return agentengine::error{agentengine::failure_class::resource, std::move(message), std::move(code)};
}

// Red-team pass 5 (2026-09-23), Minor: every runtime Vulkan failure used to collapse into one
// `failure_class::resource` error with no trace of WHICH VkResult occurred -- so a caller could not
// tell a (possibly transient) out-of-memory from VK_ERROR_DEVICE_LOST, after which this instance's
// VkDevice is permanently unusable and every later call will fail too (the caller's only recovery is
// a fresh `create()` or a CPU fallback). The numeric VkResult is now in the message, and device loss
// gets its own stable code. The class stays `resource` (not reclassified to `fatal`, which error.hpp
// defines as "the run ends" -- too strong for a condition a caller with a CPU fallback survives).
// Project-owner ruling 2026-09-23 (ADR-180 §7): `resource` is the decision, not a placeholder.
[[nodiscard]] agentengine::error vk_call_error(VkResult vk_result, std::string what, std::string code) {
    if (vk_result == VK_ERROR_DEVICE_LOST) code = "vulkan_vector_index.device_lost";
    return vulkan_error(std::move(what) + " (VkResult " + std::to_string(static_cast<int>(vk_result)) + ")",
                        std::move(code));
}

[[nodiscard]] agentengine::error moved_from_error() {
    return agentengine::error{agentengine::failure_class::contract,
                              "this VulkanCosineIndex was moved from -- it no longer owns a GPU device",
                              "vulkan_vector_index.moved_from"};
}

[[nodiscard]] bool all_finite(std::span<float const> values) {
    return std::all_of(values.begin(), values.end(), [](float x) { return std::isfinite(x); });
}

// Red-team pass 5 (2026-09-23), Real gap: copies `src` into `dst` scaled by the exact power of two
// that brings its largest |component| into [0.5, 1). Before this, the shader squared RAW components in
// float32: a finite component above ~1.8e19 overflowed to +inf (an exact match then scored 0 or NaN)
// and one below ~1e-19 underflowed to 0 (an exact match scored 0) -- while BruteForceCosineIndex,
// accumulating in double, scored both correctly as 1.0. Reproduced on the real GPU: {1e20, 1e20} vs
// query {1, 1} scored 0 on GPU, 1 on CPU, and ranked LAST -- a wrong-by-a-wide-margin divergence
// ADR-180 §3 claim 7(b) says cannot happen. After scaling, every component is < 1, so each sum of
// squares is < dim <= UINT32_MAX, far below FLT_MAX. Cosine similarity is scale-invariant, and
// multiplying by 2^k is exact in IEEE arithmetic (barring subnormal results, which only affect
// components ~2^-126 below the vector's own maximum -- contributions already negligible at that
// ratio), so for inputs that were ALREADY in range this reproduces the pre-fix shader result exactly.
void copy_rescaled_by_power_of_two(std::span<float const> src, float* dst) {
    float max_abs = 0.0f;
    for (float x : src) max_abs = (std::max)(max_abs, std::fabs(x));
    if (max_abs == 0.0f) {  // a zero vector: leave it zero, the shader's own zero-norm guard scores it 0
        std::copy(src.begin(), src.end(), dst);
        return;
    }
    int exponent = 0;
    (void)std::frexp(max_abs, &exponent);  // max_abs == f * 2^exponent, f in [0.5, 1)
    for (std::size_t i = 0; i < src.size(); ++i) dst[i] = std::ldexp(src[i], -exponent);
}

}  // namespace

namespace detail {

#ifdef AGENTENGINE_VULKAN_FAULT_INJECTION
namespace {
constexpr std::size_t kFaultPointCount = static_cast<std::size_t>(fault_point::count_);
std::atomic<int> g_armed_point{-1};
std::atomic<unsigned> g_armed_nth{0};
std::array<std::atomic<unsigned>, kFaultPointCount> g_fault_hits{};
void reset_fault_hits() {
    for (auto& h : g_fault_hits) h.store(0);
}
}  // namespace

void arm_fault_injection(fault_point point, unsigned nth) {
    reset_fault_hits();
    g_armed_nth.store(nth);
    g_armed_point.store(static_cast<int>(point));
}
void disarm_fault_injection() {
    g_armed_point.store(-1);
    reset_fault_hits();
}
unsigned fault_injection_hits(fault_point point) {
    return g_fault_hits[static_cast<std::size_t>(point)].load();
}
namespace {
[[nodiscard]] bool fault_fires(fault_point point) {
    unsigned const hit = ++g_fault_hits[static_cast<std::size_t>(point)];
    return g_armed_point.load() == static_cast<int>(point) && hit == g_armed_nth.load();
}
}  // namespace
#define AE_VK_FAULT(point) ::agentengine::backends::vulkan_vector_index::detail::fault_fires( \
    ::agentengine::backends::vulkan_vector_index::detail::fault_point::point)
#else
// Production build: the seam does not exist -- a constant `false` the optimizer removes entirely.
#define AE_VK_FAULT(point) ::agentengine::backends::vulkan_vector_index::detail::fault_never_fires()
[[nodiscard]] constexpr bool fault_never_fires() noexcept { return false; }
#endif

}  // namespace detail

namespace {

// Red-team pass 3 (2026-09-22), Critical finding 1: runs a stack of cleanup actions, in REVERSE
// (creation) order, unless dismiss() is called first -- so every transient GPU resource created
// partway through a multi-step build (a copy command buffer, a one-shot fence, a staging buffer) is
// destroyed on ANY early `return std::unexpected(...)` out of that block, not just the happy path.
// Matches this class's own destructor discipline (reverse-creation-order teardown) applied to a
// SHORT-LIVED, mid-function resource set instead of the whole Impl.
class CleanupStack {
public:
    void push(std::function<void()> action) { actions_.push_back(std::move(action)); }
    void dismiss() { actions_.clear(); }
    ~CleanupStack() {
        for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) (*it)();
    }

private:
    std::vector<std::function<void()>> actions_;
};

}  // namespace

namespace detail {

agentengine::result<void> check_gpu_dispatch_size_plausible(std::size_t n, std::size_t dim) {
    // cosine_similarity.comp's PushConstants block is {uint32 count; uint32 dimension;} --
    // vulkan_cosine_index.cpp's own search() narrows `n`/`dim` (size_t) into those fields via
    // static_cast<std::uint32_t>. Reject BEFORE that narrowing can silently wrap.
    constexpr std::size_t kMaxU32 = (std::numeric_limits<std::uint32_t>::max)();
    if (n > kMaxU32 || dim > kMaxU32) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::resource,
            "candidate count or vector dimensionality exceeds this backend's 32-bit GPU dispatch "
            "limit (cosine_similarity.comp's push constants are uint32)",
            "vulkan_vector_index.dispatch_size_exceeds_uint32"});
    }
    // The flattened vectors buffer is n * dim * sizeof(float) bytes (VkDeviceSize, 64-bit) --
    // guard the multiplication itself against size_t overflow (division-based check, never
    // multiplies first) even though the uint32 guard above already makes this astronomically
    // unlikely to be reachable in practice; named as a second, independent layer rather than
    // relying on the uint32 check alone to imply it.
    if (dim != 0 && n > (std::numeric_limits<std::size_t>::max)() / dim) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::resource,
            "candidate count * dimensionality overflows when computing the GPU vectors buffer size",
            "vulkan_vector_index.dispatch_size_overflow"});
    }
    return {};
}

agentengine::result<void> check_gpu_device_limits(std::size_t n, std::size_t dim,
                                                  std::uint64_t max_storage_buffer_range,
                                                  std::uint32_t max_workgroup_count_x) {
    // Division-based, never multiplies first (n * dim * sizeof(float) could itself overflow for
    // pathological inputs, though check_gpu_dispatch_size_plausible() runs first at the call site).
    std::uint64_t const bytes_per_vector = std::uint64_t{dim} * sizeof(float);
    if (bytes_per_vector != 0 && (bytes_per_vector > max_storage_buffer_range ||
                                  std::uint64_t{n} > max_storage_buffer_range / bytes_per_vector)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::resource,
            "the index's vectors buffer (" + std::to_string(n) + " x " + std::to_string(dim) +
                " floats) exceeds this device's maxStorageBufferRange (" +
                std::to_string(max_storage_buffer_range) +
                " bytes) -- use a CPU index (BruteForceCosineIndex) or a smaller corpus on this device",
            "vulkan_vector_index.exceeds_device_storage_buffer_range"});
    }
    std::uint64_t const group_count = (std::uint64_t{n} + 63) / 64;  // local_size_x = 64
    if (group_count > max_workgroup_count_x) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::resource,
            "the dispatch needs " + std::to_string(group_count) +
                " workgroups, above this device's maxComputeWorkGroupCount[0] (" +
                std::to_string(max_workgroup_count_x) + ")",
            "vulkan_vector_index.exceeds_device_workgroup_count"});
    }
    return {};
}

}  // namespace detail

struct VulkanCosineIndex::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family_index = 0;
    // Red-team pass 5 (2026-09-23): the selected device's own limits, queried once in create() and
    // enforced by detail::check_gpu_device_limits() before every dispatch.
    std::uint64_t max_storage_buffer_range = 0;
    std::uint32_t max_workgroup_count_x = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    // Host-side bookkeeping -- pure mirror of BruteForceCosineIndex's own shape (core/vector_index.hpp).
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::vector<float>> entries;
    std::vector<std::string> order;
    std::size_t dimension = 0;

    // Persistent GPU resource cache -- rebuilt only when the corpus actually changes (`cache_dirty`,
    // set by add_batch()), never on every search() call. Found NECESSARY by this file's own first
    // real run against a real GPU (not merely reasoned about): re-creating and re-uploading the WHOLE
    // vectors buffer (n*dimension*4 bytes -- ~30 MB at n=5000, dim=1536) on every search() call made
    // GPU search roughly 10x SLOWER than CPU brute-force at exactly the sizes this backend exists to
    // help with (ADR-063 §6's own n=1000/5000 Goal-tier-miss measurements) -- the opposite of this
    // backend's entire purpose. `mutable`: search() is logically const (VectorIndex's own concept
    // shape) but must still populate this lazily-built cache on first use / after a corpus change --
    // the same "mutable cache behind a const interface" pattern this codebase already uses elsewhere
    // for expensive-to-recompute derived state. Capacity is grow-only (matching that this concept has
    // no `remove()` -- `order.size()` never shrinks), so a buffer is only ever replaced with a BIGGER
    // one, never needlessly shrunk and regrown.
    mutable VkBuffer vectors_buffer = VK_NULL_HANDLE;
    mutable VkDeviceMemory vectors_memory = VK_NULL_HANDLE;
    mutable std::size_t vectors_buffer_capacity = 0;
    mutable VkBuffer query_buffer = VK_NULL_HANDLE;
    mutable VkDeviceMemory query_memory = VK_NULL_HANDLE;
    mutable VkBuffer scores_buffer = VK_NULL_HANDLE;
    mutable VkDeviceMemory scores_memory = VK_NULL_HANDLE;
    mutable std::size_t scores_buffer_capacity = 0;
    mutable VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    mutable VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    mutable bool cache_dirty = true;  // true: vectors_buffer does not reflect current entries/order

    ~Impl() {
        // Reverse-creation-order destruction, the standard Vulkan discipline -- every handle here is
        // either VK_NULL_HANDLE (never created, e.g. a failed create() path partway through) or real;
        // every vkDestroy* is a documented no-op on VK_NULL_HANDLE, so no extra guards are needed.
        // The cached per-search resources (buffers/memory are NOT pool-allocated, so need explicit
        // destruction; descriptor_set/command_buffer ARE pool-allocated and are freed implicitly when
        // descriptor_pool/command_pool are destroyed below, but freed explicitly here too for clarity
        // and to match this destructor's own "every handle gets an explicit destroy call" discipline).
        if (vectors_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, vectors_buffer, nullptr);
        if (vectors_memory != VK_NULL_HANDLE) vkFreeMemory(device, vectors_memory, nullptr);
        if (query_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, query_buffer, nullptr);
        if (query_memory != VK_NULL_HANDLE) vkFreeMemory(device, query_memory, nullptr);
        if (scores_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, scores_buffer, nullptr);
        if (scores_memory != VK_NULL_HANDLE) vkFreeMemory(device, scores_memory, nullptr);
        if (descriptor_set != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set);
        }
        if (command_buffer != VK_NULL_HANDLE && command_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
        }
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader_module != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader_module, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        }
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    // Finds a memory type index satisfying both `type_filter` (a bitmask from
    // VkMemoryRequirements::memoryTypeBits) and `properties` (the flags this call site actually
    // needs, e.g. HOST_VISIBLE | HOST_COHERENT). Returns UINT32_MAX (an impossible real index -- a
    // real GPU never reports more than a few dozen memory types) on no match, checked by every caller.
    [[nodiscard]] std::uint32_t find_memory_type(std::uint32_t type_filter,
                                                   VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
        for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            bool const type_ok = (type_filter & (1u << i)) != 0;
            bool const props_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
            if (type_ok && props_ok) return i;
        }
        return UINT32_MAX;
    }
};

VulkanCosineIndex::VulkanCosineIndex(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VulkanCosineIndex::VulkanCosineIndex(VulkanCosineIndex&&) noexcept = default;
VulkanCosineIndex& VulkanCosineIndex::operator=(VulkanCosineIndex&&) noexcept = default;
VulkanCosineIndex::~VulkanCosineIndex() = default;

agentengine::result<VulkanCosineIndex> VulkanCosineIndex::create() {
    auto impl = std::make_unique<Impl>();

    // ---- Instance: no extensions, no validation layers -- pure headless compute, nothing this
    // backend needs a surface/swapchain/debug-callback for in a release conformer. -------------------
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "AgentEngine VulkanCosineIndex";
    app_info.apiVersion = VK_API_VERSION_1_1;  // no feature this shader needs is newer than 1.1

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    if (AE_VK_FAULT(create_instance) || vkCreateInstance(&instance_info, nullptr, &impl->instance) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreateInstance failed -- no usable Vulkan loader/runtime on this host",
                         "vulkan_vector_index.instance_creation_failed"));
    }

    // ---- Physical device: the first device exposing a queue family with compute support. Fails
    // closed (a real error, not a crash) when no such device exists -- the whole point of `create()`
    // returning `result<...>` instead of being a plain constructor. -----------------------------------
    // Red-team pass 4 (2026-09-22), Real gap: both calls below used to be fire-and-forget -- no
    // VkResult check on either. `vkEnumeratePhysicalDevices` CAN fail in-spec
    // (VK_ERROR_OUT_OF_HOST_MEMORY / VK_ERROR_OUT_OF_DEVICE_MEMORY / VK_ERROR_INITIALIZATION_FAILED)
    // even at the count-query call; on failure the spec leaves `device_count` in an
    // implementation-defined state, not necessarily 0, so the old code could have proceeded to size
    // `devices` off a bogus count and then, on the second call, iterate past however many handles
    // were ACTUALLY written (a device removed between the two calls is a real, if rare, case where
    // the second call legitimately reports fewer than the first) -- calling
    // `vkGetPhysicalDeviceQueueFamilyProperties` on a stale/VK_NULL_HANDLE tail entry is invalid API
    // usage. Same "reject-not-coerce on any structural inconsistency" posture as every other checked
    // call in this file (§4c); `devices.resize(device_count)` after the second call additionally
    // closes the stale-tail-iteration gap regardless of which enumeration count actually won.
    std::uint32_t device_count = 0;
    if (AE_VK_FAULT(enumerate_physical_devices) ||
        vkEnumeratePhysicalDevices(impl->instance, &device_count, nullptr) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkEnumeratePhysicalDevices failed while querying the physical device count",
                         "vulkan_vector_index.device_enumeration_failed"));
    }
    if (device_count == 0) {
        return std::unexpected(vulkan_error("no Vulkan-capable physical device found on this host",
                                              "vulkan_vector_index.device_not_found"));
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    if (VkResult const enumerate_result =
            AE_VK_FAULT(enumerate_physical_devices)
                ? VK_ERROR_OUT_OF_HOST_MEMORY
                : vkEnumeratePhysicalDevices(impl->instance, &device_count, devices.data());
        enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) {
        return std::unexpected(
            vulkan_error("vkEnumeratePhysicalDevices failed while filling the physical device list",
                         "vulkan_vector_index.device_enumeration_failed"));
    }
    devices.resize(device_count);  // the second call may report fewer devices than the first (one
                                    // removed between the two calls) -- never iterate past what was
                                    // actually written.
    if (devices.empty()) {
        return std::unexpected(vulkan_error("no Vulkan-capable physical device found on this host",
                                              "vulkan_vector_index.device_not_found"));
    }

    bool found_device = false;
    for (VkPhysicalDevice candidate : devices) {
        std::uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, families.data());
        for (std::uint32_t i = 0; i < queue_family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                impl->physical_device = candidate;
                impl->queue_family_index = i;
                found_device = true;
                break;
            }
        }
        if (found_device) break;
    }
    if (!found_device) {
        return std::unexpected(
            vulkan_error("no physical device with a compute-capable queue family found",
                         "vulkan_vector_index.feature_unsupported"));
    }
    {
        // Red-team pass 5 (2026-09-23): record the limits search() must respect (see
        // detail::check_gpu_device_limits()'s own comment in the header). Void-returning call on a
        // handle the enumeration above just returned -- no failure mode to check.
        VkPhysicalDeviceProperties device_props{};
        vkGetPhysicalDeviceProperties(impl->physical_device, &device_props);
        impl->max_storage_buffer_range = device_props.limits.maxStorageBufferRange;
        impl->max_workgroup_count_x = device_props.limits.maxComputeWorkGroupCount[0];
    }

    // ---- Logical device + one compute queue --------------------------------------------------------
    float const queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl->queue_family_index;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    if (AE_VK_FAULT(create_device) ||
        vkCreateDevice(impl->physical_device, &device_info, nullptr, &impl->device) != VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkCreateDevice failed", "vulkan_vector_index.device_creation_failed"));
    }
    vkGetDeviceQueue(impl->device, impl->queue_family_index, 0, &impl->queue);

    // ---- Command pool: RESET_COMMAND_BUFFER so the one command buffer search() uses is re-recorded
    // (not re-allocated) on every call. ----------------------------------------------------------------
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = impl->queue_family_index;
    if (AE_VK_FAULT(create_command_pool) ||
        vkCreateCommandPool(impl->device, &pool_info, nullptr, &impl->command_pool) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreateCommandPool failed", "vulkan_vector_index.command_pool_creation_failed"));
    }

    // ---- Descriptor set layout: 3 storage buffers (query, vectors, scores), matching
    // cosine_similarity.comp's own binding 0/1/2 exactly. ------------------------------------------
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    if (AE_VK_FAULT(create_descriptor_set_layout) ||
        vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->descriptor_set_layout) !=
            VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkCreateDescriptorSetLayout failed",
                                              "vulkan_vector_index.descriptor_set_layout_creation_failed"));
    }

    // ---- Descriptor pool: sized for repeated per-search allocate/reset, one set at a time. -----------
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 3;
    VkDescriptorPoolCreateInfo desc_pool_info{};
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &pool_size;
    if (AE_VK_FAULT(create_descriptor_pool) ||
        vkCreateDescriptorPool(impl->device, &desc_pool_info, nullptr, &impl->descriptor_pool) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreateDescriptorPool failed", "vulkan_vector_index.descriptor_pool_creation_failed"));
    }

    // ---- Pipeline layout: one push-constant range, {uint32 count; uint32 dimension;} (8 bytes),
    // matching cosine_similarity.comp's own PushConstants block exactly. ------------------------------
    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = 2 * sizeof(std::uint32_t);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &impl->descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    if (AE_VK_FAULT(create_pipeline_layout) ||
        vkCreatePipelineLayout(impl->device, &pipeline_layout_info, nullptr, &impl->pipeline_layout) !=
            VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreatePipelineLayout failed", "vulkan_vector_index.pipeline_layout_creation_failed"));
    }

    // ---- Shader module, from the embedded, pre-compiled SPIR-V (generated header, build-time-only
    // dependency on glslc -- see cosine_similarity.comp's own top comment). ---------------------------
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = kCosineSimilaritySpirvBytes;
    shader_info.pCode = reinterpret_cast<std::uint32_t const*>(kCosineSimilaritySpirv);
    if (AE_VK_FAULT(create_shader_module) ||
        vkCreateShaderModule(impl->device, &shader_info, nullptr, &impl->shader_module) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreateShaderModule failed -- the embedded SPIR-V is malformed or "
                         "incompatible with this device",
                         "vulkan_vector_index.shader_module_creation_failed"));
    }

    VkPipelineShaderStageCreateInfo stage_info{};
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = impl->shader_module;
    stage_info.pName = "main";

    VkComputePipelineCreateInfo compute_pipeline_info{};
    compute_pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_pipeline_info.stage = stage_info;
    compute_pipeline_info.layout = impl->pipeline_layout;
    if (AE_VK_FAULT(create_compute_pipeline) ||
        vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &compute_pipeline_info, nullptr,
                                 &impl->pipeline) != VK_SUCCESS) {
        return std::unexpected(
            vulkan_error("vkCreateComputePipelines failed", "vulkan_vector_index.pipeline_creation_failed"));
    }

    return VulkanCosineIndex(std::move(impl));
}

// ---- Host-side bookkeeping (add_batch/contains/size) -- pure mirror of BruteForceCosineIndex's own
// shape and validation, no GPU work at all here. -----------------------------------------------------

agentengine::result<void> VulkanCosineIndex::add_batch(std::vector<std::string> const& ids,
                                                          std::vector<std::vector<float>> const& vectors) {
    if (!impl_) return std::unexpected(moved_from_error());
    std::unique_lock lock(impl_->mutex);
    if (ids.size() != vectors.size()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                     "ids and vectors must have the same length",
                                                     "vulkan_vector_index.add_batch_length_mismatch"});
    }
    std::unordered_set<std::string> seen_in_this_call;
    for (auto const& id : ids) {
        if (impl_->entries.contains(id) || !seen_in_this_call.insert(id).second) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                         "duplicate id in a single add_batch call",
                                                         "vulkan_vector_index.add_batch_duplicate_id"});
        }
    }
    std::size_t const expected_dim =
        impl_->dimension != 0 ? impl_->dimension : (vectors.empty() ? 0 : vectors.front().size());
    // Red-team pass 4 (2026-09-22), Real gap: without this check, a first-ever `add_batch()` call
    // carrying a zero-dimensional vector (e.g. `add_batch({"a"}, {{}})`) established `impl_->dimension
    // == 0` on a NON-EMPTY index -- a state `search()`'s own comment (below) claims is "possible only
    // if this index has never been add_batch()'d with a real vector, already excluded... by the
    // order.empty() early return," which was simply wrong: this is a second, distinct way to reach
    // dimension == 0 with a non-empty order, and it WAS reachable through this class's own public API.
    // `search()` then classified that state as `failure_class::fatal` ("internal invariant violated" --
    // error.hpp: "unrecoverable; the run ends"), the wrong severity for a normal, reachable, caller-
    // supplied-degenerate-input case (a `contract` violation, exactly like every other add_batch()
    // rejection above and below this one) -- and, separately, a zero-byte GPU buffer is invalid Vulkan
    // usage (VUID-VkBufferCreateInfo-size-00912: size must be > 0), so accepting this state at all would
    // have only deferred a real problem to search() instead of rejecting it at its actual origin.
    // Reject here, structurally, so `search()`'s own "should be unreachable" comment becomes true
    // rather than aspirational -- matching this codebase's reject-not-coerce posture, and the identical
    // shape `RemoteVectorIndex<T>`'s red-team-pass-1 fix already used (§4/§5-6): make the invariant
    // true by construction, not merely asserted after the fact.
    if (!vectors.empty() && expected_dim == 0) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "a vector must have at least one dimension -- zero-dimensional vectors cannot be indexed",
            "vulkan_vector_index.add_batch_zero_dimension"});
    }
    for (auto const& v : vectors) {
        if (v.size() != expected_dim) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "vector dimensionality mismatch: expected " + std::to_string(expected_dim) + ", got " +
                    std::to_string(v.size()),
                "vulkan_vector_index.add_batch_dimension_mismatch"});
        }
    }
    // Red-team pass 5 (2026-09-23), Real gap: NaN/Inf components used to be accepted, producing NaN
    // scores that then reached std::sort under a comparator that is not a strict weak ordering over
    // NaN -- undefined behavior (reproduced: out-of-order results). Reject at the origin, before any
    // state changes, exactly like every other add_batch() contract check above. BruteForceCosineIndex
    // had the identical latent defect and now rejects non-finite input the same way
    // (`vector_index.add_batch_non_finite`, ADR-180 §4e condition 2) -- the two conformers agree again.
    for (auto const& v : vectors) {
        if (!all_finite(v)) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "vector components must be finite -- NaN or infinity cannot be scored",
                "vulkan_vector_index.add_batch_non_finite"});
        }
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        impl_->entries.emplace(ids[i], vectors[i]);
        impl_->order.push_back(ids[i]);
    }
    if (impl_->dimension == 0) impl_->dimension = expected_dim;
    impl_->cache_dirty = true;  // the GPU vectors buffer no longer reflects entries/order
    return {};
}

bool VulkanCosineIndex::contains(std::string const& id) const {
    if (!impl_) return false;  // moved-from: owns nothing, contains nothing
    std::shared_lock lock(impl_->mutex);
    return impl_->entries.contains(id);
}

std::size_t VulkanCosineIndex::size() const {
    if (!impl_) return 0;  // moved-from: owns nothing
    std::shared_lock lock(impl_->mutex);
    return impl_->entries.size();
}

// ---- search(): the one method that actually dispatches to the GPU -----------------------------------

agentengine::result<std::vector<agentengine::ScoredId>> VulkanCosineIndex::search(std::span<float const> query,
                                                                                     std::size_t k) const {
    // unique_lock, not shared_lock: this method must be able to (re)populate the persistent GPU
    // buffer cache below. A real, honest concurrency-vs-performance tradeoff specific to this
    // GPU-backed conformer, unlike BruteForceCosineIndex's own read-concurrent search() -- named here,
    // not hidden: GPU dispatch is inherently serialized through one VkQueue on one device anyway
    // (there is exactly one `impl_->queue`), so concurrent callers would serialize at the driver level
    // regardless; this lock makes that already-real serialization explicit at the C++ level too,
    // rather than leaving concurrent callers to race on the shared cache fields above.
    if (!impl_) return std::unexpected(moved_from_error());
    std::unique_lock lock(impl_->mutex);

    if (impl_->dimension != 0 && query.size() != impl_->dimension) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "query vector dimensionality (" + std::to_string(query.size()) +
                ") does not match index dimensionality (" + std::to_string(impl_->dimension) + ")",
            "vulkan_vector_index.search_dimension_mismatch"});
    }
    // Red-team pass 5: same non-finite rejection add_batch() applies, on the query side.
    if (!all_finite(query)) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                  "query components must be finite -- NaN or infinity "
                                                  "cannot be scored",
                                                  "vulkan_vector_index.search_non_finite"});
    }
    if (impl_->order.empty()) return std::vector<agentengine::ScoredId>{};

    std::size_t const n = impl_->order.size();
    std::size_t const dim = impl_->dimension;

    // Guard against a zero-sized buffer (VK_WHOLE_SIZE-adjacent edge case). Red-team pass 4
    // (2026-09-22): this comment previously claimed dim==0 was "possible only if this index has never
    // been add_batch()'d with a real vector, already excluded above by the order.empty() early
    // return" -- that was FALSE until this same pass's add_batch() fix: a zero-dimensional vector
    // (e.g. `add_batch({"a"}, {{}})`) was a second, distinct way to reach dim==0 with a non-empty
    // order, reachable through this class's own public API, not merely a hypothetical. add_batch()
    // now rejects that input outright (`vulkan_vector_index.add_batch_zero_dimension`), so this really
    // is unreachable today -- kept as a defensive reject-not-coerce guard anyway, matching this
    // codebase's own posture toward "should be unreachable" invariants (vector_index.hpp's own
    // cosine_similarity() comment, same stance), and `failure_class::fatal` here is now honest: any
    // future code path that DID reach this despite add_batch()'s guard would be a genuine internal
    // invariant violation, not reachable caller input.
    if (dim == 0) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                     "search() reached with a zero-length dimension despite a "
                                                     "non-empty index -- an internal invariant was violated",
                                                     "vulkan_vector_index.zero_dimension_invariant"});
    }

    // Red-team pass 3 finding 3: reject (not silently truncate/overflow) a count/dimension this
    // backend's uint32 push constants or buffer-size arithmetic cannot represent, BEFORE any GPU
    // resource is touched.
    if (auto plausible = detail::check_gpu_dispatch_size_plausible(n, dim); !plausible.has_value()) {
        return std::unexpected(plausible.error());
    }
    // Red-team pass 5: the device's OWN limits, not just the uint32 representability bound above --
    // see detail::check_gpu_device_limits()'s header comment. Also before any GPU resource is touched.
    if (auto within = detail::check_gpu_device_limits(n, dim, impl_->max_storage_buffer_range,
                                                      impl_->max_workgroup_count_x);
        !within.has_value()) {
        return std::unexpected(within.error());
    }

    auto const create_host_visible_buffer =
        [&](VkDeviceSize size_bytes, VkBufferUsageFlags usage) -> std::optional<std::pair<VkBuffer, VkDeviceMemory>> {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size_bytes;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (AE_VK_FAULT(create_buffer) ||
            vkCreateBuffer(impl_->device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
            return std::nullopt;
        }

        VkMemoryRequirements mem_reqs;
        vkGetBufferMemoryRequirements(impl_->device, buffer, &mem_reqs);
        std::uint32_t const mem_type = impl_->find_memory_type(
            mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mem_type == UINT32_MAX) {
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = mem_type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (AE_VK_FAULT(allocate_memory) ||
            vkAllocateMemory(impl_->device, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }
        // Red-team pass 4 (2026-09-22), Critical finding 1: this call's VkResult was previously
        // discarded, in BOTH buffer-creation lambdas -- meaning EVERY buffer this class ever creates
        // (the vectors device-local buffer, its staging buffer, the query buffer, the scores buffer)
        // went through an unchecked bind. A real, in-spec bind failure (VK_ERROR_OUT_OF_HOST_MEMORY /
        // VK_ERROR_OUT_OF_DEVICE_MEMORY) would silently hand back a buffer/memory pair that looks
        // valid but has no memory actually bound to it -- using such a buffer in vkMapMemory,
        // vkCmdCopyBuffer, or a shader binding is undefined behavior per the Vulkan spec (a buffer must
        // have bound memory before use), the exact class of gap red-team pass 3 (§4c) already fixed
        // for this file's other Vulkan calls, just missed here. Reject, don't silently hand back an
        // unbound buffer.
        if (AE_VK_FAULT(bind_buffer_memory) || vkBindBufferMemory(impl_->device, buffer, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(impl_->device, memory, nullptr);
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }
        return std::make_pair(buffer, memory);
    };

    // DEVICE_LOCAL, not host-visible -- found necessary by this file's own second real-hardware run:
    // on the DISCRETE GPU this ran against (AMD Radeon RX 5300M, confirmed via `vulkaninfo`),
    // HOST_VISIBLE|HOST_COHERENT memory the shader reads from repeatedly (once per invocation, and
    // there are `n` invocations) routes through slow PCIe-mapped BAR memory rather than fast local
    // VRAM -- even AFTER the vectors-buffer-caching fix above, this kept GPU search several times
    // slower than CPU. The vectors buffer (read MANY times per dispatch, once per invocation) is the
    // one worth paying a staging-buffer upload for; the query buffer (`dim` floats, read once per
    // invocation but tiny) and scores buffer (write-only, `n` floats) are left host-visible --
    // small enough, and not obviously the bottleneck, so not worth the added complexity without a
    // real measurement showing they are (this codebase's own "a hot path without a bench... is not
    // done" bar cuts against optimizing a path with no evidence it needs it).
    auto const create_device_local_buffer =
        [&](VkDeviceSize size_bytes,
            VkBufferUsageFlags usage) -> std::optional<std::pair<VkBuffer, VkDeviceMemory>> {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size_bytes;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (AE_VK_FAULT(create_buffer) ||
            vkCreateBuffer(impl_->device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
            return std::nullopt;
        }

        VkMemoryRequirements mem_reqs;
        vkGetBufferMemoryRequirements(impl_->device, buffer, &mem_reqs);
        std::uint32_t mem_type =
            impl_->find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type == UINT32_MAX) {
            // No pure DEVICE_LOCAL type exists (a real, if uncommon, possibility -- some integrated
            // GPUs expose only a unified HOST_VISIBLE|DEVICE_LOCAL type). Fall back to whatever type
            // satisfies DEVICE_LOCAL alone being unavailable is not itself a failure worth rejecting
            // the whole search() call over -- retry with no property requirement at all, accepting
            // whatever the device's own default/first matching type is.
            mem_type = impl_->find_memory_type(mem_reqs.memoryTypeBits, 0);
        }
        if (mem_type == UINT32_MAX) {
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = mem_type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (AE_VK_FAULT(allocate_memory) ||
            vkAllocateMemory(impl_->device, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }
        // Red-team pass 4 (2026-09-22), Critical finding 1 -- same unchecked-bind gap as
        // create_host_visible_buffer() above, fixed identically here: this is the lambda that builds
        // the persistent, cached vectors buffer, the single largest and longest-lived GPU allocation
        // this class ever makes, so an unbound buffer surviving here would be the most consequential
        // instance of this gap, not merely a symmetric fix for its own sake.
        if (AE_VK_FAULT(bind_buffer_memory) || vkBindBufferMemory(impl_->device, buffer, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(impl_->device, memory, nullptr);
            vkDestroyBuffer(impl_->device, buffer, nullptr);
            return std::nullopt;
        }
        return std::make_pair(buffer, memory);
    };

    // ---- (Re)build the CACHED vectors buffer only when the corpus actually changed or grew past the
    // buffer's current capacity -- the fix for this file's own first-run finding (see Impl's own
    // field comment): re-uploading ~30 MB on every search() call, not just when add_batch() last ran,
    // was the real bottleneck that made GPU search ~10x SLOWER than CPU at n=5000. ---------------------
    if (impl_->cache_dirty || n > impl_->vectors_buffer_capacity) {
        // The OLD vectors_buffer/vectors_memory (if any) is destroyed further down, right before
        // being reassigned to the newly-built device-local buffer -- not here, so a failure in any
        // of the allocation/upload steps below leaves the OLD (still valid) buffer in place rather
        // than a torn/half-replaced cache.
        // Flatten the stored vectors row-major, matching cosine_similarity.comp's own layout
        // contract ("candidate i's own components live at [i*dimension, (i+1)*dimension)").
        // Red-team pass 5: each row is rescaled by an exact power of two on the way in (see
        // copy_rescaled_by_power_of_two()'s own comment) -- the stored `entries` stay untouched.
        std::vector<float> flattened(n * dim);
        for (std::size_t i = 0; i < n; ++i) {
            auto const& v = impl_->entries.at(impl_->order[i]);
            copy_rescaled_by_power_of_two(v, flattened.data() + i * dim);
        }

        VkDeviceSize const bytes = n * dim * sizeof(float);
        auto device_buf = create_device_local_buffer(
            bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!device_buf) {
            return std::unexpected(vulkan_error("failed to allocate the GPU vectors buffer (out of "
                                                 "device memory)",
                                                 "vulkan_vector_index.buffer_allocation_failed"));
        }
        auto staging_buf = create_host_visible_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!staging_buf) {
            vkDestroyBuffer(impl_->device, device_buf->first, nullptr);
            vkFreeMemory(impl_->device, device_buf->second, nullptr);
            return std::unexpected(
                vulkan_error("failed to allocate the host-visible staging buffer for the vectors upload",
                             "vulkan_vector_index.buffer_allocation_failed"));
        }

        // Red-team pass 3 (2026-09-22), Critical finding 1: EVERY call in this block used to be
        // fire-and-forget -- no VkResult/mapped-pointer check on vkMapMemory, vkAllocateCommandBuffers,
        // vkBeginCommandBuffer, vkEndCommandBuffer, vkCreateFence, or vkQueueSubmit. A real,
        // in-spec failure (host or device out-of-memory, a lost device) at any of those points was
        // either an unconditional std::memcpy(nullptr, ...) crash (the vkMapMemory case) or an
        // undefined-behavior vkWaitForFences call on a VK_NULL_HANDLE fence that was never actually
        // created -- neither is the typed `agentengine::result` error this codebase's fail-closed
        // posture requires. `cleanup` unwinds every transient resource created below (in reverse
        // order) on any early return; `dismiss()`d only once the buffer has been fully handed off to
        // `impl_->vectors_buffer`/`vectors_memory` below.
        CleanupStack cleanup;
        cleanup.push([this, device_buf] {
            vkDestroyBuffer(impl_->device, device_buf->first, nullptr);
            vkFreeMemory(impl_->device, device_buf->second, nullptr);
        });
        cleanup.push([this, staging_buf] {
            vkDestroyBuffer(impl_->device, staging_buf->first, nullptr);
            vkFreeMemory(impl_->device, staging_buf->second, nullptr);
        });

        {
            void* mapped = nullptr;
            if (AE_VK_FAULT(map_memory) ||
                vkMapMemory(impl_->device, staging_buf->second, 0, bytes, 0, &mapped) != VK_SUCCESS ||
                mapped == nullptr) {
                return std::unexpected(vulkan_error(
                    "vkMapMemory failed for the vectors staging buffer (host out of memory, or the "
                    "device was lost)",
                    "vulkan_vector_index.memory_map_failed"));
            }
            std::memcpy(mapped, flattened.data(), bytes);
            vkUnmapMemory(impl_->device, staging_buf->second);
        }

        // One-shot copy command: staging (host-visible) -> device_buf (device-local), then a barrier
        // so the compute shader's later reads see the copy's writes. This whole block runs only on a
        // cache rebuild (add_batch()-triggered), never on the per-search hot path.
        VkCommandBufferAllocateInfo copy_cmd_alloc_info{};
        copy_cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        copy_cmd_alloc_info.commandPool = impl_->command_pool;
        copy_cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        copy_cmd_alloc_info.commandBufferCount = 1;
        VkCommandBuffer copy_cmd = VK_NULL_HANDLE;
        if (AE_VK_FAULT(allocate_command_buffers) ||
            vkAllocateCommandBuffers(impl_->device, &copy_cmd_alloc_info, &copy_cmd) != VK_SUCCESS) {
            return std::unexpected(vulkan_error("vkAllocateCommandBuffers failed for the vectors-upload "
                                                 "copy command",
                                                 "vulkan_vector_index.command_buffer_allocation_failed"));
        }
        cleanup.push([this, copy_cmd] {
            VkCommandBuffer cmd = copy_cmd;
            vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd);
        });

        VkCommandBufferBeginInfo copy_begin_info{};
        copy_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        copy_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (AE_VK_FAULT(begin_command_buffer) || vkBeginCommandBuffer(copy_cmd, &copy_begin_info) != VK_SUCCESS) {
            return std::unexpected(vulkan_error("vkBeginCommandBuffer failed for the vectors-upload copy "
                                                 "command",
                                                 "vulkan_vector_index.command_buffer_begin_failed"));
        }

        VkBufferCopy copy_region{};
        copy_region.size = bytes;
        vkCmdCopyBuffer(copy_cmd, staging_buf->first, device_buf->first, 1, &copy_region);

        VkMemoryBarrier copy_barrier{};
        copy_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        copy_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                              &copy_barrier, 0, nullptr, 0, nullptr);

        if (AE_VK_FAULT(end_command_buffer) || vkEndCommandBuffer(copy_cmd) != VK_SUCCESS) {
            return std::unexpected(vulkan_error("vkEndCommandBuffer failed for the vectors-upload copy "
                                                 "command",
                                                 "vulkan_vector_index.command_buffer_end_failed"));
        }

        VkFenceCreateInfo copy_fence_info{};
        copy_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence copy_fence = VK_NULL_HANDLE;
        if (AE_VK_FAULT(create_fence) ||
            vkCreateFence(impl_->device, &copy_fence_info, nullptr, &copy_fence) != VK_SUCCESS) {
            return std::unexpected(vulkan_error("vkCreateFence failed for the vectors-upload copy command",
                                                 "vulkan_vector_index.fence_creation_failed"));
        }
        cleanup.push([this, copy_fence] { vkDestroyFence(impl_->device, copy_fence, nullptr); });

        VkSubmitInfo copy_submit_info{};
        copy_submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        copy_submit_info.commandBufferCount = 1;
        copy_submit_info.pCommandBuffers = &copy_cmd;
        if (VkResult const submit_result = AE_VK_FAULT(queue_submit)
                                               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                               : vkQueueSubmit(impl_->queue, 1, &copy_submit_info, copy_fence);
            submit_result != VK_SUCCESS) {
            return std::unexpected(vk_call_error(submit_result,
                                                 "vkQueueSubmit failed for the vectors-upload copy command",
                                                 "vulkan_vector_index.queue_submit_failed"));
        }
        VkResult wait_result = vkWaitForFences(impl_->device, 1, &copy_fence, VK_TRUE, UINT64_MAX);
        if (AE_VK_FAULT(wait_for_fences)) wait_result = VK_ERROR_DEVICE_LOST;  // test seam: the real wait already ran
        if (wait_result != VK_SUCCESS) {
            // Only reachable via VK_ERROR_DEVICE_LOST/VK_ERROR_OUT_OF_*_MEMORY at UINT64_MAX timeout
            // (VK_TIMEOUT cannot occur at an infinite timeout) -- still checked, not assumed
            // unreachable, matching this file's own "reject-not-coerce on any structural
            // inconsistency" posture rather than trusting an infinite wait can only ever succeed.
            return std::unexpected(vk_call_error(wait_result,
                                                 "vkWaitForFences failed for the vectors-upload copy "
                                                 "command (the device may have been lost)",
                                                 "vulkan_vector_index.fence_wait_failed"));
        }

        cleanup.dismiss();  // handed off below: device_buf -> impl_->vectors_buffer/memory; staging
                             // buffer, copy command, and copy fence are all genuinely done with now.
        vkDestroyFence(impl_->device, copy_fence, nullptr);
        vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &copy_cmd);
        vkDestroyBuffer(impl_->device, staging_buf->first, nullptr);
        vkFreeMemory(impl_->device, staging_buf->second, nullptr);

        if (impl_->vectors_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(impl_->device, impl_->vectors_buffer, nullptr);
            vkFreeMemory(impl_->device, impl_->vectors_memory, nullptr);
        }
        impl_->vectors_buffer = device_buf->first;
        impl_->vectors_memory = device_buf->second;
        impl_->vectors_buffer_capacity = n;
        impl_->cache_dirty = false;
    }

    // ---- Query buffer: tiny (`dim` floats), created once and reused -- `dim` is fixed once the
    // first vector is ever added (BruteForceCosineIndex's own identical invariant), so this never
    // needs to grow in practice. ------------------------------------------------------------------
    if (impl_->query_buffer == VK_NULL_HANDLE) {
        auto buf = create_host_visible_buffer(dim * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!buf) {
            return std::unexpected(vulkan_error("failed to allocate the GPU query buffer",
                                                  "vulkan_vector_index.buffer_allocation_failed"));
        }
        impl_->query_buffer = buf->first;
        impl_->query_memory = buf->second;
    }
    {
        void* mapped = nullptr;
        if (AE_VK_FAULT(map_memory) ||
            vkMapMemory(impl_->device, impl_->query_memory, 0, dim * sizeof(float), 0, &mapped) !=
                VK_SUCCESS ||
            mapped == nullptr) {
            return std::unexpected(vulkan_error("vkMapMemory failed for the GPU query buffer",
                                                  "vulkan_vector_index.memory_map_failed"));
        }
        // Red-team pass 5: the query gets the same exact power-of-two rescale every stored row gets.
        copy_rescaled_by_power_of_two(query, static_cast<float*>(mapped));
        vkUnmapMemory(impl_->device, impl_->query_memory);
    }

    // ---- Scores buffer: grow-only capacity, tracking the vectors buffer's own `n`. -----------------
    // Red-team pass 5 (2026-09-23), Critical: this block used to destroy the OLD scores buffer FIRST
    // and only then try to create the new one -- so a failed creation returned an error while
    // `impl_->scores_buffer`/`scores_memory` still held the just-DESTROYED handles (and the old, now
    // too-small capacity). The next search() on the same instance re-entered this block (n still
    // exceeds that capacity) and called vkDestroyBuffer/vkFreeMemory on those handles a SECOND time,
    // and ~Impl() would have done it again: a double-destroy/double-free of driver objects, undefined
    // behavior per the Vulkan spec. Reproduced by this pass's fault-injection test under the Khronos
    // validation layer. Now build-then-swap, the same ordering the vectors-buffer rebuild above
    // already used: the old buffer is released only after its replacement exists, so a failure
    // leaves the old, still-valid buffer (and its capacity) exactly as they were.
    if (impl_->scores_buffer == VK_NULL_HANDLE || n > impl_->scores_buffer_capacity) {
        auto buf = create_host_visible_buffer(n * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!buf) {
            return std::unexpected(vulkan_error("failed to allocate the GPU scores buffer",
                                                  "vulkan_vector_index.buffer_allocation_failed"));
        }
        if (impl_->scores_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(impl_->device, impl_->scores_buffer, nullptr);
            vkFreeMemory(impl_->device, impl_->scores_memory, nullptr);
        }
        impl_->scores_buffer = buf->first;
        impl_->scores_memory = buf->second;
        impl_->scores_buffer_capacity = n;
    }

    // ---- Descriptor set: allocate once, re-write bindings every call (cheap CPU-side call, and a
    // buffer identity may have just changed above on a regrow) -----------------------------------
    if (impl_->descriptor_set == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ds_alloc_info{};
        ds_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds_alloc_info.descriptorPool = impl_->descriptor_pool;
        ds_alloc_info.descriptorSetCount = 1;
        ds_alloc_info.pSetLayouts = &impl_->descriptor_set_layout;
        if (AE_VK_FAULT(allocate_descriptor_sets) ||
            vkAllocateDescriptorSets(impl_->device, &ds_alloc_info, &impl_->descriptor_set) != VK_SUCCESS) {
            impl_->descriptor_set = VK_NULL_HANDLE;  // never cache a handle from a failed allocation
            return std::unexpected(vulkan_error("vkAllocateDescriptorSets failed",
                                                  "vulkan_vector_index.descriptor_set_allocation_failed"));
        }
    }
    VkDescriptorBufferInfo buffer_infos[3] = {
        {impl_->query_buffer, 0, VK_WHOLE_SIZE},
        {impl_->vectors_buffer, 0, VK_WHOLE_SIZE},
        {impl_->scores_buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = impl_->descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(impl_->device, 3, writes, 0, nullptr);

    // ---- Command buffer: allocate once, re-record every call (cheap -- CPU-side recording only) ----
    if (impl_->command_buffer == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo cmd_alloc_info{};
        cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc_info.commandPool = impl_->command_pool;
        cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc_info.commandBufferCount = 1;
        if (AE_VK_FAULT(allocate_command_buffers) ||
            vkAllocateCommandBuffers(impl_->device, &cmd_alloc_info, &impl_->command_buffer) != VK_SUCCESS) {
            impl_->command_buffer = VK_NULL_HANDLE;  // a failed allocate() may still write a non-null,
                                                       // invalid handle on some drivers -- never cache one
            return std::unexpected(vulkan_error("vkAllocateCommandBuffers failed for the dispatch command",
                                                 "vulkan_vector_index.command_buffer_allocation_failed"));
        }
    } else if (AE_VK_FAULT(reset_command_buffer) || vkResetCommandBuffer(impl_->command_buffer, 0) != VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkResetCommandBuffer failed for the cached dispatch command",
                                             "vulkan_vector_index.command_buffer_reset_failed"));
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (AE_VK_FAULT(begin_command_buffer) || vkBeginCommandBuffer(impl_->command_buffer, &begin_info) != VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkBeginCommandBuffer failed for the dispatch command",
                                             "vulkan_vector_index.command_buffer_begin_failed"));
    }

    vkCmdBindPipeline(impl_->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
    vkCmdBindDescriptorSets(impl_->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline_layout, 0, 1,
                              &impl_->descriptor_set, 0, nullptr);
    struct { std::uint32_t count; std::uint32_t dimension; } push_constants{
        static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(dim)};
    vkCmdPushConstants(impl_->command_buffer, impl_->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(push_constants), &push_constants);
    // cosine_similarity.comp: local_size_x = 64 -- one workgroup per 64 candidates, rounded up.
    std::uint32_t const group_count = static_cast<std::uint32_t>((n + 63) / 64);
    vkCmdDispatch(impl_->command_buffer, group_count, 1, 1);

    // SHADER_WRITE -> HOST_READ: the correct, explicit memory-domain barrier for a host readback
    // after a compute write, even over host-coherent memory (coherency covers cache visibility, not
    // execution/memory-access ordering, which this barrier provides).
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(impl_->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                          1, &barrier, 0, nullptr, 0, nullptr);

    if (AE_VK_FAULT(end_command_buffer) || vkEndCommandBuffer(impl_->command_buffer) != VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkEndCommandBuffer failed for the dispatch command",
                                             "vulkan_vector_index.command_buffer_end_failed"));
    }

    // Fence: created and destroyed per call (cheap -- unlike buffers, a fence carries no large
    // device-memory allocation, so this was never the bottleneck the vectors buffer was).
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (AE_VK_FAULT(create_fence) || vkCreateFence(impl_->device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        return std::unexpected(vulkan_error("vkCreateFence failed for the dispatch command",
                                             "vulkan_vector_index.fence_creation_failed"));
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &impl_->command_buffer;
    if (VkResult const submit_result = AE_VK_FAULT(queue_submit)
                                           ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                           : vkQueueSubmit(impl_->queue, 1, &submit_info, fence);
        submit_result != VK_SUCCESS) {
        vkDestroyFence(impl_->device, fence, nullptr);
        return std::unexpected(vk_call_error(submit_result, "vkQueueSubmit failed for the dispatch command",
                                             "vulkan_vector_index.queue_submit_failed"));
    }

    // No explicit timeout -- a hung GPU driver is an environment failure this call has no sound way
    // to recover from short of a process-level watchdog, out of scope for this conformer; matches
    // this codebase's own "resource-capped" posture applying to hostile INPUT, not to the host's own
    // driver misbehaving. Red-team pass 3: the RETURN VALUE is now checked even so -- VK_ERROR_DEVICE_LOST
    // is a real, in-spec way for this call to stop waiting without the fence ever having signaled, and
    // that must become a typed error, not a silent fall-through into reading an unwritten scores buffer.
    VkResult wait_result = vkWaitForFences(impl_->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (AE_VK_FAULT(wait_for_fences)) wait_result = VK_ERROR_DEVICE_LOST;  // test seam: the real wait already ran
    if (wait_result != VK_SUCCESS) {
        vkDestroyFence(impl_->device, fence, nullptr);
        return std::unexpected(vk_call_error(wait_result,
                                             "vkWaitForFences failed for the dispatch command (the "
                                             "device may have been lost)",
                                             "vulkan_vector_index.fence_wait_failed"));
    }
    vkDestroyFence(impl_->device, fence, nullptr);

    std::vector<float> scores(n);
    {
        void* mapped = nullptr;
        if (AE_VK_FAULT(map_memory) ||
            vkMapMemory(impl_->device, impl_->scores_memory, 0, n * sizeof(float), 0, &mapped) !=
                VK_SUCCESS ||
            mapped == nullptr) {
            return std::unexpected(vulkan_error("vkMapMemory failed for the GPU scores buffer readback",
                                                  "vulkan_vector_index.memory_map_failed"));
        }
        std::memcpy(scores.data(), mapped, n * sizeof(float));
        vkUnmapMemory(impl_->device, impl_->scores_memory);
    }

    std::vector<agentengine::ScoredId> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back({impl_->order[i], scores[i]});
    sort_and_truncate(out, k);
    return out;
}

}  // namespace agentengine::backends::vulkan_vector_index
