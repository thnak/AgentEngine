// AgentEngine "get started" examples, 30 -- QuickstartSessionBuilder, the ergonomic app-developer
// surface, against a REAL model.
//
// Every other example in this directory builds an AgentSession (or a Workflow) by hand: pick a
// ChatClientT, `emplace_chat_client()`, wire a HistoryProvider, set capabilities, drive `start_run()`.
// That's the right layer for the engine itself, but it's a lot of ceremony for an application that
// just wants to ask a model something. `core/session_builder.hpp`'s `QuickstartSessionBuilder` (and
// its provider-specific aliases `OpenAiSessionBuilder`/`AnthropicSessionBuilder`) is that shorter
// path: `.endpoint(...)`/`.api_key_from_env(...)`/`.declare_capabilities(...)`/`.build()` returns a
// `result<Bundle>` wrapping a real, already-wired `AgentSession` behind a `ModelCallGateway`, and
// `Bundle::ask("...")` is the one-shot round trip -- a plain `result<std::string>`, no session/turn
// vocabulary required from the caller at all. See the Builder API page for the native
// `WorkflowBuilder`/`MagenticWorkflowBuilder` half of this same "ergonomic surface" story.
//
// Mirrors tests/test_session_builder_openrouter_live_e2e.cpp's QS-1 (`Bundle::ask()`) end to end --
// this file is the narrative "here's what it looks like to actually use it" counterpart, not another
// proof of the same claims (see that test for the fuller QS-2/QS-3 credential/streaming coverage).
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via
// `tools/run-live-provider-tests.ps1`, or set it yourself. SKIPS (exit 0), same as every other
// live-network example in this repo, when it's absent.
//
// Run: ./agentengine_example_30_session_builder_quickstart_live

#include <cstdio>
#include <string>

#include "agentengine/core/session_builder.hpp"
#include "agentengine/pal/env.hpp"

using namespace agentengine;
using quickstart::OpenAiSessionBuilder;

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

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Same OpenRouter-hosted OpenAI-family model every other live example/test in this suite uses.
constexpr char const* kDefaultModel = "openai/gpt-4o-mini";
constexpr char const* kDefaultHost  = "openrouter.ai";
constexpr std::uint16_t kHttpsPort  = 443;
constexpr char const* kPathPrefix   = "/api/v1";
constexpr char const* kSecretName   = "openrouter-api-key";

}  // namespace

int main() {
    auto const key_env = pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "example_30_session_builder_quickstart_live: SKIPPED -- "
                      "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                      "tools/run-live-provider-tests.ps1, or set the variable yourself, to run this "
                      "example against a real model.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_OPENAI_MODEL", kDefaultModel);
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.tool_calling      = true;
    caps.max_output_tokens = 256;

    // The whole "quickstart" surface: name a model, point it at an endpoint, resolve the API key
    // from an environment variable (never compiled in -- 018 §4), declare what the backend can do,
    // and build. No AgentSession, ChatClientT, or HistoryProvider vocabulary appears anywhere here.
    auto built = OpenAiSessionBuilder(model)
                     .endpoint(host, kHttpsPort, kPathPrefix)
                     .api_key_from_env(kSecretName, "AGENTENGINE_OPENROUTER_API_KEY")
                     .declare_capabilities(caps)
                     .build();
    check(built.has_value(), "the Bundle builds: credential resolved, session wired end to end");
    if (!built.has_value()) {
        std::fprintf(stderr, "       error: %s (%s)\n", built.error().message.c_str(),
                     built.error().code.c_str());
        return 1;
    }

    auto reply = built->ask("Reply with exactly one word: pong");
    check(reply.has_value(), "Bundle::ask() succeeds end to end against a real remote model");
    if (reply.has_value()) {
        std::fprintf(stderr, "  .. ask() reply = %s\n", reply->c_str());
        check(!reply->empty(), "the returned text is non-empty");
    } else {
        std::fprintf(stderr, "       error: %s (%s)\n", reply.error().message.c_str(),
                     reply.error().code.c_str());
    }

    std::fprintf(stderr, g_failures == 0 ? "example_30_session_builder_quickstart_live: OK\n"
                                          : "example_30_session_builder_quickstart_live: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
