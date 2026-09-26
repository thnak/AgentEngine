// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §4e (red-team pass 5,
// 2026-09-23) -- EXECUTES every checked-VkResult failure branch in VulkanCosineIndex
// (src/backends/vulkan_vector_index/vulkan_cosine_index.cpp), which every prior red-team pass (§4c,
// §4d) could only reason about: a real device-lost/out-of-memory cannot be induced safely on a dev box
// (CLAUDE.md "machine safety"), so those branches had NEVER run. This binary links a separate build of
// that .cpp compiled with AGENTENGINE_VULKAN_FAULT_INJECTION (tests/CMakeLists.txt), whose test-only
// seam (vulkan_cosine_index.hpp, `detail::arm_fault_injection`) makes the Nth call of a named Vulkan
// entry point report failure. The production library never defines that macro.
//
// For every fault point and every N at which it is actually reached, across three phases (first
// search = full cache build; growth = cache rebuild + scores-buffer regrow + cached-command-buffer
// reset; steady state = no rebuild), this asserts:
//   1. the faulted call returns a typed `vulkan_vector_index.*` error -- never a crash;
//   2. the SAME instance then recovers: the next, un-faulted search() returns the exact expected
//      ranking (proves no torn/half-replaced cache state and no dangling cached handle survived);
//   3. every fault point was reached at least once (positive control: the seam really covers each
//      branch -- a point that never fires would make (1)/(2) vacuous for it).
// And, when the Khronos validation layer is available (Vulkan SDK installed), the WHOLE run executes
// under it and must log ZERO validation errors -- this is what catches a Vulkan-side double-destroy /
// use-after-free that host ASan cannot see (driver objects, not heap). Positive control for THAT: the
// layer's own "Khronos Validation Layer Active" banner must appear in its log ONCE PER VkInstance this
// run created, or the run fails rather than silently claiming clean. That per-instance count is
// load-bearing, found the hard way while writing this test: the layer TRUNCATES its log file at every
// vkCreateInstance, so a single end-of-run read saw only the LAST instance's messages -- a first draft
// of this test (one shared log, one banner check) passed even against a build with the scores-buffer
// double-destroy re-introduced. Every create() therefore gets its OWN log file (VK_LAYER_LOG_FILENAME
// is re-pointed before each call), all read at the end.
//
// Resource-capped (CLAUDE.md "machine safety"): every index holds at most 5 three-dimensional vectors.

#if defined(AGENTENGINE_WITH_VULKAN) && defined(AGENTENGINE_VULKAN_FAULT_INJECTION)

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "backends/vulkan_vector_index/vulkan_cosine_index.hpp"

using namespace agentengine;
using namespace agentengine::backends::vulkan_vector_index;
using detail::fault_point;

namespace {

int g_failures = 0;
int g_checks = 0;
#define AE_CHECK(cond, label)                                                                   \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(cond)) {                                                                           \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"      \
                      << __LINE__ << "\n";                                                       \
            ++g_failures;                                                                        \
        }                                                                                        \
    } while (0)

void set_env(char const* name, std::string const& value) {
#ifdef _WIN32
    (void)_putenv_s(name, value.c_str());
#else
    (void)setenv(name, value.c_str(), 1);
#endif
}

// Returns a fresh, empty directory for per-instance validation logs when the Khronos validation layer
// was found and enabled for this process, or an empty path when it is unavailable (the run then
// proves typed errors + recovery only). A caller who already set VK_INSTANCE_LAYERS keeps theirs.
std::filesystem::path enable_validation_layer_if_available() {
    auto const sdk = agentengine::pal::env_var("VULKAN_SDK");
    auto const existing_layers = agentengine::pal::env_var("VK_INSTANCE_LAYERS");
    if (!sdk.has_value() || (existing_layers.has_value() && !existing_layers->empty())) return {};
    std::filesystem::path const candidates[] = {
        std::filesystem::path(*sdk) / "Bin",
        std::filesystem::path(*sdk) / "share" / "vulkan" / "explicit_layer.d",
        std::filesystem::path(*sdk) / "etc" / "vulkan" / "explicit_layer.d",
    };
    for (auto const& dir : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(dir / "VkLayer_khronos_validation.json", ec)) continue;
        auto const logs = std::filesystem::temp_directory_path(ec) / "ae_vulkan_fault_injection_validation";
        std::filesystem::remove_all(logs, ec);
        std::filesystem::create_directories(logs, ec);
        if (ec) return {};
        set_env("VK_LAYER_PATH", dir.string());
        set_env("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation");
        set_env("VK_LAYER_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG");
        set_env("VK_LAYER_REPORT_FLAGS", "error,info");  // info: the liveness banner (positive control)
        return logs;
    }
    return {};
}

struct Point {
    fault_point point;
    char const* name;
};

constexpr Point kSearchPoints[] = {
    {fault_point::create_buffer, "vkCreateBuffer"},
    {fault_point::allocate_memory, "vkAllocateMemory"},
    {fault_point::bind_buffer_memory, "vkBindBufferMemory"},
    {fault_point::map_memory, "vkMapMemory"},
    {fault_point::allocate_command_buffers, "vkAllocateCommandBuffers"},
    {fault_point::begin_command_buffer, "vkBeginCommandBuffer"},
    {fault_point::end_command_buffer, "vkEndCommandBuffer"},
    {fault_point::create_fence, "vkCreateFence"},
    {fault_point::queue_submit, "vkQueueSubmit"},
    {fault_point::wait_for_fences, "vkWaitForFences"},
    {fault_point::reset_command_buffer, "vkResetCommandBuffer"},
    {fault_point::allocate_descriptor_sets, "vkAllocateDescriptorSets"},
};

constexpr Point kCreatePoints[] = {
    {fault_point::create_instance, "vkCreateInstance"},
    {fault_point::enumerate_physical_devices, "vkEnumeratePhysicalDevices"},
    {fault_point::create_device, "vkCreateDevice"},
    {fault_point::create_command_pool, "vkCreateCommandPool"},
    {fault_point::create_descriptor_set_layout, "vkCreateDescriptorSetLayout"},
    {fault_point::create_descriptor_pool, "vkCreateDescriptorPool"},
    {fault_point::create_pipeline_layout, "vkCreatePipelineLayout"},
    {fault_point::create_shader_module, "vkCreateShaderModule"},
    {fault_point::create_compute_pipeline, "vkCreateComputePipelines"},
};

constexpr float kQuery[3] = {1.0f, 0.0f, 0.0f};

// Query {1,0,0}: a (1.0), d (0.707), b (0.0), e (0.0, tie -> id asc after b), c (-1.0).
bool ranking_is(result<std::vector<ScoredId>> const& r, std::vector<std::string> const& expected) {
    if (!r.has_value() || r->size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if ((*r)[i].id != expected[i]) return false;
    }
    return std::fabs(r->front().score - 1.0f) < 1e-5f;
}

std::vector<std::string> const kPhase1Expected{"a", "b", "c"};
std::vector<std::string> const kPhase2Expected{"a", "d", "b", "e", "c"};

bool is_typed_backend_error(result<std::vector<ScoredId>> const& r) {
    return !r.has_value() && r.error().code.rfind("vulkan_vector_index.", 0) == 0 && !r.error().message.empty();
}

// Validation-log bookkeeping -- see the top comment for why every instance gets its own file.
struct ValidationTally {
    std::filesystem::path dir;
    int instances = 0;  // VkInstances this run created (each must leave exactly one banner)
    int next_log = 0;
    int banners = 0;
    int errors = 0;
};
ValidationTally g_validation;

// Call immediately before EVERY VulkanCosineIndex::create().
void use_fresh_validation_log() {
    if (g_validation.dir.empty()) return;
    set_env("VK_LAYER_LOG_FILENAME",
            (g_validation.dir / ("instance_" + std::to_string(++g_validation.next_log) + ".log")).string());
}

void read_validation_logs() {
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(g_validation.dir, ec)) {
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("Khronos Validation Layer Active") != std::string::npos) ++g_validation.banners;
            if (line.rfind("Validation Error", 0) == 0) {
                ++g_validation.errors;
                if (g_validation.errors <= 5) std::cerr << "  [" << entry.path().filename().string() << "] " << line << "\n";
            }
        }
    }
}

// Runs one faulted search() on `idx`, then an un-faulted recovery search(). Returns whether the
// fault actually fired (the point was reached at least `nth` times during the faulted call).
bool faulted_then_recovered(VulkanCosineIndex& idx, Point const& p, unsigned nth,
                            std::vector<std::string> const& expected, std::string const& where) {
    detail::arm_fault_injection(p.point, nth);
    auto const faulted = idx.search(std::span<float const>(kQuery, 3), 10);
    unsigned const hits = detail::fault_injection_hits(p.point);
    detail::disarm_fault_injection();
    bool const fired = nth <= hits;
    std::string const label = std::string(p.name) + " #" + std::to_string(nth) + " (" + where + ")";
    if (fired) {
        AE_CHECK(is_typed_backend_error(faulted), label + ": faulted search() returns a typed error, not a crash");
    } else {
        AE_CHECK(ranking_is(faulted, expected), label + ": point not reached this often -- search() unaffected");
    }
    auto const recovered = idx.search(std::span<float const>(kQuery, 3), 10);
    AE_CHECK(ranking_is(recovered, expected),
             label + ": the SAME instance recovers -- the next search() returns the exact expected ranking");
    return fired;
}

}  // namespace

int main() {
    g_validation.dir = enable_validation_layer_if_available();

    {
        use_fresh_validation_log();
        auto probe = VulkanCosineIndex::create();
        if (!probe.has_value()) {
            std::cerr << "test_vulkan_cosine_index_fault_injection: SKIPPED -- " << probe.error().message << " ("
                      << probe.error().code << ")\n";
            return 0;
        }
        ++g_validation.instances;
    }

    // ---- create(): every step's failure returns a typed error and tears down whatever it built ----
    for (auto const& p : kCreatePoints) {
        bool fired_any = false;
        for (unsigned nth = 1; nth <= 2; ++nth) {
            use_fresh_validation_log();
            detail::arm_fault_injection(p.point, nth);
            auto created = VulkanCosineIndex::create();
            unsigned const hits = detail::fault_injection_hits(p.point);
            detail::disarm_fault_injection();
            std::string const label = std::string("create(): ") + p.name + " #" + std::to_string(nth);
            // Every create() reaches vkCreateInstance except one whose OWN vkCreateInstance was faulted.
            if (!(p.point == fault_point::create_instance && nth <= hits)) ++g_validation.instances;
            if (nth <= hits) {
                fired_any = true;
                AE_CHECK(!created.has_value() && created.error().code.rfind("vulkan_vector_index.", 0) == 0,
                         label + " fails closed with a typed error");
            } else {
                AE_CHECK(created.has_value(), label + " not reached this often -- create() succeeds");
            }
        }
        AE_CHECK(fired_any, std::string("positive control: create()'s ") + p.name + " fault point is reachable");
    }

    // ---- search(): three phases per (point, nth) --------------------------------------------------
    for (auto const& p : kSearchPoints) {
        bool fired_any = false;
        for (unsigned nth = 1; nth <= 8; ++nth) {
            bool fired = false;
            {
                use_fresh_validation_log();
                auto created = VulkanCosineIndex::create();
                AE_CHECK(created.has_value(), "setup: create()");
                if (!created.has_value()) break;
                ++g_validation.instances;
                VulkanCosineIndex idx = std::move(*created);
                AE_CHECK(idx.add_batch({"a", "b", "c"},
                                       {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}})
                             .has_value(),
                         "setup: phase-1 add_batch");
                fired = faulted_then_recovered(idx, p, nth, kPhase1Expected, "first search: full cache build");
                // Growth: forces the vectors-buffer rebuild, the scores-buffer REGROW (n 3 -> 5 exceeds its
                // capacity -- the path whose failure used to leave a destroyed handle cached), and the
                // cached dispatch command buffer's reset path.
                AE_CHECK(idx.add_batch({"d", "e"}, {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}).has_value(),
                         "setup: phase-2 add_batch");
                fired = faulted_then_recovered(idx, p, nth, kPhase2Expected,
                                               "growth: rebuild + scores regrow") || fired;
                fired = faulted_then_recovered(idx, p, nth, kPhase2Expected,
                                               "steady state: no rebuild") || fired;
            }  // idx (and its VkInstance) destroyed here -- ~Impl()'s teardown after a fault is validated too
            fired_any = fired_any || fired;
            if (!fired) break;  // N exceeds every phase's hit count -- larger N cannot fire either
        }
        AE_CHECK(fired_any, std::string("positive control: search()'s ") + p.name + " fault point is reachable");
    }

    // ---- Validation-layer verdict ----------------------------------------------------------------
    if (g_validation.dir.empty()) {
        std::cout << "  NOTE: Khronos validation layer not found (no VULKAN_SDK) -- this run proves typed "
                     "errors + functional recovery only, NOT the absence of Vulkan-side invalid usage.\n";
    } else {
        read_validation_logs();
        std::cout << "  validation layer: " << g_validation.banners << " banners for " << g_validation.instances
                  << " instances, " << g_validation.errors << " validation errors\n";
        AE_CHECK(g_validation.instances > 0 && g_validation.banners == g_validation.instances,
                 "positive control: the validation layer really ran for EVERY VkInstance this test created "
                 "(one banner per instance survived into the drained logs)");
        AE_CHECK(g_validation.errors == 0,
                 "zero Vulkan validation errors across every injected failure, every recovery, and every "
                 "teardown (" + std::to_string(g_validation.errors) + " found)");
    }

    std::cout << "test_vulkan_cosine_index_fault_injection: " << g_checks << " checks, " << g_failures
              << " failures\n";
    std::cout << (g_failures == 0 ? "test_vulkan_cosine_index_fault_injection: OK\n"
                                  : "test_vulkan_cosine_index_fault_injection: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() { return 0; }

#endif
