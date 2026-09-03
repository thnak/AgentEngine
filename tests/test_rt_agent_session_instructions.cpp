// Gap-16/21 fix (2026-08-14, decisions/ADR-042-*.md): proves `ContextContribution::instructions`
// (now `std::optional<TaintedText>`, context_provider.hpp) actually reaches the model as a real
// `role::system` `Message` -- before this fix it was read by `assemble_context()` and then never
// referenced again, silently dropped (2026-08-10 gap audit #16). Also proves the taint discipline:
// the synthesized message's `ContentItem` is marked `tainted = false` (already declassified at the
// ONE explicit `.unsafe_view()` call site in `AgentSession::run_rounds()`, agent_session.hpp), and
// that with no instructions contributed at all, behavior is byte-identical to before this fix (no
// synthesized message, nothing regressed for the common case).
//
// 2026-08-23 addition (T4-T7): docs/planning/agent-spawn-runtime-design-draft.md item 6 / §4.6,
// OpenQuestions.md OQ-16. Proves `AgentSession::set_static_instructions()` (new, additive) produces
// its own, independent, unconditionally-untainted `role::system` message -- coexisting with (never
// replacing) `contribution->instructions`' own materialization above -- and, using the REAL
// `trust::push_side_summary()` (trust/agent_library_manifest.hpp), that this is the exact mechanism
// `core/session_builder.hpp`'s `build()` now wires for real: a session constructed from a
// `CapabilitySet` holding `cap::AgentCall` gets "agent.spawn" text materialized onto the wire; one
// without does not (I2 -- nothing here widens what a session was actually granted, this only reports
// it back).

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/agent_library_manifest.hpp"

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
using agentengine::CapabilitySet;
using agentengine::capability_from_kind;
using agentengine::capability_kind;
using agentengine::trust::push_side_summary;

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

    // T4: set_static_instructions() alone (no ContextProvider .instructions contributed) -> exactly
    // one role::system message, unconditionally untainted, containing the exact text set.
    {
        AgentSession<RecordingChatClient, NoSessionState, NoInstructionsProvider> session;
        session.initialize("t4", Principal{"p1", ""});
        session.set_static_instructions("static text");
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T4: the run converges");
        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            auto system_count =
                std::ranges::count_if(msgs, [](Message const& m) { return m.role == role::system; });
            check(system_count == 1, "T4: exactly one synthesized role::system message");
            auto it = std::ranges::find_if(msgs, [](Message const& m) { return m.role == role::system; });
            if (it != msgs.end() && !it->content.empty()) {
                auto const* t = std::get_if<Text>(&it->content.front().value);
                check(t != nullptr && t->text == "static text",
                      "T4: the message carries set_static_instructions()'s own text, verbatim");
                check(!it->content.front().tainted,
                      "T4: tainted == false -- host/engine-derived, never model output (I3)");
            }
        }
    }

    // T5: BOTH contribution->instructions AND set_static_instructions() are populated -> two
    // independent role::system messages coexist, in order (contribution's own materialization first,
    // matching the "already established, already safe" precedent named in agent_session.hpp).
    {
        AgentSession<RecordingChatClient, NoSessionState, InstructionsProvider> session;
        session.initialize("t5", Principal{"p1", ""});
        session.history_provider().text_to_wrap = "from provider";
        session.set_static_instructions("from static");
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T5: the run converges");
        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            std::vector<Message const*> system_msgs;
            for (auto const& m : msgs) {
                if (m.role == role::system) system_msgs.push_back(&m);
            }
            check(system_msgs.size() == 2, "T5: two independent role::system messages, neither replaces the other");
            if (system_msgs.size() == 2) {
                auto const* t0 = std::get_if<Text>(&system_msgs[0]->content.front().value);
                auto const* t1 = std::get_if<Text>(&system_msgs[1]->content.front().value);
                check(t0 != nullptr && t0->text == "from provider",
                      "T5: the FIRST role::system message is the ContextProvider's own contribution");
                check(t1 != nullptr && t1->text == "from static",
                      "T5: the SECOND role::system message is set_static_instructions()'s own text");
            }
        }
    }

    // T6/T7: docs/planning/agent-spawn-runtime-design-draft.md §4.6 -- the REAL wiring
    // core/session_builder.hpp's build() now performs, reproduced directly against a real
    // CapabilitySet (no HTTPS-gated QuickstartSessionBuilder needed to prove the mechanism itself):
    // `session.set_static_instructions(trust::push_side_summary(capabilities))`.
    {
        // T6: a CapabilitySet holding cap::AgentCall -> "agent.spawn" text reaches the wire.
        CapabilitySet granted_with_spawn =
            CapabilitySet::grant_root({capability_from_kind(capability_kind::agent_call)});
        AgentSession<RecordingChatClient, NoSessionState, NoInstructionsProvider> session;
        session.initialize("t6", Principal{"p1", ""});
        session.set_static_instructions(push_side_summary(granted_with_spawn));
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T6: the run converges");
        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            auto it = std::ranges::find_if(msgs, [](Message const& m) { return m.role == role::system; });
            check(it != msgs.end(), "T6: a role::system message was synthesized (agent_call grants "
                                     "spawn, so push_side_summary() is non-empty)");
            if (it != msgs.end() && !it->content.empty()) {
                auto const* t = std::get_if<Text>(&it->content.front().value);
                check(t != nullptr && t->text.find("agent.spawn") != std::string::npos,
                      "T6: a session built with an agent_call capability actually has \"agent.spawn\" "
                      "text in its instructions");
            }
        }

        // T7 (negative control, same mechanism): a CapabilitySet WITHOUT cap::AgentCall -> no
        // "agent.spawn" text anywhere on the wire, even though other zero-capability modules
        // (output/progress) still legitimately produce a non-empty summary.
        CapabilitySet granted_without_spawn = CapabilitySet::grant_root({});
        AgentSession<RecordingChatClient, NoSessionState, NoInstructionsProvider> session2;
        session2.initialize("t7", Principal{"p1", ""});
        session2.set_static_instructions(push_side_summary(granted_without_spawn));
        RecordingChatClient& client2 = session2.emplace_chat_client();

        auto outcome2 = drive(session2.start_run(StartRun{user_message("hi")}));
        check(outcome2.has_value(), "T7: the run converges");
        if (!client2.requests().empty()) {
            auto const& msgs = client2.requests().front().messages;
            bool found_spawn = std::ranges::any_of(msgs, [](Message const& m) {
                if (m.content.empty()) return false;
                auto const* t = std::get_if<Text>(&m.content.front().value);
                return t != nullptr && t->text.find("agent.spawn") != std::string::npos;
            });
            check(!found_spawn, "T7 (I2): a session built WITHOUT an agent_call capability never has "
                                 "\"agent.spawn\" text in its instructions");
        }
    }

    // T8 FIX PROOF (found live 2026-09-03, the project owner inspecting OpenRouter's own dashboard/
    // request-log view mid-run): `set_static_instructions()`'s own message used to be
    // `contribution->messages.push_back(...)` -- appended at the ABSOLUTE END of the assembled
    // message list. T4/T5/T6/T7 above are all SINGLE-turn, so `contribution->messages` held nothing
    // else to land after and the bug was invisible there. Drives the SAME session through TWO real
    // turns -- on the second turn, `contribution->messages` genuinely holds prior conversation
    // (turn 1's user+assistant messages) ahead of turn 2's own new user message, exactly the shape
    // that exposed the bug live. The fix (agent_session.hpp) inserts right after any `contribution->
    // instructions` message instead of at the end.
    {
        AgentSession<RecordingChatClient, NoSessionState, NoInstructionsProvider> session;
        session.initialize("t8", Principal{"p1", ""});
        session.set_static_instructions("static text");
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome1 = drive(session.start_run(StartRun{user_message("turn 1")}));
        check(outcome1.has_value(), "T8: the first turn converges");
        auto outcome2 = drive(session.start_run(StartRun{user_message("turn 2")}));
        check(outcome2.has_value(), "T8: the second turn converges");

        check(client.requests().size() == 2, "T8: two real requests reached the wire, one per turn");
        if (client.requests().size() == 2) {
            auto const& msgs2 = client.requests().back().messages;
            check(!msgs2.empty() && msgs2.front().role == role::system,
                  "T8 (FIXED): on the SECOND turn -- with real prior conversation already in the "
                  "assembled message list -- the system message is STILL the very FIRST message, not "
                  "appended after turn 1's user+assistant messages and turn 2's own new user message");
            auto system_count =
                std::ranges::count_if(msgs2, [](Message const& m) { return m.role == role::system; });
            check(system_count == 1,
                  "T8: still exactly ONE system message on turn 2 -- not re-duplicated by history "
                  "accumulation (the synthesized message is never itself pushed into session history)");
            check(msgs2.size() == 4,
                  "T8: message count is exactly system + turn1-user + turn1-assistant + turn2-user");
            if (!msgs2.empty() && msgs2.front().role == role::system && !msgs2.front().content.empty()) {
                auto const* t = std::get_if<Text>(&msgs2.front().content.front().value);
                check(t != nullptr && t->text == "static text",
                      "T8: the system message at index 0 still carries the exact configured text");
            }
        }
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_instructions: OK\n"
                                 : "test_rt_agent_session_instructions: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
