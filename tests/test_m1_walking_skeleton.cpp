// Milestone 1 exit criterion (docs/planning/v1-implementation-roadmap.md, "Milestone 1 — Core
// substrate"; breakdown: docs/planning/milestone-1-core-substrate-breakdown.md task 4): "a single
// hard-coded agent runs one turn against a mock ChatClient, on a Quark AgentSession actor, with
// Message/Content as the wire shape ... no tools, no sandbox, no real provider, in-memory session
// only." This is 001-Execution-Model.md §9 G1/G2 "in miniature" — not the real gates (10^4
// concurrent sessions, checkpoint/restart under 019), which stay with Milestone 4.
//
// quark::TestKit<A> (third_party/quark/include/quark/core/testkit.hpp), not a full quark::Engine:
// a single-actor harness that drives ONE real Activation deterministically, with no cluster/engine
// bring-up — the right-sized tool for "one hard-coded agent, one turn."

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

// The "hard-coded" half of the exit criterion: a fixed mock ChatClient, no real provider, always
// returning the same canned assistant reply regardless of what history it was asked with. Test-
// local, not core vocabulary (mirrors test_recorded_chat_client.cpp's precedent of keeping fixture
// clients under tests/, not include/agentengine/).
class HardcodedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value = ae::Text{"the mock model's canned reply"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);

        co_return ae::ChatResponse{reply, ae::Usage{5, 7, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<HardcodedChatClient>,
              "HardcodedChatClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_user_turn() {
    ae::ContentItem item{};
    item.value = ae::Text{"hello, agent"};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role = ae::role::user;
    input.message_id = "m-input";
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<HardcodedChatClient>;

    quark::TestKit<Session> kit;
    AE_CHECK(kit.actor().history().empty(), "session starts with empty history");

    // quark::TestKit::ask returns quark::result<R> (quark's own error type), not ae::result<R> —
    // the reply-cell machinery is Quark's, distinct from AgentEngine's ae::error (core/error.hpp).
    quark::result<ae::AgentResponse> r =
        kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn()});

    AE_CHECK(r.has_value(), "one turn against the mock ChatClient succeeds");
    if (r.has_value()) {
        AE_CHECK(std::holds_alternative<ae::Text>(r->message.content.front().value),
                  "the run's AgentResponse carries the mock's Text content item");
        auto const& reply_text = std::get<ae::Text>(r->message.content.front().value);
        AE_CHECK(reply_text.text == "the mock model's canned reply",
                  "the reply is the mock's canned text, not something fabricated");
        AE_CHECK(r->usage.input_tokens == 5 && r->usage.output_tokens == 7,
                  "Usage rides through the response unchanged");
    }

    AE_CHECK(kit.actor().history().size() == 2,
              "the session's history grew by exactly the user turn + the assistant reply");
    if (kit.actor().history().size() == 2) {
        AE_CHECK(kit.actor().history()[0].role == ae::role::user, "history[0] is the user turn");
        AE_CHECK(kit.actor().history()[1].role == ae::role::assistant,
                  "history[1] is the assistant reply");
    }

    std::cout << (g_failures == 0 ? "test_m1_walking_skeleton: OK\n"
                                   : "test_m1_walking_skeleton: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
