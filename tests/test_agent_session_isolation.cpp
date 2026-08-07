// Milestone 4 Phase A1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// `AgentSession::session_id_` was declared but never wired to anything (verified before this task
// started — no assignment site anywhere in include/ or tests/), so 001 §1's "one Quark actor
// instance, key = session_id" was not actually provable. This test proves the real property in two
// parts: (1) `session_actor_id()` is a pure, deterministic function of the session_id string —
// same id always maps to the same ActorId, regardless of which AgentSession instance carries that
// id, matching what a real store-backed re-activation would need to rely on; and (2) two
// AgentSessions with distinct session_ids never observe each other's history through a shared
// turn loop, proven with content that would visibly cross-contaminate if isolation were broken
// (an echoing mock, not a fixed canned reply — a fixed reply can't distinguish "isolated" from
// "silently sharing state").
//
// This is 001 §9 G1 "in miniature" (real isolation proof, two instances) — not the real gate
// (10^4 concurrent sessions), which is this milestone's own Phase H per the breakdown's decision 2.

#include <cassert>
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

// Echoes the last user message's text back, prefixed — unlike a fixed canned reply (as
// test_m1_walking_skeleton.cpp's HardcodedChatClient deliberately uses, correctly, for its own
// narrower purpose), this makes cross-session leakage *visible*: if session A's turn loop ever saw
// session B's history, the echoed text would show it.
class EchoChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const& request, ae::EffectContext&) {
        std::string last_text = "<no user text>";
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (std::holds_alternative<ae::Text>(item.value)) {
                last_text = std::get<ae::Text>(item.value).text;
            }
        }

        ae::ContentItem item{};
        item.value  = ae::Text{"echo:" + last_text};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);

        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<EchoChatClient>,
              "EchoChatClient must satisfy the ChatClient concept (004 §1)");

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
    using Session = ae::AgentSession<EchoChatClient>;

    // --- Part 1: session_actor_id is a pure, deterministic function of the id string ------------
    auto const id_a1 = ae::session_actor_id("s-A");
    auto const id_a2 = ae::session_actor_id("s-A");
    auto const id_b  = ae::session_actor_id("s-B");

    AE_CHECK(id_a1 == id_a2,
             "A1-C1: session_actor_id(\"s-A\") is deterministic across repeated calls");
    AE_CHECK(!(id_a1 == id_b),
             "A1-C2: distinct session_ids map to distinct ActorIds (\"s-A\" != \"s-B\")");
    AE_CHECK(id_a1.type == ae::kAgentSessionTypeKey && id_b.type == ae::kAgentSessionTypeKey,
             "A1-C3: every session_actor_id carries the AgentSession type tag");

    // --- Part 2: two independently-constructed instances given the SAME session_id resolve to the
    // SAME identity — the property a future real activation/re-activation lookup needs: "the same
    // session" is a property of the id, never of which in-memory object happens to represent it.
    {
        quark::TestKit<Session> kit_a1;
        quark::TestKit<Session> kit_a2;
        kit_a1.actor().initialize("s-A", ae::Principal{"p-alice", ""});
        kit_a2.actor().initialize("s-A", ae::Principal{"p-alice", ""});
        AE_CHECK(ae::session_actor_id(kit_a1.actor().session_id()) ==
                     ae::session_actor_id(kit_a2.actor().session_id()),
                 "A1-R1: two separate instances given the same session_id resolve to the same "
                 "ActorId");
    }

    // --- Part 3: real two-session isolation, proven with content that would cross-contaminate if
    // broken, not merely a size/shape check.
    quark::TestKit<Session> kit_a;
    quark::TestKit<Session> kit_b;
    kit_a.actor().initialize("s-A", ae::Principal{"p-alice", ""});
    kit_b.actor().initialize("s-B", ae::Principal{"p-bob", ""});

    AE_CHECK(kit_a.actor().session_id() == "s-A" && kit_b.actor().session_id() == "s-B",
             "A1-R2: initialize() sets session_id, retrievable afterward");
    AE_CHECK(kit_a.actor().principal().id == "p-alice" && kit_b.actor().principal().id == "p-bob",
             "A1-R2b: initialize() sets principal, retrievable afterward");

    auto r_a = kit_a.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hello from A", "m-a1")});
    auto r_b = kit_b.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hello from B", "m-b1")});

    AE_CHECK(r_a.has_value() && r_b.has_value(), "A1-R3: both sessions complete their own turn");
    if (r_a.has_value() && r_b.has_value()) {
        AE_CHECK(reply_text(*r_a) == "echo:hello from A",
                 "A1-R4: session A's reply reflects ONLY session A's input");
        AE_CHECK(reply_text(*r_b) == "echo:hello from B",
                 "A1-R5: session B's reply reflects ONLY session B's input, not A's");
    }

    AE_CHECK(kit_a.actor().history().size() == 2 && kit_b.actor().history().size() == 2,
             "A1-R6: each session's history grew by exactly its own turn + reply");
    AE_CHECK(kit_a.actor().session_id() == "s-A" && kit_b.actor().session_id() == "s-B",
             "A1-R7: session identity survives handling a turn, unchanged and uncrossed");

    // A second turn on A must still see only A's history (no leakage introduced by B's activity
    // running in between).
    auto r_a2 = kit_a.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("second from A", "m-a2")});
    AE_CHECK(r_a2.has_value() && reply_text(*r_a2) == "echo:second from A",
             "A1-R8: session A's second turn still reflects only A's own history");
    AE_CHECK(kit_a.actor().history().size() == 4,
             "A1-R9: session A's history grew independently of session B's activity");
    AE_CHECK(kit_b.actor().history().size() == 2,
             "A1-R10: session B's history is untouched by session A's second turn");

    std::cout << (g_failures == 0 ? "test_agent_session_isolation: OK\n"
                                   : "test_agent_session_isolation: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
