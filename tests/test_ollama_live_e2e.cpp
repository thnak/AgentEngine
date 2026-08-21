// Proves 004-Model-Provider-Plane.md §3's OpenAI-compatible backend (protocol/openai/chat_client.hpp)
// end to end against a REAL, LOCAL Ollama server -- the other local-server target that section's own
// table names by name ("vLLM/llama.cpp/Ollama-style local servers"), alongside
// tests/test_llamacpp_live_e2e.cpp, which already proves the llama.cpp half. This file mirrors that
// one's EXACT pattern (ADR-016's `resolve_host`/`plaintext_http` opt-in for a local server, real
// resolver/secret/capability path, structural-only assertions, `live-network` ctest label, SKIP not
// FAIL when unconfigured) -- see that file's own top comment for the ADR-016 rationale, not repeated
// here.
//
// Sourced from Ollama's own docs, fetched 2026-08-21 (docs/research/2026-08-21-ollama-openai-compat-
// api.md): base URL `http://localhost:11434/v1/`, an `Authorization` header value that must be
// PRESENT but is ignored (the doc's own SDK example literally uses `"ollama"`), and tool calling /
// `response_format` / streaming all confirmed on the stable OpenAI-compat surface. ONE real,
// documented uncertainty carried into OL-4 below: Ollama's `response_format` support is documented
// only as "JSON mode" -- not confirmed whether that means the full OpenAI Structured-Outputs shape
// (`{"type":"json_schema","json_schema":{...}}`, what this project's own `translate_output_schema`
// sends) or just the older schema-less `{"type":"json_object"}` mode. `end_user_id` is left empty
// throughout: Ollama's docs list `user` as an explicitly UNSUPPORTED request field, so asserting
// anything about it here would test a field the vendor's own docs say goes nowhere.
//
// I5 (nondeterminism crosses a recorded seam) IS RESPECTED BY EXCLUSION: a live model is
// nondeterministic, so this test is gated on an explicitly-set environment variable, carries the
// ctest label `live-network`, and asserts only STRUCTURAL properties -- never on what the model
// actually said.
//
// Configuration (no endpoint is compiled in as a default that would make this run by accident):
//   AGENTENGINE_OLLAMA_MODEL     required -- unset means SKIP (exit 0), never a failure. Must name a
//                                 model already pulled on the target Ollama instance (`ollama pull
//                                 <name>`) -- there is no universal default that would exist on an
//                                 arbitrary contributor's machine.
//   AGENTENGINE_OLLAMA_HOST      optional -- default 127.0.0.1
//   AGENTENGINE_OLLAMA_PORT      optional -- default 11434 (Ollama's own documented default)
//   AGENTENGINE_OLLAMA_PREFIX    optional -- prepended to every prompt, default "" (empty). Same
//                                 escape hatch test_llamacpp_live_e2e.cpp's own
//                                 AGENTENGINE_LLAMACPP_PREFIX provides: `ChatRequest` carries no
//                                 sampling parameters (core/chat_client.hpp's own elision, 004 §1), so
//                                 there is no way from here to bound a reasoning model's thinking
//                                 budget, and the HTTP read loop's idle timeout (net_egress_proxy.cpp's
//                                 kIoTimeoutMs, 10s) applies to a non-streaming response as ONE single
//                                 wait. Set this to a thinking-off directive if the configured model
//                                 needs one.
//
// THIS FILE HAS NOT BEEN RUN AGAINST A REAL OLLAMA SERVER: this development machine has no local
// Ollama instance (confirmed 2026-08-21, connection refused on the default port). Compiled and its
// SKIP path verified, but every live assertion below is unverified until a contributor with a real
// Ollama instance runs it -- see docs/research/2026-08-21-ollama-openai-compat-api.md's own closing
// note.

#include "agentengine/pal/env.hpp"
#include "agentengine/pal/net.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;

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

void report(char const* what, error const& e) {
    std::fprintf(stderr, "       %s: %s (%s)\n", what, e.message.c_str(), e.code.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

std::string g_prompt_prefix;

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{g_prompt_prefix + std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    req.messages.push_back(user_message(std::move(text)));
    return req;
}

// 006's real per-run tool table entry -- the exact type ChatRequest::tools already carries.
[[nodiscard]] ToolDescriptor weather_tool() {
    ToolDescriptor t;
    t.name = "get_weather";
    t.description = "Get the current weather for a city.";
    t.args_schema_json =
        R"({"type":"object","properties":{"location":{"type":"string","description":"City name"}},)"
        R"("required":["location"]})";
    return t;
}

constexpr char const* kCapitalSchema =
    R"({"type":"object","properties":{"capital":{"type":"string"}},"required":["capital"]})";

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

[[nodiscard]] std::optional<ToolCall> first_tool_call(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (auto const* tc = std::get_if<ToolCall>(&item.value)) return *tc;
    }
    return std::nullopt;
}

[[nodiscard]] Message assistant_tool_call_message(ToolCall call) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = std::move(call);
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] Message tool_result_message(std::string call_id, std::string json_text) {
    Message m;
    m.role = role::tool;
    ToolResult tr;
    tr.call_id = std::move(call_id);
    ContentItem inner;
    inner.origin = content_origin::tool;
    inner.value = Data{std::move(json_text), std::nullopt};
    tr.content.push_back(std::move(inner));
    ContentItem item;
    item.origin = content_origin::tool;
    item.value = std::move(tr);
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
    auto const model_env = ::agentengine::pal::env_var("AGENTENGINE_OLLAMA_MODEL");
    if (!model_env || model_env->empty()) {
        std::fprintf(stderr,
                     "test_ollama_live_e2e: SKIPPED -- AGENTENGINE_OLLAMA_MODEL is not set.\n"
                     "  Start Ollama (`ollama serve`), pull a model (`ollama pull <name>`), set "
                     "AGENTENGINE_OLLAMA_MODEL=<name>, and re-run to exercise it.\n");
        return 0;
    }

#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    std::string const host = env_or("AGENTENGINE_OLLAMA_HOST", "127.0.0.1");
    std::string const port_str = env_or("AGENTENGINE_OLLAMA_PORT", "11434");
    g_prompt_prefix = env_or("AGENTENGINE_OLLAMA_PREFIX", "");

    unsigned port_value = 0;
    for (char const c : port_str) {
        if (c < '0' || c > '9' || port_value > 65535) {
            std::fprintf(stderr, "test_ollama_live_e2e: AGENTENGINE_OLLAMA_PORT ('%s') is not a port.\n",
                         port_str.c_str());
            return 1;
        }
        port_value = port_value * 10 + static_cast<unsigned>(c - '0');
    }
    if (port_value == 0 || port_value > 65535) {
        std::fprintf(stderr, "test_ollama_live_e2e: AGENTENGINE_OLLAMA_PORT ('%s') is out of range.\n",
                     port_str.c_str());
        return 1;
    }
    auto const port = static_cast<std::uint16_t>(port_value);

    std::fprintf(stderr, "test_ollama_live_e2e: endpoint=http://%s:%u/v1 model=%s prefix=\"%s\"\n",
                 host.c_str(), port_value, model_env->c_str(), g_prompt_prefix.c_str());

    // Ollama needs no REAL credential (docs/research/2026-08-21-ollama-openai-compat-api.md: the
    // Authorization header's value is ignored, but must be present), but the credential PATH is
    // exercised for real: a SecretStore, a cap::Secret grant, and resolution at the point of use
    // inside chat() (004 §1, 018 §4). "ollama" is the exact placeholder value the vendor's own SDK
    // example uses.
    constexpr char const* kSecretName = "ollama-api-key";
    InMemorySecretStore store;
    store.set(kSecretName, "ollama");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"ollama-live-e2e-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ChatClientCapabilities caps;  // DECLARED, never probed -- confirmed on Ollama's stable OpenAI-
    caps.tool_calling = true;      // compat surface per this file's own research doc.
    caps.structured_output_native = true;

    std::string const model = *model_env;

    // Exactly how a real deployment targeting a local server constructs it: real resolver, real
    // (empty) CA override, and ONE explicitly-named non-default -- the transport (ADR-016 §3:
    // appended, never inserted). `end_user_id` left empty -- Ollama's docs list `user` as explicitly
    // UNSUPPORTED (see this file's own top comment).
    openai::OpenAIChatClient client(host, port, model, SecretRef{kSecretName}, caps, store, "/v1",
                                     /*resolver=*/sandbox::resolve_host,
                                     /*ca_bundle_pem_override=*/{},
                                     /*http_referer=*/{}, /*x_title=*/{}, /*end_user_id=*/{},
                                     /*seed=*/std::nullopt,
                                     ProviderTransport::plaintext_http);
    static_assert(ChatClient<decltype(client)>,
                  "OpenAIChatClient must satisfy the real ChatClient concept (004 §1)");

    using agentengine::test_support::run_task_sync;

    // ---- OL-1: the REAL OpenAIChatClient against a real local Ollama model --------------------------
    {
        auto resp = run_task_sync<result<ChatResponse>>(
            client.chat(request_asking("Reply with exactly one word: pong"), ctx));
        check(resp.has_value(),
              "OL-1: the REAL OpenAIChatClient -- real address resolution, real secret resolution, "
              "real request build, real HTTP exchange, real response parse -- completes against a "
              "real Ollama server, mirroring what ADR-016 G5 already proves for llama.cpp");
        if (!resp) {
            report("error", resp.error());
            std::fprintf(stderr,
                         "       (net.connect_failed here means Ollama is not listening on the "
                         "configured port, or the read timed out -- a reasoning model can exceed the "
                         "10s idle timeout, see AGENTENGINE_OLLAMA_PREFIX)\n");
        } else {
            check(!text_of(resp->message).empty(), "OL-1: the assistant turn carries non-empty Text");
            check(resp->message.role == role::assistant, "OL-1: the reply is an assistant turn");
            check(resp->usage.input_tokens > 0 && resp->usage.output_tokens > 0,
                  "OL-1: usage.prompt_tokens/completion_tokens are reported by the real server and map "
                  "onto the same portable Usage fields a hosted provider's do (I6)");
            check(!resp->model.empty(), "OL-1: ChatResponse::model reports the model that answered");
            note("reply", text_of(resp->message));
            note("model", resp->model);
        }
    }

    // ---- OL-2: tool calling -- 006's tool table projected onto a real local Ollama model -------------
    std::optional<ToolCall> call;
    {
        ChatRequest req = request_asking("What is the weather in Seattle? Call the tool.");
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "OL-2: translate_tool's `{type:function, function:{name,description,parameters}}` "
              "projection is accepted by Ollama's OpenAI-compat surface -- confirmed on its stable "
              "surface per this file's own research doc, now checked against a real server");
        if (!resp) {
            report("error", resp.error());
        } else {
            call = first_tool_call(resp->message);
            check(call.has_value(), "OL-2: the local model returned a ToolCall content item");
            if (call) {
                check(call->tool_name == "get_weather", "OL-2: the declared tool name round-trips");
                check(!call->call_id.empty(), "OL-2: the server-issued call id is preserved");
                auto args = json::parse(call->arguments_json);
                check(args.has_value(), "OL-2: arguments_json is valid JSON as received");
                check(args && args->find("location") != nullptr,
                      "OL-2: the arguments carry the `location` property the declared schema required");
                note("tool arguments", call->arguments_json);
            }
        }
    }

    // ---- OL-3: the tool RESULT turn is accepted back --------------------------------------------------
    if (call) {
        ChatRequest req;
        req.messages.push_back(user_message("What is the weather in Seattle? Call the tool."));
        req.messages.push_back(assistant_tool_call_message(*call));
        req.messages.push_back(tool_result_message(call->call_id, R"({"location":"Seattle","temp_c":11})"));
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "OL-3: a history containing an assistant `tool_calls` message AND a `tool_call_id` reply "
              "message is accepted by Ollama -- translate_message's two-sided tool shape is correct "
              "against a THIRD independent implementation of the same wire contract (OpenAI, "
              "llama.cpp, now Ollama)");
        if (!resp) {
            report("error", resp.error());
        } else {
            check(!text_of(resp->message).empty(),
                  "OL-3: the model answered from the supplied tool result -- a full two-turn tool loop "
                  "completed end to end");
            note("post-tool reply", text_of(resp->message));
        }
    }

    // ---- OL-4: structured output -- the ONE genuinely uncertain check in this file ------------------
    // Ollama's docs describe `response_format` support only as "JSON mode", not confirmed to be the
    // full OpenAI Structured-Outputs shape this project's translate_output_schema sends (see this
    // file's own top comment). A failure here may mean Ollama accepted the request but ignored the
    // schema, or genuinely rejected it -- either is a real, useful, live finding, not necessarily a
    // code defect. Reported clearly either way rather than silently softened into a non-assertion.
    {
        ChatRequest req = request_asking("Give the capital of France.");
        req.output_schema_json = kCapitalSchema;
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "OL-4: translate_output_schema's `response_format` wrapper is ACCEPTED by Ollama's "
              "OpenAI-compat endpoint (documented only as 'JSON mode' -- see this file's own top "
              "comment for the real uncertainty this checks)");
        if (!resp) {
            report("error", resp.error());
        } else {
            std::string const body = text_of(resp->message);
            auto parsed = json::parse(body);
            check(parsed.has_value(), "OL-4: the reply text is itself valid JSON");
            check(parsed && parsed->find("capital") != nullptr,
                  "OL-4: the returned object carries the schema's required `capital` key -- if this "
                  "fails while the request was still ACCEPTED above, it is evidence Ollama's 'JSON "
                  "mode' ignores the schema body rather than enforcing it, which is worth filing as a "
                  "follow-up finding, not silently re-running");
            note("structured reply", body);
        }
    }

    // ---- OL-5: TLS is still the default -- the same endpoint is NOT reachable without opting in -----
    {
        openai::OpenAIChatClient tls_client(host, port, model, SecretRef{kSecretName}, caps, store, "/v1");
        auto resp = run_task_sync<result<ChatResponse>>(tls_client.chat(request_asking("hi"), ctx));
        check(!resp.has_value(),
              "OL-5 (positive control): a DEFAULT-constructed client -- identical in every respect "
              "except that it does not opt into plaintext -- cannot reach the same plaintext server");
        if (!resp) {
            note("TLS-default refusal", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- OL-6: chunked/streamed SSE over the plaintext transport --------------------------------------
    {
        stream<ChatResponseUpdate> s = client.chat_stream(request_asking("Reply with exactly one word: pong"), ctx);
        std::vector<ChatResponseUpdate> updates;
        while (!s.done()) {
            while (auto u = s.next()) updates.push_back(std::move(*u));
            if (!s.done()) std::this_thread::yield();
        }
        std::string streamed;
        for (auto const& u : updates) {
            if (auto const* t = std::get_if<Text>(&u.delta.value)) streamed += t->text;
        }
        check(s.terminal() == stream_terminal::closed,
              "OL-6: a real streaming exchange over PLAINTEXT reaches the success terminal");
        check(!streamed.empty(),
              "OL-6: Ollama's own SSE response decodes into non-empty text over the plaintext "
              "transport (the chunked decode is the backend's, not the transport's, ADR-016 §3)");
        if (!updates.empty()) {
            check(updates.back().is_final, "OL-6: the last delivered update is flagged final");
        }
        note("stream terminal", s.terminal() == stream_terminal::closed ? "Closed" : "Failed");
        note("stream updates", std::to_string(updates.size()));
        note("stream text", streamed);
    }

    // ---- OL-7: an ungranted capability fails closed BEFORE any egress (I2) ---------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities = agentengine::borrow_capabilities(empty);
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), denied));
        check(!resp.has_value(),
              "OL-7 (I2): with no cap::Secret grant the call is denied at the point of use and never "
              "reaches the network, even though a real, reachable, working endpoint is sitting right "
              "there");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OL-7: a capability denial is classified `policy`");
        }
    }

    // ---- OL-8: the GUEST path is still refused at this very same live server -------------------------
    {
        sandbox::HostEgressProxy proxy;  // real resolver, exactly as production constructs it
        cap::NetOut granted;
        granted.host_allowlist.push_back(host + ":" + std::to_string(port_value) + ":http");
        sandbox::NetEgressRequest req;
        req.method = "GET";
        req.path = "/v1/models";
        auto resp = proxy.fetch(req, granted);
        check(!resp.has_value() && resp.error().code == "net.address_blocked",
              "OL-8 (ADR-016 G1): a WASM guest holding a cap::NetOut grant that explicitly allowlists "
              "this exact live local server is STILL refused on address policy");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_ollama_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_ollama_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
