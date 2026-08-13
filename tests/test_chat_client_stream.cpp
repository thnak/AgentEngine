// Milestone 5 Phase B4b (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, decision
// 3): proves `ae::stream<T>` (core/stream.hpp) end to end against a real `ChatClient` conformer whose
// `chat_stream()` streams for real -- a background thread pushes items through a real credit-controlled
// ring while the test drains concurrently on the main thread, exactly the shape a live HTTP/SSE backend
// (Phase D/E, not yet built) will use. Not a mock of the mechanism: the SAME `quark::ReplyStream`/
// `ReplyStreamProducer` primitives ADR-018 proved, reached through `core/stream.hpp`'s thin adapter.
//
// MACHINE SAFETY (CLAUDE.md): exactly one producer thread + the main test thread per stream (SPSC,
// mirroring Quark's own reply_stream_concurrency_test.cpp) -- never more.

#include <cstdio>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

[[nodiscard]] std::vector<std::string> split_words(std::string const& text) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : text) {
        if (c == ' ') {
            if (!cur.empty()) words.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

// A real ChatClient conformer that streams for real: `chat_stream()` splits the last user message into
// words and pushes one ChatResponseUpdate per word from a background thread, closing when done. The
// ring capacity is deliberately smaller than most word counts below, so draining genuinely exercises
// the credit-controlled backpressure path (a full ring stalls the producer thread; it never drops).
class StreamingWordChatClient {
public:
    explicit StreamingWordChatClient(std::uint32_t ring_capacity) : ring_capacity_(ring_capacity) {}

    ~StreamingWordChatClient() {
        if (producer_thread_.joinable()) producer_thread_.join();
    }

    StreamingWordChatClient(StreamingWordChatClient const&) = delete;
    StreamingWordChatClient& operator=(StreamingWordChatClient const&) = delete;

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const {
        agentengine::ChatClientCapabilities caps;
        caps.streaming = true;
        return caps;
    }

    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(
        agentengine::ChatRequest const&, agentengine::EffectContext&) {
        co_return agentengine::ChatResponse{};
    }

    agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(
        agentengine::ChatRequest const& request, agentengine::EffectContext&) {
        std::vector<std::string> words;
        if (!request.messages.empty()) {
            auto const& content = request.messages.back().content;
            if (!content.empty()) {
                if (auto const* text = std::get_if<agentengine::Text>(&content.front().value)) {
                    words = split_words(text->text);
                }
            }
        }

        agentengine::stream_config<agentengine::ChatResponseUpdate> cfg;
        cfg.capacity = ring_capacity_;
        auto pair = agentengine::make_stream<agentengine::ChatResponseUpdate>(
            std::pmr::get_default_resource(), cfg);

        if (producer_thread_.joinable()) producer_thread_.join();  // a prior call's thread, if any
        producer_thread_ = std::thread([producer = std::move(pair.producer),
                                         words = std::move(words)]() mutable {
            for (std::size_t i = 0; i < words.size(); ++i) {
                agentengine::ChatResponseUpdate update;
                agentengine::ContentItem item;
                item.origin = agentengine::content_origin::assistant;
                item.value  = agentengine::Text{words[i]};
                update.delta    = std::move(item);
                update.is_final = (i + 1 == words.size());
                if (producer.push(std::move(update)) != agentengine::stream_push::ok) {
                    return;  // consumer cancelled/deadlined -- stop producing, no further pushes
                }
            }
            producer.close();
        });

        return std::move(pair.consumer);
    }

private:
    std::uint32_t ring_capacity_;
    std::thread producer_thread_;
};
static_assert(agentengine::ChatClient<StreamingWordChatClient>,
              "StreamingWordChatClient must satisfy the real ChatClient concept (004 §1), including "
              "chat_stream()'s literal ae::stream<ChatResponseUpdate> return type (Phase B4b)");

[[nodiscard]] agentengine::ChatRequest request_with_text(std::string text) {
    agentengine::ChatRequest req;
    agentengine::Message msg;
    msg.role = agentengine::role::user;
    agentengine::ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value  = agentengine::Text{std::move(text)};
    msg.content.push_back(std::move(item));
    req.messages.push_back(std::move(msg));
    return req;
}

}  // namespace

int main() {
    using namespace agentengine;

    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};

    // ---- B4b-R1: FIFO, lossless delivery under real cross-thread backpressure ---------------------
    // Ring capacity (2) is smaller than the word count (9), so the producer thread genuinely stalls on
    // credit at least once -- the SAME ADR-018 reverse-Dekker wake this test exercises through no mock.
    {
        StreamingWordChatClient client(/*ring_capacity=*/2);
        ChatRequest req = request_with_text("the quick brown fox jumps over the lazy dog");

        stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);

        std::vector<std::string> received;
        bool saw_final = false;
        while (!s.done()) {
            while (auto update = s.next()) {
                auto const* text = std::get_if<Text>(&update->delta.value);
                check(text != nullptr, "B4b-R1: every delivered update carries a Text delta");
                if (text) received.push_back(text->text);
                if (update->is_final) saw_final = true;
            }
            if (!s.done()) std::this_thread::yield();  // ring momentarily empty, producer still live
        }

        std::vector<std::string> const expected{"the", "quick", "brown", "fox",  "jumps",
                                                  "over", "the",   "lazy",  "dog"};
        check(received == expected,
              "B4b-R1: words are delivered exactly once, in FIFO order, across the thread boundary "
              "(0 loss, 0 duplication, 0 reorder -- ADR-018's own proven properties, reached through "
              "ae::stream<T>)");
        check(saw_final, "B4b-R1: the last chunk is marked is_final");
        check(s.terminal() == stream_terminal::closed,
              "B4b-R1: the stream reaches the success terminal (in-band EoS)");
        check(!s.gap_detected(), "B4b-R1: no gap in the delivered sequence");
    }

    // ---- B4b-R2: an empty message streams zero items and closes cleanly ---------------------------
    {
        StreamingWordChatClient client(/*ring_capacity=*/4);
        ChatRequest req = request_with_text("");
        stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);
        while (!s.done()) {
            while (auto update = s.next()) { (void)update; check(false, "B4b-R2: no item should be delivered"); }
            if (!s.done()) std::this_thread::yield();
        }
        check(s.terminal() == stream_terminal::closed, "B4b-R2: an empty stream still closes cleanly");
    }

    // ---- B4b-R3: dropping the consumer mid-stream cancels it; the producer thread stops and joins --
    // (004 §7 G2's own text: cancellation mid-stream releases resources within a bounded time, no
    // orphaned state -- proven here at the ae::stream<T> layer; the socket-level proof is Phase D/E's.)
    {
        StreamingWordChatClient client(/*ring_capacity=*/1);
        ChatRequest req = request_with_text("one two three four five six seven eight nine ten");
        {
            stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);
            // next() is poll-only (it does not wait) -- the producer thread races the very first call,
            // so poll until it has pushed at least one item, then let `s` go out of scope.
            std::optional<ChatResponseUpdate> first;
            while (!first && !s.done()) {
                first = s.next();
                if (!first) std::this_thread::yield();
            }
            check(first.has_value(), "B4b-R3: at least the first item is delivered before cancellation");
            // ~stream() below cancels the ring -- the producer thread's next push() must observe
            // Terminated and return, rather than hang forever waiting for credit that will never come.
        }
        // StreamingWordChatClient's own destructor joins producer_thread_ -- reaching this line at all
        // (rather than hanging in the destructor) IS the proof that cancellation woke a stalled producer.
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_stream: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_stream: %d FAILURE(S)\n", g_failures);
    return 1;
}
