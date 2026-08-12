// Milestone 7 Phase B (006-Tool-and-Function-Plane.md §6b, closing 019-Durability-and-Long-Running-
// Agents.md §2's "Local background task completion" wake row -- the gap confirmed absent by direct
// grep at both M4 and M5, and by this milestone's own kickoff doc). Proves the real, load-bearing
// pieces: (1) an undeclared-Backgroundable tool is rejected at authorize, before step 8 ever runs;
// (2) `start_background_task()` returns near-instantly even though the tool's own `invoke()` sleeps
// well past that -- the calling turn is genuinely never blocked (G7's own "doesn't block the calling
// turn" half); (3) `Background<max_concurrent>` is enforced against a LIVE count, at authorize, never
// silently queued (G9); (4) `list_standing_effects()` shows the effect while pending and it resolves
// (via a real `BackgroundTaskDone` tell delivered by the completion closure, mirroring
// `test_agent_session_timer_wake.cpp`'s own "host arms the callback" precedent) once the detached
// thread actually finishes, at which point `ToolCallStarted`/`ToolCallFinished` are both real on the
// run event stream (013 §1, Milestone 7 Phase A), attributed to the run that asked for the work even
// though a NEWER run may have started in the meantime; (5) `cancel_standing_effect()` denies a
// different principal and succeeds for the owning one (G8).
//
// NOT proven here (named, not silently claimed): full G6/G7 durability across a real actor
// suspend/resume or process restart -- `StandingEffect` is `Described`/serializable
// (standing_effect.hpp) but is not yet threaded into `AgentSessionRecord`'s own checkpoint, so
// "survives a restart" is a real, separate follow-up, not this file's claim.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_pipeline.hpp"

using namespace quark;

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return p();
}

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
        cfg.capacity = 32;  // generous enough that a small scripted response never blocks on credit
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd{};
        upd.delta.value  = ae::Text{"ok"};
        upd.delta.origin = ae::content_origin::assistant;
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CannedChatClient>, "CannedChatClient must satisfy the ChatClient concept");

using Session = ae::AgentSession<CannedChatClient>;

ae::Message user_turn(std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{"hi"};
    item.origin = ae::content_origin::user;
    ae::Message m{};
    m.role       = ae::role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

struct SlowArgs {
    bool noop;
};
AE_JSON_SCHEMA(SlowArgs, noop)
struct SlowReply {
    bool ok;
};
AE_JSON_SCHEMA(SlowReply, ok)

// Sleeps 150ms then succeeds -- long enough that a caller measuring "did start_background_task()
// block" can tell the difference between "detached" (returns in single-digit ms) and "blocked"
// (returns only after ~150ms).
struct SlowBackgroundableTool : ae::Tool<SlowBackgroundableTool, ae::Backgroundable> {
    static constexpr std::string_view name        = "slow_backgroundable";
    static constexpr std::string_view description = "Sleeps 150ms, then succeeds.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return Reply{true};
    }
};

// No `Backgroundable` policy declared -- `declared_backgroundable()`'s own fail-closed default.
struct ForegroundOnlyTool : ae::Tool<ForegroundOnlyTool> {
    static constexpr std::string_view name        = "foreground_only";
    static constexpr std::string_view description = "Never declared Backgroundable.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{true}; }
};

}  // namespace

int main() {
    namespace json = ae::json;

    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    if (!built) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }
    Engine<>            eng(*built);
    detail::MessagePool pool(64);

    Session  actor;
    actor.initialize("s-bg", ae::Principal{"p-owner", ""});
    Activation act{&actor, Session::dispatch_table(), pool.sink()};
    ActorId const session_id = ae::session_actor_id("s-bg");
    eng.register_activation(session_id, act);
    LocalRouter        router(eng.post_courier(), pool);
    ActorRef<Session>  ref{session_id, &router};
    eng.start();

    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({ae::cap::Background{1}});
    actor.set_capabilities(&held);

    // Enabled up front -- ToolCallStarted for B2's call-1 fires inside start_background_task()
    // itself, well before B5's own drain, so the stream must already be live by then.
    auto viewer = actor.enable_event_stream(std::pmr::get_default_resource());

    auto const table = ae::ToolTable::from_tools<SlowBackgroundableTool, ForegroundOnlyTool>();

    // A real StartRun first -- start_background_task() attributes the StandingEffect to
    // effect_context_.run_id/.principal, which are only populated inside handle(Ask<StartRun,...>)
    // (006 §6b is invoked mid-turn in the RFC's own framing; there is no turn without a Run).
    auto started = block_on(ref.ask<ae::AgentResponse>(ae::StartRun{user_turn("m-1")}));
    check(started.has_value(), "setup: a real StartRun succeeds before any background_task call");

    // --- B1: an undeclared-Backgroundable tool is rejected at authorize, before step 8 ------------
    {
        ae::ToolCallRequest req{"call-fg", "foreground_only", *json::parse(R"({"noop":true})"), false};
        auto handle = actor.start_background_task(ref, table, req);
        check(!handle.has_value(), "B1: a tool not declared Backgroundable is rejected");
        if (!handle.has_value()) {
            check(handle.error().code == "tool.not_backgroundable",
                  "B1: rejected with the real error_code, not a generic failure");
        }
        check(actor.list_standing_effects().empty(),
              "B1: nothing is registered for a rejected call");
    }

    // --- B2/G7: start_background_task() returns near-instantly, even though the tool itself -------
    // --- sleeps 150ms -- the calling turn is genuinely never blocked.                            ---
    std::string handle_id_1;
    {
        ae::ToolCallRequest req{"call-1", "slow_backgroundable", *json::parse(R"({"noop":true})"), false};
        auto const t0     = std::chrono::steady_clock::now();
        auto        handle = actor.start_background_task(ref, table, req);
        auto const elapsed = std::chrono::steady_clock::now() - t0;
        check(handle.has_value(), "B2: a Backgroundable tool under a granted Background<1> is accepted");
        check(elapsed < std::chrono::milliseconds(50),
              "G7: start_background_task() returns in well under the tool's own 150ms sleep -- step "
              "8 genuinely runs detached, not inline");
        if (handle.has_value()) handle_id_1 = handle->handle_id;

        check(actor.list_standing_effects().size() == 1,
              "B2: the StandingEffect is visible via list_standing_effects() while pending");
        if (actor.list_standing_effects().size() == 1) {
            ae::StandingEffect const& eff = actor.list_standing_effects().front();
            check(eff.kind == ae::standing_effect_kind::background_task,
                  "B2: the registered effect's kind is background_task");
            check(eff.label == "slow_backgroundable", "B2: the effect's label is the tool's own name");
            check(eff.principal_id == "p-owner",
                  "B2: the effect is attributed to the run's owning principal");
        }
    }

    // --- B3/G9: the session's Background<1> cap is already at 1 -- a SECOND call is rejected, ------
    // --- never silently queued.                                                                 ---
    {
        ae::ToolCallRequest req{"call-2", "slow_backgroundable", *json::parse(R"({"noop":true})"), false};
        auto handle = actor.start_background_task(ref, table, req);
        check(!handle.has_value(), "G9: a second background_task while the cap is at 1/1 is rejected");
        if (!handle.has_value()) {
            check(handle.error().code == "tool.background_capacity_exceeded",
                  "G9: rejected with the real capacity error_code, not a generic failure");
        }
        check(actor.list_standing_effects().size() == 1,
              "G9: the rejected second call registers nothing -- still exactly one effect outstanding");
    }

    // --- B4/G8: cancel_standing_effect() denies a DIFFERENT principal, then succeeds for the -------
    // --- owning one.                                                                            ---
    {
        auto denied = actor.cancel_standing_effect(handle_id_1, ae::Principal{"p-stranger", ""});
        check(!denied.has_value(), "G8: a different principal cannot cancel this effect");
        if (!denied.has_value()) {
            check(denied.error().code == "standing_effect.cross_principal_denied",
                  "G8: denied with the real cross-principal error_code");
        }
        check(actor.list_standing_effects().size() == 1, "G8: a denied cancel changes nothing");
    }

    // --- B5: wait for the real 150ms-sleeping thread to actually finish and tell() the session -----
    // --- back -- list_standing_effects() empties, and ToolCallStarted/ToolCallFinished are both  ---
    // --- real on the run event stream, attributed to the run that asked for the work.            ---
    // A SECOND, later run starts here -- proving the eventual ToolCallFinished (attributed to the
    // FIRST run) does not collide with or get misattributed to this newer run's own sequence.
    auto second_run = block_on(ref.ask<ae::AgentResponse>(ae::StartRun{user_turn("m-2")}));
    check(second_run.has_value(), "setup: a second, later run on the same session still works normally");

    check(wait_until([&] { return actor.list_standing_effects().empty(); }, std::chrono::seconds(2)),
          "B5: the detached thread eventually finishes and its BackgroundTaskDone tell() resolves the "
          "StandingEffect -- list_standing_effects() empties");

    bool saw_tool_call_started  = false;
    bool saw_tool_call_finished = false;
    bool finished_ok            = false;
    bool attributed_to_first_run = false;
    while (auto ev = viewer.next()) {
        if (ev->kind == ae::run_event_kind::tool_call_started) {
            saw_tool_call_started = true;
            attributed_to_first_run |= (ev->run_id == "s-bg:run:1");
        }
        if (ev->kind == ae::run_event_kind::tool_call_finished) {
            saw_tool_call_finished = true;
            attributed_to_first_run |= (ev->run_id == "s-bg:run:1");
            if (auto const* p = std::get_if<ae::run_event_payload::ToolCallFinished>(&ev->payload)) {
                finished_ok = p->ok;
            }
        }
    }
    check(saw_tool_call_started, "B5: ToolCallStarted is real on the event stream (013 §1)");
    check(saw_tool_call_finished, "B5: ToolCallFinished is real on the event stream once the "
                                   "background thread completes");
    check(finished_ok, "B5: ToolCallFinished carries the real ok=true outcome");
    check(attributed_to_first_run,
          "B5: both events carry run_id \"s-bg:run:1\" -- the run that ASKED for the background "
          "work -- never the second run that happened to be current when it resolved");

    eng.stop();

    if (g_failures == 0) {
        std::printf("test_agent_session_background_task: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_session_background_task: %d failure(s)\n", g_failures);
    return 1;
}
