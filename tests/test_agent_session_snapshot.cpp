// Milestone 4 Phase A4, narrowed per the project owner's own kick-off decision
// (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): before this task,
// AgentSession had no persistence at all — 005 §2's "the store is Quark 012's Store seam" was
// unbuilt. This proves the narrowed record (session_id/principal/created_at/updated_at) round-
// trips through a real Quark Store via the Snapshot model, mirroring Quark's own
// persistence_snapshot_roundtrip_test.cpp precedent — the first real use of snapshot_sequential/
// recover_snapshot anywhere in AgentEngine. `history[]`/`state`/`metadata` are deliberately NOT
// persisted this pass (Message/ContentItem have no QUARK_SERIALIZE yet, a named, real gap, not a
// silent omission) — this test asserts that absence explicitly rather than leaving it unchecked.

#include <iostream>
#include <memory_resource>
#include <optional>
#include <string>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value  = ae::Text{"ok"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"ok"};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient>;

    quark::InMemoryStore store;

    // --- A never-snapshotted session recovers to nullopt, not an error (012 §Recovery) ----------
    auto never_snapshotted = ae::load_agent_session_snapshot(store, "s-snap-fresh");
    AE_CHECK(never_snapshotted.has_value() && !never_snapshotted->has_value(),
             "A4-C1: a session never snapshotted loads as nullopt, not an error");

    // --- Save under a real fence, at a real (trivially-quiescent, Sequential) consistent point --
    quark::TestKit<Session> kit;
    kit.actor().initialize("s-snap-1", ae::Principal{"p-frank", "tenant-9"});
    // Run a turn so history/run identity are non-trivial — proves below that they are NOT what
    // gets persisted (the narrowed scope), not merely that nothing was exercised.
    auto turn = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")});
    AE_CHECK(turn.has_value(), "setup: a turn against the mock succeeds");
    AE_CHECK(kit.actor().history().size() == 2, "setup: history is non-trivial before snapshotting");

    auto const id = ae::session_actor_id(kit.actor().session_id());
    auto const fence = store.acquire_fence(id);
    auto saved = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence);
    AE_CHECK(saved.has_value(), "A4-R1: save_agent_session_snapshot persists under a real fence");

    // --- Recover into a FRESH instance, simulating a restart --------------------------------------
    auto loaded = ae::load_agent_session_snapshot(store, "s-snap-1");
    AE_CHECK(loaded.has_value() && loaded->has_value(),
             "A4-R2: the saved session loads back as a real record, not nullopt");

    if (loaded->has_value()) {
        quark::TestKit<Session> fresh_kit;
        AE_CHECK(fresh_kit.actor().session_id().empty(),
                 "setup: the fresh instance starts with no session_id, proving restore below is "
                 "what actually sets it");
        fresh_kit.actor().restore_from_record(**loaded);

        AE_CHECK(fresh_kit.actor().session_id() == "s-snap-1",
                 "A4-R3: session_id round-trips bit-identical through the real Store");
        AE_CHECK(fresh_kit.actor().principal().id == "p-frank" &&
                     fresh_kit.actor().principal().tenant_id == "tenant-9",
                 "A4-R4: principal (id + tenant_id) round-trips bit-identical");

        // --- The narrowed scope's own explicit boundary: history/state/metadata are NOT restored,
        // because they were never part of this record. Asserted, not left implicit.
        AE_CHECK(fresh_kit.actor().history().empty(),
                 "A4-R5: history is explicitly NOT restored by this narrowed snapshot (a named "
                 "gap, not silently working) — the fresh instance has none, even though the "
                 "original session's history had 2 messages");
    }

    // --- A second snapshot overwrites the latest state (005 §2's "latest-state model") ----------
    kit.actor().initialize("s-snap-1", ae::Principal{"p-frank-renamed", "tenant-9"});
    auto saved_again = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence);
    AE_CHECK(saved_again.has_value(), "setup: second snapshot under the same fence succeeds");

    auto loaded_again = ae::load_agent_session_snapshot(store, "s-snap-1");
    AE_CHECK(loaded_again.has_value() && loaded_again->has_value() &&
                 (*loaded_again)->principal_id == "p-frank-renamed",
             "A4-R6: recovery returns the LATEST snapshot, not the first one");

    std::cout << (g_failures == 0 ? "test_agent_session_snapshot: OK\n"
                                   : "test_agent_session_snapshot: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
