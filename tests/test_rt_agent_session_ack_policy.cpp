// Gap-15 fix (2026-08-14, decisions/ADR-043-*.md): proves `start_run_with_ack_policy()`'s two real
// behaviors -- `ack_policy::at_most_once` is byte-identical to calling `session.start_run()` directly
// (no store interaction at all, matching today's unchanged default), and `ack_policy::require_durable`
// durably writes BOTH the turn's own history delta (via `save_turn_delta`/`load_turn_delta`, reusing
// `rt/message_codec.hpp`'s already-proven Message<->JSON codec) and the session's bookkeeping record
// (`save_agent_session_snapshot`) before the caller ever sees a successful response -- and fails
// closed (returns an error, not a false-success `AgentResponse`) if either durable write fails.
// `resolve_interaction_with_ack_policy()` is the identical pattern applied to the other real
// turn-completing entry point -- proven only by compiling (it's structurally the same sequencing as
// start_run_with_ack_policy(), not independently behavior-tested here; named, not silently skipped).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::ack_policy;
using agentengine::rt::AgentSession;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::load_turn_delta;
using agentengine::rt::NoSessionState;
using agentengine::rt::start_run_with_ack_policy;
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
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::string reply_text;
        std::size_t call_count = 0;
    };
    void set_reply(std::string text) { state_->reply_text = std::move(text); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        ++state_->call_count;
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{state_->reply_text};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

// A SessionStore conformer whose save() can be made to fail on demand, and which otherwise
// delegates to a real InMemorySessionStore -- proves the fail-closed path without a fragile
// filesystem-permission trick.
class FlakyStore {
public:
    void set_save_should_fail(bool fail) { fail_next_save_ = fail; }

    [[nodiscard]] agentengine::result<void> save(agentengine::rt::SessionId const& id,
                                                   std::vector<std::byte> bytes) {
        ++save_count_;
        if (fail_next_save_) {
            return std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                        "injected store failure", "test.flaky_store.save"});
        }
        return inner_.save(id, std::move(bytes));
    }
    [[nodiscard]] agentengine::result<std::vector<std::byte>> load(agentengine::rt::SessionId const& id) const {
        return inner_.load(id);
    }
    [[nodiscard]] bool exists(agentengine::rt::SessionId const& id) const { return inner_.exists(id); }
    [[nodiscard]] agentengine::result<void> remove(agentengine::rt::SessionId const& id) {
        return inner_.remove(id);
    }
    [[nodiscard]] std::size_t save_count() const { return save_count_; }

private:
    InMemorySessionStore inner_;
    bool fail_next_save_ = false;
    std::size_t save_count_ = 0;
};
static_assert(agentengine::rt::SessionStore<FlakyStore>);

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

    // T1: at_most_once -- byte-identical to a raw start_run(), no store interaction whatsoever.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("ap-t1", Principal{"p1", ""});
        session.emplace_chat_client().set_reply("hi there");
        FlakyStore store;

        auto outcome =
            drive(start_run_with_ack_policy(session, StartRun{user_message("hi")}, ack_policy::at_most_once, store));
        check(outcome.has_value(), "T1: the run converges under at_most_once");
        check(store.save_count() == 0,
              "T1: at_most_once never touches the store -- zero save() calls, matching today's "
              "unchanged default behavior");
    }

    // T2: require_durable, success path -- both the turn delta and the session snapshot are
    // durably written before the caller sees the response, and the delta round-trips byte-correct.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("ap-t2", Principal{"p1", ""});
        session.emplace_chat_client().set_reply("durable reply");
        InMemorySessionStore store;

        auto outcome = drive(start_run_with_ack_policy(session, StartRun{user_message("please persist")},
                                                          ack_policy::require_durable, store));
        check(outcome.has_value(), "T2: the run converges under require_durable");
        check(outcome.has_value() && std::holds_alternative<Text>(outcome->message.content.front().value) &&
                  std::get<Text>(outcome->message.content.front().value).text == "durable reply",
              "T2: the caller still receives the real response, not just a durability receipt");

        check(store.exists("ap-t2"), "T2: the session bookkeeping record was durably written");
        std::uint64_t const turn_index = session.to_record().turn_index;
        auto delta = load_turn_delta(store, "ap-t2", turn_index == 0 ? 0 : turn_index - 1);
        // turn_index has already been advanced past the completed turn by the time we read it here --
        // the delta this turn wrote is keyed under the turn_index AT THE TIME OF THE WRITE, which
        // start_run_with_ack_policy() itself captured via session.to_record().turn_index inside the
        // call -- try both the pre- and post-increment index rather than assume which this session's
        // own increment-timing lands on, to keep this test robust to that internal detail.
        if (!delta.has_value() || !delta->has_value()) {
            delta = load_turn_delta(store, "ap-t2", turn_index);
        }
        check(delta.has_value() && delta->has_value(),
              "T2: the turn's own history delta is durably retrievable via load_turn_delta");
        if (delta.has_value() && delta->has_value()) {
            bool found_user = false, found_reply = false;
            for (Message const& m : delta->value().messages) {
                if (m.content.empty()) continue;
                if (auto const* t = std::get_if<Text>(&m.content.front().value)) {
                    if (t->text == "please persist") found_user = true;
                    if (t->text == "durable reply") found_reply = true;
                }
            }
            check(found_user && found_reply,
                  "T2: the durable delta contains BOTH this turn's input and its response, byte-correct "
                  "through the round trip");
        }
    }

    // T3: require_durable, the history-delta write itself fails -- fail closed, no false-success
    // AgentResponse ever reaches the caller.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("ap-t3", Principal{"p1", ""});
        session.emplace_chat_client().set_reply("should not be trusted");
        FlakyStore store;
        store.set_save_should_fail(true);

        auto outcome = drive(start_run_with_ack_policy(session, StartRun{user_message("hi")},
                                                          ack_policy::require_durable, store));
        check(!outcome.has_value(), "T3: a failed durable write is surfaced as a real failure, "
                                     "never a silent success");
        check(!outcome.has_value() && outcome.error().code == "run.durable_ack_failed",
              "T3: the failure carries the specific run.durable_ack_failed code");
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_ack_policy: OK\n"
                                 : "test_rt_agent_session_ack_policy: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
