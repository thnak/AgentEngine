// Milestone 5 Phase E (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// protocol/anthropic/chat_client.hpp's translation logic against LITERAL Anthropic wire-format JSON --
// no network, no live server. Wire-format field names were sourced directly from the official
// Anthropic C# SDK's generated model code (see chat_client.hpp's own top comment). Mirrors Phase D's
// test_openai_chat_client_translation.cpp in structure and coverage intent.

#include <cstdio>
#include <string>

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
