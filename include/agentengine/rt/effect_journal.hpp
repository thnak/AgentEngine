#pragma once
// ADR-037: agentengine::rt::EffectJournalEntry -- 019-Durability-and-Long-Running-Agents.md §3's
// "the intent to perform an effect is journaled before execution and the outcome journaled after.
// On resume, a journaled-but-unconfirmed effect is reconciled," ported off the old
// `core/effect_journal.hpp`'s `quark::Store`/`quark::EventLog<EffectJournalEntry,S>`/
// `quark::replay_tail` shape onto `rt::AppendLogStore` -- the same append-only-growth primitive
// this project already built and wired into `rt::WorkflowSupervisor`'s time-travel and
// `rt::ProjectRegistry`'s archived-member tail. This is a genuinely simpler port than either of
// those: there is no two-phase pending/committed discipline to preserve (an intent and its outcome
// are each a single, terminal fact the instant they're journaled -- the OLD file's own single
// `EventLog::stage()+commit()` call per write already made that clear), so this mirrors
// `project_archive.hpp`'s single-append shape.
//
// SAME STORE, DIFFERENT SLOT -- REWORKED, NOT PRESERVED LITERALLY. The old file's own header
// comment leaned on a Quark-specific guarantee: a session's Snapshot-model checkpoint and its
// EventSourced effect journal are "two DIFFERENT slots under the SAME Store/ActorId" -- true only
// because `quark::Store` serves both persistence models (Snapshot and EventSourced) through one
// interface keyed by one `ActorId`. `rt::SessionStore` (single-slot) and `rt::AppendLogStore`
// (append-only) are structurally DIFFERENT interfaces in `rt::` -- there is no single store type
// that serves both, so "same store" cannot be preserved literally. What DOES carry over, and is the
// actual substance of the old claim (nothing written to the journal ever collides with or corrupts
// the session's own checkpoint), is name-scoping both under the same `session_id`: the journal's own
// `LogId` embeds `session_id` (see `effect_journal_log_id` below), the same pattern
// `workflow_checkpoint_log_id`/`project_archive_log_id` already establish for their own owning ids.
// A host wiring both stores for one session (e.g. one `rt::SessionStore` instance and one
// `rt::AppendLogStore` instance, or even two logical regions of the same physical backing store) gets
// the identical non-collision property the original had, just via two store instances instead of one
// shared one.
//
// F3 (019 §7 G6's "surface indeterminate, never guess" claim) is deliberately NOT built here, same as
// the original -- `unconfirmed_effect_intents` below answers only "which journaled intents have no
// matching outcome yet" (a read, not a decision); deciding what to DO about one is F3's own,
// separately-scoped, design->red-team->prove->judge work.

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // IdempotencyKey
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine::rt {

enum class effect_journal_phase { intent, outcome };

// All scalars/strings -- no variant, no serialization gap, same as the original.
struct EffectJournalEntry {
    std::string           idempotency_key;  // IdempotencyKey::to_string() -- the dedup identity
    std::string           tool_name;
    std::string           call_id;
    effect_journal_phase  phase = effect_journal_phase::intent;
    bool                   outcome_ok = false;  // only meaningful when phase == outcome

    friend bool operator==(EffectJournalEntry const&, EffectJournalEntry const&) = default;
};

[[nodiscard]] inline agentengine::json::Value effect_journal_entry_to_json(
    EffectJournalEntry const& e) {
    return agentengine::json::Value::make_object({
        {"idempotency_key", agentengine::json::Value::make_string(e.idempotency_key)},
        {"tool_name", agentengine::json::Value::make_string(e.tool_name)},
        {"call_id", agentengine::json::Value::make_string(e.call_id)},
        {"phase", agentengine::json::Value::make_string(
                       e.phase == effect_journal_phase::intent ? "intent" : "outcome")},
        {"outcome_ok", agentengine::json::Value::make_bool(e.outcome_ok)},
    });
}

[[nodiscard]] inline agentengine::result<EffectJournalEntry> effect_journal_entry_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* key_v     = v.find("idempotency_key");
    agentengine::json::Value const* tool_v    = v.find("tool_name");
    agentengine::json::Value const* call_v    = v.find("call_id");
    agentengine::json::Value const* phase_v   = v.find("phase");
    agentengine::json::Value const* outcome_v = v.find("outcome_ok");
    if (key_v == nullptr || !key_v->is_string() || tool_v == nullptr || !tool_v->is_string() ||
        call_v == nullptr || !call_v->is_string() || phase_v == nullptr || !phase_v->is_string() ||
        outcome_v == nullptr || !outcome_v->is_bool()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed EffectJournalEntry",
                                                    "rt.effect_journal.entry.malformed"});
    }
    std::string const& phase_str = phase_v->as_string();
    if (phase_str != "intent" && phase_str != "outcome") {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "unknown effect_journal_phase: " + phase_str,
                                                    "rt.effect_journal.entry.malformed"});
    }
    EffectJournalEntry out;
    out.idempotency_key = key_v->as_string();
    out.tool_name         = tool_v->as_string();
    out.call_id            = call_v->as_string();
    out.phase                = phase_str == "intent" ? effect_journal_phase::intent
                                                        : effect_journal_phase::outcome;
    out.outcome_ok            = outcome_v->as_bool();
    return out;
}

// See file banner for why this is name-scoped by session_id rather than literally sharing a Store
// slot with the session's own checkpoint.
[[nodiscard]] inline LogId effect_journal_log_id(std::string_view session_id) {
    return std::string(session_id) + ":effect_journal";
}

namespace effect_journal_detail {
template <AppendLogStore StoreT>
[[nodiscard]] result<void> append_entry(StoreT& store, std::string_view session_id,
                                          EffectJournalEntry const& entry) {
    std::string const text = agentengine::json::dump(effect_journal_entry_to_json(entry));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(effect_journal_log_id(session_id), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());
    return {};
}
}  // namespace effect_journal_detail

// "The intent to perform an effect is journaled before execution."
template <AppendLogStore StoreT>
[[nodiscard]] result<void> journal_effect_intent(StoreT& store, std::string_view session_id,
                                                  IdempotencyKey const& key, std::string tool_name,
                                                  std::string call_id) {
    return effect_journal_detail::append_entry(
        store, session_id,
        EffectJournalEntry{key.to_string(), std::move(tool_name), std::move(call_id),
                            effect_journal_phase::intent, false});
}

// "...and the outcome journaled after." Same log, same key -- `ok` is the effect's own success/
// failure, not this journal write's (a failed append surfaces as this function's own error result).
template <AppendLogStore StoreT>
[[nodiscard]] result<void> journal_effect_outcome(StoreT& store, std::string_view session_id,
                                                   IdempotencyKey const& key, std::string tool_name,
                                                   std::string call_id, bool ok) {
    return effect_journal_detail::append_entry(
        store, session_id,
        EffectJournalEntry{key.to_string(), std::move(tool_name), std::move(call_id),
                            effect_journal_phase::outcome, ok});
}

// The full journal for one session, in commit order.
template <AppendLogStore StoreT>
[[nodiscard]] result<std::vector<EffectJournalEntry>> read_effect_journal(StoreT const& store,
                                                                            std::string_view session_id) {
    auto raw = store.read_from(effect_journal_log_id(session_id), 0);
    if (!raw) return std::unexpected(raw.error());

    std::vector<EffectJournalEntry> entries;
    entries.reserve(raw->size());
    for (std::vector<std::byte> const& bytes : *raw) {
        std::string text;
        text.reserve(bytes.size());
        for (std::byte b : bytes) text.push_back(static_cast<char>(b));
        auto parsed = agentengine::json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto entry = effect_journal_entry_from_json(*parsed);
        if (!entry) return std::unexpected(entry.error());
        entries.push_back(std::move(*entry));
    }
    return entries;
}

// "On resume, a journaled-but-unconfirmed effect is reconciled." A journaled intent with no
// matching outcome (same idempotency_key) -- in commit order. Read-only: answers WHICH intents
// need a decision, never what the decision should be (F3's own, deferred, job).
template <AppendLogStore StoreT>
[[nodiscard]] result<std::vector<EffectJournalEntry>> unconfirmed_effect_intents(
    StoreT const& store, std::string_view session_id) {
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

}  // namespace agentengine::rt
