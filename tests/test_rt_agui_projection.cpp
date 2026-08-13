// ADR-037 port of test_agui_projection.cpp onto agentengine::rt::AgentSession (include/agentengine/
// rt/agent_session.hpp) -- the Quark-actor-free replacement for agentengine::AgentSession (core/
// agent_session.hpp). Milestone 7 Phase E2 (013-UI-and-Streaming-Surfaces.md §2.1/§2.2,
// docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves `RunEventProjector`
// (protocol/agui/projection.hpp) both end to end against a REAL `rt::AgentSession` turn loop (via
// `enable_event_stream()`, real since Phase A, ported to rt:: by ADR-037 Phase 2 Slice 1) for the
// event kinds it actually produces today, and directly (hand-fed `RunEvent`s) for every OTHER kind
// the vocabulary names but no real producer exists for yet -- honestly split, matching
// `test_agent_session_run_event_stream.cpp`'s own precedent for exactly this same real/unwired split.
//
// ONLY the two AgentSession-driven blocks (E2-1, E2-2) changed versus the original: `quark::TestKit
// <Session> kit; kit.actor()...`/`kit.ask<AgentResponse>(...)` (a Quark actor's mailbox-serialized
// ask/reply) becomes a plain `Session session; session...` local plus a `drive<T>()` loop over
// `start_run()`'s returned `rt::task<T>` -- the same pattern test_rt_agent_session.cpp/test_rt_agent_
// session_streaming_and_events.cpp already establish. Every hand-fed-RunEvent block (E2-3 onward)
// needs no changes at all -- it never touches AgentSession in the first place.

#include <iostream>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
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
            pair.producer.fail(quark::error{quark::errc::internal, "scripted failure"});
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

}  // namespace

int main() {
    // --- E2-1: a REAL success-path run, projected end to end -----------------------------------------
    {
        using Session = agentengine::rt::AgentSession<ScriptedChatClient>;
        Session session;
        session.initialize("s-success", ae::Principal{"p1", ""});
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(agentengine::rt::StartRun{make_turn("m-1")}));
        AE_CHECK(r.has_value(), "E2-1: the real run succeeds");
        auto internal_events = drain(viewer);
        AE_CHECK(internal_events.size() == 6, "E2-1: the real turn loop emits its known 6-event sequence");

        agui::RunEventProjector projector("thread-1");
        std::vector<agui::AgUiEvent> wire_events;
        for (auto const& ev : internal_events) {
            auto projected = projector.project(ev);
            wire_events.insert(wire_events.end(), projected.begin(), projected.end());
        }

        // run_started, turn_started, model_call_started, model_call_finished, turn_finished,
        // run_finished -> RunStarted, StepStarted, TextMessageStart, TextMessageEnd, StepFinished,
        // RunFinishedSuccess -- ONE wire event per internal event on this real path (no ModelDelta
        // fires, so no TEXT_MESSAGE_CONTENT appears between START and END).
        AE_CHECK(wire_events.size() == 6, "E2-1: 6 internal events produce 6 wire events on this real path");
        if (wire_events.size() == 6) {
            AE_CHECK(std::holds_alternative<agui::RunStarted>(wire_events[0]), "E2-1[0]: RunStarted");
            AE_CHECK(std::holds_alternative<agui::StepStarted>(wire_events[1]), "E2-1[1]: StepStarted");
            AE_CHECK(std::holds_alternative<agui::TextMessageStart>(wire_events[2]), "E2-1[2]: TextMessageStart");
            AE_CHECK(std::holds_alternative<agui::TextMessageEnd>(wire_events[3]), "E2-1[3]: TextMessageEnd");
            AE_CHECK(std::holds_alternative<agui::StepFinished>(wire_events[4]), "E2-1[4]: StepFinished");
            AE_CHECK(std::holds_alternative<agui::RunFinishedSuccess>(wire_events[5]), "E2-1[5]: RunFinishedSuccess");

            auto const& start = std::get<agui::TextMessageStart>(wire_events[2]);
            auto const& end   = std::get<agui::TextMessageEnd>(wire_events[3]);
            AE_CHECK(start.message_id == end.message_id,
                     "E2-1: the SAME synthesized messageId brackets START and END, a real run's own "
                     "identity, not two independently-minted ids");

            auto const& started = std::get<agui::RunStarted>(wire_events[0]);
            auto const& finished = std::get<agui::RunFinishedSuccess>(wire_events[5]);
            AE_CHECK(started.thread_id == "thread-1" && finished.thread_id == "thread-1",
                     "E2-1: threadId is the projector's own configured thread_id on both ends");
            AE_CHECK(started.run_id == "s-success:run:1" && finished.run_id == "s-success:run:1",
                     "E2-1: runId is the REAL run_id the session actually minted");
        }
    }

    // --- E2-2: a REAL failing run projects to RunError -----------------------------------------------
    {
        using Session = agentengine::rt::AgentSession<ScriptedChatClient>;
        Session session;
        session.initialize("s-fail", ae::Principal{"p1", ""});
        session.emplace_chat_client().fail_next = true;
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(agentengine::rt::StartRun{make_turn("m-2")}));
        AE_CHECK(!r.has_value(), "E2-2: the scripted chat failure fails the run (fail-closed)");
        auto internal_events = drain(viewer);

        agui::RunEventProjector projector("thread-2");
        std::vector<agui::AgUiEvent> wire_events;
        for (auto const& ev : internal_events) {
            auto projected = projector.project(ev);
            wire_events.insert(wire_events.end(), projected.begin(), projected.end());
        }
        bool saw_run_error = false;
        for (auto const& e : wire_events) {
            if (std::holds_alternative<agui::RunError>(e)) saw_run_error = true;
        }
        AE_CHECK(saw_run_error, "E2-2: the real run_failed event projects to a real RunError -- "
                                 "RUN_ERROR, the sole error event (013 §2.1)");
    }

    // --- E2-3 through E2-N: hand-fed RunEvents for every kind with no real producer yet, honestly ---
    // --- exercised directly rather than claimed proven end-to-end (run_event.hpp's own scope note). -
    // No AgentSession involved below -- unchanged from the original file.
    agui::RunEventProjector projector("thread-3");

    // model_delta (mid-message content) -- needs an open message first.
    {
        (void)projector.project(
            ae::RunEvent{"run-x", 1, ae::run_event_kind::model_call_started, ae::run_event_payload::Empty{}});
        auto out = projector.project(ae::RunEvent{"run-x", 2, ae::run_event_kind::model_delta,
                                                    ae::run_event_payload::ModelDelta{"partial text"}});
        AE_CHECK(out.size() == 1 && std::holds_alternative<agui::TextMessageContent>(out[0]),
                 "E2-3: model_delta -> a single TextMessageContent");
        if (out.size() == 1 && std::holds_alternative<agui::TextMessageContent>(out[0])) {
            AE_CHECK(std::get<agui::TextMessageContent>(out[0]).delta == "partial text",
                     "E2-3: the delta text survives");
        }
        (void)projector.project(
            ae::RunEvent{"run-x", 3, ae::run_event_kind::model_call_finished, ae::run_event_payload::Empty{}});
    }

    // model_delta WITHOUT a preceding model_call_started -- the defensive lazy-open path.
    {
        auto out = projector.project(ae::RunEvent{"run-y", 1, ae::run_event_kind::model_delta,
                                                    ae::run_event_payload::ModelDelta{"orphaned"}});
        AE_CHECK(out.size() == 1 && std::holds_alternative<agui::TextMessageContent>(out[0]),
                 "E2-4: an out-of-order model_delta still produces a real TextMessageContent via the "
                 "defensive lazy-open path, never a crash");
    }

    // tool_call_started/delta/finished.
    {
        auto started = projector.project(
            ae::RunEvent{"run-z", 1, ae::run_event_kind::tool_call_started,
                         ae::run_event_payload::ToolCallStarted{"call-1", "search"}});
        AE_CHECK(started.size() == 1 && std::holds_alternative<agui::ToolCallStart>(started[0]),
                 "E2-5: tool_call_started -> ToolCallStart");

        auto delta = projector.project(
            ae::RunEvent{"run-z", 2, ae::run_event_kind::tool_call_delta,
                         ae::run_event_payload::ToolCallDelta{"call-1", "50% done"}});
        AE_CHECK(delta.size() == 1 && std::holds_alternative<agui::CustomEvent>(delta[0]),
                 "E2-6: tool_call_delta -> CustomEvent (ae:tool_call_progress), NOT an undocumented "
                 "TOOL_CALL_CHUNK shape this codebase has no cited field layout for");
        if (delta.size() == 1 && std::holds_alternative<agui::CustomEvent>(delta[0])) {
            AE_CHECK(std::get<agui::CustomEvent>(delta[0]).name == "ae:tool_call_progress",
                     "E2-6: the CustomEvent carries the expected namespaced name");
        }

        auto finished = projector.project(
            ae::RunEvent{"run-z", 3, ae::run_event_kind::tool_call_finished,
                         ae::run_event_payload::ToolCallFinished{"call-1", true}});
        AE_CHECK(finished.size() == 1 && std::holds_alternative<agui::ToolCallEnd>(finished[0]),
                 "E2-7: tool_call_finished -> ToolCallEnd");
    }

    // sandbox_exec_started/finished.
    {
        auto started = projector.project(
            ae::RunEvent{"run-w", 1, ae::run_event_kind::sandbox_exec_started,
                         ae::run_event_payload::SandboxExec{"exec-1"}});
        AE_CHECK(started.size() == 1 && std::holds_alternative<agui::ActivitySnapshot>(started[0]),
                 "E2-8: sandbox_exec_started -> ActivitySnapshot");
        if (started.size() == 1 && std::holds_alternative<agui::ActivitySnapshot>(started[0])) {
            auto const& snap = std::get<agui::ActivitySnapshot>(started[0]);
            AE_CHECK(snap.message_id == "exec-1" && snap.activity_type == "sandbox_exec",
                     "E2-8: the ActivitySnapshot carries the real exec_id and a real activityType");
        }
    }

    // state_changed / artifact_produced -- CustomEvent, honestly, no fabricated JSON Patch/first-class slot.
    {
        auto sc = projector.project(ae::RunEvent{"run-v", 1, ae::run_event_kind::state_changed,
                                                    ae::run_event_payload::StateChanged{"budget updated"}});
        AE_CHECK(sc.size() == 1 && std::holds_alternative<agui::CustomEvent>(sc[0]) &&
                     std::get<agui::CustomEvent>(sc[0]).name == "ae:state_changed",
                 "E2-9: state_changed -> CustomEvent(\"ae:state_changed\"), never a fabricated "
                 "RFC 6902 patch (no state-diffing engine exists anywhere in this codebase)");

        auto ap = projector.project(ae::RunEvent{"run-v", 2, ae::run_event_kind::artifact_produced,
                                                    ae::run_event_payload::ArtifactProduced{"art-1"}});
        AE_CHECK(ap.size() == 1 && std::holds_alternative<agui::CustomEvent>(ap[0]),
                 "E2-10: artifact_produced -> CustomEvent, no first-class AG-UI slot per §2.1's table");
    }

    // --- E2-11/12/13: interrupts -- the run ENDS with RunFinishedInterrupt, never a pause event -----
    {
        auto in_req = projector.project(
            ae::RunEvent{"run-u", 1, ae::run_event_kind::input_required,
                         ae::run_event_payload::InteractionRef{"interaction-1"}});
        AE_CHECK(in_req.size() == 1 && std::holds_alternative<agui::RunFinishedInterrupt>(in_req[0]),
                 "E2-11: input_required ENDS the run -- RunFinishedInterrupt, AG-UI has no pause event");
        if (in_req.size() == 1 && std::holds_alternative<agui::RunFinishedInterrupt>(in_req[0])) {
            auto const& rfi = std::get<agui::RunFinishedInterrupt>(in_req[0]);
            AE_CHECK(rfi.interrupts.size() == 1 && rfi.interrupts[0].id == "interaction-1" &&
                         rfi.interrupts[0].reason == "input_required",
                     "E2-11: interruptId IS the interaction_id verbatim (013 §2.2), native reason");
        }

        auto auth_req = projector.project(
            ae::RunEvent{"run-t", 1, ae::run_event_kind::auth_required,
                         ae::run_event_payload::InteractionRef{"interaction-2"}});
        if (auth_req.size() == 1 && std::holds_alternative<agui::RunFinishedInterrupt>(auth_req[0])) {
            AE_CHECK(std::get<agui::RunFinishedInterrupt>(auth_req[0]).interrupts[0].reason == "ae:auth_required",
                     "E2-12: auth_required uses the ae:auth_required extension namespace -- AG-UI's "
                     "own reason enum has no native auth member (013 §2.2)");
        }

        auto appr = projector.project(
            ae::RunEvent{"run-s", 1, ae::run_event_kind::approval_requested,
                         ae::run_event_payload::ApprovalRequested{"call-9"}});
        if (appr.size() == 1 && std::holds_alternative<agui::RunFinishedInterrupt>(appr[0])) {
            auto const& rfi = std::get<agui::RunFinishedInterrupt>(appr[0]);
            AE_CHECK(rfi.interrupts[0].reason == "confirmation" && rfi.interrupts[0].id == "call-9" &&
                         rfi.interrupts[0].tool_call_id == "call-9",
                     "E2-13: approval_requested uses AG-UI's native \"confirmation\" reason");
        }
    }

    // --- E2-14: resolution events (input_resolved/auth_resolved/approval_resolved) emit NOTHING ----
    // --- further on this already-ended run -- resumption is a NEW run, out of this projector's job. -
    {
        auto ir = projector.project(ae::RunEvent{"run-u", 2, ae::run_event_kind::input_resolved,
                                                    ae::run_event_payload::InteractionRef{"interaction-1"}});
        auto ar = projector.project(ae::RunEvent{"run-t", 2, ae::run_event_kind::auth_resolved,
                                                    ae::run_event_payload::InteractionRef{"interaction-2"}});
        auto pr = projector.project(ae::RunEvent{"run-s", 2, ae::run_event_kind::approval_resolved,
                                                    ae::run_event_payload::ApprovalResolved{"call-9", true}});
        AE_CHECK(ir.empty() && ar.empty() && pr.empty(),
                 "E2-14: resolution events project to nothing -- the AG-UI run already ended");
    }

    // --- E2-15: run_canceled -> RunError (§2.1: "the sole error event") -----------------------------
    {
        auto out = projector.project(
            ae::RunEvent{"run-r", 1, ae::run_event_kind::run_canceled, ae::run_event_payload::Empty{}});
        AE_CHECK(out.size() == 1 && std::holds_alternative<agui::RunError>(out[0]),
                 "E2-15: run_canceled -> RunError, never a second, invented error shape");
    }

    // --- E2-16/17: warning / policy_decision -> CustomEvent ------------------------------------------
    {
        auto w = projector.project(ae::RunEvent{"run-q", 1, ae::run_event_kind::warning,
                                                   ae::run_event_payload::Warning{"be careful"}});
        auto p = projector.project(ae::RunEvent{"run-q", 2, ae::run_event_kind::policy_decision,
                                                   ae::run_event_payload::PolicyDecision{"denied by policy"}});
        AE_CHECK(w.size() == 1 && std::holds_alternative<agui::CustomEvent>(w[0]) &&
                     std::get<agui::CustomEvent>(w[0]).name == "ae:warning",
                 "E2-16: warning -> CustomEvent(\"ae:warning\")");
        AE_CHECK(p.size() == 1 && std::holds_alternative<agui::CustomEvent>(p[0]) &&
                     std::get<agui::CustomEvent>(p[0]).name == "ae:policy_decision",
                 "E2-17: policy_decision -> CustomEvent(\"ae:policy_decision\")");
    }

    if (g_failures == 0) {
        std::cout << "test_rt_agui_projection: ALL PASS\n";
        return 0;
    }
    std::cerr << "test_rt_agui_projection: " << g_failures << " failure(s)\n";
    return 1;
}
