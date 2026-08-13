// AgentEngine "get started" examples, 1 of 4 -- the smallest possible agent turn.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/01_hello_agent: build an agent, send
// it one message, print what comes back. AgentEngine's equivalent of MAF's `AIAgent` is
// `agentengine::rt::AgentSession<ChatClientT>` (agentengine/rt/agent_session.hpp, ADR-037's
// Quark-free replacement for the old, Quark-actor-based `agentengine::AgentSession`) -- driven here
// by resuming its returned `agentengine::rt::task<T>` directly, the same "safe because nothing here
// genuinely suspends on an external wake" idiom every rt:: test file's own `drive<T>()` uses; no
// actor engine of any kind is needed for a single request/reply turn (see 04_first_workflow.cpp for
// the workflow case).
//
// The ChatClient below is a small deterministic fake (`JokerChatClient`), not a real OpenAI/
// Anthropic backend, so this example builds and runs completely offline with no API key and no
// network access. `tools/cli_chat.cpp` shows how a real network-backed ChatClient is wired up
// instead (needs AGENTENGINE_OPENROUTER_API_KEY and a build configured with AGENTENGINE_WITH_HTTPS).
//
// Run: ./agentengine_example_01_hello_agent

#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;

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

// The whole "model": always answers with the same pirate joke, regardless of what it's asked --
// deterministic on purpose, so this example is a real, repeatable, offline proof rather than a demo
// that only works against a live network call.
class JokerChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"Why did the pirate take so long to learn the alphabet? "
                            "Because he kept getting stuck at C."};

        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-joke";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = Text{"Why did the pirate take so long to learn the alphabet? "
                                 "Because he kept getting stuck at C."};
        upd.is_final = true;
        upd.usage    = Usage{1, 1, 0, 0, 0.0};
        auto pushed  = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<JokerChatClient>, "JokerChatClient must satisfy the ChatClient concept");

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

// Drives an agentengine::rt::task<T> to completion. Safe here: JokerChatClient::chat() never
// suspends on anything external (it co_returns immediately), so one resume() loop resolves the
// whole run -- the same "safe because nothing here genuinely suspends" reasoning every rt:: test
// file's own drive<T>() relies on.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // AgentSession<ChatClientT> with only one template argument uses the default HistoryProviderT
    // (HistoryProvider<Window<0>>, an unbounded window) and NoSessionState -- everything a one-shot
    // hello-world agent needs.
    using HelloAgent = AgentSession<JokerChatClient>;
    HelloAgent session;
    session.initialize("s-hello", Principal{"p-demo", ""});
    session.emplace_chat_client();

    auto r = drive(session.start_run(StartRun{user_message("Tell me a joke about a pirate.")}));
    check(r.has_value(), "the agent answers the single turn");
    if (r.has_value()) {
        std::string const reply = text_of(r->message);
        std::printf("%s\n", reply.c_str());
        check(!reply.empty(), "the reply carries text");
    }

    std::fprintf(stderr,
                 g_failures == 0 ? "example_01_hello_agent: OK\n" : "example_01_hello_agent: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
