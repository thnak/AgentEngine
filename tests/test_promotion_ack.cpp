// Implements decisions/ADR-187-evaluation-harness.md E31 (round-4 fix for the FATAL "the approver
// acknowledges an excerpt, never the bytes the model reads" finding): the ack binds a digest of the
// verbatim rendered item, and the promotion path refuses to write unless the recomputed digest still
// matches. E31's own planted mutants: "ack recorded without a digest" and "promotion writes without
// recomputing" -- this test constructs both failure shapes directly against the real functions.

#include <iostream>
#include <string>

#include "agentengine/eval/promotion_ack.hpp"

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

}  // namespace

int main() {
    namespace ev = ae::eval;

    ev::LessonCandidate const candidate{"deploy-region", "default-region",
                                         "EU production region", "run-1/turn-3"};
    auto rendered = ev::render_lesson(candidate, "v1", 0.4f);
    AE_CHECK(rendered.has_value(), "candidate renders");
    if (!rendered) return 1;

    // ---- the happy path: ack what was rendered, then verify the same candidate promotes -----------
    auto ack = ev::acknowledge_rendered_lesson(*rendered, "v1", "approver-7", "2026-09-23T00:00:00Z");
    AE_CHECK(ack.has_value(), "acknowledge_rendered_lesson succeeds on a real rendered item");
    if (!ack) return 1;
    AE_CHECK(!ack->digest.empty(), "the ack carries a non-empty digest (E31's first mutant: no digest at all)");
    AE_CHECK(ack->approver_id == "approver-7" && ack->acknowledged_at == "2026-09-23T00:00:00Z",
             "the ack records who acknowledged it and when (I4)");

    auto promoted = ev::verify_and_render_acknowledged_lesson(candidate, *ack, 0.4f);
    AE_CHECK(promoted.has_value(), "promotion succeeds when the re-rendered item still matches the ack");
    AE_CHECK(promoted.has_value() && promoted->content == rendered->content,
             "the promoted item is byte-identical to what was acknowledged");

    // ---- E31's core claim: a candidate that changed AFTER the ack must be refused ------------------
    ev::LessonCandidate changed_value = candidate;
    changed_value.value = "US production region";
    auto refused_value_swap = ev::verify_and_render_acknowledged_lesson(changed_value, *ack, 0.4f);
    AE_CHECK(!refused_value_swap.has_value(),
             "a candidate whose value changed after the ack is refused, not silently promoted");

    // A salience swap between ack and write is exactly the TOCTOU shape round 4's F1 finding named
    // ("tags and salience are not in the record identity" for MemoryItem::id) -- prove it is caught
    // HERE even though it would NOT be caught by MemoryItem::id alone.
    auto refused_salience_swap = ev::verify_and_render_acknowledged_lesson(candidate, *ack, 0.9f);
    AE_CHECK(!refused_salience_swap.has_value(),
             "a salience swapped between ack and write is refused (round-4 F1: MemoryItem::id alone "
             "would NOT have caught this)");

    // ---- E31's second mutant, constructed directly: an ack with the RIGHT shape but a digest that
    // was never actually recomputed from the rendered bytes (e.g. a stale digest from an earlier
    // render, or a placeholder) must still be refused at promotion time.
    ev::PromotionAck stale_ack = *ack;
    stale_ack.digest = "0000000000000000000000000000000000000000000000000000000000000000";
    auto refused_stale_digest = ev::verify_and_render_acknowledged_lesson(candidate, stale_ack, 0.4f);
    AE_CHECK(!refused_stale_digest.has_value(),
             "a stale/placeholder digest that was never recomputed from the actual render is refused");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_promotion_ack: all checks passed\n";
    return 0;
}
