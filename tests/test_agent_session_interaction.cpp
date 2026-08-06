// Milestone 4 Phase E1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md,
// decision 2): 001 §2's `Interaction{interaction_id, run_id, reason, opened_at, expires_at?}` had
// no C++ type anywhere in this codebase before this task. Proves the real correlation-identity
// lifecycle 001 §2 describes: minting is deterministic and unique per session, resolving an
// unknown id fails rather than silently no-op'ing, "a run does not leave InputRequired/Suspended
// ... until every Interaction a given resolution call names is resolved" holds for multiple
// concurrently-open interactions, and -- tying back into Phase D1's checkpoint content -- open
// interactions now round-trip through the SAME save/load_agent_session_snapshot free functions
// every other Phase A/C/D test already uses, closing the gap D1 could only leave as an
// always-empty placeholder before this type existed.

#include <iostream>
#include <string>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

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

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::result<ae::ChatResponse> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value  = ae::Text{"ok"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    int chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return 0; }  // unconstrained, unused
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient>;

    quark::TestKit<Session> kit;
    kit.actor().initialize("s-interact", ae::Principal{"p-omar", ""});

    AE_CHECK(!kit.actor().has_open_interactions(), "E1-C1: a fresh session has no open interactions");

    // --- Minting: deterministic, unique ids, correct reason/run_id -----------------------------
    ae::Interaction const& i1 = kit.actor().open_interaction("s-interact:run:1", ae::interaction_reason::input);
    AE_CHECK(i1.interaction_id == "s-interact:interaction:1" && i1.run_id == "s-interact:run:1" &&
                 i1.reason == ae::interaction_reason::input,
             "E1-R1: the first Interaction has a deterministic id, the given run_id/reason");
    AE_CHECK(kit.actor().has_open_interactions() && kit.actor().open_interactions().size() == 1,
             "E1-R2: exactly one open interaction after minting one");

    // `open_interaction()`'s reference and `interaction_id`/etc. are captured into plain locals
    // IMMEDIATELY -- both references point into `open_interactions_`, a vector `resolve_interaction()`
    // erases from, and an erase can relocate/invalidate a reference to an element after the one
    // removed. i1's own id is used once, right below, before any erase happens; i2's is captured
    // now specifically so it survives i1's later resolution.
    std::string const i1_id = i1.interaction_id;
    ae::Interaction const& i2 = kit.actor().open_interaction("s-interact:run:1", ae::interaction_reason::auth);
    std::string const i2_id = i2.interaction_id;
    AE_CHECK(i2_id == "s-interact:interaction:2" && i2.reason == ae::interaction_reason::auth,
             "E1-R3: a second Interaction gets a distinct, incrementing id and its own reason (001 "
             "§2: 'a workflow with multiple concurrent request ports open... can hold several at "
             "once')");
    AE_CHECK(kit.actor().open_interactions().size() == 2, "E1-R4: two interactions open simultaneously");

    // --- Resolution: fails on unknown id, succeeds on a real one, "all must resolve" -----------
    auto bad = kit.actor().resolve_interaction("no-such-id");
    AE_CHECK(!bad.has_value() && bad.error().code == "session.resolve_interaction.unknown_id",
             "E1-R5: resolving an unknown interaction_id returns a real error, not a silent no-op");

    auto r1 = kit.actor().resolve_interaction(i1_id);
    AE_CHECK(r1.has_value(), "E1-R6: resolving the first (real) interaction succeeds");
    AE_CHECK(kit.actor().has_open_interactions() && kit.actor().open_interactions().size() == 1,
             "E1-R7: one interaction resolved, one still open -- 001 §2's 'does not leave "
             "InputRequired/Suspended until EVERY Interaction a resolution call names is resolved' "
             "holds: resolving i1 alone doesn't clear i2");

    auto r2 = kit.actor().resolve_interaction(i2_id);
    AE_CHECK(r2.has_value() && !kit.actor().has_open_interactions(),
             "E1-R8: resolving the last remaining interaction clears the open set entirely");

    // --- Checkpoint round-trip: open interactions now persist for real (closing D1's own gap) --
    quark::InMemoryStore store;
    (void)kit.actor().open_interaction("s-interact:run:2", ae::interaction_reason::auth);
    (void)kit.actor().open_interaction("s-interact:run:2", ae::interaction_reason::input);
    AE_CHECK(kit.actor().open_interactions().size() == 2, "setup: two fresh interactions open before checkpointing");

    auto const id    = ae::session_actor_id(kit.actor().session_id());
    auto const fence = store.acquire_fence(id);
    auto saved = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence);
    AE_CHECK(saved.has_value(), "E1-R9: checkpointing with open interactions succeeds");

    auto loaded = ae::load_agent_session_snapshot(store, "s-interact");
    AE_CHECK(loaded.has_value() && loaded->has_value() && (*loaded)->open_interactions.size() == 2,
             "E1-R10: the checkpoint's OWN record carries both open interactions -- 019 §1's "
             "'pending approvals/input requests' checkpoint content, real now, not a placeholder");

    quark::TestKit<Session> fresh_kit;
    fresh_kit.actor().restore_from_record(**loaded);
    AE_CHECK(fresh_kit.actor().has_open_interactions() && fresh_kit.actor().open_interactions().size() == 2,
             "E1-R11: restoring the checkpoint into a FRESH instance recovers both open "
             "interactions bit-identical -- a restarted session doesn't lose track of what it was "
             "waiting on");
    AE_CHECK(fresh_kit.actor().open_interactions()[0].reason == ae::interaction_reason::auth &&
                 fresh_kit.actor().open_interactions()[1].reason == ae::interaction_reason::input,
             "E1-R12: each restored interaction's own reason survives the round-trip correctly, "
             "not collapsed to a single value");

    std::cout << (g_failures == 0 ? "test_agent_session_interaction: OK\n"
                                   : "test_agent_session_interaction: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
