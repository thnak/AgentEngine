// AgentEngine "get started" examples, 7 -- streaming a chat response token by token.
//
// Mirrors the streaming half of MAF's own samples (every `01-get-started` sample also calls
// `agent.RunStreamingAsync(...)` alongside the plain `RunAsync`). AgentEngine's streaming seam lives
// on `ChatClient` itself, not on `AgentSession`: `chat_stream()`'s literal signature is
// `ae::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&)` (004 §1) -- a plain,
// synchronous call that hands back a drain handle, not a coroutine. `AgentSession::handle()` today
// calls `ChatClient::chat()` (the blocking, whole-response method) internally, so there is no
// session-level "stream a run" ask yet -- this example demonstrates the real, already-shipped layer
// underneath one: a `ChatClient` conformer that genuinely streams, word by word, across a real
// background thread and a real credit-controlled ring (`agentengine::stream<T>`, core/stream.hpp),
// the same primitive `tests/test_chat_client_stream.cpp` proves end to end.
//
// Run: ./agentengine_example_07_streaming

#include <cstdio>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

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

// A real ChatClient conformer that streams for real: `chat_stream()` pushes one ChatResponseUpdate
// per word from a background thread, through `make_stream<T>`'s real credit-controlled ring, and
// closes when done. The ring capacity here (2) is deliberately smaller than the reply's word count,
// so draining genuinely exercises backpressure -- a full ring stalls the producer thread, it never
// drops an item.
class StreamingJokerChatClient {
public:
    ~StreamingJokerChatClient() {
        if (producer_thread_.joinable()) producer_thread_.join();
    }
    StreamingJokerChatClient() = default;
    StreamingJokerChatClient(StreamingJokerChatClient const&) = delete;
    StreamingJokerChatClient& operator=(StreamingJokerChatClient const&) = delete;

    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities caps;
        caps.streaming = true;
        return caps;
    }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        co_return ChatResponse{};  // unused -- this example only drives chat_stream()
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        std::vector<std::string> const words =
            split_words("Why did the pirate take so long to learn the alphabet");

        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 2;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);

        if (producer_thread_.joinable()) producer_thread_.join();
        producer_thread_ = std::thread([producer = std::move(pair.producer), words]() mutable {
            for (std::size_t i = 0; i < words.size(); ++i) {
                ChatResponseUpdate update;
                ContentItem item;
                item.origin = content_origin::assistant;
                item.value  = Text{(i == 0 ? words[i] : " " + words[i])};
                update.delta    = std::move(item);
                update.is_final = (i + 1 == words.size());
                if (producer.push(std::move(update)) != quark::ReplyPush::Ok) return;  // consumer gone
            }
            producer.close();
        });

        return std::move(pair.consumer);
    }

private:
    std::thread producer_thread_;
};
static_assert(ChatClient<StreamingJokerChatClient>);

}  // namespace

int main() {
    StreamingJokerChatClient client;
    EffectContext ctx;
    ctx.principal = Principal{"p-demo", ""};
    ChatRequest req;  // this fake client ignores the request and always streams the same reply

    stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);

    std::string received;
    bool saw_final = false;
    while (!s.done()) {
        while (auto update = s.next()) {
            if (auto const* t = std::get_if<Text>(&update->delta.value)) {
                std::printf("%s", t->text.c_str());
                std::fflush(stdout);
                received += t->text;
            }
            if (update->is_final) saw_final = true;
        }
        if (!s.done()) std::this_thread::yield();  // ring momentarily empty, producer thread still live
    }
    std::printf("\n");

    check(received == "Why did the pirate take so long to learn the alphabet",
          "the words arrived in order, exactly once each, across the thread boundary");
    check(saw_final, "the last update is marked is_final");
    check(s.terminal() == quark::ReplyStreamTerminal::Closed, "the stream reached the success terminal");

    std::fprintf(stderr,
                 g_failures == 0 ? "example_07_streaming: OK\n" : "example_07_streaming: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
