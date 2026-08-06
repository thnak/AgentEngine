// Milestone 4 Phase A2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// `AgentSession` had no `state`/`metadata` fields at all before this task (005 §1's data model
// names both; verified absent by direct inspection before this task started). 005 §8 Q1 resolves
// `state` as a DECLARED C++ TYPE per agent, not an untyped bag — proven here by actually declaring
// a non-default `StateT`, mutating it across turns, and checking two sessions never share it. Not
// proven here: durability (019, this milestone's own Phase D/A4) — this is the in-process half of
// "checkpointed with the session" (005 §1): the state member is stable and mutable across the
// SAME actor instance's turns, which is the precondition durability builds on, not durability
// itself.

#include <iostream>
#include <string>

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

// A fixed canned reply is enough here — this test is about `state`/`metadata`, not about the
// content the ChatClient produces (test_agent_session_isolation.cpp already covers that).
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

// A declared, agent-specific scratch-state type (005 §8 Q1) — deliberately NOT `NoSessionState`,
// to prove `AgentSession<ChatClientT, StateT>` actually carries a real, non-default type through,
// with fields an author's own agent logic would plausibly accumulate into across turns.
struct TurnCounterState {
    int         turns_seen = 0;
    std::string last_note;
};

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient, TurnCounterState>;

    // --- Default-constructed state starts at the type's own defaults, not garbage/uninitialized --
    quark::TestKit<Session> kit;
    AE_CHECK(kit.actor().state().turns_seen == 0 && kit.actor().state().last_note.empty(),
             "A2-C1: a fresh session's typed state starts at StateT's own defaults");

    // --- State is mutable in place and persists across turns within the same session (the
    // in-process half of "checkpointed with the session," 005 §1) ------------------------------
    kit.actor().initialize("s-state-1", ae::Principal{"p-carol", ""});
    kit.actor().state().turns_seen += 1;
    kit.actor().state().last_note = "first turn";
    auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")});
    AE_CHECK(r1.has_value(), "A2-R1: turn against the mock succeeds");
    AE_CHECK(kit.actor().state().turns_seen == 1 && kit.actor().state().last_note == "first turn",
             "A2-R2: state written before a turn is still there, unmodified by handle(), after it "
             "(handle() doesn't touch state at this milestone's scope — an agent author's own turn "
             "logic would be the one mutating it in a later milestone)");

    kit.actor().state().turns_seen += 1;
    kit.actor().state().last_note = "second turn";
    auto r2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi again", "m-2")});
    AE_CHECK(r2.has_value() && kit.actor().state().turns_seen == 2 &&
                 kit.actor().state().last_note == "second turn",
             "A2-R3: state accumulates across multiple turns on the same session");

    // --- metadata: a free-form string bag (005 §1), set/get round-trip -------------------------
    kit.actor().metadata()["client_tag"] = "mobile-app-v3";
    AE_CHECK(kit.actor().metadata().at("client_tag") == "mobile-app-v3",
             "A2-R4: metadata round-trips a caller-supplied key/value");

    // --- Two sessions never share state or metadata (the same isolation guarantee A1 proved for
    // history, extended to the two new fields) ---------------------------------------------------
    quark::TestKit<Session> kit_a;
    quark::TestKit<Session> kit_b;
    kit_a.actor().initialize("s-state-A", ae::Principal{"p-a", ""});
    kit_b.actor().initialize("s-state-B", ae::Principal{"p-b", ""});

    kit_a.actor().state().turns_seen = 5;
    kit_a.actor().metadata()["k"]     = "a-value";

    AE_CHECK(kit_b.actor().state().turns_seen == 0,
             "A2-R5: session B's state is untouched by session A's mutation (isolation)");
    AE_CHECK(kit_b.actor().metadata().find("k") == kit_b.actor().metadata().end(),
             "A2-R6: session B's metadata is untouched by session A's mutation (isolation)");

    std::cout << (g_failures == 0 ? "test_agent_session_state: OK\n"
                                   : "test_agent_session_state: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
