// devsim/verify-toolcall — verifies the "producing a tool call" snippet before it goes on the tool
// page. A synthesised ToolCall must drive the REAL ten-step pipeline offline: the declared tool's
// invoke() runs, its reply comes back to the model as a ToolResult, and the second turn answers.
// This is how every example's scripted client works, and how you test a tool-using agent with no
// network and no key.
#include <cstdio>
#include <string>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what);
    if (!cond) ++g_failures;
}

// Counts its own invocations, so "the pipeline really ran it" is measured, not assumed.
int g_die_calls = 0;

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
    static result<Reply> invoke(Args args, EffectContext&) {
        ++g_die_calls;
        return Reply{args.sides};
    }
};

struct Joker : Agent<Joker, ChatClientId<"demo:joker">, Tools<RollDie>, MaxTurns<4>> {
    static constexpr std::string_view name         = "joker";
    static constexpr std::string_view instructions = "Roll a die, then answer with its value.";
};

// The recipe under test: a client that SYNTHESISES a ToolCall on the first turn, then answers from
// the ToolResult the session feeds back on the second.
class ScriptedClient {
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

        if (std::string const rolled = rolled_value(request); rolled.empty()) {
            // ---- the snippet: how a TOOL CALL is produced ----
            ToolCall call;
            call.call_id        = "call-1";
            call.tool_name      = "roll_die";           // must match a declared Tool<>::name
            call.arguments_json = R"({"sides":6})";     // validated against the tool's own schema
            call.provenance     = call_provenance::vendor_structured;
            item.value          = std::move(call);
        } else {
            item.value = Text{"the die says " + rolled};
        }

        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{10, 10, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }

private:
    // Reads the tool's reply back out of the conversation the typed way -- proof a ToolResult
    // really arrived, rather than the second turn inventing an answer.
    static std::string rolled_value(ChatRequest const& request) {
        for (Message const& m : request.messages) {
            for (ContentItem const& ci : m.content) {
                auto const* r = std::get_if<ToolResult>(&ci.value);
                if (!r) continue;
                for (ContentItem const& part : r->content) {
                    auto const* d = std::get_if<Data>(&part.value);
                    if (!d) continue;
                    auto parsed = json::parse(d->json);
                    if (!parsed) continue;
                    if (auto reply = schema::from_json<RollReply>(*parsed)) {
                        return std::to_string(reply->value);
                    }
                }
            }
        }
        return {};
    }
};
static_assert(ChatClient<ScriptedClient>);

}  // namespace

int main() {
    auto agent = make_agent_session<Joker>(ScriptedClient{});
    if (!agent.has_value()) {
        std::fprintf(stderr, "FAIL: bind: %s (%s)\n", agent.error().message.c_str(),
                     agent.error().code.c_str());
        return 1;
    }

    auto reply = agent->ask("roll a die");
    check(g_die_calls == 1, "a SYNTHESISED ToolCall drove the real pipeline: invoke() ran once");
    check(reply.has_value(), "the turn converged");
    if (reply.has_value()) {
        check(reply->find("6") != std::string::npos,
              "the answer used the value from the ToolResult the session fed back");
    }

    std::fprintf(stderr, g_failures == 0 ? "verify_toolcall: OK\n" : "verify_toolcall: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
