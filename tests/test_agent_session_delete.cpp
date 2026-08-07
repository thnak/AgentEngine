// Milestone 4 Phase C3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 005
// §6's "Delete — hard removal, including derived artifacts and recordings, with a completion
// receipt" had no implementation before this task. Proves both halves the receipt names: the
// in-process actor is cleared to fresh-construction defaults (no accessor reports anything from
// before the delete), and the durable snapshot becomes unreadable through the SAME
// load_agent_session_snapshot() free function every other Phase A/C test already uses -- "no
// residue," provable at this project's own read path, not asserted separately from it.

#include <iostream>
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

    int chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return 0; }  // unconstrained, unused
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

struct ScratchState {
    int notes = 0;
};

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
    using Session = ae::AgentSession<CannedChatClient, ScratchState>;

    quark::InMemoryStore store;
    quark::TestKit<Session> kit;
    kit.actor().initialize("s-delete", ae::Principal{"p-kay", "tenant-2"});
    kit.actor().state().notes = 42;
    kit.actor().metadata()["k"] = "v";
    auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")});
    AE_CHECK(r1.has_value(), "setup: turn against the mock succeeds");

    auto const id    = ae::session_actor_id(kit.actor().session_id());
    auto const fence = store.acquire_fence(id);
    auto saved = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence);
    AE_CHECK(saved.has_value(), "setup: the session snapshots successfully before deletion");

    auto before_delete = ae::load_agent_session_snapshot(store, "s-delete");
    AE_CHECK(before_delete.has_value() && before_delete->has_value(),
             "setup: the snapshot is readable before deletion");

    auto receipt = ae::delete_session(kit.activation(), store, kit.actor(), fence);
    AE_CHECK(receipt.has_value(), "C3-R1: delete_session() succeeds");
    AE_CHECK(receipt.has_value() && receipt->session_id == "s-delete" &&
                 receipt->durable_record_removed && receipt->in_process_state_cleared,
             "C3-R2: the receipt names BOTH halves as done (durable record removed, in-process "
             "state cleared), naming which of the two happened rather than a single opaque bool");

    // --- In-process half: nothing survives through ANY accessor --------------------------------
    AE_CHECK(kit.actor().session_id().empty(), "C3-R3: session_id is cleared, not just history");
    AE_CHECK(kit.actor().principal().id.empty() && kit.actor().principal().tenant_id.empty(),
             "C3-R4: principal is cleared");
    AE_CHECK(kit.actor().history().empty(), "C3-R5: history is cleared");
    AE_CHECK(kit.actor().state().notes == 0, "C3-R6: state resets to StateT's own default");
    AE_CHECK(kit.actor().metadata().empty(), "C3-R7: metadata is cleared");
    AE_CHECK(kit.actor().last_run_id().empty(), "C3-R8: run identity is cleared");

    // --- Durable half: the SAME load path every other test uses reports nothing, exactly like a
    // session that never existed -- "no residue" is provable through this project's own read path,
    // not merely asserted as a separate claim -------------------------------------------------------
    auto after_delete = ae::load_agent_session_snapshot(store, "s-delete");
    AE_CHECK(after_delete.has_value() && !after_delete->has_value(),
             "C3-R9: after deletion, load_agent_session_snapshot() reports nullopt -- "
             "indistinguishable from a session that was never snapshotted at all");

    std::cout << (g_failures == 0 ? "test_agent_session_delete: OK\n" : "test_agent_session_delete: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
