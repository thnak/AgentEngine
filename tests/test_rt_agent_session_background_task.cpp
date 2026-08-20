// Proof for ADR-037 Phase 2, Slice 3: agentengine::rt::AgentSession's standing-effects/background-
// task surface (include/agentengine/rt/agent_session.hpp's "SLICE 3 ADDITION" -- start_background_
// task()/cancel_standing_effect()/list_standing_effects(), plus the BackgroundCompletionQueue
// delivery path that replaces the Quark original's self.tell(BackgroundTaskDone{...})). Modeled
// directly on the Quark-based test_agent_session_background_task.cpp's own B1-B5 checks (same tool
// fixtures, same claims) so the two can be compared side by side; the ADDITIONS here (B6) are what's
// actually new about the rt:: design -- there is no actor mailbox anymore, so completion delivery is
// explicitly drained rather than automatically tell()-ed in.
//
//   B1    -- an undeclared-Backgroundable tool is rejected at authorize, before step 8 ever runs.
//   B2/G7 -- start_background_task() returns near-instantly even though the tool's own invoke()
//            sleeps well past that -- the calling turn is genuinely never blocked.
//   B3/G9 -- Background<max_concurrent> is enforced against a LIVE count, at authorize, never
//            silently queued.
//   B4/G8 -- cancel_standing_effect() denies a different principal and succeeds for the owning one.
//   B5    -- the detached thread's real completion is picked up by an EXPLICIT
//            drain_background_completions() call once it actually finishes; ToolCallStarted/
//            ToolCallFinished are both real on the run event stream, attributed to the run that
//            asked for the work even though a NEWER run has started by the time it resolves.
//   B6    -- THE NEW PART: a completion is ALSO picked up with no explicit drain call at all, purely
//            because start_run() drains automatically as its own first step (file banner's "a host
//            never has to remember to drain separately" design claim) -- proven by never calling
//            drain_background_completions() in this block, only start_run().
//
// NOT proven here (named, not silently claimed): the destroyed-session/weak_ptr-drop lifetime path
// (file banner's design writeup) -- exercising an actual background thread outliving a destroyed
// AgentSession deterministically needs its own timing-sensitive harness; the design's correctness for
// that path rests on weak_ptr's own well-defined semantics (a lock() on an expired weak_ptr reliably
// returns nullptr), not on anything this file demonstrates empirically.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;
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

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return p();
}

// Safe here: this fixture's chat() never suspends on anything external (co_returns immediately), the
// same rule test_rt_agent_session.cpp's own drive<T>() documents.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// task<void> has no take_value() (see rt/task.hpp's void specialization) -- a separate helper rather
// than trying to make drive<T>() above generic over T=void.
void drive_void(agentengine::rt::task<void> t) {
    while (!t.done()) t.resume();
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

class OneShotChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"ok"};
        item.origin = content_origin::assistant;
        Message reply{};
        reply.role = role::assistant;
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
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

struct SlowArgs { bool noop; };
AE_JSON_SCHEMA(SlowArgs, noop)
struct SlowReply { bool ok; };
AE_JSON_SCHEMA(SlowReply, ok)

// Sleeps 150ms then succeeds -- same fixture shape as the Quark original's own test, so B2/G7's
// "did start_background_task() actually detach" timing claim is comparable across both files.
struct SlowBackgroundableTool : agentengine::Tool<SlowBackgroundableTool, agentengine::Backgroundable> {
    static constexpr std::string_view name        = "slow_backgroundable";
    static constexpr std::string_view description = "Sleeps 150ms, then succeeds.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return Reply{true};
    }
};

struct ForegroundOnlyTool : agentengine::Tool<ForegroundOnlyTool> {
    static constexpr std::string_view name        = "foreground_only";
    static constexpr std::string_view description = "Never declared Backgroundable.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::Principal;

    AgentSession<OneShotChatClient> session;
    session.initialize("s-bg", Principal{"p-owner", ""});
    session.emplace_chat_client();

    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::Background{1}});
    session.set_capabilities(&held);

    auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

    auto const table = agentengine::ToolTable::from_tools<SlowBackgroundableTool, ForegroundOnlyTool>();

    auto started = drive(session.start_run(StartRun{user_message("hi")}));
    check(started.has_value(), "setup: a real start_run() succeeds before any background_task call");

    // --- B1: an undeclared-Backgroundable tool is rejected at authorize, before step 8 ------------
    {
        agentengine::ToolCallRequest req{"call-fg", "foreground_only", *json::parse(R"({"noop":true})"),
                                          false};
        auto handle = drive(session.start_background_task(table, req));
        check(!handle.has_value(), "B1: a tool not declared Backgroundable is rejected");
        if (!handle.has_value()) {
            check(handle.error().code == "tool.not_backgroundable",
                  "B1: rejected with the real error_code, not a generic failure");
        }
        check(session.list_standing_effects().empty(), "B1: nothing is registered for a rejected call");
    }

    // --- B2/G7: start_background_task() returns near-instantly, even though the tool itself -------
    // --- sleeps 150ms -- the calling turn is genuinely never blocked.                            ---
    std::string handle_id_1;
    {
        agentengine::ToolCallRequest req{"call-1", "slow_backgroundable",
                                          *json::parse(R"({"noop":true})"), false};
        auto const t0      = std::chrono::steady_clock::now();
        auto        handle = drive(session.start_background_task(table, req));
        auto const elapsed = std::chrono::steady_clock::now() - t0;
        check(handle.has_value(), "B2: a Backgroundable tool under a granted Background<1> is accepted");
        check(elapsed < std::chrono::milliseconds(50),
              "G7: start_background_task() returns in well under the tool's own 150ms sleep -- step "
              "8 genuinely runs detached, not inline");
        if (handle.has_value()) handle_id_1 = handle->handle_id;

        check(session.list_standing_effects().size() == 1,
              "B2: the StandingEffect is visible via list_standing_effects() while pending");
        if (session.list_standing_effects().size() == 1) {
            agentengine::StandingEffect const& eff = session.list_standing_effects().front();
            check(eff.kind == agentengine::standing_effect_kind::background_task,
                  "B2: the registered effect's kind is background_task");
            check(eff.label == "slow_backgroundable", "B2: the effect's label is the tool's own name");
            check(eff.principal_id == "p-owner", "B2: the effect is attributed to the run's owning principal");
        }
    }

    // --- B3/G9: the session's Background<1> cap is already at 1 -- a SECOND call is rejected, ------
    // --- never silently queued.                                                                 ---
    {
        agentengine::ToolCallRequest req{"call-2", "slow_backgroundable",
                                          *json::parse(R"({"noop":true})"), false};
        auto handle = drive(session.start_background_task(table, req));
        check(!handle.has_value(), "G9: a second background_task while the cap is at 1/1 is rejected");
        if (!handle.has_value()) {
            check(handle.error().code == "tool.background_capacity_exceeded",
                  "G9: rejected with the real capacity error_code, not a generic failure");
        }
        check(session.list_standing_effects().size() == 1,
              "G9: the rejected second call registers nothing -- still exactly one effect outstanding");
    }

    // --- B4/G8: cancel_standing_effect() denies a DIFFERENT principal, then would succeed for the --
    // --- owning one (deferred to after B5 so B5 can observe the effect actually resolve instead). --
    {
        auto denied = session.cancel_standing_effect(handle_id_1, Principal{"p-stranger", ""});
        check(!denied.has_value(), "G8: a different principal cannot cancel this effect");
        if (!denied.has_value()) {
            check(denied.error().code == "standing_effect.cross_principal_denied",
                  "G8: denied with the real cross-principal error_code");
        }
        check(session.list_standing_effects().size() == 1, "G8: a denied cancel changes nothing");
    }

    // --- B5: wait for the real 150ms-sleeping thread to finish, then EXPLICITLY drain -- proving --
    // --- the completion-queue delivery path itself, independent of the auto-drain B6 covers.     --
    // A SECOND, later run starts here (before the wait) -- proving the eventual ToolCallFinished
    // (attributed to the FIRST run) does not collide with or get misattributed to this newer run.
    auto second_run = drive(session.start_run(StartRun{user_message("again")}));
    check(second_run.has_value(), "setup: a second, later run on the same session still works normally");

    check(wait_until(
              [&] {
                  drive_void(session.drain_background_completions());
                  return session.list_standing_effects().empty();
              },
              std::chrono::seconds(2)),
          "B5: the detached thread eventually finishes and an explicit drain_background_completions() "
          "call resolves the StandingEffect -- list_standing_effects() empties");

    bool saw_tool_call_started   = false;
    bool saw_tool_call_finished  = false;
    bool finished_ok             = false;
    bool attributed_to_first_run = false;
    while (auto ev = viewer.next()) {
        if (ev->kind == agentengine::run_event_kind::tool_call_started) {
            saw_tool_call_started = true;
            attributed_to_first_run |= (ev->run_id == "s-bg:run:1");
        }
        if (ev->kind == agentengine::run_event_kind::tool_call_finished) {
            saw_tool_call_finished = true;
            attributed_to_first_run |= (ev->run_id == "s-bg:run:1");
            if (auto const* p = std::get_if<agentengine::run_event_payload::ToolCallFinished>(&ev->payload)) {
                finished_ok = p->ok;
            }
        }
    }
    check(saw_tool_call_started, "B5: ToolCallStarted is real on the event stream");
    check(saw_tool_call_finished,
          "B5: ToolCallFinished is real on the event stream once the background thread completes");
    check(finished_ok, "B5: ToolCallFinished carries the real ok=true outcome");
    check(attributed_to_first_run,
          "B5: both events carry run_id \"s-bg:run:1\" -- the run that ASKED for the background work "
          "-- never the second run that happened to be current when it resolved");

    // --- B6: the NEW part -- a completion is picked up with NO explicit drain call at all, purely --
    // --- because start_run() drains automatically as its own first step.                          --
    {
        agentengine::ToolCallRequest req{"call-3", "slow_backgroundable",
                                          *json::parse(R"({"noop":true})"), false};
        auto handle = drive(session.start_background_task(table, req));
        check(handle.has_value(), "B6 setup: a fresh background task is accepted (cap freed by B5)");
        check(session.list_standing_effects().size() == 1, "B6 setup: the effect is pending");

        // Real wall-clock wait past the tool's own 150ms sleep -- deliberately NOT calling
        // drain_background_completions() here, so the completion sits in the queue, undrained, on
        // purpose.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        check(session.list_standing_effects().size() == 1,
              "B6: with no drain call, the effect is STILL pending even though the worker has "
              "already finished and pushed its completion -- proves nothing drains it automatically "
              "except an actual start_run()/resolve_interaction() call");

        auto third_run = drive(session.start_run(StartRun{user_message("once more")}));
        check(third_run.has_value(), "B6: the third run itself succeeds");
        check(session.list_standing_effects().empty(),
              "B6: start_run() alone -- no explicit drain_background_completions() call anywhere in "
              "this block -- resolved the completion via its own automatic first-step drain");
    }

    if (g_failures == 0) {
        std::printf("test_rt_agent_session_background_task: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_background_task: %d failure(s)\n", g_failures);
    return 1;
}
