// Milestone 2 Phase D task D3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md),
// decisions/ADR-010-wasm-component-host-manifest-capability-binding.md §7.3 -- proves the real
// WasmBackend implementation (src/backends/wasm/wasm_backend.{hpp,cpp}) against a genuinely
// compiled `ae:tool` component (tests/fixtures/wasm_ae_tool_fixture/), not a hand-crafted stub:
//
//   1. Positive: a manifest whose requested capabilities cover the component's real imports loads,
//      lists its real tools, and invoke_tool() returns a real computed result for both a
//      zero-capability tool ("echo") and a capability-gated one ("now", ae:tool/clock).
//   2. Negative (009 §10 G2 miniature -- D5 is the full dedicated suite): the same component with a
//      manifest that omits Clock fails closed at load_component(), never reaching instantiate.
//   3. Capability-kind confusion (ADR-010 §5 F3): a manifest that grants Entropy before Clock, so
//      the guest's first capability-handle slot is bound to the WRONG kind for what "now" calls --
//      proven via the real fixture and real host callback, not a test-only backdoor.
//   4. wall_ms is a real, measured kill (claim 5): the "spin" tool loops forever; a low wall_ms
//      limit must actually interrupt it within a bounded, measured time.
//
// SKIPs (CTest SKIP_RETURN_CODE 77, tests/CMakeLists.txt) if the fixture wasn't built -- the
// cargo-component toolchain is not assumed present on every machine running this suite.

#include "backends/wasm/wasm_backend.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

using namespace agentengine;
using namespace agentengine::wasm;

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

std::vector<std::uint8_t> read_fixture() {
    std::ifstream f(AE_WASM_FIXTURE_PATH, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "test-trace";
    ctx.span_id = "test-span";
    return ctx;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> const bytes = read_fixture();
    if (bytes.empty()) {
        std::cerr << "SKIP: " << AE_WASM_FIXTURE_PATH
                  << " not found -- cargo-component toolchain unavailable, see "
                     "tests/fixtures/wasm_ae_tool_fixture/README.md\n";
        return 77;
    }
    std::cout << "fixture: " << bytes.size() << " bytes\n";

    // -- 1. Positive: full grant -----------------------------------------------------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({cap::Clock{}, cap::Entropy{}});
        spec.limits.memory_bytes = 64ull * 1024 * 1024;

        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "positive: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.echo-now-spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = {cap::Clock{}};
        manifest.memory_bytes_limit = spec.limits.memory_bytes;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "positive: load_component() succeeds (manifest covers real imports)");

        auto tools = backend.list_tools(*handle, ctx);
        if (!tools) std::cerr << "  (list_tools error: " << tools.error().code << ": " << tools.error().message << ")\n";
        AE_CHECK(tools.has_value() && tools->size() == 3, "positive: list_tools() returns all 3 real tools");
        if (tools) {
            bool has_echo = false, has_now = false, has_spin = false, echo_parallelizable = false;
            for (auto const& t : *tools) {
                if (t.name == "echo") { has_echo = true; echo_parallelizable = t.parallelizable; }
                if (t.name == "now") has_now = true;
                if (t.name == "spin") has_spin = true;
            }
            AE_CHECK(has_echo && has_now && has_spin, "positive: tool names match the fixture's real exports");
            AE_CHECK(echo_parallelizable, "positive: echo's parallelizable flag round-trips true");
        }

        auto echo_result = backend.invoke_tool(*handle, ToolInvokeRequest{"echo", "hello from the host"}, ctx);
        if (!echo_result) std::cerr << "  (echo error: " << echo_result.error().code << ": " << echo_result.error().message << ")\n";
        AE_CHECK(echo_result.has_value() && !echo_result->is_error && echo_result->content.size() == 1,
                  "positive: echo invoke succeeds with one content item");
        if (echo_result && !echo_result->content.empty()) {
            auto const* text = std::get_if<Text>(&echo_result->content[0].value);
            AE_CHECK(text != nullptr && text->text == "hello from the host",
                      "positive: echo returns the exact input text (real computation, not a stub)");
        }

        auto const before_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        auto now_result = backend.invoke_tool(*handle, ToolInvokeRequest{"now", ""}, ctx);
        auto const after_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        AE_CHECK(now_result.has_value() && !now_result->is_error && now_result->content.size() == 1,
                  "positive: now invoke succeeds (real ae:tool/clock host call)");
        if (now_result && !now_result->content.empty()) {
            auto const* data = std::get_if<Data>(&now_result->content[0].value);
            AE_CHECK(data != nullptr, "positive: now returns a Data content item");
            if (data) {
                long long const millis = std::atoll(data->json.c_str());
                AE_CHECK(millis >= before_now && millis <= after_now,
                          "positive: now returns a plausible real timestamp, not a placeholder");
            }
        }

        backend.destroy(*handle);
    }

    // -- 2. Negative: manifest omits Clock, component still imports ae:tool/clock ---------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({cap::Clock{}});  // operator WOULD allow it...
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "negative: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.negative";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = {};  // ...but the MANIFEST doesn't request it -- fail closed.

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(!loaded.has_value(), "negative: load_component() fails closed");
        if (!loaded) {
            std::cerr << "  (negative error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
            AE_CHECK(loaded.error().code == "wasm.manifest_capability_not_requested",
                      "negative: specific diagnosis naming the offending import, not a generic refusal");
        }

        // No partial success state: list_tools()/invoke_tool() on this never-loaded handle must
        // also refuse, proving the rejection really happened before anything else was set up.
        auto tools = backend.list_tools(*handle, ctx);
        AE_CHECK(!tools.has_value(), "negative: list_tools() on a rejected handle also fails (no partial load)");

        backend.destroy(*handle);
    }

    // -- 3. Capability-kind confusion: Entropy bound where Clock is expected ---------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({cap::Entropy{}, cap::Clock{}});
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.kind-confusion";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // Order matters: capabilities[0] (what the fixture's "now" tool actually calls
        // now-unix-millis with) is bound to Entropy here, not Clock.
        manifest.requested_capabilities = {cap::Entropy{}, cap::Clock{}};

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "kind-confusion: load_component() still succeeds (both kinds are granted)");

        auto now_result = backend.invoke_tool(*handle, ToolInvokeRequest{"now", ""}, ctx);
        AE_CHECK(!now_result.has_value(),
                  "kind-confusion: now-unix-millis rejects an Entropy handle passed where Clock is required");

        backend.destroy(*handle);
    }

    // -- 4. wall_ms is a real, measured kill -------------------------------------------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // The fixture's import list is component-wide, not per-tool (ADR-010 §7.5's finding) --
        // "now"'s ae:tool/clock import must still be granted even though "spin" itself never calls
        // it, or load_component() fails closed before "spin" is ever reachable.
        spec.capabilities = CapabilitySet::grant_root({cap::Clock{}});
        spec.limits.wall_ms = 200;
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = {cap::Clock{}};
        manifest.wall_ms_limit = 200;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        if (!loaded) std::cerr << "  (wall_ms load error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
        AE_CHECK(loaded.has_value(), "wall_ms: load_component() succeeds (spin needs no capability)");

        auto const started = std::chrono::steady_clock::now();
        auto spin_result = backend.invoke_tool(*handle, ToolInvokeRequest{"spin", ""}, ctx);
        auto const elapsed = std::chrono::steady_clock::now() - started;

        AE_CHECK(!spin_result.has_value(), "wall_ms: spin is actually interrupted, not left running forever");
        auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        AE_CHECK(elapsed_ms < 5000,
                  "wall_ms: interrupted within a bounded, measured time (well under a runaway 5s ceiling)");
        std::cout << "  (spin interrupted after " << elapsed_ms << "ms, limit was 200ms)\n";

        backend.destroy(*handle);
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
