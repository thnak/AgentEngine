#pragma once
// Implements 004-Model-Provider-Plane.md §3 -- Milestone 5 Phase E: the Anthropic ChatClient backend
// (Messages API, `POST /v1/messages`), 004 §3's "first-class" backend (reasoning parts, prompt
// caching, tool use) for Claude 5 family (Fable 5, Opus 5, Sonnet 5) and Haiku 4.5. Wire-format field
// names below were sourced directly from the official Anthropic C# SDK's generated model code
// (`D:\GitSrc\anthropic-sdk-csharp`, the same rigor Phase D's OpenAI backend used against
// `D:\GitSrc\openai-dotnet`), not paraphrased from documentation.
//
// Structurally mirrors protocol/openai/chat_client.hpp (same detail-namespace-of-pure-functions
// shape, same DETACHED background thread for chat_stream(), same injectable resolver/CA-bundle
// testability seam over Phase C's perform_provider_https_exchange, same "perform_provider_https_
// exchange is a synchronous blocking call, freely callable from inside chat()'s coroutine body,
// matching Phase B4a's own already-established position" reasoning) -- differences below are
// Anthropic's own wire shape, not a second design.
//
// Anthropic-specific translation decisions, named rather than silently assumed:
//
// (1) SYSTEM PROMPT. Anthropic's Messages API has NO `role:"system"` in its `messages[]` array (the
//     SDK's own doc comment: "there is no system role for input messages in the Messages API") --
//     `system` is a distinct top-level request field. Any `role::system` messages in `ChatRequest.
//     messages` are extracted and concatenated into `system`; every other message is translated
//     normally. A `role::tool` AE message (a tool reply) has no Anthropic equivalent role either --
//     Anthropic represents a tool reply as a `role:"user"` message containing a `tool_result` content
//     block, so `role::tool` translates to wire role `"user"`.
//
// (2) TOOL-CALL ARGUMENTS ARE A REAL JSON OBJECT, not a string. Unlike OpenAI's `function.arguments`
//     (a JSON-encoded STRING, confirmed in Phase D's own research), Anthropic's `tool_use.input` is a
//     genuine JSON object on the wire (confirmed: `Dictionary<string,JsonElement>` in the SDK, no
//     stringify/parse round-trip). `ToolCall::arguments_json` (this project's content model) is
//     always a string, so outbound translation parses it into a `json::Value` and emits that object
//     directly; inbound parsing does the reverse (`json::dump` the received object back into
//     `arguments_json`) -- lossless either way, just a different wire shape than Phase D's.
//
// (3) THINKING BLOCKS: response-parsing ONLY, not outbound round-tripping. A `thinking` block
//     requires both `thinking` (text) AND `signature` (an opaque tamper-evidence string) on the wire;
//     `redacted_thinking` carries only opaque `data`, no visible text at all. This project's content
//     model (`agentengine::Reasoning{text, encrypted}`) has no field to carry `signature`/`data` --
//     so a `Reasoning` content item received in a response is translated into `Reasoning{text=
//     thinking-text or empty, encrypted=is-redacted}` for the CALLER to read, but this backend never
//     re-sends a `Reasoning` item back to Anthropic in a later turn's outbound `messages[]` (silently
//     dropped from history translation) -- resending a thinking block without its real signature is
//     either rejected by the API or defeats the tamper-evidence property it exists for, and this
//     project's content model has nowhere to have kept that signature in the first place. Named here
//     rather than silently claimed as full extended-thinking round-tripping.
//
// (4) PROMPT CACHING (`cache_control`), scoped to the two segment boundaries actually visible from
//     `ChatRequest` as received (004 §8 Q2's resolution: "a backend declaring `prompt_caching` inserts
//     its own vendor-specific breakpoints at [005 §3's] existing segment boundaries as backend-
//     internal translation logic"): the extracted `system` text and the LAST tool definition, when
//     `ChatClientCapabilities::prompt_caching` is declared true for this bound instance. `ChatRequest`
//     itself is already a FLATTENED `messages` list by the time it reaches any `ChatClient` (005 §3's
//     `ContextAssemblyResult`/`assemble_context`, core/context_assembly.hpp, merges every
//     contributor's messages into one vector with no per-contributor boundary markers surviving) --
//     so per-message-history cache breakpoints at finer-grained contributor boundaries are NOT
//     reachable from this seam without threading boundary markers through `ContextAssemblyResult` and
//     `ChatRequest` first, which is a real, separate, not-yet-built architectural change, not a gap in
//     this phase's own translation logic. system+tools is exactly Anthropic's own documented
//     best-practice cache-breakpoint placement (the stable prefix), so this is a meaningful, real
//     caching win at the boundary that IS reachable today, not a token gesture.
//
// (5) STRUCTURED OUTPUT uses Anthropic's native `output_config.format` (`{"type":"json_schema",
//     "schema":...}`), confirmed as the SDK's own primary mechanism (distinct from, and newer than,
//     the tool-forcing convention `output_schema_strategy::tool_shaped`, core/chat_client.hpp, still
//     covers via `tool_choice`). No `additionalProperties:false` forcing here unlike Phase D's OpenAI
//     backend -- nothing in the research confirms Anthropic requires or even recognizes that key, so
//     this backend does not assert an unconfirmed requirement onto the schema.
//
// (6) E2: CUMULATIVE-TO-INCREMENTAL USAGE CONVERSION. Every `message_delta.usage` field on the wire is
//     documented (and proven, via the SDK's own `MessageContentAggregator.GetResult` reduce logic) to
//     be the RUNNING TOTAL so far, not a per-event delta -- `output_tokens` is unconditionally
//     overwritten by each event's value, `input_tokens`/cache-token fields are seeded once from
//     `message_start.message.usage` and only overwritten on a `message_delta` that actually carries a
//     non-null value. `accumulate_message_delta_usage` implements exactly this reduce, tested against
//     a literal multi-event SSE sequence. **What this conversion has nowhere to surface**: `chat_
//     stream()`'s `ChatResponseUpdate` carries no `Usage` field at all -- the SAME pre-existing gap
//     Phase D's own OpenAI backend already named (`ChatResponseUpdate{delta, is_final}`, no usage
//     slot). The conversion LOGIC is real and tested; wiring it into a caller-visible per-chunk value
//     needs `ChatResponseUpdate` to grow a field this phase does not add.
//
// (7) M5 RESEARCH FOLLOW-UP (docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md,
//     "Recommended design" items 1/2/4/5/6/7 -- items 3 (`reasoning_effort`) and 8 (`prompt_cache_key`)
//     explicitly deferred there, not built here): `http_referer`/`x_title` (item 1, OpenRouter-only
//     attribution headers, stamped into every request only when non-empty); `end_user_id` (item 2,
//     Anthropic's `metadata.user_id` abuse-tracking field -- `Metadata` has ONLY that one field per the
//     locally-vendored SDK's `Metadata.cs`; Anthropic has NO native `seed` field at all per Finding 2,
//     so no seed parameter exists here -- a fake no-op would be worse than the honest absence);
//     `ChatResponse.model`/`Usage.cache_write_tokens` (items 4/5, response-parsing-only, see
//     `parse_message_response`); the 4-`cache_control`-blocks-combined hard cap (item 6,
//     `count_cache_control_blocks`, enforced defensively at the end of `build_request_body` even though
//     today's own placements -- at most 2, system + last tool -- can never reach it); and `cache_ttl`
//     (item 7, `"5m"`/`"1h"`/empty-for-server-default, validated at construction time, applied to every
//     `cache_control` object this backend builds).

#ifdef AGENTENGINE_WITH_HTTPS

#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"
#include "quark/core/error.hpp"

namespace agentengine::anthropic {

namespace detail {

// See file banner (4): a testability seam mirroring provider_http_client.hpp's own, identical to
// Phase D's OpenAI backend's `Resolver` alias.
using Resolver = std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>;

// (2): a real JSON object on the wire, not a stringified-JSON string -- ToolCall::arguments_json is
// parsed into this shape for outbound translation, and json::dump()'d back for inbound.
[[nodiscard]] inline json::Value translate_tool_use_input(std::string const& arguments_json) {
    auto parsed = json::parse(arguments_json);
    if (!parsed) return json::Value::make_object({});  // malformed input -- send an empty object
    return std::move(*parsed);
}

// Splits `messages` into (system_text, non-system messages) -- see file banner (1). System text from
// multiple role::system messages concatenates in order.
struct SplitMessages {
    std::string system_text;
    std::vector<Message const*> rest;
};

[[nodiscard]] inline SplitMessages split_system_messages(std::vector<Message> const& messages) {
    SplitMessages out;
    for (Message const& m : messages) {
        if (m.role == role::system) {
            for (ContentItem const& item : m.content) {
                if (auto const* t = std::get_if<Text>(&item.value)) out.system_text += t->text;
            }
        } else {
            out.rest.push_back(&m);
        }
    }
    return out;
}

[[nodiscard]] inline std::string_view role_to_wire(role r) noexcept {
    // role::tool has no Anthropic role of its own -- a tool reply travels as a "user" message
    // carrying a tool_result content block (file banner (1)).
    return (r == role::assistant) ? "assistant" : "user";
}

// One AE `Message` -> one Anthropic wire message object (`{"role", "content"}`, content always the
// array-of-blocks form, never the string-collapse shorthand -- simpler to always emit the array, and
// nothing here needs the shorthand). `role::system` messages must be filtered out by the caller
// BEFORE this is called (split_system_messages) -- this function has no system-role handling of its
// own, only user/assistant/tool translation. Reasoning content items are silently dropped from
// outbound translation (file banner (3)).
[[nodiscard]] inline json::Value translate_message(Message const& m) {
    std::vector<json::Value> blocks;

    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("text")},
                {"text", json::Value::make_string(t->text)},
            };
            blocks.push_back(json::Value::make_object(std::move(block)));
        } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("tool_use")},
                {"id", json::Value::make_string(tc->call_id)},
                {"name", json::Value::make_string(tc->tool_name)},
                {"input", translate_tool_use_input(tc->arguments_json)},
            };
            blocks.push_back(json::Value::make_object(std::move(block)));
        } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
            std::string content_text;
            for (ContentItem const& inner : tr->content) {
                if (auto const* it = std::get_if<Text>(&inner.value)) {
                    content_text += it->text;
                } else if (auto const* d = std::get_if<Data>(&inner.value)) {
                    content_text += d->json;
                } else if (auto const* e = std::get_if<Error>(&inner.value)) {
                    content_text += e->message;
                }
            }
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("tool_result")},
                {"tool_use_id", json::Value::make_string(tr->call_id)},
                {"content", json::Value::make_string(content_text)},
            };
            if (tr->is_error) block.emplace_back("is_error", json::Value::make_bool(true));
            blocks.push_back(json::Value::make_object(std::move(block)));
        }
        // Reasoning/Media/Citation/Custom: not translated outbound (Reasoning per file banner (3);
        // the rest is the same "narrower than the full content model" gap Phase D's OpenAI backend
        // already names for its own translate_message).
    }

    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(role_to_wire(m.role))));
    obj.emplace_back("content", json::Value::make_array(std::move(blocks)));
    return json::Value::make_object(std::move(obj));
}

// (7): `cache_ttl` validity -- the ONLY two non-empty values Anthropic's `CacheControlEphemeral.Ttl`
// enum accepts (empty means "omit `ttl` entirely, server default of 5 minutes applies"). A pure
// predicate so both the constructor-time contract check and any direct test of it share one definition.
[[nodiscard]] inline bool is_valid_cache_ttl(std::string const& ttl) noexcept {
    return ttl.empty() || ttl == "5m" || ttl == "1h";
}

// (7): one `cache_control` object, shared by both places this backend emits one (the system block and
// the last tool) -- `{"type":"ephemeral"}`, plus `"ttl"` as a sibling when a non-default TTL was
// requested. `cache_ttl` is assumed already validated (is_valid_cache_ttl) by the caller -- this
// function does not re-validate, it only shapes the wire object.
[[nodiscard]] inline json::Value make_cache_control(std::string const& cache_ttl) {
    std::vector<std::pair<std::string, json::Value>> cc{
        {"type", json::Value::make_string("ephemeral")},
    };
    if (!cache_ttl.empty()) cc.emplace_back("ttl", json::Value::make_string(cache_ttl));
    return json::Value::make_object(std::move(cc));
}

// One `ToolDescriptor` -> `{"name","description","input_schema"}` -- flat, no "function" wrapper
// (confirmed against the SDK's `Tool.cs`: `input_schema` is a top-level sibling of `name`, not nested).
// `cache_ttl` (default empty, (7)) is only consulted when `cache_this_one` is true.
[[nodiscard]] inline result<json::Value> translate_tool(ToolDescriptor const& t, bool cache_this_one,
                                                          std::string const& cache_ttl = {}) {
    auto parsed_schema = json::parse(t.args_schema_json);
    if (!parsed_schema) return std::unexpected(parsed_schema.error());
    std::vector<std::pair<std::string, json::Value>> obj{
        {"name", json::Value::make_string(t.name)},
        {"description", json::Value::make_string(t.description)},
        {"input_schema", std::move(*parsed_schema)},
    };
    if (cache_this_one) {
        obj.emplace_back("cache_control", make_cache_control(cache_ttl));
    }
    return json::Value::make_object(std::move(obj));
}

// (5): Anthropic's native structured-output mechanism -- `output_config.format`, `{"type":
// "json_schema","schema":...}` -- distinct from OpenAI's `response_format` wrapper name/shape.
[[nodiscard]] inline result<json::Value> translate_output_config(std::string const& schema_json) {
    auto parsed = json::parse(schema_json);
    if (!parsed) return std::unexpected(parsed.error());
    std::vector<std::pair<std::string, json::Value>> format{
        {"type", json::Value::make_string("json_schema")},
        {"schema", std::move(*parsed)},
    };
    std::vector<std::pair<std::string, json::Value>> output_config{
        {"format", json::Value::make_object(std::move(format))},
    };
    return json::Value::make_object(std::move(output_config));
}

// D1-equivalent: the full `POST /v1/messages` request body. `max_tokens` is REQUIRED by Anthropic
// (unlike OpenAI, where it's optional/deprecated) -- ChatRequest carries no sampling-parameter field
// at all (chat_client.hpp's own file-top comment: "sampling parameters... stay elided"), so this
// falls back to the bound backend's own declared `ChatClientCapabilities::max_output_tokens` when
// nonzero, else a conservative fixed default -- a real, named translation-layer decision, not an
// unstated guess.
inline constexpr std::uint64_t kDefaultMaxTokens = 4096;

// (7): the hard invariant Finding 5 confirms via a real reported API bug -- Anthropic rejects a request
// with `HTTP 400: A maximum of 4 blocks with cache_control may be provided`, counted across system +
// tools + messages COMBINED. A generic recursive walk (not schema-specific) so it stays correct if this
// backend ever grows a THIRD or FOURTH placement site without needing a matching update here: it counts
// every "cache_control" member anywhere in the assembled request body, at any depth. Pure function, no
// network -- independently testable against a hand-built json::Value the public request-building API
// can never itself produce (today's own placements top out at 2).
[[nodiscard]] inline std::size_t count_cache_control_blocks(json::Value const& v) {
    std::size_t count = 0;
    if (v.is_object()) {
        for (auto const& [key, member] : v.as_object()) {
            if (key == "cache_control") ++count;
            count += count_cache_control_blocks(member);
        }
    } else if (v.is_array()) {
        for (auto const& item : v.as_array()) count += count_cache_control_blocks(item);
    }
    return count;
}

inline constexpr std::size_t kMaxCacheControlBlocks = 4;

// `end_user_id`/`cache_ttl` (7): both default-empty, both optional. `end_user_id` non-empty adds
// `metadata.user_id` (Anthropic's abuse-tracking id -- `Metadata` has ONLY this one field). `cache_ttl`
// non-empty is applied to every `cache_control` object this function builds (system block + last tool,
// via `make_cache_control`) -- assumed already validated by the caller (AnthropicChatClient's
// constructor enforces `is_valid_cache_ttl`; this function does not re-check, so it stays directly
// testable with any string, including deliberately invalid ones, without throwing).
[[nodiscard]] inline result<json::Value> build_request_body(ChatRequest const& request,
                                                              std::string const& model,
                                                              ChatClientCapabilities const& caps,
                                                              bool stream,
                                                              std::string const& end_user_id = {},
                                                              std::string const& cache_ttl = {}) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("model", json::Value::make_string(model));
    obj.emplace_back("max_tokens", json::Value::make_number(static_cast<double>(
                                        caps.max_output_tokens != 0 ? caps.max_output_tokens
                                                                     : kDefaultMaxTokens)));

    SplitMessages split = split_system_messages(request.messages);
    if (!split.system_text.empty()) {
        if (caps.prompt_caching) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("text")},
                {"text", json::Value::make_string(split.system_text)},
                {"cache_control", make_cache_control(cache_ttl)},
            };
            std::vector<json::Value> system_blocks;
            system_blocks.push_back(json::Value::make_object(std::move(block)));
            obj.emplace_back("system", json::Value::make_array(std::move(system_blocks)));
        } else {
            obj.emplace_back("system", json::Value::make_string(split.system_text));
        }
    }

    std::vector<json::Value> messages;
    messages.reserve(split.rest.size());
    for (Message const* m : split.rest) messages.push_back(translate_message(*m));
    obj.emplace_back("messages", json::Value::make_array(std::move(messages)));

    if (!request.tools.empty()) {
        std::vector<json::Value> tools;
        tools.reserve(request.tools.size());
        for (std::size_t i = 0; i < request.tools.size(); ++i) {
            bool const is_last = (i + 1 == request.tools.size());
            auto tool_json = translate_tool(request.tools[i], caps.prompt_caching && is_last, cache_ttl);
            if (!tool_json) return std::unexpected(tool_json.error());
            tools.push_back(std::move(*tool_json));
        }
        obj.emplace_back("tools", json::Value::make_array(std::move(tools)));
    }

    if (request.output_schema_json) {
        auto output_config = translate_output_config(*request.output_schema_json);
        if (!output_config) return std::unexpected(output_config.error());
        obj.emplace_back("output_config", std::move(*output_config));
    }

    if (!end_user_id.empty()) {
        std::vector<std::pair<std::string, json::Value>> metadata{
            {"user_id", json::Value::make_string(end_user_id)},
        };
        obj.emplace_back("metadata", json::Value::make_object(std::move(metadata)));
    }

    if (stream) obj.emplace_back("stream", json::Value::make_bool(true));

    json::Value body = json::Value::make_object(std::move(obj));
    if (count_cache_control_blocks(body) > kMaxCacheControlBlocks) {
        return std::unexpected(error{failure_class::contract,
                                      "request would carry more than 4 cache_control blocks combined "
                                      "(system+tools+messages) -- Anthropic rejects this with HTTP 400",
                                      "anthropic.cache_control_limit_exceeded"});
    }
    return body;
}

// `http_referer`/`x_title` (7): OpenRouter's own app-attribution header convention (Finding 1) --
// stamped in ONLY when non-empty, both default-empty, so a caller who never sets them gets exactly
// today's three headers and nothing more.
[[nodiscard]] inline sandbox::NetEgressRequest build_http_request(std::string const& path,
                                                                    std::string const& api_key,
                                                                    std::string const& api_version,
                                                                    std::string body,
                                                                    std::string const& http_referer = {},
                                                                    std::string const& x_title = {}) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path = path;
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("x-api-key", api_key);
    req.headers.emplace_back("anthropic-version", api_version);
    if (!http_referer.empty()) req.headers.emplace_back("HTTP-Referer", http_referer);
    if (!x_title.empty()) req.headers.emplace_back("X-Title", x_title);
    req.body = std::move(body);
    return req;
}

// One inbound content block (§2/§7 of the wire-format research) -> zero-or-one AE ContentItem.
// Shared between the non-streaming response parser and the streaming content_block accumulator.
[[nodiscard]] inline std::optional<ContentItem> translate_response_block(json::Value const& block) {
    auto const* type = block.find("type");
    if (!type || !type->is_string()) return std::nullopt;
    std::string const& kind = type->as_string();

    ContentItem item;
    item.origin = content_origin::assistant;

    if (kind == "text") {
        auto const* text = block.find("text");
        item.value = Text{(text && text->is_string()) ? text->as_string() : std::string{}};
        return item;
    }
    if (kind == "tool_use") {
        auto const* id = block.find("id");
        auto const* name = block.find("name");
        auto const* input = block.find("input");
        ToolCall call;
        call.call_id = (id && id->is_string()) ? id->as_string() : std::string{};
        call.tool_name = (name && name->is_string()) ? name->as_string() : std::string{};
        call.arguments_json = input ? json::dump(*input) : std::string{"{}"};
        item.value = std::move(call);
        return item;
    }
    if (kind == "thinking") {
        // (3): signature is intentionally not preserved -- this content item is response-parsing-only.
        auto const* thinking = block.find("thinking");
        Reasoning r;
        r.text = (thinking && thinking->is_string()) ? thinking->as_string() : std::string{};
        r.encrypted = false;
        item.value = std::move(r);
        return item;
    }
    if (kind == "redacted_thinking") {
        Reasoning r;
        r.text.clear();  // opaque `data` intentionally dropped, see file banner (3)
        r.encrypted = true;
        item.value = std::move(r);
        return item;
    }
    return std::nullopt;  // server tool / container / other block kinds -- not translated
}

// E1: the non-streaming response. `content[]` (§7), `usage.input_tokens`/`output_tokens`/
// `cache_read_input_tokens` (confirmed exact field names, distinct from OpenAI's `prompt_tokens`/
// `completion_tokens`).
[[nodiscard]] inline result<ChatResponse> parse_message_response(json::Value const& body) {
    if (auto const* err = body.find("error")) {
        std::string msg = "unknown error";
        if (auto const* m = err->find("message"); m && m->is_string()) msg = m->as_string();
        return std::unexpected(error{failure_class::contract, "anthropic error: " + msg, "anthropic.error"});
    }
    json::Value const* content = body.find("content");
    if (!content || !content->is_array()) {
        return std::unexpected(
            error{failure_class::contract, "response has no content array", "anthropic.no_content"});
    }

    ChatResponse resp;
    resp.message.role = role::assistant;
    for (json::Value const& block : content->as_array()) {
        if (auto item = translate_response_block(block)) {
            resp.message.content.push_back(std::move(*item));
        }
    }

    // M5 research follow-up item 4 (docs/research/2026-08-07-provider-metadata-and-sampling-params-
    // survey.md Finding 4): the model that ACTUALLY answered -- a sibling of "content" at the top level,
    // empty (never fabricated from the request's own model_ field) when the response doesn't report one.
    if (auto const* model_field = body.find("model"); model_field && model_field->is_string()) {
        resp.model = model_field->as_string();
    }

    if (auto const* usage = body.find("usage")) {
        if (auto const* it = usage->find("input_tokens"); it && it->is_number()) {
            resp.usage.input_tokens = static_cast<std::uint64_t>(it->as_number());
        }
        if (auto const* ot = usage->find("output_tokens"); ot && ot->is_number()) {
            resp.usage.output_tokens = static_cast<std::uint64_t>(ot->as_number());
        }
        if (auto const* crt = usage->find("cache_read_input_tokens"); crt && crt->is_number()) {
            resp.usage.cached_input_tokens = static_cast<std::uint64_t>(crt->as_number());
        }
        // M5 research follow-up item 5 (Finding 5): cache_creation_input_tokens -- tokens spent
        // ESTABLISHING a new cache entry, the symmetric counterpart to cache_read_input_tokens above --
        // now mapped to Usage::cache_write_tokens (previously a named, unmapped gap in this same
        // comment).
        if (auto const* cct = usage->find("cache_creation_input_tokens"); cct && cct->is_number()) {
            resp.usage.cache_write_tokens = static_cast<std::uint64_t>(cct->as_number());
        }
    }

    return resp;
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
    std::string message = "anthropic http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* err = parsed->find("error")) {
            if (auto const* m = err->find("message"); m && m->is_string()) message = m->as_string();
        }
    }
    return error{klass, message, "anthropic.http_" + std::to_string(status)};
}

// (E2) The cumulative-usage reduce, proven against the SDK's own MessageContentAggregator.GetResult
// logic (file banner (6)): output_tokens is unconditionally overwritten by each message_delta event's
// value; input_tokens/cache-token fields are seeded from message_start and only overwritten when a
// later message_delta actually carries a non-null value for that field. Pure function, no network --
// testable against a literal event sequence.
struct AnthropicUsageSnapshot {
    std::optional<std::uint64_t> input_tokens;
    std::uint64_t output_tokens = 0;
    std::optional<std::uint64_t> cache_read_input_tokens;
};

inline void seed_usage_from_message_start(AnthropicUsageSnapshot& snapshot, json::Value const& usage) {
    if (auto const* it = usage.find("input_tokens"); it && it->is_number()) {
        snapshot.input_tokens = static_cast<std::uint64_t>(it->as_number());
    }
    if (auto const* ot = usage.find("output_tokens"); ot && ot->is_number()) {
        snapshot.output_tokens = static_cast<std::uint64_t>(ot->as_number());
    }
    if (auto const* crt = usage.find("cache_read_input_tokens"); crt && crt->is_number()) {
        snapshot.cache_read_input_tokens = static_cast<std::uint64_t>(crt->as_number());
    }
}

inline void accumulate_message_delta_usage(AnthropicUsageSnapshot& snapshot, json::Value const& usage) {
    if (auto const* ot = usage.find("output_tokens"); ot && ot->is_number()) {
        snapshot.output_tokens = static_cast<std::uint64_t>(ot->as_number());  // overwrite, never add
    }
    if (auto const* it = usage.find("input_tokens"); it && it->is_number()) {
        snapshot.input_tokens = static_cast<std::uint64_t>(it->as_number());
    }
    if (auto const* crt = usage.find("cache_read_input_tokens"); crt && crt->is_number()) {
        snapshot.cache_read_input_tokens = static_cast<std::uint64_t>(crt->as_number());
    }
}

// D2-equivalent chunked-transfer decoding -- identical logic to Phase D's OpenAI backend (RFC 9112
// §7.1 framing is vendor-agnostic), duplicated rather than shared across protocol/{openai,anthropic}
// per this project's own "a second, independent copy rather than a shared header" precedent
// (test_provider_http_client.cpp's top comment, applied here to production translation code instead
// of test harness code -- same reasoning: protocol/openai and protocol/anthropic are meant to stay
// independently readable, never cross-including each other's internals).
[[nodiscard]] inline result<std::string> decode_chunked_body(std::string_view body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto const line_end = body.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            return std::unexpected(
                error{failure_class::contract, "truncated chunked body: no chunk-size line terminator",
                      "anthropic.chunked_malformed"});
        }
        std::string_view size_line = body.substr(pos, line_end - pos);
        if (auto const semi = size_line.find(';'); semi != std::string_view::npos) {
            size_line = size_line.substr(0, semi);
        }
        std::size_t chunk_size = 0;
        auto const conv =
            std::from_chars(size_line.data(), size_line.data() + size_line.size(), chunk_size, 16);
        if (conv.ec != std::errc{}) {
            return std::unexpected(
                error{failure_class::contract, "malformed chunk size", "anthropic.chunked_malformed"});
        }
        pos = line_end + 2;
        if (chunk_size == 0) break;
        if (pos + chunk_size > body.size()) {
            return std::unexpected(
                error{failure_class::contract, "truncated chunk body", "anthropic.chunked_malformed"});
        }
        out.append(body.substr(pos, chunk_size));
        pos += chunk_size;
        if (body.substr(pos, 2) != "\r\n") {
            return std::unexpected(error{failure_class::contract, "missing CRLF after chunk data",
                                          "anthropic.chunked_malformed"});
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

// Real-server finding from Phase D's own live verification (a genuine OpenAI-compatible endpoint
// sends `Transfer-Encoding: chunked` on ordinary, non-streaming responses too, not only on SSE), fixed
// identically here rather than rediscovered: every response body this backend reads must be decoded
// through this helper before `json::parse`, streaming or not -- `perform_https_exchange` (Phase C)
// returns the raw, still chunk-framed bytes verbatim when there is no Content-Length header.
[[nodiscard]] inline result<std::string> decoded_response_body(sandbox::NetEgressResponse const& resp) {
    if (!response_is_chunked(resp)) return resp.body;
    return decode_chunked_body(resp.body);
}

// Anthropic's SSE uses NAMED events (an `event: <type>` line paired with the following `data: {...}`
// line) -- structurally different from OpenAI's single always-"data:"-only shape (Phase D's own
// split_sse_data_events), confirmed against the SDK's Sse.cs event-type switch. Every Anthropic event
// fits on one line of JSON.
struct SseEvent {
    std::string_view type;
    std::string_view data;
};

[[nodiscard]] inline std::vector<SseEvent> split_sse_named_events(std::string_view body) {
    std::vector<SseEvent> out;
    std::string_view pending_type;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        auto const nl = body.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("event:")) {
            std::string_view t = line.substr(6);
            while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
            pending_type = t;
        } else if (line.starts_with("data:")) {
            std::string_view d = line.substr(5);
            while (!d.empty() && d.front() == ' ') d.remove_prefix(1);
            out.push_back(SseEvent{pending_type, d});
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

// E2/D2-equivalent: the chunk-to-ChatResponseUpdate translation, factored out of the network call for
// offline testability (mirrors Phase D's `parse_streaming_response_into_updates`). Text deltas emit
// one ChatResponseUpdate per SSE event (real per-chunk fidelity); tool_use input and thinking text
// arrive incrementally across events keyed by `index` and are accumulated per content_block, emitted
// once as a complete item on that block's `content_block_stop`.
[[nodiscard]] inline result<std::vector<ChatResponseUpdate>> parse_streaming_response_into_updates(
    std::string_view raw_body, bool is_chunked) {
    std::string owned;
    std::string_view decoded = raw_body;
    if (is_chunked) {
        auto d = decode_chunked_body(raw_body);
        if (!d) return std::unexpected(d.error());
        owned = std::move(*d);
        decoded = owned;
    }

    struct PendingBlock {
        bool seen = false;
        std::string kind;
        std::string tool_id;
        std::string tool_name;
        std::string tool_input_json;  // accumulated partial_json fragments
        std::string thinking_text;
        bool redacted = false;
    };
    std::vector<PendingBlock> pending_by_index;

    std::vector<ContentItem> items;

    auto ensure_index = [&](std::size_t index) -> PendingBlock& {
        if (index >= pending_by_index.size()) pending_by_index.resize(index + 1);
        return pending_by_index[index];
    };

    for (SseEvent const& ev : split_sse_named_events(decoded)) {
        if (ev.type == "content_block_start") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            auto const* idx = parsed->find("index");
            auto const* cb = parsed->find("content_block");
            if (!idx || !idx->is_number() || !cb) continue;
            PendingBlock& b = ensure_index(static_cast<std::size_t>(idx->as_number()));
            b.seen = true;
            if (auto const* type = cb->find("type"); type && type->is_string()) b.kind = type->as_string();
            if (b.kind == "tool_use") {
                if (auto const* id = cb->find("id"); id && id->is_string()) b.tool_id = id->as_string();
                if (auto const* name = cb->find("name"); name && name->is_string()) b.tool_name = name->as_string();
            } else if (b.kind == "redacted_thinking") {
                b.redacted = true;
            }
        } else if (ev.type == "content_block_delta") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            auto const* idx = parsed->find("index");
            auto const* delta = parsed->find("delta");
            if (!idx || !idx->is_number() || !delta) continue;
            PendingBlock& b = ensure_index(static_cast<std::size_t>(idx->as_number()));
            b.seen = true;
            auto const* dtype = delta->find("type");
            std::string const dkind = (dtype && dtype->is_string()) ? dtype->as_string() : std::string{};
            if (dkind == "text_delta") {
                auto const* text = delta->find("text");
                if (text && text->is_string() && !text->as_string().empty()) {
                    ContentItem item;
                    item.origin = content_origin::assistant;
                    item.value = Text{text->as_string()};
                    items.push_back(std::move(item));
                }
            } else if (dkind == "input_json_delta") {
                if (auto const* pj = delta->find("partial_json"); pj && pj->is_string()) {
                    b.tool_input_json += pj->as_string();
                }
            } else if (dkind == "thinking_delta") {
                if (auto const* th = delta->find("thinking"); th && th->is_string()) {
                    b.thinking_text += th->as_string();
                }
            }
            // signature_delta: signature intentionally dropped (file banner (3)).
        }
        // content_block_stop/message_start/message_delta/message_stop: block completion is inferred
        // from `pending_by_index` state after the whole event list is walked (this backend's HTTP
        // fetch is not incremental either -- see Phase D's own note -- so there is nothing gained by
        // reacting to content_block_stop specifically here).
    }

    for (auto const& b : pending_by_index) {
        if (!b.seen) continue;
        if (b.kind == "tool_use") {
            ToolCall call;
            call.call_id = b.tool_id;
            call.tool_name = b.tool_name;
            call.arguments_json = b.tool_input_json.empty() ? "{}" : b.tool_input_json;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(call);
            items.push_back(std::move(item));
        } else if (b.kind == "thinking") {
            Reasoning r;
            r.text = b.thinking_text;
            r.encrypted = false;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(r);
            items.push_back(std::move(item));
        } else if (b.redacted) {
            Reasoning r;
            r.encrypted = true;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(r);
            items.push_back(std::move(item));
        }
        // "text" blocks: already emitted per-delta above, nothing to append here.
    }

    std::vector<ChatResponseUpdate> updates;
    updates.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        ChatResponseUpdate update;
        update.delta = std::move(items[i]);
        update.is_final = (i + 1 == items.size());
        updates.push_back(std::move(update));
    }
    return updates;
}

// The detached background worker -- see protocol/openai/chat_client.hpp's own identical rationale for
// why detached, not a tracked thread member (a bound instance is shared across concurrent streaming
// calls via ChatClientRegistry).
inline void run_stream_worker(std::string host, std::uint16_t port, std::string path, std::string api_key,
                               std::string api_version, std::string model, ChatClientCapabilities caps,
                               ChatRequest request, stream_producer<ChatResponseUpdate> producer,
                               Resolver resolver, std::string ca_bundle_pem_override,
                               std::string http_referer, std::string x_title, std::string end_user_id,
                               std::string cache_ttl, sandbox::ProviderTransport transport,
                               std::stop_token stop) {
    auto body = build_request_body(request, model, caps, /*stream=*/true, end_user_id, cache_ttl);
    if (!body) {
        producer.fail(quark::error{quark::errc::validation, "anthropic.request_build_failed"});
        return;
    }
    auto req = build_http_request(path, api_key, api_version, json::dump(*body), http_referer, x_title);
    auto resp = sandbox::perform_provider_https_exchange(host, port, req, stop, std::nullopt, resolver,
                                                            ca_bundle_pem_override, transport);
    if (!resp) {
        producer.fail(quark::error{quark::errc::unavailable, "anthropic.exchange_failed"});
        return;
    }
    if (resp->status < 200 || resp->status >= 300) {
        producer.fail(quark::error{quark::errc::validation, "anthropic.http_error_status"});
        return;
    }

    auto updates = parse_streaming_response_into_updates(resp->body, response_is_chunked(*resp));
    if (!updates) {
        producer.fail(quark::error{quark::errc::serialization, "anthropic.stream_parse_failed"});
        return;
    }

    for (auto& update : *updates) {
        if (producer.push(std::move(update)) != quark::ReplyPush::Ok) {
            return;  // consumer cancelled/deadlined -- stop producing, the ring already latched why
        }
    }
    producer.close();
}

}  // namespace detail

// The real, product-code `ChatClient` conformer for 004 §3's first-class Anthropic backend. `Store`
// is any real `SecretStore` (`AgentEngineSecretStore` in production; `InMemorySecretStore` in tests).
template <SecretStore Store>
class AnthropicChatClient {
public:
    // (7): four OPTIONAL trailing parameters, appended strictly at the END of the existing list, never
    // inserted -- every existing positional-argument construction call site (tests/test_anthropic_chat_
    // client_live.cpp has several) keeps compiling unchanged. `http_referer`/`x_title` (research doc
    // item 1) and `end_user_id` (item 2) have no validity constraint of their own -- any string,
    // including empty (the "don't send this header/field at all" default), is accepted verbatim.
    // `cache_ttl` (item 7) DOES have a real constraint (Anthropic's `CacheControlEphemeral.Ttl` enum has
    // exactly two non-empty values) -- checked here, at construction, a cold setup path where
    // CONVENTIONS.md permits an exception to surface (this project's error model, `ae::result<T>`, has
    // no return channel from a constructor to route a rejection through instead).
    AnthropicChatClient(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                         ChatClientCapabilities caps, Store const& store, std::string path_prefix = "/v1",
                         std::string api_version = "2023-06-01",
                         detail::Resolver resolver = sandbox::resolve_host,
                         std::string ca_bundle_pem_override = {}, std::string http_referer = {},
                         std::string x_title = {}, std::string end_user_id = {}, std::string cache_ttl = {},
                         sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls)
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          capabilities_(caps),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          api_version_(std::move(api_version)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          http_referer_(std::move(http_referer)),
          x_title_(std::move(x_title)),
          end_user_id_(std::move(end_user_id)),
          cache_ttl_(std::move(cache_ttl)),
          transport_(transport) {
        if (!detail::is_valid_cache_ttl(cache_ttl_)) {
            throw std::invalid_argument(
                "AnthropicChatClient: cache_ttl must be \"\" (server default), \"5m\", or \"1h\"");
        }
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
        // Resolution happens HERE, inside chat(), against EffectContext -- never at construction
        // (004 §1 / 018 §4, the same rule test_chat_client_credential_resolution.cpp proves).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_request_body(request, model_, capabilities_, /*stream=*/false,
                                                end_user_id_, cache_ttl_);
        if (!body) co_return std::unexpected(body.error());

        auto req = detail::build_http_request(path_prefix_ + "/messages", lease->reveal_text(),
                                                api_version_, json::dump(*body), http_referer_, x_title_);
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
        co_return detail::parse_message_response(*parsed);
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) {
            pair.producer.fail(quark::error{quark::errc::validation, "secret.not_granted"});
            return std::move(pair.consumer);
        }
        // ADR-017: read the token BEFORE moving the producer (see the OpenAI backend's identical note
        // -- unspecified argument evaluation order would otherwise let this read a moved-from producer).
        std::stop_token stop = pair.producer.stop_token();
        std::thread(&detail::run_stream_worker, host_, port_, path_prefix_ + "/messages",
                    lease->reveal_text(), api_version_, model_, capabilities_, std::move(request),
                    std::move(pair.producer), resolver_, ca_bundle_pem_override_, http_referer_, x_title_,
                    end_user_id_, cache_ttl_, transport_, std::move(stop))
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
    std::string api_version_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    std::string http_referer_;
    std::string x_title_;
    std::string end_user_id_;
    std::string cache_ttl_;
    sandbox::ProviderTransport transport_;
};

}  // namespace agentengine::anthropic

#endif  // AGENTENGINE_WITH_HTTPS
