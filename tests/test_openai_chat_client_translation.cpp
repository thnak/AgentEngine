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
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
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

// New helpers (deeper tool-call coverage, below): a lone assistant turn carrying a ToolCall, and a
// lone tool-role turn carrying a ToolResult reply -- factored out so the multi-turn round-trip test
// reads as a sequence of turns rather than four inline ContentItem constructions.
[[nodiscard]] Message tool_call_message(std::string call_id, std::string tool_name, std::string args_json) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] Message tool_result_message(std::string call_id, std::string reply_json) {
    Message m;
    m.role = role::tool;
    ContentItem item;
    item.origin = content_origin::tool;
    ToolResult tr;
    tr.call_id = std::move(call_id);
    ContentItem inner;
    inner.value = Data{std::move(reply_json), std::nullopt};
    tr.content.push_back(std::move(inner));
    item.value = std::move(tr);
    m.content.push_back(std::move(item));
    return m;
}

// A nested/complex OutputSchema<T> type for the D4-R3 full-pipeline test below -- a REAL
// AE_JSON_SCHEMA(...)-compiled schema (a nested array-of-objects field plus a std::optional field),
// mirroring test_json_schema_codec.cpp's own Hit/SearchReply nesting pattern, reused here via
// schema::json_schema_of<T>() rather than a hand-typed literal -- 003 §4's actual OutputSchema<T> path.
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
            // unified-streaming-design-draft.md §1 (Piece B): two text deltas + two companion
            // tool_call_argument_chunk updates (one per chunk carrying a non-empty arguments
            // fragment) + one assembled tool call = 5 updates. The two chunk updates are new;
            // everything else is unchanged from before this piece.
            check(updates->size() == 5,
                  "D2-R4: two text deltas + two argument-chunk updates + one assembled tool call = "
                  "5 updates");
            if (updates->size() == 5) {
                auto const* t0 = std::get_if<Text>(&(*updates)[0].delta.value);
                check(t0 && t0->text == "Let ", "D2-R4: first text delta exact, own chunk boundary preserved");
                check(!(*updates)[0].is_final, "D2-R4: the first text delta is not final");

                check((*updates)[1].tool_call_argument_chunk.has_value() &&
                          (*updates)[2].tool_call_argument_chunk.has_value(),
                      "D2-R4: the two tool-call argument fragments each get their own companion update");
                if ((*updates)[1].tool_call_argument_chunk.has_value()) {
                    auto const& c1 = *(*updates)[1].tool_call_argument_chunk;
                    check(c1.call_id == "call-1" && c1.tool_name == "get_weather" &&
                              c1.arguments_fragment == R"({"location":)" && !c1.is_final,
                          "D2-R4: the first argument fragment carries the real call_id/tool_name and "
                          "its own raw bytes, not accumulated -- OpenAI has no completion boundary, "
                          "so is_final stays false");
                }
                if ((*updates)[2].tool_call_argument_chunk.has_value()) {
                    auto const& c2 = *(*updates)[2].tool_call_argument_chunk;
                    check(c2.call_id == "call-1" && c2.arguments_fragment == R"("Seattle"})" && !c2.is_final,
                          "D2-R4: the second argument fragment carries its own raw bytes, not the "
                          "accumulated total");
                }

                auto const* t1 = std::get_if<Text>(&(*updates)[3].delta.value);
                check(t1 && t1->text == "me check.", "D2-R4: second text delta exact, own chunk boundary preserved");
                check(!(*updates)[3].is_final, "D2-R4: the second text delta is not final either");

                auto const* tc = std::get_if<ToolCall>(&(*updates)[4].delta.value);
                check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather",
                      "D2-R4: the tool call's id/name survive fragment reassembly");
                check(tc && tc->arguments_json == R"({"location":"Seattle"})",
                      "D2-R4: argument fragments from TWO separate chunks concatenate into valid JSON "
                      "-- the one real json::parse, unaffected by the new display-only fragments");
                check((*updates)[4].is_final, "D2-R4: the assembled tool call is the final update");
            }
        }
    }
    {
        // An empty stream (no delta content at all) parses to zero updates, not a fabricated one.
        std::string sse = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        auto updates = parse_streaming_response_into_updates(sse, /*is_chunked=*/false);
        check(updates.has_value() && updates->empty(), "D2-R5: an empty delta stream yields zero updates");
    }

    // ---- D1: multi-turn conversation round trip (user -> assistant tool_call -> tool reply -> user) --
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "what's the weather in Seattle?"));
        req.messages.push_back(tool_call_message("call-1", "get_weather", R"({"location":"Seattle"})"));
        req.messages.push_back(tool_result_message("call-1", R"({"temp_f":58,"condition":"cloudy"})"));
        req.messages.push_back(text_message(role::user, "should I bring an umbrella?"));

        auto body = build_request_body(req, "gpt-5", /*stream=*/false);
        check(body.has_value(), "D1-R8: a 4-turn conversation with a tool round-trip assembles without error");
        if (body) {
            auto const* messages = body->find("messages");
            check(messages && messages->is_array() && messages->as_array().size() == 4,
                  "D1-R8: all four turns are present, one wire message each, in order");
            if (messages && messages->as_array().size() == 4) {
                auto const& arr = messages->as_array();
                check(arr[0].find("role")->as_string() == "user", "D1-R8: turn 1 role is 'user'");
                check(arr[1].find("role")->as_string() == "assistant" && arr[1].find("tool_calls") != nullptr,
                      "D1-R8: turn 2 role is 'assistant' and carries the tool_calls array");
                check(arr[2].find("role")->as_string() == "tool" &&
                          arr[2].find("tool_call_id")->as_string() == "call-1",
                      "D1-R8: turn 3 role is 'tool', tool_call_id correlates back to turn 2's call id");
                check(arr[2].find("content")->as_string() == R"({"temp_f":58,"condition":"cloudy"})",
                      "D1-R8: turn 3's tool reply content is the exact JSON reply text");
                check(arr[3].find("role")->as_string() == "user" &&
                          arr[3].find("content")->as_string() == "should I bring an umbrella?",
                      "D1-R8: turn 4 (the follow-up) round-trips as an ordinary user message");
            }
        }
    }

    // ---- D1: a resolved PARALLEL tool-call turn -- one AE Message carrying TWO ToolResult items -----
    // A real caller resolving two tool calls the model issued in ONE turn (parallel_tool_calls above,
    // D1-R6) most naturally builds ONE role::tool reply Message with two ToolResult content items --
    // the multi-ToolResult shape `translate_message` alone silently mishandles (it tracks only a
    // single `tool_call_id` variable, so the SECOND ToolResult processed overwrites the first: the
    // first call's own tool_call_id is dropped entirely from the wire request, and its text gets
    // merged under the SECOND call's id instead -- a real correlation bug a live provider would 400
    // on, "tool_call_id ... not found" for the never-emitted first one). `build_request_body` is what
    // callers actually use, so this proves the FULL fix (translate_message_to_wire), not just the
    // helper in isolation.
    {
        Message reply;
        reply.role = role::tool;
        ToolResult tr1;
        tr1.call_id = "call-1";
        ContentItem inner1;
        inner1.value = Data{R"({"artist":"Taylor Swift"})", std::nullopt};
        tr1.content.push_back(std::move(inner1));
        ContentItem item1;
        item1.origin = content_origin::tool;
        item1.value = std::move(tr1);
        reply.content.push_back(std::move(item1));

        ToolResult tr2;
        tr2.call_id = "call-2";
        ContentItem inner2;
        inner2.value = Data{R"({"artist":"Maroon 5"})", std::nullopt};
        tr2.content.push_back(std::move(inner2));
        ContentItem item2;
        item2.origin = content_origin::tool;
        item2.value = std::move(tr2);
        reply.content.push_back(std::move(item2));

        ChatRequest req;
        req.messages.push_back(text_message(role::user, "play a song from each"));
        req.messages.push_back(tool_call_message("call-1", "spotify.play", R"({"artist":"Taylor Swift"})"));
        // (in a real turn a second, sibling ToolCall would also be in that SAME assistant message --
        // orthogonal to what this block proves, so kept to one for a focused fixture)
        req.messages.push_back(std::move(reply));

        auto body = build_request_body(req, "gpt-5", /*stream=*/false);
        check(body.has_value(), "D1-R9: a request with a multi-ToolResult reply Message assembles");
        if (body) {
            auto const* messages = body->find("messages");
            check(messages && messages->is_array() && messages->as_array().size() == 4,
                  "D1-R9: THREE AE Messages produce FOUR wire messages -- the multi-ToolResult reply "
                  "Message split into two separate tool-role wire objects, one per call_id, not one");
            if (messages && messages->as_array().size() == 4) {
                auto const& arr = messages->as_array();
                check(arr[2].find("role")->as_string() == "tool" &&
                          arr[2].find("tool_call_id")->as_string() == "call-1" &&
                          arr[2].find("content")->as_string() == R"({"artist":"Taylor Swift"})",
                      "D1-R9: the FIRST ToolResult (call-1) is not dropped -- previously silently lost");
                check(arr[3].find("role")->as_string() == "tool" &&
                          arr[3].find("tool_call_id")->as_string() == "call-2" &&
                          arr[3].find("content")->as_string() == R"({"artist":"Maroon 5"})",
                      "D1-R9: the SECOND ToolResult (call-2) is correct and not merged with the first's "
                      "text under the wrong id");
            }
        }
    }

    // ---- D3: a complex/nested args_schema_json passes through translate_tool completely unchanged ----
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

        auto wire = translate_tool(t);
        check(wire.has_value(), "D3-R3: a deeply nested args schema (nested objects inside an array, "
                                 "nested inside another array) translates without error");
        if (wire) {
            auto const* fn = wire->find("function");
            auto const* params = fn ? fn->find("parameters") : nullptr;
            check(params != nullptr, "D3-R3: parameters object present");
            if (params) {
                // Round-trip both the ORIGINAL and the TRANSLATED schema through json::dump and compare
                // the dumped text -- proves nothing was dropped, reordered at the value level, or
                // coerced anywhere in the nested structure (json::Value preserves member insertion
                // order, confirmed by json_value.hpp's own top comment), not merely that some fields
                // happen to still be reachable.
                auto original_parsed = json::parse(t.args_schema_json);
                check(original_parsed.has_value(), "D3-R3: the original nested schema itself parses");
                if (original_parsed) {
                    check(json::dump(*params) == json::dump(*original_parsed),
                          "D3-R3: the nested schema (array-of-objects with a nested object field, plus a "
                          "second array-of-objects) survives translate_tool byte-for-byte, unchanged");
                }
                auto const* properties = params->find("properties");
                auto const* passengers = properties ? properties->find("passengers") : nullptr;
                check(passengers && passengers->find("items") != nullptr,
                      "D3-R3: the nested array's own 'items' sub-schema is reachable, not flattened away");
            }
        }
    }

    // ---- D1: a response with text AND multiple tool_calls together -- ContentItem order matches wire --
    {
        auto parsed = json::parse(R"({
            "choices":[{"index":0,"finish_reason":"tool_calls","message":{"role":"assistant",
                "content":"Let me look that up for you.",
                "tool_calls":[
                    {"id":"call-1","type":"function","function":{"name":"get_weather","arguments":"{\"city\":\"Seattle\"}"}},
                    {"id":"call-2","type":"function","function":{"name":"get_forecast","arguments":"{\"days\":3}"}}
                ]}}]
        })");
        check(parsed.has_value(), "D1-R9: literal text+tool_calls wire JSON parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D1-R9: a text+tool_calls response parses");
            if (resp) {
                check(resp->message.content.size() == 3,
                      "D1-R9: one Text item + two ToolCall items = three content items");
                if (resp->message.content.size() == 3) {
                    auto const* text = std::get_if<Text>(&resp->message.content[0].value);
                    auto const* tc0 = std::get_if<ToolCall>(&resp->message.content[1].value);
                    auto const* tc1 = std::get_if<ToolCall>(&resp->message.content[2].value);
                    check(text && text->text == "Let me look that up for you.",
                          "D1-R9: the text content item comes first, matching the wire's own field order "
                          "(OpenAI's message object always separates content/tool_calls as distinct "
                          "fields, never interleaved -- content is translated first by this backend)");
                    check(tc0 && tc0->call_id == "call-1" && tc0->tool_name == "get_weather",
                          "D1-R9: the first tool call follows, in its own wire array order");
                    check(tc1 && tc1->call_id == "call-2" && tc1->tool_name == "get_forecast",
                          "D1-R9: the second tool call follows, order preserved");
                }
            }
        }
    }

    // ---- D4: full OutputSchema<T> pipeline -- a REAL compiled schema (schema::json_schema_of<T>), not
    // a hand-typed literal, reaching build_request_body exactly the way 003 §4's OutputSchema<T> does --
    {
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "what's the forecast?"));
        req.output_schema_json = schema::json_schema_of<WeatherReport>();

        auto body = build_request_body(req, "gpt-5", /*stream=*/false);
        check(body.has_value(), "D4-R3: a request carrying a REAL compiled OutputSchema<T> (nested array "
                                 "of objects + an optional field) assembles without error");
        if (body) {
            auto const* rf = body->find("response_format");
            check(rf != nullptr, "D4-R3: response_format present");
            auto const* js = rf ? rf->find("json_schema") : nullptr;
            auto const* schema_v = js ? js->find("schema") : nullptr;
            check(schema_v && schema_v->is_object(), "D4-R3: schema object present");
            if (schema_v) {
                auto const* ap = schema_v->find("additionalProperties");
                check(ap && ap->is_bool() && !ap->as_bool(),
                      "D4-R3: additionalProperties:false is injected into a REAL compiled schema exactly "
                      "as it would be for a hand-typed one -- json_schema_of<T>'s own ObjectBuilder never "
                      "emits this key (core/json_schema.hpp), so the injection path is genuinely exercised");
                auto const* props = schema_v->find("properties");
                auto const* forecast_prop = props ? props->find("forecast") : nullptr;
                check(forecast_prop && forecast_prop->find("items") != nullptr,
                      "D4-R3: the compiled schema's own nested array-of-objects field ('forecast': "
                      "ForecastDay[]) survives translation intact -- proves the pipeline carries a REAL "
                      "compiled schema, not a hand-typed stand-in for it");
                auto const* required = schema_v->find("required");
                check(required && required->is_array(),
                      "D4-R3: the compiled schema's own 'required' array (location, forecast -- 'alert' "
                      "is std::optional and correctly excluded) survives translation");
            }
        }
    }

    // ---- D5 (2026-08-07 provider-metadata survey, item 2): build_request_body's "user"/"seed" ---------
    {
        // Neither end_user_id nor seed supplied: both stay omitted from the body (defaulted call, no
        // 4th/5th argument) -- matching every pre-existing 3-arg call site above, proving the new
        // parameters are genuinely optional and don't change old behavior.
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        auto body = build_request_body(req, "gpt-5", /*stream=*/false);
        check(body.has_value(), "D5-R1: build_request_body still succeeds with end_user_id/seed both "
                                 "defaulted (unset)");
        if (body) {
            check(body->find("user") == nullptr,
                  "D5-R1: no 'user' field at all when end_user_id is empty -- never an empty-string user");
            check(body->find("seed") == nullptr, "D5-R1: no 'seed' field at all when seed is nullopt");
        }
    }
    {
        // Both supplied: both land on the wire, seed as a JSON number (not a string).
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        auto body = build_request_body(req, "gpt-5", /*stream=*/false, "end-user-42",
                                        std::optional<std::int64_t>{123456789});
        check(body.has_value(), "D5-R2: build_request_body succeeds with both end_user_id and seed set");
        if (body) {
            auto const* user = body->find("user");
            check(user && user->is_string() && user->as_string() == "end-user-42",
                  "D5-R2: end_user_id maps to the wire 'user' field verbatim (OpenAI's abuse-tracking id)");
            auto const* seed = body->find("seed");
            check(seed && seed->is_number() && seed->as_number() == 123456789.0,
                  "D5-R2: seed maps to the wire 'seed' field as a JSON number, exact value preserved");
        }
    }
    {
        // Only end_user_id set, seed left unset: each new field is independently optional, not an
        // all-or-nothing pair.
        ChatRequest req;
        req.messages.push_back(text_message(role::user, "hi"));
        auto body = build_request_body(req, "gpt-5", /*stream=*/false, "solo-user");
        check(body.has_value(), "D5-R3: build_request_body succeeds with only end_user_id set");
        if (body) {
            check(body->find("user") != nullptr && body->find("user")->as_string() == "solo-user",
                  "D5-R3: 'user' present when only end_user_id is set");
            check(body->find("seed") == nullptr,
                  "D5-R3: 'seed' still absent -- setting end_user_id alone doesn't fabricate a seed");
        }
    }

    // ---- D5: build_http_request's HTTP-Referer/X-Title app-attribution headers ----------------------
    {
        // Neither header requested (defaulted call): neither header appears at all -- no empty-value
        // header is ever sent, matching the OpenRouter convention these headers exist for.
        auto req = build_http_request("/v1/chat/completions", "sk-test", "{}");
        bool has_referer = false, has_title = false;
        for (auto const& [k, v] : req.headers) {
            if (k == "HTTP-Referer") has_referer = true;
            if (k == "X-Title") has_title = true;
        }
        check(!has_referer && !has_title,
              "D5-R4: build_http_request omits HTTP-Referer/X-Title entirely when both are defaulted "
              "(empty), never sending an empty header value");
        check(req.headers.size() == 2,
              "D5-R4: still exactly the two original headers (Content-Type, Authorization) -- nothing "
              "extra silently added");
    }
    {
        // Both headers requested: exact values land, alongside the two pre-existing headers.
        auto req = build_http_request("/v1/chat/completions", "sk-test", "{}",
                                       "https://myapp.example/", "My Agent App");
        std::string referer_value, title_value;
        for (auto const& [k, v] : req.headers) {
            if (k == "HTTP-Referer") referer_value = v;
            if (k == "X-Title") title_value = v;
        }
        check(referer_value == "https://myapp.example/",
              "D5-R5: HTTP-Referer carries the exact app URL supplied, this project's own attribution "
              "identifier for OpenRouter-style leaderboard ranking (Finding 1)");
        check(title_value == "My Agent App",
              "D5-R5: X-Title carries the exact display name supplied");
        check(req.headers.size() == 4,
              "D5-R5: two original headers plus exactly the two new ones -- 4 total, nothing duplicated");
    }
    {
        // Only HTTP-Referer requested, X-Title left default: independently optional, same as D5-R3
        // above proves for end_user_id/seed.
        auto req = build_http_request("/v1/chat/completions", "sk-test", "{}", "https://only-referer.example/");
        bool has_referer = false, has_title = false;
        std::string referer_value;
        for (auto const& [k, v] : req.headers) {
            if (k == "HTTP-Referer") { has_referer = true; referer_value = v; }
            if (k == "X-Title") has_title = true;
        }
        check(has_referer && referer_value == "https://only-referer.example/",
              "D5-R6: HTTP-Referer present alone when only it is supplied");
        check(!has_title, "D5-R6: X-Title still absent -- setting HTTP-Referer alone doesn't fabricate a title");
    }

    // ---- D5: parse_chat_completion_response maps top-level 'model' (Finding 4) ----------------------
    {
        auto parsed = json::parse(R"({
            "id":"chatcmpl-2","object":"chat.completion","model":"gpt-5-turbo-2026",
            "choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}]
        })");
        check(parsed.has_value(), "D5-R7: literal wire JSON carrying a top-level 'model' field parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D5-R7: response with 'model' present parses successfully");
            if (resp) {
                check(resp->model == "gpt-5-turbo-2026",
                      "D5-R7: ChatResponse::model is populated from the response body's OWN 'model' field "
                      "(the model that actually answered), not from the request's model -- this backend "
                      "never sees the request here, so a correct value proves it came from the wire, not "
                      "an echo");
            }
        }
    }
    {
        // No 'model' field at all in the response (a real, valid shape -- not every backend reports it,
        // e.g. plain OpenAI responses predating this field or minimal local servers): stays empty, not
        // fabricated from anywhere.
        auto parsed = json::parse(R"({
            "choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}]
        })");
        check(parsed.has_value(), "D5-R8: literal wire JSON with no 'model' field parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D5-R8: response with no 'model' field still parses successfully");
            if (resp) {
                check(resp->model.empty(),
                      "D5-R8: ChatResponse::model stays the empty-string default when the backend doesn't "
                      "report one -- never fabricated");
            }
        }
    }

    // ---- D5: parse_chat_completion_response maps usage.prompt_tokens_details.cache_write_tokens ------
    // (OpenRouter-specific extension, Finding 5 -- plain OpenAI responses simply won't have this field.)
    {
        auto parsed = json::parse(R"({
            "choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}],
            "usage":{"prompt_tokens":100,"completion_tokens":10,
                "prompt_tokens_details":{"cached_tokens":40,"cache_write_tokens":25}}
        })");
        check(parsed.has_value(), "D5-R9: literal wire JSON carrying cache_write_tokens parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D5-R9: response with cache_write_tokens present parses successfully");
            if (resp) {
                check(resp->usage.cache_write_tokens == 25,
                      "D5-R9: usage.prompt_tokens_details.cache_write_tokens maps to "
                      "Usage::cache_write_tokens (the OpenRouter cache-creation extension field)");
                check(resp->usage.cached_input_tokens == 40,
                      "D5-R9: the pre-existing cached_tokens mapping (a cache READ) still works "
                      "unaffected alongside the new cache_write_tokens mapping (a cache WRITE)");
            }
        }
    }
    {
        // A plain OpenAI-shaped response with prompt_tokens_details present but no cache_write_tokens
        // key at all inside it: stays at its 0 default, not an error, not a fabricated value.
        auto parsed = json::parse(R"({
            "choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}],
            "usage":{"prompt_tokens":100,"completion_tokens":10,
                "prompt_tokens_details":{"cached_tokens":40}}
        })");
        check(parsed.has_value(), "D5-R10: literal plain-OpenAI-shaped usage JSON (no cache_write_tokens) parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D5-R10: response parses successfully");
            if (resp) {
                check(resp->usage.cache_write_tokens == 0,
                      "D5-R10: cache_write_tokens stays 0 when the OpenRouter-specific extension field is "
                      "absent (a plain OpenAI response, not an OpenRouter one) -- this is fine, not an error");
            }
        }
    }
    {
        // No usage object at all: model is still parsed independently of usage (the two are unrelated
        // top-level fields), and cache_write_tokens naturally stays 0 alongside every other Usage field.
        auto parsed = json::parse(R"({
            "model":"local-llama-70b",
            "choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}]
        })");
        check(parsed.has_value(), "D5-R11: literal wire JSON with 'model' but no 'usage' object at all parses");
        if (parsed) {
            auto resp = parse_chat_completion_response(*parsed);
            check(resp.has_value(), "D5-R11: response with no usage object still parses successfully");
            if (resp) {
                check(resp->model == "local-llama-70b",
                      "D5-R11: model parsing is independent of usage parsing -- 'model' populates even "
                      "when the response carries no 'usage' object at all");
                check(resp->usage.cache_write_tokens == 0 && resp->usage.input_tokens == 0,
                      "D5-R11: every Usage field, including the new cache_write_tokens, stays at its "
                      "default (0) when there is no usage object to read from");
            }
        }
    }

    // ---- ADR-023 Phase 1: apply_response_format_scan (decisions/ADR-023-response-format-codec-
    // seam.md §6 point 3) -- `chat()` only calls this when `scan_response_format_leaks_` is armed
    // (default false); the function itself is tested directly here, the same level
    // `parse_chat_completion_response` itself is tested at above, rather than standing up
    // test_openai_chat_client_live.cpp-style TLS server infrastructure for what is, at the chat()
    // call site, a single `if` branch around this already-proven function.
    {
        Message message;
        message.role = role::assistant;
        ContentItem leaked;
        leaked.origin = content_origin::assistant;
        leaked.value = Text{"<|start|>assistant<|channel|>analysis<|message|>Need the weather.<|end|>"
                             "<|start|>assistant<|channel|>final<|message|>It is sunny.<|return|>"};
        message.content.push_back(std::move(leaked));

        Message scanned = apply_response_format_scan(message, /*tools=*/{});
        check(scanned.content.size() == 2,
              "ADR-023 P1-R1: a leaked Harmony analysis+final turn splits into exactly 2 content items");
        if (scanned.content.size() == 2) {
            check(std::holds_alternative<Reasoning>(scanned.content[0].value),
                  "ADR-023 P1-R1: item 0 is a real Reasoning content item, not raw text");
            check(std::get<Reasoning>(scanned.content[0].value).text == "Need the weather.",
                  "ADR-023 P1-R1: Reasoning text is cleaned (envelope tokens stripped)");
            check(std::holds_alternative<Text>(scanned.content[1].value) && !scanned.content[1].tainted,
                  "ADR-023 P1-R1: item 1 is clean, untainted final-channel Text");
            check(std::get<Text>(scanned.content[1].value).text == "It is sunny.",
                  "ADR-023 P1-R1: final Text is cleaned (envelope tokens stripped)");
        }
    }
    {
        // Ordinary clean content (no format markers) must survive `apply_response_format_scan`
        // byte-identical -- proves the OpenAI-backend wiring doesn't corrupt the vastly more common
        // case where nothing leaked, mirroring the codec's own negative control at a higher level.
        Message message;
        message.role = role::assistant;
        ContentItem clean;
        clean.origin = content_origin::assistant;
        clean.value = Text{"The weather in San Francisco is sunny."};
        message.content.push_back(clean);

        Message scanned = apply_response_format_scan(message, /*tools=*/{});
        check(scanned.content.size() == 1 &&
                  std::holds_alternative<Text>(scanned.content[0].value) &&
                  std::get<Text>(scanned.content[0].value).text == "The weather in San Francisco is sunny." &&
                  !scanned.content[0].tainted,
              "ADR-023 P1-R2: clean content with no format markers passes through apply_response_format_scan "
              "byte-identical and untainted");
    }
    {
        // A ToolCall item (from the wire's own structured tool_calls field) must never be touched by
        // this function -- it only ever inspects `Text` items.
        Message message;
        message.role = role::assistant;
        ContentItem call;
        call.origin = content_origin::assistant;
        call.value = ToolCall{"call_1", "get_weather", R"({"location":"SF"})"};
        message.content.push_back(call);

        Message scanned = apply_response_format_scan(message, /*tools=*/{});
        check(scanned.content.size() == 1 && std::holds_alternative<ToolCall>(scanned.content[0].value) &&
                  std::get<ToolCall>(scanned.content[0].value).call_id == "call_1",
              "ADR-023 P1-R3: a structured ToolCall content item is left completely untouched -- this "
              "function only ever inspects Text items, never re-derives or drops an already-structured call");
    }

    // ---- ADR-023 Phase 2: candidate -> real, tagged ToolCall promotion (§6 point 4) ----------------
    {
        // A candidate whose recipient matches a tool present in `request.tools` is promoted to a
        // real ToolCall content item tagged text_derived -- never vendor_structured, so invoke_tool
        // step 5 (core/tool_pipeline.hpp) can tell it apart from a vendor-structured call.
        Message message;
        message.role = role::assistant;
        ContentItem leaked;
        leaked.origin = content_origin::assistant;
        leaked.value = Text{"<tool_call>{\"name\":\"get_weather\",\"arguments\":{\"location\":\"SF\"}}</tool_call>"};
        message.content.push_back(std::move(leaked));

        ToolDescriptor known_tool;
        known_tool.name = "get_weather";
        std::vector<ToolDescriptor> tools{known_tool};

        Message scanned = apply_response_format_scan(message, tools);
        check(scanned.content.size() == 1,
              "ADR-023 P2-R1: a candidate matching a known tool yields exactly 1 content item");
        if (scanned.content.size() == 1) {
            check(std::holds_alternative<ToolCall>(scanned.content[0].value),
                  "ADR-023 P2-R1: the item is a real ToolCall, not the Phase-1 inert diagnostic Text");
            if (std::holds_alternative<ToolCall>(scanned.content[0].value)) {
                ToolCall const& call = std::get<ToolCall>(scanned.content[0].value);
                check(call.tool_name == "get_weather", "ADR-023 P2-R1: tool_name is the parsed recipient");
                check(call.provenance == call_provenance::text_derived,
                      "ADR-023 P2-R1: provenance is text_derived -- NEVER vendor_structured -- so "
                      "invoke_tool step 5 applies the capability-scoped gate, not the tool's own "
                      "approval_mode unconditionally");
            }
        }
    }
    {
        // A candidate whose recipient does NOT match any tool in `request.tools` stays the Phase-1
        // inert diagnostic Text -- promoting an unrecognized name would only reach invoke_tool step
        // 1's "unknown tool" rejection anyway.
        Message message;
        message.role = role::assistant;
        ContentItem leaked;
        leaked.origin = content_origin::assistant;
        leaked.value = Text{"<tool_call>{\"name\":\"totally_unknown_tool\",\"arguments\":{}}</tool_call>"};
        message.content.push_back(std::move(leaked));

        ToolDescriptor unrelated_tool;
        unrelated_tool.name = "get_weather";
        std::vector<ToolDescriptor> tools{unrelated_tool};

        Message scanned = apply_response_format_scan(message, tools);
        check(scanned.content.size() == 1 && std::holds_alternative<Text>(scanned.content[0].value) &&
                  scanned.content[0].tainted,
              "ADR-023 P2-R2: a candidate with no matching tool name stays the Phase-1 inert, tainted "
              "diagnostic Text -- never promoted to a ToolCall for a tool that isn't even in this "
              "call's own tool list");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_chat_client_translation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_chat_client_translation: %d FAILURE(S)\n", g_failures);
    return 1;
}
