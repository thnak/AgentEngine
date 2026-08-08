#pragma once
// Implements 014-Workflow-and-Orchestration.md §5's time-travel bullet: "rewind to any retained
// checkpoint and re-run forward, optionally with modified state ... every rewind is audited,
// because rewinding a workflow that already had external effects is a correctness hazard the
// operator must own" and "effects are not rewound ... idempotency keys (019) are the mechanism
// that keeps that from double-charging". Milestone 6 Phase F
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md, decision 9: "the audit
// obligation is built WITH the rewind, not after it -- a rewind API that ships without the audit
// record would be the footgun §5 explicitly warns against").
//
// SCOPE. `rewind_workflow()` selects a retained checkpoint (checkpoint.hpp's own
// `retained_checkpoints`), durably audits the request, and returns the `RunStateRecord` a caller
// then feeds to `WorkflowSupervisor::restore_from_record()` and continues via `ContinueWorkflow` or
// `ResumeWorkflow` -- reusing Phase F's own checkpoint/resume machinery rather than inventing a
// second execution path for "resume after a rewind" versus "resume after a crash". There is
// nothing else to build for "effects are not rewound / idempotency keys keep it from
// double-charging": `IdempotencyKey`/`EffectJournal` (019 §3, M4 Phase F1/F2, tool_pipeline.hpp/
// effect_journal.hpp) already exist and are derived from `{run_id, turn_index, call_index,
// argument_digest}` -- an executor authored against that machinery (any tool call inside a
// `FunctionExecutor`'s body) automatically re-derives the SAME key on a re-run with unmodified
// state (same run_id/round/arguments in, same digest out) and a DIFFERENT key the moment modified
// state changes an argument -- both the right answer, by construction, not something a workflow-
// level rewind API has to re-implement or re-decide.
//
// AUDIT LOG IS A SEPARATE EVENTLOG FROM THE CHECKPOINT LOG, SAME RUN, DIFFERENT ActorId. Quark's
// `encode_durable`/`read_migrated` prefix every event with its OWN type's fingerprint header
// (016), and `EventLog<Event,S>`/`replay_tail<Event,...>` are templated on exactly ONE `Event`
// type -- mixing `WorkflowCheckpointRecord` and `RewindAuditRecord` entries in the SAME per-actor
// log would make `replay_tail<WorkflowCheckpointRecord>` fail to decode the first audit entry it
// encountered (a type-key mismatch, not silently skipped). A second, dedicated ActorId keeps the
// two logs independently replayable, the same reasoning a session's Snapshot slot and its
// EventSourced effect-journal slot already stay separate under one `Store` (persistence.hpp's own
// per-actor `{snapshot, log}` layout) -- this is one more log under the same seam, not a second
// storage engine (019 §3's own rule, reused).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/ids.hpp"

#include "agentengine/workflow/checkpoint.hpp"

namespace agentengine::workflow {

// 014 §5: "every rewind is audited". Deliberately NOT a wall-clock timestamp or an ambiently-
// derived principal -- no Clock capability is wired anywhere in this project yet (the same
// documented gap `Interaction::opened_at_ns` names), and I2 forbids inferring WHO authorized a
// rewind rather than being explicitly told. `operator_id`/`reason` are the caller's own
// attribution, carried through verbatim, never guessed.
struct RewindAuditRecord {  // ae-naming-lint: allow RewindAuditRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::string   run_id;
    std::uint64_t from_checkpoint_index = 0;  // the run's most recently retained checkpoint BEFORE this rewind (0 if none existed)
    std::uint64_t to_checkpoint_index   = 0;  // the checkpoint rewound TO
    std::string   operator_id;                // caller-supplied attribution (I2: never inferred)
    std::string   reason;                     // caller-supplied, free text
    bool          state_modified = false;     // true iff the caller supplied a state override, not a bare rewind
    std::int64_t  audited_at_ns  = 0;          // 0: no wall-clock source wired (same convention as Interaction)

    friend bool operator==(RewindAuditRecord const&, RewindAuditRecord const&) = default;
};
QUARK_SERIALIZE(RewindAuditRecord, (1, run_id), (2, from_checkpoint_index), (3, to_checkpoint_index),
                (4, operator_id), (5, reason), (6, state_modified), (7, audited_at_ns))

// A stable ActorId tag for a run's rewind-audit log, distinct from `kWorkflowRunTypeKey`
// (checkpoint.hpp) for the reason this file's own header comment gives -- packing "AEWFRWD1" as 8
// ASCII bytes, the same "arbitrary fixed constant" precedent every other TypeKey in this project uses.
inline constexpr quark::TypeKey kWorkflowRewindAuditTypeKey{0x4145'5746'5257'4431ULL};  // "AEWFRWD1"

[[nodiscard]] inline quark::ActorId workflow_rewind_audit_actor_id(std::string_view run_id) noexcept {
    return quark::ActorId{kWorkflowRewindAuditTypeKey, std::hash<std::string_view>{}(run_id)};
}

// Every audited rewind for a run, in commit order -- an operator's own review trail, and the thing
// that makes "every rewind is audited" a checkable claim rather than a promise.
template <quark::Store S>
[[nodiscard]] quark::result<std::vector<RewindAuditRecord>> read_rewind_audit_log(
    S& store, std::string_view run_id) {
    std::vector<RewindAuditRecord> entries;
    auto last = quark::replay_tail<RewindAuditRecord>(
        store, workflow_rewind_audit_actor_id(run_id), /*from=*/0, entries,
        [](std::vector<RewindAuditRecord>& acc, RewindAuditRecord const& ev) { acc.push_back(ev); });
    if (!last) return std::unexpected(last.error());
    return entries;
}

// 014 §5: "rewind to any retained checkpoint and re-run forward, optionally with modified state."
// Selects `to_checkpoint_index` from `retained_checkpoints`, durably audits the request FIRST (the
// call itself is the audited event, whether or not the caller goes on to apply the returned
// record anywhere -- "every rewind is audited" names the REQUEST, not a later application step
// this function has no way to observe), then returns the record to restore. `modified_state`,
// when present, REPLACES the retained checkpoint's own state wholesale rather than patching
// individual fields -- 014 §5 does not specify a granular diff/patch mechanism, and a caller that
// wants to change one field already has the retained record in hand to copy-and-edit in ordinary
// C++ before calling this.
//
// Does NOT itself touch a `WorkflowSupervisor` -- the caller feeds the returned `RunStateRecord` to
// `restore_from_record()` and continues via `ContinueWorkflow`/`ResumeWorkflow`, the SAME machinery
// an ordinary crash-recovery resume already uses (checkpoint.hpp). A rewind and a crash-recovery
// resume restore state identically; only how the record was CHOSEN differs, and only the choice
// needs a second code path.
template <quark::Store S>
[[nodiscard]] quark::result<RunStateRecord> rewind_workflow(
    S& store, std::string_view run_id, quark::FenceToken fence, std::uint64_t to_checkpoint_index,
    std::string operator_id, std::string reason,
    std::optional<RunStateRecord> modified_state = std::nullopt) {
    auto all = retained_checkpoints(store, run_id);
    if (!all) return std::unexpected(all.error());

    std::optional<RunStateRecord> target;
    std::uint64_t                 from_index = 0;
    for (auto const& [index, rec] : *all) {
        if (index == to_checkpoint_index) target = rec;
        if (index > from_index) from_index = index;
    }
    // Matches checkpoint.hpp's own `quark::result` signature (mirroring effect_journal.hpp/
    // agent_session.hpp's checkpoint functions, all `quark::result`, never `agentengine::error`) --
    // this layer stays entirely on Quark's Store-facing error taxonomy rather than mixing in a
    // second one for an application-level "no such checkpoint" case.
    if (!target) {
        return quark::fail(quark::errc::not_found,
                           "rewind_workflow: no retained checkpoint at the requested index");
    }

    RewindAuditRecord audit;
    audit.run_id               = std::string(run_id);
    audit.from_checkpoint_index = from_index;
    audit.to_checkpoint_index   = to_checkpoint_index;
    audit.operator_id           = std::move(operator_id);
    audit.reason                = std::move(reason);
    audit.state_modified        = modified_state.has_value();

    auto const audit_id = workflow_rewind_audit_actor_id(run_id);
    quark::EventLog<RewindAuditRecord, S> log(store, audit_id, fence, store.last_seq(audit_id) + 1);
    log.stage(audit);
    auto rc = log.commit();
    if (!rc) return std::unexpected(rc.error());

    return modified_state.has_value() ? std::move(*modified_state) : std::move(*target);
}

}  // namespace agentengine::workflow
