// Gap-audit finding 20 / 003 §8 Q2 fix (2026-08-14): proves `AgentSession::run_rounds()` actually
// implements Q2's resolution -- "a Reasoning item is included in a turn's assembled context only
// when it originated from the ChatClientId currently bound; a different-provider item is EXCLUDED
// from that turn's context, not deleted" -- rather than the previous behavior, where nothing ever
// checked a `Reasoning` item's provenance at all (the field didn't exist). Checked both directions
// (a positive control proves this isn't "everything with Reasoning gets dropped"), the mixed-content
// case (only the Reasoning item is stripped, not the whole message), the trace-recording requirement
// (a real `run_event_kind::policy_decision` fires, 013 §1's own vocabulary), and the "no identity, no
// filtering" default degrade for a `ChatClientT` that doesn't expose `producer_chat_client_id()`.

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
using agentengine::Principal;
using agentengine::Reasoning;
using agentengine::RunEvent;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::run_event_kind;
using agentengine::run_event_payload::PolicyDecision;

std::vector<RunEvent> drain(agentengine::stream<RunEvent>& s) {
    std::vector<RunEvent> events;
    while (auto ev = s.next()) events.push_back(std::move(*ev));
    return events;
}

// Records every ChatRequest it receives AND reports its own identity -- satisfies
// `HasProducerChatClientId` (chat_client.hpp), mirroring `AnthropicChatClient`'s real accessor.
class IdentifiedRecordingChatClient {
public:
    IdentifiedRecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ChatRequest> requests;
    };

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    [[nodiscard]] std::string producer_chat_client_id() const { return "mock:test-model"; }

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
static_assert(agentengine::ChatClient<IdentifiedRecordingChatClient>);
static_assert(agentengine::HasProducerChatClientId<IdentifiedRecordingChatClient>);

// Deliberately has NO producer_chat_client_id() -- proves the "no identity, no filtering" default.
class PlainRecordingChatClient {
public:
    PlainRecordingChatClient() : state_(std::make_shared<State>()) {}

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
        return {};
    }

    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<PlainRecordingChatClient>);
static_assert(!agentengine::HasProducerChatClientId<PlainRecordingChatClient>);

// Contributes a FIXED set of pre-built messages (ignoring session history entirely) plus whatever
// the live turn adds -- lets each test set up an exact, known Reasoning-bearing history without
// needing several real rounds to build it up.
struct FixedSeedProvider {
    std::vector<Message> seed;

    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages = seed;
        c.messages.insert(c.messages.end(), sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<FixedSeedProvider>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message reasoning_only_message(std::string message_id, std::string producer_id) {
    Message m;
    m.role       = role::assistant;
    m.message_id = std::move(message_id);
    ContentItem item;
    item.origin = content_origin::assistant;
    Reasoning r;
    r.text                    = "internal chain-of-thought";
    r.producer_chat_client_id = std::move(producer_id);
    item.value = std::move(r);
    m.content.push_back(std::move(item));
    return m;
}

// Text + Reasoning together in ONE message -- proves the filter strips only the offending content
// item, not the whole message.
Message mixed_message(std::string message_id, std::string producer_id) {
    Message m;
    m.message_id = std::move(message_id);
    m.role       = role::assistant;
    ContentItem text_item;
    text_item.origin = content_origin::assistant;
    text_item.value  = Text{"visible answer"};
    m.content.push_back(std::move(text_item));
    ContentItem reasoning_item;
    reasoning_item.origin = content_origin::assistant;
    Reasoning r;
    r.text                    = "internal chain-of-thought";
    r.producer_chat_client_id = std::move(producer_id);
    reasoning_item.value = std::move(r);
    m.content.push_back(std::move(reasoning_item));
    return m;
}

bool has_message_id(std::vector<Message> const& msgs, std::string const& id) {
    for (auto const& m : msgs) {
        if (m.message_id == id) return true;
    }
    return false;
}

bool message_has_reasoning(std::vector<Message> const& msgs, std::string const& id) {
    for (auto const& m : msgs) {
        if (m.message_id != id) continue;
        for (auto const& item : m.content) {
            if (std::holds_alternative<Reasoning>(item.value)) return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // T1: a Reasoning item from a DIFFERENT backend is excluded -- the message it was the ONLY
    // content of is dropped entirely, and a policy_decision event records why.
    {
        AgentSession<IdentifiedRecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t1", Principal{"p1", ""});
        session.history_provider().seed = {reasoning_only_message("m-reasoning", "anthropic:claude-x")};
        IdentifiedRecordingChatClient& client = session.emplace_chat_client();
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T1: the run converges");
        check(client.requests().size() == 1, "T1: exactly one model call happened");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(!has_message_id(msgs, "m-reasoning"),
                  "T1: the cross-provider Reasoning-only message is excluded from the outbound "
                  "request entirely -- its ONLY content item was the excluded Reasoning, so nothing "
                  "was left to send");
        }

        auto events = drain(viewer);
        bool found_policy_decision = false;
        for (auto const& ev : events) {
            if (ev.kind == run_event_kind::policy_decision) {
                auto const* pd = std::get_if<PolicyDecision>(&ev.payload);
                if (pd != nullptr && pd->description.find("m-reasoning") != std::string::npos &&
                    pd->description.find("anthropic:claude-x") != std::string::npos) {
                    found_policy_decision = true;
                }
            }
        }
        check(found_policy_decision,
              "T1: a real run_event_kind::policy_decision fires naming the excluded message and its "
              "producer -- 003 §8 Q2's own 'recorded in the trace' requirement");
    }

    // T2 (positive control): a Reasoning item stamped with the CURRENTLY BOUND backend's own id
    // survives untouched -- proves T1 isn't "every Reasoning item is dropped unconditionally."
    {
        AgentSession<IdentifiedRecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t2", Principal{"p1", ""});
        session.history_provider().seed = {reasoning_only_message("m-reasoning", "mock:test-model")};
        IdentifiedRecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T2: the run converges");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(has_message_id(msgs, "m-reasoning") && message_has_reasoning(msgs, "m-reasoning"),
                  "T2 (positive control): a Reasoning item produced by the SAME backend currently "
                  "bound survives into the outbound request, unaltered");
        }
    }

    // T3: mixed content -- a Text item and a cross-provider Reasoning item in the SAME message.
    // Only the Reasoning item is stripped; the message (and its Text) survives.
    {
        AgentSession<IdentifiedRecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t3", Principal{"p1", ""});
        session.history_provider().seed = {mixed_message("m-mixed", "openai:gpt-5")};
        IdentifiedRecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T3: the run converges");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(has_message_id(msgs, "m-mixed"),
                  "T3: the mixed message survives (it still has real content after filtering)");
            check(!message_has_reasoning(msgs, "m-mixed"),
                  "T3: the cross-provider Reasoning item inside it is stripped");
            bool text_survives = false;
            for (auto const& m : msgs) {
                if (m.message_id != "m-mixed") continue;
                for (auto const& item : m.content) {
                    if (auto const* t = std::get_if<Text>(&item.value); t != nullptr && t->text == "visible answer") {
                        text_survives = true;
                    }
                }
            }
            check(text_survives, "T3: the SAME message's own Text content item is untouched");
        }
    }

    // T4: a ChatClientT with no producer_chat_client_id() at all gets NO filtering -- the exact
    // same cross-provider Reasoning item T1 excludes survives unchanged here, the "no identity, no
    // gate" default degrade (agentengine::HasProducerChatClientId, chat_client.hpp).
    {
        AgentSession<PlainRecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t4", Principal{"p1", ""});
        session.history_provider().seed = {reasoning_only_message("m-reasoning", "anthropic:claude-x")};
        PlainRecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T4: the run converges");

        if (!client.requests().empty()) {
            auto const& msgs = client.requests().front().messages;
            check(has_message_id(msgs, "m-reasoning") && message_has_reasoning(msgs, "m-reasoning"),
                  "T4: with no identity to compare against, filtering is skipped entirely -- a "
                  "ChatClientT that doesn't support HasProducerChatClientId sees unchanged behavior, "
                  "never a silently-wrong filter");
        }
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_reasoning_provenance: OK\n"
                                 : "test_rt_agent_session_reasoning_provenance: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
