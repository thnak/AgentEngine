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

#include <cstdio>
#include <string>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/effect_journal.hpp"

using agentengine::EffectContext;
using agentengine::IdempotencyKey;
using agentengine::derive_idempotency_key;
using agentengine::rt::InMemoryAppendLogStore;
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

    if (g_failures == 0) {
        std::fprintf(stderr, "All checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
