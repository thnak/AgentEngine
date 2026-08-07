// Milestone 4 Phase E3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md,
// decision 4): 019 §2's "Timer/schedule" suspension wake row ("Quark durable reminders (027) —
// at-least-once, wall-clock, mass-due-safe") is pure reuse, not new design -- Quark's own
// `ReminderService` is already Accepted and proven (ADR-017). What this task adds is the one real
// thing reminder_service.hpp's own top comment names as unbuilt: "wiring [a fire] into the live
// engine's reactivation path... is the engine-integration seam" -- AgentEngine's own glue,
// `TimerWake` (agent_session.hpp), plus this proof that a Quark reminder can target a session's
// REAL `session_actor_id()` (A1, reused unmodified) and actually reach it through a live
// `quark::Engine`, end to end.
//
// Deliberately NOT proven here: what a real "resume the paused run this timer was arming" would
// DO on wake. That needs 006 §6b's `schedule_wakeup`/`Backgroundable`, confirmed absent from this
// codebase (the M4 kickoff's own inventory table) -- a different, un-built vertical this task does
// not invent standing in for (see `TimerWake`'s own comment in agent_session.hpp).

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/reminder_service.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

using namespace quark;

namespace {

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

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

using Session = ae::AgentSession<CannedChatClient>;

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

constexpr std::int64_t kSec = 1'000'000'000LL;

} // namespace

int main() {
    bool ok = true;

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("test_agent_session_timer_wake: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(64);

    Session actor;
    actor.initialize("s-timer", ae::Principal{"p-priya", ""});
    Activation act{&actor, Session::dispatch_table(), pool.sink()};

    // Registered under `session_actor_id("s-timer")` (A1's real session-addressing, reused
    // unmodified) -- not an arbitrary/synthetic ActorId -- so the reminder below targets EXACTLY
    // what a real session's own address would be, not a stand-in.
    ActorId const session_id = ae::session_actor_id("s-timer");
    eng.register_activation(session_id, act);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Session> ref{session_id, &router};
    eng.start();

    check(actor.timer_wakes() == 0, "setup: no timer wakes before any reminder fires", ok);

    // --- Wire a fired reminder into a real tell() on the session's own lane (the "engine-
    // integration seam" reminder_service.hpp's own comment names as the caller's job) ------------
    InMemoryReminderStore store;
    ReminderConfig cfg;
    cfg.fire_rate = 0;  // fast path: fire everything due immediately (no mass-due smear needed here)
    ReminderService<InMemoryReminderStore> svc(
        store,
        [&](FireEvent const& e) {
            check(e.actor == session_id, "the fired reminder's actor id is the session's own ActorId", ok);
            ref.tell(ae::TimerWake{std::string(e.name)});
        },
        cfg);
    svc.open();

    std::int64_t const due = 100 * kSec;
    auto registered = svc.remind_at(session_id, "wake-1", WallInstant{due}, /*period_ns=*/0, {});
    check(registered.has_value(), "E3-R1: registering a durable reminder against the session's real "
                                  "ActorId succeeds",
          ok);

    // --- tick() past the due instant: fires synchronously, invoking the callback above -----------
    std::size_t const fired = svc.tick(WallInstant{due});
    check(fired == 1, "E3-R2: exactly one reminder fires at its due instant", ok);

    // --- The tell() crosses into the real Engine asynchronously -- wait for it to actually land --
    check(wait_until([&] { return actor.timer_wakes() == 1; }, std::chrono::seconds(2)),
          "E3-R3: the fired reminder's tell() actually reaches and is processed by the real "
          "Engine-hosted session -- reusing A1's session_actor_id() end to end through Quark's "
          "already-Accepted reminder mechanism, never a second wake mechanism of AgentEngine's own",
          ok);

    // --- A second turn against the SAME session still works normally after the wake -------------
    ae::ContentItem item{};
    item.value  = ae::Text{"hi"};
    item.origin = ae::content_origin::user;
    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = "m-1";
    input.content.push_back(item);
    result<ae::AgentResponse> r = block_on(ref.ask<ae::AgentResponse>(ae::StartRun{input}));
    check(r.has_value(), "E3-R4: an ordinary turn still works normally after a timer wake -- no "
                         "special state left behind",
          ok);

    eng.stop();
    std::printf("test_agent_session_timer_wake: %s (timer_wakes=%llu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(actor.timer_wakes()));
    return ok ? 0 : 1;
}
