// Milestone 5 Phase G1 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase G /
// decision 8, 004-Model-Provider-Plane.md §6): proves `RecordingChatClient<Inner>`
// (core/recording_chat_client.hpp) end to end against hand-rolled, scripted `ChatClient` conformers --
// no real backend, no network. Mirrors tests/test_resilient_chat_client.cpp's own conformer/check()
// pattern (state behind a shared_ptr so the test's local handle and the wrapper's internal copy of
// Inner observe the same state) and tests/test_chat_client_stream.cpp's own poll-loop drain idiom.
//
// Covers:
//  (1) a successful chat() call is recorded (request/response/duration) AND passed through to the
//      caller unchanged.
//  (2) a FAILED chat() call is recorded too (chat_error, not response) AND the caller still sees the
//      original error unchanged.
//  (3) a chat_stream() call that emits N ordered chunks is recorded with all N chunks in the SAME
//      order, and the caller ALSO receives all N chunks in the same order/content -- recording is
//      transparent, not lossy, doesn't reorder anything.
//  (4) stream_terminal in the recording (and the caller-facing stream's own terminal) matches the
//      inner stream's actual (normal-close) terminal condition.
//
// NOT built into tests/CMakeLists.txt yet (a parallel task is building ReplayChatClient and its own
// test file; the orchestrating session wires both in afterward to avoid a merge conflict).

#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/recording_chat_client.hpp"
#include "agentengine/trust/principal.hpp"

#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// -- (1)/(2): a minimal scripted unary conformer -- one call, success or a fixed failure, chosen by
// `State::should_fail`. State lives behind a shared_ptr for the same reason
// test_resilient_chat_client.cpp's own ScriptedChatClient does: RecordingChatClient<Inner> takes
// Inner BY VALUE and stores its own copy, so the test's local handle and the wrapper's internal copy
// are two distinct objects after construction -- sharing state through a shared_ptr is what lets a
// test configure behavior before the call either way (not needed for observation here, since these
// tests only check the RETURNED result/recording, but kept for consistency and cheap copies).
class ScriptedUnaryChatClient {
public:
    struct State {
        bool should_fail = false;
        agentengine::ChatResponse response;
        agentengine::error failure{agentengine::failure_class::transient, "scripted transient failure",
                                    "test.scripted_failure"};
    };

    explicit ScriptedUnaryChatClient(std::shared_ptr<State> state) : state_(std::move(state)) {}

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }

    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest,
                                                                             agentengine::EffectContext&) {
        if (state_->should_fail) {
            co_return std::unexpected(state_->failure);
        }
        co_return state_->response;
    }

    // Unused by (1)/(2) -- present only to satisfy the ChatClient concept.
    agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest,
                                                                       agentengine::EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedUnaryChatClient>,
              "ScriptedUnaryChatClient must satisfy the real ChatClient concept");
static_assert(agentengine::ChatClient<agentengine::RecordingChatClient<ScriptedUnaryChatClient>>,
              "RecordingChatClient<Inner> must itself satisfy the ChatClient concept (chat_client.hpp)");

// -- (3)/(4): a minimal scripted streaming conformer -- pushes a fixed word list, one
// ChatResponseUpdate per word, from a background thread, then closes normally. State is a shared_ptr
// (not an owned std::thread member, unlike test_chat_client_stream.cpp's StreamingWordChatClient) so
// this type stays copyable/movable -- required to be stored BY VALUE inside RecordingChatClient<Inner>
// (chat_client.hpp/core/recording_chat_client.hpp's own constructor). The background thread is
// detached, matching production backends' own precedent (protocol/openai/chat_client.hpp).
class ScriptedStreamingChatClient {
public:
    struct State {
        std::vector<std::string> words;
    };

    explicit ScriptedStreamingChatClient(std::vector<std::string> words)
        : state_(std::make_shared<State>(State{std::move(words)})) {}

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const {
        agentengine::ChatClientCapabilities caps;
        caps.streaming = true;
        return caps;
    }

    // Unused by (3)/(4) -- present only to satisfy the ChatClient concept.
    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest,
                                                                             agentengine::EffectContext&) {
        co_return agentengine::ChatResponse{};
    }

    agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest,
                                                                       agentengine::EffectContext&) {
        auto pair = agentengine::make_stream<agentengine::ChatResponseUpdate>(
            std::pmr::get_default_resource());
        std::vector<std::string> words = state_->words;  // a fresh copy for this call's thread
        std::thread([producer = std::move(pair.producer), words = std::move(words)]() mutable {
            for (std::size_t i = 0; i < words.size(); ++i) {
                agentengine::ChatResponseUpdate update;
                agentengine::ContentItem item;
                item.origin = agentengine::content_origin::assistant;
                item.value = agentengine::Text{words[i]};
                update.delta = std::move(item);
                update.is_final = (i + 1 == words.size());
                if (producer.push(std::move(update)) != agentengine::stream_push::ok) {
                    return;  // consumer cancelled/deadlined -- stop producing
                }
            }
            producer.close();
        }).detach();
        return std::move(pair.consumer);
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedStreamingChatClient>,
              "ScriptedStreamingChatClient must satisfy the real ChatClient concept");
static_assert(agentengine::ChatClient<agentengine::RecordingChatClient<ScriptedStreamingChatClient>>,
              "RecordingChatClient<Inner> must itself satisfy the ChatClient concept (chat_client.hpp)");

[[nodiscard]] agentengine::EffectContext make_ctx(std::string run_id, std::uint64_t turn_index) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.run_id = std::move(run_id);
    ctx.turn_index = turn_index;
    return ctx;
}

[[nodiscard]] agentengine::ChatRequest request_with_text(std::string text) {
    agentengine::ChatRequest req;
    agentengine::Message msg;
    msg.role = agentengine::role::user;
    agentengine::ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value = agentengine::Text{std::move(text)};
    msg.content.push_back(std::move(item));
    req.messages.push_back(std::move(msg));
    return req;
}

}  // namespace

int main() {
    using namespace agentengine;
    using agentengine::test_support::run_task_sync;

    // ---- (1) a successful chat() call is recorded AND passed through unchanged --------------------
    {
        auto state = std::make_shared<ScriptedUnaryChatClient::State>();
        state->should_fail = false;
        ContentItem reply_item;
        reply_item.origin = content_origin::assistant;
        reply_item.value = Text{"hello from scripted"};
        state->response.message.role = role::assistant;
        state->response.message.content.push_back(reply_item);
        state->response.model = "scripted-model";

        ScriptedUnaryChatClient inner(state);
        std::vector<ChatCallRecording> recordings;
        RecordingChatClient<ScriptedUnaryChatClient> client(
            inner, [&recordings](ChatCallRecording rec) { recordings.push_back(std::move(rec)); });

        EffectContext ctx = make_ctx("run-1", 0);
        ChatRequest req = request_with_text("hello from user");

        auto result = run_task_sync<agentengine::result<ChatResponse>>(client.chat(req, ctx));

        check(result.has_value(), "(1) chat() passes through success unchanged");
        if (result.has_value()) {
            check(result->model == "scripted-model", "(1) passthrough response model matches exactly");
            check(!result->message.content.empty(), "(1) passthrough response has content");
            if (!result->message.content.empty()) {
                auto const* text = std::get_if<Text>(&result->message.content.front().value);
                check(text != nullptr && text->text == "hello from scripted",
                      "(1) passthrough response text matches exactly");
            }
        }

        check(recordings.size() == 1, "(1) exactly one recording captured");
        if (recordings.size() == 1) {
            auto const& rec = recordings.front();
            check(rec.mode == recording_mode::unary, "(1) recorded mode is unary");
            check(rec.response.has_value(), "(1) recorded response is populated");
            check(!rec.chat_error.has_value(), "(1) recorded chat_error is NOT populated on success");
            if (rec.response.has_value()) {
                check(rec.response->model == "scripted-model", "(1) recorded response model matches");
                check(!rec.response->message.content.empty(), "(1) recorded response has content");
            }
            check(rec.request.messages.size() == 1, "(1) recorded request captured the caller's message");
            if (rec.request.messages.size() == 1 && !rec.request.messages.front().content.empty()) {
                auto const* text = std::get_if<Text>(&rec.request.messages.front().content.front().value);
                check(text != nullptr && text->text == "hello from user",
                      "(1) recorded request text matches exactly");
            }
            check(rec.duration.count() >= 0, "(1) recorded duration is non-negative");
        }
    }

    // ---- (2) a FAILED chat() call is recorded too AND the caller still sees the original error ----
    {
        auto state = std::make_shared<ScriptedUnaryChatClient::State>();
        state->should_fail = true;
        state->failure = error{failure_class::transient, "scripted transient failure",
                                "test.scripted_failure"};

        ScriptedUnaryChatClient inner(state);
        std::vector<ChatCallRecording> recordings;
        RecordingChatClient<ScriptedUnaryChatClient> client(
            inner, [&recordings](ChatCallRecording rec) { recordings.push_back(std::move(rec)); });

        EffectContext ctx = make_ctx("run-2", 0);
        ChatRequest req = request_with_text("will fail");

        auto result = run_task_sync<agentengine::result<ChatResponse>>(client.chat(req, ctx));

        check(!result.has_value(), "(2) chat() passes through the failure unchanged");
        if (!result.has_value()) {
            check(result.error().klass == failure_class::transient, "(2) passthrough error klass matches");
            check(result.error().code == "test.scripted_failure", "(2) passthrough error code matches");
            check(result.error().message == "scripted transient failure",
                  "(2) passthrough error message matches");
        }

        check(recordings.size() == 1, "(2) exactly one recording captured for the failed call");
        if (recordings.size() == 1) {
            auto const& rec = recordings.front();
            check(rec.mode == recording_mode::unary, "(2) recorded mode is unary");
            check(!rec.response.has_value(), "(2) recorded response is NOT populated on failure");
            check(rec.chat_error.has_value(), "(2) recorded chat_error IS populated on failure");
            if (rec.chat_error.has_value()) {
                check(rec.chat_error->klass == failure_class::transient,
                      "(2) recorded chat_error klass matches");
                check(rec.chat_error->code == "test.scripted_failure",
                      "(2) recorded chat_error code matches");
            }
            check(rec.duration.count() >= 0, "(2) recorded duration is non-negative");
        }
    }

    // ---- (3)/(4) a streamed call: N ordered chunks, recorded transparently, terminal mirrored ------
    {
        std::vector<std::string> const words{"the", "quick", "brown", "fox", "jumps"};
        ScriptedStreamingChatClient inner(words);
        std::vector<ChatCallRecording> recordings;
        RecordingChatClient<ScriptedStreamingChatClient> client(
            inner, [&recordings](ChatCallRecording rec) { recordings.push_back(std::move(rec)); });

        EffectContext ctx = make_ctx("run-3", 0);
        ChatRequest req = request_with_text("stream please");

        stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);

        // The exact poll-loop idiom this codebase already established
        // (tests/test_chat_client_stream.cpp ~148-198) -- next() is poll-only, never blocking.
        std::vector<std::string> received;
        bool saw_final = false;
        while (!s.done()) {
            while (auto update = s.next()) {
                auto const* text = std::get_if<Text>(&update->delta.value);
                check(text != nullptr, "(3) every delivered update carries a Text delta");
                if (text) received.push_back(text->text);
                if (update->is_final) saw_final = true;
            }
            if (!s.done()) std::this_thread::yield();
        }

        check(received == words,
              "(3) the caller receives all N chunks in the SAME order/content Inner produced");
        check(saw_final, "(3) the last delivered chunk is marked is_final");
        check(s.terminal() == stream_terminal::closed,
              "(4) the caller-facing stream's own terminal mirrors Inner's normal close");

        // s.done() == true is exactly the release/acquire synchronization point that also guarantees
        // the recording thread's sink(...) call (which happens-before this wrapper's own producer
        // latches its terminal -- core/recording_chat_client.hpp's own ordering comment) is already
        // visible here -- no extra synchronization needed to inspect `recordings` below.
        check(recordings.size() == 1, "(3)/(4) exactly one recording captured for the streamed call");
        if (recordings.size() == 1) {
            auto const& rec = recordings.front();
            check(rec.mode == recording_mode::streaming, "(3) recorded mode is streaming");
            check(rec.chunks.size() == words.size(), "(3) all N chunks were recorded");
            std::vector<std::string> recorded_words;
            for (auto const& chunk : rec.chunks) {
                auto const* text = std::get_if<Text>(&chunk.update.delta.value);
                if (text) recorded_words.push_back(text->text);
            }
            check(recorded_words == words,
                  "(3) recorded chunks are in the SAME order as delivered -- recording is transparent, "
                  "not lossy, and doesn't reorder anything");
            check(rec.stream_terminal == "closed",
                  "(4) stream_terminal in the recording matches the inner stream's actual (normal-close) "
                  "terminal condition");
            check(rec.duration.count() >= 0, "(3) recorded duration is non-negative");
            check(rec.request.messages.size() == 1,
                  "(3) recorded request captured the caller's streamed-call message");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_recording_chat_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_recording_chat_client: %d FAILURE(S)\n", g_failures);
    return 1;
}
