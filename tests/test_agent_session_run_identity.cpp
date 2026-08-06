// Milestone 4 Phase A3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 001
// §1/§2 name `Run` ("An Ask<StartRun, RunResponse> to the session actor") and `Turn` ("a segment of
// a run's coroutine between model calls") only in prose — no C++ identity existed for either
// before this task (verified: EffectContext had no run_id/turn_index field at all). 019 §3's
// idempotency-key derivation `{run_id, turn_index, call_index, argument_digest}` needs the first
// two to be real; this proves they are: deterministic per session, distinct across sessions, and
// actually delivered to the ChatClient through the SAME EffectContext every effect already
// carries (never a parallel identity parameter nobody wires through).

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

// Reports back exactly the run_id/turn_index it was actually called with, embedded in the reply
// text — proves the EffectContext AgentSession passes to chat() carries real identity, without
// needing a mutable static the test would have to reset between cases.
class RecordingChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::result<ae::ChatResponse> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value = ae::Text{"run=" + ctx.run_id + " turn=" + std::to_string(ctx.turn_index)};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    int chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return 0; }  // unconstrained, unused
};
static_assert(ae::ChatClient<RecordingChatClient>,
              "RecordingChatClient must satisfy the ChatClient concept (004 §1)");

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

[[nodiscard]] std::string reply_text(ae::AgentResponse const& r) {
    return std::get<ae::Text>(r.message.content.front().value).text;
}

} // namespace

int main() {
    using Session = ae::AgentSession<RecordingChatClient>;

    quark::TestKit<Session> kit;
    kit.actor().initialize("s-run", ae::Principal{"p-dave", ""});

    // --- run_id is real, minted per StartRun, and actually reaches the ChatClient's EffectContext
    auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("first", "m-1")});
    AE_CHECK(r1.has_value(), "A3-R1: first run against the mock succeeds");
    AE_CHECK(r1.has_value() && reply_text(*r1) == "run=s-run:run:1 turn=0",
             "A3-R2: the ChatClient observes the real run_id (\"s-run:run:1\") and turn_index (0) "
             "through EffectContext, not a parallel/unwired identity");
    AE_CHECK(kit.actor().last_run_id() == "s-run:run:1",
             "A3-R3: last_run_id() matches what the ChatClient actually received");

    // --- A second StartRun on the SAME session mints a NEW, distinct run_id (each StartRun is its
    // own run, 001 §1) — deterministic from the session's own monotonic counter, not wall-clock.
    auto r2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("second", "m-2")});
    AE_CHECK(r2.has_value() && reply_text(*r2) == "run=s-run:run:2 turn=0",
             "A3-R4: a second StartRun on the same session mints a new, incremented run_id");
    AE_CHECK(kit.actor().last_run_id() == "s-run:run:2",
             "A3-R5: last_run_id() advances with each new run");

    // --- Two different sessions never mint the same run_id for their respective first runs, even
    // though both start their own counter at 1 — the session_id prefix is what keeps them apart.
    quark::TestKit<Session> kit_other;
    kit_other.actor().initialize("s-run-other", ae::Principal{"p-erin", ""});
    auto r_other = kit_other.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-o1")});
    AE_CHECK(r_other.has_value() && reply_text(*r_other) == "run=s-run-other:run:1 turn=0",
             "A3-R6: a different session's first run has a distinct run_id despite an identical "
             "counter value, because the session_id prefix differs");

    std::cout << (g_failures == 0 ? "test_agent_session_run_identity: OK\n"
                                   : "test_agent_session_run_identity: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
