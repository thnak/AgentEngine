// ADR-199 §8 (red team MAJOR, "dangling TurnView"): a regression test for the fix, which the red team
// proved with a throwaway probe that was never checked in.
//
// ContextProvider allows `on_turn_end(TurnView const&, EffectContext&)`. Its task is lazy: the body runs
// only when AgentSessionCore co_awaits it, after `bound_on_turn_end()` has returned. So the reference must
// point at something that outlives that call. The fix makes the hook take `TurnView const&`, so it binds
// to the loop's own temporary, which lives until the co_await completes. With the hook taking `TurnView`
// by value, the reference points at the hook's parameter, gone before the body runs: g++-14 read garbage
// at -O2 and ASan reported stack-use-after-return. MSVC hid it (the parameter lived in the caller's
// coroutine frame), so this test fails on the Linux leg if the fix is reverted, not on Windows.
//
// TV1 -- a plain text answer: the provider sees the one response message, with its text.
// TV2 -- a tool round and then an answer: the turn that ends with the tool results sees the response and
//        the results message; the next sees the final answer.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;
using agentengine::task;

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

// Every fixture below co_returns without suspending, as in every other test_rt_agent_session*.cpp.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Usage;
using agentengine::role;

Message text_message(role r, std::string text) {
    Message m;
    m.role = r;
    m.content.push_back(agentengine::ContentItem{agentengine::Text{std::move(text)}});
    return m;
}

std::string joined_text(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<agentengine::Text>(&item.value)) out += t->text;
    }
    return out;
}

class ScriptedChatClient {
public:
    ScriptedChatClient() : script_(std::make_shared<std::vector<Message>>()) {}
    void set_script(std::vector<Message> script) { *script_ = std::move(script); }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = next_ < script_->size() ? next_ : script_->size() - 1;
        ++next_;
        co_return ChatResponse{(*script_)[idx], Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<std::vector<Message>> script_;
    std::size_t next_ = 0;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

// Stands in for work a real provider does before it reads the view, such as awaiting a summarizer: it
// reuses stack below the caller. Called through a volatile pointer so it is never inlined away. With the
// fix reverted, the dead hook parameter sits in that region and is overwritten; with the fix, the view
// lives in AgentSessionCore's coroutine frame and is untouched.
void scribble_stack() {
    volatile unsigned char buf[16384];
    for (auto& b : buf) b = 0xA5;
}
void (*volatile g_scribble)() = &scribble_stack;

// What one on_turn_end body read from its TurnView.
struct SeenTurn {
    std::size_t              message_count = 0;
    std::vector<std::string> texts;
    std::vector<role>        roles;
};

// Takes the view by const reference, the shape the red team's probe used. The body first does unrelated
// work on the stack, then copies what it sees into `seen`, so the test reads nothing through the view itself.
struct ConstRefTurnEndProvider {
    static constexpr std::string_view name = "const-ref-turn-end";

    std::shared_ptr<std::vector<SeenTurn>> seen = std::make_shared<std::vector<SeenTurn>>();

    task<agentengine::result<agentengine::ContextContribution>> on_context(agentengine::SessionContext&,
                                                                          EffectContext&) {
        co_return agentengine::ContextContribution{};
    }

    task<std::monostate> on_turn_end(agentengine::TurnView const& turn, EffectContext&) {
        g_scribble();
        SeenTurn s;
        s.message_count = turn.turn_messages.size();
        // A dangling view's size is whatever the stack now holds; only a plausible one is walked, so a
        // revert fails the count checks below instead of crashing on the elements.
        if (s.message_count > 16) {
            seen->push_back(std::move(s));
            co_return std::monostate{};
        }
        for (Message const& m : turn.turn_messages) {
            s.texts.push_back(joined_text(m));
            s.roles.push_back(m.role);
        }
        seen->push_back(std::move(s));
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<ConstRefTurnEndProvider>);

using Session = AgentSession<ScriptedChatClient, agentengine::rt::NoSessionState, ConstRefTurnEndProvider>;

void tv1_text_answer() {
    Session session;
    session.emplace_chat_client().set_script({text_message(role::assistant, "the answer is forty-two")});
    auto const seen = session.history_provider().seen;

    auto r = drive(session.start_run(StartRun{text_message(role::user, "what is the answer?")}));
    check(r.has_value(), "TV1: the run completes");
    check(seen->size() == 1, "TV1: on_turn_end ran once");
    if (seen->size() != 1) return;
    SeenTurn const& t = seen->front();
    check(t.message_count == 1, "TV1: the view holds exactly the response message");
    check(t.message_count == 1 && t.roles[0] == role::assistant, "TV1: ... and it is the assistant's");
    check(t.message_count == 1 && t.texts[0] == "the answer is forty-two",
          "TV1: ... with the response's own text, read after bound_on_turn_end() returned");
}

void tv2_tool_round() {
    Session session;
    Message call = text_message(role::assistant, "calling the tool");
    call.content.push_back(agentengine::ContentItem{agentengine::ToolCall{"call-1", "no_such_tool", "{}"}});
    session.emplace_chat_client().set_script({call, text_message(role::assistant, "done after the tool")});
    auto const seen = session.history_provider().seen;

    auto r = drive(session.start_run(StartRun{text_message(role::user, "use the tool")}));
    check(r.has_value(), "TV2: the run completes");
    check(seen->size() == 2, "TV2: on_turn_end ran twice, once per model response");
    if (seen->size() != 2) return;
    SeenTurn const& first = (*seen)[0];
    check(first.message_count == 2, "TV2: the tool turn's view holds the response and the tool results");
    check(first.message_count == 2 && first.texts[0] == "calling the tool" && first.roles[0] == role::assistant,
          "TV2: ... the first is the response that called the tool, text intact");
    check(first.message_count == 2 && first.roles[1] == role::tool, "TV2: ... the second is the tool results");
    SeenTurn const& second = (*seen)[1];
    check(second.message_count == 1 && second.texts[0] == "done after the tool",
          "TV2: the final turn's view holds the final answer, text intact");
}

}  // namespace

int main() {
    tv1_text_answer();
    tv2_tool_round();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_rt_agent_session_turn_end_view_lifetime: %d FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_rt_agent_session_turn_end_view_lifetime: ALL PASS\n");
    return 0;
}
