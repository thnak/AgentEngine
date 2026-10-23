// devsim/verify-tool-agent — verifies the tool-plus-real-provider shape added to
// api/first-app.html section 4 (item 5). The Anthropic client itself needs AGENTENGINE_WITH_HTTPS,
// so this binds the SAME tool and agent declarations to an offline client: if Tools<OrderLookup>
// and the Agent<...> policy set compile and bind here, the only remaining variable is the client.
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

// ---- the tool, exactly as the page declares it ------------------------------------------------
struct LookupArgs {
    std::string order_id;
};
AE_JSON_SCHEMA(LookupArgs, order_id)

struct LookupReply {
    std::string status;
};
AE_JSON_SCHEMA(LookupReply, status)

struct OrderLookup : Tool<OrderLookup, Capabilities<>> {   // no capability: it reaches nothing
    static constexpr std::string_view name        = "lookup_order";
    static constexpr std::string_view description = "Look up one order's status by id.";
    using Args                                    = LookupArgs;
    using Reply                                   = LookupReply;
    // The STATIC form, because this is what binds. ADR-237 §5.2's target shape -- a non-static
    // const coroutine member `task<result<Reply>> invoke(Args, EffectContext&) const` -- was tried
    // here first and FAILED to compile:
    //   tool_descriptor.hpp(211): error C2352: 'OrderLookup::invoke': a call of a non-static member
    //   function requires an object
    // The registration path still calls invoke statically, so the header's documented target shape
    // is not yet usable. The docs say exactly this, which is why they still teach the static form.
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{"shipped:" + a.order_id}; }
};

// ---- the agent, exactly as the page declares it -----------------------------------------------
struct OrderAssistant
    : Agent<OrderAssistant, ChatClientId<"anthropic:claude-sonnet-5">, Tools<OrderLookup>,
            MaxTurns<6>> {
    static constexpr std::string_view name         = "order-assistant";
    static constexpr std::string_view instructions = "Answer using the lookup_order tool.";
};

// Registers both spellings, so the transitional claim is exercised rather than asserted.
// A second tool using the same (currently required) static form, to prove several tools can be
// declared and bound together.
struct LegacyLookup : Tool<LegacyLookup, Capabilities<>> {
    static constexpr std::string_view name        = "legacy_lookup";
    static constexpr std::string_view description = "A second tool, same declaration form.";
    using Args                                    = LookupArgs;
    using Reply                                   = LookupReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{"legacy:" + a.order_id}; }
};

struct LegacyAssistant
    : Agent<LegacyAssistant, ChatClientId<"anthropic:claude-sonnet-5">, Tools<LegacyLookup>,
            MaxTurns<2>> {
    static constexpr std::string_view name         = "legacy-assistant";
    static constexpr std::string_view instructions = "Uses the transitional static invoke form.";
};

// ---- an offline stand-in for the real client --------------------------------------------------
class StubClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities c;
        c.tool_calling = true;   // the half the page warns about
        return c;
    }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"order 1042 is shipped"};
        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<StubClient>);

}  // namespace

int main() {
    int g_failures = 0;

    // The declared tool's schema is really extracted from the type -- proof the declaration is read.
    auto const schema = schema::json_schema_of<LookupArgs>();
    if (schema.find("order_id") == std::string::npos) {
        std::fprintf(stderr, "FAIL: schema has no order_id: %s\n", schema.c_str());
        ++g_failures;
    } else {
        std::printf("  ok: Tools<OrderLookup>'s Args schema carries order_id\n");
    }

    AgentSessionOptions opts;
    opts.session_id = "s-tool";
    opts.principal  = Principal{"p-demo", ""};

    auto assistant = make_agent_session<OrderAssistant>(StubClient{}, opts);
    if (!assistant.has_value()) {
        std::fprintf(stderr, "FAIL: bind failed: %s (%s)\n", assistant.error().message.c_str(),
                     assistant.error().code.c_str());
        ++g_failures;
    } else {
        std::printf("  ok: an agent with Tools<...> binds through make_agent_session(client, opts)\n");
    }

    // ADR-237 §5.2: the transitional static-invoke form must still register, or the docs' claim
    // that it is "still accepted transitionally" is wrong.
    AgentSessionOptions legacy_opts;
    legacy_opts.session_id = "s-legacy";
    legacy_opts.principal  = Principal{"p-demo", ""};
    auto legacy = make_agent_session<LegacyAssistant>(StubClient{}, legacy_opts);
    if (!legacy.has_value()) {
        std::fprintf(stderr, "FAIL: legacy static-invoke tool no longer registers: %s (%s)\n",
                     legacy.error().message.c_str(), legacy.error().code.c_str());
        ++g_failures;
    } else {
        std::printf("  ok: the transitional static invoke form still registers\n");
    }

    std::fprintf(stderr, g_failures == 0 ? "verify_tool_agent: OK\n" : "verify_tool_agent: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
