// The smallest thing a developer would try to build against AgentEngine.
#include <cstdio>
#include <string>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"

using namespace agentengine;

namespace {

struct RollArgs {
    int sides = 6;
};
AE_JSON_SCHEMA(RollArgs, sides)
struct RollReply {
    int value = 0;
};
AE_JSON_SCHEMA(RollReply, value)

struct RollDie : Tool<RollDie> {
    static constexpr std::string_view name        = "roll_die";
    static constexpr std::string_view description = "Roll a die with the given number of sides.";
    using Args                                    = RollArgs;
    using Reply                                   = RollReply;
    static result<Reply> invoke(Args args, EffectContext&) { return Reply{args.sides > 3 ? 4 : 1}; }
};

struct Joker : Agent<Joker, ChatClientId<"demo:joker">, Tools<RollDie>, MaxTurns<4>> {
    static constexpr std::string_view name         = "joker";
    static constexpr std::string_view instructions = "Roll a die, then tell a joke about the number.";
};

class FakeClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return ChatClientCapabilities{}; }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"hello from the consumer build"};
        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<FakeClient>);

}  // namespace

int main() {
    auto agent = make_agent_session<Joker>(FakeClient{});
    if (!agent.has_value()) {
        std::fprintf(stderr, "bind failed: %s (%s)\n", agent.error().message.c_str(),
                     agent.error().code.c_str());
        return 1;
    }
    auto reply = agent->ask("say hi");
    if (!reply.has_value()) {
        std::fprintf(stderr, "ask failed: %s (%s)\n", reply.error().message.c_str(),
                     reply.error().code.c_str());
        return 1;
    }
    std::printf("%s\n", reply->c_str());
    return 0;
}
