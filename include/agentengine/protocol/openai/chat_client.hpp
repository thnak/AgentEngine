#pragma once
// Implements 004-Model-Provider-Plane.md §3 -- Milestone 5 Phase D: the OpenAI-compatible ChatClient
// backend (Chat Completions), 004 §3's default/widest-reach backend (OpenAI, gateways, vLLM/
// llama.cpp/Ollama-style local servers, most vendor compat endpoints). Wire-format field names below
// were sourced directly from the official OpenAI .NET SDK's generated serialization code (the
// `Utf8JsonWriter`/`WritePropertyName` calls are ground truth for the real wire shape, not paraphrased
// from documentation), not guessed.
//
// Reuses Phase C's sandbox::perform_provider_https_exchange (the host-initiated HTTPS client) for the
// actual network exchange and Phase A's SecretStore seam for outbound-credential resolution AT THE
// POINT OF USE (004 §1's rule) -- `OpenAIChatClient` holds only a `SecretRef` member, the same
// behavioral shape `test_chat_client_credential_resolution.cpp`'s reference conformer already proves;
// this is that reference conformer's real, product-code analogue, not a second design.
//
// Capabilities are DECLARED, not probed (004 §3's own rule: "Capability set is per endpoint,
// discovered from config, not assumed") -- the caller constructs this type with whatever
// ChatClientCapabilities its own deployment's config says the target endpoint actually has; nothing
// here inspects a response to infer a capability.
//
// `perform_provider_https_exchange` is a synchronous, blocking call with nothing to suspend on (Phase
// B4a's own decision, milestone doc: "a sync function is freely callable from inside an async
// coroutine body with no co_await needed") -- `chat()`'s coroutine body calls it directly, matching
// that already-established project position rather than introducing a new one. It genuinely blocks
// the calling thread for the full round-trip; a real async I/O integration is future work on
// provider_http_client.hpp itself, not scoped here.
//
// D2 (streaming) scope, named honestly rather than silently claimed complete: `perform_provider_
// https_exchange` has no incremental/chunked-transfer read loop yet -- it blocks until the full HTTP
// response is buffered, then returns. `chat_stream()` below therefore performs one complete
// (blocking) HTTPS exchange on a detached background thread, decodes `Transfer-Encoding: chunked`
// framing if present (a real OpenAI-compatible streaming response's actual wire shape --
// `net_egress_proxy.cpp`'s raw-request builder always sends `Connection: close`, so the server
// closing the connection at the end is the reliable read-loop exit condition either way), splits the
// decoded body into SSE `data: ...` events, and pushes ONE ChatResponseUpdate per event/assembled-
// tool-call as it walks the already-fully-received event list -- so the vendor's own chunk
// BOUNDARIES are preserved faithfully in delivery order (004 §7 G3's own gate: "identical chunk
// boundaries"), and the credit-controlled ring (core/stream.hpp, Phase B4b) still provides genuine
// backpressure between this parsing thread and the consumer -- but the underlying network fetch
// itself is not low-latency incremental. A real incremental read loop is future work on
// provider_http_client.hpp, not scoped here. The background thread is DETACHED, not tracked as a
// member: `OpenAIChatClient` may be shared across many concurrent `chat_stream()` calls (a real
// `ChatClientRegistry` binds one instance per `ChatClientId`, reused across turns), so a single
// thread member that joins-then-replaces would wrongly serialize concurrent streams. The detached
// thread captures every value it needs (host/port/path/model/api-key text, the moved request, the
// moved `stream_producer`) and touches no state owned by `*this`, so its lifetime is fully decoupled
// from the client object's.

#ifdef AGENTENGINE_WITH_HTTPS

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"
#include "quark/core/error.hpp"

namespace agentengine::openai {

namespace detail {

[[nodiscard]] inline std::string_view role_to_wire(role r) noexcept {
    switch (r) {
        case role::system: return "system";
        case role::user: return "user";
        case role::assistant: return "assistant";
        case role::tool: return "tool";
    }
    return "user";
}

// One AE `Message` -> one OpenAI wire message object. Handles the three content shapes this project's
// own content model can express today (Text, ToolCall, ToolResult) -- Reasoning/Media/Citation/Custom
// are not translated outbound yet (Chat Completions itself has no reasoning-trace field on inbound
// messages either, confirmed against the SDK's request-message serializers: only `content`/
// `tool_calls`/`tool_call_id`/`name`/`refusal`/`audio` exist). A tool-role AE Message's `call_id`
// comes from its `ToolResult` content item -- the only place a call id lives in the content model, and
// the shape `core/tool_pipeline.hpp`'s own step-9 "normalize" produces (a `Data` item wrapping the
// tool's JSON reply, tagged `content_origin::tool`).
[[nodiscard]] inline json::Value translate_message(Message const& m) {
    std::string text;
    std::vector<json::Value> tool_calls;
    std::optional<std::string> tool_call_id;

    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            text += t->text;
        } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> fn{
                {"name", json::Value::make_string(tc->tool_name)},
                {"arguments", json::Value::make_string(tc->arguments_json)},
            };
            std::vector<std::pair<std::string, json::Value>> call{
                {"id", json::Value::make_string(tc->call_id)},
                {"type", json::Value::make_string("function")},
                {"function", json::Value::make_object(std::move(fn))},
            };
            tool_calls.push_back(json::Value::make_object(std::move(call)));
        } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
            tool_call_id = tr->call_id;
            for (ContentItem const& inner : tr->content) {
                if (auto const* it = std::get_if<Text>(&inner.value)) {
                    text += it->text;
                } else if (auto const* d = std::get_if<Data>(&inner.value)) {
                    text += d->json;
                } else if (auto const* e = std::get_if<Error>(&inner.value)) {
                    text += e->message;
                }
            }
        }
    }

    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(role_to_wire(m.role))));
    if (tool_call_id) {
        obj.emplace_back("tool_call_id", json::Value::make_string(*tool_call_id));
        obj.emplace_back("content", json::Value::make_string(text));
    } else if (!tool_calls.empty()) {
        // A message with tool_calls carries `content: null` when there is no accompanying text --
        // never a fabricated empty string (the SDK's own collapsing rule: absent text -> JSON null).
        obj.emplace_back("content", text.empty() ? json::Value::make_null() : json::Value::make_string(text));
        obj.emplace_back("tool_calls", json::Value::make_array(std::move(tool_calls)));
    } else {
        obj.emplace_back("content", json::Value::make_string(text));
    }
    return json::Value::make_object(std::move(obj));
}

// A `role::tool` AE `Message` carrying N>1 `ToolResult` content items -- the natural shape a caller
// resolving N parallel tool calls in one turn would build -- must become N separate wire
// `{role:"tool", tool_call_id, content}` objects: OpenAI's Chat Completions contract requires one
// message per `tool_call_id` (confirmed against the SDK's own request-message shape), unlike
// Anthropic's Messages API, which legitimately bundles multiple `tool_result` blocks into a single
// `user`-role message (protocol/anthropic/chat_client.hpp's own `translate_message`). `translate_
// message` above tracks only ONE `tool_call_id` variable, so feeding it a multi-ToolResult Message
// silently collapses to the LAST result's id, with every earlier ToolResult's text merged under that
// wrong id -- a real correlation bug, not merely a hygiene one, that a real provider surfaces as
// "tool_call_id ... not found" or an unresolved-tool-call 400 (the assistant turn's OTHER tool_calls
// entries are left with no matching reply message at all). Every other message shape (user/assistant/
// system, or a role::tool message with 0-1 ToolResult items) is unaffected -- still exactly one wire
// message via `translate_message` unchanged.
[[nodiscard]] inline std::vector<json::Value> translate_message_to_wire(Message const& m) {
    if (m.role == role::tool) {
        std::size_t tool_result_count = 0;
        for (ContentItem const& item : m.content) {
            if (std::holds_alternative<ToolResult>(item.value)) ++tool_result_count;
        }
        if (tool_result_count > 1) {
            std::vector<json::Value> out;
            out.reserve(tool_result_count);
            for (ContentItem const& item : m.content) {
                if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
                    Message single;
                    single.role = role::tool;
                    ContentItem wrapped;
                    wrapped.origin = item.origin;
                    wrapped.value = *tr;
                    single.content.push_back(std::move(wrapped));
                    out.push_back(translate_message(single));
                }
            }
            return out;
        }
    }
    std::vector<json::Value> out;
    out.push_back(translate_message(m));
    return out;
}

// D3: one `ToolDescriptor` -> `{"type":"function","function":{"name","description","parameters"}}`
// (confirmed field names/nesting against the SDK's `InternalChatFunctionDefinition` serializer).
// `args_schema_json` already IS the tool's JSON Schema text (006's own real per-run tool table, no
// second provider-facing declaration shape) -- passed through as raw JSON, matching the SDK's own
// `WriteRawValue` passthrough for `parameters`/`schema` (it does no client-side schema validation
// either).
[[nodiscard]] inline result<json::Value> translate_tool(ToolDescriptor const& t) {
    auto parsed_params = json::parse(t.args_schema_json);
    if (!parsed_params) return std::unexpected(parsed_params.error());
    std::vector<std::pair<std::string, json::Value>> fn{
        {"name", json::Value::make_string(t.name)},
        {"description", json::Value::make_string(t.description)},
        {"parameters", std::move(*parsed_params)},
    };
    std::vector<std::pair<std::string, json::Value>> tool{
        {"type", json::Value::make_string("function")},
        {"function", json::Value::make_object(std::move(fn))},
    };
    return json::Value::make_object(std::move(tool));
}

// D4: 004 §3's other named checklist item -- "structured-output shaping that forces
// `additionalProperties: false` into the JSON Schema before it reaches the provider." `schema_json`
// is 003 §4's `OutputSchema<T>` text (`schema::json_schema_of<T>()`, an object schema with no
// `additionalProperties` key of its own -- confirmed against `json_schema.hpp`'s `ObjectBuilder`).
// Wraps as OpenAI's Structured Outputs `response_format` (`{"type":"json_schema","json_schema":
// {"name","schema","strict"}}`, confirmed field names/order against the SDK's
// `InternalResponseFormatJsonSchemaJsonSchema` serializer). `name` is required on the wire but 003 §4
// carries none -- "response" is a fixed, non-semantic placeholder (the schema body, not its name, is
// what OpenAI actually validates against).
[[nodiscard]] inline result<json::Value> translate_output_schema(std::string const& schema_json) {
    auto parsed = json::parse(schema_json);
    if (!parsed) return std::unexpected(parsed.error());
    json::Value schema = std::move(*parsed);
    if (schema.is_object() && schema.find("additionalProperties") == nullptr) {
        std::vector<std::pair<std::string, json::Value>> members(schema.as_object());
        members.emplace_back("additionalProperties", json::Value::make_bool(false));
        schema = json::Value::make_object(std::move(members));
    }
    std::vector<std::pair<std::string, json::Value>> json_schema_obj{
        {"name", json::Value::make_string("response")},
        {"schema", std::move(schema)},
        {"strict", json::Value::make_bool(true)},
    };
    std::vector<std::pair<std::string, json::Value>> response_format{
        {"type", json::Value::make_string("json_schema")},
        {"json_schema", json::Value::make_object(std::move(json_schema_obj))},
    };
    return json::Value::make_object(std::move(response_format));
}

// 004 §2's 2026-08-07 amendment (ADR-020): map the portable ordinal level down to OpenAI's own flat
// string enum. `off` -> `"none"`, OpenAI's own spelling for the same request. This backend's native
// shape happens to be a near-match, which is exactly why the CAPABILITY GATE below lives in
// `build_request_body` and not here -- a pure spelling function has nothing to fail against.
[[nodiscard]] inline std::string_view translate_reasoning_effort(reasoning_effort effort) noexcept {
    switch (effort) {
        case reasoning_effort::off:    return "none";
        case reasoning_effort::low:    return "low";
        case reasoning_effort::medium: return "medium";
        case reasoning_effort::high:   return "high";
    }
    return "medium";  // unreachable for a valid enumerator; no fabricated level, just a safe default
}

// D1: the full `POST /v1/chat/completions` request body. `stream` is a caller-supplied flag (never
// probed from `ChatRequest`) because `chat()` and `chat_stream()` are the two distinct callers, each
// wanting a different value.
//
// `end_user_id`/`seed` (Milestone 5 research follow-up, docs/research/2026-08-07-provider-metadata-and-
// sampling-params-survey.md, "Recommended design" items 1/2): backend-constructor-local, NOT portable
// `ChatRequest` vocabulary (004 §1's own sampling-parameter elision stands -- these are an abuse-
// tracking id and a best-effort determinism hint, not a sampling knob). Both optional; `end_user_id`
// empty means "omit `user` from the body entirely" (never send an empty-string user id), `seed`
// unset means "omit `seed` entirely" (never fabricate a value).
//
// `caps` (ADR-020): APPENDED LAST, defaulted, for the one thing this function must now refuse --
// 004 §2's degradation rule applied to `reasoning_effort`. Defaulting it to an all-false capability
// set is safe rather than surprising: the gate can only fire when a caller ASKS for a reasoning
// level, so every pre-existing call site (none of which can have set the field) is unaffected.
[[nodiscard]] inline result<json::Value> build_request_body(
    ChatRequest const& request, std::string const& model, bool stream,
    std::string const& end_user_id = {}, std::optional<std::int64_t> seed = std::nullopt,
    ChatClientCapabilities const& caps = {}) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("model", json::Value::make_string(model));

    std::vector<json::Value> messages;
    messages.reserve(request.messages.size());
    for (auto const& m : request.messages) {
        for (auto& wire : translate_message_to_wire(m)) messages.push_back(std::move(wire));
    }
    obj.emplace_back("messages", json::Value::make_array(std::move(messages)));

    if (!request.tools.empty()) {
        std::vector<json::Value> tools;
        tools.reserve(request.tools.size());
        for (auto const& t : request.tools) {
            auto tool_json = translate_tool(t);
            if (!tool_json) return std::unexpected(tool_json.error());
            tools.push_back(std::move(*tool_json));
        }
        obj.emplace_back("tools", json::Value::make_array(std::move(tools)));
    }

    if (request.output_schema_json) {
        auto response_format = translate_output_schema(*request.output_schema_json);
        if (!response_format) return std::unexpected(response_format.error());
        obj.emplace_back("response_format", std::move(*response_format));
    }

    if (stream) {
        obj.emplace_back("stream", json::Value::make_bool(true));
        // AgentSession's opt-in streaming turn loop (ADR-034) needs real per-call token usage to
        // keep 004 §5's TokenBudget<N> enforced -- without this, the vendor's SSE stream never
        // includes a usage object at all. A trailing chunk with `choices: []` and a top-level
        // `usage` arrives just before `[DONE]` once this is set (confirmed against OpenRouter's own
        // OpenAI-compatible streaming docs); StreamingUpdateAccumulator::items_from_block() below is
        // what captures it.
        std::vector<std::pair<std::string, json::Value>> stream_options;
        stream_options.emplace_back("include_usage", json::Value::make_bool(true));
        obj.emplace_back("stream_options", json::Value::make_object(std::move(stream_options)));
    }

    if (!end_user_id.empty()) obj.emplace_back("user", json::Value::make_string(end_user_id));
    if (seed.has_value()) obj.emplace_back("seed", json::Value::make_number(static_cast<double>(*seed)));

    // ADR-020. `nullopt` emits nothing at all -- the vendor default, and today's exact behaviour.
    if (request.reasoning_effort.has_value()) {
        // 004 §2's degradation rule: no DECLARED fallback for reasoning effort exists, so a backend
        // that cannot reason must refuse the request rather than quietly drop the field -- dropping
        // it is the "silently ignores the request" the rule forbids. `off` is exempt: a backend
        // without the bit satisfies "do not reason" by construction.
        if (*request.reasoning_effort != reasoning_effort::off && !caps.reasoning) {
            return std::unexpected(error{
                failure_class::contract,
                "request asks for a reasoning effort level but this backend does not declare the "
                "`reasoning` capability (004 §2: no declared fallback exists)",
                "openai.reasoning_not_supported"});
        }
        obj.emplace_back("reasoning_effort", json::Value::make_string(std::string(
                                                  translate_reasoning_effort(*request.reasoning_effort))));
    }

    return json::Value::make_object(std::move(obj));
}

// `http_referer`/`x_title` (same research-doc follow-up, "Recommended design" item 1): an OpenRouter-
// specific app-attribution convention (`HTTP-Referer`/`X-Title` HTTP headers, NOT a JSON body field --
// no other surveyed backend has an equivalent, confirmed Finding 1). Stamped in only when non-empty --
// an empty string means "don't send this header at all," never a fabricated empty header value.
[[nodiscard]] inline sandbox::NetEgressRequest build_http_request(std::string const& path,
                                                                    std::string const& api_key,
                                                                    std::string body,
                                                                    std::string const& http_referer = {},
                                                                    std::string const& x_title = {}) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path = path;
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    if (!http_referer.empty()) req.headers.emplace_back("HTTP-Referer", http_referer);
    if (!x_title.empty()) req.headers.emplace_back("X-Title", x_title);
    req.body = std::move(body);
    return req;
}

[[nodiscard]] inline error map_http_status_error(std::uint16_t status, std::string const& body) {
    failure_class klass = failure_class::fatal;
    if (status == 429 || status >= 500) {
        klass = failure_class::transient;  // 004 §4: retry applies to Transient only
    } else if (status == 401 || status == 403) {
        klass = failure_class::policy;
    } else if (status >= 400) {
        klass = failure_class::contract;
    }
    std::string message = "openai http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* err = parsed->find("error")) {
            if (auto const* m = err->find("message"); m && m->is_string()) message = m->as_string();
        }
    }
    return error{klass, message, "openai.http_" + std::to_string(status)};
}

// The streaming counterpart to `map_http_status_error` above, for `producer.fail(quark::error{...})`
// call sites (below) rather than `ae::error`. Deliberately STATUS-CODE-ONLY, no body/message parsing
// at all -- `quark::error::detail` (`quark/core/error.hpp`) is a non-owning `std::string_view`, and
// this classifier feeds error sites that run on a DETACHED background thread (`run_stream_worker`
// below): that thread's stack -- including any parsed error-body string -- is gone the instant the
// function returns, while a `stream<T>` consumer may read `fail_error().detail` well after that.
// Every value this returns is a static string literal for exactly that reason (ADR-036 red-team
// finding: reusing `map_http_status_error`'s dynamic, body-derived message here would be a real
// dangling-view read, not a hypothetical one). Same retry-relevant split `map_http_status_error`
// already makes (429/5xx retryable, everything else not), re-expressed in `quark::errc`'s coarser
// two-bucket vocabulary -- the exact status is still visible in `chat()`'s own `ae::error` for a
// caller on that path; only the coarser bucket survives on the streaming path, a real (documented,
// not silently improved-away) limitation of `quark::error`'s thinner shape versus `ae::error`'s.
[[nodiscard]] inline quark::error classify_http_status_stream_error(std::uint16_t status) noexcept {
    if (status == 429 || status >= 500) {
        return quark::error{quark::errc::overloaded, "openai.http_error_status_retryable"};
    }
    return quark::error{quark::errc::validation, "openai.http_error_status_nonretryable"};
}

// Factored out so the streaming path (below) parses a trailing `usage`-only SSE chunk with the
// EXACT same field mapping as the non-streaming response body -- one implementation of the wire
// contract, not two that could drift (this file's own D2 precedent for `StreamingUpdateAccumulator`
// vs `parse_streaming_response_into_updates`).
[[nodiscard]] inline Usage parse_usage_object(json::Value const& usage) {
    Usage out;
    if (auto const* pt = usage.find("prompt_tokens"); pt && pt->is_number()) {
        out.input_tokens = static_cast<std::uint64_t>(pt->as_number());
    }
    if (auto const* ct = usage.find("completion_tokens"); ct && ct->is_number()) {
        out.output_tokens = static_cast<std::uint64_t>(ct->as_number());
    }
    if (auto const* ptd = usage.find("prompt_tokens_details")) {
        if (auto const* cached = ptd->find("cached_tokens"); cached && cached->is_number()) {
            out.cached_input_tokens = static_cast<std::uint64_t>(cached->as_number());
        }
        if (auto const* cwt = ptd->find("cache_write_tokens"); cwt && cwt->is_number()) {
            out.cache_write_tokens = static_cast<std::uint64_t>(cwt->as_number());
        }
    }
    if (auto const* ctd = usage.find("completion_tokens_details")) {
        if (auto const* rt = ctd->find("reasoning_tokens"); rt && rt->is_number()) {
            out.reasoning_tokens = static_cast<std::uint64_t>(rt->as_number());
        }
    }
    return out;
}

// D1: the non-streaming response. Field names confirmed against the SDK's `ChatCompletion`/
// `ChatTokenUsage` serializers -- `choices[0].message.content`/`.tool_calls[]`, `usage.prompt_tokens`/
// `.completion_tokens`/`.prompt_tokens_details.cached_tokens`/`.completion_tokens_details.
// reasoning_tokens`. No `reasoning`/`reasoning_content` field exists on a Chat Completions response
// message (confirmed: the SDK's response-message deserializer has no such branch) -- reasoning is
// Anthropic's "first-class" surface (Phase E, 004 §3), not this backend's.
[[nodiscard]] inline result<ChatResponse> parse_chat_completion_response(json::Value const& body) {
    if (auto const* err = body.find("error")) {
        std::string msg = "unknown error";
        if (auto const* m = err->find("message"); m && m->is_string()) msg = m->as_string();
        return std::unexpected(error{failure_class::contract, "openai error: " + msg, "openai.error"});
    }
    json::Value const* choices = body.find("choices");
    if (!choices || !choices->is_array() || choices->as_array().empty()) {
        return std::unexpected(
            error{failure_class::contract, "response has no choices", "openai.no_choices"});
    }
    json::Value const& choice0 = choices->as_array().front();
    json::Value const* message = choice0.find("message");
    if (!message) {
        return std::unexpected(error{failure_class::contract, "choice has no message", "openai.no_message"});
    }

    ChatResponse resp;
    resp.message.role = role::assistant;

    // Finding 4: the model that ACTUALLY answered -- a sibling of `choices`/`usage` at the top level,
    // not a request-echo (never read from `request`/the caller's own `model` field). Left empty, not
    // fabricated, when the backend doesn't report one.
    if (auto const* model = body.find("model"); model && model->is_string()) {
        resp.model = model->as_string();
    }

    if (auto const* content = message->find("content");
        content && content->is_string() && !content->as_string().empty()) {
        ContentItem item;
        item.value = Text{content->as_string()};
        item.origin = content_origin::assistant;
        resp.message.content.push_back(std::move(item));
    }

    if (auto const* tool_calls = message->find("tool_calls"); tool_calls && tool_calls->is_array()) {
        for (auto const& tc : tool_calls->as_array()) {
            auto const* id = tc.find("id");
            auto const* fn = tc.find("function");
            if (!fn) continue;
            auto const* name = fn->find("name");
            auto const* args = fn->find("arguments");
            ToolCall call;
            call.call_id = (id && id->is_string()) ? id->as_string() : std::string{};
            call.tool_name = (name && name->is_string()) ? name->as_string() : std::string{};
            call.arguments_json = (args && args->is_string()) ? args->as_string() : std::string{"{}"};
            ContentItem item;
            item.value = std::move(call);
            item.origin = content_origin::assistant;
            resp.message.content.push_back(std::move(item));
        }
    }

    if (auto const* usage = body.find("usage")) {
        resp.usage = parse_usage_object(*usage);
    }

    return resp;
}

// ADR-023 Phase 1+2 (decisions/ADR-023-response-format-codec-seam.md §6 points 3-4). Opt-in only --
// `OpenAIChatClient`'s `scan_response_format_leaks` constructor flag gates whether this is ever
// called at all (default off, per the ADR's Finding 6: scanning is operator-armed, never
// content-triggered).
//
// ADR-035 Phase 1: `apply_response_format_scan` itself relocated to `core/response_format_leak_
// scan.hpp` so `AgentSession::run_model_call()` can apply it backend-agnostically (Anthropic
// included) regardless of streaming or `chat()` -- this call site is unchanged in behavior, just
// now calls the shared implementation instead of a private copy of it (one implementation, not two
// that could drift).

// D2: `Transfer-Encoding: chunked` framing (RFC 9112 §7.1) -- decoded independently of the network
// read loop (pure string parsing over an already-fully-received body), so this is testable without a
// live server. Trailers after the terminal 0-size chunk are ignored (this project has no use for
// them).
[[nodiscard]] inline result<std::string> decode_chunked_body(std::string_view body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto const line_end = body.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            return std::unexpected(
                error{failure_class::contract, "truncated chunked body: no chunk-size line terminator",
                      "openai.chunked_malformed"});
        }
        std::string_view size_line = body.substr(pos, line_end - pos);
        if (auto const semi = size_line.find(';'); semi != std::string_view::npos) {
            size_line = size_line.substr(0, semi);  // chunk extensions, ignored
        }
        std::size_t chunk_size = 0;
        auto const conv =
            std::from_chars(size_line.data(), size_line.data() + size_line.size(), chunk_size, 16);
        if (conv.ec != std::errc{}) {
            return std::unexpected(
                error{failure_class::contract, "malformed chunk size", "openai.chunked_malformed"});
        }
        pos = line_end + 2;
        if (chunk_size == 0) break;  // terminal chunk
        if (pos + chunk_size > body.size()) {
            return std::unexpected(
                error{failure_class::contract, "truncated chunk body", "openai.chunked_malformed"});
        }
        out.append(body.substr(pos, chunk_size));
        pos += chunk_size;
        if (body.substr(pos, 2) != "\r\n") {
            return std::unexpected(error{failure_class::contract, "missing CRLF after chunk data",
                                          "openai.chunked_malformed"});
        }
        pos += 2;
    }
    return out;
}

[[nodiscard]] inline bool header_name_equals_ci(std::string const& a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool response_is_chunked(sandbox::NetEgressResponse const& resp) {
    for (auto const& [k, v] : resp.headers) {
        if (header_name_equals_ci(k, "transfer-encoding") && v.find("chunked") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Real-server finding, not a hypothetical: `perform_https_exchange` (Phase C) has no chunked-transfer
// awareness in its read loop at all -- for a `Transfer-Encoding: chunked` response it simply reads
// until the peer closes the connection (the only exit condition available when there is no
// Content-Length header) and returns the RAW, still chunk-framed bytes as `NetEgressResponse::body`.
// A real OpenAI-compatible endpoint (confirmed live against OpenRouter, `api.openrouter.ai`) sends
// `Transfer-Encoding: chunked` on the ORDINARY non-streaming `chat()` response too, not only on SSE --
// chat_stream()'s own decode-if-chunked step already handled this correctly; chat() originally did
// not (json::parse was called directly on the raw chunk-framed body and failed with a confusing
// "expected a digit" parse error on the literal hex chunk-size lines). Every response body this
// backend reads must be decoded through this helper before `json::parse`, streaming or not.
[[nodiscard]] inline result<std::string> decoded_response_body(sandbox::NetEgressResponse const& resp) {
    if (!response_is_chunked(resp)) return resp.body;
    return decode_chunked_body(resp.body);
}

// SSE framing (only `data:` lines matter for an OpenAI-compatible stream -- `event:`/`id:`/`:comment`
// lines, if any, are ignored). Every OpenAI event fits on one line (compact single-line JSON), so no
// multi-line data accumulation is needed.
[[nodiscard]] inline std::vector<std::string_view> split_sse_data_events(std::string_view body) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        auto const nl = body.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("data:")) {
            std::string_view payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
            out.push_back(payload);
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

// D2: the actual chunk-to-ChatResponseUpdate translation, factored out of the network call so it is
// testable against a literal canned SSE/chunked body with no live server involved. `raw_body` is
// exactly `NetEgressResponse::body` (still `Transfer-Encoding: chunked`-framed if `is_chunked`).
//
// Streaming tool-call ARGUMENT FRAGMENTS arrive incrementally across chunks, keyed by a per-choice
// `index` (confirmed against the SDK's `StreamingChatToolCallUpdate` -- `index` is required precisely
// so a consumer can correlate parallel tool-call fragments). Since this function already has every
// event in hand (Phase C's HTTP layer buffers the whole response, see file banner), fragments are
// accumulated in one pass and each tool call is emitted as ONE complete `ChatResponseUpdate` once
// fully assembled, appended after every text delta -- text deltas map 1:1 to the vendor's own SSE
// chunks (real per-chunk fidelity preserved), tool calls do not (unavoidable: a partial JSON-string
// fragment is not a valid `ToolCall::arguments_json` on its own).
// ADR-019: the incremental streaming decoder. Fed raw response bytes as they arrive off the socket,
// it returns whichever `ChatResponseUpdate`s are complete RIGHT NOW, so `chat_stream()` can push each
// text delta onto the ring the moment the vendor emitted it rather than after the whole completion
// has been received.
//
// ONE-ITEM HOLD-BACK, and why. `ChatResponseUpdate::is_final` marks the LAST update of a stream, and
// "last" is not knowable until the stream ends. Rather than weaken that contract (or emit a trailing
// empty update to carry the flag), this holds exactly one completed update back: `feed()` releases
// update N when N+1 becomes available, and `finish()` flushes the held one with `is_final` set. One
// item of lag, which is invisible next to a model's own inter-token latency, in exchange for
// `is_final` meaning exactly what it did before.
//
// TOOL CALLS STILL EMIT AT THE END, unchanged: a tool call's `arguments` arrive as JSON-string
// FRAGMENTS across many chunks (correlated by `index`), and a partial fragment is not a valid
// `ToolCall::arguments_json`. Text deltas map 1:1 to the vendor's own chunks and stream immediately;
// tool calls are assembled and appended by `finish()`. That is the same ordering and the same content
// the one-shot parser always produced -- see `parse_streaming_response_into_updates` below, which is
// now implemented in terms of THIS type precisely so the streaming and non-streaming paths cannot
// drift apart.
class StreamingUpdateAccumulator {  // ae-naming-lint: allow StreamingUpdateAccumulator — new ADR-019 vocabulary; 027 has not been updated to list it
public:
    explicit StreamingUpdateAccumulator(bool chunked) : chunked_(chunked) {}

    // Feeds raw (still chunk-framed, if the response was chunked) bytes. Returns updates ready now.
    [[nodiscard]] result<std::vector<ChatResponseUpdate>> feed(std::string_view bytes) {
        std::string decoded;
        if (chunked_) {
            auto d = chunked_decoder_.feed(bytes);
            if (!d) return std::unexpected(d.error());
            decoded = std::move(*d);
        } else {
            decoded.assign(bytes);
        }

        std::vector<ChatResponseUpdate> out;
        for (std::string const& block : framer_.feed(decoded)) {
            for (ContentItem& item : items_from_block(block)) {
                release(&out, std::move(item));
            }
        }
        return out;
    }

    // End of stream: flushes the held-back update and every assembled tool call, marking the very
    // last one final. Safe to call on a stream that produced nothing (returns empty).
    [[nodiscard]] std::vector<ChatResponseUpdate> finish() {
        std::vector<ChatResponseUpdate> out;
        // A truncated final event still carries a real item -- do not silently drop it.
        if (std::string tail = framer_.take_remainder(); !tail.empty()) {
            for (ContentItem& item : items_from_block(tail)) release(&out, std::move(item));
        }
        for (auto const& acc : pending_by_index_) {
            if (!acc.seen) continue;
            ToolCall call;
            call.call_id = acc.id;
            call.tool_name = acc.name;
            call.arguments_json = acc.arguments.empty() ? "{}" : acc.arguments;
            ContentItem item;
            item.value = std::move(call);
            item.origin = content_origin::assistant;
            release(&out, std::move(item));
        }
        if (held_) {
            ChatResponseUpdate last;
            last.delta = std::move(*held_);
            last.is_final = true;
            last.usage = captured_usage_;  // nullopt if the vendor never sent stream_options.include_usage
            held_.reset();
            out.push_back(std::move(last));
        } else if (captured_usage_.has_value()) {
            // A genuinely empty completion (no text, no tool call) still carries real usage -- never
            // drop it just because there was no content item to hang it off of. An empty Text delta
            // is a legitimate final update on its own (ADR-034's caller only reads `.usage`/
            // `.is_final` off it, never assumes non-empty text).
            ChatResponseUpdate last;
            last.delta.value  = Text{};
            last.delta.origin = content_origin::assistant;
            last.is_final     = true;
            last.usage        = captured_usage_;
            out.push_back(std::move(last));
        }
        return out;
    }

private:
    struct PendingToolCall {
        std::string id;
        std::string name;
        std::string arguments;
        bool seen = false;
    };

    // Emits the previously-held item (never final -- something came after it) and holds this one.
    void release(std::vector<ChatResponseUpdate>* out, ContentItem item) {
        if (held_) {
            ChatResponseUpdate update;
            update.delta = std::move(*held_);
            update.is_final = false;
            out->push_back(std::move(update));
        }
        held_ = std::move(item);
    }

    // One SSE event block -> zero or more content items, updating tool-call accumulation state.
    [[nodiscard]] std::vector<ContentItem> items_from_block(std::string const& block) {
        std::vector<ContentItem> out;
        for (std::string_view payload : split_sse_data_events(block)) {
            if (payload == "[DONE]") {
                done_seen_ = true;
                continue;
            }
            auto parsed = json::parse(payload);
            if (!parsed) continue;  // one malformed chunk is skipped, not fatal to the whole stream
            // ADR-034: the `stream_options.include_usage` trailing chunk carries a top-level `usage`
            // object and an EMPTY (or absent) `choices` array -- captured here, BEFORE the
            // choices-empty check below would otherwise skip this exact chunk entirely.
            if (auto const* usage = parsed->find("usage")) {
                captured_usage_ = parse_usage_object(*usage);
            }
            json::Value const* choices = parsed->find("choices");
            if (!choices || !choices->is_array() || choices->as_array().empty()) continue;
            json::Value const& choice0 = choices->as_array().front();
            json::Value const* delta = choice0.find("delta");
            if (!delta) continue;

            if (auto const* content = delta->find("content");
                content && content->is_string() && !content->as_string().empty()) {
                ContentItem item;
                item.value = Text{content->as_string()};
                item.origin = content_origin::assistant;
                out.push_back(std::move(item));
            }

            if (auto const* tool_calls = delta->find("tool_calls"); tool_calls && tool_calls->is_array()) {
                for (auto const& tc : tool_calls->as_array()) {
                    auto const* idx = tc.find("index");
                    std::size_t const index =
                        (idx && idx->is_number()) ? static_cast<std::size_t>(idx->as_number()) : 0;
                    if (index >= pending_by_index_.size()) pending_by_index_.resize(index + 1);
                    PendingToolCall& acc = pending_by_index_[index];
                    acc.seen = true;
                    if (auto const* id = tc.find("id"); id && id->is_string()) acc.id = id->as_string();
                    if (auto const* fn = tc.find("function")) {
                        if (auto const* name = fn->find("name"); name && name->is_string()) {
                            acc.name += name->as_string();
                        }
                        if (auto const* args = fn->find("arguments"); args && args->is_string()) {
                            acc.arguments += args->as_string();
                        }
                    }
                }
            }
        }
        return out;
    }

    bool chunked_;
    bool done_seen_ = false;
    sandbox::ChunkedBodyDecoder chunked_decoder_;
    sandbox::SseEventFramer framer_;
    std::vector<PendingToolCall> pending_by_index_;
    std::optional<ContentItem> held_;
    std::optional<Usage> captured_usage_;  // ADR-034: from a stream_options.include_usage trailing chunk
};

// D2: the one-shot parse, for a body that genuinely is fully in hand. Implemented ON TOP of the
// incremental accumulator (ADR-019) rather than beside it: these two used to be one function, and
// keeping them as two independent implementations of the same wire contract would be exactly the
// drift this project's own conventions warn about. Same inputs, same outputs, one decoder.
[[nodiscard]] inline result<std::vector<ChatResponseUpdate>> parse_streaming_response_into_updates(
    std::string_view raw_body, bool is_chunked) {
    StreamingUpdateAccumulator acc(is_chunked);
    auto fed = acc.feed(raw_body);
    if (!fed) return std::unexpected(fed.error());
    std::vector<ChatResponseUpdate> updates = std::move(*fed);
    for (auto& update : acc.finish()) updates.push_back(std::move(update));
    return updates;
}

// A testability seam, not a security bypass -- mirrors `provider_http_client.hpp`'s own injectable
// `resolver` parameter exactly, letting a test bind an arbitrary `Host:` name to the ephemeral
// loopback port its own server just opened without a real DNS lookup. Defaults to the real
// `sandbox::resolve_host` (ADR-016: the host-initiated provider resolver, which does real DNS and
// the resolve-once-connect-to-a-literal discipline but applies NO blocked-range filter -- a
// deployment's own llama.cpp/vLLM/Ollama endpoint on loopback or RFC 1918 is the ordinary case here,
// not an SSRF attempt; the guest path keeps `resolve_and_validate`). Production code never
// constructs `OpenAIChatClient` with a non-default resolver.
using Resolver = std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>;

// The detached background worker (see file banner for why detached, not a tracked member). Every
// parameter is owned by value -- no reference back to the `OpenAIChatClient` instance that spawned it.
// `ca_bundle_pem_override` mirrors `perform_provider_https_exchange`'s own testability seam verbatim
// (empty in production -- the vendored CA bundle applies; a test's self-signed leaf isn't in it).
// `http_referer`/`x_title`/`end_user_id`/`seed`/`transport` are `OpenAIChatClient`'s own optional
// constructor fields, threaded through by value exactly like every other captured parameter here.
inline void run_stream_worker(std::string host, std::uint16_t port, std::string path, std::string api_key,
                               std::string model, ChatRequest request,
                               stream_producer<ChatResponseUpdate> producer, Resolver resolver,
                               std::string ca_bundle_pem_override, std::string http_referer,
                               std::string x_title, std::string end_user_id,
                               std::optional<std::int64_t> seed, sandbox::ProviderTransport transport,
                               std::stop_token stop, ChatClientCapabilities caps) {
    // ADR-020: `caps` reaches here for one reason -- so `chat_stream()` enforces the SAME reasoning-
    // effort gate `chat()` does. A capability check that held on one of the two entry points would be
    // no check at all.
    auto body = build_request_body(request, model, /*stream=*/true, end_user_id, seed, caps);
    if (!body) {
        producer.fail(quark::error{quark::errc::validation, "openai.request_build_failed"});
        return;
    }
    auto req = build_http_request(path, api_key, json::dump(*body), http_referer, x_title);

    // ADR-019: decode and push AS THE BYTES ARRIVE. `chunked` is not known until the response head is
    // parsed, which happens before the first `on_body` call -- but the sink cannot see the head, so it
    // is inferred from the first fragment instead: a chunked body always begins with a hex chunk-size
    // line, an unchunked SSE body always begins with `data:`/`event:`/a colon comment. Cheap, and only
    // consulted once.
    std::optional<StreamingUpdateAccumulator> acc;
    bool push_failed = false;
    std::optional<error> decode_error;

    auto on_body = [&](std::string_view fragment) -> bool {
        if (!acc) {
            bool const looks_like_sse = fragment.starts_with("data:") || fragment.starts_with("event:") ||
                                         fragment.starts_with(":");
            acc.emplace(!looks_like_sse);
        }
        auto updates = acc->feed(fragment);
        if (!updates) {
            decode_error = updates.error();
            return false;
        }
        for (auto& update : *updates) {
            if (producer.push(std::move(update)) != quark::ReplyPush::Ok) {
                push_failed = true;  // consumer cancelled/deadlined -- the ring already latched why
                return false;
            }
        }
        return true;
    };

    auto resp = sandbox::perform_provider_streaming_exchange(host, port, req, on_body, stop, std::nullopt,
                                                              resolver, ca_bundle_pem_override, transport);
    if (push_failed) return;  // nothing left to say; the consumer is gone
    if (!resp) {
        producer.fail(quark::error{quark::errc::unavailable, "openai.exchange_failed"});
        return;
    }
    if (resp->status < 200 || resp->status >= 300) {
        // A non-2xx body is an error document, not SSE, so it produced no `data:` events and nothing
        // bogus was pushed above -- the stream simply fails here instead. ADR-036: status-code-only
        // classification (never the dynamic body message -- see classify_http_status_stream_error's
        // own comment) so a caller retrying on the streaming path (e.g. a future ModelCallGateway)
        // can actually distinguish a retryable 429/5xx from a non-retryable 401/400, unlike the prior
        // single coarse `errc::validation` for every status.
        producer.fail(classify_http_status_stream_error(resp->status));
        return;
    }
    if (decode_error) {
        producer.fail(quark::error{quark::errc::serialization, "openai.stream_parse_failed"});
        return;
    }

    if (acc) {
        for (auto& update : acc->finish()) {
            if (producer.push(std::move(update)) != quark::ReplyPush::Ok) return;
        }
    }
    producer.close();
}

}  // namespace detail

// The real, product-code `ChatClient` conformer for 004 §3's default OpenAI-compatible backend.
// `Store` is any real `SecretStore` (`AgentEngineSecretStore` in production; `InMemorySecretStore` in
// tests, matching `test_chat_client_credential_resolution.cpp`'s own pattern).
template <SecretStore Store>
class OpenAIChatClient {
public:
    // `http_referer`/`x_title`/`end_user_id`/`seed` (Milestone 5 research follow-up, docs/research/
    // 2026-08-07-provider-metadata-and-sampling-params-survey.md "Recommended design" items 1/2): ALL
    // optional, APPENDED after `ca_bundle_pem_override` -- never inserted earlier in this list. Every
    // existing construction call site in tests/test_openai_chat_client_live.cpp uses positional
    // arguments; inserting a parameter anywhere but the end would silently misalign every one of them
    // (the same class of bug `Usage::cache_write_tokens`'s own placement note in core/content.hpp
    // documents, deliberately avoided here the same way). `transport` (ADR-016) is appended after all
    // of them for exactly the same reason.
    //
    // `transport` defaults to TLS and should stay there. `plaintext_http` is for a local llama.cpp/
    // vLLM/Ollama server that has no certificate to present -- on that transport the `Authorization:
    // Bearer` header this client sends is readable on the wire, which is fine on loopback and a
    // credential disclosure anywhere else. See sandbox/provider_http_client.hpp's `ProviderTransport`.
    OpenAIChatClient(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                      ChatClientCapabilities caps, Store const& store, std::string path_prefix = "/v1",
                      detail::Resolver resolver = sandbox::resolve_host,
                      std::string ca_bundle_pem_override = {}, std::string http_referer = {},
                      std::string x_title = {}, std::string end_user_id = {},
                      std::optional<std::int64_t> seed = std::nullopt,
                      sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls,
                      // ADR-023 Phase 1: off by default -- scanning `content` for raw response-format
                      // leaks (Harmony/DeepSeek/Hermes/`<think>`) is operator-armed, never
                      // content-triggered (the ADR's Finding 6). Appended last, same "never insert
                      // earlier" convention this constructor's own file-top comment already documents
                      // for every optional param above.
                      bool scan_response_format_leaks = false)
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          capabilities_(caps),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          http_referer_(std::move(http_referer)),
          x_title_(std::move(x_title)),
          end_user_id_(std::move(end_user_id)),
          seed_(seed),
          transport_(transport),
          scan_response_format_leaks_(scan_response_format_leaks) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
        // Resolution happens HERE, inside chat(), against EffectContext -- never at construction
        // (004 §1 / 018 §4, the same rule test_chat_client_credential_resolution.cpp proves).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_request_body(request, model_, /*stream=*/false, end_user_id_, seed_,
                                                 capabilities_);
        if (!body) co_return std::unexpected(body.error());

        auto req = detail::build_http_request(path_prefix_ + "/chat/completions", lease->reveal_text(),
                                                json::dump(*body), http_referer_, x_title_);
        auto resp = sandbox::perform_provider_https_exchange(host_, port_, req, {}, std::nullopt,
                                                                resolver_, ca_bundle_pem_override_,
                                                                transport_);
        if (!resp) co_return std::unexpected(resp.error());
        auto decoded_body = detail::decoded_response_body(*resp);
        if (!decoded_body) co_return std::unexpected(decoded_body.error());
        if (resp->status < 200 || resp->status >= 300) {
            co_return std::unexpected(detail::map_http_status_error(resp->status, *decoded_body));
        }
        auto parsed = json::parse(*decoded_body);
        if (!parsed) co_return std::unexpected(parsed.error());
        auto response = detail::parse_chat_completion_response(*parsed);
        if (!response) co_return std::unexpected(response.error());
        if (scan_response_format_leaks_) {
            response->message =
                agentengine::apply_response_format_scan(std::move(response->message), request.tools);
        }
        co_return response;
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) {
            pair.producer.fail(quark::error{quark::errc::validation, "secret.not_granted"});
            return std::move(pair.consumer);
        }
        // ADR-017: read the token BEFORE moving the producer into the thread. Argument evaluation
        // order is unspecified, so `pair.producer.stop_token()` written inline alongside
        // `std::move(pair.producer)` could legally run after the move -- on a moved-from producer,
        // whose stop_source is empty, yielding a token that never fires.
        std::stop_token stop = pair.producer.stop_token();
        std::thread(&detail::run_stream_worker, host_, port_, path_prefix_ + "/chat/completions",
                    lease->reveal_text(), model_, std::move(request), std::move(pair.producer), resolver_,
                    ca_bundle_pem_override_, http_referer_, x_title_, end_user_id_, seed_, transport_,
                    std::move(stop), capabilities_)
            .detach();
        return std::move(pair.consumer);
    }

private:
    std::string host_;
    std::uint16_t port_;
    std::string model_;
    SecretRef api_key_ref_;
    ChatClientCapabilities capabilities_;
    Store const& store_;
    std::string path_prefix_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    std::string http_referer_;
    std::string x_title_;
    std::string end_user_id_;
    std::optional<std::int64_t> seed_;
    sandbox::ProviderTransport transport_;
    bool scan_response_format_leaks_;
};

}  // namespace agentengine::openai

#endif  // AGENTENGINE_WITH_HTTPS
