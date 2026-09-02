// AgentEngine "get started" examples, 32 -- a real interactive-shaped session loop driven directly
// against a raw `ChatClient::chat_stream()`, with no `AgentSession` and no `WorkflowChatClient` in
// between.
//
// Fills a gap between two existing examples: 07_streaming.cpp proves `chat_stream()` genuinely
// streams token by token, but only for ONE call against a fake client that ignores the request;
// tools/cli_chat.cpp has a real multi-turn `while (true)` session loop with live text as it
// arrives, but that streaming is `AgentSession`'s own RunEvent/`model_delta` projection (013 §1),
// never a direct `chat_stream()` call. This example is the layer underneath both: one
// `ChatClient` conformer, reused (not reconstructed) across every turn, driven by a real, bounded
// session loop that grows a real message history and prints each reply live as its words arrive --
// the same shape a real interactive REPL's turn-loop body would have if it talked to `chat_stream()`
// directly instead of going through a session.
//
// Bounded rather than a literal unbounded `while (true)` -- this codebase's own established
// convention (see the Builder API page's "bounded, single-resume() loop" note, and example 31's own
// session loop). A real interactive CLI reads its next turn from stdin instead of a canned queue;
// nothing about the loop shape below depends on where the next message text comes from.
//
// Fully offline -- no live model needed, matching examples 07/10/13/14/20/21/28/31's own style.
//
// Run: ./agentengine_example_32_chat_stream_session_loop

#include <cstdio>
#include <deque>
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

[[nodiscard]] Message text_message(std::string text, role r) {
    ContentItem item{};
    item.origin = (r == role::user) ? content_origin::user : content_origin::assistant;
    item.value = Text{std::move(text)};
    Message m{};
    m.role = r;
    m.content.push_back(std::move(item));
    return m;
}

// A real ChatClient conformer that streams for real -- same producer-thread-plus-credit-controlled-
// ring shape as 07_streaming.cpp's own StreamingJokerChatClient, generalized to answer a DIFFERENT
// canned reply on each successive call (round-robin through `replies_`) instead of always the same
// one, and safely reusable across many calls from one long-lived instance -- exactly how a real
// session loop holds ONE client and calls chat_stream() on it repeatedly, never reconstructing it
// per turn.
class StreamingScriptedChatClient {
public:
    explicit StreamingScriptedChatClient(std::vector<std::string> replies)
        : replies_(std::move(replies)) {}
    ~StreamingScriptedChatClient() {
        if (producer_thread_.joinable()) producer_thread_.join();
    }
    StreamingScriptedChatClient(StreamingScriptedChatClient const&) = delete;
    StreamingScriptedChatClient& operator=(StreamingScriptedChatClient const&) = delete;

    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities caps;
        caps.streaming = true;
        return caps;
    }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        co_return ChatResponse{};  // unused -- this example only drives chat_stream()
    }

    // Ignores `request.messages` beyond its own turn counter -- a real conformer reads the whole
    // history to produce a real reply; this fake one only needs to prove the SESSION LOOP is real,
    // not that the reply is contextually relevant (07_streaming.cpp's own fake client makes the same
    // simplification).
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        check(turn_ < replies_.size(), "chat_stream() called fewer times than there are scripted replies");
        std::vector<std::string> const words =
            split_words(turn_ < replies_.size() ? replies_[turn_] : std::string{"..."});
        ++turn_;

        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 2;  // deliberately small -- draining genuinely exercises backpressure
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);

        // Join any PRIOR call's producer thread before starting a new one -- this client is called
        // once per loop iteration from the same thread, never concurrently, but a long-lived
        // instance must still not leak or race a previous call's still-joinable thread.
        if (producer_thread_.joinable()) producer_thread_.join();
        producer_thread_ = std::thread([producer = std::move(pair.producer), words]() mutable {
            for (std::size_t i = 0; i < words.size(); ++i) {
                ChatResponseUpdate update;
                ContentItem item;
                item.origin = content_origin::assistant;
                item.value = Text{(i == 0 ? words[i] : " " + words[i])};
                update.delta = std::move(item);
                update.is_final = (i + 1 == words.size());
                if (producer.push(std::move(update)) != stream_push::ok) return;  // consumer gone
            }
            producer.close();
        });

        return std::move(pair.consumer);
    }

private:
    std::vector<std::string> replies_;
    std::size_t turn_ = 0;
    std::thread producer_thread_;
};
static_assert(ChatClient<StreamingScriptedChatClient>);

}  // namespace

int main() {
    // One long-lived client, called once per loop iteration -- never reconstructed per turn, the
    // same reuse a real interactive session holds its ChatClient across every turn.
    StreamingScriptedChatClient client({
        "I can chat with you, one word at a time, for as long as this loop keeps going.",
        "Water boils at a lower temperature at higher altitude.",
        "Goodbye! Ending the session now.",
    });
    EffectContext ctx;
    ctx.principal = Principal{"p-demo", ""};

    // The "human" side -- a canned, deterministic queue rather than real stdin, so this example
    // stays reproducible (a real CLI reads its next line from stdin instead; nothing about the loop
    // below depends on where this text comes from). tools/cli_chat.cpp's own while(true) loop reads
    // real stdin for the exact same role this queue plays here.
    std::deque<std::string> user_turns = {"Hi, what can you do?", "Tell me a short fact.", "bye"};

    std::vector<Message> history;
    std::vector<std::string> received_replies;

    // The session loop itself: as long as there's a next user turn, send it with the growing
    // history, stream the reply live (word by word, printed as it arrives -- not assembled and
    // printed after the fact), fold it into history, and go again. Bounded at one iteration per
    // scripted turn, so a bug that kept the queue non-empty forever fails loudly here rather than
    // hanging the example.
    constexpr int kMaxRounds = 8;
    for (int round = 0; round < kMaxRounds && !user_turns.empty(); ++round) {
        std::string const user_text = user_turns.front();
        user_turns.pop_front();
        std::printf("You: %s\n", user_text.c_str());
        history.push_back(text_message(user_text, role::user));

        ChatRequest req;
        req.messages = history;
        stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);

        std::printf("Assistant: ");
        std::string reply;
        bool saw_final = false;
        while (!s.done()) {
            while (auto update = s.next()) {
                if (auto const* t = std::get_if<Text>(&update->delta.value)) {
                    std::printf("%s", t->text.c_str());
                    std::fflush(stdout);
                    reply += t->text;
                }
                if (update->is_final) saw_final = true;
            }
            if (!s.done()) std::this_thread::yield();  // ring momentarily empty, producer still live
        }
        std::printf("\n");

        check(saw_final, "each turn's stream reaches a final update");
        check(s.terminal() == stream_terminal::closed, "each turn's stream reaches the success terminal");

        history.push_back(text_message(reply, role::assistant));
        received_replies.push_back(std::move(reply));
    }

    check(user_turns.empty(), "the session loop consumed every scripted turn, not just some of them");
    check(received_replies.size() == 3, "exactly three turns actually ran");
    check(history.size() == 6, "history grew by one user + one assistant message per turn");
    if (received_replies.size() == 3) {
        check(received_replies[0] == "I can chat with you, one word at a time, for as long as this loop keeps going.",
              "turn 1's streamed reply arrived intact, word boundaries reconstructed exactly");
        check(received_replies[1] == "Water boils at a lower temperature at higher altitude.",
              "turn 2's streamed reply arrived intact");
        check(received_replies[2] == "Goodbye! Ending the session now.",
              "turn 3's streamed reply arrived intact");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_32_chat_stream_session_loop: OK\n"
                                          : "example_32_chat_stream_session_loop: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
