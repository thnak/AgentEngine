// Milestone 5 Phase G1 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase G):
// proves core/chat_recording.hpp's JSON codec round-trips every ContentItem variant alternative, a
// unary ChatResponse (success and error), and a streaming chunk sequence (with per-chunk timing and
// a stream_terminal) -- the shared foundation `RecordingChatClient<Inner>` and `ReplayChatClient`
// both depend on. Offline, no network, no filesystem beyond a scratch temp file for the
// write/read-back round trip.

#include <cstdio>
#include <filesystem>
#include <vector>

#include "agentengine/core/chat_recording.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using namespace agentengine;

ContentItem make_text(std::string s) {
    ContentItem item{};
    item.value = Text{std::move(s)};
    item.origin = content_origin::assistant;
    return item;
}

void test_content_item_round_trip_all_kinds() {
    std::vector<ContentItem> items;

    {
        ContentItem c{};
        c.value = Text{"hello world"};
        c.origin = content_origin::user;
        c.tainted = true;
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = Reasoning{"because X", true};
        items.push_back(c);
    }
    {
        ContentItem c{};
        Media m;
        m.media_type = "image/png";
        m.payload = std::vector<std::byte>{std::byte{0x00}, std::byte{0x01}, std::byte{0xFF}, std::byte{0x10}};
        c.value = m;
        items.push_back(c);
    }
    {
        ContentItem c{};
        Media m;
        m.media_type = "image/jpeg";
        m.payload = std::string("https://example.invalid/a.jpg");
        c.value = m;
        items.push_back(c);
    }
    {
        ContentItem c{};
        Media m;
        m.media_type = "application/octet-stream";
        m.payload = BlobRef{"sha256:deadbeef", "application/octet-stream", 4096, "workspace-blob-store"};
        c.value = m;
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = Data{"{\"k\":1}", std::string("schema-1")};
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = ToolCall{"call-1", "sum", "{\"a\":1,\"b\":2}", content_origin::assistant};
        items.push_back(c);
    }
    {
        ContentItem c{};
        ToolResult tr;
        tr.call_id = "call-1";
        tr.content.push_back(make_text("3"));
        tr.is_error = false;
        c.value = std::move(tr);
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = Citation{"doc-7", 10, 42};
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = Error{"tool timed out"};
        items.push_back(c);
    }
    {
        ContentItem c{};
        c.value = Custom{"urn:agentengine:custom:widget", "{\"n\":7}"};
        items.push_back(c);
    }

    for (std::size_t i = 0; i < items.size(); ++i) {
        json::Value const j = content_item_to_json(items[i]);
        auto const round_tripped = content_item_from_json(j);
        check(round_tripped.has_value(), "G1-R1: content_item_from_json succeeds for every kind");
        if (!round_tripped) continue;

        json::Value const j2 = content_item_to_json(*round_tripped);
        check(json::dump(j) == json::dump(j2), "G1-R1: round trip is byte-identical on the second pass");
    }
}

void test_tool_result_recursion() {
    ContentItem inner{};
    inner.value = ToolCall{"c1", "nested", "{}", content_origin::assistant};

    ContentItem outer{};
    ToolResult tr;
    tr.call_id = "c0";
    tr.content.push_back(inner);
    tr.is_error = true;
    outer.value = std::move(tr);

    auto j = content_item_to_json(outer);
    auto back = content_item_from_json(j);
    check(back.has_value(), "G1-R2: nested tool_result parses");
    if (!back) return;
    auto const* tr2 = std::get_if<ToolResult>(&back->value);
    check(tr2 != nullptr, "G1-R2: kind preserved as tool_result");
    check(tr2 != nullptr && tr2->is_error, "G1-R2: is_error preserved");
    check(tr2 != nullptr && tr2->content.size() == 1, "G1-R2: nested content item preserved");
}

void test_chat_response_round_trip() {
    ChatResponse r;
    r.message.role = role::assistant;
    r.message.message_id = "m-9";
    r.message.content.push_back(make_text("final answer"));
    r.usage = Usage{10, 20, 3, 4, 0.0123, 5};
    r.model = "vendor/actual-model";
    r.fallback_tier = 2;
    r.route_index = 1;  // ADR-148

    auto j = chat_response_to_json(r);
    auto back = chat_response_from_json(j);
    check(back.has_value(), "G1-R3: ChatResponse parses");
    if (!back) return;
    check(back->message.message_id == "m-9", "G1-R3: message_id preserved");
    check(back->usage.input_tokens == 10 && back->usage.cache_write_tokens == 5,
          "G1-R3: usage fields preserved, including cache_write_tokens");
    check(back->model == "vendor/actual-model", "G1-R3: model preserved");
    check(back->fallback_tier == 2, "G1-R3: fallback_tier preserved");
    check(back->route_index == 1, "G1-R3: route_index preserved (ADR-148)");
}

void test_error_round_trip() {
    error e{failure_class::resource, "budget exceeded", "test.budget", 0};
    auto j = error_to_json(e);
    auto back = error_from_json(j);
    check(back.has_value(), "G1-R4: error parses");
    if (!back) return;
    check(back->klass == failure_class::resource, "G1-R4: failure_class preserved");
    check(back->message == "budget exceeded", "G1-R4: message preserved");
    check(back->code == "test.budget", "G1-R4: code preserved");
}

void test_unary_recording_round_trip_success_and_failure() {
    {
        ChatCallRecording rec;
        rec.request.messages.push_back(
            []() { Message m; m.role = role::user; m.content.push_back(make_text("hi")); return m; }());
        rec.mode = recording_mode::unary;
        ChatResponse r;
        r.message.role = role::assistant;
        r.message.content.push_back(make_text("hello"));
        rec.response = r;
        rec.duration = std::chrono::milliseconds(123);

        auto j = chat_call_recording_to_json(rec);
        auto back = chat_call_recording_from_json(j);
        check(back.has_value(), "G1-R5: unary success recording parses");
        if (back) {
            check(back->mode == recording_mode::unary, "G1-R5: mode preserved");
            check(back->response.has_value(), "G1-R5: response present");
            check(!back->chat_error.has_value(), "G1-R5: no chat_error on a success recording");
            check(back->duration == std::chrono::milliseconds(123), "G1-R5: duration preserved");
            check(back->request.messages.size() == 1, "G1-R5: request messages preserved");
        }
    }
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::unary;
        rec.chat_error = error{failure_class::fatal, "provider unreachable", "test.unreachable"};
        rec.duration = std::chrono::milliseconds(7);

        auto j = chat_call_recording_to_json(rec);
        auto back = chat_call_recording_from_json(j);
        check(back.has_value(), "G1-R6: unary error recording parses");
        if (back) {
            check(!back->response.has_value(), "G1-R6: no response on an error recording");
            check(back->chat_error.has_value(), "G1-R6: chat_error present");
            check(back->chat_error && back->chat_error->code == "test.unreachable",
                  "G1-R6: chat_error code preserved");
        }
    }
}

void test_streaming_recording_round_trip_preserves_chunk_order_and_boundaries() {
    ChatCallRecording rec;
    rec.mode = recording_mode::streaming;
    rec.duration = std::chrono::milliseconds(500);

    for (int i = 0; i < 5; ++i) {
        RecordedChunk chunk;
        chunk.update.delta = make_text("chunk-" + std::to_string(i));
        chunk.update.is_final = (i == 4);
        chunk.elapsed_since_start = std::chrono::milliseconds(i * 100);
        rec.chunks.push_back(chunk);
    }
    rec.stream_terminal = "closed";

    auto j = chat_call_recording_to_json(rec);
    auto back = chat_call_recording_from_json(j);
    check(back.has_value(), "G2-R1: streaming recording parses");
    if (!back) return;

    check(back->mode == recording_mode::streaming, "G2-R1: mode preserved");
    check(back->chunks.size() == 5, "G2-R1: all 5 chunks preserved");
    check(back->stream_terminal == "closed", "G2-R1: stream_terminal preserved");

    // 004 §7 G3's own gate: replay must reproduce identical chunk BOUNDARIES -- order and per-chunk
    // content, not just set membership.
    for (std::size_t i = 0; i < back->chunks.size(); ++i) {
        auto const* text = std::get_if<Text>(&back->chunks[i].update.delta.value);
        check(text != nullptr && text->text == "chunk-" + std::to_string(i),
              "G2-R1: chunk order and content preserved exactly");
        check(back->chunks[i].elapsed_since_start == std::chrono::milliseconds(static_cast<int>(i) * 100),
              "G2-R1: per-chunk timing preserved");
    }
    check(back->chunks[4].update.is_final, "G2-R1: is_final preserved on the terminal chunk");
}

void test_streaming_failure_recording_round_trip() {
    ChatCallRecording rec;
    rec.mode = recording_mode::streaming;
    RecordedChunk chunk;
    chunk.update.delta = make_text("partial");
    rec.chunks.push_back(chunk);
    rec.stream_terminal = "failed";
    rec.stream_error_detail = "connection reset mid-stream";

    auto j = chat_call_recording_to_json(rec);
    auto back = chat_call_recording_from_json(j);
    check(back.has_value(), "G2-R2: failed-stream recording parses");
    if (!back) return;
    check(back->stream_terminal == "failed", "G2-R2: stream_terminal == failed preserved");
    check(back->stream_error_detail == "connection reset mid-stream", "G2-R2: stream_error_detail preserved");
    check(back->chunks.size() == 1, "G2-R2: partial chunk sequence before failure preserved");
}

void test_file_round_trip() {
    ChatCallRecording rec;
    rec.mode = recording_mode::unary;
    rec.request.messages.push_back(
        []() { Message m; m.role = role::user; m.content.push_back(make_text("hi")); return m; }());
    ChatResponse r;
    r.message.role = role::assistant;
    r.message.content.push_back(make_text("hello from file"));
    rec.response = r;

    std::filesystem::path const path =
        std::filesystem::temp_directory_path() / "ae_test_chat_recording_codec.json";
    auto written = write_chat_call_recording(path, rec);
    check(written.has_value(), "G1-R7: write_chat_call_recording succeeds");

    auto read_back = read_chat_call_recording(path);
    check(read_back.has_value(), "G1-R7: read_chat_call_recording succeeds");
    if (read_back) {
        check(read_back->response.has_value(), "G1-R7: response survives the file round trip");
        if (read_back->response) {
            auto const* text = std::get_if<Text>(&read_back->response->message.content.at(0).value);
            check(text != nullptr && text->text == "hello from file",
                  "G1-R7: response content survives the file round trip");
        }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto missing = read_chat_call_recording(
        std::filesystem::temp_directory_path() / "ae_test_chat_recording_codec_does_not_exist.json");
    check(!missing.has_value(), "G1-R8: reading a missing recording file fails, never fabricates a value");
    check(missing.has_value() || missing.error().klass == failure_class::fatal,
          "G1-R8: missing-file failure is failure_class::fatal");
}

} // namespace

int main() {
    test_content_item_round_trip_all_kinds();
    test_tool_result_recursion();
    test_chat_response_round_trip();
    test_error_round_trip();
    test_unary_recording_round_trip_success_and_failure();
    test_streaming_recording_round_trip_preserves_chunk_order_and_boundaries();
    test_streaming_failure_recording_round_trip();
    test_file_round_trip();

    if (g_failures == 0) {
        std::fprintf(stderr, "OK: all chat_recording codec checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
