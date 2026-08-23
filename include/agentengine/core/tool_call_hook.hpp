#pragma once
// Implements OQ-21 (OpenQuestions.md) -- the tool-call hook stage: an optional, host-wired seam that
// runs once per round, per call, immediately before `AgentSession::run_rounds()`'s existing
// suspend-for-approval pre-check (rt/agent_session.hpp) -- the "narrow, tool-call-only" scope the
// design settled on in `docs/planning/external-process-hooks-design-draft.md`, after two earlier,
// broader drafts (run/turn-level RAII boundary hooks) were found structurally impossible (a
// destructor cannot be a coroutine, C++20 forbids it; `co_return` finalizes a coroutine's result
// before any local's destructor runs, so an "after_turn hook overrides the outcome" shape cannot be
// built on that mechanism either).
//
// This header is deliberately I2/I3-disciplined the same way `core/middleware.hpp`'s
// `ModelCallContext` already is: `ToolCallHookContext` carries no `EffectContext&` and no capability
// type, ever -- a hook gets identity (`caller`, a `Principal const&`, matching `ApprovalDecider`'s and
// `PolicyDecider`'s own parameter, core/tool_pipeline.hpp) and the call's own shape, nothing that
// could let it mint or discover authority beyond what `invoke_tool()`'s own step 4/7 already binds.
// `provenance` is READ-ONLY on this struct -- there is no field through which a hook could assert a
// different provenance for itself; only `enforce_hook_rewritten_tool_call_provenance()`
// (core/tool_pipeline.hpp, co-located with `ToolCallRequest`/`invoke_tool`) may ever change it, and
// only by diffing canonical JSON bytes captured before/after the hook ran -- the same
// "provably safe by absence" idiom this codebase already leans on for I2 (e.g. `EffectContext`'s own
// file-top comment on `bound_capabilities`).
//
// The types below beyond `ToolCallHookContext`/`ToolCallHook` exist to close a red-team-confirmed
// fatal finding in an earlier draft of this design: a naive port of `resolve_codeact_ask()`'s own
// `one_shot_approve` shape onto a new `resolve_hook_decision()` would let an external hook's dispatch
// ANSWER stand in for a human's APPROVAL decision -- two different questions with no mechanism to
// keep them apart. `hook_call_outcome`/`HookProcessedCall`/`PendingHookDecisionRound` let
// `AgentSession::run_rounds()` compute BOTH "does any call in this round need external dispatch" and
// "does any call in this round need a human decider" from the SAME post-hook per-call state every
// time, before choosing which single `Interaction` to open -- see that function's own comment
// (rt/agent_session.hpp) for the full mechanism, and `AgentSession::resolve_hook_decision()`'s own
// comment for why a hook-decision resume re-checks approval need with the real deciders rather than
// ever reusing `one_shot_approve` directly.

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// One call's worth of hook input/output. `call_id` correlates a decision back to one call in a round
// that may (and normally does) carry several parallel `ToolCall`s -- without it a hook could not act
// differently per call. `caller` costs nothing to pass (identity, not authority) and is already
// available to the two seams this hook runs immediately upstream of (`ApprovalDecider`,
// `PolicyDecider`, core/tool_pipeline.hpp) -- omitting it would make this hook strictly less capable
// than its neighbors for no I2/I3 benefit.
// ae-naming-lint: allow ToolCallHookContext — OQ-21; 027 has not been updated to list this new vocabulary
struct ToolCallHookContext {
    std::string       call_id;
    std::string       tool_name;
    json::Value       arguments;      // current (possibly hook-rewritten-so-far) arguments
    call_provenance   provenance;     // informational only -- READ-ONLY, see file-top comment
    Principal const&  caller;         // identity, not a capability -- matches ApprovalDecider/PolicyDecider

    std::optional<json::Value> rewritten_arguments = std::nullopt;  // hook wants to change the arguments
    std::optional<error>       denial = std::nullopt;               // hook wants to deny outright
    // The hook wants this call answered by an external process instead of in-process. Checked AFTER
    // `denial` at the one real call site (rt/agent_session.hpp's hook-stage block) -- a hook body
    // that sets both is treated as a denial, never as "deny AND also dispatch". A hook that sets this
    // MUST return promptly without itself blocking on the external work -- the no-inline-blocking
    // caller contract `AgentSession::resolve_hook_decision()`'s own comment names explicitly (the
    // hook stage runs while `session_mutex_` is held).
    bool needs_external_dispatch = false;
};

// `task<result<...>>`-returning so a hook body can genuinely `co_await` internal work (e.g. queuing
// the external dispatch request); a purely in-process hook (deny/rewrite only, never
// `needs_external_dispatch`) just returns immediately. Unset (`nullptr`) by default -- every existing
// session is completely unaffected until a host opts in (`AgentSession::set_tool_call_hook()`).
// ae-naming-lint: allow ToolCallHook — OQ-21; 027 has not been updated to list this new vocabulary
using ToolCallHook = std::function<task<result<std::monostate>>(ToolCallHookContext&)>;

// The three-way outcome one call can land in after the hook stage runs, mirroring
// `tool_pipeline.hpp`'s own `approval_outcome` shape one layer up.
enum class hook_call_outcome { pass_through, denied, needs_external_dispatch };  // ae-naming-lint: allow hook_call_outcome — OQ-21, same idiom as approval_outcome

// One call's hook-processed state, carried forward across a suspend/resume the same way
// `PendingCodeActAsk` (rt/agent_session.hpp) carries a codeact-ask's own replay state.
// ae-naming-lint: allow HookProcessedCall — OQ-21; 027 has not been updated to list this new vocabulary
struct HookProcessedCall {
    ToolCallRequest             request;                       // post-hook, post-enforce-provenance
    hook_call_outcome           outcome = hook_call_outcome::pass_through;
    std::optional<ToolResult>   denial_result = std::nullopt;   // set iff outcome == denied
};

// The whole round's hook-processed state -- keyed by `Interaction::interaction_id` in
// `AgentSession::pending_hook_decisions_`, the same "one entry per one open Interaction" shape
// `pending_codeact_asks_`/`PendingCodeActAsk` already establish.
// ae-naming-lint: allow PendingHookDecisionRound — OQ-21; 027 has not been updated to list this new vocabulary
struct PendingHookDecisionRound {
    std::vector<HookProcessedCall> calls;  // whole round, original order, one entry per ToolCall
};

// One external process's answer for one call that was left `needs_external_dispatch`. Interpreted
// ONLY when the named `Interaction`'s own `reason == interaction_reason::hook_decision`
// (`AgentSession::resolve_hook_decision()`); ignored otherwise, the same "additive field interpreted
// only for its own reason" convention `ResolveInteraction::answer`/`::authority` already establish
// (rt/agent_session.hpp). A VECTOR, not a single field, because a round may have MULTIPLE calls
// pending external dispatch at once, and resolving an `Interaction` closes the whole round in one
// resume (`resolve_interaction_record()` erases on first resolve) -- so every pending call_id must
// have a matching answer in one resume, or the resume fails closed
// (`session.hook_decision.incomplete`), never a partial round resolution.
// ae-naming-lint: allow HookDispatchAnswer — OQ-21; 027 has not been updated to list this new vocabulary
struct HookDispatchAnswer {
    std::string                 call_id;
    bool                        approved = false;   // the external process's allow/deny outcome
    std::optional<json::Value>  rewritten_arguments = std::nullopt;
    std::optional<std::string>  denial_message = std::nullopt;
};

}  // namespace agentengine
