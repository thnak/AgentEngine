#pragma once
// Implements 007-Capability-and-Trust-Model.md and I4 — the mandatory attribution parameter
// carried into every effect. Never an ambient thread-local (CONVENTIONS.md "Security rules").

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// ADR-061 §20.3's type change (EffectContext::capabilities: raw pointer -> shared_ptr) means a call
// site that used to write `ctx.capabilities = &local_caps;` directly (constructing an EffectContext
// by hand -- every test that exercises a Tool<>::invoke()/invoke_tool() directly, not through
// AgentSession) no longer compiles by simple assignment. This is the non-owning bridge: a shared_ptr
// that aliases a caller-owned CapabilitySet without ever deleting it -- the caller must outlive every
// use of the returned shared_ptr, the same lifetime contract a raw pointer already implied.
[[nodiscard]] inline std::shared_ptr<CapabilitySet const> borrow_capabilities(
        CapabilitySet const& cs) noexcept {
    return std::shared_ptr<CapabilitySet const>(&cs, [](CapabilitySet const*) noexcept {});
}

struct EffectContext {
    Principal                             principal;
    // ADR-061 §20.3: a shared_ptr, not a raw pointer, since Tier 3 populates this per-request from a
    // RequestAuthority-owned CapabilitySet (rt/agent_session.hpp) that must outlive the coroutine
    // frame that constructed it -- a raw pointer borrowed from that frame would dangle once the
    // originating start_run()/resolve_interaction() call returns. Still non-owning in the sense that
    // matters for the session-level fallback case: AgentSession::capabilities_ is itself a shared_ptr
    // constructed with a no-op deleter (ADR-061 §26.1), so copying it here never implies deleting the
    // caller-owned CapabilitySet it points at.
    std::shared_ptr<CapabilitySet const>  capabilities;  // borrowed or per-request-owned; see above
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
    // `Tool<>::invoke()` calls `ctx.report_progress(ContentItem{...})` to push a `run_event_kind::
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
    // unified-streaming-design-draft.md §5 (Piece E): widened from `std::string_view` to the engine's
    // whole `ContentItem` vocabulary (content.hpp) -- a tool author gets plain text, a structured
    // `Data` fact, or a namespaced `Custom` payload for anything app-specific, not a bespoke shape.
    // Every real bind site forces `tainted = true` (recursively, through any nested `ToolResult`)
    // before constructing the event -- a tool never gets to mark its own pushed content trusted; see
    // `rt/agent_session.hpp`'s `force_tainted()`.
    std::function<void(ContentItem)> report_progress = [](ContentItem) {};

    // 006 §7 / 028 §2: an oversized tool result is promoted to a `BlobRef` rather than inlined into
    // the model's context. Both fields below default OFF (`nullopt` / an empty `std::function`) --
    // every existing caller that never sets them keeps today's behavior (`tool_pipeline.hpp`'s step
    // 9 always builds a `Data` item) byte for byte, the same "optional-but-always-safe-to-call, no
    // wiring means no behavior change" idiom `report_progress`/`ApprovalDecider` above already use.
    //
    // The threshold this call's raw result bytes are compared against. 006 §7's own anti-pattern
    // warning ("a fixed byte constant... applied uniformly regardless of model" is explicitly
    // rejected) is why this is never a compiled-in default: a caller derives it from whatever
    // effective per-turn token budget it is tracking (005 §3's `TokenBudget`, scaled to a declared
    // fraction) and sets it here per call -- the same per-call-hint shape 006 §6b Q5 already
    // established for `watch_resource`'s poll interval, applied to this seam instead.
    std::optional<std::uint64_t> tool_result_byte_threshold;

    // Where an oversized result's bytes actually go once step 9 decides to promote -- content-
    // addressed storage is the named seam (025's worktree object store; 003 §3's own `BlobRef::store`
    // field comment: "which blob store seam resolves this digest"). A caller wires this to a real
    // `WorktreeObjectStore::put_blob` call (core/worktree.hpp) for the run's own worktree. Unset
    // means "no sink available" -- step 9 treats that as "cannot promote" and fails closed rather
    // than silently inlining an over-threshold result anyway, which is exactly the hazard this
    // mechanism exists to prevent (see `tool_pipeline.hpp`'s own comment on this decision).
    std::function<result<BlobRef>(std::span<std::byte const> bytes, std::string const& media_type)>
        blob_sink;
};

} // namespace agentengine
