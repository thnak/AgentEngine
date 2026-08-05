// Milestone 2 Phase D tasks D3 and D5 (docs/planning/milestone-2-tools-capabilities-sandbox-
// breakdown.md), decisions/ADR-010-wasm-component-host-manifest-capability-binding.md §7.3 --
// proves the real WasmBackend implementation (src/backends/wasm/wasm_backend.{hpp,cpp}) against a
// genuinely compiled `ae:tool` component (tests/fixtures/wasm_ae_tool_fixture/), not a hand-crafted
// stub. D5 extended the fixture with four tools ("read-file"/"write-file"/"fetch"/"get-secret",
// importing ae:tool/{fs,http,secrets}) purely so its own gated-callback probes below have something
// real to call -- the component's whole import set (capability/fs/http/secrets/clock/types) now
// applies to every test in this file, not just the ones that exercise the new tools.
//
//   1. Positive: a manifest whose requested capabilities cover the component's real imports loads,
//      lists its real tools, and invoke_tool() returns a real computed result for both a
//      zero-capability tool ("echo") and a capability-gated one ("now", ae:tool/clock).
//   2. Negative, branch 1 (009 §10 G2 / ADR-010 claim 2, "manifest under-requests"): a manifest that
//      omits Clock, while the operator would have granted it, fails closed at load_component() with
//      `wasm.manifest_capability_not_requested`, never reaching instantiate.
//   3. Negative, branch 2 (D5 -- the same claim's other half, untested by D3): a manifest that DOES
//      request FsRead, but the operator's CapabilitySet does not grant it, fails closed with
//      `wasm.operator_grant_missing`.
//   4. Capability-kind confusion (ADR-010 §5 F3 / claim 4), clock: a manifest that grants Entropy
//      before Clock, so the guest's first capability-handle slot is bound to the WRONG kind for what
//      "now" calls -- proven via the real fixture and real host callback, not a test-only backdoor.
//   5. Capability-kind confusion, the other four gated callbacks (D5 closes claim 4's remaining gap
//      -- D3's ADR explicitly left this proven for only one of five callbacks): for each of
//      fs-read/fs-write/http-request/resolve-secret, one probe places the matching capability first
//      (right kind -- reaches the callback's "not implemented" stub, proving the kind check passed)
//      and one places a mismatched capability first (wrong kind -- rejected before reaching the
//      stub), against the real fixture's read-file/write-file/fetch/get-secret tools.
//   6. wall_ms is a real, measured kill (claim 5): the "spin" tool loops forever; a low wall_ms
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

// The D5-extended fixture's real, whole-component import set (capability/types are always_ok and
// need no capability; fs/http/secrets/clock do) -- every test in this file that loads the component
// at all must cover this exact set, regardless of which tool it goes on to invoke (ADR-010 §3.2's
// found imprecision: imports are component-wide, not per-tool). Clock listed first so tests that
// invoke "now" (which reads request.capabilities.first()) keep getting the kind it expects; the
// per-callback probes below (§5) define their own orderings deliberately instead of using this one.
std::vector<Capability> const kAllRequiredCapabilities = {
    cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{},
};

// One gated-callback kind-check probe (D5, closing ADR-010 claim 4's remaining four-callback gap):
// loads the real fixture with `order` as both the operator grant and the manifest request -- so
// `order[0]` is exactly what `tool`'s `request.capabilities.first()` resolves to inside the guest
// (invoke_tool() binds capabilities in manifest.requested_capabilities order) -- invokes `tool`, and
// checks the resulting failure's message contains `expect_substring`. Every probe's `order` still
// contains the component's whole real import set (kAllRequiredCapabilities, reordered), or
// load_component() itself would reject it before the probe ever reaches invoke_tool().
void probe_gated_callback(std::vector<std::uint8_t> const& bytes, std::string const& label,
                           std::string const& tool, std::vector<Capability> const& order,
                           std::string const& expect_substring) {
    WasmBackend backend;
    SandboxSpec spec;
    spec.capabilities = CapabilitySet::grant_root(order);
    EffectContext ctx = make_ctx();
    auto handle = backend.create(spec, ctx);
    if (!handle) {
        AE_CHECK(false, label + ": create() succeeds");
        return;
    }

    PluginManifest manifest;
    manifest.id = "test.gated." + tool + "." + label;
    manifest.version = "0.1.0";
    manifest.world = plugin_world::tool;
    manifest.requested_capabilities = order;

    auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
    if (!loaded) std::cerr << "  (" << label << " load error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
    AE_CHECK(loaded.has_value(), label + ": load_component() succeeds");
    if (!loaded) {
        backend.destroy(*handle);
        return;
    }

    auto result = backend.invoke_tool(*handle, ToolInvokeRequest{tool, ""}, ctx);
    AE_CHECK(!result.has_value(), label + ": " + tool + " call fails");
    if (result) {
        std::cerr << "  (" << label << ": expected a failure, call unexpectedly succeeded)\n";
    } else {
        std::cerr << "  (" << label << " message: " << result.error().message << ")\n";
        bool const matches = result.error().message.find(expect_substring) != std::string::npos;
        AE_CHECK(matches, label + ": failure reason contains \"" + expect_substring + "\"");
    }

    backend.destroy(*handle);
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
        // The D5-extended fixture now imports fs/http/secrets too (four new tools calling them), so
        // the whole-component import check needs all five gated kinds granted, not just Clock.
        spec.capabilities = CapabilitySet::grant_root({cap::Clock{}, cap::Entropy{}, cap::FsRead{},
                                                         cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        spec.limits.memory_bytes = 64ull * 1024 * 1024;

        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "positive: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.echo-now-spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = kAllRequiredCapabilities;
        manifest.memory_bytes_limit = spec.limits.memory_bytes;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "positive: load_component() succeeds (manifest covers real imports)");

        auto tools = backend.list_tools(*handle, ctx);
        if (!tools) std::cerr << "  (list_tools error: " << tools.error().code << ": " << tools.error().message << ")\n";
        // D5 added four tools (read-file/write-file/fetch/get-secret) alongside D3's original three.
        AE_CHECK(tools.has_value() && tools->size() == 7, "positive: list_tools() returns all 7 real tools");
        if (tools) {
            bool has_echo = false, has_now = false, has_spin = false, echo_parallelizable = false;
            bool has_read = false, has_write = false, has_fetch = false, has_secret = false;
            for (auto const& t : *tools) {
                if (t.name == "echo") { has_echo = true; echo_parallelizable = t.parallelizable; }
                if (t.name == "now") has_now = true;
                if (t.name == "spin") has_spin = true;
                if (t.name == "read-file") has_read = true;
                if (t.name == "write-file") has_write = true;
                if (t.name == "fetch") has_fetch = true;
                if (t.name == "get-secret") has_secret = true;
            }
            AE_CHECK(has_echo && has_now && has_spin && has_read && has_write && has_fetch && has_secret,
                      "positive: tool names match the fixture's real exports");
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

    // -- 2. Negative, branch 1: manifest omits Clock, component still imports ae:tool/clock ------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // Operator would allow everything the component needs, Clock included...
        spec.capabilities = CapabilitySet::grant_root(
            {cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "negative: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.negative";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // ...but the MANIFEST requests everything else and deliberately omits Clock -- fail closed,
        // and specifically on Clock (the only uncovered import), not some other one.
        manifest.requested_capabilities = {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}};

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

    // -- 3. Negative, branch 2 (D5, ADR-010 claim 2's other half, untested by D3): manifest ------
    //       requests Clock, but the operator's own CapabilitySet does not grant it.
    //
    //       Clock (not FsRead) is the omitted kind deliberately: fs-read and fs-write share the
    //       same `ae:tool/fs` interface class (interface_covered() checks the *interface*, not the
    //       specific function), so granting FsWrite alone would still satisfy the fs import and this
    //       probe would not actually exercise the operator-side rejection -- a real mistake this
    //       task's own first attempt made and caught by re-running the test, not by inspection.
    //       Clock has no sibling capability kind, so omitting it is unambiguous.
    {
        WasmBackend backend;
        SandboxSpec spec;
        // Operator grants everything the component needs EXCEPT Clock.
        spec.capabilities =
            CapabilitySet::grant_root({cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "operator-grant-missing: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.operator-grant-missing";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // Manifest requests the component's real, whole import set, Clock included -- the manifest
        // side of the check passes; the operator side must be what rejects this.
        manifest.requested_capabilities = kAllRequiredCapabilities;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(!loaded.has_value(), "operator-grant-missing: load_component() fails closed");
        if (!loaded) {
            std::cerr << "  (operator-grant-missing error: " << loaded.error().code << ": "
                       << loaded.error().message << ")\n";
            AE_CHECK(loaded.error().code == "wasm.operator_grant_missing",
                      "operator-grant-missing: specific diagnosis, distinct from the manifest-side code");
        }

        backend.destroy(*handle);
    }

    // -- 4. Capability-kind confusion: Entropy bound where Clock is expected ---------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {cap::Entropy{}, cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.kind-confusion";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // Order matters: capabilities[0] (what the fixture's "now" tool actually calls
        // now-unix-millis with) is bound to Entropy here, not Clock.
        manifest.requested_capabilities = {cap::Entropy{},   cap::Clock{},  cap::FsRead{},
                                            cap::FsWrite{},   cap::NetOut{}, cap::Secret{}};

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "kind-confusion: load_component() still succeeds (both kinds are granted)");

        auto now_result = backend.invoke_tool(*handle, ToolInvokeRequest{"now", ""}, ctx);
        AE_CHECK(!now_result.has_value(),
                  "kind-confusion: now-unix-millis rejects an Entropy handle passed where Clock is required");

        backend.destroy(*handle);
    }

    // -- 5. Capability-kind confusion, the other four gated callbacks (D5 closes ADR-010 claim 4's
    //       remaining gap -- D3 proved this mechanism for now-unix-millis only). For each function,
    //       one probe with the matching capability first (right kind) and one with a mismatched
    //       capability first (wrong kind), against the real fixture's four new tools.
    {
        probe_gated_callback(bytes, "fs-read/right-kind", "read-file",
                              {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "fs-read/wrong-kind", "read-file",
                              {cap::FsWrite{}, cap::FsRead{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        probe_gated_callback(bytes, "fs-write/right-kind", "write-file",
                              {cap::FsWrite{}, cap::FsRead{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "fs-write/wrong-kind", "write-file",
                              {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        probe_gated_callback(bytes, "http-request/right-kind", "fetch",
                              {cap::NetOut{}, cap::FsRead{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "http-request/wrong-kind", "fetch",
                              {cap::FsRead{}, cap::NetOut{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        probe_gated_callback(bytes, "resolve-secret/right-kind", "get-secret",
                              {cap::Secret{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "resolve-secret/wrong-kind", "get-secret",
                              {cap::Clock{}, cap::Secret{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}},
                              "wrong kind");
    }

    // -- 6. wall_ms is a real, measured kill -------------------------------------------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // The fixture's import list is component-wide, not per-tool (ADR-010 §7.5's finding) --
        // every gated import (now clock/fs/http/secrets, since D5 added tools calling all four) must
        // still be granted even though "spin" itself calls none of them, or load_component() fails
        // closed before "spin" is ever reachable.
        spec.capabilities = CapabilitySet::grant_root(kAllRequiredCapabilities);
        spec.limits.wall_ms = 200;
        EffectContext ctx = make_ctx();
        auto handle = backend.create(spec, ctx);
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = kAllRequiredCapabilities;
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
