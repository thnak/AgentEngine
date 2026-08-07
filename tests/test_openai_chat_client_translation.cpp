// Milestone 5 Phase D (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// protocol/openai/chat_client.hpp's translation logic (D1 request/response, D3 tool-schema shaping,
// D4 structured-output shaping, D2's SSE/chunked-transfer parsing) against LITERAL OpenAI wire-format
// JSON -- no network, no live server, so this runs unconditionally fast and offline. Wire-format field
// names/shapes were sourced directly from the official OpenAI .NET SDK's generated serialization code
// (see chat_client.hpp's own top comment) -- these fixtures mirror that shape, not this project's own
// tests/fixtures/chat_client/*.json (a DIFFERENT, internal RecordedChatClient schema -- see that
// directory's README -- used here only as scenario inspiration: this file's tool-call and
// parallel-tool-call scenarios mirror simple_reply.json/tool_call.json/parallel_tool_calls.json's own
// shapes, translated into the real vendor wire format).
//
// test_openai_chat_client_live.cpp (a separate file, same phase) proves the REST of the path -- the
// actual HTTPS exchange + secret resolution -- against a local TLS test server; this file is pure
// parsing/serialization logic, deliberately with no server involved.

#include <cstdio>
#include <string>

#include "agentengine/protocol/openai/chat_client.hpp"

using namespace agentengine;
using namespace agentengine::openai::detail;

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
    // ---- D1: outbound message translation -------------------------------------------------------
    {
        Message m = text_message(role::user, "hello there");
        json::Value wire = translate_message(m);
        check(wire.is_object(), "D1-R1: a translated message is a JSON object");
        auto const* r = wire.find("role");
        check(r && r->is_string() && r->as_string() == "user", "D1-R1: role field is the literal 'user'");
        auto const* c = wire.find("content");
        check(c && c->is_string() && c->as_string() == "hello there",
              "D1-R1: a single-text-part message serializes content as a plain string");
    }

    // ---- D1: outbound assistant message with a tool call (content becomes null) -------------------
    {
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = ToolCall{"call-1", "get_weather", "{\"location\":\"Seattle\"}"};
        m.content.push_back(std::move(item));

        json::Value wire = translate_message(m);
        auto const* content = wire.find("content");
        check(content && content->is_null(),
              "D1-R2: an assistant message with only tool_calls and no text carries content:null "
              "(mirrors tool_call.json's scenario -- a pure tool-call turn)");
        auto const* tool_calls = wire.find("tool_calls");
        check(tool_calls && tool_calls->is_array() && tool_calls->as_array().size() == 1,
              "D1-R2: exactly one tool_calls entry");
        if (tool_calls && !tool_calls->as_array().empty()) {
            json::Value const& tc = tool_calls->as_array().front();
            auto const* id = tc.find("id");
            auto const* type = tc.find("type");
            auto const* fn = tc.find("function");
            check(id && id->as_string() == "call-1", "D1-R2: tool call id round-trips");
            check(type && type->as_string() == "function", "D1-R2: type is the literal 'function'");
            check(fn != nullptr, "D1-R2: function object present");
            if (fn) {
                auto const* name = fn->find("name");
                auto const* args = fn->find("arguments");
                check(name && name->as_string() == "get_weather", "D1-R2: function.name round-trips");
                check(args && args->as_string() == "{\"location\":\"Seattle\"}",
                      "D1-R2: function.arguments is the RAW JSON STRING, not a nested object "
                      "(confirmed against the SDK: WriteStringValue, not WriteRawValue, for this field)");
            }
        }
    }

    // ---- D1: outbound tool-role reply (mirrors a ToolResult being sent back) ----------------------
    {
        Message m;
        m.role = role::tool;
        ContentItem item;
        item.origin = content_origin::tool;
        ToolResult tr;
        tr.call_id = "call-1";
        ContentItem inner;
        inner.value = Data{"{\"temp_f\":72}", std::nullopt};
        tr.content.push_back(std::move(inner));
        item.value = std::move(tr);
        m.content.push_back(std::move(item));

        json::Value wire = translate_message(m);
        auto const* role_field = wire.find("role");
        auto const* tool_call_id = wire.find("tool_call_id");
        auto const* content = wire.find("content");
        check(role_field && role_field->as_string() == "tool", "D1-R3: role is the literal 'tool'");
        check(tool_call_id && tool_call_id->as_string() == "call-1",
              "D1-R3: tool_call_id sourced from the ToolResult's own call_id");
        check(content && content->is_string() && content->as_string() == "{\"temp_f\":72}",
              "D1-R3: the tool reply's nested Data content flattens into the wire 'content' string");
    }

    // ---- D3: tool-schema shaping -------------------------------------------------------------------
    {
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "Get the weather for a location";
        t.args_schema_json = R"({"type":"object","properties":{"location":{"type":"string"}},"required":["location"]})";

        auto wire = translate_tool(t);
        check(wire.has_value(), "D3-R1: a well-formed args schema translates without error");
        if (wire) {
            auto const* type = wire->find("type");
            auto const* fn = wire->find("function");
            check(type && type->as_string() == "function", "D3-R1: outer type is the literal 'function'");
            check(fn != nullptr, "D3-R1: function object present");
            if (fn) {
                auto const* name = fn->find("name");
                auto const* desc = fn->find("description");
                auto const* params = fn->find("parameters");
                check(name && name->as_string() == "get_weather", "D3-R1: function.name round-trips");
                check(desc && desc->as_string() == "Get the weather for a location",
                      "D3-R1: function.description round-trips");
                check(params && params->is_object(), "D3-R1: parameters is the parsed JSON Schema object");
                check(params && params->find("properties") != nullptr,
                      "D3-R1: the schema's own 'properties' survives translation unchanged");
            }
        }
    }
    {
        ToolDescriptor t;
        t.name = "broken";
        t.args_schema_json = "{not valid json";
        auto wire = translate_tool(t);
        check(!wire.has_value(), "D3-R2: a malformed args schema fails translation rather than silently "
                                  "sending garbage to the provider");
    }

    // ---- D4: structured-output shaping (additionalProperties:false injection) ---------------------
    {
        std::string schema_without_ap =
            R"({"type":"object","properties":{"answer":{"type":"string"}},"required":["answer"]})";
        auto wire = translate_output_schema(schema_without_ap);
        check(wire.has_value(), "D4-R1: a well-formed output schema translates without error");
        if (wire) {
            auto const* type = wire->find("type");
            check(type && type->as_string() == "json_schema", "D4-R1: type is the literal 'json_schema'");
            auto const* js = wire->find("json_schema");
            check(js != nullptr, "D4-R1: json_schema wrapper object present");
            if (js) {
                auto const* strict = js->find("strict");
                auto const* schema = js->find("schema");
                check(strict && strict->is_bool() && strict->as_bool(), "D4-R1: strict:true");
                check(schema && schema->is_object(), "D4-R1: schema object present");
                if (schema) {
                    auto const* ap = schema->find("additionalProperties");
                    check(ap && ap->is_bool() && !ap->as_bool(),
                          "D4-R1: additionalProperties:false was INJECTED (the input schema had none -- "
                          "json_schema_of<T>'s own ObjectBuilder never emits this key, confirmed in "
                          "core/json_schema.hpp)");
                    auto const* props = schema->find("properties");
                    check(props != nullptr, "D4-R1: the original schema's properties survive unchanged");
                }
            }
        }
    }
    {
        // A schema that ALREADY declares additionalProperties (any value) is left alone -- this
        // function adds the key only when absent, it never overrides an explicit author choice.
        std::string schema_with_ap =
            R"({"type":"object","properties":{},"additionalProperties":true})";
        auto wire = translate_output_schema(schema_with_ap);
        check(wire.has_value(), "D4-R2: translates without error");
        if (wire) {
            auto const* schema = wire->find("json_schema")->find("schema");
            auto const* ap = schema->find("additionalProperties");
            check(ap && ap->is_bool() && ap->as_bool(),
                  "D4-R2: an explicit additionalProperties:true is preserved, not silently flipped");
        }
    }

    // ---- D1: full request body assembly (model/messages/tools/response_format/stream) -------------
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "what's the weather in Seattle?"));
        ToolDescriptor t;
        t.name = "get_weather";
        t.description = "weather lookup";
        t.args_schema_json = R"({"type":"object","properties":{}})";
        req.tools.push_back(t);

        auto body = build_request_body(req, "gpt-5", /*stream=*/true);
        check(body.has_value(), "D1-R4: a full request body assembles without error");
        if (body) {
            check(body->find("model") && body->find("model")->as_string() == "gpt-5",
                  "D1-R4: model round-trips");
            check(body->find("stream") && body->find("stream")->as_bool(), "D1-R4: stream:true present");
            auto const* messages = body->find("messages");
            check(messages && messages->is_array() && messages->as_array().size() == 1,
                  "D1-R4: one translated message");
            auto const* tools = body->find("tools");
            check(tools && tools->is_array() && tools->as_array().size() == 1, "D1-R4: one translated tool");
        }
    }

    // ---- D1: response parsing -- plain text (mirrors simple_reply.json's scenario) -----------------
    {
        auto parsed = json::parse(R"({
            "id":"chatcmpl-1","object":"chat.completion","choices":[
                {"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hello from a fixture"}}
            ],
            "usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}
        })");
        check(parsed.has_value(), "D1-R5: literal wire JSON parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D1-R5: a plain-text response parses into a ChatResponse");
            if (resp) {
                check(resp->message.content.size() == 1, "D1-R5: exactly one content item");
                if (!resp->message.content.empty()) {
                    auto const* text = std::get_if<Text>(&resp->message.content.front().value);
                    check(text && text->text == "hello from a fixture", "D1-R5: text round-trips");
                }
                check(resp->usage.input_tokens == 10 && resp->usage.output_tokens == 5,
                      "D1-R5: usage.prompt_tokens/completion_tokens map to input_tokens/output_tokens");
            }
        }
    }

    // ---- D1: response parsing -- parallel tool calls (mirrors parallel_tool_calls.json) ------------
    {
        auto parsed = json::parse(R"({
            "choices":[{"index":0,"finish_reason":"tool_calls","message":{"role":"assistant","content":null,
                "tool_calls":[
                    {"id":"call-1","type":"function","function":{"name":"spotify.play","arguments":"{\"artist\":\"Taylor Swift\",\"duration\":20}"}},
                    {"id":"call-2","type":"function","function":{"name":"spotify.play","arguments":"{\"artist\":\"Maroon 5\",\"duration\":15}"}}
                ]}}],
            "usage":{"prompt_tokens":35,"completion_tokens":14}
        })");
        check(parsed.has_value(), "D1-R6: literal parallel-tool-call wire JSON parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D1-R6: a parallel-tool-call response parses");
            if (resp) {
                check(resp->message.content.size() == 2, "D1-R6: two ToolCall content items, one per call");
                if (resp->message.content.size() == 2) {
                    auto const* tc0 = std::get_if<ToolCall>(&resp->message.content[0].value);
                    auto const* tc1 = std::get_if<ToolCall>(&resp->message.content[1].value);
                    check(tc0 && tc0->call_id == "call-1" && tc0->tool_name == "spotify.play",
                          "D1-R6: first call round-trips in order");
                    check(tc1 && tc1->call_id == "call-2" && tc1->tool_name == "spotify.play",
                          "D1-R6: second call round-trips in order (FIFO, not reordered)");
                }
            }
        }
    }

    // ---- D1: response parsing -- a provider error envelope maps to a real failure ------------------
    {
        auto parsed = json::parse(R"({"error":{"message":"invalid api key","type":"invalid_request_error"}})");
        check(parsed.has_value(), "D1-R7: literal error wire JSON parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(!resp.has_value(), "D1-R7: an error envelope fails translation, never a fabricated success");
            if (!resp) {
                check(resp.error().message.find("invalid api key") != std::string::npos,
                      "D1-R7: the provider's own error message is preserved, not swallowed");
            }
        }
    }

    // ---- D2: Transfer-Encoding: chunked decoding -----------------------------------------------------
    {
        std::string chunked = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        auto decoded = decode_chunked_body(chunked);
        check(decoded.has_value() && *decoded == "hello world", "D2-R1: chunked transfer-encoding decodes correctly");
    }
    {
        std::string malformed = "not-hex\r\ndata\r\n0\r\n\r\n";
        auto decoded = decode_chunked_body(malformed);
        check(!decoded.has_value(), "D2-R2: a malformed chunk size fails cleanly, not a garbled decode");
    }

    // ---- D2: SSE event splitting --------------------------------------------------------------------
    {
        std::string sse = "data: {\"a\":1}\n\ndata: {\"a\":2}\n\ndata: [DONE]\n\n";
        auto events = split_sse_data_events(sse);
        check(events.size() == 3, "D2-R3: three data events extracted, in order");
        if (events.size() == 3) {
            check(events[0] == "{\"a\":1}", "D2-R3: first event payload exact");
            check(events[1] == "{\"a\":2}", "D2-R3: second event payload exact");
            check(events[2] == "[DONE]", "D2-R3: terminal sentinel extracted verbatim");
        }
    }

    // ---- D2: full streaming-chunk-to-ChatResponseUpdate translation ---------------------------------
    {
        // Text deltas arrive per-chunk; a tool call's argument fragments arrive incrementally across
        // TWO chunks (index-correlated) and must be reassembled into one complete ToolCall.
        std::string sse =
            "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Let \"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"me check.\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-1\","
            "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"\\\"Seattle\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
            "data: [DONE]\n\n";
        auto updates = parse_streaming_response_into_updates(sse, /*is_chunked=*/false);
        check(updates.has_value(), "D2-R4: a full SSE stream parses into an update list");
        if (updates) {
            check(updates->size() == 3,
                  "D2-R4: two text deltas (per-chunk fidelity) + one assembled tool call = 3 updates");
            if (updates->size() == 3) {
                auto const* t0 = std::get_if<Text>(&(*updates)[0].delta.value);
                auto const* t1 = std::get_if<Text>(&(*updates)[1].delta.value);
                check(t0 && t0->text == "Let ", "D2-R4: first text delta exact, own chunk boundary preserved");
                check(t1 && t1->text == "me check.", "D2-R4: second text delta exact, own chunk boundary preserved");
                check(!(*updates)[0].is_final && !(*updates)[1].is_final,
                      "D2-R4: only the LAST update is marked is_final");
                auto const* tc = std::get_if<ToolCall>(&(*updates)[2].delta.value);
                check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather",
                      "D2-R4: the tool call's id/name survive fragment reassembly");
                check(tc && tc->arguments_json == R"({"location":"Seattle"})",
                      "D2-R4: argument fragments from TWO separate chunks concatenate into valid JSON");
                check((*updates)[2].is_final, "D2-R4: the assembled tool call is the final update");
            }
        }
    }
    {
        // An empty stream (no delta content at all) parses to zero updates, not a fabricated one.
        std::string sse = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        auto updates = parse_streaming_response_into_updates(sse, /*is_chunked=*/false);
        check(updates.has_value() && updates->empty(), "D2-R5: an empty delta stream yields zero updates");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_chat_client_translation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_chat_client_translation: %d FAILURE(S)\n", g_failures);
    return 1;
}
