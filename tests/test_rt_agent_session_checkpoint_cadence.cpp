// Proof for ADR-037 Phase 2: porting the OLD, Quark-actor-based test_agent_session_checkpoint.cpp's
// D2 claim onto agentengine::rt::AgentSession (include/agentengine/rt/agent_session.hpp) directly.
// Deterministic, offline, no live model, no network.
//
// WHY THIS FILE EXISTS DESPITE test_rt_agent_session_snapshot.cpp's OWN P5: P5 already proves the
// basic cadence gate with CheckpointCadence<3> at two sample points (below threshold at count=1,
// at-threshold at count=3). Judged as genuinely ADDITIONAL coverage, not a repeat: this file runs the
// SAME mechanism across a realistic 7-turn sequence and checks the EXACT fire pattern (skip, skip,
// fire, skip, skip, fire, skip -- turns 3 and 6, never any other turn) rather than two isolated
// samples, and it additionally proves the "honest lag" property P5 never touches at all: after turn
// 7, the durable checkpoint still reflects turn 6's position (run_counter=6) while the LIVE
// in-process session has already moved on to run 7 -- the observable, un-hidden trade-off a cadence
// > 1 creates (019 §1's "cost is bounded"). That gap-between-live-and-durable-state check is a real,
// distinct claim P5's two-sample-point proof cannot make on its own.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/session_store.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::CheckpointCadence;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::StartRun;
using agentengine::rt::checkpoint_if_due;
using agentengine::rt::load_agent_session_snapshot;
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

// Converges on every call, deterministically -- this test only needs real run_counter movement
// across many sequential start_run() calls, not round-loop mechanics.
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
    session.initialize("s-cadence", Principal{"p-mia", ""});
    session.emplace_chat_client();
    InMemorySessionStore store;

    std::uint64_t since_last_checkpoint = 0;
    std::vector<bool> fired;
    for (int turn = 1; turn <= 7; ++turn) {
        auto turn_result = drive(session.start_run(StartRun{user_message("t" + std::to_string(turn))}));
        check(turn_result.has_value(), ("D2 setup: turn " + std::to_string(turn) + " succeeds").c_str());
        ++since_last_checkpoint;

        auto due = drive(checkpoint_if_due<CheckpointCadence<3>>(session, store, since_last_checkpoint));
        check(due.has_value(), "D2 setup: checkpoint_if_due never errors against InMemorySessionStore");
        bool const did_fire = due.has_value() && *due;
        fired.push_back(did_fire);
        if (did_fire) since_last_checkpoint = 0;
    }

    check(fired.size() == 7 &&
              !fired[0] && !fired[1] && fired[2] &&  // turns 1,2 skip, turn 3 fires
              !fired[3] && !fired[4] && fired[5] &&  // turns 4,5 skip, turn 6 fires
              !fired[6],                              // turn 7 skips
          "D2-R1: CheckpointCadence<3> fires at exactly turns 3 and 6 out of 7 -- 5 writes skipped, "
          "not every turn hitting the Store (019 §1's 'cost is bounded')");

    auto loaded = load_agent_session_snapshot(store, "s-cadence");
    check(loaded.has_value() && loaded->has_value() && (*loaded)->run_counter == 6,
          "D2-R2: after turn 7, the durable checkpoint still reflects turn 6's position "
          "(run_counter=6) -- an HONEST lag the cadence policy creates, never silently advanced to "
          "look current");
    check(session.last_run_id() == "s-cadence:run:7",
          "D2-R3: meanwhile the LIVE in-process session is already at run 7 -- the gap between live "
          "state and the last written checkpoint is exactly what a cadence > 1 means, and it's "
          "observable here, not hidden");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_checkpoint_cadence: ALL PASS\n");
    return 0;
}
