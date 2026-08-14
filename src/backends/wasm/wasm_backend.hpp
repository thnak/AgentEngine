#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for the `wasm` profile --
// 009-Plugin-and-Extension-System.md's `ae:tool` world (wit/ae-tool.wit, revised by this task),
// backed by wasmtime's Component Model C API (AGENTENGINE_WITH_WASM, D1) -- Milestone 2 Phase D task
// D3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md). Security-critical: built
// through design->red-team->prove->judge per CLAUDE.md, landed as
// decisions/ADR-010-wasm-component-host-manifest-capability-binding.md. That ADR is the source of
// truth for *why* this file is shaped the way it is -- comments here cite it by section, not restate
// it.
//
// Scope (ADR-010's own scope note, restated briefly): the `ae:tool` world only; no signature/
// publisher verification (`PluginManifest` has no signature field yet); no AOT-cache-by-digest; no
// instance pooling (008 SS6a's snapshot-reset problem is sidestepped, not solved, by instantiating
// fresh per call -- ADR-010 SS3.5); `blob`/`tool-call` host imports are recognized in the WIT
// contract but never linked (ADR-010 SS3.1) -- a component that imports either always fails to load.
// Wired into core/tool_pipeline.hpp's ToolTable/invoke_tool via wasm_tool_bridge.hpp
// (decisions/ADR-040-wasm-tool-pipeline-bridge.md) -- that bridge calls create()/load_component()/
// list_tools()/invoke_tool() directly (ADR-010's own §9 point: "any future SandboxBackend consumer
// written against just the three concept methods will not see plugin loading at all, by design"),
// never the generic SandboxBackend::exec() adapter below. This backend's own internals (import
// verification, capability-kind confusion, wall_ms kill) stay proven standalone (tests/
// test_wasm_backend.cpp); the bridge's own tests (tests/test_wasm_tool_bridge.cpp) prove only the
// new seam, not re-prove this file's internals.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/plugin/plugin.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::wasm {

// One exported tool's shape, the C++-side mirror of wit/ae-tool.wit's `guest.tool-descriptor`
// (ADR-010 SS3.5) -- discovered by calling the component's `list-tools` export once at load time
// (009 SS4).
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::string args_schema_json;
    std::string result_schema_json;
    bool        parallelizable = false;
};

// The C++-side mirror of `guest.invoke-request`, minus `capabilities` (this backend materializes
// those itself, from the handle's own bound-at-load capability set -- ADR-010 SS3.2) and minus
// `run-id`/`span-id` (already carried by `EffectContext`, never duplicated -- 006 SS1's "EffectContext
// is mandatory... no ambient-context accessor" extends naturally to "no second attribution channel").
struct ToolInvokeRequest {
    std::string tool_name;
    std::string args_json;
};

class WasmBackend {
public:
    static constexpr ProfileTraits traits{
        /*strength=*/40,  // software, capability-based, no kernel boundary (008 §3) -- below
                          // native-jail's 50, above `none`'s 0.
        /*platform_mask=*/static_cast<std::uint8_t>(platform_id::windows_x86_64) |
            platform_id::linux_x86_64,  // wasmtime's Component Model C API is identical on both --
                                        // only D1's *link* mechanics (DLL vs static archive) differed.
        cold_start_class::microseconds_to_low_ms,
    };

    // Both declared here, defined in wasm_backend.cpp -- std::unique_ptr<Instance>'s implicit
    // destructor (needed by both the default constructor's exception-unwind path and the real
    // destructor) requires Instance to be a complete type, which it is not yet at this point in the
    // header (Pimpl idiom's standard requirement, not specific to this class).
    WasmBackend();
    ~WasmBackend();
    WasmBackend(WasmBackend const&) = delete;
    WasmBackend& operator=(WasmBackend const&) = delete;
    WasmBackend(WasmBackend&&) = delete;   // instances_ holds handles other objects reference
    WasmBackend& operator=(WasmBackend&&) = delete;

    // Generic SandboxBackend surface (008 §2). `create()` is deliberately thin -- SandboxSpec (008's
    // shared, cross-backend type) carries no "which component" field, so it cannot be where 009 §4's
    // compile-and-verify step happens (a real gap ADR-010 §7.5 found while implementing this, not
    // anticipated during design). It only allocates this handle's private Instance and records
    // `spec`/`limits` for later use by `load_component`.
    result<SandboxHandle> create(SandboxSpec const& spec, EffectContext& ctx);

    // ADR-010 §3.3's real entry point: compiles `component_bytes`, enumerates its actual imports via
    // wasmtime_component_type_import_*, and fails closed (never reaching instantiate) if any imported
    // interface falls outside `manifest.requested_capabilities` intersected with the handle's granted
    // `SandboxSpec.capabilities` (007 §3's real CapabilitySet::contains, not a parallel check). Must
    // be called exactly once per handle, after create() and before list_tools()/invoke_tool().
    result<void> load_component(SandboxHandle& handle, PluginManifest const& manifest,
                                 std::vector<std::uint8_t> const& component_bytes, EffectContext& ctx);

    // 009 §4's load-time discovery step -- calls the already-instantiated-and-verified component's
    // `guest.list-tools` export once. Requires load_component() to have already succeeded.
    result<std::vector<ToolDescriptor>> list_tools(SandboxHandle const& handle, EffectContext& ctx);

    // The real per-call path (ADR-010 §3.2/§3.5): binds exactly the capabilities `manifest.
    // requested_capabilities` granted for this component (never the operator's whole set, 007 §3's
    // attenuation-only rule), builds a fresh store+linker+instance (no pooling, ADR-010 §3.5),
    // constructs one host-resource capability-handle per bound capability, calls `guest.invoke`, and
    // unconditionally revokes every bound capability before returning -- success or failure alike,
    // mirroring 006 §3 step 10.
    result<ToolResult> invoke_tool(SandboxHandle const& handle, ToolInvokeRequest const& request,
                                    EffectContext& ctx);

    // Generic SandboxBackend adapter (008 §2, ADR-010 §3.5): `request.language == "ae:tool"` marks
    // it; `request.source` carries `{"tool_name":..., "args_json":...}` as JSON text, decoded and
    // forwarded to invoke_tool() -- exactly one real implementation of "invoke a tool in a wasm
    // component" (006 §2's uniformity rule), not two that could silently diverge.
    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx);

    void destroy(SandboxHandle& handle);

    // Defined in wasm_backend.cpp -- owns wasmtime_component_t*, granted kinds, the manifest, and
    // spec.capabilities/limits. Public (not private) so the .cpp's free-function helpers (the
    // per-call linker/instantiate machinery, ADR-010 §3.5) can take `Instance&` as a plain
    // parameter type without needing friend declarations -- this does not leak wasmtime types into
    // the header regardless: `Instance` itself is only forward-declared here, never defined, so
    // AGENTENGINE_WITH_WASM's one heavy dependency still never appears in any translation unit that
    // merely includes this header without also linking wasmtime.
    struct Instance;

private:
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
};

static_assert(SandboxBackend<WasmBackend>,
              "WasmBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::wasm
