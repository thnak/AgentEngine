// devsim/p1 -- a small-business order-desk CLI built ON AgentEngine (not part of it).
//
// One declared tool (lookup_order) and one declared agent that may call it -- both in
// order_assistant_decl.hpp -- bound to a real Anthropic backend whose API key is resolved from the
// environment at the point of use. One turn in, one answer out. Any engine failure prints the
// engine's own error message + code and exits non-zero.
//
// Doc surface this is written against (web/marketing/src/data/*.ts):
//   apiContent.ts      minimalToolSnippet / toolSchemaSnippet / toolStaticMembers   -> declaring a tool
//   apiContent.ts      minimalAgentSnippet / heroCodeSnippet                        -> declaring an agent
//   apiContent.ts      helloAgentRunSnippet                                         -> make_agent_session<A>(client)
//   providerContent.ts credentialSnippet / conformerRows / credentialSteps          -> real backend + SecretRef
//   builderContent.ts  quickstartWorkedExampleSnippet                               -> error().message / error().code
// Everything the docs did NOT cover is itemised in REPORT.md's fallback log.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

#include "order_assistant_decl.hpp"

using namespace agentengine;

namespace {

// ---- Deployment configuration: host code, never anything derived from model output (I3). ---------
constexpr char const*   kApiKeyEnv    = "ANTHROPIC_API_KEY";   // the SecretRef *name*, not a value
constexpr char const*   kModelEnv     = "ANTHROPIC_MODEL";
constexpr char const*   kHostEnv      = "ANTHROPIC_HOST";
constexpr char const*   kDefaultModel = "claude-sonnet-5";
constexpr char const*   kDefaultHost  = "api.anthropic.com";
constexpr std::uint16_t kHttpsPort    = 443;
constexpr char const*   kDefaultPrompt =
    "Is order 1042 shipped yet, and what did it cost? Use the order id exactly as given.";

// Requirement 4: print the ENGINE's own message and code -- never a paraphrase, never a swallow.
int report_engine_failure(char const* stage, error const& e) {
    std::fprintf(stderr, "order-assistant: %s failed: %s (code: %s)\n", stage, e.message.c_str(),
                 e.code.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const prompt = argc > 1 ? std::string{argv[1]} : std::string{kDefaultPrompt};
    std::string const model  = pal::env_var(kModelEnv).value_or(kDefaultModel);
    std::string const host   = pal::env_var(kHostEnv).value_or(kDefaultHost);

    // The API key is NEVER read into this program: no getenv of the credential, no std::string copy
    // of it. `EnvSecretSource("")` maps SecretRef{"ANTHROPIC_API_KEY"} onto the environment variable
    // of that exact name, and the backend resolves it inside chat() -- at the point of use -- failing
    // closed with the engine's own `secret.not_found` / `secret.not_granted` when it cannot (018 §4).
    AgentEngineSecretStore secret_store{std::make_unique<EnvSecretSource>("")};

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.tool_calling      = true;
    caps.max_output_tokens = 1024;

    anthropic::AnthropicChatClient<AgentEngineSecretStore> chat_client{
        host, kHttpsPort, model, SecretRef{kApiKeyEnv}, caps, secret_store};

    AgentSessionOptions opts;
    opts.session_id = "order-desk";
    opts.principal  = Principal{"cli-operator", ""};
    // The credential the ChatClient itself resolves. That is deployment configuration, not something
    // the agent *is*, so it rides chat_client_grants and is not narrowed by the agent's ceiling.
    // Without this line the run fails closed with `secret.not_granted` before any socket opens.
    opts.chat_client_grants = {Capability{cap::Secret{kApiKeyEnv, std::chrono::seconds{0}}}};
    // lookup_order reaches no privileged effect, so the session needs no other authority (I2's
    // empty-by-default rule, apiContent.ts:979-984).

    // Build the session from the declared agent, then run exactly one turn.
    auto agent = make_agent_session<order_desk::OrderAssistant>(std::move(chat_client), std::move(opts));
    if (!agent) return report_engine_failure("make_agent_session<OrderAssistant>", agent.error());

    auto reply = agent->ask(prompt);
    if (!reply) return report_engine_failure("ask", reply.error());

    std::printf("%s\n", reply->c_str());
    return 0;
}
