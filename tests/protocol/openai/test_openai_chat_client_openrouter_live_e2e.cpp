// End-to-end proof that `openai::OpenAIChatClient<>` -- AgentEngine's real OpenAI-shaped ChatClient
// conformer, unmodified -- works correctly when pointed at OpenRouter's host with a CONCRETELY NAMED
// OpenAI model (e.g. "openai/gpt-4o-mini"), not the routing ALIAS
// (`~deepseek/deepseek-v4-flash-latest`) tests/protocol/openai/test_openrouter_live_e2e.cpp already exercises. That
// file's own OR-OAI-1 note explicitly can't assert much about `ChatResponse::model` because an alias
// resolves server-side to whichever backend OpenRouter happens to route to; a concretely named vendor
// model makes that field a meaningful, checkable claim instead -- which is the one thing this file
// adds over the existing suite. Confirmed live, 2026-08-21, before this file was written: real
// api.openai.com traffic answers "I was created by OpenAI." through this exact class/host/model
// combination -- see docs/research/2026-08-21-openrouter-batch-api.md's sibling discovery
// (`openai/gpt-4o-mini` is also the one model confirmed to support OpenRouter's beta Batch endpoint,
// unlike the deepseek alias).
//
// Mirrors tests/protocol/openai/test_openrouter_live_e2e.cpp's and tests/protocol/openai/test_openai_embedder_openrouter_live_e2e.cpp's
// EXACT pattern (env-var-gated credential, SKIP not FAIL when absent, structural-only assertions, a
// positive control proving the credential is load-bearing, and an I2 capability-denial control) -- see
// either file's own top comment for the full rationale, not repeated here. Deliberately does NOT
// re-prove tool calling / structured output / reasoning-effort gating: those are already proven
// against this exact class (test_openrouter_live_e2e.cpp's OR-OAI-3/4/5), and re-running them against
// a second model would add live-API cost without adding a new claim -- the ONLY new claim here is
// "a concretely named vendor model, not an alias, is reachable through this class/host pair."
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_OPENROUTER_API_KEY          required -- unset means SKIP (exit 0), never a failure.
//     (Reuses the SAME key every other live test in this suite uses.)
//   AGENTENGINE_OPENROUTER_OPENAI_MODEL     optional -- default below, confirmed live to work.
//   AGENTENGINE_OPENROUTER_HOST             optional -- default `openrouter.ai`.
//   AGENTENGINE_OPENROUTER_PATH_PREFIX      optional -- default `/api/v1`. Set to `/v1` for a
//                                           vendor endpoint such as api.deepseek.com.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "../../support/run_task_sync.hpp"

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

void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Confirmed live, 2026-08-21: accepts both OpenRouter's synchronous /v1/chat/completions endpoint AND
// its beta :batch endpoint (tools/batch_infer.cpp's own default) -- unlike the routing alias
// test_openrouter_live_e2e.cpp uses, which 400s on :batch and (being an alias) makes `resp->model`
// unpredictable.
constexpr char const* kDefaultModel = "openai/gpt-4o-mini";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";

// The endpoint's path prefix. OpenRouter serves its OpenAI-compatible surface under `/api/v1`; a
// vendor's own endpoint typically serves `/v1` (DeepSeek: `https://api.deepseek.com/v1`, confirmed
// live 2026-09-18). Host, model and key were already environment-driven -- this constant was the one
// remaining thing pinning these tests to a single provider, so it is overridable too, via
// AGENTENGINE_OPENROUTER_PATH_PREFIX. Read through a function-local static because call sites below
// sit outside main(), where no local could reach them.
// The family token the answering model's id must still contain -- derived from the model actually
// CONFIGURED rather than hardcoded to one vendor, so that pointing this test at a second provider
// keeps the check meaningful instead of turning it into a guaranteed failure. `openai/gpt-4o-mini`
// yields `gpt`; `deepseek-flash` yields `deepseek`. Overridable for an id whose family token is not
// its own first segment.
[[nodiscard]] std::string model_family_token(std::string const& model) {
    std::string tail = model.substr(model.rfind('/') + 1);
    if (!tail.empty() && tail.front() == '~') tail.erase(0, 1);
    auto const cut = tail.find_first_of("-.:_0123456789");
    if (cut != std::string::npos && cut > 0) tail.resize(cut);
    return env_or("AGENTENGINE_OPENROUTER_MODEL_FAMILY", tail);
}

[[nodiscard]] std::string const& path_prefix() {
    static std::string const value = env_or("AGENTENGINE_OPENROUTER_PATH_PREFIX", kPathPrefix);
    return value;
}

constexpr char const* kSecretName = "openrouter-api-key";
constexpr char const* kXTitle = "AgentEngine: openai-via-openrouter-live-e2e";

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    req.messages.push_back(user_message(std::move(text)));
    return req;
}

[[nodiscard]] std::vector<ChatResponseUpdate> drain(stream<ChatResponseUpdate>& s) {
    std::vector<ChatResponseUpdate> out;
    while (!s.done()) {
        while (auto update = s.next()) out.push_back(std::move(*update));
        if (!s.done()) std::this_thread::yield();
    }
    return out;
}

[[nodiscard]] std::string text_of(std::vector<ChatResponseUpdate> const& updates) {
    std::string out;
    for (auto const& u : updates) {
        if (auto const* t = std::get_if<Text>(&u.delta.value)) out += t->text;
    }
    return out;
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "test_openai_chat_client_openrouter_live_e2e: SKIPPED -- "
                      "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                      "tools/run-live-provider-tests.ps1, or set the variable yourself, to exercise "
                      "the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_OPENAI_MODEL", kDefaultModel);
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::string const family = model_family_token(model);
    std::fprintf(stderr, "test_openai_chat_client_openrouter_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal    = Principal{"live-e2e-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    using agentengine::test_support::run_task_sync;

    ChatClientCapabilities caps;
    caps.streaming        = true;
    caps.tool_calling     = true;
    caps.max_output_tokens = 256;

    // Default resolver AND default CA bundle, same as every other file in this live suite.
    openai::OpenAIChatClient<InMemorySecretStore> oai(
        host, kHttpsPort, model, SecretRef{kSecretName}, caps, store, path_prefix(), sandbox::resolve_host,
        /*ca=*/{}, /*http_referer=*/{}, /*x_title=*/kXTitle,
        /*end_user_id=*/"test-openai-via-openrouter", /*seed=*/std::nullopt,
        /*transport=*/sandbox::ProviderTransport::tls, /*scan_response_format_leaks=*/false,
        /*session_id=*/"test-openai-via-openrouter");

    static_assert(ChatClient<decltype(oai)>, "OpenAIChatClient must satisfy the ChatClient concept");

    // ---- OC-1: chat() succeeds against a CONCRETELY NAMED OpenAI model, real endpoint ----------------
    {
        auto resp = run_task_sync<result<ChatResponse>>(
            oai.chat(request_asking("In one short sentence, which company made you (the model)?"), ctx));
        check(resp.has_value(),
              "OC-1: chat() succeeds against the real OpenRouter Chat Completions endpoint, routed to "
              "a concretely named OpenAI model, over real DNS/TLS -- no injected resolver, no "
              "self-signed leaf anywhere on this path");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            std::string reply;
            for (ContentItem const& item : resp->message.content) {
                if (auto const* t = std::get_if<Text>(&item.value)) reply += t->text;
            }
            check(!reply.empty(), "OC-1: the assistant turn carries non-empty Text");
            note("reply", reply);
            check(!resp->model.empty(), "OC-1: ChatResponse::model reports the model that answered");
            note("reported model", resp->model);
            // Unlike test_openrouter_live_e2e.cpp's routing-ALIAS case (where the reported model is
            // deliberately unpredictable -- that file's own OR-OAI-1 comment), a CONCRETE request here
            // makes "the model that answered is the family we asked for" a meaningful structural
            // check, not a guess. Not asserted byte-equal to `model`: a provider may report the
            // underlying dated snapshot rather than echoing the alias/slug verbatim. The family token
            // comes from the configured model (above), NOT from a hardcoded vendor name -- a hardcoded
            // one made this the single assertion that could not survive a second provider, which is
            // precisely the thing a second provider is here to detect.
            check(resp->model.find(family) != std::string::npos,
                  "OC-1: the reported model is recognizably the family that was asked for, not a silent "
                  "reroute to a different vendor");
            check(resp->usage.input_tokens > 0 && resp->usage.output_tokens > 0,
                  "OC-1: real, nonzero token usage came back");
            note("usage in/out", std::to_string(resp->usage.input_tokens) + "/" +
                                       std::to_string(resp->usage.output_tokens));
        }
    }

    // ---- OC-2: chat_stream() over a real SSE response, same model ------------------------------------
    {
        stream<ChatResponseUpdate> s =
            oai.chat_stream(request_asking("Reply with exactly one word: pong"), ctx);
        auto updates = drain(s);
        check(s.terminal() == stream_terminal::closed,
              "OC-2: a real streaming exchange against a concretely named OpenAI model reaches the "
              "SUCCESS terminal");
        check(!updates.empty(), "OC-2: at least one update was delivered through ae::stream<T>");
        check(!text_of(updates).empty(),
              "OC-2: the concatenated text deltas of a real SSE response are non-empty");
        note("stream text", text_of(updates));
    }

    // ---- OC-3: POSITIVE CONTROL -- a wrong credential is rejected by the real service -----------------
    {
        InMemorySecretStore bad_store;
        bad_store.set(kSecretName,
                       "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000");
        openai::OpenAIChatClient<InMemorySecretStore> bad(
            host, kHttpsPort, model, SecretRef{kSecretName}, caps, bad_store, path_prefix(),
            sandbox::resolve_host);
        auto resp = run_task_sync<result<ChatResponse>>(bad.chat(request_asking("hi"), ctx));
        check(!resp.has_value(),
              "OC-3 (positive control): a syntactically valid but WRONG api key is rejected -- proving "
              "the credential resolved above was genuinely load-bearing");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OC-3: an authentication rejection is classified 'policy' (401/403)");
            note("auth failure", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- OC-4: an ungranted capability fails closed BEFORE any egress (I2) ---------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities  = agentengine::borrow_capabilities(empty);
        auto resp = run_task_sync<result<ChatResponse>>(oai.chat(request_asking("hi"), denied));
        check(!resp.has_value(),
              "OC-4 (I2): with no cap::Secret grant the call is denied at the point of use and never "
              "reaches the network, even though a real, reachable, correctly-credentialed endpoint is "
              "sitting right there");
        if (!resp) {
            check(resp.error().klass == failure_class::policy, "OC-4: a capability denial is classified 'policy'");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_chat_client_openrouter_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_chat_client_openrouter_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
