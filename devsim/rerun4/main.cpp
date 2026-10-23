// devsim/rerun4/main.cpp
//
// A command-line order-lookup assistant for a small business, built on AgentEngine.
//
// What it is: one custom Tool of our own (`lookup_order`, a small JSON reply), one Agent declared the
// way the engine wants an agent declared (a compile-time CRTP policy set), bound to a REAL model
// provider (Anthropic) whose API key is resolved from an environment variable at run time and is
// never compiled in.
//
// Usage:  ae_order_assistant ["<question>"]
// Env:    ANTHROPIC_API_KEY   (required by default; resolved by EnvSecretSource with an EMPTY
//                              prefix, so the bare name is the variable name -- see the docs' own
//                              note on this). AE_API_KEY_ENV names a different variable.
//         AE_MODEL_HOST / AE_MODEL_PORT / AE_MODEL_ID / AE_MODEL_PATH_PREFIX
//                             (optional deployment overrides; defaults are api.anthropic.com,
//                              443, claude-sonnet-5, /v1 -- i.e. the real Anthropic endpoint)
//
// Exit codes: 0 = one turn ran and the answer was printed; 1 = the *agent build* (register/bind) or
// the *run* failed, in which case the engine's own error message and error code are printed to
// stderr. Nothing is swallowed.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

// ---- Includes, each taken from a documented page (see REPORT.md "Getting started") -------------
#include "agentengine/core/agent.hpp"                  // Agent<>, ChatClientId<>, Tools<>, MaxTurns<>
#include "agentengine/core/agent_session_bridge.hpp"   // make_agent_session<A>(), AgentSessionOptions
#include "agentengine/core/chat_client.hpp"            // the ChatClient concept
#include "agentengine/core/content.hpp"                // Message / ContentItem / Text (unused here, but
                                                       // documented as part of the minimal include set)
#include "agentengine/core/json_schema.hpp"            // AE_JSON_SCHEMA, Described<T, "...">
#include "agentengine/core/tool.hpp"                   // Tool<Derived, Policies...>
#include "agentengine/protocol/anthropic/chat_client.hpp"  // anthropic::AnthropicChatClient<Store>
#include "agentengine/trust/capability.hpp"            // Capability, cap::Secret, CapabilitySet
#include "agentengine/trust/principal.hpp"             // Principal
#include "agentengine/trust/secret.hpp"                // EnvSecretSource, AgentEngineSecretStore

using namespace agentengine;

namespace {

// ---------------------------------------------------------------------------------------------
// 1. The custom tool: look up one order by id, returning a small JSON reply.
//    Declared exactly the way 006 wants it declared: name, description, an Args/Reply pair each
//    paired with AE_JSON_SCHEMA, and a static invoke(). Capabilities<> is empty on purpose -- this
//    tool reaches no privileged effect, so it is also the form that needs no capability grant.
// ---------------------------------------------------------------------------------------------

struct LookupArgs {
    Described<std::string, "The order id to look up, for example 'A-1042'"> order_id;
};
AE_JSON_SCHEMA(LookupArgs, order_id)

struct LookupReply {
    std::string status;
    int         total_cents = 0;
};
AE_JSON_SCHEMA(LookupReply, status, total_cents)

struct LookupOrderTool : Tool<LookupOrderTool, Capabilities<>> {
    static constexpr std::string_view name        = "lookup_order";
    static constexpr std::string_view description = "Look up one order's status and total by its order id.";
    using Args  = LookupArgs;
    using Reply = LookupReply;

    static result<Reply> invoke(Args args, EffectContext& /*ctx*/) {
        // A real deployment would query the shop's own order database here. This is a fixed table so
        // that the tool's own reply shape is deterministic and reviewable.
        // NOTE: a Described<T, "..."> field is read as `.value`; the docs warn about exactly this
        // (args.order_id == "A-1042" fails to compile with an operator-overload error rather than a
        // clear one).
        std::string const& id = args.order_id.value;
        if (id == "A-1042") return Reply{"shipped", 4599};
        if (id == "A-1043") return Reply{"awaiting_payment", 1299};
        if (id == "A-1044") return Reply{"delivered", 2199};
        return std::unexpected(error{failure_class::contract,
                                     "no order with id '" + id + "'",
                                     "lookup_order.not_found"});
    }
};

// ---------------------------------------------------------------------------------------------
// 2. The agent: a TYPE. Policies are template parameters, resolved at compile time.
// ---------------------------------------------------------------------------------------------

struct OrderAssistant : Agent<OrderAssistant,
                              ChatClientId<"anthropic:claude-sonnet-5">,
                              Tools<LookupOrderTool>,
                              MaxTurns<6>> {
    static constexpr std::string_view name         = "order-assistant";
    static constexpr std::string_view instructions =
        "You answer a small business's questions about customer orders. Look the order up with the "
        "lookup_order tool; never guess an order's status. Answer in one short sentence.";
};

// The engine's error model in one place: every fallible call returns result<T>, and on failure
// error() carries a stable machine-readable `code` and host-facing `message`. Print both, exit 1.
[[nodiscard]] int report_failure(char const* stage, error const& e) {
    std::fprintf(stderr, "%s failed: %s (code %s)\n", stage, e.message.c_str(), e.code.c_str());
    return 1;
}

// Deployment configuration, read from the environment with documented defaults -- the host, port,
// model id and path prefix are properties of the DEPLOYMENT, not of the program (018 §4's rule is
// about the credential, but the same reasoning applies to every other constructor argument here).
// This also means one build can be pointed at any Anthropic-wire endpoint without a recompile.
[[nodiscard]] std::string env_or(char const* name, char const* fallback) {
    if (char const* v = std::getenv(name); v != nullptr && *v != '\0') return std::string{v};
    return std::string{fallback};
}

}  // namespace

int main(int argc, char** argv) {
    std::string const question =
        (argc > 1) ? std::string{argv[1]} : std::string{"What is the status of order A-1042?"};

    // -- The credential: resolved from the environment, never compiled in ------------------------
    // EnvSecretSource's prefix DEFAULTS to the historical "QUARK_SECRET_", so passing "" is what
    // makes the bare name ANTHROPIC_API_KEY the variable that is read.
    auto store = std::make_unique<AgentEngineSecretStore>(std::make_unique<EnvSecretSource>(""));

    // Which environment variable holds the key, and which endpoint/model to talk to. All five are
    // deployment configuration with documented defaults; none is the credential itself.
    std::string const key_var   = env_or("AE_API_KEY_ENV", "ANTHROPIC_API_KEY");
    std::string const host      = env_or("AE_MODEL_HOST", "api.anthropic.com");
    auto const        port      = static_cast<std::uint16_t>(std::stoi(env_or("AE_MODEL_PORT", "443")));
    std::string const model     = env_or("AE_MODEL_ID", "claude-sonnet-5");
    std::string const path      = env_or("AE_MODEL_PATH_PREFIX", "/v1");

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.tool_calling      = true;   // defaults to FALSE: with Tools<...> declared and this left
                                     // false, the model simply answers and never calls the tool.
    caps.max_output_tokens = 512;

    using RealClient = anthropic::AnthropicChatClient<AgentEngineSecretStore>;
    RealClient client{host, port, model, SecretRef{key_var}, caps, *store, path};

    // The credential grant is deployment configuration, handed to the session explicitly.
    // cap::Secret is the ONLY capability this field accepts; omitting it compiles cleanly and then
    // fails the first run with secret.not_granted.
    AgentSessionOptions opts;
    opts.session_id         = "s-order-assistant";
    opts.principal          = Principal{"cli-user", "small-business"};
    opts.chat_client_grants = {Capability{cap::Secret{key_var}}};

    // -- The agent "build": register_agent<A>()'s whole policy validation, then the session bind --
    auto agent = make_agent_session<OrderAssistant>(client, opts);
    if (!agent.has_value()) {
        return report_failure("agent build", agent.error());
    }

    // -- One turn ---------------------------------------------------------------------------------
    auto reply = agent->ask(question);
    if (!reply.has_value()) {
        return report_failure("run", reply.error());
    }

    std::printf("%s\n", reply->c_str());
    return 0;
}
