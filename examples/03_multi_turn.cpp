// AgentEngine "get started" examples, 3 of 4 -- a multi-turn conversation.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/03_multi_turn: ask the same agent two
// things in a row and show the second answer is aware of the first. In AgentEngine there is no
// separate "session" object you thread through calls -- the `AgentSession` actor itself IS the
// conversation: every `StartRun` ask against the same session actor appends to its own durable
// `history_` (005 §3) and that whole accumulated history is what the next turn's `ChatClient` sees
// (through whatever `HistoryProviderT` the session was built with -- the default,
// `HistoryProvider<Window<0>>`, is unbounded, matching what's used here).
//
// Run: ./agentengine_example_03_multi_turn

#include <cstdio>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

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

// Answers differently depending on how many times it's been called -- a stand-in for a real model
// actually reading the conversation history it was sent (`request.messages` carries the whole
// accumulated history by the time the second call happens; this fake doesn't need to re-read it to
// demonstrate that the SESSION carried it forward, which is the point of this example).
class JokerChatClient {
public:
    std::size_t call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        std::string const text =
            call_count == 0
                ? "Why did the pirate take so long to learn the alphabet? He kept getting stuck at C."
                : "Same joke, now in the voice of the pirate's parrot: 'Stuck at C! Stuck at C!'";

        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{text};

        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-" + std::to_string(call_count);
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }  // unused
};
static_assert(ChatClient<JokerChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using JokeAgent = AgentSession<JokerChatClient>;
    quark::TestKit<JokeAgent> kit;
    kit.actor().initialize("s-multi-turn", Principal{"p-demo", ""});

    // Turn 1, on the session.
    auto r1 = kit.ask<AgentResponse>(StartRun{user_message("Tell me a joke about a pirate.")});
    check(r1.has_value(), "turn 1 answers");
    if (r1.has_value()) std::printf("%s\n", text_of(r1->message).c_str());

    // Turn 2, on the SAME session -- no new AgentSession, no session object passed explicitly; the
    // history from turn 1 is already part of this actor's own state.
    auto r2 = kit.ask<AgentResponse>(
        StartRun{user_message("Now tell the same joke in the voice of a pirate's parrot.")});
    check(r2.has_value(), "turn 2 answers");
    if (r2.has_value()) std::printf("%s\n", text_of(r2->message).c_str());

    if (r1.has_value() && r2.has_value()) {
        check(text_of(r1->message) != text_of(r2->message),
              "the two replies differ -- the second turn is a genuinely new model call, not a "
              "cached repeat of the first");
    }
    // history_ now holds 4 messages: [user:1, assistant:1, user:2, assistant:2] -- both turns are
    // durably recorded on the session, not just the two replies printed above.
    check(kit.actor().history().size() == 4,
          "both turns are durably recorded in the session's own history");

    std::fprintf(stderr,
                 g_failures == 0 ? "example_03_multi_turn: OK\n" : "example_03_multi_turn: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
