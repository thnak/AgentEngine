#pragma once
// Implements 007-Capability-and-Trust-Model.md and I4 — the mandatory attribution parameter
// carried into every effect. Never an ambient thread-local (CONVENTIONS.md "Security rules").

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

struct EffectContext {
    Principal                             principal;
    CapabilitySet const*                  capabilities = nullptr;  // borrowed; never owned here
    // 006 §3 step 7's per-call handles ("materialize capability handles for this call only") --
    // distinct from `capabilities` above (the run's whole held set, useful for read-only checks):
    // these are freshly minted for THIS invocation and revoked at step 10 (core/tool_pipeline.hpp),
    // so a tool that copies one out of this vector (BoundCapability is copyable, ADR-009) still
    // loses it the moment the call ends -- the shared-ticket revocation the handle carries doesn't
    // care how many copies exist. Borrowed; never owned here.
    std::vector<BoundCapability> const*   bound_capabilities = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    std::string                           trace_id;
    std::string                           span_id;
    // Milestone 4 Phase A3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
    // real 001 §1/§2 Run/Turn identity, threaded through here because 019 §3's per-effect
    // idempotency key is derived from exactly `{run_id, turn_index, call_index, argument_digest}`
    // -- the first two live on the EffectContext every effect already carries, rather than a
    // parallel identity parameter. Minted once per `Ask<StartRun, AgentResponse>`
    // (`core/agent_session.hpp`), deterministically from the session's own monotonic run counter
    // -- never wall-clock-derived (001 §7: an unrecorded wall-clock read here would be exactly the
    // kind of untracked nondeterminism I5 forbids).
    std::string  run_id;
    std::uint64_t turn_index = 0;
    // ADR-057 §9 (Design B: abort-and-replay for `agent.ask()`, 026 §5): host-driven REPLAY state,
    // set by `AgentSession::resolve_interaction()`'s `codeact_ask` branch (rt/agent_session.hpp)
    // immediately before re-invoking `execute_code` against a STORED script, and cleared immediately
    // after that one `invoke_tool()` call returns -- never left populated across an ordinary,
    // model-issued round. Threaded through here (rather than a new `ExecuteCodeArgs` field,
    // cli_chat.cpp) specifically BECAUSE it must never be model-facing: `ExecuteCodeArgs`'s
    // `AE_JSON_SCHEMA` is what the model sees, and preseeded answers are the host's own bookkeeping,
    // never something the model supplies (I3). A host's `execute_code` implementation reads this to
    // build its own `sandbox::ExecRequest::preseeded_answers` -- see that field's own comment for the
    // full consumption contract.
    std::vector<std::string> codeact_preseeded_answers;
    // ADR-060: a real, call-scoped reverse channel into the session's own `emit_run_event()` --
    // `Tool<>::invoke()` calls `ctx.report_progress("...")` to push a `run_event_kind::
    // tool_call_delta` (run_event.hpp) for ITSELF, mid-call, onto `enable_event_stream()`'s stream.
    // Default-initialized to a no-op (this codebase's "optional-but-always-safe-to-call" idiom, e.g.
    // `ApprovalDecider`'s own default) so a tool that calls it with no session-side listener bound
    // (a test, or any code path that never sets this) is a harmless, observable-nowhere no-op.
    // Set immediately before, and reset to this same no-op immediately after, EACH of the three real
    // `invoke_tool()` call sites in `rt/agent_session.hpp` -- the identical bracketing discipline
    // `codeact_preseeded_answers` above already established (ADR-057 §9), applied independently at
    // all three sites here (that field's own bracket lives at only one of the three -- it is specific
    // to codeact-ask replay -- this one needs its own bracket at each, not a shared one).
    // CALL-SCOPED, NOT OWNED -- same discipline `capabilities`/`bound_capabilities` above already
    // carry ("borrowed; never owned here"): ADR-060 §4 red-teamed whether a tool that copies this
    // out into a member and calls it later is a hazard. The only tool shape that could ever capture
    // something reaching back to the session this way is a session-scoped stateful tool built via
    // `make_tool_descriptor_with_invoke()` (ADR-028) -- `Tool::invoke()`'s ordinary static-call shape
    // has nothing to capture a session reference INTO in the first place -- and that construction is
    // structurally forbidden from ever being `Backgroundable` (`tool_pipeline.hpp`'s own
    // `captures_session_state` guard). So a stashed-and-later-called copy can only ever run
    // synchronously, on the session's own `session_mutex_`-serialized coroutine thread (I1), producing
    // at worst a `tool_call_delta` for a stale-but-real `call_id` from an earlier call -- never a
    // cross-thread hazard. Documented here, not structurally prevented, matching the same convention
    // `capabilities`/`bound_capabilities` already rely on documentation (not the type system) for.
    // NEVER carried across `tool_pipeline.hpp::background_task()`'s by-value `EffectContext ctx` onto
    // its detached `std::thread` -- that function resets its own copy of this field to the no-op,
    // structurally, before step 8 ever runs, precisely BECAUSE `AgentSession::start_background_task()`
    // is documented "PLAIN, UNLOCKED" (rt/agent_session.hpp's own file banner) and can race a bracket
    // window above from a genuinely different thread of control; letting a live callback into `this`
    // (captured by whichever bracket happened to be open) leak onto that detached thread would let a
    // backgrounded tool's ORDINARY call to `report_progress()` reach `emit_run_event()`'s unlocked
    // `run_event_seq_by_run_` mutation from off-thread -- see ADR-060 §4 for the full finding.
    std::function<void(std::string_view)> report_progress = [](std::string_view) {};
};

} // namespace agentengine
