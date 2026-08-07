// Milestone 4 Phase E2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 019
// §2's "A Suspended run holds no activation, no sandbox, no connection, no thread... measured by
// census, not asserted" had zero test coverage anywhere in this codebase before this task --
// EVERY prior test in this project (M1 through this milestone's own Phases A-D) drives
// `AgentSession` through `quark::TestKit`, which has no passivation to census in the first place.
// This is the first AgentEngine test anywhere to stand up a REAL `quark::Engine`, mirroring
// Quark's own `engine_passivate_test.cpp` exactly (same construction shape: a locally-owned
// `Activation` registered directly via `register_activation`, not `Engine::spawn<A>()`, which
// hands back only an `ActorId` -- no mutable reference AgentSession's own `initialize()` could
// use; see `agent_session.hpp`'s own comment on that gap).
//
// Proves, by CENSUS (Quark's `Activation::went_dormant()`/`state()`, never a comment asserting
// it): a real session's Quark activation reaches Dormant after `.passivate()` (E2's own claim),
// an open `Interaction` (Phase E1) -- the "durable record" half of 019 §2's "durable record plus a
// wake condition" -- survives the Dormant round-trip untouched, and the NEXT message reactivates
// the session with its in-process identity/history fully intact ("resumable", 001 §2).
//
// Named, not silently covered: `AgentSession`'s own `chat_client_`/`history_provider_` hold no
// live sandbox handle or network connection at this milestone's scope (every `ChatClient` in this
// project is still a mock, 004's real providers are M5's job) -- so "no sandbox, no connection" is
// vacuously true for what this session ACTUALLY holds today, not yet a proof that a future
// resource-holding ChatClient would also release its resources on Dormant.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

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

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
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

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("test_agent_session_suspend_resume: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(64);

    // `initialize()`'d BEFORE registration -- the only way to give a real Engine-hosted session its
    // identity, since Engine::spawn<A>() hands back no mutable reference (see this file's own top
    // comment).
    Session actor;
    actor.initialize("s-suspend", ae::Principal{"p-nora", ""});
    Activation act{&actor, Session::dispatch_table(), pool.sink()};
    eng.register_activation(actor_id_of<Session>(1), act);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Session> ref = router.get<Session>(1);
    eng.start();

    // --- A real turn against a real, scheduled Engine -------------------------------------------
    result<ae::AgentResponse> r1 = block_on(ref.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("hi", "m-1")}));
    check(r1.has_value(), "a real turn against a real Engine-hosted session succeeds", ok);
    check(actor.last_run_id() == "s-suspend:run:1", "the real turn minted the expected run_id", ok);

    // --- An Interaction is open before suspending -- 019 §2's "durable record" half -------------
    ae::Interaction const& interaction =
        actor.open_interaction(actor.last_run_id(), ae::interaction_reason::input);
    std::string const interaction_id = interaction.interaction_id;
    check(actor.has_open_interactions(), "an Interaction is open before passivating", ok);

    // --- Suspend: passivate on demand, CENSUS-prove Dormant (019 §2's own "measured by census, not
    // asserted" bar -- act.went_dormant() is Quark's own activation-lifecycle introspection, not a
    // comment) --------------------------------------------------------------------------------------
    check(ref.passivate(), "passivate() is accepted on a live, resolvable session", ok);
    check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
          "E2-R1: the session's Quark activation reaches Dormant -- no activation, matching 019 §2's "
          "own wording, proven by census",
          ok);
    check(!act.armed_deactivate_entry(),
          "E2-R2: no idle-timeout wheel entry was ever armed -- purely on-demand passivation, the "
          "same distinction Quark's own engine_passivate_test.cpp draws",
          ok);

    // --- The durable record (the open Interaction) survives the Dormant round-trip untouched ----
    check(actor.has_open_interactions() && actor.open_interactions().front().interaction_id == interaction_id,
          "E2-R3: the open Interaction survives passivation -- Suspended's own 'durable record', "
          "not lost while the activation holds nothing",
          ok);

    // --- Resume: the next message reactivates it, with identity/history fully intact -------------
    result<ae::AgentResponse> r2 =
        block_on(ref.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("again", "m-2")}));
    check(r2.has_value(), "E2-R4: a message posted to a Dormant session reactivates it and replies -- "
                          "'resumable' (001 §2), no loss",
          ok);
    check(actor.history().size() == 4, "E2-R5: history survived the Dormant round-trip intact (2 "
                                       "turns * 2 messages each)",
          ok);
    check(actor.last_run_id() == "s-suspend:run:2",
          "E2-R6: run identity survived the Dormant round-trip too -- the second run continues the "
          "ORIGINAL sequence, not reset by passivation",
          ok);

    // --- Resolving the interaction after resume works exactly like any other in-process state ---
    auto resolved = actor.resolve_interaction(interaction_id);
    check(resolved.has_value(), "E2-R7: resolving the interaction after resume succeeds", ok);
    check(!actor.has_open_interactions(), "E2-R8: no open interactions remain after resolution", ok);

    eng.stop();
    std::printf("test_agent_session_suspend_resume: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
