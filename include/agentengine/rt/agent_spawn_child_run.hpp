#pragma once
// Implements docs/planning/agent-spawn-runtime-design-draft.md item 2 / §4.2 (the nested-agent-run
// invocation mechanism) for 026-Agent-Facing-Runtime-Surface.md §5's `agent.spawn` --
// OpenQuestions.md OQ-14, named the project's own "sharpest case". THIS FILE IS ITEM 2 ONLY:
// constructing + driving a FRESH child `rt::AgentSession` to completion, synchronously, mid-run,
// from inside a parent's own tool-call loop -- given an ALREADY-MINTED child `CapabilitySet`
// (`trust::mint_child_spawn_capabilities()`, item 5, `trust/agent_spawn_capability.hpp`, already
// landed). This function trusts `ChildSpawnRequest::capabilities` exactly as given and mints nothing
// itself (I2: the CALLER of this function is the one bounding authority; this mechanism only drives
// a session under whatever ceiling it is handed).
//
// NOT built here -- see the design doc for their own file list/scope:
//   - item 1 (the `Tool<>` surface / `SpawnTargetRegistry` / `AgentSpawnTool`),
//   - item 3 (dynamic sub-worktree minting, `core/agent_spawn_worktree.hpp`),
//   - item 4c (the `SpawnPump` single-threaded serialization point).
// Item 6 (OQ-16's `agent_library_manifest.hpp` wired into real session construction, §4.6) is now
// PARTLY landed here (2026-08-23): `ChildSpawnRequest::instructions`/the `set_static_instructions()`
// call below is item 6's own wiring for the CHILD side; computing the actual
// `trust::push_side_summary(...)` string that populates it is the CALLER's job
// (`rt/agent_spawn.hpp`'s `perform_agent_spawn()`), not this file's.
// A `child_id` is accepted as a plain parameter here (used only as this child's `session_id()`) --
// item 3's real, collision-proof, deterministic derivation formula is not this file's job, and a
// session_id collision here has no security consequence (it labels the child, it grants nothing).
//
// RC-1 (design doc §9, Critical, CLOSED here): a spawned child's own `ToolTable` could include an
// ordinary `Backgroundable` tool whose step-8 `invoke()` detaches a real `std::thread`
// (`core/tool_pipeline.hpp`'s `background_task()`) via `AgentSession::start_background_task()` --
// a real, already-wired, already-tested entry point, not a hypothetical one. Since
// `run_child_agent_session()` below drives `child` with a plain "resume until done" loop and then
// destroys it on return, any such detached thread would outlive the very object it captured a
// reference into -- a genuine use-after-free. Closed by construction: every child this function
// constructs gets `set_background_execution_disabled(true)` (new, additive-only method on
// `rt::AgentSession`, `agent_session.hpp`) unconditionally, before `start_run()` is ever called, so a
// spawned child can never create a second thread of control touching itself. This is what makes the
// "freshly constructed, referenced by nothing else, driven by exactly one thread" precondition this
// function's own drive loop relies on -- the SAME precondition `rt/agent_workflow_executor.hpp`'s
// `agent_executor_detail::drive<T>()` already documents for the identical pattern -- actually true
// here, not merely assumed.

#include <string>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // ApprovalDecider, PolicyDecider (GitHub issue #30 / ADR-151)
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::rt {

// §4.2's `ChildSpawnRequest`. `instructions` (below) is item 6's own addition (docs/planning/
// agent-spawn-runtime-design-draft.md §4.6, OQ-16) -- everything else here is unchanged from item 2's
// original standalone scope. No `deadline` field -- still not this file's job. A `HistoryProviderT`
// that wants to hand the child its own system instructions independently of `instructions` below
// already can, via the ordinary `ContextContribution::instructions` mechanism every `ContextProvider`
// already has (the SAME materialization path `run_rounds()` already turns into a `role::system`
// message) -- the two are independent, additive `role::system` messages (`rt::AgentSession::
// set_static_instructions()`'s own comment).
struct ChildSpawnRequest {
    agentengine::Message input;
    agentengine::CapabilitySet capabilities;  // MUST be mint_child_spawn_capabilities()'s own output
                                                // (item 5) -- trusted here exactly as given, never
                                                // re-derived or widened.
    agentengine::Principal principal;          // the CALLER's own principal -- re-derived below via
                                                // trust::derive_on_behalf_of(), never reused directly.
    std::optional<std::uint64_t> token_budget;  // threaded through as-is; item 1's
                                                 // SpawnTargetDescriptor::child_token_budget (§9
                                                 // I2-3) is what picks a real, never-unbounded value
                                                 // -- this function does not default it for the
                                                 // caller.
    std::uint64_t max_turns = 25;               // finite by default (§4.2's own reasoning: an
                                                 // unbounded child turn loop is a real availability
                                                 // hazard, not merely a convenience gap -- matches
                                                 // QuickstartSessionBuilder's own §7 red-team finding).
    // §4.6 (item 6, OQ-16): the caller (perform_agent_spawn(), rt/agent_spawn.hpp) is expected to set
    // this to `trust::push_side_summary(capabilities)` above -- an accurate, freshly-computed manifest
    // of the CHILD's own granted surface, never a copy of the parent's. Empty by default (this file
    // itself never computes it -- see this struct's own top comment for why that's item 6's job, not
    // item 2's).
    std::string instructions;

    // GitHub issue #30 / ADR-151: opt-in seam letting a spawned child get its OWN
    // ApprovalDecider/PolicyDecider, independent of whatever the PARENT session configured for
    // ITSELF (`AgentSession::approval_decider()`/`policy_decider()`, agent_session.hpp -- neither
    // one is inherited automatically; a host must supply this explicitly). Default `{}` for both
    // reproduces exactly today's pre-ADR-151 behavior -- no decider ever reached a spawned child's
    // own tool-call pipeline -- matching ADR-070 property 2 (fails closed/safe when unset).
    // Populated by `perform_agent_spawn()` (rt/agent_spawn.hpp) from the resolved target's own
    // `SpawnTargetDescriptor::approval_decider`/`policy_decider` -- per-TARGET, the SAME
    // host-authored-only-at-registry-build-time scoping `child_token_budget`/`worktree_mode` already
    // have, never per-call and never derived from `AgentSpawnArgs` (I3: a model's own `agent_id`
    // argument only ever SELECTS which already-registered target's own deciders apply, it cannot
    // supply or influence either decider's actual logic).
    agentengine::ApprovalDecider approval_decider{};
    agentengine::PolicyDecider   policy_decider{};
};

namespace agent_spawn_detail {

// Drives an `rt::task<T>` to completion from a plain, non-coroutine call site -- the SAME hand-rolled
// "resume until done" loop `rt/agent_workflow_executor.hpp`'s own `agent_executor_detail::drive<T>()`
// already uses for the identical "fresh session, referenced by nothing else, uncontended
// session_mutex_" precondition (that file's own CONCURRENCY CONTRACT comment; this file's own top
// comment restates why it holds here too). Not shared from there -- deliberately kept byte-for-byte
// identical in shape rather than introducing a header/namespace coupling neither file otherwise needs.
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace agent_spawn_detail

// The mechanism itself -- design doc §4.2 / §2 step [8]. Constructs a FRESH
// `AgentSession<ChatClientT, StateT, HistoryProviderT>`, configures it from `req` (an already-minted,
// already-bounded `ChildSpawnRequest` -- see that struct's own comment), lets the caller wire
// whatever this particular target's `ChatClientT` needs via `configure_chat_client` (host-authored,
// matching the design's own residual "ChildRunner's ChatClientT selection is fully host-authored,
// static per registered target" -- item 1, not built here, is what will eventually own per-target
// selection; this function only takes it as a template parameter plus a configuration callback), and
// drives it synchronously to completion.
//
// The child's `Principal` is ALWAYS a delegated derivation of `req.principal` via
// `agentengine::derive_on_behalf_of()` (018 §2, already-proven, independently depth-bounded by
// `kMaxDelegationDepth`) -- NEVER `req.principal` reused directly -- so the "the parent's raw
// Principal is never inherited" property (design doc §5 item 4) holds even though this function in
// isolation has no `SpawnTargetRegistry`/host policy layer of its own; the derivation itself is the
// enforcement. Fails closed (propagating `derive_on_behalf_of`'s own error) if the caller's own
// `delegation_depth` is already at `kMaxDelegationDepth` -- no child session is ever constructed for
// a request that would exceed it.
template <class ChatClientT, class StateT = agentengine::rt::NoSessionState,
          class HistoryProviderT = agentengine::HistoryProvider<agentengine::Window<0>>,
          class ConfigureChatClientFn>
[[nodiscard]] agentengine::result<agentengine::rt::AgentResponse> run_child_agent_session(
    std::string child_id, ChildSpawnRequest req, ConfigureChatClientFn&& configure_chat_client) {
    agentengine::result<agentengine::Principal> child_principal =
        agentengine::derive_on_behalf_of(req.principal, child_id);
    if (!child_principal) {
        return std::unexpected(child_principal.error());
    }

    agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT> child;
    child.initialize(child_id, *child_principal, req.token_budget,
                      std::optional<std::uint64_t>{req.max_turns});
    configure_chat_client(child.emplace_chat_client());
    child.set_capabilities(&req.capabilities);
    // §4.6 (item 6, OQ-16) -- see `ChildSpawnRequest::instructions`'s own comment above. No-op when
    // the caller left it empty (matches `set_static_instructions()`'s own "no-op until set" default).
    if (!req.instructions.empty()) {
        child.set_static_instructions(req.instructions);
    }
    // GitHub issue #30 / ADR-151: unconditional, matching `set_capabilities()` above -- assigning an
    // unset (default `{}`) decider is a no-op (a freshly constructed `child` already defaults both to
    // `{}`), so this is byte-identical to before this ADR for every caller that leaves `req`'s two new
    // fields untouched. SPAWNED-CHILD RESIDUAL, NAMED NOT FIXED (see
    // `trust/delegated_approval_policy.hpp`'s own file banner for the full account): `child` is driven
    // synchronously to completion and destroyed before this function returns (this file's own top
    // comment) -- it can never genuinely SUSPEND for a later human answer. A `policy_decider` verdict
    // of `require_approval` (or an unset one, falling through to `tool_call_requires_approval()`'s own
    // default) resolves through the ordinary step-5 `ApprovalDecider` fallback ONLY -- with no
    // `approval_decider` supplied, that fallback is an outright denial of that one tool call, not a
    // pause. A host wiring `policy_decider` onto a spawn target without also giving it a real,
    // synchronous `approval_decider` should expect every policy_driven call this policy doesn't
    // auto-approve to fail, not defer.
    child.set_approval_decider(std::move(req.approval_decider));
    child.set_policy_decider(std::move(req.policy_decider));
    // RC-1 (this file's own top comment) -- unconditional, not opt-in: every child this mechanism
    // constructs is background-execution-disabled, full stop, before start_run() is ever called.
    child.set_background_execution_disabled(true);

    // Safe under this file's own top-comment precondition: `child` is freshly constructed, referenced
    // by nothing else, background execution disabled, and driven to completion before this function
    // returns and `child` is destroyed -- `req.capabilities` (pointed to by `child`'s own
    // `capabilities_`) stays alive for the whole call, since function parameters outlive every local
    // variable declared after them.
    return agent_spawn_detail::drive(
        child.start_run(agentengine::rt::StartRun{std::move(req.input)}));
}

}  // namespace agentengine::rt
