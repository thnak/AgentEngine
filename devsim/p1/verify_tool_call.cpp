// devsim/p1/verify_tool_call.cpp -- build-time verification that the declared tool really runs.
//
// This is NOT the product. It is the offline half of the proof: the same `OrderAssistant` declaration
// and the same `LookupOrderTool`, driven by a deterministic in-process ChatClient instead of a real
// backend, because this machine has no API credential. It proves the part a real provider cannot prove
// on its own here -- that `lookup_order` reaches the model's tool table, that the engine's ten-step
// pipeline (006 §3) really invokes it, and that its schema-typed JSON reply folds back into the
// conversation the scripted model then answers from.
//
// Off by default; enable with -DORDER_ASSISTANT_BUILD_OFFLINE_CHECK=ON.

#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"

// The product's own declaration, shared verbatim -- not a copy.
#include "order_assistant_decl.hpp"

using namespace agentengine;
using namespace order_desk;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (cond) {
        std::fprintf(stderr, "  ok: %s\n", what);
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// Pull the tool's JSON reply back out of the request the session assembled for the second round.
std::string tool_reply_json(ChatRequest const& request) {
    for (Message const& m : request.messages) {
        for (ContentItem const& item : m.content) {
            if (auto const* r = std::get_if<ToolResult>(&item.value)) {
                for (ContentItem const& part : r->content) {
                    if (auto const* d = std::get_if<Data>(&part.value)) return d->json;
                }
            }
        }
    }
    return {};
}

class ScriptedOrderClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities c;
        c.tool_calling = true;
        return c;
    }

    task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        Message reply{};
        reply.role       = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;

        std::string const from_tool = tool_reply_json(request);
        if (from_tool.empty()) {
            ToolCall call;
            call.call_id        = "call-1";
            call.tool_name      = "lookup_order";
            call.arguments_json = R"({"order_id":"1042"})";
            call.provenance     = call_provenance::vendor_structured;
            item.value          = std::move(call);
        } else {
            auto parsed = json::parse(from_tool);
            std::string status = "?";
            if (parsed) {
                if (auto const* v = parsed->find("status"); v && v->is_string()) status = v->as_string();
            }
            item.value = Text{"Order 1042 is " + status + "."};
        }
        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{10, 10, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<ScriptedOrderClient>, "the scripted client must satisfy the ChatClient concept");

}  // namespace

int main() {
    // The declared tool's own schema, compiled by the engine from AE_JSON_SCHEMA -- not hand-written.
    std::string const args_schema = schema::json_schema_of<LookupOrderArgs>();
    check(args_schema.find("\"order_id\"") != std::string::npos,
          "lookup_order's Args schema really carries order_id");
    check(args_schema.find("The order id to look up") != std::string::npos,
          "the Described<...> field description reaches the schema the model is shown");

    // The tool body itself, called directly.
    EffectContext ctx{};
    auto direct = LookupOrderTool::invoke(LookupOrderArgs{std::string{"1042"}}, ctx);
    check(direct.has_value() && direct->found && direct->status == "shipped" && direct->total == 61.50,
          "lookup_order(1042) returns shipped / 61.50");
    auto missing = LookupOrderTool::invoke(LookupOrderArgs{std::string{"9999"}}, ctx);
    check(missing.has_value() && !missing->found && missing->status == "not_found",
          "an unknown id comes back as found:false / not_found, not an error");

    // The whole declared agent, bound exactly the way main.cpp binds it.
    AgentSessionOptions opts;
    opts.session_id = "verify";
    opts.principal  = Principal{"verify", ""};
    auto agent = make_agent_session<OrderAssistant>(ScriptedOrderClient{}, std::move(opts));
    check(agent.has_value(), "make_agent_session<OrderAssistant>() validates the declaration");
    if (!agent.has_value()) {
        std::fprintf(stderr, "bind failed: %s (%s)\n", agent.error().message.c_str(),
                     agent.error().code.c_str());
        return 1;
    }

    auto reply = agent->ask("Where is order 1042?");
    check(reply.has_value(), "the run converges");
    if (reply.has_value()) {
        std::printf("%s\n", reply->c_str());
        check(*reply == "Order 1042 is shipped.",
              "the answer is built from what lookup_order actually returned through the tool pipeline");
    }

    std::fprintf(stderr, g_failures == 0 ? "verify_tool_call: OK\n" : "verify_tool_call: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
