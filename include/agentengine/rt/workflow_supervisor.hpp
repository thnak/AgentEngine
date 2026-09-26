#pragma once
// ADR-200 (#120 S1): the member function bodies live in src/rt/workflow_supervisor.cpp, compiled once into
// agentengine_rt_workflow; this header keeps the class, every declaration with its comment, small accessors,
// constexpr members, drive<T>() and ScopedForwardedEventSink. Bodies were moved verbatim.
// (Also implements ADR-193 round 2: a failed executor's spend is counted in usage(); §9: WorkflowResult::usage is
// what each run/resume/continue call itself spent.)
// ADR-037 Phase 3, Slice 1: `agentengine::rt::WorkflowSupervisor`, the Quark-actor-free replacement
// for `agentengine::workflow::WorkflowSupervisor` (workflow/supervisor.hpp)'s core superstep loop.
// Lives under `agentengine::rt`, a NEW namespace, deliberately NOT wired into any live call site yet
// -- nothing in the current Quark-based build is touched by this file existing.
//
// SCOPE OF SLICE 1 (matching the discipline `rt::AgentSession`'s own Slice 1 established): the core
// superstep loop -- `run_workflow()`/`resume_workflow()`/`continue_workflow()`'s full `execute()`
// and every pure routing helper (`route_from`/`policy_for`/`edge_fires`/`deliver_or_merge`/
// `record_partial`/`index_of`/`is_output_selected`/`is_retryable`/`mint_interaction`/`finish` -- all
// already zero-Quark-dependency in the original, ported near-verbatim). `deliver_to_fan_in`/
// `seed_fan_in_holds`/`resolve_fan_in_await`/`RunState::held_fan_in` (GitHub issue #52 fix,
// 2026-09-03, generalized by issue #62, 2026-09-03 -- see `HeldFanIn`'s own comment) are new, not
// part of the original port.
//
// SLICE 2 ADDITION (checkpointing): `to_record()`/`restore_from_record()`, `snapshot_record()`, and
// the free functions `save_workflow_checkpoint`/`load_workflow_checkpoint`, built against
// `rt::SessionStore` (already built, session_store.hpp) -- same in-flight-guard design
// `rt::AgentSession`'s own Slice 2 established (`snapshot_record()` acquires `run_mutex_`, the same
// `rt::AsyncMutex` every public entry point already uses for I1, so a concurrent snapshot can never
// observe a torn read of an in-flight round).
//
// A REAL, DELIBERATE NARROWING vs. the Quark original's own `workflow/checkpoint.hpp`, named rather
// than silently dropped:
//   - NO two-phase pending->committed discipline. The original needed it because Quark's `Store`
//     snapshot slot is latest-only, so 014 §5's "rewind to ANY retained checkpoint" required the
//     EventSourced model (`quark::EventLog`) instead -- every checkpoint attempt retained, in commit
//     order, with a two-commit crash-safety split (see checkpoint.hpp's own header comment for the
//     full reasoning). `rt::SessionStore` is a single-slot, overwrite-latest store BY DESIGN (same
//     contract `rt::AgentSession`'s own snapshot already accepts) -- a single `store.save()` call is
//     either fully durable or not, so the two-phase split has nothing left to protect against; one
//     save call replaces both phases, matching `save_agent_session_snapshot()`'s own shape exactly.
//   - NO `retained_checkpoints()`/time-travel (workflow/time_travel.hpp). 014 §5's "rewind to ANY
//     retained checkpoint" genuinely needs an append-only, multi-version log -- `rt::SessionStore`'s
//     single-slot contract cannot provide that no matter how its records are encoded. A real
//     Quark-free append-only log store is separate, not-yet-built infrastructure (nothing in Phase 1
//     provides one) -- named here as a real capability gap, not quietly narrowed away by this
//     record's own shape. Only the LATEST checkpoint is ever recoverable through this slice, the
//     same "no full history, just final state" narrowing `AgentSessionRecord`'s own comment already
//     accepts for sessions.
//   - `RunStateRecord`'s Message-bearing fields (`pending`/`partial`/`selected_output`/port
//     `response`s) ARE carried, unlike `rt::AgentSession`'s own Slice 2 (which explicitly dropped
//     history/state as a narrowing session checkpoints could afford) -- `content_record.hpp`'s own
//     banner already establishes why a workflow's payloads cannot be dropped the same way ("unlike a
//     session's history... a workflow's pending/partial payloads ARE its state"). Encoded via
//     `rt::message_codec.hpp` (message_codec.hpp's own banner explains why that file exists as a
//     small, adapted duplicate of `core/chat_recording.hpp`'s already-proven Message<->JSON codec,
//     rather than an #include of it) -- and, since JSON natively expresses a tagged union, this
//     record stores `agentengine::Message` directly rather than needing the Quark original's own
//     flat `MessageRecord` indirection (`content_record.hpp`'s whole reason for existing was working
//     around `quark::Described` having no variant primitive -- a constraint that does not apply here).
//
// Live view (enable_live_view()/live_view_producer_) -- IMPLEMENTED (a later ADR-037 pass, after
// Slice 1/2 were first written): rides `core/stream.hpp`'s `stream<T>` directly, which by this point
// carries NO Quark dependency at all, type-level or runtime (an earlier ADR-037 pass moved it off
// `quark::ReplyStream` onto `rt::channel<T>`; a later one closed the last type-level residual --
// `terminal()`/`fail_error()` now return `agentengine::stream_terminal`/`agentengine::error`, not
// `quark::ReplyStreamTerminal`/`quark::error`). `workflow/live_view.hpp` (`WorkflowLiveEvent`,
// `ExecutorLiveState`) is reused directly via `#include`, the same "reuse the shape, not reproduce
// it" precedent `workflow/graph.hpp` already set for this file -- that header is already plain value
// types with zero Quark coupling of its own, so there is nothing to hand-reproduce. Fires from the
// SAME superstep-boundary point the original's own `live_view_producer_.push()` did (`execute()`,
// right after `state_.pending = std::move(next);`, before the suspend check) -- built from the SAME
// round-local `exec_deliveries`/`replies`/`port_deliveries` the original used, not reconstructed from
// `state_` after the fact (which no longer distinguishes "ran ok" from "ran and failed" once folded).
//
// Checkpoint hook (set_checkpoint_hook()/checkpoint_hook_) -- ADDED for ADR-037 task #60 (porting
// test_workflow_checkpoint_g2.cpp's G2 promotion-gate sweep). The original's own `CheckpointHook`
// (`std::function<void(std::uint32_t round, RunStateRecord const&)>`) is reproduced verbatim, fired
// from the exact SAME superstep-boundary point as the original (`execute()`, right after
// `state_.pending = std::move(next);`, immediately BEFORE the live-view push above) -- 014 §5's own
// "round N's results are fully folded into state_/ports_, round N+1 has not started yet" reasoning
// carries over unchanged, since this is a caller-injected callback (I2: no ambient Store authority),
// not a design that needed re-deriving for rt:: land. G2 itself needs no OTHER new capability: it
// only ever drives `run_workflow()`/`continue_workflow()` and reads `to_record()`/
// `restore_from_record()`, all of which Slice 2 already built.
//
// Merge-on-join hook (set_merge_on_join_hook()/merge_on_join_hook_) -- ADDED closing decisions/
// ADR-055-conflict-evidence-materialization.md's own §6 residual ("does not build merge-on-join
// wiring itself"). 025-Worktree-and-Virtual-Filesystem.md §4: "A `branch` sub-worktree merges back
// when its agent completes" -- a per-EXECUTOR-completion event, not tied to `edge_kind::fan_in`
// (that is a message-ROUTING join, a different, easily-confused concept this file already implements
// via `route_from()`; 025 §4's own merge-on-join has never been about how a reply gets routed).
// Fired from the SAME per-executor fold loop that already runs `record_partial()`/checks `is_output_
// selected()` for each `exec_deliveries[i]` (`execute()`, right after `rounds_` increments) -- for
// every executor whose `graph_.executors[idx].worktree_mode == sharing_mode::branch` AND whose reply
// this round was `ok` (a failed/retried-out job never "completes" in 025 §4's sense). This file still
// holds NO worktree.hpp type and NO object_store/ref_store reference of its own -- the hook receives
// only the executor's own `id` string; the HOST (which already called `worktree_scoping.hpp`'s
// `mint_executor_worktrees()` before ever driving this supervisor, and so already has every
// `ExecutorWorktreeGrant` in hand, keyed by the SAME executor id) looks up its own grant and performs
// the real `merge_branch_into_parent()`/(on conflict) `materialize_merge_conflicts()` calls against
// its own stores -- the identical "caller-injected callback (I2), no ambient authority" shape
// `checkpoint_hook_` above already established, extended to a second, independent concern riding a
// nearby (not identical) boundary.
//
// A cyclic graph can revisit the SAME executor id across multiple rounds (014 §9 Q2; `mint_executor_
// worktrees()` mints exactly ONE `SubWorktree` per executor id for the whole run, not per visit) --
// this file does NOT attempt to detect "is this the LAST time this executor will ever run" before
// deciding to merge, which would need unsound-to-do-cheaply reachability analysis (the same reason
// ADR-032 §4 gave for why `branch` defaults unconditionally rather than per-provably-safe-node). The
// design instead merges on EVERY completion, unconditionally: a branch's own filesystem work folds
// back into its parent after each round it genuinely finishes, keeping divergence windows as small as
// possible rather than accumulating them across revisits -- a real, deliberate choice (not a punt),
// consistent with 025 §4's own literal wording ("when its agent completes") read per-completion-event
// rather than per-node-forever.
//
// A merge failure (the hook returns a real `error`, matching 025 §4's own "never resolved by
// guessing... a human resolves it" — conflicts are NOT retried automatically) is treated exactly like
// an existing fatal per-round outcome (`routing_failed`/`executor_failed`): `state_.failed_executor`
// is set to the executor id, any same-round `request_port` deliveries are recorded as `unopened_ports`
// (the identical treatment the routing-failure `broke` path already gives them), and the run finishes
// with the new `workflow_status::merge_conflict` rather than continuing into a round built on top of
// an un-merged, possibly-conflicting branch.
//
// THE ONE GENUINELY HARD DESIGN QUESTION: decision 5 in the original's own file banner ("Fan-out is
// ISSUE-ALL-THEN-COLLECT... the supervisor issues every ask for round N before awaiting any of them...
// then collects the futures in FIXED INDEX ORDER") got its REAL concurrency from each executor being a
// separate `quark::ActorRef<FunctionExecutor>`, potentially scheduled on a different worker thread by
// Quark's own scheduler. There is no actor scheduler here. This slice's answer: `rt::ThreadPool`
// (already built and proven, thread_pool.hpp) -- each round's `todo` items become independently
// submitted `task<void>` jobs (issued via `ThreadPool::submit()` for every item BEFORE collecting any,
// preserving decision 5's ordering exactly), collected via `std::future<JobOutcome>::get()` in FIXED
// INDEX ORDER, matching the original's own "completion order is whatever the scheduler produces;
// assembly order is this loop's" guarantee bit-for-bit. Each job's own coroutine body never suspends
// on anything (it wraps one synchronous `ExecutorBody` call, unlike `AsyncMutex`/`channel<T>`-using
// coroutines) -- the textbook safe case for `ThreadPool`'s own documented "only safe for a coroutine
// that suspends purely via nested task<T> -- here, not at all" constraint.
//
// FAULT ISOLATION, changed on purpose, not a narrowing silently accepted: the original relied on
// Quark's `OnFailure<Restart, MaxRestarts<3, Within<1000>>>` actor supervision (a throwing executor
// body restarts its actor, bounded, and Quark dead-letters the faulted ask so the supervisor's
// `co_await` sees an error rather than hanging). `ThreadPool::submit()`'s own `JobOutcome{faulted,
// fault_ptr}` already provides the equivalent containment (a throwing job never crashes the process or
// hangs the collector) -- WITHOUT needing a restart-budget mechanism at all, because there is no
// persistent per-executor actor state a restart would be recovering from: `ExecutorBody` is a plain
// `std::function`, and every invocation is already an independent call with no state to corrupt across
// attempts. A faulted job is classified `failure_class::transient` (matching the original's own
// reasoning for the actor-restart case: "a further attempt meets a fresh instance rather than the one
// that just faulted"), which lets the EXISTING workflow-level retry policy (014 §6's `retry`, already
// unchanged in this port) handle it exactly like any other transient failure -- no second, narrower
// retry mechanism needed underneath it.
//
// `ExecuteReply`/`ExecutorOutcome`/`ExecutorBody`/`failure_marker()` are reused SHAPES, hand-
// reproduced here rather than `#include`-d from `workflow/executor.hpp` (historical: that header,
// since deleted along with the rest of the pre-ADR-037 actor machinery, also pulled in
// `quark/core/actor.hpp`/`quark/core/supervision.hpp` for `FunctionExecutor`'s own actor machinery,
// which this file had to not transitively depend on). Same "reuse the shape, not the include" precedent
// `rt::task<T>` itself set relative to `quark::task<T>` (task.hpp's own banner). `ExecuteRequest`
// itself is NOT reproduced -- it existed only because Quark's fixed 192-byte message-pool cell forced
// the round number into its own struct rather than an ordinary function parameter; that constraint is
// gone, so `run_executor_job()` below just takes `round` as a plain argument.
//
// I1 ("one workflow run, one executor") is enforced the same way `rt::AgentSession` enforces it for
// sessions: `rt::AsyncMutex run_mutex_`, acquired for the whole duration of every public async entry
// point (`run_workflow()`/`resume_workflow()`/`continue_workflow()`).
//
// ADMISSION (ADR-169, GitHub issue #65): I1's mutex answers "one caller at a time"; it never answered
// "WHICH caller". Until ADR-169 those same three entry points performed no ownership check at all --
// `resume_workflow()` in particular let any holder of an `interaction_id` resolve a suspended HITL
// interaction, supply its response `Message` AND name its `routes`, which is authority over the run
// reachable without presenting any (I2), attributed to nobody (I4). `AgentSession`'s own analogues
// (`start_run()`/`resolve_interaction()`) had gated exactly this since Milestone 5 Phase H2; this
// class did not. Every public entry point now consults `admit_caller()` FIRST -- before the `valid_`
// check, before any lookup, before any structural event a denied caller could otherwise push -- and a
// denial returns a BARE `WorkflowResult{workflow_status::admission_denied}` that deliberately carries
// no `open_interactions`, so a probing caller learns nothing it did not already know. See
// decisions/ADR-169-workflow-supervisor-admission.md for the full design, including why this is
// identity-only (no `RequestAuthority` capability half) and how a bound sub-workflow inherits.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/interaction_codec.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/rt/workflow_run_state_record.hpp"  // RunStateRecord + codec (014 §5)
// ADR-169 (issue #65): `Principal` + `principal_admitted_for()`, the one shared ownership predicate
// 018 §2 requires at every actor boundary. Depends only on core/error.hpp -- no cycle with rt/.
#include "agentengine/trust/principal.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/live_view.hpp"
#include "agentengine/workflow/workflow_event.hpp"

namespace agentengine::rt {

// -- Reused shapes (see file banner: hand-reproduced, not #include-d from workflow/executor.hpp) ---

// ae-naming-lint: allow ExecutorOutcome — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ExecutorOutcome {
    agentengine::Message     payload;
    std::vector<std::string> routes;
    // ADR-149 (issue #28 item 2): an explicit, opt-in "no progress this round" self-report. Trusted
    // by `execute()`'s stall/reset bookkeeping ONLY when this executor's id matches
    // `WorkflowSupervisor::initialize()`'s `designated_stall_reporter` -- every other executor
    // setting this is inert, by design (ADR-149 §3 finding 1: NOT a generic "any output can end the
    // run" signal, which would cross I2/I3 the way `routes` -- bounded to edges the graph author
    // already wired -- does not). Default `false`; appended as a trailing field so every existing
    // `ExecutorOutcome{message}`/`{message, routes}` call site is unaffected.
    bool stalled = false;
    // GitHub issue #35 follow-up (closing ADR-162's own named residual: WorkflowChatClient could
    // never report honest token Usage because nothing in this engine tracked it). Real, honest, never
    // fabricated -- an ordinary `function`-kind body leaves this at its default zero `Usage{}`
    // (structurally correct: it made no metered model call through any tracked path). Populated ONLY
    // by `agent_session_as_executor_body()` (rt/agent_workflow_executor.hpp), from the wrapped
    // AgentSession's own real `run_usage()` -- see that file's own updated comment. Appended trailing,
    // same field-ordering discipline `stalled` above already established, so every existing
    // `ExecutorOutcome{message}`/`{message, routes}` call site keeps compiling unchanged.
    agentengine::Usage usage{};

    ExecutorOutcome() = default;
    ExecutorOutcome(agentengine::Message m) : payload(std::move(m)) {}  // NOLINT(google-explicit-constructor)
    ExecutorOutcome(agentengine::Message m, std::vector<std::string> r)
        : payload(std::move(m)), routes(std::move(r)) {}
};

// ae-naming-lint: allow ExecutorBody — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using ExecutorBody =
    std::function<agentengine::result<ExecutorOutcome>(agentengine::Message const&, agentengine::EffectContext&)>;

// OQ-19 (OpenQuestions.md; docs/planning/agent-as-workflow-executor-design-draft.md §5 item 4): a
// FIXED, non-templated functor type -- independent of rt::agent_workflow_executor.hpp's own adapter
// template parameters (ChatClientT/StateT/HistoryProviderT) -- so `initialize()` below can
// structurally tell a genuinely agent-backed body apart from an ordinary function closure via
// `std::function`'s own built-in type erasure (`bodies_[i].target<AgentExecutorBodyTag>() !=
// nullptr`), rather than trusting a data-only check on the graph's declared `kind` (which a caller
// could satisfy by mistake with a plain function that never touches an `AgentSession` at all).
// Defined HERE, not in the adapter header, specifically so this file never has to
// `#include "agentengine/rt/agent_session.hpp"` (a much heavier header) just to name this type --
// the adapter header includes both this file and agent_session.hpp and constructs one of these.
// ae-naming-lint: allow AgentExecutorBodyTag — the structural marker OQ-19's design draft names verbatim
class AgentExecutorBodyTag {
public:
    using Impl = std::function<agentengine::result<ExecutorOutcome>(agentengine::Message const&,
                                                                      agentengine::EffectContext&)>;

    explicit AgentExecutorBodyTag(Impl impl) : impl_(std::move(impl)) {}

    agentengine::result<ExecutorOutcome> operator()(agentengine::Message const& in,
                                                      agentengine::EffectContext& ctx) const {
        return impl_(in, ctx);
    }

private:
    Impl impl_;
};

// ae-naming-lint: allow ExecuteReply — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ExecuteReply {
    agentengine::Message     payload;
    std::vector<std::string> routes;
    bool                     ok    = true;
    agentengine::failure_class klass = agentengine::failure_class::fatal;
    // ADR-149: threaded from `ExecutorOutcome::stalled` the same way `routes` already is. Default
    // `false`, appended trailing so every existing 4-arg brace-init keeps compiling unchanged.
    bool stalled = false;
    // ADR-157 (issues #33/#38): set ONLY by run_sub_workflow_job(), ONLY when a sub_workflow-kind
    // delivery's inner run status was `suspended` -- every other field above is unused/default in
    // that case. A round-local, transient marker (never persisted/checkpointed, unlike OpenPort --
    // see docs/planning/sub-workflow-nested-request-port-design-draft.md §2 finding 2 for why THAT
    // distinction is what keeps this safe) telling execute()'s five per-index loops to skip this
    // entry entirely rather than treat it as a real reply.
    std::optional<std::string> pending_sub_workflow_inner_interaction_id;
    // GitHub issue #35 follow-up -- threaded from `ExecutorOutcome::usage` the same way `stalled`
    // already is. NOT populated for a suspended sub_workflow delivery (matches
    // `pending_sub_workflow_inner_interaction_id`'s own "every other field above is unused/default"
    // disclosure immediately above) -- a real, named residual: usage incurred by a nested sub_workflow
    // BEFORE it suspends is not yet bubbled into the outer run's total until that nested run eventually
    // resolves (via `resume_workflow()`'s own `pending_sub_workflows_` branch) and its own OpenPort
    // response completes normally, at which point it is captured retroactively as part of that flow --
    // see `WorkflowSupervisor::usage()`'s own comment for the full disclosure.
    agentengine::Usage usage{};
};

[[nodiscard]] inline agentengine::Message failure_marker(std::string const& executor_id,
                                                          agentengine::failure_class klass) {
    auto const* name = "fatal";
    switch (klass) {
        case agentengine::failure_class::transient: name = "transient"; break;
        case agentengine::failure_class::policy:    name = "policy"; break;
        case agentengine::failure_class::contract:  name = "contract"; break;
        case agentengine::failure_class::resource:  name = "resource"; break;
        case agentengine::failure_class::fatal:     name = "fatal"; break;
    }
    agentengine::ContentItem item{};
    item.origin  = agentengine::content_origin::system;
    item.tainted = false;
    item.value   = agentengine::Error{"executor '" + executor_id + "' failed (" + name + ")"};
    agentengine::Message m{};
    m.role = agentengine::role::system;
    m.content.push_back(std::move(item));
    return m;
}

// -- Ported verbatim from workflow/supervisor.hpp (pure data, zero Quark dependency there either) --

// ae-naming-lint: allow workflow_status — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class workflow_status {
    completed,
    suspended,
    bound_max_rounds,
    bound_deadline,
    executor_failed,
    routing_failed,
    // ADR-055 follow-up (see file banner's "Merge-on-join hook" paragraph): a branch-mode executor's
    // merge-on-join failed (a genuine conflict, or the host's own store I/O failure) -- 025 §4's own
    // "never resolved by guessing" rule means this is a terminal outcome, never auto-retried, the
    // same shape `executor_failed`/`routing_failed` already are.
    merge_conflict,
    // ADR-149 (issue #28 item 2): `TerminationBound::max_stalls`/`max_resets` tripped. `bound_max_
    // stalls` -- `max_resets` was never set, so the FIRST stall trip ends the run. `bound_max_resets`
    // -- `max_resets` WAS set, and stall trips exceeded it (every trip under the ceiling is silently
    // absorbed and the run continues). Mirrors `bound_max_rounds`'s own shape: an honest, expected,
    // non-error termination, not a fault.
    bound_max_stalls,
    bound_max_resets,
    invalid,
    // docs/planning/workflow-mid-run-cancellation-design-draft.md (GitHub issue #37, red-teamed):
    // a caller called cancel() (or held a stop_token this run's own cancel_source_ shares) and the
    // round loop observed it at the same round-boundary point every other bound is already checked.
    // An honest, expected, non-error termination -- the same shape bound_max_rounds/bound_max_stalls
    // already are, never a fault.
    cancelled,
    // ADR-169 (GitHub issue #65): the caller named on this request is not admitted for this
    // supervisor's owning principal (or omitted an identity a `require_caller()` supervisor demands).
    // Deliberately DISTINCT from `invalid`, which this path could have reused: `invalid` already
    // means three unrelated things (graph never validated, unknown/replayed interaction_id, a bound
    // sub-workflow gone), and folding a security decision into that bucket would make an admission
    // denial indistinguishable from a typo in an id -- unusable for the audit surface I4 requires,
    // and unusable for a host that wants to alert on one but not the other. No run state is touched
    // on this path, so it is not a run OUTCOME in the sense every other enumerator here is: it is a
    // refusal to start/resume at all.
    admission_denied,
};

// ADR-152 (issue #29): the plain-string form of `workflow_status` used ONLY for
// workflow_event_payload::RunFailed's `status_tag` field (workflow/workflow_event.hpp) --
// workflow/ does not depend on rt/ (this file already depends on workflow/; the reverse would be
// circular), so the payload carries a string, and this is the one place that converts.
[[nodiscard]] inline char const* workflow_status_tag(workflow_status s) noexcept {
    switch (s) {
        case workflow_status::completed:         return "completed";
        case workflow_status::suspended:         return "suspended";
        case workflow_status::bound_max_rounds:  return "bound_max_rounds";
        case workflow_status::bound_deadline:    return "bound_deadline";
        case workflow_status::executor_failed:   return "executor_failed";
        case workflow_status::routing_failed:    return "routing_failed";
        case workflow_status::merge_conflict:    return "merge_conflict";
        case workflow_status::bound_max_stalls:  return "bound_max_stalls";
        case workflow_status::bound_max_resets:  return "bound_max_resets";
        case workflow_status::invalid:           return "invalid";
        case workflow_status::cancelled:         return "cancelled";
        case workflow_status::admission_denied:  return "admission_denied";
    }
    return "invalid";
}

// ADR-169 (GitHub issue #65): all three request types below carry the SAME additive, defaulted
// `caller` field, appended last -- this project's established field-ordering convention, so every
// existing positional aggregate-init call site (`RunWorkflow{input}`, `ResumeWorkflow{id, msg,
// routes}`, `ContinueWorkflow{}`) compiles and behaves byte-for-byte unchanged.
//
// A full `agentengine::Principal`, NOT the narrower `rt::SessionCaller` (`{id, tenant_id}` only)
// that `StartRun`/`ResolveInteraction` carry. That narrowing exists in agent_session.hpp for a
// wire-shape reason its own comment records, and it has a real cost there: reconstructing
// `Principal{caller->id, caller->tenant_id}` DROPS `on_behalf_of`, so a legitimately delegated
// principal (007 §2) is not admitted by an `AgentSession`'s non-Tier-3 branch even though
// `principal_admitted_for()` would admit it. There is no wire shape here to be constrained by, so
// this surface carries the identity the predicate actually reads, and the single-hop delegation
// 018 §2 grants works -- matching `rt/multi_agent.hpp:191`'s own full-`Principal` use, not
// agent_session.hpp's narrowing.
//
// Identity ONLY -- deliberately no `RequestAuthority` (identity + `CapabilitySet` + expiry) half.
// ADR-169 §4: a workflow's capabilities are per-EXECUTOR (`contexts_`, fixed at `initialize()` and
// checked against each node's `capability_ceiling` by `check_workflow_executable()`), not
// per-request; a per-request `CapabilitySet` would need a defined interaction with that per-executor
// ceiling that nothing in 014/007 specifies today. Building half of it here -- accepting the
// capabilities and ignoring them -- would be worse than not having it.
struct RunWorkflow {
    agentengine::Message input;
    std::optional<agentengine::Principal> caller = std::nullopt;
};

// ae-naming-lint: allow ContinueWorkflow — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContinueWorkflow {
    std::optional<agentengine::Principal> caller = std::nullopt;
};

// ae-naming-lint: allow ResumeWorkflow — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ResumeWorkflow {
    std::string           interaction_id;
    agentengine::Message  response;
    std::vector<std::string> routes;
    std::optional<agentengine::Principal> caller = std::nullopt;
};

struct ExecutorOutput {
    std::string           executor_id;
    std::uint32_t         round = 0;
    agentengine::Message  payload;
};

// ae-naming-lint: allow WorkflowResult — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct WorkflowResult {
    workflow_status status = workflow_status::invalid;
    std::uint32_t   rounds = 0;
    agentengine::Message output{};
    std::vector<ExecutorOutput> partial{};
    std::string     failed_executor{};
    std::vector<agentengine::Interaction> open_interactions{};
    std::vector<std::string> unopened_ports{};
    // ADR-193 §9 (issue #113): what THIS call spent -- a `run_workflow()` reports its whole run so far, a
    // `resume_workflow()`/`continue_workflow()` only what it added. Measured under the supervisor's own run lock,
    // so a caller never has to difference two reads of the cumulative `usage()` (which a fresh run resets, and
    // which another caller can advance in between -- `WorkflowChatClient` charged a wrapped delta that way).
    agentengine::Usage usage{};
};

// ADR-149 (issue #28 item 6), REVISED SCOPE -- ADR-149 §3 finding 8 (a red-team pass found this
// during design, before any code existed): `record_partial()` keeps AT MOST ONE entry per
// `executor_id` in `partial` (overwritten in place on every revisit, never appended) -- so a cyclic
// graph (which is what Magentic/Planner IS: the manager and every participant are revisited across
// many rounds) has no "round order" left to extract by the time a run completes. This is therefore
// NOT a full multi-visit transcript -- it is exactly what `partial` actually contains, honestly
// named: the most recent message each executor produced. A genuine multi-visit transcript needs a
// per-round hook into `execute()`'s dispatch loop (right where `record_partial()` currently
// overwrites) -- the SAME mechanism a follow-on ADR/issue #29's per-executor event multiplexing needs to
// build anyway, so it is deferred there rather than building a second, throwaway hook here.
using Transcript = std::vector<agentengine::Message>;

[[nodiscard]] inline Transcript latest_outputs_of(WorkflowResult const& r) {
    Transcript out;
    out.reserve(r.partial.size());
    for (ExecutorOutput const& o : r.partial) out.push_back(o.payload);
    return out;
}

// -- Slice 2: the checkpoint record + its JSON codec (see file banner) -----------------------------

// `RunStateRecord` and its JSON codec live in rt/workflow_run_state_record.hpp (included above).

// ae-naming-lint: allow WorkflowSupervisor — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class WorkflowSupervisor {
public:
    // docs/planning/nested-workflow-threadpool-budget-design-draft.md (red-teamed; issue #42 item
    // 2): `worker_budget` forwards directly to `pool_`'s own existing constructor --
    // `ThreadPool`'s own `worker_count == 0` sentinel already means "use `default_worker_count()`,"
    // so `worker_budget`'s own default of `0` is source-compatible with every existing
    // `WorkflowSupervisor sup;`/`std::make_shared<WorkflowSupervisor>()` call site (zero breakage).
    // NOT a `bind_sub_workflow()` parameter -- by the time any bind call could happen, `pool_`'s
    // workers already exist, already running at whatever size this constructor gave them (confirmed
    // by red-team: no real or plausible call site in this codebase ever constructs-then-binds in
    // the other order). A caller composing a nested tree calls `split_worker_budget()`
    // (thread_pool.hpp) once per level and constructs each `inner` with its own share.
    explicit WorkflowSupervisor(std::size_t worker_budget = 0) : pool_(worker_budget) {}

    // Public read of the private `kMaxNestingDepth` bound -- a plain compile-time constant with no
    // encapsulation risk; exposed so callers/tests can reason about the declared ceiling without
    // duplicating the literal (a duplicated literal would silently drift from the real bound).
    [[nodiscard]] static constexpr std::size_t max_nesting_depth() noexcept { return kMaxNestingDepth; }

    // ---- ADR-169 (GitHub issue #65): admission ------------------------------------------------
    //
    // The owning principal every `RunWorkflow`/`ResumeWorkflow`/`ContinueWorkflow` caller is admitted
    // against, mirroring `rt::AgentSession::principal_`'s role exactly. Host configuration, set by the
    // host binary (never from model output -- I3), and deliberately NOT a parameter of `initialize()`
    // nor part of `RunStateRecord`:
    //
    //   - Not in `initialize()` because that function is the GRAPH's bring-up (graph/bodies/contexts)
    //     and is legitimately called twice in the ADR-157 bind sequence; an admission setting reset by
    //     the second call would be a fail-OPEN footgun of exactly the shape this ADR exists to close.
    //     Ordering is therefore free: `set_principal()` before or after `initialize()` both work.
    //   - Not in `RunStateRecord` for the same reason `designated_stall_reporter_`/`bodies_`/
    //     `contexts_`/`sub_workflows_` are not: a restored run's caller re-supplies host configuration
    //     fresh. Because these are object-level members that `restore_from_record()` never touches, a
    //     supervisor configured and THEN restored keeps its gate -- proven by A10 in
    //     tests/test_rt_workflow_supervisor_admission.cpp, which is the positive control for the
    //     "checkpoint round-trip silently disarms the gate" failure this shape could otherwise have.
    //
    // Unset (a default-constructed `Principal`) plus a caller-bearing request is DENIED, not admitted:
    // `principal_admitted_for()` needs a real owner to admit against, and "no owner configured" must
    // never read as "everyone is the owner". This is the same behavior `AgentSession` already has for
    // an unconfigured session, reached the same way (through the predicate, not a special case).
    void set_principal(agentengine::Principal principal) {
        principal_             = std::move(principal);
        inherited_admission_   = false;  // an explicit host choice outranks any inherited value
        propagate_admission_to_children();
    }
    [[nodiscard]] agentengine::Principal const& principal() const noexcept { return principal_; }

    // Strict mode: a request with NO `caller` is refused outright rather than admitted. Off by
    // default -- every existing call site passes no caller, and flipping the default would break all
    // of them while proving nothing about the mechanism. A host that fronts this supervisor for
    // anything but its own trusted in-process code turns this ON; see ADR-169 §6 for why the engine
    // cannot make that choice on the host's behalf (ADR-070's Delegated Decision Seam: explicit host
    // opt-in, fails closed once set, host code never model output, always audited).
    void set_require_caller(bool require) {
        require_caller_      = require;
        inherited_admission_ = false;  // same reason as set_principal()
        propagate_admission_to_children();
    }
    [[nodiscard]] bool require_caller() const noexcept { return require_caller_; }

    // Monotonic count of requests refused by `admit_caller()`, mirroring
    // `AgentSession::admission_denied_count_`. The cheap metric a host alerts on, and the negative
    // control every admission test in this project asserts against -- a gate that "denies" by doing
    // nothing observable is exactly the security claim that cannot fail, which CLAUDE.md forbids.
    // Never reset by `run_workflow()` (unlike `rounds_`/`total_usage_`): a denial is not part of any
    // one run's accounting, it is a property of this supervisor's whole lifetime.
    [[nodiscard]] std::uint64_t admission_denied_count() const noexcept { return admission_denied_count_; }

    // `bodies`/`contexts` are parallel to `graph.executors` by INDEX -- the same convention the
    // original's `refs_` used (see that file's own comment: it was originally forced by the 192-byte
    // wire cell, but index-addressing is kept here anyway since the graph description already
    // provides it and a string-id lookup per round would be real, needless work).
    // `contexts` defaults empty; a missing/short entry is filled with a fresh `EffectContext{}` --
    // matching what every real caller in the current test suite already passes today (every
    // `FunctionExecutor::initialize()` call site uses a default-constructed one; nothing in this
    // codebase yet populates it meaningfully).
    //
    // OQ-19: for an `agent`-kind executor, `contexts[i].capabilities` is now genuinely consumed --
    // `rt::agent_session_as_executor_body()` (rt/agent_workflow_executor.hpp) reads it on EVERY call
    // via `AgentSession::set_capabilities()`, and `check_workflow_executable(graph_, contexts_)`
    // below verifies it satisfies `graph_.executors[i].capability_ceiling` before the graph is
    // accepted as runnable at all. LIFETIME CONTRACT (matching `EffectContext::capabilities`'s own
    // documented borrowed-pointer contract, core/effect_context.hpp): whatever `CapabilitySet` a
    // `contexts[i].capabilities` shared_ptr aliases must outlive every call this `WorkflowSupervisor`
    // instance ever dispatches to that node -- `contexts_` itself (this object's own member, storing
    // a COPY of that shared_ptr for the whole lifetime of this instance) is what keeps the pointee
    // alive across rounds; a per-call `EffectContext` copy handed to `run_executor_job()` below is
    // only ever a SHORT-LIVED alias of the same underlying object, never its owner.
    // `designated_stall_reporter`: ADR-149 (issue #28 item 2) -- the ONE executor id whose
    // `ExecutorOutcome::stalled` self-report `execute()` trusts for `TerminationBound::max_stalls`/
    // `max_resets` bookkeeping; every other executor's `stalled` is inert. Empty (default) disables
    // stall/reset tracking entirely, regardless of what `TerminationBound` declares -- an explicit
    // host opt-in, matching ADR-070/ADR-071's "fails closed/safe when unset" Delegated Decision Seam
    // shape. See `ExecutorOutcome::stalled`'s own comment for the full I2/I3 reasoning.
    void initialize(agentengine::workflow::Workflow graph, std::vector<ExecutorBody> bodies,
                     std::vector<agentengine::EffectContext> contexts = {},
                     std::string designated_stall_reporter = {});

    // ADR-157 (issues #33/#38): binds a `sub_workflow`-kind node (declared, until now,
    // unconditionally refused -- see graph.hpp's own updated comment) to a real, already-
    // `initialize()`-d inner `WorkflowSupervisor`. Called AFTER `initialize()` -- like
    // `set_checkpoint_hook()`/`set_merge_on_join_hook()`, an opt-in configuration call, not a
    // constructor parameter. Recomputes `valid_` immediately (from the cached `valid_base_` plus a
    // fresh `sub_workflow_kind_nodes_are_bound()` check) so a caller does NOT need to call
    // `initialize()` a second time after binding every sub_workflow-kind node -- `valid_` simply
    // flips true the moment the last one is bound.
    //
    // SILENTLY REFUSED (this node stays unbound; `sub_workflow_kind_nodes_are_bound()` is what
    // surfaces it as invalid, not this call -- mirroring `set_checkpoint_hook()`'s own
    // "caller-injected callback, no ambient validation" shape) in THREE cases:
    //   - `executor_id` names a node not present in `graph()` at all.
    //   - `executor_id` names a real node that is NOT `sub_workflow`-kind (the comment above this
    //     ADR-157 pass claimed this was already refused; it was not actually checked -- fixed here).
    //   - `inner` is ALREADY bound to a DIFFERENT executor_index in THIS SAME graph (ADR-157 §4's
    //     own documented-but-unenforced caller contract: a single `inner` instance must be bound to
    //     at MOST one executor_index, because the OQ-19-generalized same-round quarantine dedupes
    //     by executor_index, not by which `WorkflowSupervisor` a node happens to be bound to -- two
    //     DIFFERENT sub_workflow nodes both wrapping the SAME `inner` would defeat that quarantine's
    //     own per-instance concurrency guarantee (docs/planning/sub-workflow-nested-request-port-
    //     design-draft.md's own `drive()` comment). Checked by pointer identity (`inner.get()`)
    //     against every value already in `sub_workflows_` -- cheap, and this map is expected to stay
    //     small (one entry per sub_workflow-kind node in a graph, not a hot-path structure).
    //
    // See docs/planning/sub-workflow-nested-request-port-design-draft.md §3a for the full design.
    // `inner` capability sourcing: entirely decoupled from this node's own `capability_ceiling`,
    // mirroring `workflow_as_executor_body()`'s (ADR-150) own I2 answer exactly -- `inner`'s own
    // executors run under whatever `EffectContext`s were passed to `inner->initialize(...)` before
    // ever binding it here.
    void bind_sub_workflow(std::string const& executor_id, std::shared_ptr<WorkflowSupervisor> inner);

    // docs/planning/workflow-mid-run-cancellation-design-draft.md (GitHub issue #37, red-teamed):
    // callable from ANY thread while run_workflow()/resume_workflow()/continue_workflow() is in
    // flight on another -- std::stop_source::request_stop() is specified thread-safe with no
    // external synchronization needed (the same guarantee std::jthread's own built-in stop
    // mechanism relies on, already used by this codebase's own ThreadPool). Idempotent (a second
    // call is a harmless no-op, matching request_stop()'s own contract) and requires no lock: it
    // never touches state_/ports_/rounds_ or anything else run_mutex_ (I1) protects -- a pure
    // out-of-band signal the round loop polls on its own schedule, exactly like
    // core/stream.hpp's ADR-017 stop_source/stop_token precedent this mechanism reuses.
    void cancel() noexcept { cancel_source_.request_stop(); }

    // A caller can hold this independently (or hand it to unrelated code that wants to observe the
    // same cancellation) -- mirrors stream_producer<T>::stop_token()'s own exposed-handle shape.
    [[nodiscard]] std::stop_token cancellation_token() const noexcept {
        return cancel_source_.get_token();
    }

    [[nodiscard]] std::string const& designated_stall_reporter() const noexcept {
        return designated_stall_reporter_;
    }
    [[nodiscard]] std::uint32_t stall_streak() const noexcept { return stall_streak_; }
    [[nodiscard]] std::uint32_t resets_used() const noexcept { return resets_used_; }

    [[nodiscard]] agentengine::workflow::Workflow const& graph() const noexcept { return graph_; }
    [[nodiscard]] std::uint32_t rounds_executed() const noexcept { return rounds_; }
    [[nodiscard]] std::string const& run_id() const noexcept { return run_id_; }

    // decisions/ADR-070-host-configurable-responsibility-boundary.md: `true` whenever
    // `graph().bound.token_budget` is set -- see that field's own doc comment
    // (workflow/graph.hpp) for why. Unconditional and deterministic (a pure read of already-
    // validated `graph_` state, nothing timing-dependent), so a host can check this right after
    // `initialize()` and log/assert/refuse to run/whatever fits its own policy -- there is no path
    // where a `token_budget`-bearing workflow makes this return `false`.
    [[nodiscard]] bool token_budget_unenforced() const noexcept {
        return graph_.bound.token_budget.has_value();
    }

    // ADR-157: also unions `pending_sub_workflows_` -- every entry there is, by construction,
    // always "open" (an entry is erased the instant it resolves, see resume_workflow()'s new
    // branch), unlike `ports_`, which retains resolved entries until execute()'s own port-prologue
    // folds them. See §3e of the sub-workflow design draft for why this union must be unconditional
    // (not gated behind a particular `status`) -- an unrelated round abort must never silently
    // orphan a real, live nested interaction.
    [[nodiscard]] std::vector<agentengine::Interaction> open_interactions() const;

    // GitHub issue #35 (docs/planning/workflow-as-chatclient-adapter-design-draft.md §5): `open_
    // interactions()`'s own bare `Interaction` (interaction_id/run_id/reason/timestamps only) carries
    // no ask PAYLOAD -- a `WorkflowChatClient`-shaped caller needs to know WHAT is being asked, not just
    // that something is. Narrowed to `OpenPort` only (NOT a uniform superset of open_interactions()):
    // an `OpenPort`'s own ask genuinely lives in `p.response` pre-resolution (see `port_deliveries`'s
    // own fold below, `ports_.push_back(OpenPort{interaction, d.executor_index, d.payload, ...})` --
    // `d.payload` IS the ask, stored in the SAME field slot the real answer later overwrites), but a
    // `PendingSubWorkflow` entry (ADR-157's nested sub-workflow mechanism) carries no ask `Message` at
    // all -- the real ask lives inside the wrapped nested `WorkflowSupervisor`, reachable only by a
    // provably-terminating recursive walk this accessor does not attempt (real, unbuilt follow-on work).
    // A `PendingSubWorkflow` entry therefore gets an honestly-EMPTY `Message{}` placeholder here, never
    // a fabricated one -- narrower than a first pass of this design claimed, corrected before
    // implementation. `open_interactions()` itself is unchanged; every existing caller keeps compiling.
    struct InteractionAsk {  // ae-naming-lint: allow InteractionAsk — GitHub issue #35 names this concept normatively; 027 has not been updated to list it
        agentengine::Interaction interaction;
        agentengine::Message     ask;
    };
    [[nodiscard]] std::vector<InteractionAsk> open_interaction_asks() const;

    // GitHub issue #35 follow-up (closing WorkflowChatClient/ADR-162's own named blocker: nothing in
    // this engine tracked real token usage, so that adapter could never report anything honest). Real,
    // cumulative, LIVE (a caller can read it mid-run, the same "read directly off the supervisor
    // instance" shape `open_interactions()` already establishes) -- summed from every `agent`-kind
    // executor's own real `AgentSession::run_usage()` (via `agent_session_as_executor_body()`), plus
    // any nested `sub_workflow`'s own recursively-summed total once it resolves (`OpenPort::usage`'s
    // own comment has the full nested-suspend disclosure). Reset only by a fresh `run_workflow()` call,
    // NOT by `resume_workflow()`/`continue_workflow()` -- the same "accumulates across a suspend/resume
    // lifecycle" contract `rounds_` itself already has. A caller that wants what ONE call spent reads
    // `WorkflowResult::usage` (ADR-193 §9), never a difference of two reads of this: a fresh run resets it, so
    // "after minus before" across a `run_workflow()` is wrong (and wraps when the new run spent less).
    //
    // NAMED, DISCLOSED RESIDUAL, not fixed here: an ordinary `function`-kind executor's `ExecutorBody`
    // is arbitrary C++, free to hold and call a real `ChatClient` directly without ever reporting
    // through this tracked path -- this accessor answers "how much did every TRACKED (agent-kind, or a
    // resolved nested sub_workflow) dispatch cost," not "prove this run made zero untracked calls." A
    // caller relying on this for a hard budget ceiling should know it bounds the common, supported
    // path, not every conceivable graph shape.
    [[nodiscard]] agentengine::Usage usage() const noexcept { return total_usage_; }

    // 014 §5's checkpoint-at-superstep-boundary hook -- see file banner's "Checkpoint hook" paragraph.
    // Reproduced verbatim from the original: a caller-injected callback (I2), never an ambient Store,
    // fired with the round just completed and a full `to_record()` snapshot taken at exactly that
    // boundary.
    using CheckpointHook = std::function<void(std::uint32_t round, RunStateRecord const&)>;
    void set_checkpoint_hook(CheckpointHook hook) { checkpoint_hook_ = std::move(hook); }

    // ADR-055 follow-up -- see file banner's "Merge-on-join hook" paragraph for the full design.
    // Fired once per branch-mode executor, immediately after ITS OWN reply is folded successfully
    // this round. `result<void>{}` means "merged cleanly, or nothing needed merging"; a real `error`
    // is a fatal, non-retried outcome for the whole run (`workflow_status::merge_conflict`).
    using MergeOnJoinHook = std::function<agentengine::result<void>(std::string const& executor_id)>;
    void set_merge_on_join_hook(MergeOnJoinHook hook) { merge_on_join_hook_ = std::move(hook); }

    // 014 §7's live-view bullet. Constructs a fresh, directly-connected producer/consumer pair
    // (`agentengine::make_stream`, core/stream.hpp) over `WorkflowLiveEvent`, keeps the producer, and
    // hands the caller the consumer -- see file banner for exactly where in `execute()` it fires from.
    // A second call REPLACES the producer (the previous consumer then reads `done()`); there is no
    // fan-out to multiple simultaneous live viewers in this build, matching the original exactly.
    [[nodiscard]] agentengine::stream<agentengine::workflow::WorkflowLiveEvent> enable_live_view(
        std::pmr::memory_resource* mr,
        agentengine::stream_config<agentengine::workflow::WorkflowLiveEvent> cfg = {}) {
        auto pair            = agentengine::make_stream<agentengine::workflow::WorkflowLiveEvent>(mr, cfg);
        live_view_producer_  = std::move(pair.producer);
        return std::move(pair.consumer);
    }

    // ADR-152 (issue #29): the fine-grained, genuinely-live sibling of enable_live_view() above --
    // docs/planning/workflow-event-stream-design-draft.md has the full design and the red-team pass
    // that shaped it. Same "second call replaces the [structural] producer" single-consumer
    // convention enable_live_view()/AgentSession::enable_event_stream() already use. Once called:
    // every round's structural decisions are pushed live (single-writer, execute()'s own thread,
    // the SAME safe shape enable_live_view() already uses -- zero new concurrency risk), AND every
    // dispatched executor's own multiplexed events (an agent-kind node's real per-token deltas, or
    // a function-kind moderator's own forwarded chat_stream() deltas) are forwarded the instant
    // they happen, through a dedicated non-blocking sink (workflow/multiplex_sink.hpp) -- never the
    // blocking channel the structural bucket rides, precisely because a ThreadPool worker thread
    // doing real workflow compute must never be able to stall on a lagging consumer (see the design
    // draft §2 for the full red-teamed reasoning this split exists to satisfy).
    [[nodiscard]] agentengine::workflow::WorkflowEventStream enable_event_stream(
        std::pmr::memory_resource* mr,
        agentengine::stream_config<agentengine::workflow::WorkflowEvent> cfg = {}) {
        auto pair = agentengine::make_stream<agentengine::workflow::WorkflowEvent>(mr, cfg);
        workflow_event_producer_       = std::move(pair.producer);
        workflow_event_stream_enabled_ = true;
        return agentengine::workflow::WorkflowEventStream(std::move(pair.consumer), multiplex_sink_);
    }

    task<WorkflowResult> run_workflow(RunWorkflow request);

    // ADR-193 §9: the lock is taken here, not in the body, so the spend this call adds is measured under it.
    task<WorkflowResult> resume_workflow(ResumeWorkflow request);

    task<WorkflowResult> continue_workflow(ContinueWorkflow request);

private:
    // Both bodies below run with `run_mutex_` held by their public wrapper above.
    task<WorkflowResult> resume_workflow_locked(ResumeWorkflow request);

    task<WorkflowResult> continue_workflow_locked(ContinueWorkflow request);

    // ADR-193 §9: this call's spend, from two readings of the cumulative total taken under one hold of `run_mutex_`
    // (so the total only grew between them). Saturating all the same: a wrapped unsigned difference is a charge of
    // ~2^64 tokens.
    [[nodiscard]] agentengine::Usage usage_added_since(agentengine::Usage const& before) const noexcept;

public:
    // ---- Slice 2: checkpointing (file banner has the design writeup) --------------------------

    // Unlocked, synchronous -- matches rt::AgentSession::to_record()'s own shape. Not the caller's
    // normal way to take a checkpoint; see snapshot_record() below for the locked path.
    [[nodiscard]] RunStateRecord to_record() const;

    void restore_from_record(RunStateRecord const& rec);

    // The in-flight-safe read: acquires run_mutex_ for the whole read, the same I1 guard every
    // public entry point uses -- see file banner. save_workflow_checkpoint() (free function, below)
    // is built on this.
    [[nodiscard]] task<RunStateRecord> snapshot_record() {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();
        co_return to_record();
    }

private:
    struct Delivery {
        std::size_t           executor_index;
        agentengine::Message  payload;
    };

    // GitHub issue #52 fix, GENERALIZED by issue #62 (2026-09-03): a `fan_in` target with 2+
    // DISTINCT declared sources (edges of kind `fan_in` sharing the same `to`) is HELD here for the
    // target's whole join episode -- from before round 1 ever runs (seeded by `seed_fan_in_holds()`,
    // called once per `run_workflow()`) until every declared source has resolved -- instead of
    // dispatching as soon as ANY source's contribution arrives. 014 §2's "Concurrent = fan-out +
    // fan-in with an aggregator" is the only documented fan_in shape, and a fan_out's targets always
    // ALL fire (`edge_fires()` never conditions a `fan_out`/`fan_in` edge), so waiting for every
    // declared source is the correct join semantic, not an optional refinement.
    //
    // Originally (issue #52) this struct only ever held the ONE narrow shape a `fallback` edge's
    // named recovery rejoining the SAME target directly produced, registered freshly each round by a
    // since-removed `register_fan_in_holds()`. Issue #62 found -- via
    // tests/test_workflow_research_pipeline_large_context_live_e2e.cpp, a production-shaped pipeline
    // where two genuinely multi-round cyclic specialists (each ~11 real sequential turns) shared a
    // fan_in target with a fast-resolving third branch -- that this left the GENERAL case (two
    // ORDINARY, non-failing fan_in sources that simply resolve after different numbers of rounds)
    // completely unguarded: the target dispatched as soon as the fast source delivered, then AGAIN
    // once the slow source finally delivered, silently overwriting the first (pinned offline by
    // tests/test_workflow_fanin_uneven_round_sources_fix.cpp's U1, before this fix; now a positive
    // proof of the fix instead). Pre-registering EVERY multi-source fan_in target up front, once,
    // subsumes the narrow #52 shape too (a `fallback` edge's own `to` and the recovery's own `fan_in`
    // edge back to it are both ordinary declared sources under this generalization) -- so
    // `register_fan_in_holds()`'s per-round, direct-rejoin-only registration is gone; only
    // `route_from()`'s `propagate`/`fallback` failure handling still needs to know about a hold, to
    // resolve a permanently-failed source's OWN slot without ever receiving a delivery from it (see
    // `resolve_fan_in_await()`). Resolved/released by `deliver_to_fan_in()`, same as before.
    struct HeldFanIn {
        std::size_t               executor_index = 0;
        agentengine::Message      payload;
        bool                       seeded = false;
        std::vector<std::size_t>  awaiting_sources;
    };

    struct RunState {
        std::vector<Delivery>       pending;
        std::vector<ExecutorOutput> partial;
        agentengine::Message        selected_output;
        std::string                 failed_executor;
        std::vector<std::string>    unopened_ports;
        std::int64_t                elapsed_ns = 0;
        std::vector<HeldFanIn>      held_fan_in;
    };

    struct OpenPort {
        agentengine::Interaction interaction;
        std::size_t              executor_index = 0;
        agentengine::Message     response;
        std::vector<std::string> routes;
        bool                     resolved = false;
        // GitHub issue #35 follow-up: real usage a nested sub_workflow's own inner run incurred, set
        // ONLY when this OpenPort is the RESOLVED result of a nested `pending_sub_workflows_`
        // completion (resume_workflow()'s own branch) -- an ordinary request_port's own ask/answer
        // never involves a metered model call, so it stays at its honest default zero `Usage{}`. This
        // is what closes the residual `ExecuteReply::usage`'s own comment names: a nested sub_workflow
        // that suspends mid-run has its usage bubbled into the OUTER run's total RETROACTIVELY, at the
        // point this OpenPort is folded (the same port-prologue loop `execute()` already runs), not
        // lost.
        agentengine::Usage       usage{};
    };

    // ADR-157 (issues #33/#38) -- see docs/planning/sub-workflow-nested-request-port-design-draft.md
    // §3c. Deliberately NOT a variant/extension of OpenPort -- the design draft's own §2 finding 2
    // traces exactly why mixing "ordinary HITL answer, fold verbatim" and "nested sub-workflow
    // request, needs a synchronous resume call before anything is foldable" into one struct is a
    // real hazard, not a hypothetical one (`OpenPort::response` gets written unconditionally by
    // resume_workflow() before any marker could be checked). `inner_interaction_id` is NEVER
    // exposed outside WorkflowSupervisor -- the caller only ever sees `interaction`, the ordinary,
    // unmodified `Interaction` shape `mint_interaction()` already produces.
    struct PendingSubWorkflow {
        agentengine::Interaction interaction;
        std::size_t              executor_index = 0;
        std::string              inner_interaction_id;
    };

    enum class route_result { ok, routing_failed, workflow_failed };

    // One round's worth of concurrent fan-out. Wraps ONE synchronous ExecutorBody call -- never
    // suspends on anything else -- the safe case for ThreadPool::submit()'s own documented
    // constraint (see file banner). Takes everything BY VALUE so the coroutine frame owns independent
    // copies, not references into the caller's stack -- the same "coroutine frame owns its inputs"
    // discipline this project's own AsyncMutex test debugging (async_mutex.hpp's own history) already
    // established as load-bearing, not just tidy.
    // ADR-152 (issue #29): `sink`/`executor_id`/`round`/`attempt` wire EffectContext's two dedicated
    // multiplexed-event fields (`agent_turn_sink`/`moderator_delta_sink`, core/effect_context.hpp)
    // for the duration of THIS one call, before `body(payload, ctx)` runs -- `sink` is null unless
    // `enable_event_stream()` has been called (execute()'s own dispatch loop passes
    // `workflow_event_stream_enabled_ ? multiplex_sink_ : nullptr`), so a caller that never wires
    // the event stream pays nothing beyond one pointer check. `multiplex_sink<T>::push()` never
    // blocks (workflow/multiplex_sink.hpp) -- safe to call from this ThreadPool worker thread even
    // under a concurrently-dispatched fan-out round.
    // `path_prefix` (issue #42 item 3): this SUPERVISOR's own current `event_path_prefix_`, passed
    // by value like everything else here -- empty for an ordinary top-level dispatch, non-empty only
    // while THIS supervisor is itself being driven as a nested inner (see
    // ScopedForwardedEventSink below). A static function has no `this` to read it from directly, so
    // the caller (execute()'s dispatch loop) passes its own current value explicitly.
    static task<void> run_executor_job(
        ExecutorBody body, agentengine::Message payload, agentengine::EffectContext ctx,
        std::shared_ptr<ExecuteReply> out,
        std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>> sink,
        std::string executor_id, std::uint32_t round, std::uint32_t attempt,
        std::vector<std::string> path_prefix);

    // ADR-157 (issues #33/#38), driven by `block_on()` since decisions/ADR-175: this was a hand-rolled
    // `while (!t.done()) t.resume();` loop, and the argument below guarantees only that `inner`'s own
    // `run_mutex_` is uncontended -- a node inside `inner` can still park on something shared, which
    // the loop resumed a second time. The original argument, for `run_mutex_`: the identical reason
    // ADR-150 established for rt/workflow_as_executor.hpp's adapter, PLUS one
    // additional guarantee specific to nesting: `inner->run_workflow()`/`inner->resume_workflow()`
    // are only ever called from (a) inside THIS supervisor's own execute() round loop (via
    // run_sub_workflow_job, below) or (b) THIS supervisor's own resume_workflow()'s pending-sub-
    // workflow branch -- and BOTH (a) and (b) only ever run while THIS (the OUTER) supervisor's own
    // `run_mutex_` is held (I1) by run_workflow()/resume_workflow()/continue_workflow(), which
    // serializes them against each other. So at most ONE thread can ever be driving a GIVEN `inner`
    // at a time, structurally, regardless of how many worker threads this round's fan-out uses --
    // `inner`'s own `run_mutex_` is therefore never genuinely contended by two callers from this
    // supervisor. The OQ-19-style quarantine extension (below, in execute()) additionally prevents
    // two DIFFERENT deliveries in the SAME round from targeting the SAME `inner` via the same
    // executor_index. CALLER CONTRACT this does NOT enforce, matching this codebase's existing
    // trust-based lifetime contracts (e.g. workflow_as_executor_body()'s own reference-overload
    // documentation): a single `inner` instance must be bound via bind_sub_workflow() to AT MOST
    // ONE executor_index across the whole outer graph -- binding the same `inner` to two different
    // sub_workflow nodes is unsupported and not guarded against, and would defeat the quarantine's
    // own per-index dedup.
    template <class T>
    [[nodiscard]] static T drive(agentengine::rt::task<T> t) {
        return agentengine::rt::block_on(std::move(t));  // ADR-175: was `while (!t.done()) t.resume();`
    }

    // docs/planning/nested-workflow-event-forwarding-design-draft.md (issue #42 item 3, red-teamed):
    // wires `inner`'s multiplexed event sink + path prefix for EXACTLY the scope of one nested
    // `drive()` call, restoring `inner`'s own prior state on destruction. Computed fresh at
    // DISPATCH time by whichever caller is about to drive `inner` -- see event_path_prefix_'s own
    // comment for why this must never be precomputed at bind/enable time. `sink` may be null (the
    // forwarding supervisor's own stream isn't enabled); in that case this is a pure no-op --
    // `inner`'s own prior state (e.g. an independently-configured stream some other owner already
    // set up directly on `inner`, before or after it was bound here -- unsupported for the DURATION
    // of a forwarding dispatch, but otherwise untouched, see the design draft §5) is saved and
    // written back unchanged.
    class ScopedForwardedEventSink {
    public:
        ScopedForwardedEventSink(
            WorkflowSupervisor& inner,
            std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>>
                sink,
            std::vector<std::string> path_prefix)
            : inner_(inner), saved_sink_(inner.multiplex_sink_),
              saved_enabled_(inner.workflow_event_stream_enabled_),
              saved_prefix_(inner.event_path_prefix_) {
            if (sink) {
                inner_.multiplex_sink_             = std::move(sink);
                inner_.workflow_event_stream_enabled_ = true;
                inner_.event_path_prefix_          = std::move(path_prefix);
            }
        }
        ~ScopedForwardedEventSink() {
            inner_.multiplex_sink_             = std::move(saved_sink_);
            inner_.workflow_event_stream_enabled_ = saved_enabled_;
            inner_.event_path_prefix_          = std::move(saved_prefix_);
        }
        ScopedForwardedEventSink(ScopedForwardedEventSink const&)            = delete;
        ScopedForwardedEventSink& operator=(ScopedForwardedEventSink const&) = delete;

    private:
        WorkflowSupervisor& inner_;
        std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>>
            saved_sink_;
        bool                      saved_enabled_;
        std::vector<std::string> saved_prefix_;
    };

    // One dispatch attempt against a bound inner WorkflowSupervisor -- see the design draft
    // (docs/planning/sub-workflow-nested-request-port-design-draft.md) §3b. `out`'s
    // `pending_sub_workflow_inner_interaction_id` is set ONLY when the inner run suspended; every
    // other field is left default in that case (execute()'s own handling below never reads them).
    // `sink`/`path_prefix` (issue #42 item 3): forwarded into `inner` for exactly the scope of the
    // nested `drive()` call below via ScopedForwardedEventSink -- see that class and
    // event_path_prefix_'s own comments.
    static task<void> run_sub_workflow_job(
        std::shared_ptr<WorkflowSupervisor> inner, agentengine::Message payload,
        std::shared_ptr<ExecuteReply> out,
        std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>> sink,
        std::vector<std::string> path_prefix);

    task<WorkflowResult> execute();

    [[nodiscard]] WorkflowResult finish(workflow_status status,
                                        std::chrono::steady_clock::time_point entered_at);

    [[nodiscard]] agentengine::Interaction mint_interaction(std::size_t executor_index) const;

    // ADR-152 (issue #29): emits message_routed/fan_out_dispatched/route_selected as it decides
    // each edge, and appends to fan_in_edges_this_round_ for every firing fan_in edge -- the
    // caller (execute()) clears that member before its own routing loop and drains it via
    // push_fan_in_aggregated_events() after, so a fan_in target that receives contributions from
    // SEVERAL different route_from() calls within one round gets ONE aggregated event, not one per
    // contributing call. No longer `const` -- pushing a structural event mutates
    // workflow_event_producer_'s underlying channel state.
    // `is_quarantine_echo` (GitHub issue #52 fix, added alongside the propagate/fallback fix below):
    // true when THIS `!ok` reply is the OQ-19 same-round duplicate-delivery quarantine's synthetic
    // failure for an executor that ALSO has a real, successful (`ok`) delivery THIS round (see
    // is_same_round_quarantine_echo()). ADR-077 P9/T7 (test_rt_agent_workflow_executor.cpp) requires
    // that quarantine artifact to be "silently absorbed" -- it must inject NOTHING into any
    // downstream target, not even a marker `deliver_or_merge()` would harmlessly no-op against a
    // matching target, since a target the survivor's real routing never reached would otherwise get
    // a spurious failure marker despite the executor genuinely having succeeded once this round.
    // Default `false` for every other caller (the request-port prologue's replies are always `ok`,
    // so this branch never runs there).
    [[nodiscard]] route_result route_from(std::size_t from_index, ExecuteReply const& reply,
                                          std::vector<Delivery>& next, bool is_quarantine_echo = false);

    // Drains fan_in_edges_this_round_ (populated by route_from() above) and emits ONE
    // fan_in_aggregated event per distinct target, listing every contributing source -- see
    // route_from()'s own comment for why this aggregation cannot happen inside route_from() itself.
    void push_fan_in_aggregated_events();

    // GitHub issue #52 fix. See route_from()'s own comment on `is_quarantine_echo` for what this
    // detects and why: this exact `!ok` entry shares its executor_index with another entry THIS
    // round that succeeded (`ok`) -- the OQ-19 same-round duplicate-delivery quarantine's synthetic
    // failure, not a genuine independent failure.
    [[nodiscard]] static bool is_same_round_quarantine_echo(std::vector<Delivery> const& exec_deliveries,
                                                             std::vector<ExecuteReply> const& replies,
                                                             std::size_t i) noexcept;

    [[nodiscard]] static bool is_retryable(agentengine::failure_class klass) noexcept {
        return klass == agentengine::failure_class::transient ||
               klass == agentengine::failure_class::resource;
    }

    // GitHub issue #52 fix (renamed from `deliver_once`, which used to no-op on an existing entry --
    // see route_from()'s own comment on the `propagate`/`fallback` switch for the bug that caused).
    // Symmetric with the normal-path merge below it: append onto an existing `next` entry for
    // `target` if one already exists this round, or create one -- order-independent regardless of
    // whether the marker or a sibling's normal delivery reaches `target` first.
    static void deliver_or_merge(std::vector<Delivery>& next, std::size_t target,
                                 agentengine::Message const& marker);

    // GitHub issue #52 fix, generalized by issue #62. Delivers `payload` (routed by executor
    // `from_index`) to fan_in target `target`. If `target` is currently held back (an entry in
    // `state_.held_fan_in` -- seeded once, before round 1, by `seed_fan_in_holds()`, for every
    // fan_in target with 2+ declared sources; persists across as many rounds as it takes), the
    // content merges into the held accumulator instead of `next`; when `from_index` is one of that
    // hold's `awaiting_sources`, that source's slot resolves, and once every declared source has
    // resolved the accumulated content is released into `next` for dispatch, exactly like an
    // ordinary delivery. A target with no outstanding hold (a single-source fan_in target, or one
    // already released) behaves exactly like the pre-existing normal-path merge: append onto an
    // existing `next` entry, or create one.
    void deliver_to_fan_in(std::vector<Delivery>& next, std::size_t from_index, std::size_t target,
                           agentengine::Message const& payload);

    // Issue #62. Resolves `from_index`'s own slot in `target`'s fan_in barrier WITHOUT contributing
    // any payload -- used from route_from()'s `fallback` handling, when the failed source itself will
    // never independently deliver to `target` (a NAMED recovery executor, a separate declared source
    // when its own edge back to `target` is also `fan_in`, resolves its own slot later, normally, via
    // `deliver_to_fan_in()`). Mirrors that function's own release logic. A no-op if `target` has no
    // outstanding hold (a single-source fan_in target the `fallback` edge itself is the sole source
    // of -- nothing left to wait for once it fails and reroutes -- or one already released).
    void resolve_fan_in_await(std::vector<Delivery>& next, std::size_t target, std::size_t from_index);

    // Issue #62. Called ONCE, at the very start of a fresh `run_workflow()` (after `state_` is
    // reset), before round 1 ever runs -- NOT per round; see `HeldFanIn`'s own comment for why the
    // since-removed `register_fan_in_holds()`'s per-round re-registration is no longer needed once
    // every multi-source target is held from the very beginning of its whole join episode. For every
    // executor `T` with 2 or more DISTINCT declared `fan_in` sources (edges of kind `fan_in` whose
    // `to` names `T`), registers a `HeldFanIn` awaiting every one of them. A target with 0 or 1
    // sources is untouched -- a single fan_in source is an ordinary immediate-dispatch edge, no
    // barrier needed (matches this codebase's pre-#62 behavior exactly for that common case).
    void seed_fan_in_holds();

    void record_partial(std::vector<ExecutorOutput>& partial, std::size_t executor_index,
                        std::uint32_t round, agentengine::Message const& payload) const;

    // GitHub issue #35 follow-up -- the one place `total_usage_` is ever mutated. `agentengine::Usage`
    // (core/content.hpp) is a plain aggregate with no `operator+=` of its own, so this is
    // field-by-field, over every real field that struct declares.
    void accumulate_usage(agentengine::Usage const& delta) noexcept { add_usage(total_usage_, delta); }
    static void add_usage(agentengine::Usage& to, agentengine::Usage const& delta) noexcept;

    using EdgeFailurePolicy = agentengine::workflow::EdgeFailurePolicy;

    [[nodiscard]] EdgeFailurePolicy policy_for(std::size_t executor_index) const;

    [[nodiscard]] static bool edge_fires(agentengine::workflow::Edge const& edge,
                                        ExecuteReply const& reply) noexcept;

    [[nodiscard]] std::size_t index_of(std::string_view executor_id) const noexcept {
        for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
            if (graph_.executors[i].id == executor_id) return i;
        }
        return 0;
    }

    [[nodiscard]] bool is_output_selected(std::size_t executor_index) const noexcept {
        for (auto const& sel : graph_.output_selection) {
            if (sel == graph_.executors[executor_index].id) return true;
        }
        return false;
    }

    // OQ-19 design draft §5 item 4 -- the structural half of the check `check_workflow_executable()`
    // (workflow/graph.hpp) cannot perform itself, since that function sees only graph DATA, never
    // `bodies_`. Every `agent`-kind executor must be bound to a body `std::function`'s own type
    // erasure confirms was actually produced by `agent_session_as_executor_body()`, not merely a
    // plain closure that happens to satisfy `ExecutorBody`'s call signature.
    [[nodiscard]] bool agent_kind_bodies_are_structurally_agent_backed() const;

    // ADR-157 (issues #33/#38) -- the sub_workflow-kind sibling of the check above. `sub_workflows_`
    // is populated by `bind_sub_workflow()`, called AFTER `initialize()` -- so this check, run
    // DURING `initialize()`, only ever sees bindings from a PRIOR call (or none, for a fresh
    // instance). A caller must therefore call `initialize()`, then `bind_sub_workflow()` for every
    // sub_workflow-kind node, then `initialize()` AGAIN (re-validating with the now-populated
    // `sub_workflows_`) -- an unusual two-pass sequence, but the only one that keeps
    // `check_workflow_executable()` (workflow/graph.hpp) genuinely `rt::`-free while still letting
    // `WorkflowSupervisor` itself enforce the structural binding requirement, mirroring the
    // agent-kind check's own established shape exactly.
    [[nodiscard]] bool sub_workflow_kind_nodes_are_bound() const;

    // ---- ADR-169 (GitHub issue #65): the one admission predicate every entry point calls ------
    //
    // Placement at each call site is FIRST -- immediately after the I1 lock, before the `valid_`
    // check, before any `ports_`/`pending_sub_workflows_` lookup, before any structural event.
    // Deliberately dominating the `valid_` check rather than following it: an unadmitted caller must
    // not be able to push `workflow_run_failed` into the host's event stream (a denied caller with no
    // rate limit in front of it would otherwise have a free, unattributed write into an observability
    // surface), and must not learn whether this supervisor holds a runnable graph at all.
    //
    // Two modes, mirroring ADR-061 §20.4's own mode-branch discipline in the shape this narrower
    // surface actually needs. §20.4's rule is "no code path reads BOTH `caller` and `authority`";
    // there is only one identity field here, so what survives is the half that matters: a
    // `require_caller_` supervisor NEVER admits an identity-less request by falling through to the
    // permissive branch. Written as an explicit early return, not a widened `||`, because the widened
    // condition is precisely the shape §20.4 records as having broken once already.
    [[nodiscard]] bool admit_caller(std::optional<agentengine::Principal> const& caller) const {
        if (!caller.has_value()) return !require_caller_;
        return agentengine::principal_admitted_for(*caller, principal_);
    }

    // The one denial path. Returns a BARE result: `status` and nothing else. Specifically NOT
    // `open_interactions()` (which every other early return in `resume_workflow()` does populate) --
    // handing a caller the live interaction ids of a run it was just refused would turn the denial
    // into an id-enumeration oracle for the very authority the gate withholds. Nor `rounds`/`partial`/
    // `output`, which are run content for the same reason. Not `[[nodiscard]]`-free bookkeeping
    // either: `admission_denied_count_` is bumped and a structural event IS pushed, because I4 makes
    // a refused attempt an event the host must be able to see -- the asymmetry is deliberate, the
    // host learns everything, the denied caller learns nothing.
    [[nodiscard]] WorkflowResult deny_admission();

    // A bound sub-workflow that the host gave NO owner of its own inherits this one's -- recursively,
    // bounded by `kMaxNestingDepth`. Called from `set_principal()`/`set_require_caller()` AND from
    // `bind_sub_workflow()`, so the two possible orderings (configure-then-bind, bind-then-configure)
    // converge on the same state; neither is privileged and a host need not know which it used.
    //
    // Why this exists at all: `resume_workflow()` forwards the ORIGINAL caller into
    // `inner->resume_workflow()` rather than treating the nested hop as pre-admitted by construction
    // (issue #65's own item 5). That is the correct attribution -- the effect belongs to the real
    // caller, not to "the outer supervisor" -- but it means the inner gate now runs against the
    // inner's own owner, and an un-owned inner would deny every forwarded caller, breaking nested
    // HITL for hosts that reasonably configured only the root. Inheritance closes that without
    // weakening anything: the child's gate still genuinely runs, and a child the host DID give a
    // distinct owner keeps it and gates independently (ADR-169 §7 -- a deliberate, documented cost:
    // such a host must make its callers admissible at both levels).
    //
    // `inherited_admission_` is what makes this idempotent across SEVERAL configuration calls, and
    // its absence was a real bug caught by A11b before this shape existed: with only an "is the
    // child's principal empty?" test, `set_principal()` followed by `set_require_caller()` propagated
    // the first and then skipped the second -- the child, now non-empty, looked exactly like a child
    // the host had deliberately given its own owner. The flag distinguishes "owner arrived by
    // inheritance" (overwritable, keeps tracking the parent) from "owner set explicitly on this
    // object" (never overwritten), and either setter called directly on a child clears it, because an
    // explicit host choice outranks inheritance from that point on.
    void propagate_admission_to_children();

    agentengine::workflow::Workflow         graph_;
    std::vector<ExecutorBody>               bodies_;
    std::vector<agentengine::EffectContext> contexts_;
    // ADR-169 (issue #65) -- see set_principal()/set_require_caller()/admission_denied_count() for
    // the full contract. Host configuration, never reset by initialize() or restore_from_record().
    agentengine::Principal principal_{};
    bool                   require_caller_ = false;
    std::uint64_t          admission_denied_count_ = 0;
    // True only while `principal_`/`require_caller_` arrived from a parent's
    // propagate_admission_to_children() and were never set explicitly on THIS object. See that
    // function's own comment for the bug this exists to prevent.
    bool                   inherited_admission_ = false;
    bool           valid_       = false;
    // ADR-157 -- see initialize()'s own comment. Everything `valid_` depends on EXCEPT sub_workflow
    // binding; set once per initialize() call, read by bind_sub_workflow() to recompute `valid_`.
    bool           valid_base_  = false;
    std::uint32_t  rounds_      = 0;
    std::uint64_t  run_counter_ = 0;
    std::string    run_id_;
    // GitHub issue #35 follow-up -- see usage()'s own comment for the full contract. Same
    // reset-only-by-run_workflow() lifetime as rounds_ immediately above.
    agentengine::Usage total_usage_;
    // ADR-149 (issue #28 item 2). `designated_stall_reporter_` is host configuration set fresh at
    // `initialize()` -- like `bodies_`/`contexts_`, deliberately NOT part of `RunStateRecord`
    // (a resumed run's caller re-supplies it, same "caller supplies fresh at initialize()"
    // convention `agent_workflow_executor.hpp`'s own checkpoint/resume limitation already
    // established). `stall_streak_`/`resets_used_` ARE run-durable state and DO round-trip through
    // `to_record()`/`restore_from_record()` -- see `RunStateRecord`'s own fields.
    std::string    designated_stall_reporter_;
    std::uint32_t  stall_streak_ = 0;
    std::uint32_t  resets_used_  = 0;
    RunState       state_;
    std::vector<OpenPort> ports_;
    // ADR-157 (issues #33/#38) -- see docs/planning/sub-workflow-nested-request-port-design-draft.md.
    // `sub_workflows_`: bindings supplied by `bind_sub_workflow()`, keyed by executor_index -- like
    // `bodies_`/`contexts_`, deliberately NOT part of `RunStateRecord` (a resumed run's caller
    // re-supplies fresh bindings, same "caller supplies fresh at initialize()-time" convention
    // `agent_session_as_executor_body()`'s own checkpoint limitation already established).
    std::unordered_map<std::size_t, std::shared_ptr<WorkflowSupervisor>> sub_workflows_;
    // `pending_sub_workflows_`: keyed by the OUTER-visible interaction_id. NOT persisted (§4 of the
    // design draft) -- a restored/fresh instance always starts with this empty, which is exactly
    // what makes `resume_workflow()`'s fail-closed behavior against a stale interaction_id work
    // (falls through to "not found," `workflow_status::invalid`, never silent misrouting).
    std::unordered_map<std::string, PendingSubWorkflow> pending_sub_workflows_;
    // docs/planning/nested-workflow-threadpool-budget-design-draft.md (issue #42 item 2): tracked
    // automatically through `bind_sub_workflow()`, never caller-set directly -- 0 for a
    // standalone/root instance; `bind_sub_workflow()` sets an `inner`'s own `nesting_depth_` to
    // `this->nesting_depth_ + 1` and REFUSES the bind (mirroring every other refusal shape that
    // function already has -- the node stays unbound, `sub_workflow_kind_nodes_are_bound()` is what
    // surfaces it, not the bind call itself) if that would exceed `kMaxNestingDepth`. This is a
    // LOAD-BEARING precondition for `split_worker_budget()`'s own arithmetic to be a real ceiling,
    // not merely "recommended defense in depth" -- without a depth cap, a subtree's worst-case
    // thread count is unbounded once fan-out is unbounded, regardless of how carefully any single
    // level's own budget is computed (red-team finding, same design draft).
    std::size_t nesting_depth_ = 0;
    // A generous but real ceiling -- S11 (tests/test_rt_workflow_sub_workflow.cpp) proves 3 levels
    // genuinely work; this is not tuned to that number, just chosen as a small, structural,
    // CLAUDE.md-compliant bound rather than left unbounded. A host that legitimately needs deeper
    // nesting is free to raise this constant, but the DEFAULT must not be "unbounded."
    static constexpr std::size_t kMaxNestingDepth = 16;
    // Sized 0 (system-determined default) -- a round's own fan-out width varies by graph, so a fixed
    // worker count chosen here would either under-parallelize a wide round or waste threads on a
    // narrow one; ThreadPool's own default already picks a reasonable system-wide figure.
    ThreadPool     pool_;
    // I1 -- see file banner. Every public async entry point acquires this for its whole duration.
    AsyncMutex     run_mutex_;
    // 014 §7 live view -- invalid until enable_live_view() is called, matching the original's own
    // "Phase G: invalid until enable_live_view()" comment verbatim.
    agentengine::stream_producer<agentengine::workflow::WorkflowLiveEvent> live_view_producer_;
    // 014 §5 checkpoint hook -- see file banner's "Checkpoint hook" paragraph. nullptr by default,
    // matching the original's own "Phase F: nullptr by default" comment.
    CheckpointHook checkpoint_hook_;
    // ADR-055 follow-up -- see file banner's "Merge-on-join hook" paragraph. nullptr by default: a
    // supervisor with no hook set behaves EXACTLY as before this change (every `branch`-mode executor
    // simply never gets an on-completion callback, matching this codebase's own "additive, existing
    // callers unaffected" convention for every optional hook in this class).
    MergeOnJoinHook merge_on_join_hook_;

    // ADR-152 (issue #29) -- see enable_event_stream()'s own comment above.
    //
    // Structural bucket: same single-writer channel shape live_view_producer_ already uses --
    // invalid until enable_event_stream() is called.
    agentengine::stream_producer<agentengine::workflow::WorkflowEvent> workflow_event_producer_;
    // Multiplexed bucket: never null (constructed once, up front) so run_executor_job's per-
    // delivery wiring never needs a null check on THIS -- workflow_event_stream_enabled_ below is
    // the actual gate on whether it's ever handed to a delivery at all. shared_ptr (not a plain
    // member) because it is captured, by value, into closures that outlive this round's stack
    // frame (WorkflowEventStream also holds a copy, handed out by enable_event_stream()).
    std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>>
        multiplex_sink_ =
            std::make_shared<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>>();
    // Zero-cost-when-unattached gate (matching live_view_producer_.valid()'s own role for the
    // structural bucket): false until enable_event_stream() is called, at which point every
    // subsequent dispatch wires the multiplexed bridge; never reset back to false afterward
    // (matches enable_live_view()'s own "once attached, stays attached for this instance's
    // lifetime unless replaced" shape -- there is no disable_event_stream()).
    bool workflow_event_stream_enabled_ = false;
    // docs/planning/nested-workflow-event-forwarding-design-draft.md (issue #42 item 3): this
    // supervisor's own accumulated lineage of sub_workflow executor_ids, from the OUTERMOST
    // supervisor currently forwarding into it down to (but not including) this instance's own
    // graph. Empty for a standalone/root instance FOREVER -- unlike `nesting_depth_`, this is
    // NEVER set at bind_sub_workflow() time. It is wired transiently, immediately before this
    // instance is driven as a nested inner, and restored immediately after, by
    // ScopedForwardedEventSink below -- computed fresh at DISPATCH time on the calling thread, not
    // precomputed at bind/enable time. Two real defects a bind-time/enable-time design would have
    // had, closed by construction:
    //   - WRONG VALUES under bottom-up construction order (build innermost first, bind upward --
    //     exactly how every real nested tree in this codebase, including S8/S11, is built): a
    //     value baked in at bind_sub_workflow() time can only reflect what the OUTER's own prefix
    //     was AT THAT MOMENT, which is wrong if more binding happens above it afterward.
    //   - A genuine cross-thread data race: `enable_event_stream()`'s own doc comment already
    //     blesses calling it more than once, and nothing prevents `bind_sub_workflow()`/
    //     `enable_event_stream()` from running on one thread while a PREVIOUS run against the same
    //     `inner` is still mid-flight on another -- a bind-time/enable-time cascade would plainly
    //     write into a live object's members while its own dispatch loop concurrently reads them.
    // Dispatch-time wiring has neither problem: the write always immediately precedes, on the SAME
    // thread, the one `drive()` call whose own worker threads are the only readers, and those
    // workers are always spawned AND joined strictly inside that one call's own synchronous
    // extent -- never outside the window ScopedForwardedEventSink's own lifetime covers.
    std::vector<std::string> event_path_prefix_;
    // docs/planning/workflow-mid-run-cancellation-design-draft.md (GitHub issue #37, red-teamed):
    // ADR-017's own stop_source/stop_token precedent (core/stream.hpp), reused here rather than a
    // bespoke CancellationToken type. Always present (default-constructed, never requested) --
    // cancel()/cancellation_token() need no null check, matching multiplex_sink_'s own "always
    // valid" shape. Never touches state_/ports_/rounds_ or anything else run_mutex_ (I1) protects;
    // request_stop()/stop_requested() are specified thread-safe with no external synchronization,
    // so this is genuinely callable from a different thread than whichever one is driving execute()
    // right now, unlike every other member near it. Not part of RunStateRecord -- like
    // nesting_depth_/event_path_prefix_, a restored instance's cancel_source_ is always
    // unrequested; a caller wanting a restored run to stay cancelled must call cancel() again after
    // restore.
    std::stop_source cancel_source_;
    // Scratch state for push_fan_in_aggregated_events() -- see route_from()'s own comment. Cleared
    // by execute() before each routing loop that calls route_from(), drained (and re-cleared) by
    // push_fan_in_aggregated_events() right after. Never holds state across a co_await suspension
    // point (both routing loops are fully synchronous), so this being a plain member rather than a
    // loop-local is a convenience only, not a concurrency concern.
    std::vector<std::pair<std::size_t, std::string>> fan_in_edges_this_round_;

    void push_structural_event(agentengine::workflow::workflow_event_kind kind,
                                agentengine::workflow::WorkflowEventPayload payload =
                                    agentengine::workflow::workflow_event_payload::Empty{});
};

// Save `supervisor`'s current run under its own run_id() -- see file banner and snapshot_record()'s
// own comment for the in-flight guard this relies on. `supervisor` is non-const (not const&) because
// acquiring run_mutex_ mutates the mutex's own state even though this is logically a read.
// NOT the original's two-phase pending->committed discipline, and NOT retained across calls -- see
// file banner: a single rt::SessionStore save call replaces both phases, and only the LATEST
// checkpoint survives a second call.
template <SessionStore StoreT>
[[nodiscard]] task<result<void>> save_workflow_checkpoint(WorkflowSupervisor& supervisor, StoreT& store) {
    RunStateRecord rec = co_await supervisor.snapshot_record();
    co_return store.save(rec.run_id, encode_run_state_record(rec));
}

// Load the latest durable checkpoint for `run_id`, or std::nullopt if none was ever saved. Synchronous
// (no task<T>) -- like rt::AgentSession's own load_agent_session_snapshot(), this never touches a live
// WorkflowSupervisor instance, so there is no in-flight state to guard against.
template <SessionStore StoreT>
[[nodiscard]] result<std::optional<RunStateRecord>> load_workflow_checkpoint(
    StoreT const& store, std::string const& run_id) {
    if (!store.exists(run_id)) return std::optional<RunStateRecord>{};
    result<std::vector<std::byte>> bytes = store.load(run_id);
    if (!bytes) return std::unexpected(bytes.error());
    result<RunStateRecord> rec = decode_run_state_record(*bytes);
    if (!rec) return std::unexpected(rec.error());
    return std::optional<RunStateRecord>{std::move(*rec)};
}

}  // namespace agentengine::rt
