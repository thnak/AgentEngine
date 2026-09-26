// ADR-037 port of test_cross_surface_equivalence.cpp onto agentengine::rt::AgentSession (include/
// agentengine/rt/agent_session.hpp) -- the Quark-actor-free replacement for agentengine::AgentSession
// (core/agent_session.hpp). Milestone 7 Phase E4 (013-UI-and-Streaming-Surfaces.md §6 G3: "the same
// run projects to AG-UI and A2A streaming with equivalent content, proven by a cross-surface
// comparison test", docs/planning/milestone-7-protocol-conformance-breakdown.md). Drives ONE real
// `rt::AgentSession` run (via `enable_event_stream()`, real since Phase A, ported to rt:: by ADR-037
// Phase 2 Slice 1) and feeds the SAME captured internal event sequence through both `agui::
// RunEventProjector` (E2) and `a2a::A2aStreamProjector` (E4) -- equivalence is judged at the OUTCOME
// level (both surfaces agree the run succeeded/failed, in the same relative order relative to the
// internal stream), not byte-identical wire content, which is impossible across two structurally
// different protocols and not what G3 asks for ("equivalent content", not "identical bytes").
//
// ONLY the AgentSession-driven setup in CS-1/CS-2 changed versus the original: `quark::TestKit<Session>
// kit; kit.actor()...`/`kit.ask<AgentResponse>(...)` becomes a plain `Session session; session...`
// local plus a `drive<T>()` loop over `start_run()`'s returned `rt::task<T>` -- the same pattern
// test_rt_agent_session.cpp/test_rt_agui_projection.cpp already establish. The projection/comparison
// logic that follows is untouched -- it only ever consumes the captured `std::vector<RunEvent>`.

#include <iostream>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/protocol/a2a/streaming.hpp"
#include "agentengine/protocol/agui/projection.hpp"
#include "agentengine/rt/agent_session.hpp"

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

namespace agui = agentengine::agui;
namespace a2a  = agentengine::a2a;

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- the ScriptedChatClient fixture below co_returns
// immediately from both chat() and chat_stream().
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

ae::Message make_turn(std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{"hello"};
    item.origin = ae::content_origin::user;
    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

class ScriptedChatClient {
public:
    bool fail_next = false;
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        if (fail_next) {
            co_return std::unexpected(ae::error{ae::failure_class::transient, "scripted failure", "test.fail"});
        }
        ae::ContentItem item{};
        item.value  = ae::Text{"reply"};
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
        if (fail_next) {
            pair.producer.fail(
                ae::error{ae::failure_class::fatal, "scripted failure", "test.scripted_failure"});
            return std::move(pair.consumer);
        }
        ae::ChatResponseUpdate upd{};
        upd.delta.value  = ae::Text{"reply"};
        upd.delta.origin = ae::content_origin::assistant;
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<ScriptedChatClient>);

std::vector<ae::RunEvent> drain(ae::stream<ae::RunEvent>& s) {
    std::vector<ae::RunEvent> events;
    while (auto ev = s.next()) events.push_back(std::move(*ev));
    return events;
}

// Did the AG-UI-projected sequence end the run successfully?
bool agui_saw_success(std::vector<agui::AgUiEvent> const& events) {
    for (auto const& e : events) {
        if (std::holds_alternative<agui::RunFinishedSuccess>(e)) return true;
    }
    return false;
}
bool agui_saw_error(std::vector<agui::AgUiEvent> const& events) {
    for (auto const& e : events) {
        if (std::holds_alternative<agui::RunError>(e)) return true;
    }
    return false;
}

bool a2a_reached_state(std::vector<a2a::StreamResponse> const& events, a2a::task_state s) {
    for (auto const& e : events) {
        if (auto const* upd = std::get_if<a2a::TaskStatusUpdateEvent>(&e)) {
            if (upd->status.state == s) return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // --- CS-1: a REAL successful run -- both surfaces agree it succeeded, neither shows failure ----
    {
        using Session = agentengine::rt::AgentSession<ScriptedChatClient>;
        Session session;
        session.initialize("s-success", ae::Principal{"p1", ""});
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(agentengine::rt::StartRun{make_turn("m-1")}));
        AE_CHECK(r.has_value(), "CS-1: the real run succeeds");
        auto internal_events = drain(viewer);
        AE_CHECK(!internal_events.empty(), "CS-1: the real turn loop emits real internal events");

        agui::RunEventProjector    agui_projector("thread-1");
        a2a::A2aStreamProjector    a2a_projector("s-success:run:1", "ctx-1");
        std::vector<agui::AgUiEvent>     agui_events;
        std::vector<a2a::StreamResponse> a2a_events;
        for (auto const& ev : internal_events) {
            for (auto& e : agui_projector.project(ev)) agui_events.push_back(std::move(e));
            for (auto& e : a2a_projector.project(ev)) a2a_events.push_back(std::move(e));
        }

        AE_CHECK(agui_saw_success(agui_events),
                 "CS-1: the AG-UI projection reports RunFinishedSuccess for the successful run");
        AE_CHECK(!agui_saw_error(agui_events),
                 "CS-1: the AG-UI projection reports NO RunError for the successful run");
        AE_CHECK(a2a_reached_state(a2a_events, a2a::task_state::completed),
                 "CS-1: the A2A projection reaches TASK_STATE_COMPLETED for the same successful run");
        AE_CHECK(!a2a_reached_state(a2a_events, a2a::task_state::failed),
                 "CS-1: the A2A projection reports NO failed state for the same successful run --  "
                 "both surfaces agree, from the SAME internal event sequence (013 §6 G3)");

        // Ordering equivalence: both surfaces observe "started" strictly before "finished".
        auto agui_started_at = -1, agui_finished_at = -1;
        for (std::size_t i = 0; i < agui_events.size(); ++i) {
            if (std::holds_alternative<agui::RunStarted>(agui_events[i])) agui_started_at = static_cast<int>(i);
            if (std::holds_alternative<agui::RunFinishedSuccess>(agui_events[i]))
                agui_finished_at = static_cast<int>(i);
        }
        auto a2a_working_at = -1, a2a_completed_at = -1;
        for (std::size_t i = 0; i < a2a_events.size(); ++i) {
            if (auto const* upd = std::get_if<a2a::TaskStatusUpdateEvent>(&a2a_events[i])) {
                if (upd->status.state == a2a::task_state::working && a2a_working_at < 0)
                    a2a_working_at = static_cast<int>(i);
                if (upd->status.state == a2a::task_state::completed) a2a_completed_at = static_cast<int>(i);
            }
        }
        AE_CHECK(agui_started_at >= 0 && agui_finished_at > agui_started_at,
                 "CS-1: AG-UI observes RunStarted strictly before RunFinishedSuccess");
        AE_CHECK(a2a_working_at >= 0 && a2a_completed_at > a2a_working_at,
                 "CS-1: A2A observes WORKING strictly before COMPLETED -- the same relative order, "
                 "off the identical underlying internal event sequence");
    }

    // --- CS-2: a REAL failing run -- both surfaces agree it failed, neither shows success ----------
    {
        using Session = agentengine::rt::AgentSession<ScriptedChatClient>;
        Session session;
        session.initialize("s-fail", ae::Principal{"p1", ""});
        session.emplace_chat_client().fail_next = true;
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(agentengine::rt::StartRun{make_turn("m-2")}));
        AE_CHECK(!r.has_value(), "CS-2: the scripted chat failure fails the run");
        auto internal_events = drain(viewer);

        agui::RunEventProjector agui_projector("thread-2");
        a2a::A2aStreamProjector a2a_projector("s-fail:run:1", "ctx-2");
        std::vector<agui::AgUiEvent>     agui_events;
        std::vector<a2a::StreamResponse> a2a_events;
        for (auto const& ev : internal_events) {
            for (auto& e : agui_projector.project(ev)) agui_events.push_back(std::move(e));
            for (auto& e : a2a_projector.project(ev)) a2a_events.push_back(std::move(e));
        }

        AE_CHECK(agui_saw_error(agui_events) && !agui_saw_success(agui_events),
                 "CS-2: AG-UI reports RunError, never RunFinishedSuccess, for the failing run");
        AE_CHECK(a2a_reached_state(a2a_events, a2a::task_state::failed) &&
                     !a2a_reached_state(a2a_events, a2a::task_state::completed),
                 "CS-2: A2A reaches TASK_STATE_FAILED, never COMPLETED, for the SAME failing run -- "
                 "both surfaces agree on the outcome, from the identical internal event sequence");
    }

    if (g_failures == 0) {
        std::cout << "test_rt_cross_surface_equivalence: ALL PASS\n";
        return 0;
    }
    std::cerr << "test_rt_cross_surface_equivalence: " << g_failures << " failure(s)\n";
    return 1;
}
