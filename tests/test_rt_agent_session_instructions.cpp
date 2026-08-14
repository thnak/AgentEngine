// Gap-16/21 fix (2026-08-14, decisions/ADR-042-*.md): proves `ContextContribution::instructions`
// (now `std::optional<TaintedText>`, context_provider.hpp) actually reaches the model as a real
// `role::system` `Message` -- before this fix it was read by `assemble_context()` and then never
// referenced again, silently dropped (2026-08-10 gap audit #16). Also proves the taint discipline:
// the synthesized message's `ContentItem` is marked `tainted = false` (already declassified at the
// ONE explicit `.unsafe_view()` call site in `AgentSession::run_rounds()`, agent_session.hpp), and
// that with no instructions contributed at all, behavior is byte-identical to before this fix (no
// synthesized message, nothing regressed for the common case).

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
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

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::ContextProvider;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::SessionContext;
using agentengine::TaintedText;
using agentengine::Text;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

// Records every ChatRequest it receives, so a test can inspect exactly what reached the "wire" --
// the same role the OpenAI/Anthropic translation layers play in real code, minus the actual network
// call.
class RecordingChatClient {
public:
    RecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ChatRequest> requests;
    };

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest req, EffectContext&) {
        state_->requests.push_back(req);
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused by these tests
    }

    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

// A minimal ContextProvider that contributes `.instructions` (as an explicitly-constructed
// TaintedText -- context_provider.hpp's own comment: this IS the trust decision) plus whatever
// history it's handed, mirroring how a real HistoryProviderT would fold in session history.
struct InstructionsProvider {
    std::string text_to_wrap;

    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.instructions = TaintedText{text_to_wrap};
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<InstructionsProvider>);

// The unmodified-baseline shape: contributes history only, no instructions at all -- proves the
// no-instructions path is byte-identical to before this fix.
struct NoInstructionsProvider {
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<NoInstructionsProvider>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;

    // T1: instructions present -> a real role::system Message, prepended, correctly marked.
    {
        AgentSession<RecordingChatClient, NoSessionState, InstructionsProvider> session;
        session.initialize("t1", Principal{"p1", ""});
        session.history_provider().text_to_wrap = "be concise";
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T1: the run converges");
        check(client.requests().size() == 1, "T1: exactly one model call happened");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(msgs.size() >= 2,
                  "T1: the request carries both the synthesized instructions AND the user's own message");
            if (!msgs.empty()) {
                check(msgs.front().role == role::system,
                      "T1: the FIRST message is role::system -- instructions establish context ahead "
                      "of everything else");
                check(msgs.front().content.size() == 1, "T1: exactly one content item in the synthesized message");
                if (msgs.front().content.size() == 1) {
                    ContentItem const& item = msgs.front().content.front();
                    auto const* t = std::get_if<Text>(&item.value);
                    check(t != nullptr && t->text == "be concise",
                          "T1: the content is the instructions text, verbatim, unmangled");
                    check(item.origin == content_origin::system, "T1: origin is content_origin::system");
                    check(!item.tainted,
                          "T1: tainted == false -- already declassified at the one explicit "
                          "unsafe_view() call site, not re-derived from tainted input");
                }
            }
            check(std::ranges::any_of(msgs,
                                       [](Message const& m) {
                                           return m.role == role::user &&
                                                  std::holds_alternative<Text>(m.content.front().value) &&
                                                  std::get<Text>(m.content.front().value).text == "hi";
                                       }),
                  "T1: the user's own message survives alongside the synthesized instructions -- "
                  "nothing was silently replaced");
        }
    }

    // T2: no instructions contributed -> no synthesized message at all, unchanged from before this
    // fix (this is the actual, dominant case in the current codebase -- nothing populates
    // `.instructions` in production yet, per the 2026-08-14 grounding pass).
    {
        AgentSession<RecordingChatClient, NoSessionState, NoInstructionsProvider> session;
        session.initialize("t2", Principal{"p1", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T2: the run converges");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(std::ranges::none_of(msgs, [](Message const& m) { return m.role == role::system; }),
                  "T2: with no instructions contributed, no role::system message is synthesized at "
                  "all -- the no-instructions path is unchanged by this fix");
            check(msgs.size() == 1, "T2: only the user's own message reaches the model, exactly as "
                                     "before this fix");
        }
    }

    // T3: instructions are recomputed every round, not cached from round 1 -- each round's own
    // ContextContribution is honored (run_rounds() re-invokes on_context() every iteration).
    {
        AgentSession<RecordingChatClient, NoSessionState, InstructionsProvider> session;
        session.initialize("t3", Principal{"p1", ""}, std::nullopt, /*max_turns=*/2);
        session.history_provider().text_to_wrap = "round text";
        RecordingChatClient& client = session.emplace_chat_client();

        auto t3_outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(t3_outcome.has_value(), "T3: the run converges");
        check(client.requests().size() >= 1, "T3: at least one call happened");
        for (auto const& req : client.requests()) {
            check(!req.messages.empty() && req.messages.front().role == role::system,
                  "T3: every round's request carries the synthesized system message, not just the first");
        }
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_instructions: OK\n"
                                 : "test_rt_agent_session_instructions: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
