#pragma once
// ADR-037: agentengine::rt::WorkflowSupervisor's time-travel surface -- 014-Workflow-and-
// Orchestration.md §5's "rewind to any retained checkpoint and re-run forward, optionally with
// modified state ... every rewind is audited". Closes the gap `rt/workflow_supervisor.hpp`'s own
// Slice 2 banner named as deferred: "NO retained_checkpoints()/time-travel ... rt::SessionStore is a
// single-slot, overwrite-latest store BY DESIGN ... 014 §5's 'rewind to ANY retained checkpoint'
// genuinely needs an append-only, multi-version log" -- that log is `rt::AppendLogStore`
// (append_log_store.hpp), built and proven standalone in an earlier ADR-037 pass, wired in here.
//
// ADDITIVE, not a replacement: `rt::WorkflowSupervisor`'s existing Slice 2
// `save_workflow_checkpoint`/`load_workflow_checkpoint` (workflow_supervisor.hpp, built on
// `rt::SessionStore`) still answer "resume restores exactly" for ordinary crash recovery -- a
// caller who only needs that keeps using them unchanged. This file answers the genuinely different
// claim "rewind to ANY retained checkpoint," which a single-slot store cannot serve at all.
//
// TWO-PHASE PENDING->COMMITTED, preserved from the Quark original's own workflow/checkpoint.hpp
// (see that file's own header comment for the full "why two commits" reasoning -- a crash between
// the two leaves a dangling pending with no matching committed, and both `load_workflow_checkpoint`
// below and `retained_checkpoints` treat that as "never happened," never as ambiguous partial
// state). Re-expressed here as two separate `AppendLogStore::append()` calls to the SAME log id
// rather than two `quark::EventLog::commit()` calls -- `AppendLogStore::append()` is itself
// all-or-nothing per entry (append_log_store.hpp's own contract), the same atomicity property
// `EventLog::commit()` had, just exercised once per phase instead of batched.
//
// REWIND AUDIT IS A SEPARATE LOG FROM THE CHECKPOINT LOG, same run, different LogId. The Quark
// original's time_travel.hpp gives the real reason: mixing two differently-shaped records in one
// `EventLog<Event,S>` breaks type-safe replay (that type is templated on exactly one Event type).
// `rt::AppendLogStore` has no such per-log type constraint (it stores opaque bytes, decoded by the
// caller), so this file COULD technically share one log -- kept separate anyway, because the two
// records answer different questions ("what is this run's own state at index N" vs. "who rewound
// this run, when, and why") and an operator reviewing the audit trail should never have to filter
// out checkpoint payloads to find it.
//
// `rewind_workflow()` does NOT touch a live `WorkflowSupervisor` -- the caller feeds the returned
// `RunStateRecord` to `restore_from_record()` and continues via `resume_workflow()`, the exact same
// machinery an ordinary crash-recovery resume already uses (Slice 2). A rewind and a crash-recovery
// resume restore state identically; only how the record was CHOSEN differs, and only the choice
// needs a second code path.
//
// EFFECTS ARE NOT REWOUND (014 §5's own second claim) -- nothing to build here. `IdempotencyKey`
// derivation (`{run_id, turn_index/round, call_index, argument_digest}`, tool_pipeline.hpp) already
// re-derives the SAME key on a re-run with unmodified state and a DIFFERENT key the moment modified
// state changes an argument, by construction -- a workflow-level rewind API does not need to
// re-implement or re-decide that.
//
// I2 (CLAUDE.md): `operator_id`/`reason` below are the CALLER's own attribution, carried through
// verbatim, never inferred -- there is no Clock capability wired anywhere in this project yet
// (`audited_at_ns` stays 0, the same documented convention `Interaction::opened_at_ns` already uses)
// and no ambient identity this file could substitute for an explicit one.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow checkpoint_phase — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class checkpoint_phase { pending, committed };

// One checkpoint-log entry. `state` is only meaningful when `phase == pending` -- the `committed`
// marker carries no payload (already durable via its matching `pending` entry); re-encoding the
// whole state a second time would double the log's storage cost for zero benefit, the same shape
// the Quark original's own `WorkflowCheckpointRecord` used.
// ae-naming-lint: allow WorkflowCheckpointEntry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct WorkflowCheckpointEntry {
    std::uint64_t    checkpoint_index = 0;
    checkpoint_phase phase = checkpoint_phase::pending;
    RunStateRecord   state;
};

[[nodiscard]] inline agentengine::json::Value checkpoint_entry_to_json(
    WorkflowCheckpointEntry const& e) {
    std::vector<std::pair<std::string, agentengine::json::Value>> members = {
        {"checkpoint_index",
         agentengine::json::Value::make_number(static_cast<double>(e.checkpoint_index))},
        {"phase", agentengine::json::Value::make_string(
                       e.phase == checkpoint_phase::pending ? "pending" : "committed")},
    };
    if (e.phase == checkpoint_phase::pending) {
        members.emplace_back("state", run_state_record_to_json(e.state));
    }
    return agentengine::json::Value::make_object(std::move(members));
}

[[nodiscard]] inline agentengine::result<WorkflowCheckpointEntry> checkpoint_entry_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* index_v = v.find("checkpoint_index");
    agentengine::json::Value const* phase_v = v.find("phase");
    if (index_v == nullptr || !index_v->is_number() || phase_v == nullptr || !phase_v->is_string()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed WorkflowCheckpointEntry",
                                                    "rt.workflow_time_travel.entry.malformed"});
    }
    WorkflowCheckpointEntry out;
    out.checkpoint_index = static_cast<std::uint64_t>(index_v->as_number());
    std::string const& phase_str = phase_v->as_string();
    if (phase_str == "pending") {
        out.phase = checkpoint_phase::pending;
        agentengine::json::Value const* state_v = v.find("state");
        if (state_v == nullptr) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "pending entry missing its own state",
                                                        "rt.workflow_time_travel.entry.malformed"});
        }
        auto state = run_state_record_from_json(*state_v);
        if (!state) return std::unexpected(state.error());
        out.state = std::move(*state);
    } else if (phase_str == "committed") {
        out.phase = checkpoint_phase::committed;
    } else {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "unknown checkpoint phase: " + phase_str,
                                                    "rt.workflow_time_travel.entry.malformed"});
    }
    return out;
}

// 014 §5: "every rewind is audited". See file banner for why operator_id/reason are caller-supplied
// rather than inferred, and why audited_at_ns stays 0.
// ae-naming-lint: allow RewindAuditRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RewindAuditRecord {
    std::string   run_id;
    std::uint64_t from_checkpoint_index = 0;
    std::uint64_t to_checkpoint_index   = 0;
    std::string   operator_id;
    std::string   reason;
    bool          state_modified = false;
    std::int64_t  audited_at_ns  = 0;
};

[[nodiscard]] inline agentengine::json::Value rewind_audit_to_json(RewindAuditRecord const& r) {
    return agentengine::json::Value::make_object({
        {"run_id", agentengine::json::Value::make_string(r.run_id)},
        {"from_checkpoint_index",
         agentengine::json::Value::make_number(static_cast<double>(r.from_checkpoint_index))},
        {"to_checkpoint_index",
         agentengine::json::Value::make_number(static_cast<double>(r.to_checkpoint_index))},
        {"operator_id", agentengine::json::Value::make_string(r.operator_id)},
        {"reason", agentengine::json::Value::make_string(r.reason)},
        {"state_modified", agentengine::json::Value::make_bool(r.state_modified)},
        {"audited_at_ns", agentengine::json::Value::make_number(static_cast<double>(r.audited_at_ns))},
    });
}

[[nodiscard]] inline agentengine::result<RewindAuditRecord> rewind_audit_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* run_id_v    = v.find("run_id");
    agentengine::json::Value const* from_v      = v.find("from_checkpoint_index");
    agentengine::json::Value const* to_v        = v.find("to_checkpoint_index");
    agentengine::json::Value const* operator_v  = v.find("operator_id");
    agentengine::json::Value const* reason_v    = v.find("reason");
    agentengine::json::Value const* modified_v  = v.find("state_modified");
    agentengine::json::Value const* audited_v   = v.find("audited_at_ns");
    if (run_id_v == nullptr || !run_id_v->is_string() || from_v == nullptr || !from_v->is_number() ||
        to_v == nullptr || !to_v->is_number() || operator_v == nullptr || !operator_v->is_string() ||
        reason_v == nullptr || !reason_v->is_string() || modified_v == nullptr ||
        !modified_v->is_bool() || audited_v == nullptr || !audited_v->is_number()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed RewindAuditRecord",
                                                    "rt.workflow_time_travel.audit.malformed"});
    }
    RewindAuditRecord out;
    out.run_id                = run_id_v->as_string();
    out.from_checkpoint_index = static_cast<std::uint64_t>(from_v->as_number());
    out.to_checkpoint_index   = static_cast<std::uint64_t>(to_v->as_number());
    out.operator_id            = operator_v->as_string();
    out.reason                 = reason_v->as_string();
    out.state_modified         = modified_v->as_bool();
    out.audited_at_ns          = static_cast<std::int64_t>(audited_v->as_number());
    return out;
}

// The two log ids a run's time-travel surface lives under -- see file banner for why they're
// separate rather than one shared log.
[[nodiscard]] inline LogId workflow_checkpoint_log_id(std::string_view run_id) {
    return std::string(run_id) + ":checkpoints";
}
[[nodiscard]] inline LogId workflow_rewind_audit_log_id(std::string_view run_id) {
    return std::string(run_id) + ":rewind_audit";
}

// ---- Phase 1: PENDING, carrying the real payload -----------------------------------------------

template <AppendLogStore StoreT>
[[nodiscard]] result<void> stage_pending_checkpoint(StoreT& store, std::string_view run_id,
                                                     std::uint64_t checkpoint_index,
                                                     RunStateRecord const& state) {
    WorkflowCheckpointEntry entry{checkpoint_index, checkpoint_phase::pending, state};
    std::string const text = agentengine::json::dump(checkpoint_entry_to_json(entry));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(workflow_checkpoint_log_id(run_id), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());
    return {};
}

// ---- Phase 2: COMMITTED, a bare marker ---------------------------------------------------------

template <AppendLogStore StoreT>
[[nodiscard]] result<void> commit_checkpoint(StoreT& store, std::string_view run_id,
                                              std::uint64_t checkpoint_index) {
    WorkflowCheckpointEntry entry{checkpoint_index, checkpoint_phase::committed, RunStateRecord{}};
    std::string const text = agentengine::json::dump(checkpoint_entry_to_json(entry));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(workflow_checkpoint_log_id(run_id), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());
    return {};
}

// The convenience most callers want: both phases, in order.
template <AppendLogStore StoreT>
[[nodiscard]] result<void> save_retained_checkpoint(StoreT& store, std::string_view run_id,
                                                     std::uint64_t checkpoint_index,
                                                     RunStateRecord const& state) {
    auto r1 = stage_pending_checkpoint(store, run_id, checkpoint_index, state);
    if (!r1) return r1;
    return commit_checkpoint(store, run_id, checkpoint_index);
}

namespace detail {

struct CheckpointSlot {
    bool           has_pending   = false;
    bool           has_committed = false;
    RunStateRecord state;
};

template <AppendLogStore StoreT>
[[nodiscard]] result<std::map<std::uint64_t, CheckpointSlot>> read_checkpoint_slots(
    StoreT const& store, std::string_view run_id) {
    auto raw = store.read_from(workflow_checkpoint_log_id(run_id), 0);
    if (!raw) return std::unexpected(raw.error());

    std::map<std::uint64_t, CheckpointSlot> by_index;  // ordered: oldest-first iteration
    for (std::vector<std::byte> const& bytes : *raw) {
        std::string text;
        text.reserve(bytes.size());
        for (std::byte b : bytes) text.push_back(static_cast<char>(b));
        auto parsed = agentengine::json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto entry = checkpoint_entry_from_json(*parsed);
        if (!entry) return std::unexpected(entry.error());

        CheckpointSlot& slot = by_index[entry->checkpoint_index];
        if (entry->phase == checkpoint_phase::pending) {
            slot.has_pending = true;
            slot.state       = std::move(entry->state);
        } else {
            slot.has_committed = true;
        }
    }
    return by_index;
}

}  // namespace detail

// The latest FULLY committed checkpoint (both phases durable). `nullopt` when the run has never had
// a fully-committed checkpoint through THIS surface (a dangling pending with no matching committed
// counts as never, by design -- see file banner).
template <AppendLogStore StoreT>
[[nodiscard]] result<std::optional<RunStateRecord>> load_latest_retained_checkpoint(
    StoreT const& store, std::string_view run_id) {
    auto slots = detail::read_checkpoint_slots(store, run_id);
    if (!slots) return std::unexpected(slots.error());

    bool          found      = false;
    std::uint64_t best_index = 0;
    for (auto const& [index, slot] : *slots) {
        if (!slot.has_pending || !slot.has_committed) continue;
        if (!found || index > best_index) {
            found      = true;
            best_index = index;
        }
    }
    if (!found) return std::optional<RunStateRecord>{};
    return std::optional<RunStateRecord>{slots->at(best_index).state};
}

// Every retained checkpoint (both phases durable), oldest first -- 014 §5's "rewind to ANY retained
// checkpoint," not just the latest.
template <AppendLogStore StoreT>
[[nodiscard]] result<std::vector<std::pair<std::uint64_t, RunStateRecord>>> retained_checkpoints(
    StoreT const& store, std::string_view run_id) {
    auto slots = detail::read_checkpoint_slots(store, run_id);
    if (!slots) return std::unexpected(slots.error());

    std::vector<std::pair<std::uint64_t, RunStateRecord>> out;
    for (auto const& [index, slot] : *slots) {
        if (slot.has_pending && slot.has_committed) out.emplace_back(index, slot.state);
    }
    return out;
}

// Every audited rewind for a run, in commit order -- an operator's own review trail, and the thing
// that makes "every rewind is audited" a checkable claim rather than a promise.
template <AppendLogStore StoreT>
[[nodiscard]] result<std::vector<RewindAuditRecord>> read_rewind_audit_log(StoreT const& store,
                                                                            std::string_view run_id) {
    auto raw = store.read_from(workflow_rewind_audit_log_id(run_id), 0);
    if (!raw) return std::unexpected(raw.error());

    std::vector<RewindAuditRecord> out;
    out.reserve(raw->size());
    for (std::vector<std::byte> const& bytes : *raw) {
        std::string text;
        text.reserve(bytes.size());
        for (std::byte b : bytes) text.push_back(static_cast<char>(b));
        auto parsed = agentengine::json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto rec = rewind_audit_from_json(*parsed);
        if (!rec) return std::unexpected(rec.error());
        out.push_back(std::move(*rec));
    }
    return out;
}

// 014 §5: "rewind to any retained checkpoint and re-run forward, optionally with modified state."
// Selects `to_checkpoint_index` from `retained_checkpoints`, durably audits the request FIRST (the
// call itself is the audited event, whether or not the caller goes on to apply the returned record
// anywhere), then returns the record to restore. `modified_state`, when present, REPLACES the
// retained checkpoint's own state wholesale -- 014 §5 does not specify a granular diff/patch
// mechanism, and a caller that wants to change one field already has the retained record in hand to
// copy-and-edit in ordinary C++ before calling this.
//
// Does NOT itself touch a live WorkflowSupervisor -- see file banner.
template <AppendLogStore StoreT>
[[nodiscard]] result<RunStateRecord> rewind_workflow(
    StoreT& store, std::string_view run_id, std::uint64_t to_checkpoint_index,
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
    if (!target) {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::contract,
                                "rewind_workflow: no retained checkpoint at the requested index",
                                "rt.workflow_time_travel.rewind.not_found"});
    }

    RewindAuditRecord audit;
    audit.run_id                = std::string(run_id);
    audit.from_checkpoint_index = from_index;
    audit.to_checkpoint_index   = to_checkpoint_index;
    audit.operator_id           = std::move(operator_id);
    audit.reason                = std::move(reason);
    audit.state_modified        = modified_state.has_value();

    std::string const text = agentengine::json::dump(rewind_audit_to_json(audit));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(workflow_rewind_audit_log_id(run_id), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());

    return modified_state.has_value() ? std::move(*modified_state) : std::move(*target);
}

}  // namespace agentengine::rt
