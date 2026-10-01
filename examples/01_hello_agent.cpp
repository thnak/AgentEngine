// AgentEngine "get started" examples, 1 of 4 -- the smallest possible agent, declared and run.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/01_hello_agent: build an agent, send
// it one message, print what comes back. In AgentEngine an agent is a TYPE (002 §1): `Joker` below
// declares its model binding, its tools, its capability ceiling and its turn bound as CRTP policy tags
// on `Agent<Joker, ...>`, and `make_agent_session<Joker>()` (core/agent_session_bridge.hpp,
// decisions/ADR-226, 002 §2.1) compiles and validates that declaration with `register_agent<Joker>()`
// and turns it into a running `rt::AgentSession` -- so the policy set the type declares is the policy set
// the session enforces. Nothing below re-states a policy by hand.
//
// What the run shows: the model (a deterministic fake) first calls the declared `roll_die` tool, then
// answers using the tool's result -- one real model -> tool -> model round trip through the engine's
// ten-step tool pipeline (006 §3).
//
// Offline on purpose: `JokerChatClient` is a small deterministic stand-in for a real backend, so this
// example builds and runs with no API key and no network access. To run the same agent against a real
// model, construct a real ChatClient (protocol/openai/chat_client.hpp, needs AGENTENGINE_WITH_HTTPS)
// and pass it to `make_agent_session<Joker>()` instead, with its API-key `cap::Secret` in
// `AgentSessionOptions::chat_client_grants`; `tools/cli_chat.cpp` shows a real client being wired up.
//
// Run: ./agentengine_example_01_hello_agent

#include <cstdio>
#include <memory_resource>
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

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// ---- a tool (006 §1): typed arguments and reply, a static invoke, no capabilities needed --------------
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
    using Args  = RollArgs;
    using Reply = RollReply;
    // Deterministic for a repeatable example: a fair die is someone else's problem.
    static result<Reply> invoke(Args args, EffectContext&) { return Reply{args.sides > 3 ? 4 : 1}; }
};

// ---- the agent (002 §2): policies are types; register_agent<Joker>() compiles and validates them ------
struct Joker : Agent<Joker, ChatClientId<"demo:joker">, Tools<RollDie>, MaxTurns<4>, TokenBudget<1'000>> {
    static constexpr std::string_view name         = "joker";
    static constexpr std::string_view instructions = "Roll a die, then tell a pirate joke that uses the number.";
};

// ---- the "model": calls roll_die once, then tells a joke built from the tool's result -------------------
// The tool's reply reaches the model as JSON ({"value":4}); this reads it back the typed way.
std::string rolled_value(ChatRequest const& request) {
    for (Message const& m : request.messages) {
        for (ContentItem const& item : m.content) {
            if (auto const* r = std::get_if<ToolResult>(&item.value)) {
                for (ContentItem const& part : r->content) {
                    auto const* d = std::get_if<Data>(&part.value);
                    if (!d) continue;
                    auto parsed = json::parse(d->json);
                    if (!parsed) continue;
                    if (auto reply = schema::from_json<RollReply>(*parsed)) return std::to_string(reply->value);
                }
            }
        }
    }
    return {};
}

class JokerChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities c;
        c.tool_calling = true;
        return c;
    }

    task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;
        std::string const rolled = rolled_value(request);
        if (rolled.empty()) {
            ToolCall call;
            call.call_id        = "call-1";
            call.tool_name      = "roll_die";
            call.arguments_json = R"({"sides":6})";
            call.provenance     = call_provenance::vendor_structured;
            item.value          = std::move(call);
        } else {
            item.value = Text{"The die says " + rolled + ". Why did the pirate take so long to learn the "
                              "alphabet? Because he kept getting stuck at C."};
        }
        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{10, 10, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        // Unused here (the session calls chat() unless streaming is switched on); the concept needs it.
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<JokerChatClient>, "JokerChatClient must satisfy the ChatClient concept");

}  // namespace

int main() {
    // The caller supplies the client and whatever authority it holds (AgentSessionOptions::grants -- none
    // needed by roll_die); the agent's declaration supplies everything else.
    auto agent = make_agent_session<Joker>(JokerChatClient{});
    check(agent.has_value(), "make_agent_session<Joker>() validates the declaration and binds a session");
    if (!agent.has_value()) {
        std::fprintf(stderr, "bind failed: %s (%s)\n", agent.error().message.c_str(), agent.error().code.c_str());
        return 1;
    }

    auto reply = agent->ask("Tell me a joke about a pirate.");
    check(reply.has_value(), "the agent answers");
    if (reply.has_value()) {
        std::printf("%s\n", reply->c_str());
        check(reply->find("4") != std::string::npos, "the answer used the declared tool's result");
    }
    check(agent->session().max_turns() == std::optional<std::uint64_t>{4},
          "the session's turn bound is the one Joker declared (MaxTurns<4>)");

    std::fprintf(stderr,
                 g_failures == 0 ? "example_01_hello_agent: OK\n" : "example_01_hello_agent: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
