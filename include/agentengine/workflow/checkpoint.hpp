#pragma once
// Implements 014-Workflow-and-Orchestration.md §5 (checkpoint, resume) and the milestone-6
// breakdown doc's decision 7 (two-phase pending->committed discipline, proven single-node this
// phase) and decision 11 (a suspended run's state lives in the actor's own fields, which is what
// makes it a plain durable record in the first place). Milestone 6 Phase F
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// `RunStateRecord` is `WorkflowSupervisor`'s durable projection -- the run's position (run_id,
// run_counter, rounds), its in-flight messages (pending), its workflow state (partial results,
// selected output, failure), and any open request ports -- mirroring `AgentSessionRecord`'s own
// shape (agent_session.hpp) exactly: a flat record, `to_record()`/`restore_from_record()` as the
// only two places that cross between the live actor and its durable shape. Depends on
// `content_record.hpp` (Message/ContentItem's own durable projection, this milestone's own
// prerequisite) for every field that carries a real payload -- §5's "resume restores exactly"
// means a checkpoint that dropped Message content would not actually restore the run exactly, the
// same reasoning `AgentSessionRecord`'s comment gives for why it could NOT cover history/state
// before this projection existed.
//
// TWO-PHASE PENDING->COMMITTED (decision 7 / 014 §9 resolved Q4). `Store`'s snapshot slot is
// latest-only by construction (persistence.hpp's own `InMemoryStore::save_snapshot` overwrites);
// 014 §5's "rewind to ANY retained checkpoint" needs more than one, so this uses the EventSourced
// model instead (`EventLog<WorkflowCheckpointRecord,S>`, the exact idiom `effect_journal.hpp`
// already established for an intent/outcome pair) -- every checkpoint attempt is retained in
// commit order, for free, as a property of an append-only log. A checkpoint attempt is TWO
// SEPARATE `EventLog::commit()` calls, not one atomic batch: `checkpoint_phase::pending` (carrying
// the real state) durable FIRST, `checkpoint_phase::committed` (a bare marker) durable SECOND.
// `EventLog::commit()` is itself all-or-nothing (ADR-009 C7), so a crash strictly BEFORE phase 1's
// commit returns leaves nothing durable -- already indistinguishable from "never attempted", which
// `load_workflow_checkpoint` treats correctly by definition. The two-phase split earns its keep
// for the crash BETWEEN the two commits: `load_workflow_checkpoint` only trusts an index that has
// BOTH markers, so a dangling pending-without-committed at the tail is ignored, falling back to
// whichever earlier index last had both -- "resume treats it as hadn't happened," per the RFC's
// own words, not as ambiguous partial state. `stage_pending_checkpoint`/`commit_checkpoint` are
// exposed separately (not just the combined `save_workflow_checkpoint`) so a test can inject
// exactly that crash by calling the first and never the second.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quark/core/event_log.hpp"
#include "quark/core/ids.hpp"

#include "agentengine/core/content_record.hpp"
#include "agentengine/core/interaction.hpp"

namespace agentengine::workflow {

// One pending delivery: which executor, and its payload -- `WorkflowSupervisor::Delivery`'s
// durable shape.
struct DeliveryRecord {  // ae-naming-lint: allow DeliveryRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::uint64_t executor_index = 0;
    MessageRecord payload;

    friend bool operator==(DeliveryRecord const&, DeliveryRecord const&) = default;
};
QUARK_SERIALIZE(DeliveryRecord, (1, executor_index), (2, payload))

// 014 §6's third bullet, durable shape -- `WorkflowSupervisor::ExecutorOutput`.
struct ExecutorOutputRecord {  // ae-naming-lint: allow ExecutorOutputRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::string   executor_id;
    std::uint32_t round = 0;
    MessageRecord payload;

    friend bool operator==(ExecutorOutputRecord const&, ExecutorOutputRecord const&) = default;
};
QUARK_SERIALIZE(ExecutorOutputRecord, (1, executor_id), (2, round), (3, payload))

// 014 §4's open request port, durable shape -- `WorkflowSupervisor::OpenPort`. Checkpointing this
// (not just `RunState`) is what lets a restored actor answer a REAL `ResumeWorkflow` after a
// process restart -- Phase E's G5 was only ever "half proven" (activation census; the "resumes
// after a process restart" half had nothing to restore FROM until this record existed).
struct OpenPortRecord {  // ae-naming-lint: allow OpenPortRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    Interaction               interaction;  // already Described (interaction.hpp) -- no gap here
    std::uint64_t             executor_index = 0;
    MessageRecord             response;
    std::vector<std::string>  routes;
    bool                      resolved = false;

    friend bool operator==(OpenPortRecord const&, OpenPortRecord const&) = default;
};
QUARK_SERIALIZE(OpenPortRecord, (1, interaction), (2, executor_index), (3, response), (4, routes),
                (5, resolved))

// `WorkflowSupervisor`'s full durable projection: the run's position, its in-flight messages, its
// workflow state, and its open ports. NOT the graph description (`Workflow`, graph.hpp) or the
// executor `ActorRef`s (`refs_`) -- 014 §5's "resume restores exactly, on any node in a cluster"
// presumes the graph itself is redeployed by the host from the same description, the same relationship
// `AgentSessionRecord` has to a session's static configuration versus its dynamic state.
struct RunStateRecord {  // ae-naming-lint: allow RunStateRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::uint64_t run_counter = 0;
    std::string   run_id;
    std::uint32_t rounds = 0;
    std::vector<DeliveryRecord>       pending;
    std::vector<ExecutorOutputRecord> partial;
    MessageRecord selected_output;
    std::string   failed_executor;
    std::vector<std::string> unopened_ports;
    std::int64_t  elapsed_ns = 0;
    std::vector<OpenPortRecord> ports;

    friend bool operator==(RunStateRecord const&, RunStateRecord const&) = default;
};
QUARK_SERIALIZE(RunStateRecord, (1, run_counter), (2, run_id), (3, rounds), (4, pending),
                (5, partial), (6, selected_output), (7, failed_executor), (8, unopened_ports),
                (9, elapsed_ns), (10, ports))

// ---- The two-phase checkpoint event, and where it lives in the Store -------------------------

enum class checkpoint_phase { pending, committed };  // ae-naming-lint: allow checkpoint_phase — 014 §5 names "checkpoint" normatively; 027 has not been updated to list this vocabulary

// One checkpoint-log entry. `state` is only meaningful when `phase == pending` -- the `committed`
// marker carries no payload (already durable via its matching `pending` record); re-writing the
// whole state a second time would double the log's storage cost for zero benefit, the same shape
// `EffectJournalEntry`'s intent/outcome pair already uses (effect_journal.hpp).
struct WorkflowCheckpointRecord {  // ae-naming-lint: allow WorkflowCheckpointRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::uint64_t    checkpoint_index = 0;  // this run's own monotonic counter, one per superstep
    checkpoint_phase phase = checkpoint_phase::pending;
    RunStateRecord   state;

    friend bool operator==(WorkflowCheckpointRecord const&, WorkflowCheckpointRecord const&) = default;
};
QUARK_SERIALIZE(WorkflowCheckpointRecord, (1, checkpoint_index), (2, phase), (3, state))

// A stable, hand-picked ActorId tag for a workflow run's checkpoint log -- the same "arbitrary
// fixed constant, not a serialization fingerprint" precedent `kAgentSessionTypeKey`
// (agent_session.hpp) already uses, packing "AEWFRUN1" as 8 ASCII bytes.
inline constexpr quark::TypeKey kWorkflowRunTypeKey{0x4145'5746'5255'4E31ULL};  // "AEWFRUN1"

// The stable ActorId a run's `run_id` string maps to, for addressing its checkpoint log under a
// `Store` -- mirrors `session_actor_id()` exactly (same string->ActorId bridge).
[[nodiscard]] inline quark::ActorId workflow_run_actor_id(std::string_view run_id) noexcept {
    return quark::ActorId{kWorkflowRunTypeKey, std::hash<std::string_view>{}(run_id)};
}

// ---- Phase 1: PENDING, carrying the real payload ------------------------------------------

template <quark::Store S>
[[nodiscard]] quark::result<void> stage_pending_checkpoint(S& store, std::string_view run_id,
                                                            quark::FenceToken fence,
                                                            std::uint64_t checkpoint_index,
                                                            RunStateRecord const& state) {
    auto const id = workflow_run_actor_id(run_id);
    quark::EventLog<WorkflowCheckpointRecord, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(WorkflowCheckpointRecord{checkpoint_index, checkpoint_phase::pending, state});
    auto rc = log.commit();
    if (!rc) return std::unexpected(rc.error());
    return {};
}

// ---- Phase 2: COMMITTED, a bare marker -----------------------------------------------------

template <quark::Store S>
[[nodiscard]] quark::result<void> commit_checkpoint(S& store, std::string_view run_id,
                                                     quark::FenceToken fence,
                                                     std::uint64_t checkpoint_index) {
    auto const id = workflow_run_actor_id(run_id);
    quark::EventLog<WorkflowCheckpointRecord, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(
        WorkflowCheckpointRecord{checkpoint_index, checkpoint_phase::committed, RunStateRecord{}});
    auto rc = log.commit();
    if (!rc) return std::unexpected(rc.error());
    return {};
}

// The convenience most callers want: both phases, in order. A test proving decision 7's fallback
// calls `stage_pending_checkpoint` alone and never `commit_checkpoint`, simulating a crash between
// the two -- see this file's own header comment.
template <quark::Store S>
[[nodiscard]] quark::result<void> save_workflow_checkpoint(S& store, std::string_view run_id,
                                                            quark::FenceToken fence,
                                                            std::uint64_t checkpoint_index,
                                                            RunStateRecord const& state) {
    auto rc1 = stage_pending_checkpoint(store, run_id, fence, checkpoint_index, state);
    if (!rc1) return rc1;
    return commit_checkpoint(store, run_id, fence, checkpoint_index);
}

// The latest FULLY committed checkpoint (both phases durable) -- 014 §5's "resume restores
// exactly", read back. `nullopt` when the run has never had a fully-committed checkpoint (a
// dangling `pending` with no matching `committed` counts as never, by design -- decision 7).
template <quark::Store S>
[[nodiscard]] quark::result<std::optional<RunStateRecord>> load_workflow_checkpoint(
    S& store, std::string_view run_id) {
    std::vector<WorkflowCheckpointRecord> entries;
    auto last = quark::replay_tail<WorkflowCheckpointRecord>(
        store, workflow_run_actor_id(run_id), /*from=*/0, entries,
        [](std::vector<WorkflowCheckpointRecord>& acc, WorkflowCheckpointRecord const& ev) {
            acc.push_back(ev);
        });
    if (!last) return std::unexpected(last.error());

    struct Slot {
        bool           has_pending   = false;
        bool           has_committed = false;
        RunStateRecord state;
    };
    std::unordered_map<std::uint64_t, Slot> by_index;
    for (auto const& e : entries) {
        auto& slot = by_index[e.checkpoint_index];
        if (e.phase == checkpoint_phase::pending) {
            slot.has_pending = true;
            slot.state       = e.state;
        } else {
            slot.has_committed = true;
        }
    }

    bool          found = false;
    std::uint64_t best_index = 0;
    for (auto const& [index, slot] : by_index) {
        if (!slot.has_pending || !slot.has_committed) continue;  // decision 7: not fully committed
        if (!found || index > best_index) {
            found      = true;
            best_index = index;
        }
    }
    if (!found) return std::optional<RunStateRecord>{};
    return std::optional<RunStateRecord>{by_index[best_index].state};
}

// Every retained checkpoint (both phases durable), oldest first -- 014 §5's "rewind to ANY
// retained checkpoint", not just the latest. Time-travel (workflow/time_travel.hpp) selects among
// these by `checkpoint_index`.
template <quark::Store S>
[[nodiscard]] quark::result<std::vector<std::pair<std::uint64_t, RunStateRecord>>>
retained_checkpoints(S& store, std::string_view run_id) {
    std::vector<WorkflowCheckpointRecord> entries;
    auto last = quark::replay_tail<WorkflowCheckpointRecord>(
        store, workflow_run_actor_id(run_id), /*from=*/0, entries,
        [](std::vector<WorkflowCheckpointRecord>& acc, WorkflowCheckpointRecord const& ev) {
            acc.push_back(ev);
        });
    if (!last) return std::unexpected(last.error());

    struct Slot {
        bool           has_pending   = false;
        bool           has_committed = false;
        RunStateRecord state;
    };
    std::map<std::uint64_t, Slot> by_index;  // ordered: iteration below yields oldest-first
    for (auto const& e : entries) {
        auto& slot = by_index[e.checkpoint_index];
        if (e.phase == checkpoint_phase::pending) {
            slot.has_pending = true;
            slot.state       = e.state;
        } else {
            slot.has_committed = true;
        }
    }

    std::vector<std::pair<std::uint64_t, RunStateRecord>> out;
    for (auto const& [index, slot] : by_index) {
        if (slot.has_pending && slot.has_committed) out.emplace_back(index, slot.state);
    }
    return out;
}

}  // namespace agentengine::workflow
