// End-to-end proof of `core/session_builder.hpp`'s `Bundle::ask()`/`Bundle::ask_stream()` against a
// REAL, remote inference service (OpenRouter) -- the one surface `test_session_builder_prototype.cpp`
// explicitly named as out of its own scope: "no .ask()/start_run() call, so no real (or even
// attempted) network exchange happens in this test... A live, real-network proof of .ask() end to end
// is separately scoped future work." This file is that work, extended to `.ask_stream()`
// (unified-streaming-design-draft.md §4, Piece D) as well, since that method has NEVER been exercised
// by any test before this one -- its own method body was never even instantiated by the compiler until
// this file called it (C++ member-function templates are lazily instantiated; the prototype test never
// triggered it).
//
// Also closes a real bug `Bundle::ask_stream()` had that neither adversarial red-team pass caught,
// found while writing this exact test: without `session_->set_stream_model_calls(true)` inside
// `ask_stream()` itself, `stream_model_calls_` stays at its default `false`, so `run_model_call()`'s
// own dispatch never reaches `ModelCallGateway::call_stream()` at all -- the relay thread would then
// see only `run_started`/the terminal event, never a single `model_delta`, and `ask_stream()` would
// silently return an EMPTY text stream regardless of what the model actually said. Fixed in
// `session_builder.hpp` before this test was written against the fix, not discovered by running a
// broken version of this file.
//
// Mirrors tests/test_openai_chat_client_openrouter_live_e2e.cpp's and tests/test_openrouter_live_e2e.
// cpp's EXACT pattern (env-var-gated credential, SKIP not FAIL when absent, structural-only assertions
// on a live model's nondeterministic output, a positive control proving the credential is genuinely
// load-bearing) -- see either file's own top comment for the full rationale, not repeated here. Unlike
// those two, this file drives the FULL stack (`QuickstartSessionBuilder` -> `Bundle` -> real
// `ModelCallGateway<OpenAIChatClient<...>>` -> `AgentSession`), not the raw `ChatClient` layer alone --
// the point of this file is proving that whole composition actually works end to end for the first
// time, live.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_OPENROUTER_API_KEY          required -- unset means SKIP (exit 0), never a failure.
//     (Reuses the SAME key every other live test in this suite uses; tools/run-live-provider-tests.ps1
//     populates it from a local, git-ignored key file.)
//   AGENTENGINE_OPENROUTER_OPENAI_MODEL     optional -- default below, the same concretely-named
//     OpenAI-family model test_openai_chat_client_openrouter_live_e2e.cpp already confirmed live.
//   AGENTENGINE_OPENROUTER_HOST             optional -- default `openrouter.ai`.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "agentengine/core/session_builder.hpp"

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

void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Same model test_openai_chat_client_openrouter_live_e2e.cpp already confirmed live (2026-08-21):
// works on OpenRouter's synchronous /v1/chat/completions endpoint, reports a real, recognizable model.
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
                      "test_session_builder_openrouter_live_e2e: SKIPPED -- "
                      "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                      "tools/run-live-provider-tests.ps1, or set the variable yourself, to exercise "
                      "the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_OPENAI_MODEL", kDefaultModel);
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_session_builder_openrouter_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.tool_calling      = true;
    caps.max_output_tokens = 256;

    // ---- QS-1: Bundle::ask() -- the quickstart facade's synchronous round trip, live, for the -------
    // ---- first time ever (test_session_builder_prototype.cpp's own named scope gap) -----------------
    {
        auto built = OpenAiSessionBuilder(model)
                         .endpoint(host, kHttpsPort, kPathPrefix)
                         .api_key_from_env(kSecretName, "AGENTENGINE_OPENROUTER_API_KEY")
                         .declare_capabilities(caps)
                         .build();
        check(built.has_value(), "QS-1 setup: the Bundle builds (credential resolved, session wired)");
        if (built.has_value()) {
            auto reply = built->ask("Reply with exactly one word: pong");
            check(reply.has_value(),
                  "QS-1: Bundle::ask() succeeds end to end -- QuickstartSessionBuilder -> Bundle -> a "
                  "real ModelCallGateway<OpenAIChatClient<...>> -> AgentSession, against a real remote "
                  "model, over real DNS/TLS");
            if (!reply) {
                std::fprintf(stderr, "       error: %s (%s)\n", reply.error().message.c_str(),
                             reply.error().code.c_str());
            } else {
                check(!reply->empty(), "QS-1: the returned text is non-empty");
                note("ask() reply", *reply);
            }
        }
    }

    // ---- QS-2: Bundle::ask_stream() -- NEVER exercised by any test before this one; its own method --
    // ---- body was never even instantiated until this call. Piece D's real proof. --------------------
    {
        auto built = OpenAiSessionBuilder(model)
                         .endpoint(host, kHttpsPort, kPathPrefix)
                         .api_key_from_env(kSecretName, "AGENTENGINE_OPENROUTER_API_KEY")
                         .declare_capabilities(caps)
                         .build();
        check(built.has_value(), "QS-2 setup: a second, independent Bundle builds");
        if (built.has_value()) {
            auto streamed = built->ask_stream("Count from one to five, one number per word, no punctuation.");
            check(streamed.has_value(),
                  "QS-2: Bundle::ask_stream() returns a real stream<std::string> -- the driver/relay "
                  "thread pair spun up without error");
            if (!streamed) {
                std::fprintf(stderr, "       error: %s (%s)\n", streamed.error().message.c_str(),
                             streamed.error().code.c_str());
            } else {
                std::string joined;
                std::size_t chunk_count = 0;
                agentengine::stream<std::string> s = std::move(*streamed);
                while (!s.done()) {
                    while (auto chunk = s.next()) {
                        joined += *chunk;
                        ++chunk_count;
                    }
                    if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(s.terminal() == stream_terminal::closed,
                      "QS-2: the stream reaches a clean success terminal, not a failure/cancellation");
                check(chunk_count >= 1,
                      "QS-2: at least one live text chunk was actually pushed through the relay "
                      "thread -- proving set_stream_model_calls(true) really did reach "
                      "run_model_call()'s dispatch and call_stream() really did get used, not "
                      "silently fell back to the buffered call() path");
                check(!joined.empty(), "QS-2: the concatenated live chunks are non-empty");
                note("ask_stream() chunk_count", std::to_string(chunk_count));
                note("ask_stream() joined text", joined);
            }
        }
    }

    // ---- QS-3: POSITIVE CONTROL -- a syntactically valid but WRONG credential is rejected by the -----
    // ---- real service, and that rejection survives the FULL Bundle/gateway/session stack, not just --
    // ---- the raw backend (test_openai_chat_client_openrouter_live_e2e.cpp's OC-3 already proves the -
    // ---- raw-backend case; this proves the same fact reaches Bundle::ask()'s own result<T> honestly) -
    {
        // A second env var, set only for this block, never compiled in (018 §4) -- api_key_from_env()
        // is the ONLY way this builder ever populates a secret VALUE (session_builder.hpp's own top
        // comment), so a wrong credential has to reach it the same way a real one does.
#ifdef _WIN32
        _putenv_s("AGENTENGINE_OPENROUTER_BAD_KEY",
                  "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000");
#else
        setenv("AGENTENGINE_OPENROUTER_BAD_KEY",
               "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000", 1);
#endif
        auto built = OpenAiSessionBuilder(model)
                         .endpoint(host, kHttpsPort, kPathPrefix)
                         .api_key_from_env(kSecretName, "AGENTENGINE_OPENROUTER_BAD_KEY")
                         .declare_capabilities(caps)
                         .build();
        check(built.has_value(), "QS-3 setup: the Bundle builds -- the wrong key is syntactically fine, "
                                  "build() has no way to know it's wrong without a real network call");
        if (built.has_value()) {
            auto reply = built->ask("hi");
            check(!reply.has_value(),
                  "QS-3 (positive control): a syntactically valid but WRONG api key is rejected by the "
                  "real service -- proving QS-1/QS-2's own credential was genuinely load-bearing, not "
                  "just present");
            if (!reply) {
                check(reply.error().klass == failure_class::policy,
                      "QS-3: an authentication rejection is classified 'policy' (401/403), surviving "
                      "translation through ModelCallGateway::call() -> AgentSession::run_model_call() "
                      "-> Bundle::ask()'s own result<std::string>");
                note("auth failure", reply.error().code + ": " + reply.error().message);
            }
        }
#ifdef _WIN32
        _putenv_s("AGENTENGINE_OPENROUTER_BAD_KEY", "");
#else
        unsetenv("AGENTENGINE_OPENROUTER_BAD_KEY");
#endif
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_session_builder_openrouter_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_session_builder_openrouter_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
