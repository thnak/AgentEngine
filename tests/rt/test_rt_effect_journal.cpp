// Proof for ADR-037: agentengine::rt::EffectJournalEntry (include/agentengine/rt/effect_journal.hpp)
// -- 019 §3's "the intent to perform an effect is journaled before execution and the outcome
// journaled after. On resume, a journaled-but-unconfirmed effect is reconciled," ported off the old
// core/effect_journal.hpp's quark::Store/EventLog shape onto rt::AppendLogStore. Deterministic,
// offline, single-threaded. Ports the old test_effect_journal.cpp's own F2-R1..R7 claims
// (intent-then-outcome sequencing, reconciliation by idempotency key, never confusing two distinct
// effects); F2-R8 ("rides the SAME Store/ActorId a session's own checkpoint uses") does not carry
// over literally -- rt::SessionStore and rt::AppendLogStore are structurally different interfaces,
// see effect_journal.hpp's own banner -- and is REPLACED here with J8, a genuinely different but
// analogous claim: name-scoping by session_id means two DIFFERENT sessions' journals, held in the
// SAME AppendLogStore instance, never collide or leak into each other.
//   J1 -- a fresh session has an empty effect journal.
//   J2 -- journaling an intent succeeds; the journal holds exactly one intent entry with the real key.
//   J3 -- with no outcome yet, the intent is correctly reported as unconfirmed.
//   J4 -- journaling the matching outcome succeeds.
//   J5 -- the journal holds BOTH entries in commit order (intent then outcome), outcome recording
//         success.
//   J6 -- a confirmed effect (intent + matching outcome) is never reported as unconfirmed.
//   J7 -- a SECOND, genuinely interrupted effect (intent only) is correctly distinguished from the
//         first (already-confirmed) effect by idempotency key.
//   J8 -- two different sessions' journals, same AppendLogStore instance, never collide.
//   J9..J18 (#51) -- the fixed little-endian binary record layout: round-trip, and decode hardening
//         (every truncation, length past end, oversized length, bad phase/outcome/version/magic
//         byte, trailing bytes, pre-#51 JSON record refused with its own code, encode/decode field
//         limit symmetry, arbitrary bytes round-trip).

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/effect_journal.hpp"

using agentengine::EffectContext;
using agentengine::IdempotencyKey;
using agentengine::derive_idempotency_key;
using agentengine::rt::EffectJournalEntry;
using agentengine::rt::InMemoryAppendLogStore;
using agentengine::rt::decode_effect_journal_entry;
using agentengine::rt::encode_effect_journal_entry;
using agentengine::rt::effect_journal_phase;
using agentengine::rt::journal_effect_intent;
using agentengine::rt::journal_effect_outcome;
using agentengine::rt::read_effect_journal;
using agentengine::rt::unconfirmed_effect_intents;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    InMemoryAppendLogStore store;
    std::string const session_id = "s-journal";

    EffectContext ctx{};
    ctx.run_id     = "s-journal:run:1";
    ctx.turn_index = 0;

    auto args1 = *agentengine::json::parse(R"({"path":"/tmp/a"})");
    IdempotencyKey const key1 = derive_idempotency_key(ctx, /*call_index=*/0, args1);

    // ---- J1: empty journal before anything happens -------------------------------------------------
    auto empty = read_effect_journal(store, session_id);
    check(empty.has_value() && empty->empty(), "J1: a fresh session has an empty effect journal");

    // ---- J2: intent journaled BEFORE execution -------------------------------------------------------
    auto intent1 = journal_effect_intent(store, session_id, key1, "fs_write", "call-1");
    check(intent1.has_value(), "J2: journaling an intent succeeds");

    auto after_intent = read_effect_journal(store, session_id);
    check(after_intent.has_value() && after_intent->size() == 1 &&
              (*after_intent)[0].phase == effect_journal_phase::intent &&
              (*after_intent)[0].idempotency_key == key1.to_string(),
          "J2: the journal now has exactly one intent entry, carrying the real idempotency key");

    // ---- J3: unconfirmed while no outcome exists yet -------------------------------------------------
    auto pending1 = unconfirmed_effect_intents(store, session_id);
    check(pending1.has_value() && pending1->size() == 1,
          "J3: with no outcome yet, the intent is correctly reported as unconfirmed -- 'a "
          "journaled-but-unconfirmed effect' (019 §3), the exact reconciliation candidate");

    // ---- J4/J5: outcome journaled AFTER execution -- confirms the intent -----------------------------
    auto outcome1 = journal_effect_outcome(store, session_id, key1, "fs_write", "call-1", true);
    check(outcome1.has_value(), "J4: journaling the matching outcome succeeds");

    auto after_outcome = read_effect_journal(store, session_id);
    check(after_outcome.has_value() && after_outcome->size() == 2 &&
              (*after_outcome)[0].phase == effect_journal_phase::intent &&
              (*after_outcome)[1].phase == effect_journal_phase::outcome &&
              (*after_outcome)[1].outcome_ok,
          "J5: the journal now holds BOTH entries in commit order (intent then outcome), the outcome "
          "recording success");

    // ---- J6: a confirmed effect is never reported as unconfirmed ------------------------------------
    auto pending2 = unconfirmed_effect_intents(store, session_id);
    check(pending2.has_value() && pending2->empty(),
          "J6: a confirmed effect (intent + matching outcome) is never reported as unconfirmed once "
          "its outcome lands");

    // ---- J7: a SECOND, genuinely interrupted effect is distinguished by idempotency key -------------
    auto args2 = *agentengine::json::parse(R"({"path":"/tmp/b"})");
    EffectContext ctx2{};
    ctx2.run_id     = "s-journal:run:1";
    ctx2.turn_index = 1;  // a later turn -- a genuinely different effect
    IdempotencyKey const key2 = derive_idempotency_key(ctx2, 0, args2);
    check(!(key1 == key2), "setup: the second effect's key differs from the first's");

    auto intent2 = journal_effect_intent(store, session_id, key2, "fs_write", "call-2");
    check(intent2.has_value(), "setup: journaling the second (interrupted) intent succeeds");

    auto pending3 = unconfirmed_effect_intents(store, session_id);
    check(pending3.has_value() && pending3->size() == 1 &&
              (*pending3)[0].idempotency_key == key2.to_string(),
          "J7: only the genuinely-interrupted second effect is reported unconfirmed -- the "
          "already-confirmed first effect never reappears, distinguished purely by idempotency key, "
          "not by call order or count");

    // ---- J8: two different sessions' journals, same store instance, never collide -------------------
    std::string const other_session_id = "s-journal-other";
    auto other_empty = read_effect_journal(store, other_session_id);
    check(other_empty.has_value() && other_empty->empty(),
          "J8: a DIFFERENT session_id's journal, in the SAME AppendLogStore instance, starts empty "
          "-- 's-journal's own two entries above did not leak into it");

    auto other_intent = journal_effect_intent(store, other_session_id, key1, "fs_write", "call-x");
    check(other_intent.has_value(), "J8: journaling into the other session's own journal succeeds");

    auto s_journal_unchanged = read_effect_journal(store, session_id);
    check(s_journal_unchanged.has_value() && s_journal_unchanged->size() == 3,
          "J8: writing to the OTHER session's journal leaves 's-journal's own journal at its prior "
          "size (3: intent1, outcome1, intent2 from J2/J4/J7) -- name-scoping by session_id "
          "genuinely isolates the two");

    // ---- J9..J16 (#51): the binary record layout and its decode hardening ---------------------------
    // The journal is read on crash-resume, so a torn/truncated/garbage record must be a clean error,
    // never a crash or out-of-bounds read.
    EffectJournalEntry const sample{"key-1", "fs_write", "call-9", effect_journal_phase::outcome, true};
    auto encoded = encode_effect_journal_entry(sample);
    check(encoded.has_value() && !encoded->empty() && (*encoded)[0] == std::byte{0xAE} &&
              (*encoded)[1] == std::byte{1},
          "J9: an entry encodes to the binary layout (magic 0xAE, version 1), not JSON");
    auto decoded = decode_effect_journal_entry(*encoded);
    check(decoded.has_value() && *decoded == sample, "J9: encode -> decode round-trips exactly");

    // J10: every proper prefix of a valid record (a torn write at any byte) is refused cleanly.
    bool every_prefix_refused = true;
    for (std::size_t n = 0; n < encoded->size(); ++n) {
        auto r = decode_effect_journal_entry(std::span<std::byte const>(encoded->data(), n));
        if (r.has_value() || r.error().code != "rt.effect_journal.entry.malformed") {
            every_prefix_refused = false;
        }
    }
    check(every_prefix_refused,
          "J10: every truncation of a valid record (all proper prefixes) is refused as malformed");

    // J11: a length prefix that runs past the end of the record.
    {
        auto bad = *encoded;
        bad[4] = std::byte{0xFF};  // idempotency_key length low byte: 255 > bytes remaining
        auto r = decode_effect_journal_entry(bad);
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.malformed",
              "J11: a string length past the end of the record is refused");
    }
    // J12: an oversized length (garbage 0xFFFFFFFF) is refused before any allocation.
    {
        auto bad = *encoded;
        for (int i = 4; i < 8; ++i) bad[static_cast<std::size_t>(i)] = std::byte{0xFF};
        auto r = decode_effect_journal_entry(bad);
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.malformed",
              "J12: a 4 GiB length prefix is refused (field limit), not allocated");
    }
    // J13: a bad phase byte, and an intent claiming outcome_ok.
    {
        auto bad = *encoded;
        bad[2] = std::byte{2};
        auto r = decode_effect_journal_entry(bad);
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.malformed",
              "J13: an unknown phase byte (2) is refused");
        auto bad2 = *encoded;
        bad2[2] = std::byte{0};  // intent, but outcome_ok byte is still 1
        auto r2 = decode_effect_journal_entry(bad2);
        check(!r2.has_value(), "J13: an intent record carrying outcome_ok=1 is refused");
        auto bad3 = *encoded;
        bad3[3] = std::byte{7};
        check(!decode_effect_journal_entry(bad3).has_value(),
              "J13: an outcome_ok byte other than 0/1 is refused");
    }
    // J14: a bad version byte and a bad magic byte each get a clear error.
    {
        auto bad = *encoded;
        bad[1] = std::byte{2};
        auto r = decode_effect_journal_entry(bad);
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.unsupported_version",
              "J14: an unknown format version is refused with its own code");
        auto bad2 = *encoded;
        bad2[0] = std::byte{0x00};
        auto r2 = decode_effect_journal_entry(bad2);
        check(!r2.has_value() && r2.error().code == "rt.effect_journal.entry.malformed",
              "J14: a bad magic byte is refused");
    }
    // J15: trailing bytes after the last field are refused (a record is exactly one entry).
    {
        auto bad = *encoded;
        bad.push_back(std::byte{0});
        check(!decode_effect_journal_entry(bad).has_value(),
              "J15: trailing bytes after call_id are refused");
    }
    // J16: a pre-#51 JSON record is refused with its own code, and through read_effect_journal too.
    {
        InMemoryAppendLogStore legacy_store;
        std::string const json =
            R"({"call_id":"c","idempotency_key":"k","outcome_ok":false,"phase":"intent","tool_name":"t"})";
        std::vector<std::byte> legacy;
        for (char c : json) legacy.push_back(static_cast<std::byte>(c));
        (void)legacy_store.append(agentengine::rt::effect_journal_log_id("s-legacy"), std::move(legacy));
        auto r = read_effect_journal(legacy_store, "s-legacy");
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.legacy_json_format",
              "J16: an old JSON-format journal is refused clearly (legacy_json_format), not misread");
    }
    // J17: the writer refuses a field the reader would refuse (symmetry), and a field at the limit
    // still round-trips.
    {
        EffectJournalEntry big = sample;
        big.call_id.assign(agentengine::rt::effect_journal_max_field_bytes + 1, 'x');
        auto r = encode_effect_journal_entry(big);
        check(!r.has_value() && r.error().code == "rt.effect_journal.entry.field_too_large",
              "J17: an over-limit field is refused at encode time");
        big.call_id.assign(agentengine::rt::effect_journal_max_field_bytes, 'x');
        auto ok = encode_effect_journal_entry(big);
        check(ok.has_value() && decode_effect_journal_entry(*ok).has_value() &&
                  *decode_effect_journal_entry(*ok) == big,
              "J17: a field exactly at the limit round-trips");
    }
    // J18: arbitrary-byte strings (embedded NUL, quotes, non-UTF-8) round-trip byte-exactly --
    // no escaping layer to get wrong.
    {
        EffectJournalEntry odd{std::string("k\0\"\\\xff", 5), "", "c\n", effect_journal_phase::intent,
                               false};
        auto enc = encode_effect_journal_entry(odd);
        check(enc.has_value() && decode_effect_journal_entry(*enc).has_value() &&
                  *decode_effect_journal_entry(*enc) == odd,
              "J18: strings with NUL/quote/backslash/0xFF and an empty string round-trip exactly");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "All checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
