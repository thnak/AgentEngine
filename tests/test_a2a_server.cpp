// Milestone 7 Phase D3 (012-A2A-Conformance.md §2.3, docs/planning/milestone-7-protocol-conformance-
// breakdown.md). Proves `A2aServer` (protocol/a2a/server.hpp) end to end against a REAL
// `agentengine::rt::AgentSession` -- `Task.id` really is the session's own `run_id`
// (`AgentSession::last_run_id()`), `SendMessage` really drives a full turn through a real
// `ChatClient`, and `GetTask`/`CancelTask` behave exactly as this codebase's own current
// capabilities allow (never claiming an in-flight/cancellable/interrupted state this synchronous
// dispatch cannot actually produce -- see server.hpp's own file-top comment for why).
//
// ADR-037: ported off `quark::Engine`/`Actor`/`ActorRef` onto `rt::AgentSession` directly -- this
// test's own `RunStarter` now `drive()`s `session.start_run()`'s returned `rt::task<...>` to
// completion inline (single-threaded, deterministic; matching the `drive<T>()` idiom every other
// `rt::` test in this suite already uses) instead of `block_on(ref.ask<...>(...))` against a real
// actor mailbox. Same behavior proven, no engine/mailbox left to stand it up.

#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/protocol/a2a/server.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace ae  = agentengine;
namespace art = agentengine::rt;
namespace a2a = agentengine::a2a;

template <class T>
T drive(art::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// Same shape test_agent_session_suspend_resume.cpp's own CannedChatClient uses -- echoes the run_id
// back so a test can verify the REAL run actually executed, not a stub returning canned text blind
// to which run it was.
class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value  = ae::Text{"reply to run " + ctx.run_id};
        item.origin = ae::content_origin::assistant;
        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;  // generous enough that a small scripted response never blocks on credit
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd{};
        upd.delta.value  = ae::Text{"reply to run " + ctx.run_id};
        upd.delta.origin = ae::content_origin::assistant;
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CannedChatClient>);

// Always fails -- the RunStarter's own "no reply" path (server.hpp's own file-top comment: admission
// denial, context failure, chat failure, and budget-exceeded all collapse to this ONE generic shape).
class FailingChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        co_return std::unexpected(ae::error{ae::failure_class::transient, "deliberate chat failure",
                                             "test.deliberate_chat_failure"});
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.fail(ae::error{ae::failure_class::transient, "deliberate chat failure",
                                      "test.deliberate_chat_failure"});
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<FailingChatClient>);

template <class ChatClientT>
struct Harness {
    using Session = art::AgentSession<ChatClientT>;

    Session session;

    explicit Harness(std::string session_id) {
        session.initialize(std::move(session_id), ae::Principal{"p-owner", ""});
        session.emplace_chat_client();
    }

    [[nodiscard]] a2a::RunStarter starter() {
        return [this](art::StartRun req) -> ae::result<a2a::RunOutcome> {
            ae::result<art::AgentResponse> resp = drive(session.start_run(std::move(req)));
            if (!resp) {
                return std::unexpected(ae::error{ae::failure_class::transient,
                                                  "the run did not complete", "a2a.run_did_not_complete"});
            }
            return a2a::RunOutcome{session.last_run_id(), *resp};
        };
    }
};

a2a::Message text_message(std::string text) {
    a2a::Message m;
    m.message_id = "wire-msg-1";
    m.role        = a2a::a2a_role::user;
    a2a::Part p;
    p.value = a2a::TextPart{std::move(text)};
    m.parts.push_back(std::move(p));
    return m;
}

}  // namespace

int main() {
    // --- D3-1/2/3: a real SendMessage produces a Task whose id IS the real run_id, COMPLETED, with -
    // --- the real reply content and a two-message history.                                        ---
    Harness<CannedChatClient> h{"s-a2a"};
    a2a::A2aServer server(h.starter(), "ctx-1");

    auto sent = server.send_message(text_message("hello"));
    check(sent.has_value(), "D3-1: send_message() against a real AgentSession succeeds");
    std::string task_id;
    if (sent.has_value()) {
        task_id = sent->id;
        check(task_id == h.session.last_run_id(),
              "D3-1: Task.id IS the real run_id (012 §1: Task ← Run), not a separately invented id");
        check(sent->status.state == a2a::task_state::completed,
              "D3-2: a real successful run produces TASK_STATE_COMPLETED");
        check(sent->status.message.has_value() && sent->status.message->role == a2a::a2a_role::agent,
              "D3-2: the task's own status.message is the real agent reply, role ROLE_AGENT");
        if (sent->status.message.has_value() && !sent->status.message->parts.empty()) {
            auto const* t = std::get_if<a2a::TextPart>(&sent->status.message->parts.front().value);
            check(t && t->text == "reply to run " + task_id,
                  "D3-2: the reply text really came from the real ChatClient, naming the real run_id "
                  "-- proof this is a genuine end-to-end turn, not a stub");
        }
        check(sent->history.size() == 2, "D3-3: history carries exactly the inbound + reply messages");
        if (sent->history.size() == 2) {
            check(sent->history[0].role == a2a::a2a_role::user && sent->history[1].role == a2a::a2a_role::agent,
                  "D3-3: history preserves speaking order, user then agent");
        }
    }

    // --- D3-4: GetTask on that same id returns the identical Task -----------------------------------
    {
        auto fetched = server.get_task(task_id);
        check(fetched.has_value() && fetched->id == task_id &&
                  fetched->status.state == a2a::task_state::completed,
              "D3-4: GetTask retrieves the exact same completed Task by its real run_id");
    }

    // --- D3-5: GetTask on an unknown id is rejected --------------------------------------------------
    {
        auto missing = server.get_task("no-such-task");
        check(!missing.has_value(), "D3-5: GetTask on an unknown taskId is rejected");
    }

    // --- D3-6: CancelTask on an (already-terminal) task is rejected -- "terminal is terminal" (§2.3),-
    // --- never fabricating a CANCELED transition this synchronous dispatch cannot really produce.  ---
    {
        auto cancelled = server.cancel_task(task_id);
        check(!cancelled.has_value(),
              "D3-6: CancelTask on an already-completed task is rejected, faithfully, not silently "
              "accepted");
        // Confirm it is STILL retrievable and STILL completed -- rejection didn't corrupt state.
        auto still_there = server.get_task(task_id);
        check(still_there.has_value() && still_there->status.state == a2a::task_state::completed,
              "D3-6: the task is unaffected by the rejected cancel attempt");
    }

    // --- D3-7: CancelTask on an unknown id is rejected too, distinctly from the terminal-task case --
    {
        auto cancel_missing = server.cancel_task("no-such-task");
        check(!cancel_missing.has_value(), "D3-7: CancelTask on an unknown taskId is rejected");
        if (!cancel_missing.has_value()) {
            check(cancel_missing.error().code == "a2a.unknown_task",
                  "D3-7: an unknown taskId is rejected with a DIFFERENT reason than an already-"
                  "terminal known task -- \"not found\" and \"can't cancel this\" are not conflated");
        }
    }

    // --- D3-8: a run that never completes (chat failure) still produces a real, retrievable Task, --
    // --- state FAILED, with its own dispatcher-minted id (no run_id was learnable from the failed --
    // --- ask) -- proven against a SECOND real session, not a fabricated in-memory shortcut.        ---
    {
        Harness<FailingChatClient> hf{"s-a2a-fail"};
        a2a::A2aServer failing_server(hf.starter(), "ctx-2");
        auto failed = failing_server.send_message(text_message("this will fail"));
        check(failed.has_value(),
              "D3-8: even a run that never completes still produces a real Task -- SendMessage "
              "itself does not fail, the TASK reports the failure");
        if (failed.has_value()) {
            check(failed->status.state == a2a::task_state::failed,
                  "D3-8: TASK_STATE_FAILED for a run whose Ask never resolved");
            check(!failed->id.empty(), "D3-8: the dispatcher minted its own real (non-run) task id");
            auto refetched = failing_server.get_task(failed->id);
            check(refetched.has_value() && refetched->status.state == a2a::task_state::failed,
                  "D3-8: the FAILED task is retrievable by its own minted id, exactly like a "
                  "COMPLETED one would be");
        }
    }

    if (g_failures == 0) {
        std::printf("test_a2a_server: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_server: %d failure(s)\n", g_failures);
    return 1;
}
