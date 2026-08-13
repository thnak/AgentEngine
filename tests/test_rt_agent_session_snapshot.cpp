// Proof for ADR-037 Phase 2, Slice 2: agentengine::rt::AgentSession's snapshot/checkpoint surface
// (include/agentengine/rt/agent_session.hpp's "Slice 2 addition" -- to_record()/restore_from_record(),
// snapshot_record(), save_agent_session_snapshot/load_agent_session_snapshot/checkpoint_if_due/
// delete_session), built against rt::SessionStore instead of quark::FenceToken/snapshot_sequential.
// Deterministic, offline, no live model, no network. Covers:
//   P1 -- to_record()/restore_from_record() round-trips session identity + run position.
//   P2 -- save_agent_session_snapshot()/load_agent_session_snapshot() round-trip through a real
//         SessionStore conformer (InMemorySessionStore).
//   P2b -- (ported from test_agent_session_snapshot.cpp, now fully superseded and retired) a second
//         save() under the same id overwrites the latest state -- recovery returns the LATEST
//         snapshot, not the first one.
//   P3 -- loading a session id that was never saved returns std::nullopt, not an error.
//   P4 -- delete_session() tombstones the durable record AND clears in-process state; a post-delete
//         load() sees no residue, matching the Quark original's own "no distinction between never
//         existed and deleted" read-path property. Also proves (ported from
//         test_agent_session_delete.cpp, now fully superseded and retired) that EVERY accessor
//         reports no residue -- principal, history, a non-trivial StateT reset to its own default,
//         metadata, and run identity -- not just session_id.
//   P5 -- checkpoint_if_due()'s cadence gate: skips writes below the threshold, writes once it's
//         reached, exactly like the Quark original's CheckpointCadence<N>.
//   P6 -- THE REAL POINT of this slice's own design (file banner): the in-flight guard.
//         snapshot_record() must not observe a torn read while a start_run() is genuinely
//         in-flight -- proven deterministically, single-threaded, by manually driving two
//         agentengine::rt::task<T> coroutines with individual resume() calls (never the naive
//         drive() loop, which is unsafe here -- see that helper's own comment) around a
//         genuinely-suspending ChatClientT built on rt::channel<T> (already proven in Phase 1).

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/channel.hpp"
#include "agentengine/rt/session_store.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::AgentSessionRecord;
using agentengine::rt::AgentResponse;
using agentengine::rt::CheckpointCadence;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::StartRun;
using agentengine::rt::checkpoint_if_due;
using agentengine::rt::delete_session;
using agentengine::rt::load_agent_session_snapshot;
using agentengine::rt::save_agent_session_snapshot;
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

// Same "safe here because nothing genuinely suspends externally" rule as test_rt_agent_session.cpp's
// own drive<T>() -- deliberately NOT used by P6 below, which needs individually-timed resume() calls
// to observe an in-flight state, not a run-to-completion helper.
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

// P1-P5's fixture: converges on the first call, every call, deterministically -- these tests only
// need real run_counter/turn_index movement, not round-loop mechanics (already proven by
// test_rt_agent_session.cpp's S1/S2).
class OneShotChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return ChatResponse{text_response("ok"), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<OneShotChatClient>);

// P6's fixture: chat() genuinely suspends on an rt::channel<int> gate until the test explicitly
// pushes a value -- the one thing OneShotChatClient above can't provide (it never suspends on
// anything external, so it can't put a start_run() call into an observably in-flight state).
class SuspendingChatClient {
public:
    explicit SuspendingChatClient(agentengine::rt::channel_consumer<int> gate) : gate_(std::move(gate)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        (void)co_await gate_.next_async();  // await_resume() is [[nodiscard]]; the pushed value itself
                                             // (a bare signal, not real data) is unused by design here.
        co_return ChatResponse{text_response("done"), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    agentengine::rt::channel_consumer<int> gate_;
};
static_assert(agentengine::ChatClient<SuspendingChatClient>);

}  // namespace

int main() {
    using agentengine::Principal;

    // P1: to_record()/restore_from_record() round-trips identity + run position.
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("p1", Principal{"p1", "tenant-y"});
        session.emplace_chat_client();
        auto ran = drive(session.start_run(StartRun{user_message("hi")}));
        check(ran.has_value(), "P1 setup: the run converges");

        AgentSessionRecord rec = session.to_record();
        check(rec.session_id == "p1", "P1: to_record() carries session_id");
        check(rec.principal_id == "p1" && rec.principal_tenant_id == "tenant-y",
              "P1: to_record() carries the flattened principal");
        check(rec.run_counter == 1, "P1: to_record() carries run position (run_counter)");
        check(!rec.deleted, "P1: a live session's record is not a tombstone");

        AgentSession<OneShotChatClient> restored;
        restored.emplace_chat_client();
        restored.restore_from_record(rec);
        check(restored.session_id() == "p1", "P1: restore_from_record() restores session_id");
        check(restored.principal().id == "p1" && restored.principal().tenant_id == "tenant-y",
              "P1: restore_from_record() restores the principal");
        check(restored.last_run_id() == "p1:run:1",
              "P1: restore_from_record() reconstructs last_run_id from the restored run_counter, so "
              "the NEXT StartRun on the restored session won't remint a colliding run_id");
    }

    // P2: save_agent_session_snapshot()/load_agent_session_snapshot() round-trip through a real
    // SessionStore conformer.
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("p2", Principal{"p2", "tenant-x"});
        session.emplace_chat_client();
        InMemorySessionStore store;
        auto ran = drive(session.start_run(StartRun{user_message("hi")}));
        check(ran.has_value(), "P2 setup: the run converges");

        auto saved = drive(save_agent_session_snapshot(session, store));
        check(saved.has_value(), "P2: save_agent_session_snapshot() succeeds");

        auto loaded = load_agent_session_snapshot(store, "p2");
        check(loaded.has_value() && loaded->has_value(), "P2: load_agent_session_snapshot() finds it");
        if (loaded.has_value() && loaded->has_value()) {
            AgentSessionRecord const& rec = **loaded;
            check(rec.session_id == "p2", "P2: the loaded record's session_id round-trips");
            check(rec.principal_id == "p2" && rec.principal_tenant_id == "tenant-x",
                  "P2: the loaded record's principal round-trips");
            check(rec.run_counter == 1, "P2: the loaded record's run_counter round-trips");
        }

        // P2b (ported from test_agent_session_snapshot.cpp, now retired): a second save() under the
        // same id overwrites the latest state -- the store's own single-slot, overwrite-latest
        // contract, exercised through save_agent_session_snapshot() specifically rather than assumed.
        session.initialize("p2", Principal{"p2-renamed", "tenant-x"});
        auto saved_again = drive(save_agent_session_snapshot(session, store));
        check(saved_again.has_value(), "P2b setup: a second snapshot under the same id succeeds");
        auto loaded_again = load_agent_session_snapshot(store, "p2");
        check(loaded_again.has_value() && loaded_again->has_value() &&
                  (*loaded_again)->principal_id == "p2-renamed",
              "P2b: recovery returns the LATEST snapshot, not the first one");
    }

    // P3: loading a session id that was never saved returns std::nullopt, not an error.
    {
        InMemorySessionStore store;
        auto loaded = load_agent_session_snapshot(store, "never-existed");
        check(loaded.has_value() && !loaded->has_value(),
              "P3: an id that was never saved reports std::nullopt, not a failure");
    }

    // P4: delete_session() tombstones the durable record AND clears in-process state; a post-delete
    // load sees no residue. Uses a non-trivial StateT (ported from test_agent_session_delete.cpp,
    // now retired) so this also proves clear_in_process_state() resets StateT to its own default,
    // not just session_id -- and every OTHER accessor (principal, history, metadata, last_run_id)
    // reports no residue either, matching that file's own C3-R3..R8 sweep.
    {
        struct ScratchState { int notes = 0; };
        AgentSession<OneShotChatClient, ScratchState> session;
        session.initialize("p4", Principal{"p4", "tenant-p4"});
        session.emplace_chat_client();
        session.state().notes = 42;
        session.metadata()["k"] = "v";
        InMemorySessionStore store;
        auto ran = drive(session.start_run(StartRun{user_message("hi")}));
        check(ran.has_value(), "P4 setup: the run converges");
        auto saved = drive(save_agent_session_snapshot(session, store));
        check(saved.has_value(), "P4 setup: a snapshot exists before deletion");

        auto receipt = drive(delete_session(session, store));
        check(receipt.has_value(), "P4: delete_session() succeeds");
        if (receipt.has_value()) {
            check(receipt->durable_record_removed, "P4: the receipt reports the durable half done");
            check(receipt->in_process_state_cleared, "P4: the receipt reports the in-process half done");
        }
        check(session.session_id().empty(), "P4: session_id is cleared, not just history");
        check(session.principal().id.empty() && session.principal().tenant_id.empty(),
              "P4: principal is cleared");
        check(session.history().empty(), "P4: history is cleared");
        check(session.state().notes == 0, "P4: state resets to StateT's own default, not just left as-is");
        check(session.metadata().empty(), "P4: metadata is cleared");
        check(session.last_run_id().empty(), "P4: run identity is cleared");

        auto loaded = load_agent_session_snapshot(store, "p4");
        check(loaded.has_value() && !loaded->has_value(),
              "P4: post-delete load returns std::nullopt -- indistinguishable from never-existed, "
              "matching the original's own 'no residue' read-path property");
    }

    // P5: checkpoint_if_due()'s cadence gate.
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("p5", Principal{"p5", ""});
        session.emplace_chat_client();
        InMemorySessionStore store;
        auto ran = drive(session.start_run(StartRun{user_message("hi")}));
        check(ran.has_value(), "P5 setup: the run converges");

        auto below_threshold = drive(checkpoint_if_due<CheckpointCadence<3>>(session, store, 1));
        check(below_threshold.has_value() && !*below_threshold,
              "P5: below the cadence threshold, checkpoint_if_due() reports false (skipped)");
        check(!store.exists("p5"), "P5: a skipped checkpoint writes nothing to the store");

        auto at_threshold = drive(checkpoint_if_due<CheckpointCadence<3>>(session, store, 3));
        check(at_threshold.has_value() && *at_threshold,
              "P5: at the cadence threshold, checkpoint_if_due() reports true (written)");
        check(store.exists("p5"), "P5: a due checkpoint actually writes a record to the store");
    }

    // P6: the in-flight guard -- snapshot_record() queues behind a genuinely in-flight start_run()
    // and observes POST-run state once released, never a torn read. See file header for the full
    // trace of why manually-timed resume() calls make this deterministic without real threads.
    {
        auto gate = agentengine::rt::make_channel<int>(1);
        AgentSession<SuspendingChatClient> session;
        session.initialize("p6", Principal{"p6", ""});
        session.emplace_chat_client(std::move(gate.consumer));

        // Explicitly agentengine::rt::task<T> here, NOT the bare `task` alias (which this file's own
        // `using agentengine::task;` binds to the OLD quark-based task, needed instead for
        // SuspendingChatClient::chat()'s own return type above) -- start_run()/snapshot_record() are
        // agentengine::rt::AgentSession members and return agentengine::rt::task<T>.
        agentengine::rt::task<agentengine::result<AgentResponse>> run_task =
            session.start_run(StartRun{user_message("hi")});
        run_task.resume();
        check(!run_task.done(),
              "P6: start_run() is genuinely in flight (parked inside chat() on the gate), not finished");

        agentengine::rt::task<AgentSessionRecord> snap_task = session.snapshot_record();
        snap_task.resume();
        check(!snap_task.done(),
              "P6: a concurrent snapshot_record() call queues behind session_mutex_ instead of "
              "reading state while the run is still in flight");

        auto push_result = gate.producer.push(0);
        check(push_result == agentengine::rt::channel_producer<int>::push_result::ok,
              "P6 setup: the gate accepted the push");

        check(run_task.done(), "P6: start_run() completed once the gate opened");
        check(snap_task.done(),
              "P6: the queued snapshot_record() was released and completed once start_run() finished "
              "and released session_mutex_ -- the guard actually gates on real completion, not just "
              "on the gate opening");

        agentengine::result<AgentResponse> run_result = run_task.take_value();
        check(run_result.has_value(), "P6: the gated run itself converged normally");

        AgentSessionRecord rec = snap_task.take_value();
        check(rec.run_counter == 1,
              "P6: the snapshot observed POST-run state (run_counter incremented) -- proof the guard "
              "prevented a torn read, not just an assumption that ordering happened to work out");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_snapshot: ALL PASS\n");
    return 0;
}
