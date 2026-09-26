// Proof for ADR-053 (2026-08-10-full-codebase-adr-gap-audit.md gap #7): agentengine::rt::AgentSession's
// THIRD real StandingEffect producer, schedule_wakeup()/due_standing_effects() (agent_session.hpp's
// "SLICE 4 ADDITION"). Modeled on test_rt_agent_session_background_task.cpp's own shape (same session
// setup, same check()/drive() helpers) so the two closure proofs are easy to compare side by side.
//
//   S1 -- no cap::Schedule granted at all: schedule_wakeup() fails closed, nothing registered.
//   S2 -- a delay exceeding the grant's own max_horizon fails closed (the audit's own "currently
//         unbounded, a live I2 gap" finding, closed structurally).
//   S3 -- a successful call: real StandingEffect{kind=schedule_wakeup, fire_at=now+delay}, visible via
//         list_standing_effects(), and a real StateChanged event lands on the run's event stream (006
//         §6b: "Registering... is visible... via StateChanged"), NOT ToolCallStarted.
//   S4 -- max_active is enforced against a LIVE count, at registration, never silently queued (the
//         Background<max_concurrent>/G9 precedent, applied to Schedule<max_active> instead).
//   S5 -- due_standing_effects(now) returns exactly the effects whose fire_at <= now, and none of the
//         ones still in the future -- and never mutates standing_effects_ itself (calling it twice
//         returns the same set; only cancel_standing_effect() clears an entry).
//   S6 -- cancel_standing_effect() (the pre-existing, general mechanism) works on a schedule_wakeup
//         effect exactly like it already does for background_task -- denies a different principal,
//         succeeds for the owner, and the entry then no longer appears in due_standing_effects().

#include <chrono>
#include <cstdio>
#include <string>

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

// Safe here: this fixture's chat() never suspends on anything external, same rule every other
// rt::AgentSession test file's own drive<T>() documents.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
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

}  // namespace

int main() {
    using agentengine::Principal;
    auto const t0 = std::chrono::steady_clock::now();

    // --- S1: no cap::Schedule granted at all -- fails closed, nothing registered -------------------
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("s-sched-1", Principal{"p-owner", ""});
        session.emplace_chat_client();
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        auto started = drive(session.start_run(StartRun{user_message("hi")}));
        check(started.has_value(), "S1 setup: start_run succeeds");

        auto handle = drive(session.schedule_wakeup(std::chrono::milliseconds(1000), "reminder", t0));
        check(!handle.has_value(), "S1: schedule_wakeup fails closed with no cap::Schedule granted");
        if (!handle.has_value()) {
            check(handle.error().code == "schedule_wakeup.not_granted",
                  "S1: rejected with the real not_granted error_code");
        }
        check(session.list_standing_effects().empty(), "S1: nothing is registered for a rejected call");
    }

    // --- S2: delay exceeds the grant's own max_horizon -- fails closed ------------------------------
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("s-sched-2", Principal{"p-owner", ""});
        session.emplace_chat_client();
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::Schedule{std::chrono::seconds{60}, 5}});
        session.set_capabilities(&held);
        auto started = drive(session.start_run(StartRun{user_message("hi")}));
        check(started.has_value(), "S2 setup: start_run succeeds");

        auto handle = drive(session.schedule_wakeup(std::chrono::milliseconds(120000), "too far out", t0));
        check(!handle.has_value(), "S2: a delay past the granted max_horizon (60s) is rejected");
        if (!handle.has_value()) {
            check(handle.error().code == "schedule_wakeup.horizon_exceeded",
                  "S2: rejected with the real horizon_exceeded error_code");
        }
        check(session.list_standing_effects().empty(), "S2: the rejected call registers nothing");

        // A delay within the horizon still succeeds under the same grant -- proves S2 above rejected
        // for the stated reason, not because the grant itself is unusable.
        auto ok = drive(session.schedule_wakeup(std::chrono::milliseconds(30000), "within horizon", t0));
        check(ok.has_value(), "S2: a delay within the granted horizon (30s <= 60s) is accepted");
    }

    // --- S3: a successful call -- real StandingEffect + StateChanged event, not ToolCallStarted -----
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("s-sched-3", Principal{"p-owner", ""});
        session.emplace_chat_client();
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::Schedule{std::chrono::seconds{3600}, 5}});
        session.set_capabilities(&held);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto started = drive(session.start_run(StartRun{user_message("hi")}));
        check(started.has_value(), "S3 setup: start_run succeeds");

        auto const delay = std::chrono::milliseconds(5000);
        auto handle = drive(session.schedule_wakeup(delay, "daily digest", t0));
        check(handle.has_value(), "S3: a schedule_wakeup within grant is accepted");
        if (handle.has_value()) {
            check(handle->kind == agentengine::standing_effect_kind::schedule_wakeup,
                  "S3: the registered effect's kind is schedule_wakeup");
            check(handle->label == "daily digest", "S3: the effect's label is the caller-supplied label");
            check(handle->principal_id == "p-owner",
                  "S3: the effect is attributed to the run's owning principal");
            check(handle->fire_at.has_value() && *handle->fire_at == t0 + delay,
                  "S3: fire_at is exactly now + delay, computed from the caller-supplied now");
        }
        check(session.list_standing_effects().size() == 1,
              "S3: the StandingEffect is visible via list_standing_effects()");

        bool saw_state_changed = false;
        bool saw_tool_call_started = false;
        while (auto ev = viewer.next()) {
            if (ev->kind == agentengine::run_event_kind::state_changed) saw_state_changed = true;
            if (ev->kind == agentengine::run_event_kind::tool_call_started) saw_tool_call_started = true;
        }
        check(saw_state_changed,
              "S3: registration emits state_changed (006 §6b's own normative event for this)");
        check(!saw_tool_call_started,
              "S3: registration does NOT emit tool_call_started -- there is no ToolCallRequest behind "
              "a schedule_wakeup call, unlike start_background_task()");
    }

    // --- S4: max_active is enforced against a LIVE count, at registration ---------------------------
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("s-sched-4", Principal{"p-owner", ""});
        session.emplace_chat_client();
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::Schedule{std::chrono::seconds{3600}, 1}});
        session.set_capabilities(&held);
        auto started = drive(session.start_run(StartRun{user_message("hi")}));
        check(started.has_value(), "S4 setup: start_run succeeds");

        auto first = drive(session.schedule_wakeup(std::chrono::milliseconds(1000), "first", t0));
        check(first.has_value(), "S4: the first call, under a Schedule<..,1> grant, is accepted");

        auto second = drive(session.schedule_wakeup(std::chrono::milliseconds(1000), "second", t0));
        check(!second.has_value(), "G9-analog: a second call while max_active is already at 1/1 is rejected");
        if (!second.has_value()) {
            check(second.error().code == "schedule_wakeup.capacity_exceeded",
                  "S4: rejected with the real capacity_exceeded error_code");
        }
        check(session.list_standing_effects().size() == 1,
              "S4: the rejected second call registers nothing -- still exactly one effect outstanding");
    }

    // --- S5/S6: due_standing_effects() timing + cancel_standing_effect() interop --------------------
    {
        AgentSession<OneShotChatClient> session;
        session.initialize("s-sched-5", Principal{"p-owner", ""});
        session.emplace_chat_client();
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::Schedule{std::chrono::seconds{3600}, 5}});
        session.set_capabilities(&held);
        auto started = drive(session.start_run(StartRun{user_message("hi")}));
        check(started.has_value(), "S5 setup: start_run succeeds");

        auto soon = drive(session.schedule_wakeup(std::chrono::milliseconds(1000), "soon", t0));
        auto later = drive(session.schedule_wakeup(std::chrono::milliseconds(10000), "later", t0));
        check(soon.has_value() && later.has_value(), "S5 setup: both calls succeed");

        auto const check_time = t0 + std::chrono::milliseconds(5000);
        auto due = session.due_standing_effects(check_time);
        check(due.size() == 1, "S5: due_standing_effects() at t0+5s returns exactly the ONE effect "
                                "whose fire_at (t0+1s) has already passed");
        if (due.size() == 1) {
            check(due.front().label == "soon", "S5: the due effect is the one labeled \"soon\"");
        }

        // Calling it again does not mutate anything -- the entry is still pending, still due.
        auto due_again = session.due_standing_effects(check_time);
        check(due_again.size() == 1,
              "S5: due_standing_effects() is a pure read -- a second call sees the same result, "
              "nothing was consumed by the first call");
        check(session.list_standing_effects().size() == 2,
              "S5: due_standing_effects() never removes anything from list_standing_effects() itself");

        // S6: cancel_standing_effect() -- the existing, general mechanism -- clears a schedule_wakeup
        // entry exactly like it already does for background_task.
        auto denied = session.cancel_standing_effect(soon->handle_id, Principal{"p-stranger", ""});
        check(!denied.has_value(), "S6: a different principal cannot cancel this schedule_wakeup effect");
        auto allowed = session.cancel_standing_effect(soon->handle_id, Principal{"p-owner", ""});
        check(allowed.has_value(), "S6: the owning principal can cancel it");
        check(session.due_standing_effects(check_time).empty(),
              "S6: once canceled, the entry no longer appears in due_standing_effects()");
        check(session.list_standing_effects().size() == 1,
              "S6: exactly the canceled entry is gone -- \"later\" (not yet due) is untouched");
    }

    if (g_failures == 0) {
        std::printf("test_rt_agent_session_schedule_wakeup: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_schedule_wakeup: %d failure(s)\n", g_failures);
    return 1;
}
