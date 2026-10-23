#include <cstdio>
#include <string>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

using namespace agentengine;

namespace {

// A deterministic stand-in for a real model, so this runs with no API key and no network.
// A real one is AnthropicChatClient / OpenAIChatClient (see the Model providers page).
class HelloClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return ChatClientCapabilities{}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;

        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"Hello from AgentEngine."};
        reply.content.push_back(std::move(item));

        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        // Unused here -- the session calls chat() unless streaming is switched on. The ChatClient
        // concept still requires it, so it returns an already-closed stream.
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<HelloClient>, "HelloClient must satisfy the ChatClient concept");

// An agent is a TYPE: its policies are template parameters, resolved at compile time.
struct Greeter : Agent<Greeter, ChatClientId<"demo:hello">, MaxTurns<4>> {
    static constexpr std::string_view name         = "greeter";
    static constexpr std::string_view instructions = "Greet the user in one short sentence.";
};

}  // namespace

int main() {
    // make_agent_session<A>() runs register_agent<A>()'s validation and binds the compiled policy
    // set to a real session. The caller supplies the client; nothing else is needed.
    auto agent = make_agent_session<Greeter>(HelloClient{});
    if (!agent.has_value()) {
        std::fprintf(stderr, "bind failed: %s (%s)\n", agent.error().message.c_str(),
                     agent.error().code.c_str());
        return 1;
    }

    auto reply = agent->ask("say hello");
    if (!reply.has_value()) {
        std::fprintf(stderr, "run failed: %s (%s)\n", reply.error().message.c_str(),
                     reply.error().code.c_str());
        return 1;
    }

    std::printf("%s\n", reply->c_str());
    return 0;
}
