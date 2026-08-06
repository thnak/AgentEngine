#pragma once
// Implements 019-Durability-and-Long-Running-Agents.md §3 — "Effect journaling: the intent to
// perform an effect is journaled before execution and the outcome journaled after. On resume, a
// journaled-but-unconfirmed effect is reconciled." Milestone 4 Phase F2
// (docs/planning/milestone-4-sessions-durability-memory-breakdown.md).
//
// Rides Quark's `Store` seam directly, EventSourced model, via the EXACT `EventLog<T,S>::stage()`/
// `commit()` idiom `core/worktree.hpp`'s `Ref`/`commit_ref` already established (M3 decision 1) —
// applied to a session's own ActorId (`session_actor_id()`, Phase A1) rather than a worktree Ref's.
// A session's Snapshot-model checkpoint (Phase A4/D1, `AgentSessionRecord`) and its EventSourced
// effect journal are two DIFFERENT slots under the SAME `Store`/`ActorId` (012's own storage seam
// keeps snapshot and log state separate per actor, `persistence.hpp`'s own `InMemoryStore` layout)
// — no conflict, and nothing else in this codebase appends to a session's event-log slot.
//
// F3 (019 §7 G6's "surface indeterminate, never guess" claim) is deliberately NOT built here —
// flagged by the M4 kickoff for its own design→red-team→prove→judge/ADR pass, per CLAUDE.md's own
// "contested, hot-path, or security-critical" bar. `unconfirmed_effect_intents()` below answers
// only "which journaled intents have no matching outcome yet" — a read, not a decision; deciding
// what to DO about one (retry under the same key, or surface as indeterminate) is F3's job.

#include <string>
#include <unordered_map>
#include <vector>

#include "quark/core/event_log.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

enum class effect_journal_phase { intent, outcome };  // ae-naming-lint: allow effect_journal_phase — 019 §3 names "journaled" normatively; 027 has not been updated to list this vocabulary

// All scalars/strings — no variant, no serialization gap (unlike `Message`/`ContentItem` or
// `Capability`), so this is real from the start, matching `Interaction`'s own precedent (Phase E1).
struct EffectJournalEntry {  // ae-naming-lint: allow EffectJournalEntry — 019 §3 names "effect journaling" normatively; 027 has not been updated to list a record type
    std::string          idempotency_key;  // IdempotencyKey::to_string() — the dedup identity
    std::string          tool_name;
    std::string          call_id;
    effect_journal_phase phase = effect_journal_phase::intent;
    bool                  outcome_ok = false;  // only meaningful when phase == outcome

    friend bool operator==(EffectJournalEntry const&, EffectJournalEntry const&) = default;
};
QUARK_SERIALIZE(EffectJournalEntry, (1, idempotency_key), (2, tool_name), (3, call_id), (4, phase),
                 (5, outcome_ok))

// "The intent to perform an effect is journaled before execution." One event, appended under the
// session's own ActorId — mirrors `worktree.hpp::commit_ref_impl` exactly: acquire/reuse a fence,
// stage one event, commit.
template <quark::Store S>
[[nodiscard]] quark::result<quark::SeqNo> journal_effect_intent(S& store, std::string_view session_id,
                                                                  quark::FenceToken fence,
                                                                  IdempotencyKey const& key,
                                                                  std::string tool_name,
                                                                  std::string call_id) {
    auto const id = session_actor_id(session_id);
    quark::EventLog<EffectJournalEntry, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(EffectJournalEntry{key.to_string(), std::move(tool_name), std::move(call_id),
                                  effect_journal_phase::intent, false});
    return log.commit();
}

// "...and the outcome journaled after." Same log, same key — `ok` is the effect's own success/
// failure, not this journal write's (a failed append surfaces as this function's own error result,
// same as `journal_effect_intent`).
template <quark::Store S>
[[nodiscard]] quark::result<quark::SeqNo> journal_effect_outcome(S& store, std::string_view session_id,
                                                                   quark::FenceToken fence,
                                                                   IdempotencyKey const& key,
                                                                   std::string tool_name,
                                                                   std::string call_id, bool ok) {
    auto const id = session_actor_id(session_id);
    quark::EventLog<EffectJournalEntry, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(EffectJournalEntry{key.to_string(), std::move(tool_name), std::move(call_id),
                                  effect_journal_phase::outcome, ok});
    return log.commit();
}

// The full journal for one session, in commit order — reuses `quark::replay_tail`'s own fold
// mechanism (event_log.hpp) rather than hand-decoding `EventRecord` bytes a second time.
template <quark::Store S>
[[nodiscard]] quark::result<std::vector<EffectJournalEntry>> read_effect_journal(
    S& store, std::string_view session_id) {
    std::vector<EffectJournalEntry> entries;
    auto last = quark::replay_tail<EffectJournalEntry>(
        store, session_actor_id(session_id), /*from=*/0, entries,
        [](std::vector<EffectJournalEntry>& acc, EffectJournalEntry const& ev) { acc.push_back(ev); });
    if (!last) return std::unexpected(last.error());
    return entries;
}

// "On resume, a journaled-but-unconfirmed effect is reconciled." A journaled intent with no
// matching outcome (same idempotency_key) — in commit order, so a caller reconciling them can
// process the oldest first. Read-only: this answers WHICH intents need a decision, never what the
// decision should be (F3's own, deliberately deferred, job).
template <quark::Store S>
[[nodiscard]] quark::result<std::vector<EffectJournalEntry>> unconfirmed_effect_intents(
    S& store, std::string_view session_id) {
    auto entries = read_effect_journal(store, session_id);
    if (!entries) return std::unexpected(entries.error());

    std::unordered_map<std::string, bool> has_outcome;
    for (auto const& e : *entries) {
        if (e.phase == effect_journal_phase::outcome) has_outcome[e.idempotency_key] = true;
    }

    std::vector<EffectJournalEntry> unconfirmed;
    for (auto const& e : *entries) {
        if (e.phase == effect_journal_phase::intent && !has_outcome.count(e.idempotency_key)) {
            unconfirmed.push_back(e);
        }
    }
    return unconfirmed;
}

} // namespace agentengine
