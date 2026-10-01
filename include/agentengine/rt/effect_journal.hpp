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

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // IdempotencyKey
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow effect_journal_phase — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class effect_journal_phase { intent, outcome };

// All scalars/strings -- no variant, no serialization gap, same as the original.
// ae-naming-lint: allow EffectJournalEntry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct EffectJournalEntry {
    std::string           idempotency_key;  // IdempotencyKey::to_string() -- the dedup identity
    std::string           tool_name;
    std::string           call_id;
    effect_journal_phase  phase = effect_journal_phase::intent;
    bool                   outcome_ok = false;  // only meaningful when phase == outcome

    friend bool operator==(EffectJournalEntry const&, EffectJournalEntry const&) = default;
};

// ---- Record layout (#51) ------------------------------------------------------------------------
// Each journal record is a fixed little-endian, length-prefixed byte layout -- no JSON, no scanner,
// no escaping, no Value tree built and thrown away per entry. Nothing outside this header ever reads
// the journal, so there is no external format to honour. The layout (version 1):
//
//   offset  size  field
//   0       1     magic   0xAE  (never '{', so a pre-#51 JSON record is told apart, see below)
//   1       1     version 0x01
//   2       1     phase   0 = intent, 1 = outcome
//   3       1     outcome_ok 0/1 (always 0 for an intent)
//   4       4     u32 LE length of idempotency_key, then that many bytes
//   ..      4     u32 LE length of tool_name, then that many bytes
//   ..      4     u32 LE length of call_id, then that many bytes
//   (end)         the record ends exactly here; trailing bytes are rejected
//
// Decoding is a bounded sequential read: every length is checked against the bytes that remain
// AND against `effect_journal_max_field_bytes` before anything is copied, so a torn, truncated or
// garbage record (this is read on crash-resume) is a clean `rt.effect_journal.entry.*` error,
// never an out-of-bounds read or a huge allocation.
//
// PRE-#51 RECORDS ARE REFUSED, NOT MIGRATED. 024 §2 promises persisted formats stay readable
// across upgrades only from 1.0 on ("Pre-1.0: everything may break"); this engine is pre-1.0, and
// no production code path writes this journal yet (only tests call `journal_effect_*`, see
// ADR-181's §0 correction). A record that starts with '{' gets its own error code,
// `rt.effect_journal.entry.legacy_json_format`, so an operator holding such a journal sees exactly
// why it no longer reads instead of a generic "malformed". Bump `effect_journal_format_version` and
// add a decode branch (024 §2's forward migration) for any future layout change.
inline constexpr std::byte     effect_journal_magic{0xAE};
inline constexpr std::uint8_t  effect_journal_format_version = 1;
// Generous for an idempotency key / tool name / call id, small enough that a garbage length can
// never ask for a big allocation.
inline constexpr std::uint32_t effect_journal_max_field_bytes = 64u * 1024u;

namespace effect_journal_detail {
[[nodiscard]] inline error malformed(std::string message,
                                     std::string code = "rt.effect_journal.entry.malformed") {
    return error{failure_class::contract, std::move(message), std::move(code)};
}

inline void put_u32_le(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

// Reads sequentially from [pos, data.size()); every read is bounds-checked first.
struct reader {
    std::span<std::byte const> data;
    std::size_t                pos = 0;

    [[nodiscard]] std::size_t remaining() const noexcept { return data.size() - pos; }

    [[nodiscard]] result<std::uint8_t> u8(char const* what) {
        if (remaining() < 1) {
            return std::unexpected(malformed(std::string("truncated EffectJournalEntry: missing ") + what));
        }
        return static_cast<std::uint8_t>(data[pos++]);
    }

    [[nodiscard]] result<std::string> str(char const* what) {
        if (remaining() < 4) {
            return std::unexpected(
                malformed(std::string("truncated EffectJournalEntry: missing length of ") + what));
        }
        std::uint32_t len = 0;
        for (int i = 0; i < 4; ++i) {
            len |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[pos + i])) << (8 * i);
        }
        pos += 4;
        if (len > effect_journal_max_field_bytes) {
            return std::unexpected(malformed(std::string("EffectJournalEntry ") + what +
                                             " length exceeds the field limit"));
        }
        if (len > remaining()) {
            return std::unexpected(malformed(std::string("truncated EffectJournalEntry: ") + what +
                                             " length runs past the end of the record"));
        }
        std::string out(len, '\0');
        for (std::uint32_t i = 0; i < len; ++i) out[i] = static_cast<char>(data[pos + i]);
        pos += len;
        return out;
    }
};
}  // namespace effect_journal_detail

// Encodes one entry. Fails (contract) only if a string field exceeds
// `effect_journal_max_field_bytes` -- the writer refuses what the reader would refuse.
[[nodiscard]] inline result<std::vector<std::byte>> encode_effect_journal_entry(
    EffectJournalEntry const& e) {
    for (std::string const* field : {&e.idempotency_key, &e.tool_name, &e.call_id}) {
        if (field->size() > effect_journal_max_field_bytes) {
            return std::unexpected(effect_journal_detail::malformed(
                "EffectJournalEntry field exceeds the field limit",
                "rt.effect_journal.entry.field_too_large"));
        }
    }
    std::vector<std::byte> out;
    out.reserve(4 + 12 + e.idempotency_key.size() + e.tool_name.size() + e.call_id.size());
    out.push_back(effect_journal_magic);
    out.push_back(static_cast<std::byte>(effect_journal_format_version));
    out.push_back(static_cast<std::byte>(e.phase == effect_journal_phase::intent ? 0 : 1));
    out.push_back(static_cast<std::byte>(
        e.phase == effect_journal_phase::outcome && e.outcome_ok ? 1 : 0));
    for (std::string const* field : {&e.idempotency_key, &e.tool_name, &e.call_id}) {
        effect_journal_detail::put_u32_le(out, static_cast<std::uint32_t>(field->size()));
        for (char c : *field) out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// Decodes one record. Never reads out of bounds; every failure is a `contract` error with a
// stable `rt.effect_journal.entry.*` code.
[[nodiscard]] inline result<EffectJournalEntry> decode_effect_journal_entry(
    std::span<std::byte const> bytes) {
    using effect_journal_detail::malformed;
    if (bytes.empty()) return std::unexpected(malformed("empty EffectJournalEntry record"));
    if (bytes[0] != effect_journal_magic) {
        if (bytes[0] == std::byte{'{'}) {
            return std::unexpected(malformed(
                "EffectJournalEntry record is in the pre-#51 JSON format, which is no longer "
                "read; this journal was written by an older pre-1.0 engine and must be discarded",
                "rt.effect_journal.entry.legacy_json_format"));
        }
        return std::unexpected(malformed("EffectJournalEntry record has a bad magic byte"));
    }
    effect_journal_detail::reader r{bytes, 1};
    auto version = r.u8("version");
    if (!version) return std::unexpected(version.error());
    if (*version != effect_journal_format_version) {
        return std::unexpected(malformed("unsupported EffectJournalEntry format version " +
                                             std::to_string(*version),
                                         "rt.effect_journal.entry.unsupported_version"));
    }
    auto phase = r.u8("phase");
    if (!phase) return std::unexpected(phase.error());
    if (*phase > 1) return std::unexpected(malformed("unknown effect_journal_phase byte"));
    auto ok = r.u8("outcome_ok");
    if (!ok) return std::unexpected(ok.error());
    if (*ok > 1 || (*phase == 0 && *ok != 0)) {
        return std::unexpected(malformed("invalid EffectJournalEntry outcome_ok byte"));
    }
    EffectJournalEntry out;
    out.phase      = *phase == 0 ? effect_journal_phase::intent : effect_journal_phase::outcome;
    out.outcome_ok = *ok == 1;
    auto key = r.str("idempotency_key");
    if (!key) return std::unexpected(key.error());
    auto tool = r.str("tool_name");
    if (!tool) return std::unexpected(tool.error());
    auto call = r.str("call_id");
    if (!call) return std::unexpected(call.error());
    if (r.remaining() != 0) {
        return std::unexpected(malformed("EffectJournalEntry record has trailing bytes"));
    }
    out.idempotency_key = std::move(*key);
    out.tool_name       = std::move(*tool);
    out.call_id         = std::move(*call);
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
    auto bytes = encode_effect_journal_entry(entry);
    if (!bytes) return std::unexpected(bytes.error());
    auto appended = store.append(effect_journal_log_id(session_id), std::move(*bytes));
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
        auto entry = decode_effect_journal_entry(bytes);
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
