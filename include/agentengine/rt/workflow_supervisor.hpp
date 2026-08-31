#pragma once
// ADR-037 Phase 3, Slice 1: `agentengine::rt::WorkflowSupervisor`, the Quark-actor-free replacement
// for `agentengine::workflow::WorkflowSupervisor` (workflow/supervisor.hpp)'s core superstep loop.
// Lives under `agentengine::rt`, a NEW namespace, deliberately NOT wired into any live call site yet
// -- nothing in the current Quark-based build is touched by this file existing.
//
// SCOPE OF SLICE 1 (matching the discipline `rt::AgentSession`'s own Slice 1 established): the core
// superstep loop -- `run_workflow()`/`resume_workflow()`/`continue_workflow()`'s full `execute()`
// and every pure routing helper (`route_from`/`policy_for`/`edge_fires`/`deliver_once`/
// `record_partial`/`index_of`/`is_output_selected`/`is_retryable`/`mint_interaction`/`finish` -- all
// already zero-Quark-dependency in the original, ported near-verbatim).
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

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

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
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/live_view.hpp"

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
};

struct RunWorkflow {
    agentengine::Message input;
};

// ae-naming-lint: allow ContinueWorkflow — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContinueWorkflow {};

// ae-naming-lint: allow ResumeWorkflow — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ResumeWorkflow {
    std::string           interaction_id;
    agentengine::Message  response;
    std::vector<std::string> routes;
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

// ae-naming-lint: allow DeliveryRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct DeliveryRecord {
    std::uint64_t         executor_index = 0;
    agentengine::Message  payload;
};

// ae-naming-lint: allow ExecutorOutputRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ExecutorOutputRecord {
    std::string           executor_id;
    std::uint32_t         round = 0;
    agentengine::Message  payload;
};

// ae-naming-lint: allow OpenPortRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct OpenPortRecord {
    agentengine::Interaction interaction;
    std::uint64_t             executor_index = 0;
    agentengine::Message      response;
    std::vector<std::string>  routes;
    bool                       resolved = false;
};

// ae-naming-lint: allow RunStateRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RunStateRecord {
    std::uint64_t  run_counter = 0;
    std::string    run_id;
    std::uint32_t  rounds = 0;
    std::vector<DeliveryRecord>       pending;
    std::vector<ExecutorOutputRecord> partial;
    agentengine::Message  selected_output;
    std::string           failed_executor;
    std::vector<std::string> unopened_ports;
    std::int64_t              elapsed_ns = 0;
    std::vector<OpenPortRecord> ports;
    // ADR-149 (issue #28 item 2/5): must round-trip through checkpoint/resume, or a host that
    // checkpoints a Magentic run at all (the normal, encouraged persistence pattern) would silently
    // reset stall bookkeeping to zero on every resume -- an unlimited-stall-budget bypass of the
    // exact safety valve this field exists to provide. Optional on read (default 0) so a checkpoint
    // taken before ADR-149 still decodes.
    std::uint32_t stall_streak = 0;
    std::uint32_t resets_used  = 0;
};

// interaction_to_json()/interaction_from_json() live in interaction_codec.hpp -- shared with
// rt::AgentSession's own record codec (see that header's own banner for why this used to be a
// duplicated copy here and isn't anymore -- the two copies were byte-identical apart from their
// error code's own dotted suffix, now unified to "rt.interaction.record.malformed").

[[nodiscard]] inline agentengine::json::Value delivery_record_to_json(DeliveryRecord const& d) {
    return agentengine::json::Value::make_object({
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(d.executor_index))},
        {"payload", message_to_json(d.payload)},
    });
}
[[nodiscard]] inline agentengine::result<DeliveryRecord> delivery_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* payload         = v.find("payload");
    if (executor_index == nullptr || !executor_index->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed DeliveryRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    DeliveryRecord d;
    d.executor_index = static_cast<std::uint64_t>(executor_index->as_number());
    d.payload         = std::move(*msg);
    return d;
}

[[nodiscard]] inline agentengine::json::Value executor_output_record_to_json(ExecutorOutputRecord const& o) {
    return agentengine::json::Value::make_object({
        {"executor_id", agentengine::json::Value::make_string(o.executor_id)},
        {"round", agentengine::json::Value::make_number(static_cast<double>(o.round))},
        {"payload", message_to_json(o.payload)},
    });
}
[[nodiscard]] inline agentengine::result<ExecutorOutputRecord> executor_output_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_id = v.find("executor_id");
    agentengine::json::Value const* round        = v.find("round");
    agentengine::json::Value const* payload      = v.find("payload");
    if (executor_id == nullptr || !executor_id->is_string() || round == nullptr ||
        !round->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed ExecutorOutputRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    ExecutorOutputRecord o;
    o.executor_id = executor_id->as_string();
    o.round       = static_cast<std::uint32_t>(round->as_number());
    o.payload     = std::move(*msg);
    return o;
}

[[nodiscard]] inline agentengine::json::Value open_port_record_to_json(OpenPortRecord const& p) {
    std::vector<agentengine::json::Value> routes;
    routes.reserve(p.routes.size());
    for (std::string const& r : p.routes) routes.push_back(agentengine::json::Value::make_string(r));
    return agentengine::json::Value::make_object({
        {"interaction", interaction_to_json(p.interaction)},
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(p.executor_index))},
        {"response", message_to_json(p.response)},
        {"routes", agentengine::json::Value::make_array(std::move(routes))},
        {"resolved", agentengine::json::Value::make_bool(p.resolved)},
    });
}
[[nodiscard]] inline agentengine::result<OpenPortRecord> open_port_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* interaction    = v.find("interaction");
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* response        = v.find("response");
    agentengine::json::Value const* routes          = v.find("routes");
    agentengine::json::Value const* resolved        = v.find("resolved");
    if (interaction == nullptr || executor_index == nullptr || !executor_index->is_number() ||
        response == nullptr || routes == nullptr || !routes->is_array() || resolved == nullptr ||
        !resolved->is_bool()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed OpenPortRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Interaction> ia = interaction_from_json(*interaction);
    if (!ia) return std::unexpected(ia.error());
    agentengine::result<agentengine::Message> resp = message_from_json(*response);
    if (!resp) return std::unexpected(resp.error());
    OpenPortRecord p;
    p.interaction     = std::move(*ia);
    p.executor_index  = static_cast<std::uint64_t>(executor_index->as_number());
    p.response        = std::move(*resp);
    p.routes.reserve(routes->as_array().size());
    for (agentengine::json::Value const& r : routes->as_array()) {
        if (!r.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed OpenPortRecord.routes entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        p.routes.push_back(r.as_string());
    }
    p.resolved = resolved->as_bool();
    return p;
}

[[nodiscard]] inline agentengine::json::Value run_state_record_to_json(RunStateRecord const& rec) {
    std::vector<agentengine::json::Value> pending;
    pending.reserve(rec.pending.size());
    for (auto const& d : rec.pending) pending.push_back(delivery_record_to_json(d));

    std::vector<agentengine::json::Value> partial;
    partial.reserve(rec.partial.size());
    for (auto const& o : rec.partial) partial.push_back(executor_output_record_to_json(o));

    std::vector<agentengine::json::Value> unopened_ports;
    unopened_ports.reserve(rec.unopened_ports.size());
    for (std::string const& id : rec.unopened_ports) {
        unopened_ports.push_back(agentengine::json::Value::make_string(id));
    }

    std::vector<agentengine::json::Value> ports;
    ports.reserve(rec.ports.size());
    for (auto const& p : rec.ports) ports.push_back(open_port_record_to_json(p));

    return agentengine::json::Value::make_object({
        {"run_counter", agentengine::json::Value::make_number(static_cast<double>(rec.run_counter))},
        {"run_id", agentengine::json::Value::make_string(rec.run_id)},
        {"rounds", agentengine::json::Value::make_number(static_cast<double>(rec.rounds))},
        {"pending", agentengine::json::Value::make_array(std::move(pending))},
        {"partial", agentengine::json::Value::make_array(std::move(partial))},
        {"selected_output", message_to_json(rec.selected_output)},
        {"failed_executor", agentengine::json::Value::make_string(rec.failed_executor)},
        {"unopened_ports", agentengine::json::Value::make_array(std::move(unopened_ports))},
        {"elapsed_ns", agentengine::json::Value::make_number(static_cast<double>(rec.elapsed_ns))},
        {"ports", agentengine::json::Value::make_array(std::move(ports))},
        {"stall_streak", agentengine::json::Value::make_number(static_cast<double>(rec.stall_streak))},
        {"resets_used", agentengine::json::Value::make_number(static_cast<double>(rec.resets_used))},
    });
}

[[nodiscard]] inline agentengine::result<RunStateRecord> run_state_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* run_counter      = v.find("run_counter");
    agentengine::json::Value const* run_id           = v.find("run_id");
    agentengine::json::Value const* rounds           = v.find("rounds");
    agentengine::json::Value const* pending          = v.find("pending");
    agentengine::json::Value const* partial          = v.find("partial");
    agentengine::json::Value const* selected_output  = v.find("selected_output");
    agentengine::json::Value const* failed_executor  = v.find("failed_executor");
    agentengine::json::Value const* unopened_ports   = v.find("unopened_ports");
    agentengine::json::Value const* elapsed_ns       = v.find("elapsed_ns");
    agentengine::json::Value const* ports            = v.find("ports");
    if (run_counter == nullptr || !run_counter->is_number() || run_id == nullptr ||
        !run_id->is_string() || rounds == nullptr || !rounds->is_number() || pending == nullptr ||
        !pending->is_array() || partial == nullptr || !partial->is_array() ||
        selected_output == nullptr || failed_executor == nullptr || !failed_executor->is_string() ||
        unopened_ports == nullptr || !unopened_ports->is_array() || elapsed_ns == nullptr ||
        !elapsed_ns->is_number() || ports == nullptr || !ports->is_array()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed RunStateRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    RunStateRecord rec;
    rec.run_counter = static_cast<std::uint64_t>(run_counter->as_number());
    rec.run_id      = run_id->as_string();
    rec.rounds      = static_cast<std::uint32_t>(rounds->as_number());
    rec.pending.reserve(pending->as_array().size());
    for (agentengine::json::Value const& item : pending->as_array()) {
        auto d = delivery_record_from_json(item);
        if (!d) return std::unexpected(d.error());
        rec.pending.push_back(std::move(*d));
    }
    rec.partial.reserve(partial->as_array().size());
    for (agentengine::json::Value const& item : partial->as_array()) {
        auto o = executor_output_record_from_json(item);
        if (!o) return std::unexpected(o.error());
        rec.partial.push_back(std::move(*o));
    }
    agentengine::result<agentengine::Message> sel = message_from_json(*selected_output);
    if (!sel) return std::unexpected(sel.error());
    rec.selected_output = std::move(*sel);
    rec.failed_executor  = failed_executor->as_string();
    rec.unopened_ports.reserve(unopened_ports->as_array().size());
    for (agentengine::json::Value const& item : unopened_ports->as_array()) {
        if (!item.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed RunStateRecord.unopened_ports entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        rec.unopened_ports.push_back(item.as_string());
    }
    rec.elapsed_ns = static_cast<std::int64_t>(elapsed_ns->as_number());
    rec.ports.reserve(ports->as_array().size());
    for (agentengine::json::Value const& item : ports->as_array()) {
        auto p = open_port_record_from_json(item);
        if (!p) return std::unexpected(p.error());
        rec.ports.push_back(std::move(*p));
    }
    // ADR-149: optional on read -- a pre-ADR-149 checkpoint has neither field, and 0 is the correct
    // "no stall bookkeeping yet" value for one, matching this record's other additive-field precedent.
    if (agentengine::json::Value const* stall_streak = v.find("stall_streak");
        stall_streak != nullptr && stall_streak->is_number()) {
        rec.stall_streak = static_cast<std::uint32_t>(stall_streak->as_number());
    }
    if (agentengine::json::Value const* resets_used = v.find("resets_used");
        resets_used != nullptr && resets_used->is_number()) {
        rec.resets_used = static_cast<std::uint32_t>(resets_used->as_number());
    }
    return rec;
}

[[nodiscard]] inline std::vector<std::byte> encode_run_state_record(RunStateRecord const& rec) {
    std::string const text = agentengine::json::dump(run_state_record_to_json(rec));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}
[[nodiscard]] inline agentengine::result<RunStateRecord> decode_run_state_record(
    std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
    agentengine::result<agentengine::json::Value> parsed = agentengine::json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return run_state_record_from_json(*parsed);
}

// ae-naming-lint: allow WorkflowSupervisor — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class WorkflowSupervisor {
public:
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
                     std::string designated_stall_reporter = {}) {
        graph_    = std::move(graph);
        bodies_   = std::move(bodies);
        contexts_ = std::move(contexts);
        contexts_.resize(graph_.executors.size());
        designated_stall_reporter_ = std::move(designated_stall_reporter);
        stall_streak_ = 0;
        resets_used_  = 0;
        valid_ = agentengine::workflow::validate_workflow(graph_).has_value() &&
                 bodies_.size() == graph_.executors.size() &&
                 agentengine::workflow::check_workflow_executable(graph_, contexts_).has_value() &&
                 agent_kind_bodies_are_structurally_agent_backed();
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

    [[nodiscard]] std::vector<agentengine::Interaction> open_interactions() const {
        std::vector<agentengine::Interaction> out;
        for (auto const& p : ports_) {
            if (!p.resolved) out.push_back(p.interaction);
        }
        return out;
    }

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

    task<WorkflowResult> run_workflow(RunWorkflow request) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};

        ++run_counter_;
        run_id_ = graph_.id + ":run:" + std::to_string(run_counter_);
        state_  = RunState{};
        ports_.clear();
        rounds_ = 0;
        state_.pending.push_back(Delivery{index_of(graph_.start), request.input});

        co_return co_await execute();
    }

    task<WorkflowResult> resume_workflow(ResumeWorkflow request) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};

        OpenPort* port = nullptr;
        for (auto& p : ports_) {
            if (p.interaction.interaction_id == request.interaction_id) { port = &p; break; }
        }
        if (port == nullptr || port->resolved) {
            WorkflowResult r{workflow_status::invalid};
            r.rounds            = rounds_;
            r.open_interactions = open_interactions();
            co_return r;
        }

        port->resolved = true;
        port->response = request.response;
        port->routes   = request.routes;

        for (auto const& p : ports_) {
            if (p.resolved) continue;
            WorkflowResult r{workflow_status::suspended};
            r.rounds            = rounds_;
            r.partial           = state_.partial;
            r.output            = state_.selected_output;
            r.open_interactions = open_interactions();
            co_return r;
        }

        co_return co_await execute();
    }

    task<WorkflowResult> continue_workflow(ContinueWorkflow) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};
        co_return co_await execute();
    }

    // ---- Slice 2: checkpointing (file banner has the design writeup) --------------------------

    // Unlocked, synchronous -- matches rt::AgentSession::to_record()'s own shape. Not the caller's
    // normal way to take a checkpoint; see snapshot_record() below for the locked path.
    [[nodiscard]] RunStateRecord to_record() const {
        RunStateRecord rec;
        rec.run_counter = run_counter_;
        rec.run_id      = run_id_;
        rec.rounds      = rounds_;
        rec.pending.reserve(state_.pending.size());
        for (auto const& d : state_.pending) {
            rec.pending.push_back(DeliveryRecord{static_cast<std::uint64_t>(d.executor_index), d.payload});
        }
        rec.partial.reserve(state_.partial.size());
        for (auto const& p : state_.partial) {
            rec.partial.push_back(ExecutorOutputRecord{p.executor_id, p.round, p.payload});
        }
        rec.selected_output = state_.selected_output;
        rec.failed_executor  = state_.failed_executor;
        rec.unopened_ports   = state_.unopened_ports;
        rec.elapsed_ns       = state_.elapsed_ns;
        rec.ports.reserve(ports_.size());
        for (auto const& p : ports_) {
            rec.ports.push_back(OpenPortRecord{p.interaction, static_cast<std::uint64_t>(p.executor_index),
                                               p.response, p.routes, p.resolved});
        }
        rec.stall_streak = stall_streak_;
        rec.resets_used  = resets_used_;
        return rec;
    }

    void restore_from_record(RunStateRecord const& rec) {
        run_counter_ = rec.run_counter;
        run_id_      = rec.run_id;
        rounds_      = rec.rounds;
        state_       = RunState{};
        state_.pending.reserve(rec.pending.size());
        for (auto const& d : rec.pending) {
            state_.pending.push_back(Delivery{static_cast<std::size_t>(d.executor_index), d.payload});
        }
        state_.partial.reserve(rec.partial.size());
        for (auto const& p : rec.partial) {
            state_.partial.push_back(ExecutorOutput{p.executor_id, p.round, p.payload});
        }
        state_.selected_output = rec.selected_output;
        state_.failed_executor = rec.failed_executor;
        state_.unopened_ports  = rec.unopened_ports;
        state_.elapsed_ns      = rec.elapsed_ns;
        ports_.clear();
        ports_.reserve(rec.ports.size());
        for (auto const& p : rec.ports) {
            ports_.push_back(OpenPort{p.interaction, static_cast<std::size_t>(p.executor_index),
                                      p.response, p.routes, p.resolved});
        }
        stall_streak_ = rec.stall_streak;
        resets_used_  = rec.resets_used;
    }

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

    struct RunState {
        std::vector<Delivery>       pending;
        std::vector<ExecutorOutput> partial;
        agentengine::Message        selected_output;
        std::string                 failed_executor;
        std::vector<std::string>    unopened_ports;
        std::int64_t                elapsed_ns = 0;
    };

    struct OpenPort {
        agentengine::Interaction interaction;
        std::size_t              executor_index = 0;
        agentengine::Message     response;
        std::vector<std::string> routes;
        bool                     resolved = false;
    };

    enum class route_result { ok, routing_failed, workflow_failed };

    // One round's worth of concurrent fan-out. Wraps ONE synchronous ExecutorBody call -- never
    // suspends on anything else -- the safe case for ThreadPool::submit()'s own documented
    // constraint (see file banner). Takes everything BY VALUE so the coroutine frame owns independent
    // copies, not references into the caller's stack -- the same "coroutine frame owns its inputs"
    // discipline this project's own AsyncMutex test debugging (async_mutex.hpp's own history) already
    // established as load-bearing, not just tidy.
    static task<void> run_executor_job(ExecutorBody body, agentengine::Message payload,
                                        agentengine::EffectContext ctx,
                                        std::shared_ptr<ExecuteReply> out) {
        if (!body) {
            *out = ExecuteReply{agentengine::Message{}, {}, false, agentengine::failure_class::contract};
            co_return;
        }
        agentengine::result<ExecutorOutcome> outcome = body(payload, ctx);
        if (!outcome) {
            *out = ExecuteReply{agentengine::Message{}, {}, false, outcome.error().klass};
            co_return;
        }
        *out = ExecuteReply{std::move(outcome->payload), std::move(outcome->routes), true,
                             agentengine::failure_class::fatal, outcome->stalled};
        co_return;
    }

    task<WorkflowResult> execute() {
        auto const entered_at = std::chrono::steady_clock::now();
        workflow_status status = workflow_status::completed;

        if (!ports_.empty()) {
            std::vector<Delivery> next = state_.pending;
            for (auto const& p : ports_) {
                ExecuteReply const reply{p.response, p.routes, true, agentengine::failure_class::fatal};
                record_partial(state_.partial, p.executor_index, rounds_ - 1, p.response);
                if (is_output_selected(p.executor_index)) state_.selected_output = p.response;
                route_result const rr = route_from(p.executor_index, reply, next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[p.executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                              : workflow_status::executor_failed;
                ports_.clear();
                co_return finish(status, entered_at);
            }
            ports_.clear();
            state_.pending = std::move(next);
        }

        while (!state_.pending.empty()) {
            if (graph_.bound.max_rounds.has_value() && rounds_ >= *graph_.bound.max_rounds) {
                status = workflow_status::bound_max_rounds;
                break;
            }
            if (graph_.bound.deadline_ms.has_value()) {
                auto const elapsed = std::chrono::nanoseconds{state_.elapsed_ns} +
                                     (std::chrono::steady_clock::now() - entered_at);
                auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (static_cast<std::uint64_t>(ms) >= *graph_.bound.deadline_ms) {
                    status = workflow_status::bound_deadline;
                    break;
                }
            }

            std::vector<Delivery> exec_deliveries;
            std::vector<Delivery> port_deliveries;
            for (auto& d : state_.pending) {
                if (graph_.executors[d.executor_index].kind ==
                    agentengine::workflow::executor_kind::request_port) {
                    port_deliveries.push_back(std::move(d));
                } else {
                    exec_deliveries.push_back(std::move(d));
                }
            }

            // OQ-19 design draft §5 item 2: two ordinary (non-fan_in) edges converging on the SAME
            // agent-kind node in one round would otherwise submit two concurrent `Delivery` entries
            // for the same `AgentSession` -- `AgentSession::start_run()`'s `session_mutex_.lock()`
            // genuinely parks a contended waiter and resumes it from a DIFFERENT thread, which the
            // naive "resume until done" drive loop `rt::agent_session_as_executor_body()` uses
            // (rt/agent_workflow_executor.hpp) cannot survive. Detected HERE, at gather time, before
            // any `pool_.submit()` -- `exec_deliveries` is built once, fully, above, so this cannot be
            // evaded across the retry-attempt loop below. Only the SECOND-and-later delivery to a
            // given agent-kind `executor_index` this round is quarantined (synthetically failed,
            // `contract`-class -- never retried, since `is_retryable()` excludes that class -- and
            // never dispatched at all); the FIRST still runs normally, and every OTHER unrelated
            // delivery this round is completely unaffected. This reuses the EXISTING failure-policy/
            // retry/fallback machinery (a quarantined entry is simply `!ok`, routed exactly like any
            // other real executor failure per that node's own declared edge policy) rather than
            // aborting the whole round -- an earlier "abort the round" resolution was rejected as
            // strictly harsher than the existing `broke` failure path (see the design draft).
            std::vector<bool> quarantined(exec_deliveries.size(), false);
            {
                std::vector<std::size_t> seen_agent_indices;
                for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                    std::size_t const idx = exec_deliveries[i].executor_index;
                    if (graph_.executors[idx].kind != agentengine::workflow::executor_kind::agent) {
                        continue;
                    }
                    bool seen = false;
                    for (std::size_t const s : seen_agent_indices) {
                        if (s == idx) { seen = true; break; }
                    }
                    if (seen) {
                        quarantined[i] = true;
                        continue;
                    }
                    seen_agent_indices.push_back(idx);
                }
            }

            std::vector<ExecuteReply> replies(exec_deliveries.size());
            std::vector<std::size_t>  todo;
            todo.reserve(exec_deliveries.size());
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                if (quarantined[i]) {
                    replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                               agentengine::failure_class::contract};
                } else {
                    todo.push_back(i);
                }
            }

            for (std::uint32_t attempt = 0; !todo.empty(); ++attempt) {
                // ---- decision 5, first half: ISSUE every job before awaiting any ----------------
                std::vector<std::future<JobOutcome>>        in_flight;
                std::vector<std::shared_ptr<ExecuteReply>>  slots;
                in_flight.reserve(todo.size());
                slots.reserve(todo.size());
                for (std::size_t const i : todo) {
                    std::size_t const idx = exec_deliveries[i].executor_index;
                    auto slot = std::make_shared<ExecuteReply>();
                    slots.push_back(slot);
                    in_flight.push_back(pool_.submit(
                        run_executor_job(bodies_[idx], exec_deliveries[i].payload, contexts_[idx], slot)));
                }

                // ---- decision 5, second half: COLLECT in fixed index order ---------------------
                std::vector<std::size_t> retry_next;
                for (std::size_t k = 0; k < in_flight.size(); ++k) {
                    std::size_t const i       = todo[k];
                    JobOutcome         outcome = in_flight[k].get();
                    if (outcome.faulted) {
                        // A throwing executor body -- see file banner for why this is classified
                        // transient rather than needing a restart-budget mechanism of its own.
                        replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                                   agentengine::failure_class::transient};
                    } else {
                        replies[i] = std::move(*slots[k]);
                    }
                    if (replies[i].ok) continue;

                    EdgeFailurePolicy const pol = policy_for(exec_deliveries[i].executor_index);
                    if (pol.kind == agentengine::workflow::edge_failure_policy::retry &&
                        attempt < pol.attempts && is_retryable(replies[i].klass)) {
                        retry_next.push_back(i);
                    }
                }
                todo = std::move(retry_next);
            }

            ++rounds_;

            bool merge_failed = false;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                if (!replies[i].ok) continue;
                std::size_t const idx = exec_deliveries[i].executor_index;
                record_partial(state_.partial, idx, rounds_ - 1, replies[i].payload);
                if (is_output_selected(idx)) {
                    state_.selected_output = replies[i].payload;
                }
                // 025 §4 / ADR-055 follow-up -- see file banner's "Merge-on-join hook" paragraph.
                // `!merge_failed` stops attempting further same-round merges once one has already
                // failed (the round is terminating regardless; matches the routing loop's own
                // break-on-first-failure convention below).
                if (merge_on_join_hook_ && !merge_failed &&
                    graph_.executors[idx].worktree_mode == agentengine::sharing_mode::branch) {
                    agentengine::result<void> merged = merge_on_join_hook_(graph_.executors[idx].id);
                    if (!merged) {
                        state_.failed_executor = graph_.executors[idx].id;
                        merge_failed = true;
                    }
                }
            }
            if (merge_failed) {
                status = workflow_status::merge_conflict;
                for (auto const& d : port_deliveries) {
                    state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                }
                break;
            }

            std::vector<Delivery> next;
            bool                  broke = false;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                route_result const rr = route_from(exec_deliveries[i].executor_index, replies[i], next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[exec_deliveries[i].executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                              : workflow_status::executor_failed;
                broke = true;
                break;
            }
            if (broke) {
                for (auto const& d : port_deliveries) {
                    state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                }
                break;
            }

            // ADR-149 (issue #28 item 2): stall/reset safety valve. Checked here, using THIS round's
            // already-computed `replies`, in the exact same position as the `broke`/`merge_failed`
            // checks above -- so a trip that ends the run gets the identical terminal-path treatment
            // (unresolved `port_deliveries` become `unopened_ports`, `ports_` stays untouched, never
            // reported as a stale open interaction on a run that has actually ended). This position
            // also FIXES the precedence against `max_rounds`/`deadline_ms`, which are only re-checked
            // at the TOP of the loop for a would-be next round: a stall/reset trip on round N always
            // takes effect before that next check is ever reached (ADR-149 §3 finding 5).
            if (!designated_stall_reporter_.empty()) {
                // A REAL bug an early implementation had, caught by an end-to-end test against a
                // builder-produced manager/participant graph (not the self-loop-only unit tests
                // written first, none of which happened to exercise this): the designated reporter
                // does NOT run every round in a normal manager/participant alternation (the manager
                // runs, then a participant runs, then the manager again). Resetting stall_streak_ on
                // ANY round the reporter didn't run -- as an earlier version of this block did --
                // means the streak can never accumulate past 1 in that shape, silently defeating
                // max_stalls for exactly the graph this feature targets. A round the reporter did
                // not run in is NEUTRAL (leaves stall_streak_ unchanged) -- only a round the reporter
                // DID run in updates it, from that reply's own `stalled` value.
                //
                // A SECOND real bug, found by an independent post-implementation audit: the
                // quarantine block above only dedupes concurrent same-round deliveries to an
                // `agent`-kind executor -- a `function`-kind (or any non-agent-kind) designated
                // reporter can genuinely receive TWO deliveries in one round (e.g. two ordinary
                // edges converging on it), and an earlier version of this loop took only the FIRST
                // matching delivery's `stalled` value and `break`-ed, silently discarding a real
                // stall self-report on the second. Fixed by OR-aggregating `stalled` across EVERY
                // delivery to the reporter's index this round -- a safety valve must fail toward
                // counting a real stall report, not discarding one because of dispatch order.
                bool reporter_ran     = false;
                bool reporter_stalled = false;
                for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                    if (graph_.executors[exec_deliveries[i].executor_index].id != designated_stall_reporter_) {
                        continue;
                    }
                    reporter_ran = true;
                    if (replies[i].stalled) reporter_stalled = true;
                }
                if (reporter_ran) {
                    stall_streak_ = reporter_stalled ? stall_streak_ + 1 : 0;
                }

                if (graph_.bound.max_stalls.has_value() && stall_streak_ >= *graph_.bound.max_stalls) {
                    ++resets_used_;
                    stall_streak_ = 0;
                    bool const trip_ends_run = !graph_.bound.max_resets.has_value() ||
                                                resets_used_ > *graph_.bound.max_resets;
                    if (trip_ends_run) {
                        status = graph_.bound.max_resets.has_value() ? workflow_status::bound_max_resets
                                                                       : workflow_status::bound_max_stalls;
                        for (auto const& d : port_deliveries) {
                            state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                        }
                        break;
                    }
                    // Under the ceiling: this reset is silently absorbed and the run continues -- MAF's
                    // own "force a replan, capped total resets" shape. The engine never forces a
                    // replan itself; that stays the moderator's own job on its next invocation (014
                    // §3: "safety valve, not the termination contract").
                }
            }

            for (auto const& d : port_deliveries) {
                ports_.push_back(OpenPort{mint_interaction(d.executor_index), d.executor_index,
                                          d.payload, {}, false});
            }
            state_.pending = std::move(next);

            // 014 §5: "Checkpoint at superstep boundaries." Right here -- round N's results are fully
            // folded into state_/ports_, round N+1 (or a suspension) has not started yet. Fires
            // whether or not this round is about to suspend, so a checkpoint taken here always has
            // enough to resume from either way -- see file banner's "Checkpoint hook" paragraph.
            if (checkpoint_hook_) checkpoint_hook_(rounds_, to_record());

            // 014 §7's live-view bullet, same superstep boundary -- see file banner. `exec_deliveries`/
            // `replies`/`port_deliveries` are still this iteration's locals, built fresh from THIS
            // round, not reconstructed from `state_` (which no longer distinguishes "ran ok" from "ran
            // and failed" once folded into `partial`/`unopened_ports`).
            if (live_view_producer_.valid()) {
                agentengine::workflow::WorkflowLiveEvent ev;
                ev.round = rounds_;
                ev.executor_states.reserve(exec_deliveries.size() + port_deliveries.size());
                for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                    auto const st = replies[i].ok ? agentengine::workflow::executor_live_state::ran_ok
                                                    : agentengine::workflow::executor_live_state::ran_failed;
                    ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                        graph_.executors[exec_deliveries[i].executor_index].id, st});
                }
                for (auto const& d : port_deliveries) {
                    ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                        graph_.executors[d.executor_index].id,
                        agentengine::workflow::executor_live_state::port_open});
                }
                ev.in_flight_message_count = state_.pending.size();
                (void)live_view_producer_.push(std::move(ev));
            }

            if (!ports_.empty()) {
                status = workflow_status::suspended;
                break;
            }
        }

        co_return finish(status, entered_at);
    }

    [[nodiscard]] WorkflowResult finish(workflow_status status,
                                        std::chrono::steady_clock::time_point entered_at) {
        state_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - entered_at)
                                 .count();
        WorkflowResult r{};
        r.status          = status;
        r.rounds          = rounds_;
        r.output          = state_.selected_output;
        r.partial         = state_.partial;
        r.failed_executor = state_.failed_executor;
        if (status == workflow_status::suspended) r.open_interactions = open_interactions();
        r.unopened_ports = state_.unopened_ports;
        return r;
    }

    [[nodiscard]] agentengine::Interaction mint_interaction(std::size_t executor_index) const {
        agentengine::Interaction i{};
        i.interaction_id = run_id_ + ":port:" + graph_.executors[executor_index].id + ":" +
                           std::to_string(rounds_ == 0 ? 0 : rounds_ - 1);
        i.run_id = run_id_;
        i.reason = agentengine::interaction_reason::input;
        return i;
    }

    [[nodiscard]] route_result route_from(std::size_t from_index, ExecuteReply const& reply,
                                          std::vector<Delivery>& next) const {
        using agentengine::workflow::edge_kind;
        using agentengine::workflow::edge_failure_policy;
        std::string const& from_id = graph_.executors[from_index].id;

        if (!reply.ok) {
            EdgeFailurePolicy const pol    = policy_for(from_index);
            agentengine::Message const marker = failure_marker(from_id, reply.klass);

            switch (pol.kind) {
                case edge_failure_policy::fail:
                case edge_failure_policy::retry:
                    return route_result::workflow_failed;

                case edge_failure_policy::propagate:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from == from_id) deliver_once(next, index_of(edge.to), marker);
                    }
                    return route_result::ok;

                case edge_failure_policy::fallback:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from != from_id) continue;
                        if (edge.on_failure.kind != edge_failure_policy::fallback) continue;
                        deliver_once(next, index_of(edge.on_failure.fallback), marker);
                    }
                    return route_result::ok;
            }
            return route_result::workflow_failed;
        }

        std::size_t switch_edges = 0;
        std::size_t switch_fired = 0;

        for (auto const& edge : graph_.edges) {
            if (edge.from != from_id) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_edges;
            if (!edge_fires(edge, reply)) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_fired;

            std::size_t const target = index_of(edge.to);

            if (edge.kind == edge_kind::fan_in) {
                bool merged = false;
                for (auto& delivery : next) {
                    if (delivery.executor_index != target) continue;
                    for (auto const& item : reply.payload.content) {
                        delivery.payload.content.push_back(item);
                    }
                    merged = true;
                    break;
                }
                if (merged) continue;
            }
            next.push_back(Delivery{target, reply.payload});
        }

        if (switch_edges > 0 && switch_fired != 1) return route_result::routing_failed;
        return route_result::ok;
    }

    [[nodiscard]] static bool is_retryable(agentengine::failure_class klass) noexcept {
        return klass == agentengine::failure_class::transient ||
               klass == agentengine::failure_class::resource;
    }

    static void deliver_once(std::vector<Delivery>& next, std::size_t target,
                             agentengine::Message const& marker) {
        for (auto const& d : next) {
            if (d.executor_index == target) return;
        }
        next.push_back(Delivery{target, marker});
    }

    void record_partial(std::vector<ExecutorOutput>& partial, std::size_t executor_index,
                        std::uint32_t round, agentengine::Message const& payload) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto& out : partial) {
            if (out.executor_id != id) continue;
            out.round   = round;
            out.payload = payload;
            return;
        }
        partial.push_back(ExecutorOutput{id, round, payload});
    }

    using EdgeFailurePolicy = agentengine::workflow::EdgeFailurePolicy;

    [[nodiscard]] EdgeFailurePolicy policy_for(std::size_t executor_index) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto const& edge : graph_.edges) {
            if (edge.from == id) return edge.on_failure;
        }
        return EdgeFailurePolicy{};
    }

    [[nodiscard]] static bool edge_fires(agentengine::workflow::Edge const& edge,
                                        ExecuteReply const& reply) noexcept {
        using agentengine::workflow::edge_kind;
        switch (edge.kind) {
            case edge_kind::direct:
            case edge_kind::chain:
            case edge_kind::fan_out:
            case edge_kind::fan_in:
                return true;
            case edge_kind::switch_case:
            case edge_kind::multi_selection:
                for (auto const& route : reply.routes) {
                    if (route == edge.case_label) return true;
                }
                return false;
        }
        return false;
    }

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
    [[nodiscard]] bool agent_kind_bodies_are_structurally_agent_backed() const {
        for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
            if (graph_.executors[i].kind != agentengine::workflow::executor_kind::agent) continue;
            if (i >= bodies_.size() || !bodies_[i] ||
                bodies_[i].target<AgentExecutorBodyTag>() == nullptr) {
                return false;
            }
        }
        return true;
    }

    agentengine::workflow::Workflow         graph_;
    std::vector<ExecutorBody>               bodies_;
    std::vector<agentengine::EffectContext> contexts_;
    bool           valid_       = false;
    std::uint32_t  rounds_      = 0;
    std::uint64_t  run_counter_ = 0;
    std::string    run_id_;
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
