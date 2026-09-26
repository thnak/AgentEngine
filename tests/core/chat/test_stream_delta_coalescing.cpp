// Proof that a streamed reply is reconstructed as one item per piece of content rather than one item
// per token -- which is what lets the response-format leak scans see a leak that arrives in
// fragments -- and that only the PRODUCER decides what counts as one piece.
//
// WHAT WAS WRONG. Both stream drains (`core/chat_stream_drain.hpp`'s `drain_chat_stream()` and
// `rt/agent_session_trust.hpp`'s `drain_streaming_response()`) pushed every delta as its own
// `ContentItem`. A backend emits one delta per SSE chunk, so a streamed reply became one 160-byte
// item per token. Two costs followed, and the second is the one that matters:
//
//   1. Footprint. A 2000-token reply retained ~30x its own text, then got deep-copied every turn.
//   2. The scans that read a reply read it ONE ITEM AT A TIME. `detect_undeclared_tool_call_leak()`
//      (OQ-23: refuse a raw wire-format tool call rather than accept it as text) and
//      `apply_response_format_scan()` (ADR-035: promote it to a tainted, text_derived call) decode
//      each `Text` item separately. Measured before the fix: the same `<tool_call>` bytes were
//      refused and promoted when they arrived whole, and neither when they arrived as nineteen
//      fragments. `AgentSession::run_model_call()` runs both scans on the streamed path too, so
//      turning streaming on was a way around a fail-closed check.
//
// WHY THE PRODUCER DECIDES. A first version joined any two adjacent `Text` items in the drain. A
// red-team pass measured that gluing two things the non-streamed reply keeps apart: Anthropic text
// blocks either side of a `tool_use` block (that accumulator defers `tool_use` to `finish()`, so the
// texts arrive back to back) became one string with no separator, and a workflow's fan-in joined one
// agent's text onto another's. `ChatResponseUpdate::continues_previous` now carries the producer's
// knowledge; a drain joins only what was marked, and only when the metadata also matches.
//
//   S1 (positive control) -- the detector and the promoter both fire on a whole leak.
//   S2 (the guard) -- the same leak, streamed as producer-marked fragments through a REAL stream and
//         `drain_chat_stream()`, is refused and promoted. Unfixed: neither.
//   S3 (the guard, session path) -- the same through `drain_streaming_response()`, with live
//         `model_delta` events still one per fragment.
//   S4 -- 2000 marked fragments retain one item with byte-identical text.
//   S5 -- UNMARKED adjacent text is never joined: the fan-in shape the red-team found. Meaningful
//         boundaries survive; a differing origin or taint never joins even when marked.
//   S6 -- reasoning joins only with reasoning from the same producer; an encrypted trace never joins.
//   S7 -- text and reasoning never join each other, marked or not.
//   S8 -- a tool-call argument chunk leaves no empty placeholder item in `drain_chat_stream()`, the
//         rule the session drain already had.
//   S9 -- the recording codec round-trips the flag, so a replayed stream rebuilds the same message
//         the live one did (I5), and a recording written before the flag existed reads as unmarked.
//   P1-P4 (HTTPS builds, which is where the provider parsers live) -- the real Anthropic and OpenAI
//         accumulators mark what they should: one Anthropic text block streamed in pieces becomes
//         one item and its leak is caught; two blocks around a tool call stay two items, matching
//         the non-streamed parse; OpenAI content and reasoning each become one item.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/chat_stream_drain.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
#include "agentengine/rt/agent_session_trust.hpp"
#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#endif

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace agentengine;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

std::string const kLeak =
    "<tool_call>{\"name\":\"get_weather\",\"arguments\":{\"location\":\"SF\"}}</tool_call>";

[[nodiscard]] std::vector<ToolDescriptor> live_tools() {
    ToolDescriptor t;
    t.name = "get_weather";
    return {t};
}

[[nodiscard]] ContentItem text_item(std::string s, content_origin origin = content_origin::assistant,
                                    bool tainted = false) {
    ContentItem item;
    item.value = Text{std::move(s)};
    item.origin = origin;
    item.tainted = tainted;
    return item;
}

[[nodiscard]] ContentItem reasoning_item(std::string s, bool encrypted = false,
                                         std::string producer = "openai:gpt") {
    ContentItem item;
    item.value = Reasoning{std::move(s), encrypted, std::move(producer)};
    return item;
}

[[nodiscard]] ChatResponseUpdate update_of(ContentItem item, bool continues) {
    ChatResponseUpdate u;
    u.delta = std::move(item);
    u.continues_previous = continues;
    return u;
}

// A real stream, not a hand-built Message: the fix is in how a drain reconstructs what a producer
// pushed, so the producer has to actually push. A final, usage-bearing update is appended because the
// session drain fails closed without one -- except for a provider parser's output (`parsed`), which
// already ends in its own final update and is pushed exactly as produced. Capacity covers every
// update, so nothing blocks.
[[nodiscard]] stream<ChatResponseUpdate> stream_of(std::vector<ChatResponseUpdate> updates,
                                                   bool parsed = false) {
    stream_config<ChatResponseUpdate> cfg;
    cfg.capacity = updates.size() + 1;
    auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
    for (auto& u : updates) (void)pair.producer.push(std::move(u));
    if (parsed) {
        pair.producer.close();
        return std::move(pair.consumer);
    }
    ChatResponseUpdate last;
    last.is_final = true;
    last.usage = Usage{1, 1, 0, 0, 0.0};
    last.delta = text_item("");
    last.continues_previous = true;
    (void)pair.producer.push(std::move(last));
    pair.producer.close();
    return std::move(pair.consumer);
}

// What a producer pushes for one string streamed in `width`-byte pieces: the first piece starts an
// item, every later one continues it.
[[nodiscard]] std::vector<ChatResponseUpdate> marked_fragments_of(std::string const& s, std::size_t width) {
    std::vector<ChatResponseUpdate> out;
    for (std::size_t i = 0; i < s.size(); i += width) {
        out.push_back(update_of(text_item(s.substr(i, width)), /*continues=*/i != 0));
    }
    return out;
}

[[nodiscard]] int promoted_calls(Message const& m) {
    int n = 0;
    for (auto const& c : m.content) n += std::holds_alternative<ToolCall>(c.value) ? 1 : 0;
    return n;
}

template <class T>
[[nodiscard]] int items_of(Message const& m) {
    int n = 0;
    for (auto const& c : m.content) n += std::holds_alternative<T>(c.value) ? 1 : 0;
    return n;
}

#ifdef AGENTENGINE_WITH_HTTPS
[[nodiscard]] std::string sse(char const* ev, std::string const& data) {
    return std::string("event: ") + ev + "\ndata: " + data + "\n\n";
}
#endif

}  // namespace

int main() {
    auto const tools = live_tools();

    // ---- S1: the positive control.
    {
        Message whole;
        whole.role = role::assistant;
        whole.content.push_back(text_item(kLeak));
        check(!detect_undeclared_tool_call_leak(whole, tools).has_value(),
              "S1 (positive control): the detector refuses a leak that arrives whole");
        check(promoted_calls(apply_response_format_scan(whole, tools)) == 1,
              "S1 (positive control): the scan promotes a leak that arrives whole");
    }

    // ---- S2: the guard, through drain_chat_stream().
    {
        auto fragments = marked_fragments_of(kLeak, 4);
        std::size_t const n = fragments.size();
        DrainedChatStream drained = drain_chat_stream(stream_of(std::move(fragments)));
        check(drained.ok && n > 10,
              "S2: the stream drains to a clean terminal and really was split, into " + std::to_string(n) +
                  " fragments");
        check(!detect_undeclared_tool_call_leak(drained.accumulated, tools).has_value(),
              "S2: the detector REFUSES the leak after it was streamed in fragments -- unfixed each "
              "fragment was scanned alone and the leak was accepted as an ordinary reply");
        check(promoted_calls(apply_response_format_scan(drained.accumulated, tools)) == 1,
              "S2: the scan PROMOTES the fragmented leak -- unfixed it promoted nothing");
    }

    // ---- S3: the guard, through the drain AgentSession actually uses.
    {
        auto fragments = marked_fragments_of(kLeak, 4);
        std::size_t const n = fragments.size();
        auto live_deltas = std::make_shared<int>(0);
        rt::detail::EmitFn const emit = [live_deltas](run_event_kind k, RunEventPayload) {
            if (k == run_event_kind::model_delta) ++*live_deltas;
        };
        auto response = rt::detail::drain_streaming_response(stream_of(std::move(fragments)), true, emit);
        check(response.has_value(), "S3: drain_streaming_response() completes");
        check(response.has_value() &&
                  !detect_undeclared_tool_call_leak(response->message, tools).has_value(),
              "S3: the session drain's reply is refused by the detector, as the non-streamed reply is");
        check(response.has_value() &&
                  promoted_calls(apply_response_format_scan(response->message, tools)) == 1,
              "S3: ... and promoted by the scan");
        check(*live_deltas == static_cast<int>(n),
              "S3: live model_delta events are still one per fragment (" + std::to_string(*live_deltas) +
                  ") -- the stored reply is joined, the live view is not");
    }

    // ---- S4: byte-identical text, and one item for 2000 fragments.
    {
        std::string expected;
        std::vector<ChatResponseUpdate> tokens;
        for (int i = 0; i < 2000; ++i) {
            std::string tok = "tok" + std::to_string(i) + " ";
            expected += tok;
            tokens.push_back(update_of(text_item(std::move(tok)), i != 0));
        }
        DrainedChatStream drained = drain_chat_stream(stream_of(std::move(tokens)));
        check(drained.ok && drained.accumulated.content.size() == 1,
              "S4: 2000 marked fragments are retained as ONE item (was 2001), got " +
                  std::to_string(drained.accumulated.content.size()));
        auto const* t = drained.accumulated.content.empty()
                            ? nullptr
                            : std::get_if<Text>(&drained.accumulated.content[0].value);
        check(t != nullptr && t->text == expected,
              "S4: ... and its text is byte-identical to the fragments concatenated in order");
    }

    // ---- S5: unmarked adjacency is not continuity; meaningful boundaries survive.
    {
        // The red-team's fan-in shape: two agents' outputs handed over back to back, unmarked.
        std::vector<ChatResponseUpdate> fan_in;
        fan_in.push_back(update_of(text_item("A says: <tool_call>{\"name\":\"get_weather\","), false));
        fan_in.push_back(update_of(text_item("\"arguments\":{}}</tool_call> B done"), false));
        DrainedChatStream drained = drain_chat_stream(stream_of(std::move(fan_in)));
        check(items_of<Text>(drained.accumulated) == 2,
              "S5: two UNMARKED adjacent text updates stay two items -- a drain no longer invents a "
              "join the producer did not claim, so two agents' outputs are not glued into one string");

        Message m;
        append_stream_delta(m, text_item("before "), false);
        append_stream_delta(m, text_item("call"), true);
        ContentItem call;
        call.value = ToolCall{"c1", "get_weather", "{}"};
        append_stream_delta(m, call, true);
        append_stream_delta(m, text_item("after"), true);
        check(m.content.size() == 3 && std::get<Text>(m.content[0].value).text == "before call" &&
                  std::holds_alternative<ToolCall>(m.content[1].value) &&
                  std::get<Text>(m.content[2].value).text == "after",
              "S5: text, tool call, text stays three items in order even when every update is marked "
              "-- a tool call never joins and text never joins across one");

        Message o;
        append_stream_delta(o, text_item("model says ", content_origin::assistant), false);
        append_stream_delta(o, text_item("tool said", content_origin::tool), true);
        check(o.content.size() == 2, "S5: a differing origin never joins, even when marked");

        Message tn;
        append_stream_delta(tn, text_item("clean ", content_origin::assistant, false), false);
        append_stream_delta(tn, text_item("tainted", content_origin::assistant, true), true);
        check(tn.content.size() == 2, "S5: a differing taint never joins, even when marked");
    }

    // ---- S6: reasoning.
    {
        Message r;
        append_stream_delta(r, reasoning_item("think "), false);
        append_stream_delta(r, reasoning_item("more"), true);
        check(r.content.size() == 1 && std::get<Reasoning>(r.content[0].value).text == "think more",
              "S6: marked plaintext reasoning from one producer joins like text");

        Message p;
        append_stream_delta(p, reasoning_item("a", false, "openai:gpt"), false);
        append_stream_delta(p, reasoning_item("b", false, "anthropic:claude"), true);
        check(p.content.size() == 2,
              "S6: reasoning from different producers never joins -- 003 §8 Q2 decides replay from it");

        Message e;
        append_stream_delta(e, reasoning_item("blobA", true), false);
        append_stream_delta(e, reasoning_item("blobB", true), true);
        append_stream_delta(e, reasoning_item("plain", false), true);
        check(e.content.size() == 3,
              "S6: an encrypted trace never joins, onto or from anything -- two opaque blobs "
              "concatenated are a corruption");
    }

    // ---- S7: kinds do not join each other.
    {
        Message k;
        append_stream_delta(k, reasoning_item("thinking"), false);
        append_stream_delta(k, text_item("answer"), true);
        append_stream_delta(k, reasoning_item("again"), true);
        check(k.content.size() == 3 && std::holds_alternative<Reasoning>(k.content[0].value) &&
                  std::holds_alternative<Text>(k.content[1].value) &&
                  std::holds_alternative<Reasoning>(k.content[2].value),
              "S7: text and reasoning never join each other, and order across kinds is preserved");
    }

    // ---- S8: argument chunks leave no placeholder in drain_chat_stream().
    {
        std::vector<ChatResponseUpdate> ups;
        ups.push_back(update_of(text_item("hello "), false));
        ChatResponseUpdate chunk;
        chunk.tool_call_argument_chunk = ToolCallArgumentChunk{"c1", "get_weather", "{\"loc", false};
        ups.push_back(std::move(chunk));
        ups.push_back(update_of(text_item("world"), true));
        DrainedChatStream drained = drain_chat_stream(stream_of(std::move(ups)));
        check(drained.accumulated.content.size() == 1 &&
                  std::get<Text>(drained.accumulated.content[0].value).text == "hello world",
              "S8: an argument-chunk update adds no empty item to drain_chat_stream()'s reply, got " +
                  std::to_string(drained.accumulated.content.size()) + " item(s)");
    }

    // ---- S9: the recording codec.
    {
        auto marked = update_of(text_item("next"), true);
        auto back = chat_response_update_from_json(chat_response_update_to_json(marked));
        check(back.has_value() && back->continues_previous,
              "S9: a marked update survives a recording round trip, so a replay joins what the live run "
              "joined (I5)");
        auto unmarked = update_of(text_item("first"), false);
        std::string const written = json::dump(chat_response_update_to_json(unmarked));
        check(written.find("continues_previous") == std::string::npos,
              "S9: an unmarked update writes no new key, so recordings made before this field are "
              "byte-identical to what is written for the same updates now");
        // The document just written has no key at all, which is exactly what every recording made
        // before this field existed looks like -- so reading it back is the legacy case, not a guess
        // at an older format.
        auto legacy = json::parse(written);
        auto legacy_update = legacy.has_value() ? chat_response_update_from_json(*legacy)
                                                : result<ChatResponseUpdate>(std::unexpected(error{}));
        check(legacy_update.has_value() && !legacy_update->continues_previous,
              "S9: a recording without the key reads back as unmarked, never as a join");
    }

#ifdef AGENTENGINE_WITH_HTTPS
    // ---- P1/P2: the real Anthropic accumulator.
    {
        // One text block streamed in three text_deltas, carrying the whole leak between them.
        std::string const l1 = R"(<tool_call>{\"name\":\"get_weather\",)";
        std::string const l2 = R"(\"arguments\":{\"location\":)";
        std::string const l3 = R"(\"SF\"}}</tool_call>)";
        std::string body;
        body += sse("message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":5,"output_tokens":1}}})");
        body += sse("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
        for (auto const* piece : {&l1, &l2, &l3}) {
            body += sse("content_block_delta",
                        std::string(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":")") +
                            *piece + R"("}})");
        }
        body += sse("content_block_stop", R"({"type":"content_block_stop","index":0})");
        body += sse("message_delta", R"({"type":"message_delta","usage":{"output_tokens":20}})");
        body += sse("message_stop", R"({"type":"message_stop"})");
        auto ups = anthropic::detail::parse_streaming_response_into_updates(body, false, "anthropic:claude");
        check(ups.has_value(), "P1: the Anthropic stream parses");
        if (ups.has_value()) {
            DrainedChatStream drained = drain_chat_stream(stream_of(std::move(*ups), /*parsed=*/true));
            check(items_of<Text>(drained.accumulated) == 1,
                  "P1: three text_deltas of ONE Anthropic block become one item, got " +
                      std::to_string(items_of<Text>(drained.accumulated)));
            check(!detect_undeclared_tool_call_leak(drained.accumulated, tools).has_value(),
                  "P1: ... so the leak streamed across them is refused by the real parser's path");
        }
    }
    {
        // The red-team's shape: text block | tool_use block | text block.
        std::string body;
        body += sse("message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":5,"output_tokens":1}}})");
        body += sse("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
        body += sse("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Let me "}})");
        body += sse("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"check."}})");
        body += sse("content_block_stop", R"({"type":"content_block_stop","index":0})");
        body += sse("content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"tu1","name":"get_weather","input":{}}})");
        body += sse("content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"location\":\"NYC\"}"}})");
        body += sse("content_block_stop", R"({"type":"content_block_stop","index":1})");
        body += sse("content_block_start", R"({"type":"content_block_start","index":2,"content_block":{"type":"text","text":""}})");
        body += sse("content_block_delta", R"({"type":"content_block_delta","index":2,"delta":{"type":"text_delta","text":"The weather"}})");
        body += sse("content_block_stop", R"({"type":"content_block_stop","index":2})");
        body += sse("message_delta", R"({"type":"message_delta","usage":{"output_tokens":20}})");
        body += sse("message_stop", R"({"type":"message_stop"})");
        auto ups = anthropic::detail::parse_streaming_response_into_updates(body, false, "anthropic:claude");
        check(ups.has_value(), "P2: the multi-block Anthropic stream parses");
        if (ups.has_value()) {
            DrainedChatStream drained = drain_chat_stream(stream_of(std::move(*ups), /*parsed=*/true));
            bool glued = false;
            for (auto const& c : drained.accumulated.content) {
                if (auto const* t = std::get_if<Text>(&c.value); t && t->text.find("check.The") != std::string::npos) {
                    glued = true;
                }
            }
            check(items_of<Text>(drained.accumulated) == 2 && !glued,
                  "P2: two text blocks either side of a tool_use stay TWO items -- the first version "
                  "glued them into \"Let me check.The weather\"; got " +
                      std::to_string(items_of<Text>(drained.accumulated)) + " text item(s)");
            check(items_of<ToolCall>(drained.accumulated) == 1, "P2: ... and the vendor tool call is kept");
        }
    }
    // ---- P3/P4: the real OpenAI accumulator.
    {
        std::string body;
        auto d = [&](std::string const& j) { body += "data: " + j + "\n\n"; };
        d(R"({"choices":[{"delta":{"reasoning":"think "}}]})");
        d(R"({"choices":[{"delta":{"reasoning":"hard"}}]})");
        d(R"({"choices":[{"delta":{"content":"<tool_call>{\"name\":\"get_weather\","}}]})");
        d(R"({"choices":[{"delta":{"content":"\"arguments\":{\"location\":"}}]})");
        d(R"({"choices":[{"delta":{"content":"\"SF\"}}</tool_call>"}}]})");
        d(R"({"choices":[],"usage":{"prompt_tokens":1,"completion_tokens":1}})");
        body += "data: [DONE]\n\n";
        auto ups = openai::detail::parse_streaming_response_into_updates(body, false, "openai:gpt");
        check(ups.has_value(), "P3: the OpenAI stream parses");
        if (ups.has_value()) {
            auto response = rt::detail::drain_streaming_response(stream_of(std::move(*ups), /*parsed=*/true), false,
                                                                 [](run_event_kind, RunEventPayload) {});
            check(response.has_value() && items_of<Reasoning>(response->message) == 1 &&
                      items_of<Text>(response->message) == 1,
                  "P3: two reasoning deltas become one reasoning item and three content deltas one text "
                  "item");
            check(response.has_value() &&
                      !detect_undeclared_tool_call_leak(response->message, tools).has_value(),
                  "P4: the leak split across OpenAI content deltas is refused on the session's path");
        }
    }
#else
    std::printf("[skip] P1-P4: provider parsers are only built with AGENTENGINE_WITH_HTTPS\n");
#endif

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
