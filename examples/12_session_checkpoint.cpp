// AgentEngine "get started" examples, 12 -- surviving a restart.
//
// Mirrors MAF's samples/03-workflows/Checkpoint. `AgentSession` persists its own durable identity
// (`AgentSessionRecord`: session_id, principal, run/turn position) through a real `quark::Store` via
// Quark's Snapshot model (005 §2) -- `save_agent_session_snapshot`/`load_agent_session_snapshot`
// (agent_session.hpp). Recovering a FRESH instance from a loaded record is what "surviving a
// restart" actually means here, so this example builds one session, snapshots it, throws the
// instance away, and reconstructs identity on a brand-new one -- not just re-reading the same object.
//
// Honestly scoped, matching the record's own narrowing: `history_`/`state`/`metadata` are NOT part
// of `AgentSessionRecord` yet (`Message`/`ContentItem` have no `QUARK_SERIALIZE` yet -- a named,
// real gap, not silently working). This example asserts that absence explicitly, the same way
// `tests/test_agent_session_snapshot.cpp` does, rather than only showing the part that round-trips.
//
// Run: ./agentengine_example_12_session_checkpoint

#include <cstdio>
#include <string>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"

using namespace agentengine;

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

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }  // unused
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

}  // namespace

int main() {
    using Session = AgentSession<CannedChatClient>;

    quark::InMemoryStore store;

    // Build a session, run a turn, then snapshot it under a real fence.
    quark::TestKit<Session> kit;
    kit.actor().initialize("s-checkpoint", Principal{"p-demo", "tenant-9"});
    auto turn = kit.ask<AgentResponse>(StartRun{user_message("hi")});
    check(turn.has_value(), "the run succeeds before checkpointing");
    if (turn.has_value()) std::printf("[before restart] %s (history size %zu)\n",
                                       text_of(turn->message).c_str(), kit.actor().history().size());

    auto const id    = session_actor_id(kit.actor().session_id());
    auto const fence = store.acquire_fence(id);
    check(save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence).has_value(),
          "the session snapshots under a real fence");

    // ---- Simulate a restart: throw the old instance's identity away, load a fresh one -------------
    auto loaded = load_agent_session_snapshot(store, "s-checkpoint");
    check(loaded.has_value() && loaded->has_value(), "the saved session loads back as a real record");

    if (loaded.has_value() && loaded->has_value()) {
        quark::TestKit<Session> fresh_kit;
        check(fresh_kit.actor().session_id().empty(),
              "the fresh instance starts with no identity of its own");

        fresh_kit.actor().restore_from_record(**loaded);
        check(fresh_kit.actor().session_id() == "s-checkpoint",
              "session_id round-trips bit-identical through the real Store");
        check(fresh_kit.actor().principal().id == "p-demo" &&
                  fresh_kit.actor().principal().tenant_id == "tenant-9",
              "principal (id + tenant_id) round-trips bit-identical");
        std::printf("[after restart]  session_id=%s principal=%s/%s\n",
                     std::string(fresh_kit.actor().session_id()).c_str(),
                     fresh_kit.actor().principal().id.c_str(),
                     fresh_kit.actor().principal().tenant_id.c_str());

        // The named, real gap: conversation history is NOT part of this record yet.
        check(fresh_kit.actor().history().empty(),
              "conversation history is NOT restored by this snapshot (a real, named gap -- "
              "Message/ContentItem have no QUARK_SERIALIZE yet) even though the original session's "
              "history had 2 messages before checkpointing");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_12_session_checkpoint: OK\n"
                                          : "example_12_session_checkpoint: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
