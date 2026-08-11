// AgentEngine "get started" examples, 2 of 4 -- an agent that calls a function tool.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/02_add_tools: declare a weather
// lookup as a tool, let the agent call it, print the final answer. AgentEngine tools are CRTP
// types (`Tool<Derived, Policies...>`, agentengine/core/tool.hpp) declared with a capability
// ceiling, an approval mode, and an effect class up front -- 006/007's "every effect is
// attributable, nothing runs on ambient authority" (I2) applies to a tool call exactly like it
// applies to any other effect. `GetWeatherTool` below declares an EMPTY capability ceiling
// (`Capabilities<>`) because it touches nothing outside its own arguments; a tool that touched the
// filesystem or network would name that capability explicitly (see
// `tests/test_agent_session_tool_call_loop.cpp`'s `WriteTool` for a capability-bearing example).
//
// A session's available tools come from its `ContextProvider` (005 §5) -- there is no separate
// "tool registry" a session reaches into. `WeatherHistoryProvider` below both replays history AND
// declares the one tool this agent may call, the same shape `ToolLoopHistoryProvider` uses in the
// test suite this example is a narrated version of.
//
// As in 01_hello_agent.cpp, the ChatClient is a small scripted fake, not a live backend -- this
// example is deterministic and needs no network access or API key. It runs the real, production
// tool-call loop inside `AgentSession::handle()` (ADR-027), not a simulation of it.
//
// Run: ./agentengine_example_02_add_tools

#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_pipeline.hpp"
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

// ---- The tool ------------------------------------------------------------------------------

struct WeatherArgs { std::string location; };
AE_JSON_SCHEMA(WeatherArgs, location)
struct WeatherReply { std::string forecast; };
AE_JSON_SCHEMA(WeatherReply, forecast)

[[nodiscard]] bool& weather_tool_invoked() {
    static bool invoked = false;
    return invoked;
}

// Empty capability ceiling: this tool only formats its own argument, so it needs no capability
// grant to run, and `never_require` approval (the default, undeclared here) is the honest policy
// for it -- 007's "declare only what the effect actually needs."
struct GetWeatherTool : Tool<GetWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description = "Gets the current weather for a location.";
    using Args = WeatherArgs;
    using Reply = WeatherReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        weather_tool_invoked() = true;
        return Reply{"The weather in " + a.location + " is cloudy with a high of 15C."};
    }
};

// ---- ContextProvider: replays history and declares the one tool above ----------------------

class WeatherHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<GetWeatherTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<WeatherHistoryProvider>);

// ---- A scripted "model": asks for the tool once, then answers from its result --------------

class ScriptedWeatherChatClient {
public:
    std::size_t call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply;
        reply.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        if (call_count == 0) {
            reply.message_id = "m-call";
            item.value = ToolCall{"c1", "get_weather", R"({"location":"Amsterdam"})",
                                   content_origin::assistant, call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-answer";
            item.value = Text{"Amsterdam: cloudy, 15C high."};
        }
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }  // unused
};
static_assert(ChatClient<ScriptedWeatherChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

using WeatherAgent = AgentSession<ScriptedWeatherChatClient, NoSessionState, WeatherHistoryProvider>;

}  // namespace

int main() {
    quark::TestKit<WeatherAgent> kit;
    kit.actor().initialize("s-weather", Principal{"p-demo", ""});
    // Even an all-empty-Capabilities<> tool must be authorized against a real, explicitly granted
    // CapabilitySet (I2: no ambient authority) -- there is no "no capabilities needed" bypass.
    CapabilitySet const held = CapabilitySet::grant_root({});
    kit.actor().set_capabilities(&held);

    auto r = kit.ask<AgentResponse>(StartRun{user_message("What is the weather like in Amsterdam?")});
    check(r.has_value(), "the agent's tool-call round trip converges to a final answer");
    if (r.has_value()) {
        std::string const reply = text_of(r->message);
        std::printf("%s\n", reply.c_str());
        check(!reply.empty(), "the final reply carries text");
    }
    check(weather_tool_invoked(), "get_weather's invoke() actually ran, not just the scripted text");

    std::fprintf(stderr,
                 g_failures == 0 ? "example_02_add_tools: OK\n" : "example_02_add_tools: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
