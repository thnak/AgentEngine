// Proof for ADR-037 Phase 2, Slice 1: porting five real behavioral claims from the OLD,
// Quark-actor-based AgentSession tests (core/agent_session.hpp) onto agentengine::rt::AgentSession
// (rt/agent_session.hpp) directly -- deterministic, offline, no live model, no network. Each source
// file's own claims are proven here under that file's own label prefix, NOT its Quark/TestKit
// plumbing (which this migration is retiring):
//   STATE (from test_agent_session_state.cpp) -- a caller-declared StateT genuinely mutates across
//         turns within one session, AND two independent AgentSession instances never share StateT.
//   WIN (from test_agent_session_history_window.cpp) -- a real HistoryProviderT (Window<N>, N>0)
//         actually changes what the ChatClientT receives on on_context(), inspected by capturing
//         ChatRequest::messages inside the fixture's own chat(), not inferred from output alone.
//   FORK (from test_agent_session_fork.cpp) -- fork_from() produces a session with a distinct
//         session_id, a TRUE COPY of history/state (mutating either afterward never crosses back),
//         history truncated to history_prefix_len (or full history when omitted), run_counter_/
//         last_run_id_ reset, and open_interactions_ cleared even if the source had some open.
//   REDACT (from test_agent_session_redact.cpp) -- redact() replaces ALL content of the matching
//         message with exactly one Custom{"ae:redacted", ...} tombstone, an unknown message_id
//         fails with session.redact.unknown_message_id, and every OTHER message is untouched.
//   INT (from test_agent_session_interaction.cpp) -- open_interaction() mints a real Interaction
//         with a deterministic session-scoped id, multiple concurrently-open interactions are all
//         tracked, resolve_interaction_record() removes exactly the matching one, resolving an
//         unknown id fails with session.resolve_interaction.unknown_id, and open interactions
//         round-trip through to_record()/restore_from_record().
//
// This file does NOT re-prove the core turn loop (S1-S5, test_rt_agent_session.cpp) or checkpoint
// round-trip mechanics (P1-P6, test_rt_agent_session_snapshot.cpp) -- both already cover that ground
// against rt::AgentSession directly.

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::AgentSessionRecord;
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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- every ChatClientT fixture below co_returns immediately.
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
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ContentItem;
using agentengine::Custom;

// -- Fixture helpers (copied/adapted per this project's own "no shared test helpers" convention) --

struct ScriptedOutcome {
    Message message;
    Usage usage;
};

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout this file
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

// WIN's fixture: records the joined text of every message it was actually called with, on every
// call -- proves what the ChatClientT received, not what a standalone HistoryProvider computes.
class RecordingChatClient {
public:
    RecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<std::string> calls_seen;  // one joined-text entry per chat() call
    };

    [[nodiscard]] std::vector<std::string> const& calls_seen() const { return state_->calls_seen; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        std::ostringstream joined;
        for (auto const& m : request.messages) {
            if (!m.content.empty()) {
                if (auto const* t = std::get_if<Text>(&m.content.front().value)) {
                    joined << t->text << ";";
                }
            }
        }
        state_->calls_seen.push_back(joined.str());

        Message reply = text_response_of("reply-" + std::to_string(state_->calls_seen.size()));
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    static Message text_response_of(std::string text) {
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{std::move(text)};
        m.content.push_back(item);
        return m;
    }

    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message user_message_with_id(std::string text, std::string message_id) {
    Message m = user_message(std::move(text));
    m.message_id = std::move(message_id);
    return m;
}

// STATE's declared, agent-specific scratch-state type -- deliberately NOT NoSessionState, to prove
// AgentSession<ChatClientT, StateT> carries a real, non-default type through.
struct TurnCounterState {
    int         turns_seen = 0;
    std::string last_note;
};

}  // namespace

int main() {
    using agentengine::Principal;
    using agentengine::Interaction;
    using agentengine::interaction_reason;

    // ============================================================================================
    // STATE (from test_agent_session_state.cpp)
    // ============================================================================================
    {
        AgentSession<ScriptedChatClient, TurnCounterState> session;
        session.initialize("state-1", Principal{"p-carol", ""});
        session.emplace_chat_client().set_script({{text_response("ok"), Usage{1, 1, 0, 0, 0.0}}});

        check(session.state().turns_seen == 0 && session.state().last_note.empty(),
              "STATE0: a fresh session's typed state starts at StateT's own defaults");

        session.state().turns_seen += 1;
        session.state().last_note = "first turn";
        auto r1 = drive(session.start_run(StartRun{user_message("hi")}));
        check(r1.has_value(), "STATE1 setup: turn 1 succeeds");
        check(session.state().turns_seen == 1 && session.state().last_note == "first turn",
              "STATE1: state written before a turn is still there, unmodified by the turn loop, "
              "after it -- the turn loop itself never touches StateT");

        session.state().turns_seen += 1;
        session.state().last_note = "second turn";
        auto r2 = drive(session.start_run(StartRun{user_message("hi again")}));
        check(r2.has_value() && session.state().turns_seen == 2 &&
                  session.state().last_note == "second turn",
              "STATE1: state genuinely mutates and accumulates across multiple turns on the same "
              "session");
    }
    {
        AgentSession<ScriptedChatClient, TurnCounterState> session_a;
        AgentSession<ScriptedChatClient, TurnCounterState> session_b;
        session_a.initialize("state-a", Principal{"p-a", ""});
        session_b.initialize("state-b", Principal{"p-b", ""});

        session_a.state().turns_seen = 5;
        session_a.state().last_note  = "a-only";

        check(session_b.state().turns_seen == 0 && session_b.state().last_note.empty(),
              "STATE2: two independent AgentSession instances never share StateT -- mutating one's "
              "state has zero effect on the other's");
    }

    // ============================================================================================
    // WIN (from test_agent_session_history_window.cpp)
    // ============================================================================================
    {
        // Default third template parameter (Window<0>, unbounded): the ChatClientT sees the WHOLE
        // accumulated history every turn.
        AgentSession<RecordingChatClient> session;
        session.initialize("win-default", Principal{"p-hank", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        auto r1 = drive(session.start_run(StartRun{user_message("first")}));
        check(r1.has_value(), "WIN1 setup: turn 1 succeeds");
        check(client.calls_seen().size() == 1 && client.calls_seen()[0] == "first;",
              "WIN1: default HistoryProviderT (Window<0>) sees just the new input on turn 1");

        auto r2 = drive(session.start_run(StartRun{user_message("second")}));
        check(r2.has_value(), "WIN1 setup: turn 2 succeeds");
        check(client.calls_seen().size() == 2,
              "WIN1: two real chat() calls happened, one per turn");
        check(client.calls_seen()[1] == "first;reply-1;second;",
              "WIN1: default HistoryProviderT (Window<0>) sees the FULL accumulated history on turn "
              "2 (first + turn-1's own reply + second) -- nothing dropped");
    }
    {
        // A real Window<2> actually drops older messages from what the ChatClientT receives.
        AgentSession<RecordingChatClient, agentengine::rt::NoSessionState,
                     agentengine::HistoryProvider<agentengine::Window<2>>>
            session;
        session.initialize("win-windowed", Principal{"p-hank", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        auto r1 = drive(session.start_run(StartRun{user_message("alpha")}));
        check(r1.has_value(), "WIN2 setup: turn 1 succeeds");
        check(client.calls_seen()[0] == "alpha;",
              "WIN2: Window<2> with only 1 message so far sees all of it -- nothing to drop yet");

        // History is now [user:alpha, assistant:reply-1, user:beta] (3 entries) when turn 2 runs --
        // Window<2> must keep only the LAST 2: [assistant:reply-1, user:beta].
        auto r2 = drive(session.start_run(StartRun{user_message("beta")}));
        check(r2.has_value(), "WIN2 setup: turn 2 succeeds");
        check(client.calls_seen()[1] == "reply-1;beta;",
              "WIN2: Window<2> drops the oldest message (user:alpha) once history exceeds 2 -- the "
              "ChatClientT never sees it, proving the window reaches the real turn loop, not just a "
              "standalone HistoryProvider computation");
        check(session.history().size() == 4,
              "WIN2: the WINDOW is what's budgeted for the model -- the session's own durable "
              "history_ still keeps everything (windowing is a read-side view, never a mutation)");
    }

    // ============================================================================================
    // FORK (from test_agent_session_fork.cpp)
    // ============================================================================================
    {
        using ForkSession = AgentSession<ScriptedChatClient, TurnCounterState>;

        ForkSession source;
        source.initialize("fork-source", Principal{"p-iris", "tenant-5"});
        source.state().turns_seen = 7;
        source.metadata()["k"] = "v";
        source.emplace_chat_client().set_script(
            {{text_response("one"), Usage{1, 1, 0, 0, 0.0}}, {text_response("two"), Usage{1, 1, 0, 0, 0.0}}});
        auto sr1 = drive(source.start_run(StartRun{user_message("one")}));
        check(sr1.has_value(), "FORK setup: source turn 1 succeeds");
        auto sr2 = drive(source.start_run(StartRun{user_message("two")}));
        check(sr2.has_value(), "FORK setup: source turn 2 succeeds");
        check(source.history().size() == 4, "FORK setup: source history has 4 entries");

        // Also give the source an open interaction at fork time -- FORK5 needs this to be present
        // in the source but cleared in the fork.
        Interaction const& source_interaction =
            source.open_interaction(source.last_run_id(), interaction_reason::approval);
        check(source.has_open_interactions(), "FORK setup: the source has an open interaction");
        (void)source_interaction;

        // FORK1: distinct session_id.
        ForkSession fork;
        fork.emplace_chat_client().set_script(
            {{text_response("fork-reply"), Usage{1, 1, 0, 0, 0.0}}});
        fork.fork_from(source, "fork-target");
        check(fork.session_id() == "fork-target" && fork.session_id() != source.session_id(),
              "FORK1: the fork gets a real, distinct session_id, never the same as its source");

        // FORK2: a TRUE COPY of history/state -- mutating the fork afterward never crosses back to
        // the source, and vice versa.
        check(fork.state().turns_seen == 7 && fork.metadata().at("k") == "v",
              "FORK2: state and metadata are copied alongside history at fork time");
        fork.state().turns_seen = 100;
        check(source.state().turns_seen == 7,
              "FORK2: mutating the fork's state after the fork never crosses back into the source");
        source.state().turns_seen = 999;
        check(fork.state().turns_seen == 100,
              "FORK2: mutating the source's state after the fork never crosses back into the fork "
              "(true copy in both directions, not a shared reference)");

        // FORK3: history truncated to history_prefix_len when given, full history when omitted.
        check(fork.history().size() == 4, "FORK3: a fork with no prefix length copies the WHOLE history");
        ForkSession partial_fork;
        partial_fork.emplace_chat_client();
        partial_fork.fork_from(source, "fork-partial", std::size_t{2});
        check(partial_fork.history().size() == 2,
              "FORK3: a fork with history_prefix_len=2 keeps exactly the first 2 messages");

        // FORK4: run_counter_/last_run_id_ reset -- the fork has run no runs of its own yet, and a
        // fresh start_run() on the fork mints run counter 1, not a continuation of the source's.
        check(fork.last_run_id().empty(),
              "FORK4: a fresh fork's last_run_id() is empty -- it has run no runs of its own");
        auto fr1 = drive(fork.start_run(StartRun{user_message("fork-only")}));
        check(fr1.has_value() && fork.last_run_id() == "fork-target:run:1",
              "FORK4: the fork's own first run mints run counter 1, prefixed by ITS OWN session_id "
              "-- not a continuation of the source's counter (source is already past run 2)");
        check(source.last_run_id() == "fork-source:run:2",
              "FORK4: running a turn on the fork never mutates the source's own run identity");

        // FORK5: open_interactions_ cleared even though the source had one open at fork time.
        check(!fork.has_open_interactions() && fork.open_interactions().empty(),
              "FORK5: the fork's open_interactions_ is cleared even though the source had one open "
              "at fork time -- a fork inherits none of the source's outstanding interactions");
        check(source.has_open_interactions(),
              "FORK5: clearing the fork's open interactions never mutates the source's own set");
    }

    // ============================================================================================
    // REDACT (from test_agent_session_redact.cpp)
    // ============================================================================================
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("redact-unknown", Principal{"p-june", ""});
        session.emplace_chat_client();
        auto bad = session.redact("no-such-id", "gdpr", "operator-1");
        check(!bad.has_value() && bad.error().code == "session.redact.unknown_message_id",
              "REDACT2: redacting an unknown message_id returns a real error, not a silent no-op");
    }
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("redact", Principal{"p-june", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({{text_response("reply-a"), Usage{1, 1, 0, 0, 0.0}},
                            {text_response("reply-b"), Usage{1, 1, 0, 0, 0.0}}});

        Message secret = user_message_with_id("my secret ssn is 123-45-6789", "m-secret");
        ContentItem extra{};
        extra.value  = Text{"a second content item, also secret"};
        extra.origin = content_origin::user;
        secret.content.push_back(extra);

        auto r1 = drive(session.start_run(StartRun{secret}));
        check(r1.has_value(), "REDACT setup: turn 1 succeeds");
        auto r2 = drive(session.start_run(StartRun{user_message_with_id("second turn", "m-second")}));
        check(r2.has_value(), "REDACT setup: turn 2 succeeds");
        check(session.history().size() == 4 && session.history()[0].content.size() == 2,
              "REDACT setup: the source message has 2 content items before redaction, and 3 other "
              "messages exist to prove non-interference");

        // Snapshot the OTHER messages' content before redacting, to compare byte-for-byte after.
        std::vector<Message> const before = session.history();

        auto redacted = session.redact("m-secret", "gdpr-erasure-request", "operator-1");
        check(redacted.has_value(), "REDACT1: redacting a message that exists succeeds");

        Message const& after = session.history()[0];
        check(after.content.size() == 1,
              "REDACT1: the tombstone replaces ALL content items with exactly one -- nothing of the "
              "original 2 items survives");
        auto const* custom = std::get_if<Custom>(&after.content.front().value);
        check(custom != nullptr && custom->type_id == "ae:redacted",
              "REDACT1: the tombstone is a Custom content item, namespaced \"ae:redacted\"");
        if (custom != nullptr) {
            auto parsed = agentengine::json::parse(custom->payload_json);
            check(parsed.has_value(), "REDACT1: the tombstone payload is valid JSON");
            if (parsed.has_value()) {
                auto const* reason = parsed->find("reason");
                auto const* actor  = parsed->find("actor");
                check(reason != nullptr && reason->as_string() == "gdpr-erasure-request",
                      "REDACT1: the tombstone carries the exact reason passed to redact()");
                check(actor != nullptr && actor->as_string() == "operator-1",
                      "REDACT1: the tombstone carries the exact actor passed to redact() (I4 "
                      "attribution)");
            }
        }

        check(session.history().size() == before.size(),
              "REDACT3: redaction does not add or remove messages, only replaces content in place");
        for (std::size_t i = 1; i < session.history().size(); ++i) {
            check(session.history()[i].content.size() == before[i].content.size(),
                  "REDACT3: every OTHER message's content item count is untouched by redacting "
                  "m-secret");
            if (!session.history()[i].content.empty() && !before[i].content.empty()) {
                auto const* orig_after  = std::get_if<Text>(&session.history()[i].content.front().value);
                auto const* orig_before = std::get_if<Text>(&before[i].content.front().value);
                check((orig_after == nullptr) == (orig_before == nullptr) &&
                          (orig_after == nullptr || orig_after->text == orig_before->text),
                      "REDACT3: every OTHER message's own text content is byte-identical before and "
                      "after redacting a DIFFERENT message");
            }
        }
    }

    // ============================================================================================
    // INT (from test_agent_session_interaction.cpp)
    // ============================================================================================
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("interact", Principal{"p-omar", ""});
        session.emplace_chat_client();

        check(!session.has_open_interactions(), "INT0: a fresh session has no open interactions");

        // INT1: minting produces a deterministic, session-scoped id.
        Interaction const& i1 = session.open_interaction("interact:run:1", interaction_reason::input);
        check(i1.interaction_id == "interact:interaction:1" && i1.run_id == "interact:run:1" &&
                  i1.reason == interaction_reason::input,
              "INT1: the first Interaction has the deterministic id "
              "\"<session_id>:interaction:<counter>\", and the given run_id/reason");
        std::string const i1_id = i1.interaction_id;

        // INT2: multiple concurrently-open interactions are all tracked.
        Interaction const& i2 = session.open_interaction("interact:run:1", interaction_reason::auth);
        std::string const i2_id = i2.interaction_id;
        check(i2_id == "interact:interaction:2" && i2.reason == interaction_reason::auth,
              "INT2: a second Interaction gets a distinct, incrementing id and its own reason");
        check(session.has_open_interactions() && session.open_interactions().size() == 2,
              "INT2: both interactions are tracked simultaneously by open_interactions()");

        // INT4: resolving an unknown id fails.
        auto bad = session.resolve_interaction_record("no-such-id");
        check(!bad.has_value() && bad.error().code == "session.resolve_interaction.unknown_id",
              "INT4: resolving an unknown interaction_id returns a real error, not a silent no-op");

        // INT3: resolve_interaction_record() removes exactly the matching one, leaves the other.
        auto r1 = session.resolve_interaction_record(i1_id);
        check(r1.has_value(), "INT3: resolving the first (real) interaction succeeds");
        check(session.has_open_interactions() && session.open_interactions().size() == 1 &&
                  session.open_interactions()[0].interaction_id == i2_id,
              "INT3: resolving i1 removes EXACTLY i1 -- i2 is still open and untouched");

        auto r2 = session.resolve_interaction_record(i2_id);
        check(r2.has_value() && !session.has_open_interactions(),
              "INT3: resolving the last remaining interaction clears the open set entirely");

        // INT5: open interactions round-trip through to_record()/restore_from_record().
        (void)session.open_interaction("interact:run:2", interaction_reason::auth);
        (void)session.open_interaction("interact:run:2", interaction_reason::input);
        check(session.open_interactions().size() == 2,
              "INT5 setup: two fresh interactions open before round-tripping");

        AgentSessionRecord rec = session.to_record();
        check(rec.open_interactions.size() == 2,
              "INT5: to_record() carries BOTH open interactions (unlike history/state/metadata, "
              "AgentSessionRecord.open_interactions IS carried)");

        AgentSession<ScriptedChatClient> restored;
        restored.emplace_chat_client();
        restored.restore_from_record(rec);
        check(restored.has_open_interactions() && restored.open_interactions().size() == 2,
              "INT5: restoring the record into a FRESH instance recovers both open interactions");
        check(restored.open_interactions()[0].reason == interaction_reason::auth &&
                  restored.open_interactions()[1].reason == interaction_reason::input,
              "INT5: each restored interaction's own reason survives the round-trip correctly, not "
              "collapsed to a single value");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_lifecycle: ALL PASS\n");
    return 0;
}
