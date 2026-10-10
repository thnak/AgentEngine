// devsim/rerun1/main.cpp -- a command-line order-desk assistant for a small business.
//
// Built from the documentation site only; see REPORT.md for which claim on this page came from
// which doc file and line, and which names had to be reverse-engineered from engine sources.
//
// Shape: include-set + session binding from ApiFirstAppReference.tsx ("Your first program"),
// tool declaration from ApiToolReference.tsx ("Tool"), real-provider wiring from
// ApiFirstAppReference.tsx §4 ("Point it at a real model") + providerContent.ts.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;

namespace {

// ------------------------------------------------------------------------------------------------
// 1. The one custom tool the product needs: look up an order by id.
//    Declaration surface: the five required members from ApiToolReference.tsx (name, description,
//    Args, Reply, invoke). Args/Reply are schema-typed structs; AE_JSON_SCHEMA only lists field
//    names, so a field's own description lives on its type via Described<T, "...">.
// ------------------------------------------------------------------------------------------------

struct LookupOrderArgs {
    Described<std::string, "The order id to look up, e.g. \"A-1042\""> order_id;
};
AE_JSON_SCHEMA(LookupOrderArgs, order_id)

struct LookupOrderReply {
    std::string order_id;
    std::string status;
    int         total_cents;
    std::string eta;
};
AE_JSON_SCHEMA(LookupOrderReply, order_id, status, total_cents, eta)

struct LookupOrderTool
    : Tool<LookupOrderTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "lookup_order";
    static constexpr std::string_view description =
        "Look up a single order by its id; returns status, total in cents, and ETA.";
    using Args  = LookupOrderArgs;
    using Reply = LookupOrderReply;

    static result<Reply> invoke(Args args, EffectContext&) {
        // Described<T, "..."> is a real wrapper type (json_schema.hpp:100); its public `value`
        // member is how invoke() reads the field. No doc page shows this.
        std::string const& id = args.order_id.value;
        if (id == "A-1042") {
            return Reply{"A-1042", "shipped", 4599, "2026-03-04"};
        }
        if (id == "A-2001") {
            return Reply{"A-2001", "processing", 1299, "2026-03-07"};
        }
        return std::unexpected(
            error{failure_class::contract,
                  "no order with id '" + id + "'",
                  "order.not_found"});
    }
};

// ------------------------------------------------------------------------------------------------
// 2. The agent is a TYPE. Tools<...> is the policy the engine compiles into the run's tool table;
//    ChatClientId has no default and is required. register_agent<A>()'s validation runs inside
//    make_agent_session<A>().
// ------------------------------------------------------------------------------------------------

struct OrderAssistant
    : Agent<OrderAssistant,
            ChatClientId<"anthropic:claude-sonnet-5">,
            Tools<LookupOrderTool>,
            MaxTurns<6>> {
    static constexpr std::string_view name = "order-assistant";
    static constexpr std::string_view instructions =
        "You are the order desk for a small business. Answer questions about a customer's order. "
        "Use the lookup_order tool for any question about a specific order, then answer in one "
        "short sentence. Never invent an order status.";
};

// Every fallible call returns result<T> == std::expected<T, agentengine::error>; nothing throws.
// One place that prints the engine's own error message and error code, and never swallows them.
[[noreturn]] void fail(char const* what, error const& e) {
    std::fprintf(stderr, "%s: %s (error code: %s)\n", what, e.message.c_str(), e.code.c_str());
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    // A small CLI affordance: show exactly what the model is told this tool accepts and returns.
    // AE_JSON_SCHEMA compiles that schema out of the Args/Reply types; Tool<Derived>'s own
    // args_schema()/reply_schema() are what the run's ToolDescriptor carries to the backend
    // (ApiToolReference.tsx, "Where the schema goes"). No API key, no network.
    if (argc > 1 && std::string_view{argv[1]} == "--schema") {
        std::printf("lookup_order args:  %s\n", LookupOrderTool::args_schema().c_str());
        std::printf("lookup_order reply: %s\n", LookupOrderTool::reply_schema().c_str());
        return 0;
    }

    std::string const question =
        argc > 1 ? argv[1] : "What is the status of order A-1042?";

    // The credential is resolved from the environment at the point of use, never compiled in
    // (018 §4). EnvSecretSource's prefix defaults to the historical "QUARK_SECRET_", so an empty
    // prefix is what gives a bare ANTHROPIC_API_KEY.
    auto store = std::make_unique<AgentEngineSecretStore>(
        std::make_unique<EnvSecretSource>(""));

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.tool_calling      = true;
    caps.max_output_tokens = 512;

    // The real class is agentengine::anthropic::AnthropicChatClient (chat_client.hpp:118,444);
    // no doc page carries the namespace qualifier.
    using RealClient = agentengine::anthropic::AnthropicChatClient<AgentEngineSecretStore>;
    RealClient client{"api.anthropic.com", 443, "claude-sonnet-5",
                      SecretRef{"ANTHROPIC_API_KEY"}, caps, *store};

    // The client's credential is DEPLOYMENT configuration, handed over explicitly. cap::Secret is
    // the only capability chat_client_grants accepts; omitting it compiles and then fails the first
    // run with secret.not_granted, which reads like a broken key.
    AgentSessionOptions opts;
    opts.session_id         = "s-order-desk";
    opts.principal          = Principal{"p-demo", ""};
    opts.chat_client_grants = {Capability{cap::Secret{"ANTHROPIC_API_KEY"}}};

    // A failed build: make_agent_session<A>() ran register_agent<A>()'s validation and refused.
    auto agent = make_agent_session<OrderAssistant>(client, opts);
    if (!agent.has_value()) {
        fail("bind failed", agent.error());
    }

    // A failed run, including a credential the engine refuses to resolve.
    std::printf("> %s\n", question.c_str());
    auto reply = agent->ask(question);
    if (!reply.has_value()) {
        fail("run failed", reply.error());
    }

    std::printf("%s\n", reply->c_str());
    return 0;
}
