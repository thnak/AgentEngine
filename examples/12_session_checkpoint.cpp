// AgentEngine "get started" examples, 12 -- surviving a restart.
//
// Mirrors MAF's samples/03-workflows/Checkpoint. `rt::AgentSession` persists its own durable identity
// (`AgentSessionRecord`: session_id, principal, run/turn position) through `rt::SessionStore`
// (ADR-037's Quark-free replacement for `quark::Store`'s Snapshot model, 005 §2) --
// `save_agent_session_snapshot`/`load_agent_session_snapshot` (rt/agent_session.hpp). Recovering a
// FRESH instance from a loaded record is what "surviving a restart" actually means here, so this
// example builds one session, snapshots it, throws the instance away, and reconstructs identity on a
// brand-new one -- not just re-reading the same object.
//
// Honestly scoped, matching the record's own narrowing: `history_`/`state`/`metadata` are NOT part
// of `AgentSessionRecord` yet (the same narrowing the original, Quark-based record had -- this
// migration carried it forward unchanged, not silently working now). This example asserts that
// absence explicitly, the same way `tests/test_rt_agent_session_snapshot.cpp` does, rather than
// only showing the part that round-trips.
//
// Run: ./agentengine_example_12_session_checkpoint

#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::StartRun;
using agentengine::rt::load_agent_session_snapshot;
using agentengine::rt::save_agent_session_snapshot;

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

class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"ok"};
        item.origin = content_origin::assistant;

        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = Text{"ok"};
        upd.is_final     = true;
        upd.usage        = Usage{1, 1, 0, 0, 0.0};
        auto pushed      = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<CannedChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

// Drives an agentengine::rt::task<T> to completion. Safe here: CannedChatClient::chat() never
// suspends on anything external, and neither does save/load_agent_session_snapshot() against an
// InMemorySessionStore.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    using Session = AgentSession<CannedChatClient>;

    InMemorySessionStore store;

    // Build a session, run a turn, then snapshot it.
    Session session;
    session.initialize("s-checkpoint", Principal{"p-demo", "tenant-9"});
    session.emplace_chat_client();
    auto turn = drive(session.start_run(StartRun{user_message("hi")}));
    check(turn.has_value(), "the run succeeds before checkpointing");
    if (turn.has_value()) std::printf("[before restart] %s (history size %zu)\n",
                                       text_of(turn->message).c_str(), session.history().size());

    check(drive(save_agent_session_snapshot(session, store)).has_value(),
          "the session snapshots to the store");

    // ---- Simulate a restart: throw the old instance's identity away, load a fresh one -------------
    auto loaded = load_agent_session_snapshot(store, "s-checkpoint");
    check(loaded.has_value() && loaded->has_value(), "the saved session loads back as a real record");

    if (loaded.has_value() && loaded->has_value()) {
        Session fresh_session;
        fresh_session.emplace_chat_client();
        check(fresh_session.session_id().empty(),
              "the fresh instance starts with no identity of its own");

        fresh_session.restore_from_record(**loaded);
        check(fresh_session.session_id() == "s-checkpoint",
              "session_id round-trips bit-identical through the real store");
        check(fresh_session.principal().id == "p-demo" &&
                  fresh_session.principal().tenant_id == "tenant-9",
              "principal (id + tenant_id) round-trips bit-identical");
        std::printf("[after restart]  session_id=%s principal=%s/%s\n",
                     std::string(fresh_session.session_id()).c_str(),
                     fresh_session.principal().id.c_str(),
                     fresh_session.principal().tenant_id.c_str());

        // The named, real gap: conversation history is NOT part of this record yet.
        check(fresh_session.history().empty(),
              "conversation history is NOT restored by this snapshot (a real, named gap, carried "
              "forward unchanged from the original Quark-based record's own narrowing) even though "
              "the original session's history had 2 messages before checkpointing");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_12_session_checkpoint: OK\n"
                                          : "example_12_session_checkpoint: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
