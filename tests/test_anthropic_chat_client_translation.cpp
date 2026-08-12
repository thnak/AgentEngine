// Milestone 5 Phase E (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// protocol/anthropic/chat_client.hpp's translation logic against LITERAL Anthropic wire-format JSON --
// no network, no live server. Wire-format field names were sourced directly from the official
// Anthropic C# SDK's generated model code (see chat_client.hpp's own top comment). Mirrors Phase D's
// test_openai_chat_client_translation.cpp in structure and coverage intent.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"

using namespace agentengine;
using namespace agentengine::anthropic::detail;

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

[[nodiscard]] Message text_message(role r, std::string text) {
    Message m;
    m.role = r;
    ContentItem item;
    item.origin = (r == role::user) ? content_origin::user : content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// New helpers (deeper tool-call coverage, below): a lone assistant turn carrying a ToolCall, and a
// lone tool-role turn carrying a ToolResult reply -- mirrors test_openai_chat_client_translation.cpp's
// own identically-named helpers, adapted to this project's uniform ToolCall::arguments_json string
// (Anthropic-specific reshaping into a real JSON object happens inside translate_message itself).
[[nodiscard]] Message tool_call_message(std::string call_id, std::string tool_name, std::string args_json) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] Message tool_result_message(std::string call_id, std::string reply_text) {
    Message m;
    m.role = role::tool;
    ContentItem item;
    item.origin = content_origin::tool;
    ToolResult tr;
    tr.call_id = std::move(call_id);
    ContentItem inner;
    inner.value = Text{std::move(reply_text)};
    tr.content.push_back(std::move(inner));
    item.value = std::move(tr);
    m.content.push_back(std::move(item));
    return m;
}

// A nested/complex OutputSchema<T> type for the E5-R2 full-pipeline test below -- a REAL
// AE_JSON_SCHEMA(...)-compiled schema (a nested array-of-objects field plus a std::optional field),
// mirroring test_json_schema_codec.cpp's own Hit/SearchReply nesting pattern and
// test_openai_chat_client_translation.cpp's own identically-shaped ForecastDay/WeatherReport pair
// (each TU keeps its own independent copy -- this project's own established "independent copy rather
// than a shared header" discipline for test code, named in test_provider_http_client.cpp's own top
// comment) -- reused here via schema::json_schema_of<T>() rather than a hand-typed literal.
struct ForecastDay {
    std::string condition;
    double high_f;
    double low_f;
};
AE_JSON_SCHEMA(ForecastDay, condition, high_f, low_f)

struct WeatherReport {
    std::string location;
    std::vector<ForecastDay> forecast;
    std::optional<std::string> alert;
};
AE_JSON_SCHEMA(WeatherReport, location, forecast, alert)

}  // namespace

int main() {
    // ---- E1: system-message extraction (Anthropic has no role:"system" in messages[]) -------------
    {
        std::vector<Message> messages{
            text_message(role::system, "You are a helpful assistant."),
            text_message(role::user, "hi"),
            text_message(role::system, " Be concise."),
        };
        SplitMessages split = split_system_messages(messages);
        check(split.system_text == "You are a helpful assistant. Be concise.",
              "E1-R1: multiple system messages concatenate, in order, into the top-level system text");
        check(split.rest.size() == 1 && split.rest[0]->role == role::user,
              "E1-R1: system messages are excluded from the remaining (translatable) message list");
    }

    // ---- E1: role::tool has no Anthropic role -- translates to wire role "user" -------------------
    {
        Message m;
        m.role = role::tool;
        ContentItem item;
        item.origin = content_origin::tool;
        ToolResult tr;
        tr.call_id = "call-1";
        ContentItem inner;
        inner.value = Text{"72F"};
        tr.content.push_back(std::move(inner));
        item.value = std::move(tr);
        m.content.push_back(std::move(item));

        json::Value wire = translate_message(m);
        auto const* role_field = wire.find("role");
        check(role_field && role_field->as_string() == "user",
              "E1-R2: role::tool translates to wire role 'user' (Anthropic has no tool role)");
        auto const* content = wire.find("content");
        check(content && content->is_array() && content->as_array().size() == 1, "E1-R2: one content block");
        if (content && !content->as_array().empty()) {
            json::Value const& block = content->as_array().front();
            auto const* type = block.find("type");
            auto const* tool_use_id = block.find("tool_use_id");
            auto const* text = block.find("content");
            check(type && type->as_string() == "tool_result", "E1-R2: block type is 'tool_result'");
            check(tool_use_id && tool_use_id->as_string() == "call-1",
                  "E1-R2: tool_use_id sourced from ToolResult's own call_id");
            check(text && text->as_string() == "72F", "E1-R2: tool reply content flattens correctly");
        }
    }

    // ---- E1(2): tool_use.input is a REAL JSON OBJECT, not a stringified string ----------------------
    {
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.value = ToolCall{"call-1", "get_weather", "{\"location\":\"Seattle\"}"};
        m.content.push_back(std::move(item));

        json::Value wire = translate_message(m);
        auto const* content = wire.find("content");
        check(content && content->is_array() && content->as_array().size() == 1, "E1-R3: one content block");
        if (content && !content->as_array().empty()) {
            json::Value const& block = content->as_array().front();
            auto const* type = block.find("type");
            auto const* id = block.find("id");
            auto const* name = block.find("name");
            auto const* input = block.find("input");
            check(type && type->as_string() == "tool_use", "E1-R3: block type is 'tool_use'");
            check(id && id->as_string() == "call-1", "E1-R3: id round-trips");
            check(name && name->as_string() == "get_weather", "E1-R3: name round-trips");
            check(input && input->is_object(),
                  "E1-R3: input is a REAL JSON OBJECT (unlike OpenAI's stringified arguments)");
            if (input) {
                auto const* location = input->find("location");
                check(location && location->is_string() && location->as_string() == "Seattle",
                      "E1-R3: input's fields are directly queryable, not nested inside a string");
            }
        }
    }

    // ---- E1: tool-schema shaping is flat -- input_schema, no "function" wrapper --------------------
    {
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "Get the weather for a location";
        t.args_schema_json = R"({"type":"object","properties":{"location":{"type":"string"}},"required":["location"]})";
        auto wire = translate_tool(t, /*cache_this_one=*/false);
        check(wire.has_value(), "E1-R4: a well-formed args schema translates without error");
        if (wire) {
            check(wire->find("name") && wire->find("name")->as_string() == "get_weather",
                  "E1-R4: name is a top-level sibling, not nested under 'function'");
            check(wire->find("input_schema") != nullptr,
                  "E1-R4: the wire key is 'input_schema', not 'parameters'");
            check(wire->find("function") == nullptr, "E1-R4: no 'function' wrapper object exists at all");
            check(wire->find("cache_control") == nullptr, "E1-R4: no cache_control when not requested");
        }
    }
    {
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "weather";
        t.args_schema_json = R"({"type":"object","properties":{}})";
        auto wire = translate_tool(t, /*cache_this_one=*/true);
        check(wire.has_value(), "E4-R1: translates with caching requested");
        if (wire) {
            auto const* cc = wire->find("cache_control");
            check(cc != nullptr, "E4-R1: cache_control present when requested");
            if (cc) {
                auto const* type = cc->find("type");
                check(type && type->as_string() == "ephemeral", "E4-R1: cache_control type is 'ephemeral'");
            }
        }
    }

    // ---- E5: native structured-output shaping (output_config.format, distinct from OpenAI's) -------
    {
        std::string schema = R"({"type":"object","properties":{"answer":{"type":"string"}}})";
        auto wire = translate_output_config(schema);
        check(wire.has_value(), "E5-R1: translates without error");
        if (wire) {
            auto const* format = wire->find("format");
            check(format != nullptr, "E5-R1: output_config.format present");
            if (format) {
                auto const* type = format->find("type");
                auto const* schema_field = format->find("schema");
                check(type && type->as_string() == "json_schema", "E5-R1: format.type is 'json_schema'");
                check(schema_field && schema_field->is_object(), "E5-R1: format.schema is the parsed schema object");
                check(schema_field && schema_field->find("additionalProperties") == nullptr,
                      "E5-R1: unlike Phase D's OpenAI backend, additionalProperties:false is NOT forced "
                      "(unconfirmed requirement for Anthropic -- not asserted)");
            }
        }
    }

    // ---- E1: full request body assembly, including max_tokens fallback and system+cache -----------
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::system, "be helpful"));
        req.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps;
        caps.prompt_caching = true;
        caps.max_output_tokens = 0;  // unset -- falls back to kDefaultMaxTokens

        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false);
        check(body.has_value(), "E1-R5: a full request body assembles without error");
        if (body) {
            auto const* max_tokens = body->find("max_tokens");
            check(max_tokens && max_tokens->is_number() && max_tokens->as_number() == kDefaultMaxTokens,
                  "E1-R5: max_tokens falls back to the fixed default when caps.max_output_tokens is unset "
                  "(Anthropic requires max_tokens; ChatRequest carries no such field)");
            auto const* system = body->find("system");
            check(system && system->is_array(),
                  "E1-R5: system is the ARRAY form (not a plain string) when prompt_caching is declared, "
                  "so a cache_control breakpoint can attach to it");
            if (system && !system->as_array().empty()) {
                auto const* cc = system->as_array().front().find("cache_control");
                check(cc != nullptr, "E1-R5: the system block itself carries the cache breakpoint");
            }
            auto const* messages = body->find("messages");
            check(messages && messages->as_array().size() == 1,
                  "E1-R5: the system message was extracted, only the user message remains in messages[]");
        }
    }
    {
        // Without prompt_caching declared, system stays the plain-string shorthand -- no unrequested
        // cache_control ever appears.
        ChatRequest req;
        req.messages.push_back(text_message(role::system, "be helpful"));
        ChatClientCapabilities caps;  // prompt_caching = false (default)
        caps.max_output_tokens = 8192;
        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false);
        check(body.has_value(), "E1-R6: translates without error");
        if (body) {
            auto const* max_tokens = body->find("max_tokens");
            check(max_tokens && max_tokens->as_number() == 8192,
                  "E1-R6: a nonzero caps.max_output_tokens is used verbatim, not the fixed default");
            auto const* system = body->find("system");
            check(system && system->is_string(),
                  "E1-R6: system stays the plain-string shorthand when caching isn't declared");
        }
    }

    // ---- E1: response parsing -- text + tool_use ----------------------------------------------------
    {
        auto parsed = json::parse(R"({
            "id":"msg_1","type":"message","role":"assistant",
            "content":[
                {"type":"text","text":"Let me check that."},
                {"type":"tool_use","id":"call-1","name":"get_weather","input":{"location":"Seattle"}}
            ],
            "stop_reason":"tool_use",
            "usage":{"input_tokens":42,"output_tokens":17,"cache_read_input_tokens":30}
        })");
        check(parsed.has_value(), "E1-R7: literal wire JSON parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value(), "E1-R7: a text+tool_use response parses into a ChatResponse");
            if (resp) {
                check(resp->message.content.size() == 2, "E1-R7: two content items");
                if (resp->message.content.size() == 2) {
                    auto const* text = std::get_if<Text>(&resp->message.content[0].value);
                    auto const* tc = std::get_if<ToolCall>(&resp->message.content[1].value);
                    check(text && text->text == "Let me check that.", "E1-R7: text round-trips");
                    check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather",
                          "E1-R7: tool_use id/name round-trip");
                    check(tc && tc->arguments_json.find("Seattle") != std::string::npos,
                          "E1-R7: the JSON OBJECT input round-trips into arguments_json (dumped back to a "
                          "string, this project's own uniform representation)");
                }
                check(resp->usage.input_tokens == 42 && resp->usage.output_tokens == 17,
                      "E1-R7: usage.input_tokens/output_tokens map directly (distinct field names from "
                      "OpenAI's prompt_tokens/completion_tokens)");
                check(resp->usage.cached_input_tokens == 30,
                      "E1-R7: cache_read_input_tokens maps to cached_input_tokens");
            }
        }
    }

    // ---- E3: thinking / redacted_thinking response parsing (never round-tripped outbound) ----------
    {
        auto parsed = json::parse(R"({
            "type":"message","role":"assistant",
            "content":[
                {"type":"thinking","thinking":"The user wants weather info.","signature":"abc123"},
                {"type":"text","text":"Sure, one moment."}
            ]
        })");
        check(parsed.has_value(), "E3-R1: literal thinking-block wire JSON parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value(), "E3-R1: parses into a ChatResponse");
            if (resp && resp->message.content.size() == 2) {
                auto const* reasoning = std::get_if<Reasoning>(&resp->message.content[0].value);
                check(reasoning && reasoning->text == "The user wants weather info." && !reasoning->encrypted,
                      "E3-R1: thinking text round-trips as Reasoning{encrypted=false} (signature dropped "
                      "by design, not preserved anywhere in this project's content model)");
            }
        }
    }
    {
        auto parsed = json::parse(R"({
            "type":"message","role":"assistant",
            "content":[{"type":"redacted_thinking","data":"opaque-ciphertext-blob"}]
        })");
        check(parsed.has_value(), "E3-R2: literal redacted_thinking wire JSON parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value() && resp->message.content.size() == 1, "E3-R2: parses to one item");
            if (resp && resp->message.content.size() == 1) {
                auto const* reasoning = std::get_if<Reasoning>(&resp->message.content[0].value);
                check(reasoning && reasoning->encrypted && reasoning->text.empty(),
                      "E3-R2: redacted_thinking maps to Reasoning{encrypted=true, text empty} -- the "
                      "opaque 'data' is never exposed as visible text");
            }
        }
    }
    {
        // Outbound: a Reasoning content item is silently dropped, never sent back as a thinking block.
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.value = Reasoning{"some prior reasoning", false};
        m.content.push_back(std::move(item));
        ContentItem text_item;
        text_item.value = Text{"the answer"};
        m.content.push_back(std::move(text_item));

        json::Value wire = translate_message(m);
        auto const* content = wire.find("content");
        check(content && content->as_array().size() == 1,
              "E3-R3: a Reasoning content item is dropped from outbound translation (never re-sent as a "
              "thinking block without its real signature) -- only the text item survives");
    }

    // ---- E1: an error envelope maps to a real failure ------------------------------------------------
    {
        auto parsed = json::parse(R"({"type":"error","error":{"type":"invalid_request_error","message":"bad model"}})");
        check(parsed.has_value(), "E1-R8: literal error wire JSON parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(!resp.has_value(), "E1-R8: an error envelope fails translation, never a fabricated success");
            if (!resp) {
                check(resp.error().message.find("bad model") != std::string::npos,
                      "E1-R8: the provider's own error message is preserved");
            }
        }
    }

    // ---- D2/E2-equivalent: chunked-transfer decoding (same RFC 9112 logic as Phase D) --------------
    {
        std::string chunked = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        auto decoded = decode_chunked_body(chunked);
        check(decoded.has_value() && *decoded == "hello world", "chunked decoding works identically to Phase D");
    }

    // ---- Named-event SSE splitting (distinct from OpenAI's single-field shape) ----------------------
    {
        std::string sse =
            "event: message_start\ndata: {\"a\":1}\n\n"
            "event: content_block_start\ndata: {\"b\":2}\n\n"
            "event: message_stop\ndata: {}\n\n";
        auto events = split_sse_named_events(sse);
        check(events.size() == 3, "SSE-R1: three named events extracted, in order");
        if (events.size() == 3) {
            check(events[0].type == "message_start" && events[0].data == "{\"a\":1}",
                  "SSE-R1: event type and data correctly paired for event 1");
            check(events[1].type == "content_block_start" && events[1].data == "{\"b\":2}",
                  "SSE-R1: event type and data correctly paired for event 2");
            check(events[2].type == "message_stop", "SSE-R1: event 3 type correct");
        }
    }

    // ---- E2: the cumulative-usage reduce (overwrite, never add) -------------------------------------
    {
        AnthropicUsageSnapshot snapshot;
        auto start_usage = json::parse(R"({"input_tokens":100,"output_tokens":1,"cache_read_input_tokens":20})");
        check(start_usage.has_value(), "E2-R1: literal message_start usage parses");
        if (start_usage) seed_usage_from_message_start(snapshot, *start_usage);
        check(snapshot.input_tokens == 100 && snapshot.output_tokens == 1,
              "E2-R1: message_start seeds the initial snapshot");

        // First message_delta: output_tokens grows, no input_tokens field present (typical).
        auto delta1 = json::parse(R"({"output_tokens":15})");
        check(delta1.has_value(), "E2-R1: literal message_delta usage parses");
        if (delta1) accumulate_message_delta_usage(snapshot, *delta1);
        check(snapshot.output_tokens == 15,
              "E2-R1: output_tokens is OVERWRITTEN by the delta's cumulative value, not summed (1+15=16 "
              "would be wrong -- the wire value IS already the running total)");
        check(snapshot.input_tokens == 100,
              "E2-R1: input_tokens carries forward from message_start when the delta omits it");

        // Second message_delta: output_tokens grows further.
        auto delta2 = json::parse(R"({"output_tokens":42})");
        if (delta2) accumulate_message_delta_usage(snapshot, *delta2);
        check(snapshot.output_tokens == 42, "E2-R1: the second delta's cumulative total overwrites again");
        check(snapshot.input_tokens == 100 && snapshot.cache_read_input_tokens == 20,
              "E2-R1: input_tokens/cache_read_input_tokens remain stable across deltas that omit them");
    }

    // ---- E2/D2-equivalent: full streaming-chunk-to-ChatResponseUpdate translation -------------------
    {
        std::string sse =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"
            "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"there\"}}\n\n"
            "event: content_block_stop\ndata: {\"index\":0}\n\n"
            "event: content_block_start\ndata: {\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call-1\",\"name\":\"get_weather\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"location\\\":\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"Seattle\\\"}\"}}\n\n"
            "event: content_block_stop\ndata: {\"index\":1}\n\n"
            "event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":25}}\n\n"
            "event: message_stop\ndata: {}\n\n";
        auto updates = parse_streaming_response_into_updates(sse, /*is_chunked=*/false);
        check(updates.has_value(), "E-STREAM-R1: a full named-event SSE stream parses into an update list");
        if (updates) {
            check(updates->size() == 3,
                  "E-STREAM-R1: two text deltas (own chunk boundaries) + one assembled tool_use = 3 updates");
            if (updates->size() == 3) {
                auto const* t0 = std::get_if<Text>(&(*updates)[0].delta.value);
                auto const* t1 = std::get_if<Text>(&(*updates)[1].delta.value);
                check(t0 && t0->text == "Hi ", "E-STREAM-R1: first text delta exact");
                check(t1 && t1->text == "there", "E-STREAM-R1: second text delta exact");
                auto const* tc = std::get_if<ToolCall>(&(*updates)[2].delta.value);
                check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather",
                      "E-STREAM-R1: tool_use id/name survive fragment reassembly");
                check(tc && tc->arguments_json == R"({"location":"Seattle"})",
                      "E-STREAM-R1: partial_json fragments from separate content_block_delta events "
                      "concatenate into valid JSON");
                check((*updates)[2].is_final, "E-STREAM-R1: the last update is marked is_final");
                check(!(*updates)[0].usage.has_value() && !(*updates)[1].usage.has_value(),
                      "E-STREAM-R1: usage is not attached to the interim (non-final) updates");
                check((*updates)[2].usage.has_value(),
                      "E-STREAM-R1 (ADR-035): the final update carries real usage -- the message_start/"
                      "message_delta events threaded through StreamingUpdateAccumulator all the way to "
                      "ChatResponseUpdate.usage, closing the gap AnthropicUsageSnapshot/"
                      "seed_usage_from_message_start/accumulate_message_delta_usage were already tested "
                      "for (E2-R1) but never wired to actually populate");
                if ((*updates)[2].usage.has_value()) {
                    check((*updates)[2].usage->input_tokens == 10,
                          "E-STREAM-R1: input_tokens comes from message_start (10), never overwritten by "
                          "message_delta since message_delta's usage omits input_tokens here");
                    check((*updates)[2].usage->output_tokens == 25,
                          "E-STREAM-R1: output_tokens is message_delta's cumulative total (25), NOT "
                          "message_start's initial value (1) and NOT their sum (26)");
                }
            }
        }
    }

    // ---- E-STREAM-R2 (ADR-035): a completion with genuinely no content items (no text, no tool call, no
    // thinking) still surfaces real usage -- the synthesized-empty-final-update path in finish() ---------
    {
        std::string sse =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":8,\"output_tokens\":0}}}\n\n"
            "event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}\n\n"
            "event: message_stop\ndata: {}\n\n";
        auto updates = parse_streaming_response_into_updates(sse, /*is_chunked=*/false);
        check(updates.has_value(), "E-STREAM-R2: a content-free stream (usage-only) still parses");
        if (updates) {
            check(updates->size() == 1,
                  "E-STREAM-R2: exactly one synthesized final update carries the usage, even though no "
                  "content_block ever arrived to hold it");
            if (updates->size() == 1) {
                check((*updates)[0].is_final, "E-STREAM-R2: the synthesized update is marked is_final");
                check((*updates)[0].usage.has_value(), "E-STREAM-R2: the synthesized update carries usage");
                if ((*updates)[0].usage.has_value()) {
                    check((*updates)[0].usage->input_tokens == 8 && (*updates)[0].usage->output_tokens == 3,
                          "E-STREAM-R2: usage values are correct on the synthesized update (input from "
                          "message_start, output overwritten by message_delta's cumulative total)");
                }
            }
        }
    }

    // ---- E1: multi-turn conversation round trip (system + user -> assistant tool_use -> tool reply ->
    // user) -- proves the tool-reply message correctly becomes wire role "user" with a tool_result
    // block, sitting correctly IN SEQUENCE alongside the other turns, not merely in isolation ----------
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::system, "You are a weather assistant."));
        req.messages.push_back(text_message(role::user, "what's the weather in Seattle?"));
        req.messages.push_back(tool_call_message("call-1", "get_weather", R"({"location":"Seattle"})"));
        req.messages.push_back(tool_result_message("call-1", "58F and cloudy"));
        req.messages.push_back(text_message(role::user, "should I bring an umbrella?"));
        ChatClientCapabilities caps;  // prompt_caching = false -- plain system string, not the focus here

        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false);
        check(body.has_value(), "E1-R9: a 5-message conversation with a tool round-trip assembles without error");
        if (body) {
            auto const* system = body->find("system");
            check(system && system->is_string() && system->as_string() == "You are a weather assistant.",
                  "E1-R9: the system message is extracted out of messages[] entirely, as E1-R1 already "
                  "proves in isolation -- here proven alongside a full multi-turn tool exchange");
            auto const* messages = body->find("messages");
            check(messages && messages->is_array() && messages->as_array().size() == 4,
                  "E1-R9: the remaining FOUR non-system turns (question, tool_use, tool_result, "
                  "follow-up) all appear in messages[], in order");
            if (messages && messages->as_array().size() == 4) {
                auto const& arr = messages->as_array();
                check(arr[0].find("role")->as_string() == "user", "E1-R9: turn 1 (the question) is role 'user'");
                check(arr[1].find("role")->as_string() == "assistant",
                      "E1-R9: turn 2 (the tool call) is role 'assistant'");
                auto const* t2content = arr[1].find("content");
                check(t2content && t2content->is_array() && t2content->as_array().size() == 1 &&
                          t2content->as_array().front().find("type")->as_string() == "tool_use",
                      "E1-R9: turn 2's content is a single tool_use block");
                check(arr[2].find("role")->as_string() == "user",
                      "E1-R9: turn 3 (the tool reply) becomes wire role 'user' -- Anthropic's own "
                      "tool_result-as-user-message shape -- sitting correctly BETWEEN the tool_use turn "
                      "and the follow-up user turn, not appended out of order at the end");
                auto const* t3content = arr[2].find("content");
                check(t3content && t3content->is_array() && t3content->as_array().size() == 1 &&
                          t3content->as_array().front().find("type")->as_string() == "tool_result" &&
                          t3content->as_array().front().find("tool_use_id")->as_string() == "call-1",
                      "E1-R9: turn 3's tool_result block correlates back to turn 2's tool_use id");
                auto const* t3text = t3content->as_array().front().find("content");
                check(t3text && t3text->as_string() == "58F and cloudy",
                      "E1-R9: the tool reply's own text content is preserved exactly inside the "
                      "tool_result block");
                check(arr[3].find("role")->as_string() == "user",
                      "E1-R9: turn 4 (the follow-up) is an ordinary trailing user message, correctly "
                      "AFTER the tool exchange, not interleaved incorrectly with it");
            }
        }
    }

    // ---- E1: a complex/nested args_schema_json passes through translate_tool completely unchanged -----
    {
        ToolDescriptor t;
        t.name = "book_flight";
        t.description = "Book a flight with multiple passengers and stops";
        t.args_schema_json = R"({"type":"object","properties":{)"
            R"("passengers":{"type":"array","items":{"type":"object","properties":{)"
            R"("name":{"type":"string"},"age":{"type":"integer"},)"
            R"("seat_pref":{"type":"object","properties":{"aisle":{"type":"boolean"},)"
            R"("window":{"type":"boolean"}}}},"required":["name"]}},)"
            R"("legs":{"type":"array","items":{"type":"object","properties":{)"
            R"("from":{"type":"string"},"to":{"type":"string"}},"required":["from","to"]}}},)"
            R"("required":["passengers","legs"]})";

        auto wire = translate_tool(t, /*cache_this_one=*/false);
        check(wire.has_value(), "E1-R10: a deeply nested args schema (the SAME nested shape "
                                 "test_openai_chat_client_translation.cpp's D3-R3 case proves for that "
                                 "backend) translates without error here too");
        if (wire) {
            auto const* schema_v = wire->find("input_schema");
            check(schema_v != nullptr, "E1-R10: input_schema present (flat, no 'function' wrapper, as "
                                        "E1-R4 already establishes)");
            if (schema_v) {
                auto original_parsed = json::parse(t.args_schema_json);
                check(original_parsed.has_value(), "E1-R10: the original nested schema itself parses");
                if (original_parsed) {
                    check(json::dump(*schema_v) == json::dump(*original_parsed),
                          "E1-R10: the nested schema survives translate_tool byte-for-byte, unchanged -- "
                          "no client-side reshaping of a caller's declared JSON Schema");
                }
            }
        }
    }

    // ---- E4: with 3+ tools and prompt_caching=true, cache_control appears ONLY on the LAST tool --------
    // E4-R1 already proves the single-tool case; this proves the placement RULE (last, not first/middle)
    // actually holds once there is more than one tool to place it wrong on.
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        ToolDescriptor t1;
        t1.name = "tool_one";
        t1.description = "first";
        t1.args_schema_json = R"({"type":"object","properties":{}})";
        ToolDescriptor t2;
        t2.name = "tool_two";
        t2.description = "second";
        t2.args_schema_json = R"({"type":"object","properties":{}})";
        ToolDescriptor t3;
        t3.name = "tool_three";
        t3.description = "third";
        t3.args_schema_json = R"({"type":"object","properties":{}})";
        req.tools = {t1, t2, t3};
        ChatClientCapabilities caps;
        caps.prompt_caching = true;

        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false);
        check(body.has_value(), "E4-R2: a 3-tool request with prompt_caching declared assembles without error");
        if (body) {
            auto const* tools = body->find("tools");
            check(tools && tools->is_array() && tools->as_array().size() == 3, "E4-R2: all three tools present");
            if (tools && tools->as_array().size() == 3) {
                auto const& arr = tools->as_array();
                check(arr[0].find("cache_control") == nullptr,
                      "E4-R2: the FIRST tool carries no cache_control breakpoint");
                check(arr[1].find("cache_control") == nullptr,
                      "E4-R2: the MIDDLE tool carries no cache_control breakpoint either");
                check(arr[2].find("cache_control") != nullptr,
                      "E4-R2: only the LAST tool carries the cache_control breakpoint -- the stable-"
                      "prefix placement 004 §8 Q2 names, never repeated on every tool in the list");
            }
        }
    }

    // ---- E1: a response with text AND multiple tool_use blocks, ARBITRARILY interleaved -- order proof --
    // Unlike OpenAI's fixed "text field, then a separate tool_calls array" shape (proven in
    // test_openai_chat_client_translation.cpp's D1-R9), Anthropic's content[] is a single ordered array
    // that can genuinely interleave text and tool_use blocks in any order the model chooses -- this
    // proves translate_response_block's per-block dispatch preserves that exact wire order.
    {
        auto parsed = json::parse(R"({
            "type":"message","role":"assistant",
            "content":[
                {"type":"tool_use","id":"call-1","name":"get_weather","input":{"city":"Seattle"}},
                {"type":"text","text":"and also,"},
                {"type":"tool_use","id":"call-2","name":"get_forecast","input":{"days":3}},
                {"type":"text","text":"here's the forecast."}
            ]
        })");
        check(parsed.has_value(), "E1-R11: literal interleaved text+tool_use wire JSON parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value(), "E1-R11: an interleaved text+tool_use response parses");
            if (resp) {
                check(resp->message.content.size() == 4, "E1-R11: all four blocks translate to content items");
                if (resp->message.content.size() == 4) {
                    auto const* i0 = std::get_if<ToolCall>(&resp->message.content[0].value);
                    auto const* i1 = std::get_if<Text>(&resp->message.content[1].value);
                    auto const* i2 = std::get_if<ToolCall>(&resp->message.content[2].value);
                    auto const* i3 = std::get_if<Text>(&resp->message.content[3].value);
                    check(i0 && i0->call_id == "call-1",
                          "E1-R11: item 0 is the FIRST tool_use, matching wire position 0");
                    check(i1 && i1->text == "and also,", "E1-R11: item 1 is text, matching wire position 1");
                    check(i2 && i2->call_id == "call-2",
                          "E1-R11: item 2 is the SECOND tool_use, matching wire position 2");
                    check(i3 && i3->text == "here's the forecast.",
                          "E1-R11: item 3 is text, matching wire position 3 -- the exact wire ordering is "
                          "preserved end to end, including two tool_use blocks separated by text");
                }
            }
        }
    }

    // ---- E5: full OutputSchema<T> pipeline -- a REAL compiled schema, NO additionalProperties forced ---
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "what's the forecast?"));
        req.output_schema_json = schema::json_schema_of<WeatherReport>();
        ChatClientCapabilities caps;
        caps.max_output_tokens = 2048;

        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false);
        check(body.has_value(), "E5-R2: a request carrying a REAL compiled OutputSchema<T> assembles without error");
        if (body) {
            auto const* oc = body->find("output_config");
            auto const* format = oc ? oc->find("format") : nullptr;
            auto const* schema_v = format ? format->find("schema") : nullptr;
            check(schema_v && schema_v->is_object(), "E5-R2: output_config.format.schema present");
            if (schema_v) {
                check(schema_v->find("additionalProperties") == nullptr,
                      "E5-R2: unlike Phase D's OpenAI backend (D4-R3), a REAL compiled schema is NOT "
                      "forced to carry additionalProperties:false here -- the same documented per-backend "
                      "decision E5-R1 already proves against a hand-typed schema, now proven against the "
                      "actual compiled-schema pipeline instead");
                auto const* props = schema_v->find("properties");
                auto const* forecast_prop = props ? props->find("forecast") : nullptr;
                check(forecast_prop && forecast_prop->find("items") != nullptr,
                      "E5-R2: the compiled schema's nested array-of-objects field ('forecast': "
                      "ForecastDay[]) survives translation unchanged");
            }
        }
    }

    // ---- F1: app-attribution headers (research doc item 1) -- stamped ONLY when non-empty -----------
    {
        auto req = build_http_request("/v1/messages", "sk-ant-x", "2023-06-01", "{}");
        check(req.headers.size() == 3,
              "F1-R1: with http_referer/x_title both left at their default-empty, exactly the original "
              "three headers are present -- no unrequested HTTP-Referer/X-Title header ever appears");
        bool has_referer = false, has_title = false;
        for (auto const& [k, v] : req.headers) {
            if (k == "HTTP-Referer") has_referer = true;
            if (k == "X-Title") has_title = true;
        }
        check(!has_referer && !has_title, "F1-R1: neither attribution header name appears at all");
    }
    {
        auto req = build_http_request("/v1/messages", "sk-ant-x", "2023-06-01", "{}", "https://my.app",
                                       "My Agent");
        check(req.headers.size() == 5,
              "F1-R2: with both http_referer and x_title set, exactly two headers are added on top of "
              "the original three");
        std::string referer_value, title_value;
        for (auto const& [k, v] : req.headers) {
            if (k == "HTTP-Referer") referer_value = v;
            if (k == "X-Title") title_value = v;
        }
        check(referer_value == "https://my.app", "F1-R2: HTTP-Referer carries the exact value given");
        check(title_value == "My Agent", "F1-R2: X-Title carries the exact value given");
    }
    {
        // Only one of the two set -- proves they're independent, not an all-or-nothing pair.
        auto req = build_http_request("/v1/messages", "sk-ant-x", "2023-06-01", "{}", "https://only-referer.app",
                                       "");
        check(req.headers.size() == 4,
              "F1-R3: http_referer alone (x_title left empty) adds exactly one header, not two -- the "
              "two attribution headers are independently gated, not an all-or-nothing pair");
    }

    // ---- F2: abuse-tracking id -- metadata.user_id (research doc item 2) ----------------------------
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps;
        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false, /*end_user_id=*/"");
        check(body.has_value(), "F2-R1: translates without error when end_user_id is left empty");
        if (body) {
            check(body->find("metadata") == nullptr,
                  "F2-R1: with end_user_id left at its default-empty, no 'metadata' key is added at all -- "
                  "Anthropic's Metadata type has ONLY user_id, so an empty id means omit the whole object, "
                  "not send an empty one");
        }
    }
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps;
        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false,
                                        /*end_user_id=*/"user-abc-123");
        check(body.has_value(), "F2-R2: translates without error when end_user_id is set");
        if (body) {
            auto const* metadata = body->find("metadata");
            check(metadata != nullptr, "F2-R2: 'metadata' appears when end_user_id is non-empty");
            if (metadata) {
                auto const* user_id = metadata->find("user_id");
                check(user_id && user_id->is_string() && user_id->as_string() == "user-abc-123",
                      "F2-R2: metadata.user_id carries the exact end_user_id value -- the confirmed exact "
                      "wire shape (Metadata.cs has ONLY this one field, no general free-form map like "
                      "OpenAI's)");
                check(metadata->find("no_such_field") == nullptr,
                      "F2-R2: metadata carries nothing beyond user_id -- no invented general metadata map");
            }
        }
    }

    // ---- F3: cache TTL as a constructor option (research doc item 7) --------------------------------
    {
        check(is_valid_cache_ttl(""), "F3-R1: empty string is valid (server default, omit ttl entirely)");
        check(is_valid_cache_ttl("5m"), "F3-R1: \"5m\" is a valid TTL value");
        check(is_valid_cache_ttl("1h"), "F3-R1: \"1h\" is a valid TTL value");
        check(!is_valid_cache_ttl("10m"), "F3-R1: an unsupported duration string is rejected");
        check(!is_valid_cache_ttl("3600s"), "F3-R1: a value in the wrong unit is rejected, not coerced");
        check(!is_valid_cache_ttl("EPHEMERAL"), "F3-R1: garbage input is rejected, not silently accepted");
    }
    {
        // Empty cache_ttl -- the system-block cache_control carries no "ttl" sibling at all (server's own
        // 5-minute default applies).
        ChatRequest req;
        req.messages.push_back(text_message(role::system, "be helpful"));
        req.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps;
        caps.prompt_caching = true;
        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false, /*end_user_id=*/"",
                                        /*cache_ttl=*/"");
        check(body.has_value(), "F3-R2: translates without error with cache_ttl left empty");
        if (body) {
            auto const* system = body->find("system");
            auto const* cc = (system && !system->as_array().empty())
                                  ? system->as_array().front().find("cache_control")
                                  : nullptr;
            check(cc != nullptr, "F3-R2: the system block still carries a cache_control breakpoint");
            check(cc && cc->find("ttl") == nullptr,
                  "F3-R2: with cache_ttl left empty, no 'ttl' key is added to cache_control at all -- the "
                  "server's own 5-minute default applies, exactly matching this backend's pre-item-7 "
                  "behavior for anyone who never opts in");
        }
    }
    {
        // Non-empty cache_ttl -- applied to BOTH the system block and the last-tool cache_control.
        ChatRequest req;
        req.messages.push_back(text_message(role::system, "be helpful"));
        req.messages.push_back(text_message(role::user, "hi"));
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "weather";
        t.args_schema_json = R"({"type":"object","properties":{}})";
        req.tools = {t};
        ChatClientCapabilities caps;
        caps.prompt_caching = true;
        auto body = build_request_body(req, "claude-sonnet-5", caps, /*stream=*/false, /*end_user_id=*/"",
                                        /*cache_ttl=*/"1h");
        check(body.has_value(), "F3-R3: translates without error with cache_ttl=\"1h\"");
        if (body) {
            auto const* system = body->find("system");
            auto const* sys_cc = (system && !system->as_array().empty())
                                      ? system->as_array().front().find("cache_control")
                                      : nullptr;
            auto const* sys_ttl = sys_cc ? sys_cc->find("ttl") : nullptr;
            check(sys_ttl && sys_ttl->is_string() && sys_ttl->as_string() == "1h",
                  "F3-R3: the system block's cache_control carries ttl=\"1h\" when requested");

            auto const* tools = body->find("tools");
            auto const* tool_cc =
                (tools && !tools->as_array().empty()) ? tools->as_array().front().find("cache_control") : nullptr;
            auto const* tool_ttl = tool_cc ? tool_cc->find("ttl") : nullptr;
            check(tool_ttl && tool_ttl->is_string() && tool_ttl->as_string() == "1h",
                  "F3-R3: the (only, and therefore last) tool's cache_control ALSO carries ttl=\"1h\" -- "
                  "the same cache_ttl option applies uniformly to every cache_control this backend builds, "
                  "not just the system block");
        }
    }
    {
        // translate_tool directly, with a non-default TTL, mirroring the earlier E4-R1 single-tool case.
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "weather";
        t.args_schema_json = R"({"type":"object","properties":{}})";
        auto wire = translate_tool(t, /*cache_this_one=*/true, /*cache_ttl=*/"5m");
        check(wire.has_value(), "F3-R4: translate_tool itself accepts the cache_ttl parameter directly");
        if (wire) {
            auto const* cc = wire->find("cache_control");
            auto const* ttl = cc ? cc->find("ttl") : nullptr;
            check(ttl && ttl->as_string() == "5m", "F3-R4: translate_tool's own cache_control carries ttl=\"5m\"");
        }
    }

    // ---- F4: the 4-cache_control-blocks-combined hard invariant (research doc item 6) ---------------
    {
        // A hand-built body with FIVE cache_control occurrences -- 2 in a fake "system" array, 2 in a
        // fake "tools" array, 1 inside a fake "messages" content block -- exactly the shape
        // count_cache_control_blocks is documented to walk, but NOT reachable through the public
        // request-building API as currently designed (it places at most 2).
        auto fake_cc = [] {
            return json::Value::make_object(
                {{"type", json::Value::make_string("ephemeral")}});
        };
        json::Value system_arr = json::Value::make_array(
            {json::Value::make_object({{"type", json::Value::make_string("text")},
                                        {"text", json::Value::make_string("a")},
                                        {"cache_control", fake_cc()}}),
             json::Value::make_object({{"type", json::Value::make_string("text")},
                                        {"text", json::Value::make_string("b")},
                                        {"cache_control", fake_cc()}})});
        json::Value tools_arr = json::Value::make_array(
            {json::Value::make_object({{"name", json::Value::make_string("t1")}, {"cache_control", fake_cc()}}),
             json::Value::make_object({{"name", json::Value::make_string("t2")}, {"cache_control", fake_cc()}})});
        json::Value message_content = json::Value::make_array(
            {json::Value::make_object({{"type", json::Value::make_string("text")},
                                        {"text", json::Value::make_string("c")},
                                        {"cache_control", fake_cc()}})});
        json::Value messages_arr = json::Value::make_array(
            {json::Value::make_object({{"role", json::Value::make_string("user")}, {"content", message_content}})});
        json::Value fake_body = json::Value::make_object({{"system", system_arr},
                                                            {"tools", tools_arr},
                                                            {"messages", messages_arr}});
        check(count_cache_control_blocks(fake_body) == 5,
              "F4-R1: count_cache_control_blocks correctly counts FIVE cache_control occurrences spread "
              "across system(2)+tools(2)+messages(1) in a hand-built body -- a count the public "
              "build_request_body API can never itself produce today, proving the counting function "
              "itself is correct independent of what currently reaches it");
    }
    {
        // Realistic under-the-cap cases, via the real code path.
        ChatRequest req0;
        req0.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps0;  // prompt_caching = false, no tools
        auto body0 = build_request_body(req0, "claude-sonnet-5", caps0, /*stream=*/false);
        check(body0.has_value() && count_cache_control_blocks(*body0) == 0,
              "F4-R2: a request with prompt_caching off and no tools produces ZERO cache_control blocks "
              "-- well under the 4-block cap, and build_request_body still succeeds");

        ChatRequest req1;
        req1.messages.push_back(text_message(role::system, "be helpful"));
        req1.messages.push_back(text_message(role::user, "hi"));
        ChatClientCapabilities caps1;
        caps1.prompt_caching = true;  // no tools -- only the system block gets a breakpoint
        auto body1 = build_request_body(req1, "claude-sonnet-5", caps1, /*stream=*/false);
        check(body1.has_value() && count_cache_control_blocks(*body1) == 1,
              "F4-R2: system-only prompt_caching produces exactly ONE cache_control block -- still under "
              "the cap, and build_request_body still succeeds normally (never rejected)");

        ChatRequest req2;
        req2.messages.push_back(text_message(role::system, "be helpful"));
        req2.messages.push_back(text_message(role::user, "hi"));
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "weather";
        t.args_schema_json = R"({"type":"object","properties":{}})";
        req2.tools = {t};
        ChatClientCapabilities caps2;
        caps2.prompt_caching = true;
        auto body2 = build_request_body(req2, "claude-sonnet-5", caps2, /*stream=*/false);
        check(body2.has_value() && count_cache_control_blocks(*body2) == 2,
              "F4-R2: system + one tool with prompt_caching produces exactly TWO cache_control blocks -- "
              "today's real maximum, still under the 4-block cap, build_request_body succeeds normally "
              "rather than ever hitting the defensive rejection path");
    }

    // ---- F5: model + cache_creation_input_tokens response parsing (research doc items 4/5) ----------
    {
        auto parsed = json::parse(R"({
            "id":"msg_1","type":"message","role":"assistant","model":"claude-sonnet-5-20260101",
            "content":[{"type":"text","text":"hi"}],
            "usage":{"input_tokens":10,"output_tokens":5,"cache_creation_input_tokens":37}
        })");
        check(parsed.has_value(), "F5-R1: literal wire JSON with a model field and cache_creation_input_tokens parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value(), "F5-R1: parses into a ChatResponse");
            if (resp) {
                check(resp->model == "claude-sonnet-5-20260101",
                      "F5-R1: ChatResponse.model is populated from the response's own top-level 'model' "
                      "field -- the model that ACTUALLY answered, not merely what was requested");
                check(resp->usage.cache_write_tokens == 37,
                      "F5-R1: usage.cache_creation_input_tokens maps to Usage::cache_write_tokens -- the "
                      "symmetric counterpart to cache_read_input_tokens/cached_input_tokens, previously a "
                      "named, unmapped gap");
            }
        }
    }
    {
        // Both absent -- default values, never fabricated.
        auto parsed = json::parse(R"({
            "type":"message","role":"assistant",
            "content":[{"type":"text","text":"hi"}],
            "usage":{"input_tokens":1,"output_tokens":1}
        })");
        check(parsed.has_value(), "F5-R2: literal wire JSON with NO model field and NO cache_creation_input_tokens parses");
        if (parsed) {
            auto resp = parse_message_response(*parsed);
            check(resp.has_value(), "F5-R2: parses into a ChatResponse");
            if (resp) {
                check(resp->model.empty(),
                      "F5-R2: ChatResponse.model stays empty when the response doesn't report one -- "
                      "never fabricated from the request's own requested model name");
                check(resp->usage.cache_write_tokens == 0,
                      "F5-R2: usage.cache_write_tokens stays at its default (0) when "
                      "cache_creation_input_tokens is absent from the wire response");
            }
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_anthropic_chat_client_translation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_anthropic_chat_client_translation: %d FAILURE(S)\n", g_failures);
    return 1;
}
