// Proves decisions/ADR-016-provider-egress-address-policy.md's gate G5 -- and, through it,
// 004-Model-Provider-Plane.md §3's OpenAI-compatible backend (protocol/openai/chat_client.hpp)
// end to end against a REAL, LOCAL llama.cpp server.
//
// This is the endpoint class 004 §3 names explicitly ("vLLM/llama.cpp/Ollama-style local servers")
// and the one that motivated ADR-016. Before it, a local server was unreachable through the real
// client for two independent reasons, neither of which had anything to do with how it was bound:
//
//   (a) ADDRESS. `resolve_and_validate` refused every address a local server can have -- 127.0.0.0/8,
//       the three RFC 1918 ranges, 169.254/16, 100.64/10, and 0.0.0.0/8 are all in the blocked table.
//       Binding to 0.0.0.0 does not help: that only makes it reachable on a LAN address, which is
//       RFC 1918 and equally blocked.
//   (b) SCHEME. The provider exchange only ever spoke TLS; llama.cpp has no certificate.
//
// ADR-016 addressed both, narrowly: the provider path now resolves through `resolve_host` (no
// blocked-range filter -- that table is an SSRF defense and there is no attacker on a path whose
// destination is the deployment's own config, never guest-supplied and never model-derived per I3),
// and `ProviderTransport::plaintext_http` is available opt-in. The GUEST path is untouched;
// tests/test_provider_egress_address_policy.cpp is the positive control that proves it, asserting
// both halves against the same live loopback address in the same run.
//
// So this test needs no proxy, no injected resolver, and no CA override. It constructs
// `OpenAIChatClient` the way a real deployment targeting a local server would -- the real resolver,
// the real secret store, a real capability grant -- and the only non-default thing about it is the
// one explicitly-named transport enumerator. What a canned server can never establish, that the
// bytes this project SENDS are well-formed enough for a real inference server to accept, is
// established here against an implementation completely independent of any hosted vendor's
// (test_openrouter_live_e2e.cpp covers the hosted side over real TLS).
//
// SECURITY NOTE, since this is the file where someone will copy the pattern from: under
// `plaintext_http` the `Authorization: Bearer` header this client sends crosses the wire IN CLEAR.
// On loopback that is uncontroversial -- an attacker who can read loopback traffic already owns the
// process. Pointed at anything else it is a credential disclosure. ADR-016 §3 states the trade.
//
// STREAMING WORKS ON THIS TRANSPORT, which was not obvious and is why LC-6 asserts it. Neither
// `perform_http_exchange` nor `perform_https_exchange` decodes `Transfer-Encoding: chunked` framing;
// both simply read until the peer closes when there is no `Content-Length` and hand back the still-
// framed bytes. The decoding lives one layer up, in this backend (`decoded_response_body` /
// `parse_streaming_response_into_updates`, both branching on `response_is_chunked`) -- above the
// transport split, so it applies to plaintext exactly as it does to TLS. ADR-016 §3 initially
// claimed the opposite; LC-6 falsified that against this real server, and the ADR was corrected.
//
// I5 (nondeterminism crosses a recorded seam) IS RESPECTED BY EXCLUSION: a live model is
// nondeterministic, so this test is gated on an explicitly-set environment variable, carries the
// ctest label `live-network`, and asserts only STRUCTURAL properties (a tool call named
// `get_weather` came back; the reply parses as JSON with the declared key; usage counters are
// nonzero) -- never on what the model actually said.
//
// Configuration (no endpoint is compiled in as a default that would make this run by accident):
//   AGENTENGINE_LLAMACPP_PORT     required -- unset means SKIP (exit 0), never a failure
//   AGENTENGINE_LLAMACPP_HOST     optional -- default 127.0.0.1
//   AGENTENGINE_LLAMACPP_MODEL    optional -- llama.cpp ignores it; default "local"
//   AGENTENGINE_LLAMACPP_PREFIX   optional -- prepended to every prompt, default "/no_think "
//
// Why AGENTENGINE_LLAMACPP_PREFIX exists: `ChatRequest` carries no sampling parameters at all
// (core/chat_client.hpp's own elision, 004 §1), so there is no way from here to bound a reasoning
// model's thinking budget. The HTTP read loop's idle timeout (net_egress_proxy.cpp's kIoTimeoutMs,
// 10s) applies to a non-streaming response as ONE single wait, and a small local model generating a
// long chain of thought will exceed it. The default prefix is Qwen's own thinking-off directive;
// point this at a non-Qwen model and set the variable to that model's equivalent, or to empty.

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
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
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

// 006's real per-run tool table entry -- the exact type ChatRequest::tools already carries, not a
// second provider-facing declaration shape. A real server rejects a malformed projection with HTTP
// 400, which is the point of declaring it here.
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

// Concatenated text of every Text content item. A reasoning model emits its chain of thought in a
// separate field this backend does not translate, so "the first content item" is not the answer.
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

// A tool reply in the shape core/tool_pipeline.hpp's own step-9 "normalize" produces: a `Data` item
// wrapping the tool's JSON reply, tagged content_origin::tool, inside a ToolResult carrying the id.
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
    char const* port_env = std::getenv("AGENTENGINE_LLAMACPP_PORT");
    if (!port_env || !*port_env) {
        std::fprintf(stderr,
                     "test_llamacpp_live_e2e: SKIPPED -- AGENTENGINE_LLAMACPP_PORT is not set.\n"
                     "  Start a llama.cpp server (llama-server --port 8080) and run "
                     "tools/run-live-provider-tests.ps1 to exercise it.\n");
        return 0;
    }

#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    std::string const host = env_or("AGENTENGINE_LLAMACPP_HOST", "127.0.0.1");
    std::string const model = env_or("AGENTENGINE_LLAMACPP_MODEL", "local");
    g_prompt_prefix = env_or("AGENTENGINE_LLAMACPP_PREFIX", "/no_think ");

    unsigned port_value = 0;
    for (char const* p = port_env; *p; ++p) {
        if (*p < '0' || *p > '9' || port_value > 65535) {
            std::fprintf(stderr, "test_llamacpp_live_e2e: AGENTENGINE_LLAMACPP_PORT ('%s') is not a port.\n",
                         port_env);
            return 1;
        }
        port_value = port_value * 10 + static_cast<unsigned>(*p - '0');
    }
    if (port_value == 0 || port_value > 65535) {
        std::fprintf(stderr, "test_llamacpp_live_e2e: AGENTENGINE_LLAMACPP_PORT ('%s') is out of range.\n",
                     port_env);
        return 1;
    }
    auto const port = static_cast<std::uint16_t>(port_value);

    std::fprintf(stderr, "test_llamacpp_live_e2e: endpoint=http://%s:%u/v1 model=%s prefix=\"%s\"\n",
                 host.c_str(), port_value, model.c_str(), g_prompt_prefix.c_str());

    // llama.cpp needs no credential, but the credential PATH is exercised for real: a SecretStore, a
    // cap::Secret grant, and resolution at the point of use inside chat() (004 §1, 018 §4). That is
    // what makes LC-7's denial case meaningful rather than decorative.
    constexpr char const* kSecretName = "llamacpp-api-key";
    InMemorySecretStore store;
    store.set(kSecretName, "sk-local-no-auth-required");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"llamacpp-live-e2e-principal", ""};
    ctx.capabilities = &held;

    ChatClientCapabilities caps;  // DECLARED from what this deployment's server supports, never probed.
    caps.tool_calling = true;
    caps.structured_output_native = true;

    // Exactly how a real deployment targeting a local server constructs it: real resolver, real
    // (empty) CA override, and ONE explicitly-named non-default -- the transport. Everything between
    // `path_prefix` and `transport` is defaulted, spelled out here only because C++ has no named
    // arguments and `transport` is last (ADR-016 §3: appended, never inserted).
    openai::OpenAIChatClient client(host, port, model, SecretRef{kSecretName}, caps, store, "/v1",
                                     /*resolver=*/sandbox::resolve_host,
                                     /*ca_bundle_pem_override=*/{},
                                     /*http_referer=*/{}, /*x_title=*/{}, /*end_user_id=*/{},
                                     /*seed=*/std::nullopt,
                                     ProviderTransport::plaintext_http);
    static_assert(ChatClient<decltype(client)>,
                  "OpenAIChatClient must satisfy the real ChatClient concept (004 §1)");

    using agentengine::test_support::run_task_sync;

    // ---- LC-1 (ADR-016 G5): the REAL OpenAIChatClient against a real local model -------------------
    {
        auto resp = run_task_sync<result<ChatResponse>>(
            client.chat(request_asking("Reply with exactly one word: pong"), ctx));
        check(resp.has_value(),
              "LC-1 (ADR-016 G5): the REAL OpenAIChatClient -- real address resolution, real secret "
              "resolution, real request build, real HTTP exchange, real response parse -- completes "
              "against a real llama.cpp server, with no proxy and no injected resolver. This is the "
              "case that was unreachable before ADR-016, and it is unreachable again if either half "
              "of that ADR regresses.");
        if (!resp) {
            report("error", resp.error());
            std::fprintf(stderr,
                         "       (net.connect_failed here means llama.cpp is not listening on the "
                         "configured port, or the read timed out -- a reasoning model can exceed the "
                         "10s idle timeout, see AGENTENGINE_LLAMACPP_PREFIX)\n");
        } else {
            check(!text_of(resp->message).empty(), "LC-1: the assistant turn carries non-empty Text");
            check(resp->message.role == role::assistant, "LC-1: the reply is an assistant turn");
            check(resp->usage.input_tokens > 0 && resp->usage.output_tokens > 0,
                  "LC-1: usage.prompt_tokens/completion_tokens are reported by the real server and map "
                  "onto the same portable Usage fields a hosted provider's do (I6)");
            check(!resp->model.empty(),
                  "LC-1: ChatResponse::model reports the model that actually answered -- for llama.cpp "
                  "that is the loaded GGUF path, nothing like the id that was REQUESTED, so this could "
                  "not possibly be a request echo");
            note("reply", text_of(resp->message));
            note("model", resp->model);
        }
    }

    // ---- LC-2: tool calling -- 006's tool table projected onto a real local model -------------------
    std::optional<ToolCall> call;
    {
        ChatRequest req = request_asking("What is the weather in Seattle? Call the tool.");
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "LC-2: translate_tool's `{type:function, function:{name,description,parameters}}` "
              "projection is accepted by a real local server too, not only by remote vendors");
        if (!resp) {
            report("error", resp.error());
        } else {
            call = first_tool_call(resp->message);
            check(call.has_value(), "LC-2: the local model returned a ToolCall content item");
            if (call) {
                check(call->tool_name == "get_weather", "LC-2: the declared tool name round-trips");
                check(!call->call_id.empty(), "LC-2: the server-issued call id is preserved");
                auto args = json::parse(call->arguments_json);
                check(args.has_value(), "LC-2: arguments_json is valid JSON as received");
                check(args && args->find("location") != nullptr,
                      "LC-2: the arguments carry the `location` property the declared schema required "
                      "-- the schema really reached the model, it was not silently dropped");
                note("tool arguments", call->arguments_json);
            }
        }
    }

    // ---- LC-3: the tool RESULT turn is accepted back ------------------------------------------------
    if (call) {
        ChatRequest req;
        req.messages.push_back(user_message("What is the weather in Seattle? Call the tool."));
        req.messages.push_back(assistant_tool_call_message(*call));
        req.messages.push_back(tool_result_message(call->call_id, R"({"location":"Seattle","temp_c":11})"));
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "LC-3: a history containing an assistant `tool_calls` message AND a `tool_call_id` reply "
              "message is accepted by a real local server -- translate_message's two-sided tool shape "
              "is correct against a SECOND, independent implementation of the same wire contract");
        if (!resp) {
            report("error", resp.error());
        } else {
            check(!text_of(resp->message).empty(),
                  "LC-3: the model answered from the supplied tool result -- a full two-turn tool loop "
                  "completed end to end");
            note("post-tool reply", text_of(resp->message));
        }
    }

    // ---- LC-4: structured output against a local grammar-constrained decoder --------------------------
    {
        ChatRequest req = request_asking("Give the capital of France.");
        req.output_schema_json = kCapitalSchema;
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
        check(resp.has_value(),
              "LC-4: translate_output_schema's `response_format` wrapper -- including the forced "
              "`additionalProperties:false` and `strict:true` (004 §3's named checklist item) -- is "
              "accepted by llama.cpp's own grammar-constrained decoder, a completely different "
              "implementation from a hosted vendor's Structured Outputs");
        if (!resp) {
            report("error", resp.error());
        } else {
            std::string const body = text_of(resp->message);
            auto parsed = json::parse(body);
            check(parsed.has_value(),
                  "LC-4: the reply text is itself valid JSON -- structured output rides the ORDINARY "
                  "Text content item; this backend requests the shape outbound and never parses or "
                  "validates it inbound");
            check(parsed && parsed->find("capital") != nullptr,
                  "LC-4: the returned object carries the schema's required `capital` key, so the schema "
                  "really constrained decoding");
            note("structured reply", body);
        }
    }

    // ---- LC-5: TLS is still the default -- the same endpoint is NOT reachable without opting in ------
    // POSITIVE CONTROL for ADR-016 §3's "TLS remains the default" (gate G4), asserted at the level a
    // reader of this file cares about: the ONLY difference between this client and the working one
    // above is the transport argument, and this one must fail. Without it, `plaintext_http` could be
    // inert and the successes above could be TLS somehow working -- which they are not.
    {
        openai::OpenAIChatClient tls_client(host, port, model, SecretRef{kSecretName}, caps, store, "/v1");
        auto resp = run_task_sync<result<ChatResponse>>(tls_client.chat(request_asking("hi"), ctx));
        check(!resp.has_value(),
              "LC-5 (positive control): a DEFAULT-constructed client -- identical in every respect "
              "except that it does not opt into plaintext -- cannot reach the same plaintext server. "
              "Plaintext is genuinely opt-in, and the exchanges above genuinely used it.");
        if (!resp) {
            note("TLS-default refusal", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- LC-6: chunked SSE streaming DOES work over the plaintext transport --------------------------
    // This assertion started life inverted, on the reasonable-sounding but wrong premise that
    // `perform_http_exchange`'s "Content-Length-framed only" note meant no chunked support anywhere on
    // this transport. Running it against this real server falsified that, and ADR-016 §3 was corrected
    // rather than the test being bent to fit. Kept, in the true direction, precisely because it is a
    // claim about layering that is easy to get wrong twice: the chunked decode lives in the BACKEND
    // (`response_is_chunked` -> `parse_streaming_response_into_updates`), above the transport split,
    // so it is transport-independent by construction.
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
              "LC-6: a real streaming exchange over PLAINTEXT reaches the success terminal");
        check(!streamed.empty(),
              "LC-6: llama.cpp's own `Transfer-Encoding: chunked` SSE response -- real `data:` events "
              "and a real `[DONE]` terminator -- decodes into non-empty text over the plaintext "
              "transport. The chunked decode is the backend's, not the transport's, so it works the "
              "same on both (ADR-016 §3, corrected by this very assertion).");
        if (!updates.empty()) {
            check(updates.back().is_final, "LC-6: the last delivered update is flagged final");
        }
        note("stream terminal", s.terminal() == stream_terminal::closed ? "Closed" : "Failed");
        note("stream updates", std::to_string(updates.size()));
        note("stream text", streamed);
    }

    // ---- LC-7: an ungranted capability fails closed BEFORE any egress --------------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities = &empty;
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), denied));
        check(!resp.has_value(),
              "LC-7 (I2): with no cap::Secret grant the call is denied at the point of use and never "
              "reaches the network, even though a real, reachable, working endpoint is sitting right "
              "there -- the strongest form this assertion can take");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "LC-7: a capability denial is classified `policy`");
        }
    }

    // ---- LC-8: the GUEST path is still refused at this very same live server -------------------------
    // ADR-016's gate G1, restated where it matters most: a real, currently-serving endpoint that the
    // provider path just completed four exchanges against, which a sandboxed guest must still not be
    // able to reach. The allowlist deliberately NAMES this exact target, so a passing result could
    // not be dismissed as a mis-specified capability.
    // test_provider_egress_address_policy.cpp proves the same property deterministically and offline;
    // this is the version with a real inference server on the other end.
    {
        sandbox::HostEgressProxy proxy;  // real resolver, exactly as production constructs it
        cap::NetOut granted;
        granted.host_allowlist.push_back(host + ":" + std::to_string(port_value) + ":http");
        sandbox::NetEgressRequest req;
        req.method = "GET";
        req.path = "/v1/models";
        auto resp = proxy.fetch(req, granted);
        check(!resp.has_value() && resp.error().code == "net.address_blocked",
              "LC-8 (ADR-016 G1): a WASM guest holding a cap::NetOut grant that explicitly allowlists "
              "this exact live local server is STILL refused on address policy -- ADR-016 relaxed the "
              "host-initiated provider path and demonstrably nothing else");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_llamacpp_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_llamacpp_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
