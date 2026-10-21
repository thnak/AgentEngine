#pragma once
// Implements rfcs/010-Python-Code-Interpreter.md §6 / rfcs/006-Tool-and-Function-Plane.md §3 -- Milestone 3
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
// Approval (ADR-244 §2.4): `ToolBridgeConfig::approval` is a required `BridgeApproval` -- per call through a
// host-supplied decider, or 010 §10 Q2's bundled decision scoped to one principal (`standing_for`), never a
// standing yes for every caller. `bridge_tool_call` passes `invoke_tool` the resulting `ApprovalDecider`
// (step 5's own contract).
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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/block_on.hpp"  // ADR-237 §5.2: drive the async pipeline from this sync bridge
#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail {

// ADR-244 §2.4: how bridged calls are approved -- a required choice, no default. Revision history: this was
// `bool approved = false`, one standing decision for every bridged call of every caller.
//   - `per_call(decider)`: each bridged call goes to `decider` (the session's own `ApprovalDecider`, passed
//     by the host), exactly like a model tool call.
//   - `standing_for(principal)`: the bundled decision of 010 §10 Q2, scoped to ONE principal. A bridged
//     call made under any other principal is not approved.
class BridgeApproval {
public:
    BridgeApproval() = delete;

    [[nodiscard]] static BridgeApproval per_call(ApprovalDecider decider) {
        return BridgeApproval(std::move(decider), std::nullopt);
    }
    [[nodiscard]] static BridgeApproval standing_for(Principal principal) {
        return BridgeApproval(nullptr, std::move(principal));
    }

    [[nodiscard]] ApprovalDecider decider() const {
        if (decider_) return decider_;
        return [standing = standing_](Principal const& caller, std::string_view,
                                      std::string const&) -> task<bool> {
            co_return standing.has_value() && caller.id == standing->id && caller.tenant_id == standing->tenant_id;
        };
    }

private:
    BridgeApproval(ApprovalDecider decider, std::optional<Principal> standing)
        : decider_(std::move(decider)), standing_(std::move(standing)) {}

    ApprovalDecider          decider_;
    std::optional<Principal> standing_;
};

// Host-configured, per-session: which tools are reachable from inside the sandbox, the most capability
// that may back them, and how their calls are approved. Never guest-derived; constructed only by host
// policy, the same posture `MediatedPythonConfig::mount_roots` and `core/worktree.hpp`'s `Mount` already
// have one layer up.
struct ToolBridgeConfig {
    ToolTable bridged_tools;
    std::vector<Capability> capabilities;
    BridgeApproval approval;
};

// The one bridge entry point: runs `request` through the REAL 006 §3 pipeline. ADR-244 §2.4: the
// authority is `config.capabilities` **narrowed to what the calling `ctx` holds** -- an entry the caller
// does not hold is dropped, so the bridge can never reach more than the caller could itself -- and the tool
// body sees a context carrying exactly that set, not the caller's whole one (a body that reads
// `ctx.capabilities` directly would otherwise act on more than the pipeline allowed). A null
// `ctx.capabilities` holds nothing. Arguments are unconditionally marked tainted (see file-top comment);
// the pipeline's own step 9 taints a successful result's content the identical way for every caller.
[[nodiscard]] inline ToolResult bridge_tool_call(ToolBridgeConfig const& config, ToolCallRequest request,
                                                  EffectContext& ctx,
                                                  ToolInvocationAudit* audit_out = nullptr) {
    request.arguments_tainted = true;
    std::vector<Capability> held;
    if (ctx.capabilities) {
        for (Capability const& c : config.capabilities) {
            if (ctx.capabilities->contains(c)) held.push_back(c);
        }
    }
    auto const bridged_capabilities = std::make_shared<CapabilitySet const>(CapabilitySet::grant_root(std::move(held)));
    EffectContext bridged_ctx = ctx;
    bridged_ctx.capabilities = bridged_capabilities;
    // ADR-237 §5.2: the pipeline is async; this sandbox-side bridge is a genuinely-sync edge.
    return agentengine::rt::block_on(invoke_tool(config.bridged_tools, *bridged_capabilities, request, bridged_ctx,
                                                 config.approval.decider(), audit_out));
}

} // namespace agentengine::native_jail
