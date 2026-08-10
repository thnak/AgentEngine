// End-to-end proof of 004-Model-Provider-Plane.md §3's two shipped backends
// (protocol/openai/chat_client.hpp, protocol/anthropic/chat_client.hpp) against a REAL, remote,
// third-party inference service -- OpenRouter, which exposes BOTH an OpenAI-compatible
// (`/api/v1/chat/completions`) and an Anthropic-compatible (`/api/v1/messages`) surface on the same
// host, so one credential exercises both backends against genuinely independent wire contracts.
//
// WHY THIS EXISTS ALONGSIDE test_openai_chat_client_live.cpp / test_anthropic_chat_client_live.cpp:
// those two are end-to-end over a REAL TLS socket but against a CANNED loopback server -- every byte
// the client receives was written by the test itself, so they prove plumbing and parsing but cannot
// prove that what this project SENDS is actually well-formed enough for a real provider to accept.
// A canned server drains and replies unconditionally; a real one returns HTTP 400 on a malformed
// request body. That gap is exactly what this file closes: every success assertion below is also,
// implicitly, a proof that the outbound translation (004 §3's request shaping, 006's tool-schema
// projection, 003 §4's structured-output shaping) produced something a real provider accepted.
//
// I5 (nondeterminism crosses a recorded seam) IS RESPECTED BY EXCLUSION, NOT VIOLATED: a live model
// is nondeterministic, so this test is NOT part of the default suite and nothing here is asserted on
// model CONTENT. It is gated on an explicitly-set environment variable, carries the ctest label
// `live-network`, and every assertion is STRUCTURAL (a tool call came back named `get_weather`; the
// text parses as JSON with the declared key; usage counters are nonzero; the stream reached its
// success terminal) rather than semantic (the answer said "Paris"). Deterministic replay of a real
// provider's bytes remains RecordingChatClient/ReplayChatClient's job (004 §5), not this file's.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_OPENROUTER_API_KEY   required -- unset means SKIP (exit 0), never a failure
//   AGENTENGINE_OPENROUTER_MODEL     optional -- default below
//   AGENTENGINE_OPENROUTER_HOST      optional -- default `openrouter.ai`
// The key reaches the client the same way a production one would: through a real SecretStore,
// resolved at the point of use inside chat()/chat_stream() against a real capability grant, never
// held as a member. `tools/run-live-provider-tests.ps1` populates these from a local key file.
//
// Both clients use the DEFAULT resolver (real DNS + resolve_and_validate's real blocked-range
// enforcement) and the DEFAULT CA bundle (the vendored, pinned Mozilla roots) -- unlike the canned
// loopback tests, which must inject a fake resolver and a self-signed leaf. This is the only test in
// the suite that exercises those two production paths for real.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "support/run_task_sync.hpp"

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

// Observations that are real and worth printing but are NOT assertions -- a live provider's caching
// state, reasoning budget, and routing decisions are its own, not this project's contract.
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

// OpenRouter's default routing alias for the DeepSeek V4 Flash family. Deliberately an ALIAS, not a
// pinned snapshot: it makes the `model` field of the response (004 §3 / the 2026-08-07 provider-
// metadata survey's Finding 4 -- "the model that ACTUALLY answered", never a request echo) genuinely
// observable, because the alias resolves server-side to a different, dated id.
constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;

// Both surfaces live under the same `/api` prefix on the same host: the OpenAI-compatible base is
// `https://openrouter.ai/api/v1` (+ the client's own `/chat/completions`) and the Anthropic-
// compatible base is `https://openrouter.ai/api` (+ the Messages API's own `/v1/messages`). Same
// resulting path prefix for both clients; they differ only in the endpoint each appends.
constexpr char const* kPathPrefix = "/api/v1";

constexpr char const* kSecretName = "openrouter-api-key";

// A live reasoning model can spend a long time before emitting anything, and the HTTP read loop's
// idle timeout (net_egress_proxy.cpp's kIoTimeoutMs, 10s) applies to a NON-streaming response as one
// single wait. Every prompt below is therefore deliberately tiny and answerable in a few tokens.
[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    req.messages.push_back(user_message(std::move(text)));
    return req;
}

// 006's real per-run tool table entry -- the exact type ChatRequest::tools already carries, not a
// second provider-facing declaration shape. Both backends project it into their own vendor form
// (OpenAI's `tools[].function.parameters`, Anthropic's `tools[].input_schema`); a real provider
// rejects a malformed projection with HTTP 400, which is the point of declaring it here.
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

// Concatenated text of every Text content item, ignoring Reasoning/ToolCall/etc. Both surfaces of
// this provider return a `thinking`/`reasoning` part ahead of the answer for the default model, so
// "the first content item" is NOT the answer -- asserting on it would be asserting on the provider's
// reasoning configuration, which is not this project's contract.
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

[[nodiscard]] bool has_reasoning(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Reasoning>(item.value)) return true;
    }
    return false;
}

// Drains a live stream to completion. Returns every update in delivery order -- the caller asserts
// on shape (at least one text delta, a final flag, the success terminal), never on token content.
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

// A tool reply, in the shape core/tool_pipeline.hpp's own step-9 "normalize" produces: a `Data` item
// wrapping the tool's JSON reply, tagged content_origin::tool, inside a ToolResult carrying the
// call id. Feeding this back is what proves each backend's history translation (OpenAI's
// `tool_call_id` message, Anthropic's `tool_result` block inside a user message) is accepted by a
// real server rather than merely round-trippable through this project's own parser.
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

[[nodiscard]] Message assistant_tool_call_message(ToolCall call) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = std::move(call);
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
    char const* key_env = std::getenv("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || !*key_env) {
        // A SKIP, not a pass and not a failure: this test needs a real credential and real egress.
        // Reported as success so a default `ctest` run on a machine with neither stays green -- the
        // ctest label `live-network` is how a run that MEANS to exercise it selects it.
        std::fprintf(stderr,
                     "test_openrouter_live_e2e: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_openrouter_live_e2e: host=%s model=%s\n", host.c_str(), model.c_str());

    // The credential travels the production route: a real SecretStore, a real cap::Secret grant, and
    // resolution at the point of use inside chat()/chat_stream() (004 §1, 018 §4). Nothing below ever
    // holds the key text itself.
    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"live-e2e-principal", ""};
    ctx.capabilities = &held;

    using agentengine::test_support::run_task_sync;

    ChatClientCapabilities caps;  // DECLARED from what OpenRouter's model catalogue reports for this
    caps.streaming = true;         // model, never probed from a response (004 §3's own rule).
    caps.tool_calling = true;
    caps.parallel_tool_calls = true;
    caps.structured_output_native = true;
    caps.reasoning = true;
    caps.seed = true;
    caps.max_output_tokens = 1024;  // bounds cost and wall-clock; Anthropic REQUIRES max_tokens.

    // Default resolver AND default CA bundle -- the real ones, on both clients.
    openai::OpenAIChatClient oai(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                  kPathPrefix);
    anthropic::AnthropicChatClient ant(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                        kPathPrefix);

    static_assert(ChatClient<decltype(oai)>, "OpenAIChatClient must satisfy the ChatClient concept");
    static_assert(ChatClient<decltype(ant)>, "AnthropicChatClient must satisfy the ChatClient concept");

    std::string oai_reply_text;
    std::string ant_reply_text;

    // ================= OpenAI-compatible surface (/api/v1/chat/completions) =========================

    // ---- OR-OAI-1: chat() completes against the real service -----------------------------------------
    {
        auto resp = run_task_sync<result<ChatResponse>>(
            oai.chat(request_asking("Reply with exactly one word: pong"), ctx));
        check(resp.has_value(),
              "OR-OAI-1: chat() succeeds against the REAL OpenRouter Chat Completions endpoint over "
              "real DNS, real resolve_and_validate, real TLS against the vendored CA bundle -- no "
              "injected resolver and no self-signed leaf anywhere on this path");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            oai_reply_text = text_of(resp->message);
            check(!oai_reply_text.empty(),
                  "OR-OAI-1: the assistant turn carries non-empty Text -- a real model's real bytes "
                  "survived chunked-transfer decoding and response parsing");
            check(resp->message.role == role::assistant, "OR-OAI-1: the reply is an assistant turn");
            check(resp->usage.input_tokens > 0 && resp->usage.output_tokens > 0,
                  "OR-OAI-1: usage.prompt_tokens/completion_tokens come back nonzero from a real "
                  "billing-grade counter, not a canned literal");
            check(!resp->model.empty(),
                  "OR-OAI-1: ChatResponse::model reports the model that ACTUALLY answered (Finding 4) "
                  "-- populated from the response body, never echoed from the request");
            if (resp->model != model) {
                check(true,
                      "OR-OAI-1: the reported model differs from the requested routing alias, which is "
                      "only observable against a real router -- a request echo could not do this");
            }
            note("reply", oai_reply_text);
            note("model", resp->model);
            note("reasoning_tokens", std::to_string(resp->usage.reasoning_tokens));
            note("cached_input_tokens", std::to_string(resp->usage.cached_input_tokens));
            note("cache_write_tokens", std::to_string(resp->usage.cache_write_tokens));
        }
    }

    // ---- OR-OAI-2: chat_stream() over a real SSE response ---------------------------------------------
    {
        stream<ChatResponseUpdate> s =
            oai.chat_stream(request_asking("Reply with exactly one word: pong"), ctx);
        auto updates = drain(s);
        check(s.terminal() == quark::ReplyStreamTerminal::Closed,
              "OR-OAI-2: a real streaming exchange reaches the SUCCESS terminal -- the detached worker "
              "closed the ring rather than failing it");
        check(!updates.empty(), "OR-OAI-2: at least one update was delivered through ae::stream<T>");
        check(!text_of(updates).empty(),
              "OR-OAI-2: the concatenated text deltas of a REAL server-sent-events response are "
              "non-empty -- real `Transfer-Encoding: chunked` framing, real `data:` events, and real "
              "`[DONE]` termination all decoded correctly");
        if (!updates.empty()) {
            check(updates.back().is_final, "OR-OAI-2: the last delivered update is flagged final");
        }
        note("stream updates", std::to_string(updates.size()));
        note("stream text", text_of(updates));
    }

    // ---- OR-OAI-3: tool calling -- 006's tool table projected onto a real provider ---------------------
    std::optional<ToolCall> oai_call;
    {
        ChatRequest req = request_asking("What is the weather in Seattle? Call the tool.");
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(oai.chat(req, ctx));
        check(resp.has_value(),
              "OR-OAI-3: a request carrying a real ToolDescriptor is ACCEPTED by the real provider -- "
              "translate_tool's `{type:function, function:{name,description,parameters}}` projection "
              "of args_schema_json is well-formed enough for a live server, which a canned server "
              "that drains unconditionally can never establish");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            oai_call = first_tool_call(resp->message);
            check(oai_call.has_value(),
                  "OR-OAI-3: the real model returned a ToolCall content item");
            if (oai_call) {
                check(oai_call->tool_name == "get_weather",
                      "OR-OAI-3: the declared tool name round-trips exactly");
                check(!oai_call->call_id.empty(),
                      "OR-OAI-3: the provider-issued call id is preserved -- it is what the follow-up "
                      "turn must correlate against");
                auto args = json::parse(oai_call->arguments_json);
                check(args.has_value(),
                      "OR-OAI-3: arguments_json is valid JSON as received (the provider sends it as a "
                      "JSON-encoded STRING; this backend passes it through without re-encoding)");
                check(args && args->find("location") != nullptr,
                      "OR-OAI-3: the arguments carry the `location` property the declared schema "
                      "required -- the schema really reached the model, it was not silently dropped");
                note("tool arguments", oai_call->arguments_json);
            }
        }
    }

    // ---- OR-OAI-4: the tool RESULT turn is accepted back ----------------------------------------------
    if (oai_call) {
        ChatRequest req;
        req.messages.push_back(user_message("What is the weather in Seattle? Call the tool."));
        req.messages.push_back(assistant_tool_call_message(*oai_call));
        req.messages.push_back(
            tool_result_message(oai_call->call_id, R"({"location":"Seattle","temp_c":11})"));
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(oai.chat(req, ctx));
        check(resp.has_value(),
              "OR-OAI-4: a history containing an assistant `tool_calls` message AND a `tool_call_id` "
              "reply message is accepted by the real provider -- translate_message's two-sided tool "
              "shape is correct on the wire, not merely self-consistent (a wrong shape is HTTP 400)");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            check(!text_of(resp->message).empty(),
                  "OR-OAI-4: the model answered from the supplied tool result -- a full two-turn tool "
                  "loop completed end to end against a live provider");
            note("post-tool reply", text_of(resp->message));
        }
    }

    // ---- OR-OAI-5: structured output -- 003 §4's shaping accepted by a real provider -------------------
    {
        ChatRequest req = request_asking("Give the capital of France.");
        req.output_schema_json = kCapitalSchema;
        auto resp = run_task_sync<result<ChatResponse>>(oai.chat(req, ctx));
        check(resp.has_value(),
              "OR-OAI-5: translate_output_schema's `response_format` wrapper -- including the forced "
              "`additionalProperties:false` and `strict:true` (004 §3's named checklist item) -- is "
              "accepted by a real Structured Outputs implementation");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            std::string const body = text_of(resp->message);
            auto parsed = json::parse(body);
            check(parsed.has_value(),
                  "OR-OAI-5: the reply text is itself valid JSON -- structured output rides the "
                  "ORDINARY Text content item; this backend requests the shape outbound and never "
                  "parses or validates it inbound");
            check(parsed && parsed->find("capital") != nullptr,
                  "OR-OAI-5: the returned object carries the schema's required `capital` key, so the "
                  "schema genuinely constrained a real model's decoding");
            note("structured reply", body);
        }
    }

    // ---- OR-OAI-6: attribution/abuse-tracking options survive a real exchange --------------------------
    // OpenRouter is the provider these four options were designed against (2026-08-07 survey items
    // 1/2): HTTP-Referer/X-Title are its own app-attribution convention, `user` its abuse-tracking id,
    // `seed` its determinism hint. The canned-server tests can only show they do not break request
    // CONSTRUCTION; only the real service can show it does not reject them.
    {
        openai::OpenAIChatClient attributed(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                             kPathPrefix, sandbox::resolve_and_validate, /*ca=*/{},
                                             "https://agentengine.test/", "AgentEngine Live E2E",
                                             "live-e2e-user", std::optional<std::int64_t>{7});
        auto resp = run_task_sync<result<ChatResponse>>(
            attributed.chat(request_asking("Reply with exactly one word: pong"), ctx));
        check(resp.has_value(),
              "OR-OAI-6: HTTP-Referer + X-Title headers and `user` + `seed` body fields, all set at "
              "once, are ACCEPTED by the real provider that defines them -- not merely non-fatal to "
              "local request construction");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        }
    }

    // ---- OR-OAI-7: POSITIVE CONTROL -- a wrong credential is rejected by the real service --------------
    // Without this, every success above could in principle be explained by an endpoint that ignores
    // authentication entirely. This is the test that cannot pass unless the credential was load-bearing.
    {
        InMemorySecretStore bad_store;
        bad_store.set(kSecretName, "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000");
        openai::OpenAIChatClient bad(host, kHttpsPort, model, SecretRef{kSecretName}, caps, bad_store,
                                      kPathPrefix);
        auto resp = run_task_sync<result<ChatResponse>>(bad.chat(request_asking("hi"), ctx));
        check(!resp.has_value(),
              "OR-OAI-7 (positive control): a syntactically valid but WRONG api key is rejected by the "
              "real service -- proving the credential resolved above was genuinely load-bearing and "
              "the successes are not an unauthenticated endpoint answering anyone");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OR-OAI-7: an authentication rejection is classified `policy` (401/403), so 004 §4's "
                  "retry policy will not retry it -- never `transient`");
            note("auth failure", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- OR-OAI-8: an ungranted capability fails closed BEFORE any egress -----------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities = &empty;
        auto resp = run_task_sync<result<ChatResponse>>(oai.chat(request_asking("hi"), denied));
        check(!resp.has_value(),
              "OR-OAI-8 (I2): with no cap::Secret grant the call is denied at the point of use and "
              "never reaches the network, even though a real, reachable, correctly-credentialed "
              "endpoint is sitting right there -- the strongest form this assertion can take");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OR-OAI-8: a capability denial is classified `policy`");
        }
    }

    // ================= Anthropic-compatible surface (/api/v1/messages) ==============================

    // ---- OR-ANT-1: chat() against the real Messages API surface ---------------------------------------
    {
        auto resp = run_task_sync<result<ChatResponse>>(
            ant.chat(request_asking("Reply with exactly one word: pong"), ctx));
        check(resp.has_value(),
              "OR-ANT-1: chat() succeeds against the REAL Messages-API-compatible endpoint -- a "
              "structurally DIFFERENT wire contract (top-level `system`, `content` block array, "
              "`x-api-key`/`anthropic-version` headers, mandatory `max_tokens`) reached over the same "
              "real DNS/TLS path");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            ant_reply_text = text_of(resp->message);
            check(!ant_reply_text.empty(),
                  "OR-ANT-1: a non-empty Text block was extracted from the real `content[]` array");
            check(resp->usage.input_tokens > 0 && resp->usage.output_tokens > 0,
                  "OR-ANT-1: Anthropic's own `usage.input_tokens`/`output_tokens` names map onto the "
                  "same portable Usage fields (I6: one vocabulary, two wire shapes)");
            check(!resp->model.empty(), "OR-ANT-1: the answering model is reported");
            note("reply", ant_reply_text);
            note("model", resp->model);
            note("has Reasoning item", has_reasoning(resp->message) ? "yes" : "no");
        }
    }

    // ---- OR-ANT-2: chat_stream() over real NAMED-EVENT SSE --------------------------------------------
    {
        stream<ChatResponseUpdate> s =
            ant.chat_stream(request_asking("Reply with exactly one word: pong"), ctx);
        auto updates = drain(s);
        check(s.terminal() == quark::ReplyStreamTerminal::Closed,
              "OR-ANT-2: a real named-event streaming exchange reaches the success terminal");
        check(!updates.empty(), "OR-ANT-2: at least one update was delivered");
        check(!text_of(updates).empty(),
              "OR-ANT-2: `event: content_block_delta` / `text_delta` events from a REAL server decode "
              "into non-empty text -- split_sse_named_events handled real framing, not a fixture's");
        note("stream updates", std::to_string(updates.size()));
        note("stream text", text_of(updates));
    }

    // ---- OR-ANT-3: tool_use -- the same ToolDescriptor, a different projection -------------------------
    std::optional<ToolCall> ant_call;
    {
        ChatRequest req = request_asking("What is the weather in Seattle? Call the tool.");
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(ant.chat(req, ctx));
        check(resp.has_value(),
              "OR-ANT-3: the SAME 006 ToolDescriptor, projected into Anthropic's `input_schema` shape "
              "instead of OpenAI's `function.parameters`, is accepted by the real provider");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            ant_call = first_tool_call(resp->message);
            check(ant_call.has_value(), "OR-ANT-3: a tool_use block came back as a ToolCall item");
            if (ant_call) {
                check(ant_call->tool_name == "get_weather", "OR-ANT-3: the tool name round-trips");
                check(!ant_call->call_id.empty(), "OR-ANT-3: the tool_use id is preserved");
                auto args = json::parse(ant_call->arguments_json);
                check(args.has_value(),
                      "OR-ANT-3: arguments_json is valid JSON -- note the provider sends `input` as a "
                      "JSON OBJECT here (not a string, as OpenAI does); this backend re-serializes it "
                      "so both surfaces present the SAME portable ToolCall shape (I6)");
                check(args && args->find("location") != nullptr,
                      "OR-ANT-3: the arguments carry the schema's required `location` property");
                note("tool arguments", ant_call->arguments_json);
            }
        }
    }

    // ---- OR-ANT-4: the tool RESULT turn is accepted back ----------------------------------------------
    if (ant_call) {
        ChatRequest req;
        req.messages.push_back(user_message("What is the weather in Seattle? Call the tool."));
        req.messages.push_back(assistant_tool_call_message(*ant_call));
        req.messages.push_back(
            tool_result_message(ant_call->call_id, R"({"location":"Seattle","temp_c":11})"));
        req.tools.push_back(weather_tool());
        auto resp = run_task_sync<result<ChatResponse>>(ant.chat(req, ctx));
        check(resp.has_value(),
              "OR-ANT-4: a role::tool AE message translated into Anthropic's own `role:\"user\"` + "
              "`tool_result` block shape (this backend's named translation decision (1)) is accepted "
              "by the real provider -- proving that decision correct on the wire, not just internally");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            check(!text_of(resp->message).empty(),
                  "OR-ANT-4: the model answered from the supplied tool result");
            note("post-tool reply", text_of(resp->message));
        }
    }

    // ---- OR-ANT-5: structured output via `output_config` -----------------------------------------------
    {
        ChatRequest req = request_asking("Give the capital of France.");
        req.output_schema_json = kCapitalSchema;
        auto resp = run_task_sync<result<ChatResponse>>(ant.chat(req, ctx));
        check(resp.has_value(),
              "OR-ANT-5: translate_output_config's `output_config.format` wrapper is accepted by the "
              "real provider -- the same 003 §4 OutputSchema<T> text, a different vendor envelope");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            std::string const body = text_of(resp->message);
            auto parsed = json::parse(body);
            check(parsed.has_value(), "OR-ANT-5: the reply text is valid JSON");
            check(parsed && parsed->find("capital") != nullptr,
                  "OR-ANT-5: the returned object carries the schema's required `capital` key");
            note("structured reply", body);
        }
    }

    // ---- OR-ANT-6: system extraction + prompt-caching breakpoints on a real request ---------------------
    {
        ChatClientCapabilities cache_caps = caps;
        cache_caps.prompt_caching = true;
        anthropic::AnthropicChatClient cached(host, kHttpsPort, model, SecretRef{kSecretName}, cache_caps,
                                               store, kPathPrefix, "2023-06-01",
                                               sandbox::resolve_and_validate, /*ca=*/{},
                                               "https://agentengine.test/", "AgentEngine Live E2E",
                                               "live-e2e-user", /*cache_ttl=*/"5m");
        ChatRequest req;
        Message sys;
        sys.role = role::system;
        ContentItem sys_item;
        sys_item.origin = content_origin::system;
        sys_item.value = Text{"You are terse. Answer with a single word."};
        sys.content.push_back(std::move(sys_item));
        req.messages.push_back(std::move(sys));
        req.messages.push_back(user_message("Reply with exactly one word: pong"));
        req.tools.push_back(weather_tool());

        auto resp = run_task_sync<result<ChatResponse>>(cached.chat(req, ctx));
        check(resp.has_value(),
              "OR-ANT-6: a real request carrying ALL of -- system messages hoisted out of messages[] "
              "into the top-level `system` ARRAY, ttl-bearing `cache_control` breakpoints on both the "
              "system block and the last tool, `metadata.user_id`, and the attribution headers -- is "
              "accepted by the real provider. Every one of those is a translation decision a canned "
              "server could never have validated.");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            note("cached-request reply", text_of(resp->message));
            note("cache_write_tokens", std::to_string(resp->usage.cache_write_tokens));
            note("cached_input_tokens", std::to_string(resp->usage.cached_input_tokens));
        }
    }

    // ---- OR-ANT-7: POSITIVE CONTROL -- a wrong credential is rejected --------------------------------
    {
        InMemorySecretStore bad_store;
        bad_store.set(kSecretName, "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000");
        anthropic::AnthropicChatClient bad(host, kHttpsPort, model, SecretRef{kSecretName}, caps,
                                            bad_store, kPathPrefix);
        auto resp = run_task_sync<result<ChatResponse>>(bad.chat(request_asking("hi"), ctx));
        check(!resp.has_value(),
              "OR-ANT-7 (positive control): the `x-api-key` header path is genuinely authenticated too "
              "-- a wrong key is rejected");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OR-ANT-7: classified `policy`, not `transient`");
            note("auth failure", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- OR-ANT-8: reasoning_effort has an OBSERVABLE effect on a real provider (ADR-020) -------------
    // The behavioural half of ADR-020, and the only place it can be had. On the OpenAI surface
    // `reasoning_effort` is a field a lenient gateway can silently ignore -- ADR-020 §2 measured an
    // invented field returning HTTP 200 there, so "the request was accepted" proves nothing. Anthropic's
    // mapping is structural instead: enabling thinking changes the SHAPE of the response, adding a
    // `thinking` content block that a disabled request does not produce. That is a difference no
    // silent-ignore can fake, so it is asserted here rather than in the offline suite.
    {
        ChatClientCapabilities think_caps = caps;
        think_caps.reasoning = true;
        // Raised from this file's cost-bounding 1024: Anthropic's extended-thinking floor IS 1024, and
        // its budget must be strictly BELOW max_tokens, so at 1024 no level above `off` is expressible
        // at all. That is not a quirk of this fixture -- the first run of this case failed exactly here,
        // with `anthropic.thinking_budget_unsatisfiable`, which is the design refusing to send a request
        // the vendor would reject. Recorded in ADR-020 §4 as a real deployment constraint; the number
        // below is the smallest that makes the case expressible while still bounding cost.
        think_caps.max_output_tokens = 4096;
        anthropic::AnthropicChatClient thinker(host, kHttpsPort, model, SecretRef{kSecretName},
                                                think_caps, store, kPathPrefix, "2023-06-01",
                                                sandbox::resolve_and_validate);

        auto count_reasoning = [](Message const& m) {
            std::size_t n = 0;
            for (auto const& item : m.content) {
                if (std::holds_alternative<Reasoning>(item.value)) ++n;
            }
            return n;
        };

        ChatRequest off_req = request_asking("What is 17*23? Think it through.");
        off_req.reasoning_effort = reasoning_effort::off;
        auto off_resp = run_task_sync<result<ChatResponse>>(thinker.chat(off_req, ctx));
        // Some reasoning-only models (observed live against openai/gpt-oss-120b) reject a disable
        // request outright -- HTTP 400 "Reasoning is mandatory for this endpoint and cannot be
        // disabled." -- rather than silently ignoring `thinking:{type:"disabled"}`. That is a real,
        // model-specific serving constraint this backend cannot negotiate around (it sent exactly the
        // portable request asked of it), the same category of documented deployment fact as this
        // block's own thinking-budget-floor comment below. Tolerated ONLY for that exact rejection
        // shape; any other failure still fails the check.
        bool const reasoning_mandatory_for_model =
            !off_resp && off_resp.error().klass == failure_class::contract &&
            off_resp.error().message.find("Reasoning is mandatory") != std::string::npos;
        if (reasoning_mandatory_for_model) {
            note("OR-ANT-8 off", "SKIPPED (model requires reasoning always-on): " + off_resp.error().message);
        }
        check(off_resp.has_value() || reasoning_mandatory_for_model,
              "OR-ANT-8: `off` -> `thinking:{type:\"disabled\"}` is accepted by the real provider, OR "
              "the provider explicitly refuses to disable reasoning for a reasoning-only model");
        if (!off_resp && !reasoning_mandatory_for_model) {
            std::fprintf(stderr, "       error: %s (%s)\n", off_resp.error().message.c_str(),
                         off_resp.error().code.c_str());
        }

        ChatRequest high_req = request_asking("What is 17*23? Think it through.");
        high_req.reasoning_effort = reasoning_effort::high;
        auto high_resp = run_task_sync<result<ChatResponse>>(thinker.chat(high_req, ctx));
        check(high_resp.has_value(),
              "OR-ANT-8: `high` -> a computed `budget_tokens` under this request's own max_tokens is "
              "accepted by the real provider -- the arithmetic satisfies Anthropic's real constraints");
        if (!high_resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", high_resp.error().message.c_str(),
                         high_resp.error().code.c_str());
        }

        if (off_resp && high_resp) {
            std::size_t const off_blocks = count_reasoning(off_resp->message);
            std::size_t const high_blocks = count_reasoning(high_resp->message);
            note("reasoning blocks, off", std::to_string(off_blocks));
            note("reasoning blocks, high", std::to_string(high_blocks));
            check(off_blocks == 0,
                  "OR-ANT-8: a disabled request comes back with NO Reasoning content item");
            check(high_blocks > 0,
                  "OR-ANT-8: the SAME question with effort=high comes back carrying a Reasoning content "
                  "item -- the portable level reached the model and changed what it produced, which no "
                  "silently-ignored field could have done");
        }
    }

    // ---- OR-PARITY-1: one unchanged call site, two real backends (004 §7 G1, I6) ----------------------
    // test_chat_client_cross_backend_parity.cpp already proves this shape against canned servers. This
    // is the same claim where it is hardest to fake: two genuinely different remote wire contracts,
    // both driven through the identical `ChatClient` concept call, both yielding usable assistant text.
    {
        check(!oai_reply_text.empty() && !ant_reply_text.empty(),
              "OR-PARITY-1 (I6): the SAME ChatRequest, through the SAME `chat(request, ctx)` call "
              "shape, produced usable assistant text from BOTH a real Chat Completions endpoint and a "
              "real Messages API endpoint -- the caller never learned which wire format it was using");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openrouter_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openrouter_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
