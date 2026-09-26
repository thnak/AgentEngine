// Proof for ADR-037 Phase 2: porting the OLD, Quark-actor-based test_agent_session_checkpoint.cpp's
// D1 claim onto agentengine::rt::AgentSession (include/agentengine/rt/agent_session.hpp) directly.
// Deterministic, offline, no live model, no network.
//
// WHY THIS FILE EXISTS DESPITE test_rt_agent_session_snapshot.cpp's OWN P1: P1 already proves
// restore_from_record() reconstructs last_run_id from the restored run_counter (checked via the
// accessor immediately after restore_from_record() returns). That is MOST of D1's claim, but not all
// of it -- P1 never actually calls start_run() again on the restored session. The old D1's own text
// names the real point explicitly: "a restart must not remint a run_id that already happened" -- an
// END-TO-END claim about what the NEXT start_run() mints, not just about what restore_from_record()
// computes internally. Since restore_from_record() and start_run() both derive a run_id from
// run_counter_ via the identical `session_id_ + ":run:" + std::to_string(run_counter_)` formula, a
// bug that decoupled the two (e.g. restore_from_record() computing last_run_id_ from something other
// than the run_counter_ member it also sets) would slip past P1 but be caught here -- so this is a
// genuine, if narrow, additional positive control (CLAUDE.md: "a test that cannot fail proves
// nothing"), not a mechanical re-port of an already-proven claim. Judged NOT fully redundant; ported.

#include <cstdio>
#include <string>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/session_store.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::AgentSessionRecord;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::StartRun;
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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses.
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

// Converges on every call, deterministically -- this test only needs real run_counter/last_run_id
// movement, not round-loop mechanics (already proven elsewhere).
class OneShotChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message m;
        m.role = role::assistant;
        m.message_id = "m-reply";
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<OneShotChatClient>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;

    AgentSession<OneShotChatClient> session;
    session.initialize("s-ckpt", Principal{"p-leo", ""});
    session.emplace_chat_client();

    auto r1 = drive(session.start_run(StartRun{user_message("one")}));
    auto r2 = drive(session.start_run(StartRun{user_message("two")}));
    check(r1.has_value() && r2.has_value(), "setup: both turns succeed");
    check(session.last_run_id() == "s-ckpt:run:2", "setup: 2 turns run, last_run_id is s-ckpt:run:2");

    InMemorySessionStore store;
    auto saved = drive(save_agent_session_snapshot(session, store));
    check(saved.has_value(), "D1-R1: checkpointing after turn 2 succeeds");

    auto loaded = load_agent_session_snapshot(store, "s-ckpt");
    check(loaded.has_value() && loaded->has_value() && (*loaded)->run_counter == 2,
          "D1-R2: the checkpoint's own record carries the run's real position (run_counter=2)");

    AgentSession<OneShotChatClient> fresh;
    fresh.emplace_chat_client();
    fresh.restore_from_record(**loaded);
    check(fresh.last_run_id() == "s-ckpt:run:2",
          "D1-R3: restoring the checkpoint recovers last_run_id exactly, via the persisted "
          "run_counter, not a re-parsed string");

    // The real point of D1: a restart must not remint a run_id that already happened. This is the
    // one check P1 (test_rt_agent_session_snapshot.cpp) does NOT make -- it stops at checking the
    // restored accessor, never calling start_run() again to prove the mechanism end to end.
    auto r3 = drive(fresh.start_run(StartRun{user_message("three")}));
    check(r3.has_value() && fresh.last_run_id() == "s-ckpt:run:3",
          "D1-R4: after restoring from a checkpoint, the NEXT StartRun continues the ORIGINAL run "
          "sequence (run:3) -- never reminting run:1 or run:2, which already happened before "
          "whatever this checkpoint survived (001 §1)");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_checkpoint_restart: ALL PASS\n");
    return 0;
}
