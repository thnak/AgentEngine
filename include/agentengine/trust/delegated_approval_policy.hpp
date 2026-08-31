#pragma once
// GitHub issue #30 / ADR-151: a reference `PolicyDecider` (decisions/ADR-070-host-configurable-
// responsibility-boundary.md's Delegated Decision Seam, core/tool_pipeline.hpp) implementing "a
// delegated/spawned agent's own policy_driven tool calls auto-approve by default" -- the pattern
// issue #30 found fully EXPRESSIBLE today (`Principal` already carries `on_behalf_of`/
// `delegation_depth`, trust/principal.hpp) but UNDEMONSTRATED anywhere in this codebase (grepped
// tests/examples for a real `PolicyDecider` keyed off either field: zero hits before this file).
//
// Per ADR-070 §3/§4a this is explicitly NOT an engine default (I2/I3 stay untouched) -- it is ONE
// opt-in `PolicyDecider` implementation a host MAY choose to wire, at either or both of two distinct
// points:
//   1. `AgentSession::set_policy_decider()` (rt/agent_session.hpp) -- governs the HOST's own
//      top-level session's `policy_driven` tool calls.
//   2. `SpawnTargetDescriptor::policy_decider` (rt/agent_spawn.hpp, ADR-151) -- governs a SPECIFIC
//      spawned-agent TARGET's own children's `policy_driven` tool calls. Independent of (1) --
//      neither is inherited from the other; a host wiring only (1) leaves every spawned child
//      exactly as unaffected as before ADR-151, and vice versa. See `examples/19_delegated_agent_
//      approval.cpp` for both wired side by side, deliberately contrasted.
//
// ADOPTING THIS IS A REAL TRUST DECISION, not a safe default: every `policy_driven` tool call from a
// principal this policy recognizes as delegated bypasses human review entirely, bounded only by
// whatever capability ceiling that principal's own `CapabilitySet` already carries -- step 4/7 of
// `invoke_tool()` binds/checks capabilities BEFORE step 5 ever consults this decider (structurally,
// not by convention), so this policy can never widen what a delegated caller could already do, only
// decide whether a call it's already authorized for also needs a human to look at it first.
//
// `arguments_tainted` -- NOT consulted here, deliberately, and this is the single most important
// correction this design went through (decisions/ADR-151.md §2 has the full record of an
// independent red-team pass finding the original draft dead-on-arrival otherwise). Traced every
// real call site that ever threads a LIVE `PolicyDecider` (`rt/agent_session.hpp`'s suspend-for-
// approval pre-check, and its one `invoke_tool()` call site -- both build their `ToolCallRequest`
// via `core/tool_call_extraction.hpp`'s `tool_call_request_of()`) and `arguments_tainted` is
// hard-coded `true`, unconditionally, for every genuinely model-originated call -- the ONLY kind of
// call that ever reaches a `policy_driven` decision at all (the three "already resolved by a real
// human" `invoke_tool()` call sites never consult `PolicyDecider` in the first place, see
// `AgentSession::set_policy_decider()`'s own comment). A policy that returned `require_approval`
// whenever `arguments_tainted` is true would therefore return `require_approval` UNCONDITIONALLY in
// production -- dead code wearing a delegation-aware policy's clothes, its `on_behalf_of`/
// `delegation_depth` branches never reachable. This file does not repeat that mistake: it names the
// constant honestly in this comment instead of pretending to key off it. (If a future caller ever
// makes `arguments_tainted` genuinely vary for a live `PolicyDecider` call, that is real, separate
// work needing its own review -- this reference does not attempt to anticipate its shape.)
//
// SPAWNED-CHILD RESIDUAL, NAMED NOT FIXED: a spawned child (`rt/agent_spawn_child_run.hpp`'s
// `run_child_agent_session()`) is driven SYNCHRONOUSLY to completion and destroyed before that
// function returns -- it can never genuinely suspend for a LATER human answer (ADR-029's
// `Interaction` mechanism needs a session that outlives the call that opened it). So for a spawned
// child specifically, this policy's `require_approval` fallback (a non-delegated caller, or one past
// a host-chosen `max_depth`) resolves through the ordinary step-5 `ApprovalDecider` fallback ONLY --
// with no `SpawnTargetDescriptor::approval_decider` also supplied, that fallback is an outright
// DENIAL of that one tool call, never a pause. A host wiring this policy onto a spawn target without
// also giving it a real, synchronous `approval_decider` should expect every `policy_driven` call this
// policy doesn't auto-approve to fail, not defer -- named here, not silently discovered later.

#include <cstdint>
#include <optional>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::trust {

// Delegation-aware auto-approve: `auto_approve` iff `caller` is a delegated/spawned principal
// (`on_behalf_of` non-empty -- `derive_on_behalf_of()`'s own contract, principal.hpp -- true for
// EVERY delegated/spawned principal, never for a top-level human/service one) AND, when `max_depth`
// is supplied, `caller.delegation_depth <= *max_depth`.
//
// `max_depth` is independent of `kMaxDelegationDepth` (principal.hpp's own hard, structural bound on
// how deep delegation can go AT ALL, currently 8, enforced by `derive_on_behalf_of()` itself failing
// closed past it -- no principal this policy ever sees can carry a `delegation_depth` above that).
// This parameter is a SOFTER, host-chosen ceiling on how deep AUTO-APPROVAL specifically reaches; a
// caller past it still runs, just without this policy's fast path -- falls through to
// `require_approval`, exactly as if this policy had never been wired for that call. Unset (the
// default) means "no additional depth ceiling beyond `kMaxDelegationDepth`'s own structural one."
//
// Never returns `auto_deny` -- a deliberate choice (see file banner): this policy only ever narrows
// toward "ask" (`require_approval`), never introduces a new denial surface a host didn't already
// have from whatever `ApprovalDecider`/suspend behavior it configured for everything this policy
// doesn't recognize as delegated-and-in-bounds.
[[nodiscard]] inline PolicyDecider approve_delegated_calls(std::optional<std::uint32_t> max_depth = std::nullopt) {
    return [max_depth](Principal const& caller, ToolDescriptor const&, bool /*arguments_tainted*/) -> policy_decision {
        if (caller.on_behalf_of.empty()) return policy_decision::require_approval;
        if (max_depth.has_value() && caller.delegation_depth > *max_depth) {
            return policy_decision::require_approval;
        }
        return policy_decision::auto_approve;
    };
}

}  // namespace agentengine::trust
