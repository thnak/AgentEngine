// Milestone 5 Phase G2 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md lines
// 513-521; 004-Model-Provider-Plane.md §6/§7 G3): proves `ReplayChatClient`
// (core/replay_chat_client.hpp) end to end -- the real, product-code recording PLAYER that
// supersedes tests/support/recorded_chat_client.hpp's `RecordedChatClient` for product code
// (decision 8). Every scenario is constructed directly against `ChatCallRecording`
// (core/chat_recording.hpp) -- no JSON file I/O needed here, since chat_recording.hpp's own codec
// already has a dedicated, separately-proven test (test_chat_recording_codec.cpp).
//
// Covers:
//  (1) a unary recording with `.response` set replays it exactly via chat().
//  (2) a unary recording with `.chat_error` set replays the SAME error via chat().
//  (3) chat() against a STREAMING-mode recording fails closed with failure_class::contract.
//  (4) a streaming recording with N ordered chunks replays all N via chat_stream(), same order/content
//      (004 §7 G3's literal gate: "identical chunk boundaries").
//  (5) inter-chunk timing: an injected (non-sleeping) SleepFn observes the exact recorded deltas, in
//      order -- proves "UI cadence reproduces exactly" without any real wall-clock wait in this test.
//  (6) stream_terminal == "closed" -> terminal() == Closed; == "failed" -> terminal() == Failed with
//      fail_error() carrying the recorded detail text (plus a light check of the "cancelled"/
//      "deadline_exceeded" mappings, since they're cheap to prove alongside).
//  (7) chat_stream() against a UNARY-mode recording fails immediately, no item ever pushed.
//  (8) check()/g_failures throughout -- NOT assert() (RelWithDebInfo strips asserts, CLAUDE.md task
//      guidance).
//
// NOT built into tests/CMakeLists.txt yet (per the task's own instruction, to avoid a merge conflict
// with another agent editing that file concurrently for RecordingChatClient) -- wiring is pending.

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/replay_chat_client.hpp"
#include "agentengine/trust/principal.hpp"

#include "quark/core/error.hpp"

#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

[[nodiscard]] agentengine::EffectContext make_ctx() {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.run_id = "run-replay";
    ctx.turn_index = 0;
    return ctx;
}

[[nodiscard]] agentengine::ContentItem make_text_item(std::string text) {
    agentengine::ContentItem item;
    item.value = agentengine::Text{std::move(text)};
    item.origin = agentengine::content_origin::assistant;
    return item;
}

[[nodiscard]] agentengine::ChatResponse make_full_response() {
    agentengine::ChatResponse r;
    r.message.role = agentengine::role::assistant;
    r.message.message_id = "m-replay-1";
    r.message.content.push_back(make_text_item("hello from the recording"));
    r.usage.input_tokens = 11;
    r.usage.output_tokens = 22;
    r.usage.cached_input_tokens = 3;
    r.usage.reasoning_tokens = 4;
    r.usage.cost_estimate = 0.125;
    r.usage.cache_write_tokens = 5;
    r.model = "gpt-replay-test";
    r.fallback_tier = 2;
    return r;
}

// One RecordedChunk carrying a single Text delta, `is_final` set on the caller's say-so.
[[nodiscard]] agentengine::RecordedChunk make_chunk(std::string text, std::chrono::milliseconds elapsed,
                                                     bool is_final) {
    agentengine::RecordedChunk chunk;
    chunk.update.delta = make_text_item(std::move(text));
    chunk.update.is_final = is_final;
    chunk.elapsed_since_start = elapsed;
    return chunk;
}

// The poll-loop drain idiom from test_chat_client_stream.cpp: next() is poll-only, so drain to empty
// then check done() before yielding and trying again.
void drain_stream(agentengine::stream<agentengine::ChatResponseUpdate>& s,
                   std::vector<std::string>& received_texts, bool& saw_final) {
    while (!s.done()) {
        while (auto update = s.next()) {
            if (auto const* text = std::get_if<agentengine::Text>(&update->delta.value)) {
                received_texts.push_back(text->text);
            }
            if (update->is_final) saw_final = true;
        }
        if (!s.done()) std::this_thread::yield();
    }
}

}  // namespace

int main() {
    using namespace agentengine;
    using agentengine::test_support::run_task_sync;

    // ---- (1) unary .response replays exactly via chat() --------------------------------------------
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::unary;
        rec.response = make_full_response();

        ReplayChatClient client(rec);
        EffectContext ctx = make_ctx();
        auto result = run_task_sync<agentengine::result<ChatResponse>>(client.chat(ChatRequest{}, ctx));

        check(result.has_value(), "(1) a unary .response recording succeeds via chat()");
        if (result.has_value()) {
            check(result->model == "gpt-replay-test", "(1) model replays verbatim");
            check(result->fallback_tier == 2, "(1) fallback_tier replays verbatim");
            check(result->usage.input_tokens == 11 && result->usage.output_tokens == 22 &&
                      result->usage.cached_input_tokens == 3 && result->usage.reasoning_tokens == 4 &&
                      result->usage.cache_write_tokens == 5,
                  "(1) every Usage field replays verbatim");
            check(result->usage.cost_estimate == 0.125, "(1) Usage::cost_estimate replays verbatim");
            check(result->message.role == role::assistant, "(1) message role replays verbatim");
            check(result->message.message_id == "m-replay-1", "(1) message_id replays verbatim");
            check(result->message.content.size() == 1, "(1) message content replays with the same shape");
            if (result->message.content.size() == 1) {
                auto const* text = std::get_if<Text>(&result->message.content[0].value);
                check(text != nullptr, "(1) the sole content item is a Text alternative");
                if (text) check(text->text == "hello from the recording", "(1) text content replays verbatim");
            }
        }
    }

    // ---- (2) unary .chat_error replays the SAME error via chat() -----------------------------------
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::unary;
        rec.chat_error =
            error{failure_class::transient, "synthetic provider outage", "test.replay_error", 42};

        ReplayChatClient client(rec);
        EffectContext ctx = make_ctx();
        auto result = run_task_sync<agentengine::result<ChatResponse>>(client.chat(ChatRequest{}, ctx));

        check(!result.has_value(), "(2) a unary .chat_error recording fails via chat()");
        if (!result.has_value()) {
            check(result.error().klass == failure_class::transient, "(2) error klass replays verbatim");
            check(result.error().message == "synthetic provider outage",
                  "(2) error message replays verbatim");
            check(result.error().code == "test.replay_error", "(2) error code replays verbatim");
            check(result.error().native_code == 42, "(2) error native_code replays verbatim");
        }
    }

    // ---- (3) chat() against a STREAMING-mode recording fails closed, never fabricates a response ---
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::streaming;
        rec.chunks.push_back(make_chunk("irrelevant", std::chrono::milliseconds(0), true));
        rec.stream_terminal = "closed";

        ReplayChatClient client(rec);
        EffectContext ctx = make_ctx();
        auto result = run_task_sync<agentengine::result<ChatResponse>>(client.chat(ChatRequest{}, ctx));

        check(!result.has_value(), "(3) chat() against a streaming recording fails, never fabricates");
        if (!result.has_value()) {
            check(result.error().klass == failure_class::contract,
                  "(3) the mode mismatch is reported as failure_class::contract");
            check(result.error().code == "replay_chat_client.mode_mismatch_streaming",
                  "(3) the mismatch carries a stable, explicit error code");
        }
    }

    // ---- (4) a streaming recording with N ordered chunks replays all N via chat_stream() -----------
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::streaming;
        rec.chunks.push_back(make_chunk("Hello", std::chrono::milliseconds(0), false));
        rec.chunks.push_back(make_chunk("World", std::chrono::milliseconds(0), false));
        rec.chunks.push_back(make_chunk("!", std::chrono::milliseconds(0), true));
        rec.stream_terminal = "closed";

        // No real wall-clock wait needed here (deltas are all 0) -- default real SleepFn is fine.
        ReplayChatClient client(rec);
        EffectContext ctx = make_ctx();
        stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);

        std::vector<std::string> received;
        bool saw_final = false;
        drain_stream(s, received, saw_final);

        std::vector<std::string> const expected{"Hello", "World", "!"};
        check(received == expected,
              "(4) all N chunks replay in the SAME order with the SAME content (004 §7 G3's literal "
              "gate: identical chunk boundaries)");
        check(saw_final, "(4) the last chunk's is_final flag replays verbatim");
        check(s.terminal() == quark::ReplyStreamTerminal::Closed,
              "(4) a \"closed\" stream_terminal reaches the success terminal");
    }

    // ---- (5) inter-chunk timing: an injected SleepFn observes the exact recorded deltas ------------
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::streaming;
        rec.chunks.push_back(make_chunk("a", std::chrono::milliseconds(0), false));
        rec.chunks.push_back(make_chunk("b", std::chrono::milliseconds(50), false));
        rec.chunks.push_back(make_chunk("c", std::chrono::milliseconds(125), true));
        rec.stream_terminal = "closed";

        // Written only by the replay worker thread (in delta order, strictly before each push()); read
        // only after the drain loop below observes s.done() == true, which cannot become true until
        // AFTER close() is latched by that same worker thread -- close()'s own terminal-latch CAS is an
        // acq_rel operation the consumer's is_open()/done() acquire-loads synchronize with, so every
        // prior same-thread write (every recorded_deltas.push_back below) is guaranteed visible by the
        // time this test reads the vector. Same reasoning core/stream.hpp's own file banner gives for
        // "credit-return/terminal wake" visibility -- not a new assumption invented for this test.
        std::vector<std::chrono::milliseconds> recorded_deltas;
        ReplayChatClient::SleepFn recording_sleep = [&recorded_deltas](std::chrono::milliseconds d) {
            recorded_deltas.push_back(d);  // deliberately never actually sleeps -- no wall-clock wait
        };

        ReplayChatClient client(rec, ChatClientCapabilities{}, recording_sleep);
        EffectContext ctx = make_ctx();
        stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);

        std::vector<std::string> received;
        bool saw_final = false;
        drain_stream(s, received, saw_final);

        check(s.terminal() == quark::ReplyStreamTerminal::Closed, "(5) the stream still closes cleanly");
        std::vector<std::chrono::milliseconds> const expected_deltas{
            std::chrono::milliseconds(0),   // first chunk: delta from 0
            std::chrono::milliseconds(50),  // 50 - 0
            std::chrono::milliseconds(75),  // 125 - 50
        };
        check(recorded_deltas == expected_deltas,
              "(5) the injected SleepFn observed the exact recorded per-chunk deltas, in order -- proves "
              "real inter-chunk timing is reproduced, not just content/order");
    }

    // ---- (6) stream_terminal -> ae::stream terminal() / fail_error() mapping -----------------------
    {
        // "closed" -> Closed.
        ChatCallRecording rec;
        rec.mode = recording_mode::streaming;
        rec.chunks.push_back(make_chunk("hi", std::chrono::milliseconds(0), true));
        rec.stream_terminal = "closed";

        ReplayChatClient::SleepFn no_op = [](std::chrono::milliseconds) {};
        ReplayChatClient client(rec, ChatClientCapabilities{}, no_op);
        EffectContext ctx = make_ctx();
        stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);
        std::vector<std::string> received;
        bool saw_final = false;
        drain_stream(s, received, saw_final);
        check(s.terminal() == quark::ReplyStreamTerminal::Closed,
              "(6) stream_terminal == \"closed\" produces terminal() == Closed");
    }
    {
        // "failed" -> Failed, carrying the recorded detail text. The ReplayChatClient instance (owner
        // of the recording the detail text is borrowed from -- see replay_chat_client.hpp's own file
        // banner) is kept alive for as long as `s`/`fail_error()` are read, by construction (same
        // block scope).
        ChatCallRecording rec;
        rec.mode = recording_mode::streaming;
        rec.chunks.push_back(make_chunk("partial", std::chrono::milliseconds(0), false));
        rec.stream_terminal = "failed";
        rec.stream_error_detail = "synthetic mid-stream provider failure";

        ReplayChatClient::SleepFn no_op = [](std::chrono::milliseconds) {};
        ReplayChatClient client(rec, ChatClientCapabilities{}, no_op);
        EffectContext ctx = make_ctx();
        stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);
        std::vector<std::string> received;
        bool saw_final = false;
        drain_stream(s, received, saw_final);

        check(received == std::vector<std::string>{"partial"},
              "(6) the one chunk before the failure still replays before the Failed terminal");
        check(s.terminal() == quark::ReplyStreamTerminal::Failed,
              "(6) stream_terminal == \"failed\" produces terminal() == Failed");
        check(s.fail_error().detail == "synthetic mid-stream provider failure",
              "(6) fail_error() carries the recorded stream_error_detail text verbatim");
    }
    {
        // Bonus (cheap alongside the above): "cancelled"/"deadline_exceeded" map onto the matching
        // quark::errc, not a generic catch-all -- proves terminal_to_quark_error's own dispatch table.
        for (auto const& [terminal_str, expected_code] :
             std::vector<std::pair<std::string, quark::errc>>{
                 {"cancelled", quark::errc::cancelled},
                 {"deadline_exceeded", quark::errc::timeout},
                 {"failed", quark::errc::internal},
                 {"some_unrecognized_value", quark::errc::internal},
             }) {
            ChatCallRecording rec;
            rec.mode = recording_mode::streaming;
            rec.stream_terminal = terminal_str;
            rec.stream_error_detail = "detail for " + terminal_str;

            ReplayChatClient::SleepFn no_op = [](std::chrono::milliseconds) {};
            ReplayChatClient client(rec, ChatClientCapabilities{}, no_op);
            EffectContext ctx = make_ctx();
            stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);
            std::vector<std::string> received;
            bool saw_final = false;
            drain_stream(s, received, saw_final);

            check(s.terminal() == quark::ReplyStreamTerminal::Failed,
                  ("(6 bonus) stream_terminal \"" + terminal_str + "\" produces terminal() == Failed").c_str());
            check(s.fail_error().code == expected_code,
                  ("(6 bonus) stream_terminal \"" + terminal_str + "\" maps to the expected quark::errc")
                      .c_str());
        }
    }

    // ---- (7) chat_stream() against a UNARY-mode recording fails immediately, pushes nothing --------
    {
        ChatCallRecording rec;
        rec.mode = recording_mode::unary;
        rec.response = make_full_response();

        ReplayChatClient client(rec);
        EffectContext ctx = make_ctx();
        stream<ChatResponseUpdate> s = client.chat_stream(ChatRequest{}, ctx);

        std::vector<std::string> received;
        bool saw_final = false;
        drain_stream(s, received, saw_final);

        check(received.empty(),
              "(7) chat_stream() against a unary recording never pushes a single item -- contract "
              "mismatch, not silently coerced");
        check(!saw_final, "(7) no is_final chunk is ever observed");
        check(s.terminal() == quark::ReplyStreamTerminal::Failed,
              "(7) the stream fails closed immediately rather than silently returning an empty success");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_replay_chat_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_replay_chat_client: %d FAILURE(S)\n", g_failures);
    return 1;
}
