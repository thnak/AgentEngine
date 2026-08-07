// Milestone 4 Phase H2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md), the
// roadmap's own M4 exit criterion (019 §7 G3): "Suspended holds zero resources." Phase E2
// (test_agent_session_suspend_resume.cpp) already proved this by census (Quark's own
// `went_dormant()`/`armed_deactivate_entry()`) -- but IN ISOLATION: it passivated and then resumed
// the SAME in-process `AgentSession` object, so the open Interaction it found intact after
// Dormant could, in principle, have merely survived because the C++ object was never actually
// destroyed (passivation releases Quark's own scheduling/runtime resources -- the activation --
// not necessarily the actor's own memory).
//
// This phase closes that gap end to end: checkpoint (Phase D1's `save_agent_session_snapshot`) →
// passivate to Dormant (census-proven, E2's own mechanism) → the ENTIRE Engine/Activation/actor
// object is destroyed (simulating the process actually exiting, not merely idling) → a genuinely
// NEW Engine, Activation, and `AgentSession` object is constructed from nothing but the durable
// checkpoint and reactivated. If the durable record (open Interactions, run position) still comes
// back correctly through a reconstruction this thorough, "Suspended holds zero resources" and "a
// session survives restart" are proven TOGETHER, not as two separately-true-in-isolation claims.

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

using namespace quark;

namespace {

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value  = ae::Text{"run=" + ctx.run_id};
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

using Session = ae::AgentSession<CannedChatClient>;

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

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    InMemoryStore durable_store;  // the ONLY thing that survives the "process exit" below

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("test_suspended_zero_resources_e2e: FAIL (config build failed)\n");
        return 1;
    }

    std::string interaction_id;
    quark::FenceToken fence_a{};

    // === Process A: a real Engine-hosted session runs a turn, opens an Interaction, checkpoints,
    // is passivated to Dormant (census-proven), then is torn down completely ======================
    {
        Engine<> eng(*built);
        detail::MessagePool pool(64);

        Session actor;
        actor.initialize("s-h2", ae::Principal{"p-priya", ""});
        Activation act{&actor, Session::dispatch_table(), pool.sink()};
        eng.register_activation(actor_id_of<Session>(1), act);

        LocalRouter router(eng.post_courier(), pool);
        ActorRef<Session> ref = router.get<Session>(1);
        eng.start();

        result<ae::AgentResponse> r1 =
            block_on(ref.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")}));
        check(r1.has_value(), "a real turn against process A's Engine succeeds", ok);
        check(actor.last_run_id() == "s-h2:run:1", "process A minted run:1", ok);

        ae::Interaction const& interaction =
            actor.open_interaction(actor.last_run_id(), ae::interaction_reason::input);
        interaction_id = interaction.interaction_id;

        // --- The checkpoint boundary: this is the ONLY copy of the Interaction/run-position that
        // outlives process A ------------------------------------------------------------------------
        auto const id = ae::session_actor_id(actor.session_id());
        fence_a        = durable_store.acquire_fence(id);
        auto saved = ae::save_agent_session_snapshot(act, durable_store, actor, fence_a);
        check(saved.has_value(), "H2 setup: checkpointing before suspension succeeds", ok);

        // --- Suspend: census-proven Dormant, E2's own already-Judged mechanism ---------------------
        check(ref.passivate(), "passivate() is accepted on a live, resolvable session", ok);
        check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
              "H2-R1: process A's activation reaches Dormant before teardown -- the suspended "
              "state this test is about to destroy really was resource-free, not merely idle",
              ok);
        check(!act.armed_deactivate_entry(),
              "H2-R2: no idle-timeout wheel entry was armed -- on-demand passivation only, "
              "matching E2's own distinction",
              ok);

        eng.stop();
        // `eng`, `pool`, `router`, `act`, and `actor` are ALL destroyed here, at scope exit --
        // simulating the process actually exiting while Suspended, not merely sleeping.
    }

    // === Process B: nothing survives from process A except `durable_store`. A genuinely NEW
    // Engine, Activation, and AgentSession object is built from only the checkpoint, and reactivated
    // ================================================================================================
    auto loaded = ae::load_agent_session_snapshot(durable_store, "s-h2");
    check(loaded.has_value() && loaded->has_value(),
          "H2-R3: the checkpoint written before process A's teardown is readable by process B, "
          "which shares nothing else with process A",
          ok);

    auto built_b = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built_b.has_value(), "process B's own ConfigBuilder produces a valid EngineConfig", ok);
    if (!built_b || !loaded || !loaded->has_value()) {
        std::printf("test_suspended_zero_resources_e2e: FAIL (process B setup failed)\n");
        return 1;
    }

    Engine<> eng_b(*built_b);
    detail::MessagePool pool_b(64);

    Session actor_b;  // a BRAND NEW object -- not the one from process A, which no longer exists
    actor_b.restore_from_record(**loaded);
    check(actor_b.session_id() == "s-h2" && actor_b.last_run_id() == "s-h2:run:1",
          "H2-R4: the reconstructed session's identity and run position come back correctly "
          "through nothing but the durable checkpoint",
          ok);
    check(actor_b.has_open_interactions() &&
              actor_b.open_interactions().front().interaction_id == interaction_id,
          "H2-R5: the Interaction opened by process A -- Suspended's own 'durable record' half -- "
          "survives a REAL destroy-and-reconstruct cycle, not just an in-memory resume of the same "
          "object (closing E2's own named residual)",
          ok);

    Activation act_b{&actor_b, Session::dispatch_table(), pool_b.sink()};
    eng_b.register_activation(actor_id_of<Session>(1), act_b);
    LocalRouter router_b(eng_b.post_courier(), pool_b);
    ActorRef<Session> ref_b = router_b.get<Session>(1);
    eng_b.start();

    // --- Resume: the next message reactivates the reconstructed session, continuing the ORIGINAL
    // run sequence, never reminting run:1 -----------------------------------------------------------
    result<ae::AgentResponse> r2 =
        block_on(ref_b.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("again", "m-2")}));
    check(r2.has_value(), "H2-R6: process B's reconstructed session accepts a new turn and replies",
          ok);
    check(actor_b.last_run_id() == "s-h2:run:2",
          "H2-R7: the resumed run continues the original sequence (run:2) -- the full "
          "checkpoint -> Dormant -> destroy -> reconstruct -> reactivate cycle preserves run "
          "identity end to end",
          ok);

    auto resolved = actor_b.resolve_interaction(interaction_id);
    check(resolved.has_value(), "H2-R8: resolving the restored interaction after reconstruction "
                                "works exactly like any other in-process state",
          ok);
    check(!actor_b.has_open_interactions(), "H2-R9: no open interactions remain after resolution", ok);

    eng_b.stop();
    std::printf("test_suspended_zero_resources_e2e: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
