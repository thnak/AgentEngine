// Milestone 4 Phase E4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// 019 §4's "Node loss: Quark placement (010/026) reactivates the session elsewhere; fencing
// prevents two activations of the same session, which is the split-brain that would corrupt
// history" -- pure reuse of Quark's already-proven fencing mechanism (`Store::acquire_fence`),
// applied through AgentEngine's OWN checkpoint path (Phase A4/D1's `save_agent_session_snapshot`,
// unmodified). Proves: once a second node re-acquires the fence for a session (simulating
// re-placement after node loss), the FIRST node's own zombie activation can no longer commit --
// its stale fence is rejected -- while the new owner's fence still writes successfully.

#include <iostream>
#include <memory_resource>
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
    quark::TestKit<Session> kit;
    kit.actor().initialize("s-fence", ae::Principal{"p-quinn", ""});
    auto turn = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")});
    AE_CHECK(turn.has_value(), "setup: a turn against the mock succeeds");

    auto const id = ae::session_actor_id(kit.actor().session_id());

    // --- "Node A" acquires the fence and checkpoints successfully ------------------------------
    auto const fence_node_a = store.acquire_fence(id);
    auto saved_by_a = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence_node_a);
    AE_CHECK(saved_by_a.has_value(), "E4-R1: node A's initial checkpoint under its own fence succeeds");

    // --- Node loss: Quark's placement reactivates the session elsewhere ("node B") --------------
    auto const fence_node_b = store.acquire_fence(id);
    AE_CHECK(fence_node_b != fence_node_a,
             "setup: re-acquiring the fence for the same ActorId yields a strictly different token "
             "(the fencing contract, quark/core/persistence.hpp)");

    // --- Node A's own activation is now a ZOMBIE -- its checkpoint write must be REJECTED --------
    auto turn2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("again", "m-2")});
    AE_CHECK(turn2.has_value(), "setup: node A's zombie activation still runs a (locally-successful) "
                                "turn -- the split-brain scenario 019 §4 is worried about");
    auto zombie_write = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence_node_a);
    AE_CHECK(!zombie_write.has_value(),
             "E4-R2: node A's zombie activation's commit under its now-stale fence is REJECTED -- "
             "the exact split-brain-prevention property 019 §4 names, proven through AgentEngine's "
             "own checkpoint path, not asserted separately from it");

    // --- Node B (the current, legitimate owner) still checkpoints successfully -------------------
    auto saved_by_b = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence_node_b);
    AE_CHECK(saved_by_b.has_value(),
             "E4-R3: the current owner's (node B's) fence still writes successfully -- fencing "
             "rejects the ZOMBIE specifically, not every writer");

    auto loaded = ae::load_agent_session_snapshot(store, "s-fence");
    AE_CHECK(loaded.has_value() && loaded->has_value() && (*loaded)->run_counter == 2,
             "E4-R4: the durable record reflects node B's (the legitimate owner's) write, never "
             "the rejected zombie write");

    std::cout << (g_failures == 0 ? "test_agent_session_node_loss_fencing: OK\n"
                                   : "test_agent_session_node_loss_fencing: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
