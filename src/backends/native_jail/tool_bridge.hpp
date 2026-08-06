#pragma once
// Implements 010-Python-Code-Interpreter.md §6 / 006-Tool-and-Function-Plane.md §3 -- Milestone 3
// Phase F2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). The `call_tool`
// bridge: a mechanism that lets code running INSIDE a sandboxed interpreter/shell (Stage D's
// `_ae_internal` idiom, `MediatedShellRunner`'s `RegisteredTool`) reach the REAL 006 §3 ten-step
// pipeline (`core/tool_pipeline.hpp`'s `invoke_tool`, real since Milestone 2), deliberately NOT via
// `core/agent_registry.hpp`'s `invoke_agent_tool` -- that wrapper binds the AGENT's own
// `capability_ceiling` (register_agent<A>()'s compiled table), which is exactly the wrong authority
// source here (I2): a bridged call from inside a sandbox must be scoped to the SANDBOX's own,
// separately host-configured `ToolBridgeConfig::capabilities`, never anything reachable from the
// agent's broader grant. There is no parameter on `bridge_tool_call` an agent-level `CapabilitySet`
// could even be passed through as -- the narrower authority source isn't merely preferred, it is the
// only one this function's signature can reach.
//
// Bundled approval (010 §10 Q2's resolution, "not per-call"): `ToolBridgeConfig::approved` is
// decided ONCE, at `execute_code` time (before the sandboxed run starts, by the host/human deciding
// whether to authorize this run's whole pre-registered bridged tool set), not re-asked on every
// `call_tool()` a guest script happens to issue. `bridge_tool_call` still passes `invoke_tool` a real
// `ApprovalDecider` (steps 5's own contract), it just always answers with the one bundled decision
// rather than prompting per call.
//
// Results re-enter as tainted data (003 §2): `invoke_tool`'s own step 9 already stamps every
// successful result's `ContentItem::tainted = true` unconditionally (tool_pipeline.hpp's own
// comment: "a tool result is external content and the primary prompt-injection vector") -- inherited
// here for free by calling the real pipeline rather than reimplementing normalization. This bridge
// additionally taints the CALL's own arguments (`arguments_tainted = true`, unconditionally): they
// originate from code the interpreter is running under agent control, which is model-influenced by
// construction (I3 -- model output is data, never authority).
//
// Capability-handle-reuse-denial (proven with M2 B4's own discipline, tests/
// test_tool_bridge.cpp): inherited structurally, not re-derived -- `bridge_tool_call` does not bind
// or revoke anything itself, it hands `invoke_tool` a fresh `CapabilitySet` built from
// `ToolBridgeConfig::capabilities` and lets that function's own bind (step 7) / revoke (step 10)
// machinery run exactly as it does for any other caller (ADR-009).
//
// SCOPE, named rather than silently narrower: this file wires the bridge into
// `MediatedPythonRunner` (Stage D style, `_ae_internal.call_tool`) where a real JSON-args calling
// convention already exists (Python's own `json.dumps`/`json.loads`). `MediatedShellRunner`'s own
// `RegisteredTool::invoke(argv, ctx)` -> JSON-args mapping is NOT wired here: ADR-001 §11 item 3
// already names "the argv-to-typed-Args mapping" as undesigned, and this pass does not invent a
// shell argument-quoting convention for JSON under time pressure just to close that gap -- carried
// forward as a residual, not a new one this pass created.

#include <string>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail {

// Host-configured, per-session: which tools are reachable from inside the sandbox, what capability
// set backs them (the sandbox's OWN, deliberately separate from any agent-level ceiling -- I2), and
// whether the host/human bundled-approved this run's whole bridged set at `execute_code` time. Never
// guest-derived; constructed only by host policy, the same posture `MediatedPythonConfig::
// mount_roots` and `core/worktree.hpp`'s `Mount` already have one layer up.
struct ToolBridgeConfig {
    ToolTable bridged_tools;
    std::vector<Capability> capabilities;
    bool approved = false;
};

// The one bridge entry point: runs `request` through the REAL 006 §3 pipeline, at
// `config.capabilities` (never an agent's own ceiling -- there is no parameter here one could even
// be passed through as), with `config.approved`'s single bundled decision standing in for a
// per-call approval prompt. Arguments are unconditionally marked tainted (see file-top comment);
// the pipeline's own step 9 taints a successful result's content the identical way for every caller.
[[nodiscard]] inline ToolResult bridge_tool_call(ToolBridgeConfig const& config, ToolCallRequest request,
                                                  EffectContext& ctx,
                                                  ToolInvocationAudit* audit_out = nullptr) {
    request.arguments_tainted = true;
    CapabilitySet const sandbox_capabilities = CapabilitySet::grant_root(config.capabilities);
    ApprovalDecider const bundled_approval = [approved = config.approved](std::string_view,
                                                                            std::string const&) {
        return approved;
    };
    return invoke_tool(config.bridged_tools, sandbox_capabilities, request, ctx, bundled_approval, audit_out);
}

} // namespace agentengine::native_jail
