// Milestone 4 Phase F2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 019
// §3's "the intent to perform an effect is journaled before execution and the outcome journaled
// after. On resume, a journaled-but-unconfirmed effect is reconciled" had no implementation
// anywhere before this task. Proves the full intent-then-outcome sequence lands in commit order
// on the SAME Store/ActorId a session's own checkpoint already uses (Phase A4/D1, "no second
// persistence engine"), that a confirmed effect (intent + matching outcome) is never reported as
// unconfirmed, and that a genuinely interrupted effect (intent only, simulating a crash before the
// outcome could be journaled) IS correctly identified for reconciliation -- by idempotency key,
// never confusing two distinct effects with each other.

#include <iostream>
#include <string>

#include "quark/core/persistence.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_journal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

} // namespace

int main() {
    quark::InMemoryStore store;
    std::string const session_id = "s-journal";
    auto const id    = ae::session_actor_id(session_id);
    auto const fence = store.acquire_fence(id);

    ae::EffectContext ctx{};
    ctx.run_id     = "s-journal:run:1";
    ctx.turn_index = 0;

    auto args1 = *ae::json::parse(R"({"path":"/tmp/a"})");
    ae::IdempotencyKey const key1 = ae::derive_idempotency_key(ctx, /*call_index=*/0, args1);

    // --- Empty journal before anything happens --------------------------------------------------
    auto empty = ae::read_effect_journal(store, session_id);
    AE_CHECK(empty.has_value() && empty->empty(), "F2-C1: a fresh session has an empty effect journal");

    // --- Intent journaled BEFORE execution --------------------------------------------------------
    auto intent1 = ae::journal_effect_intent(store, session_id, fence, key1, "fs_write", "call-1");
    AE_CHECK(intent1.has_value(), "F2-R1: journaling an intent succeeds");

    auto after_intent = ae::read_effect_journal(store, session_id);
    AE_CHECK(after_intent.has_value() && after_intent->size() == 1 &&
                 (*after_intent)[0].phase == ae::effect_journal_phase::intent &&
                 (*after_intent)[0].idempotency_key == key1.to_string(),
             "F2-R2: the journal now has exactly one intent entry, carrying the real idempotency key");

    auto pending1 = ae::unconfirmed_effect_intents(store, session_id);
    AE_CHECK(pending1.has_value() && pending1->size() == 1,
             "F2-R3: with no outcome yet, the intent is correctly reported as unconfirmed -- "
             "'a journaled-but-unconfirmed effect' (019 §3), the exact reconciliation candidate");

    // --- Outcome journaled AFTER execution -- confirms the intent -------------------------------
    auto outcome1 = ae::journal_effect_outcome(store, session_id, fence, key1, "fs_write", "call-1", true);
    AE_CHECK(outcome1.has_value(), "F2-R4: journaling the matching outcome succeeds");

    auto after_outcome = ae::read_effect_journal(store, session_id);
    AE_CHECK(after_outcome.has_value() && after_outcome->size() == 2 &&
                 (*after_outcome)[0].phase == ae::effect_journal_phase::intent &&
                 (*after_outcome)[1].phase == ae::effect_journal_phase::outcome &&
                 (*after_outcome)[1].outcome_ok,
             "F2-R5: the journal now holds BOTH entries in commit order (intent then outcome), the "
             "outcome recording success");

    auto pending2 = ae::unconfirmed_effect_intents(store, session_id);
    AE_CHECK(pending2.has_value() && pending2->empty(),
             "F2-R6: a confirmed effect (intent + matching outcome) is never reported as "
             "unconfirmed once its outcome lands");

    // --- A SECOND, genuinely interrupted effect (intent only -- simulating a crash before the
    // outcome could be journaled) is correctly distinguished from the first (already-confirmed)
    // effect by idempotency key, never confused with it --------------------------------------------
    auto args2 = *ae::json::parse(R"({"path":"/tmp/b"})");
    ae::EffectContext ctx2{};
    ctx2.run_id     = "s-journal:run:1";
    ctx2.turn_index = 1;  // a later turn -- a genuinely different effect
    ae::IdempotencyKey const key2 = ae::derive_idempotency_key(ctx2, 0, args2);
    AE_CHECK(!(key1 == key2), "setup: the second effect's key differs from the first's");

    auto intent2 = ae::journal_effect_intent(store, session_id, fence, key2, "fs_write", "call-2");
    AE_CHECK(intent2.has_value(), "setup: journaling the second (interrupted) intent succeeds");

    auto pending3 = ae::unconfirmed_effect_intents(store, session_id);
    AE_CHECK(pending3.has_value() && pending3->size() == 1 &&
                 (*pending3)[0].idempotency_key == key2.to_string(),
             "F2-R7: only the genuinely-interrupted second effect is reported unconfirmed -- the "
             "already-confirmed first effect never reappears, distinguished purely by idempotency "
             "key, not by call order or count");

    // --- The journal rides the SAME Store/ActorId a session's own checkpoint uses -- reading the
    // session's own snapshot slot still works exactly as it always did, untouched by the journal
    // living in its (separate) event-log slot under the same id ------------------------------------
    auto session_snapshot = ae::load_agent_session_snapshot(store, session_id);
    AE_CHECK(session_snapshot.has_value() && !session_snapshot->has_value(),
             "F2-R8: the session's own snapshot slot is untouched (still 'never snapshotted') by "
             "everything written to its event-log slot -- 012's own separate-slots-per-ActorId "
             "guarantee, not asserted separately from it");

    std::cout << (g_failures == 0 ? "test_effect_journal: OK\n" : "test_effect_journal: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
