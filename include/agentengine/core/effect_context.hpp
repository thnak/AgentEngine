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
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
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

    // ADR-170 (GitHub issue #64): the producer side of `run_event_kind::sandbox_exec_started`/
    // `sandbox_exec_finished`. Those two kinds, and 013 §1's `SandboxExecStarted`/`SandboxExecFinished`
    // rows, and the AG-UI projection that turns them into an `ActivitySnapshot`
    // (`protocol/agui/projection.hpp`), all existed and were tested -- with **nothing anywhere in the
    // tree ever emitting one**. Only a synthetic event in `tests/test_rt_agui_projection.cpp` did.
    //
    // A DEDICATED field, not `report_progress` above, for the same "one field per audience" reason
    // ADR-152's own two bridge fields are separate from it: `report_progress` is bound per CALL and
    // carries that call's `call_id` into a `tool_call_delta`; a sandbox exec is not a tool-call
    // delta, has its own correlation id (`SandboxExec::exec_id`), and is emitted by the sandbox
    // layer beneath a tool rather than by the tool's own `invoke()` body.
    //
    // WHY A SINK ON THIS STRUCT, and not a bracket inside `AgentSession` the way `tool_call_started`/
    // `tool_call_finished` are emitted: issue #64 proposed the latter, and it cannot work --
    // `AgentSession` never calls `SandboxBackend::create()`/`exec()` at all (grep it: the class
    // contains no such call site, and never did). Every real sandbox execution in this tree happens
    // BENEATH an opaque `Tool<>::invoke()`, inside `sandbox/`- or `src/backends/`-level code. What
    // those call sites DO all already have is an `EffectContext&` -- `SandboxBackend::create(spec,
    // ctx)`/`exec(handle, req, ctx)` (sandbox/sandbox.hpp) and `Runner::run(request, state, ctx)`
    // (sandbox/runner.hpp) take one by contract -- so the reverse channel this needs already exists
    // structurally; only the field was missing. See ADR-170 §2.
    //
    // Bound and reset with EXACTLY the same call-scoped bracket discipline as `report_progress`
    // (ADR-060), at the same three `invoke_tool()` sites in `rt/agent_session.hpp`, and reset to this
    // no-op by `tool_pipeline.hpp::background_task()` for the same detached-thread reason that
    // function already resets `report_progress`/`sandbox_fs`.
    //
    // Prefer `SandboxExecScope` below over calling this directly -- a raw call site that early-returns
    // between the started and finished halves silently reports a start that never ends.
    std::function<void(run_event_kind, run_event_payload::SandboxExec)> sandbox_exec_sink =
        [](run_event_kind, run_event_payload::SandboxExec) {};

    // ADR-152 (issue #29): two DEDICATED bridge fields for WorkflowSupervisor's per-node
    // multiplexed live event stream (workflow/workflow_event.hpp) -- deliberately NOT the same
    // field as `report_progress` above. A red-team pass on the design found a real collision, not
    // a hypothetical one: a plain `function`-kind executor body is free to call `Tool<>::invoke()`
    // directly (a normal, already-supported pattern, no AgentSession involved), and that call site
    // also reads `ctx.report_progress` -- reusing the same field for the workflow bridge would
    // silently reinterpret that tool's own progress as a workflow-stream event. Same "one field per
    // audience" precedent this file already establishes (`report_progress`/
    // `codeact_preseeded_answers`/`blob_sink` are all separately scoped for exactly this reason).
    //
    // Both default no-op (this file's own "optional-but-always-safe-to-call" idiom). Populated by
    // WorkflowSupervisor, per delivery, ONLY when a workflow-level event stream is actually
    // attached (`WorkflowSupervisor::enable_event_stream()` was called) -- otherwise left at the
    // default no-op, so a body that calls either costs nothing beyond one no-op call when nobody is
    // listening (`workflow_supervisor.hpp`'s own `run_executor_job`).
    //
    // `agent_turn_sink`: `rt::agent_session_as_executor_body()` (ADR-077,
    // rt/agent_workflow_executor.hpp) wires this to the inner AgentSession's own
    // `set_run_event_tap()` for the duration of exactly one call, so the session's REAL RunEvents
    // (model_delta, tool_call_*, ...) reach the workflow stream the instant they're emitted --
    // synchronous, no polling, no restructuring of that adapter's existing drive-to-completion loop
    // needed (the tap fires from inside `emit_run_event_for()`, on whatever thread is already
    // running that coroutine's resumption).
    std::function<void(RunEvent const&)> agent_turn_sink = [](RunEvent const&) {};
    // `moderator_delta_sink`: for a plain `function`-kind body (a moderator/router/planner node)
    // that chooses to call `chat_stream()` itself and forward its own deltas -- opt-in, no engine
    // enforcement that any body actually does this (I3: a body that stays non-streaming is simply
    // coarser-grained observability for that node, never a violation).
    std::function<void(std::string const&)> moderator_delta_sink = [](std::string const&) {};

    // docs/planning/workflow-mid-run-cancellation-design-draft.md (GitHub issue #37, red-teamed):
    // `WorkflowSupervisor`'s own cooperative mid-call cancellation signal (`rt/workflow_supervisor.
    // hpp`'s `cancel()`/`cancel_source_`), populated per-dispatch from the run's own current
    // `stop_token` — default-constructed here (`stop_possible() == false`, `stop_requested() ==
    // false`, always), the same "optional-but-always-safe-to-call" shape `report_progress`/
    // `agent_turn_sink` above already establish, just via a cheap value type instead of a
    // `std::function` — no wiring means a body that checks this simply never sees a stop request.
    // COOPERATIVE ONLY, matching every other bound this engine enforces (`max_rounds`/
    // `deadline_ms`): a body that never reads this runs to its own natural completion for that one
    // call, same as today. Distinct from `deadline` above — that field is a per-call TIMEOUT hint
    // already consumed by the model-call-gateway layer (`core/model_call_gateway.hpp`) for a
    // different purpose; this field means "the WHOLE RUN was told to stop," reaches no further into
    // the call stack than a body's own direct read of it, and is not itself derived from model
    // output (I3) or gated behind any capability (I2) — a pure host-driven external signal, exactly
    // like `cancel()`'s own caller-facing contract. Not an I5 concern: unlike `deadline` (excluded
    // from `SelectFn`'s own visible `EffectContext` specifically because a live read of it is not
    // reproducible across a replay boundary, `core/routing_model_call_gateway.hpp`), no
    // `ExecutorBody`/`WorkflowSupervisor` code path in this codebase makes any I5 replay-determinism
    // claim today — this is the same category of live, unrecorded read, disclosed here rather than
    // silently assumed harmless, so a future I5-related change to this layer knows to revisit it.
    std::stop_token cancellation;

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

    // The session's own mediated filesystem view into its sandbox mount, if one is currently live
    // (008 §6's `per_session` sandbox lifetime: "created on first use, retained while the session
    // is active" -- `nullptr` means no sandbox exists yet for this session, the same "no wiring
    // means no behavior change" default every other opt-in field on this type already uses).
    // Borrowed, never owned here -- same discipline `capabilities`/`bound_capabilities` above
    // already carry: the caller (whatever constructs and keeps the session's real
    // `FileSystemAdapter` implementation alive, e.g. `MediatedFileSystemAdapter`,
    // `src/backends/native_jail/`) must outlive every call this pointer is read during.
    //
    // Deliberately NOT gated by any capability check of its own -- reaching this pointer grants a
    // native `Tool` nothing by itself (I2). A tool that wants to use it must still perform its OWN
    // dynamic capability check against `capabilities` above before calling anything on it, the same
    // "real, path/host-scoped check against `EffectContext::capabilities`, not a static `Tool<>`
    // ceiling" pattern `mediated_shell_dispatch.hpp` and `tools/read_content.hpp` already establish
    // for exactly this reason: which mount/path a call needs is a per-call question, never knowable
    // at a tool's compile-time `Capabilities<...>` declaration the way a fixed-mount `FsRead<"...">`
    // would require. See `tools/read_content.hpp`'s own file-top comment for why that gap existed in
    // the first place -- this field is what closes it.
    FileSystemAdapter* sandbox_fs = nullptr;
};

// ADR-170 (GitHub issue #64): the bracket every real producer of `sandbox_exec_started`/
// `sandbox_exec_finished` should use, rather than calling `EffectContext::sandbox_exec_sink`
// directly twice.
//
// FAILS CLOSED BY CONSTRUCTION, which is the whole reason this is a scope object and not two free
// functions. The finished event's default outcome is `ok = false` with error code
// `"sandbox.exec.abandoned"`: a call site that returns early between provisioning and completion --
// `extract_pdf_text_detail::invoke_worker()` has four such returns between `create()` and `exec()`
// -- reports honestly that the exec did not finish, instead of either emitting nothing (a start with
// no end, which a UI must then time out on its own) or emitting a success that never happened. A
// caller marks the real outcome explicitly with `succeeded()` or `failed(code)`.
//
// Neither copyable nor movable: two live objects sharing one `exec_id` would emit two finished
// events for one exec, and this type's entire value is that the pairing is structural.
//
// Costs one no-op `std::function` call at each end when nothing is listening -- the same
// "optional-but-always-safe-to-call" bargain `report_progress` already makes.
// ae-naming-lint: allow SandboxExecScope — ADR-170 (issue #64); 013 §1 names the event pair, 027 has not been updated to list this producer-side helper
class SandboxExecScope {
public:
    SandboxExecScope(EffectContext& ctx, std::string exec_id, std::string backend, std::string stage)
        : ctx_(ctx) {
        payload_.exec_id = std::move(exec_id);
        payload_.backend = std::move(backend);
        payload_.stage   = std::move(stage);
        run_event_payload::SandboxExec started = payload_;
        started.ok         = true;   // meaningless on a started event; see SandboxExec's own comment
        started.error_code = {};
        ctx_.sandbox_exec_sink(run_event_kind::sandbox_exec_started, std::move(started));
        payload_.ok         = false;
        payload_.error_code = "sandbox.exec.abandoned";
    }

    SandboxExecScope(SandboxExecScope const&)            = delete;
    SandboxExecScope& operator=(SandboxExecScope const&) = delete;
    SandboxExecScope(SandboxExecScope&&)                 = delete;
    SandboxExecScope& operator=(SandboxExecScope&&)      = delete;

    void succeeded() noexcept {
        payload_.ok = true;
        payload_.error_code.clear();
    }
    void failed(std::string error_code) {
        payload_.ok         = false;
        payload_.error_code = std::move(error_code);
    }

    ~SandboxExecScope() {
        // A sink is a host-supplied `std::function`; a throwing one must not propagate out of a
        // destructor. Swallowed deliberately -- an observability event is never worth terminating a
        // run over, and there is no channel to report it through from here anyway.
        try {
            ctx_.sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(payload_));
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

private:
    EffectContext&                   ctx_;
    run_event_payload::SandboxExec   payload_{};
};

} // namespace agentengine
