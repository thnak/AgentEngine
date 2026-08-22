// Milestone 7 Phase E4 (012-A2A-Conformance.md §2.3, 013-UI-and-Streaming-Surfaces.md §3,
// docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves `A2aStreamProjector`
// (protocol/a2a/streaming.hpp) maps `RunEvent` onto A2A's real task-lifecycle states, and that the
// oneof `StreamResponse` wire wrapping picks the right single key per alternative.

#include <cstdio>
#include <string>

#include "agentengine/protocol/a2a/streaming.hpp"

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

namespace a2a  = agentengine::a2a;
namespace json = agentengine::json;

ae::RunEvent make(std::string run_id, std::uint64_t seq, ae::run_event_kind kind,
                   ae::RunEventPayload payload = ae::run_event_payload::Empty{}) {
    return ae::RunEvent{std::move(run_id), seq, kind, std::move(payload)};
}

}  // namespace

int main() {
    a2a::A2aStreamProjector projector("task-1", "ctx-1");

    // --- lifecycle transitions map onto real task_state values --------------------------------------
    {
        auto out = projector.project(make("run-1", 1, ae::run_event_kind::run_started));
        check(out.size() == 1 && std::holds_alternative<a2a::TaskStatusUpdateEvent>(out[0]),
              "run_started -> a single TaskStatusUpdateEvent");
        if (out.size() == 1 && std::holds_alternative<a2a::TaskStatusUpdateEvent>(out[0])) {
            auto const& e = std::get<a2a::TaskStatusUpdateEvent>(out[0]);
            check(e.status.state == a2a::task_state::working && e.task_id == "task-1" &&
                      e.context_id == "ctx-1",
                  "run_started -> TASK_STATE_WORKING, carrying the real task_id/context_id");
        }
    }
    {
        auto out = projector.project(make("run-1", 2, ae::run_event_kind::run_finished));
        check(out.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(out[0]).status.state ==
                                       a2a::task_state::completed,
              "run_finished -> TASK_STATE_COMPLETED");
    }
    {
        auto out = projector.project(make("run-1", 3, ae::run_event_kind::run_failed,
                                            ae::run_event_payload::RunFailed{"e", "m"}));
        check(out.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(out[0]).status.state ==
                                       a2a::task_state::failed,
              "run_failed -> TASK_STATE_FAILED");
    }
    {
        auto out = projector.project(make("run-1", 4, ae::run_event_kind::run_canceled));
        check(out.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(out[0]).status.state ==
                                       a2a::task_state::canceled,
              "run_canceled -> TASK_STATE_CANCELED");
    }

    // --- interrupts: A2A's task model natively supports these as NON-terminal states, unlike -------
    // --- AG-UI which had to END the run -- confirming 012 §5a's "continues a task" framing.        ---
    {
        auto out = projector.project(make("run-1", 5, ae::run_event_kind::input_required,
                                            ae::run_event_payload::InteractionRef{"i-1"}));
        check(out.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(out[0]).status.state ==
                                       a2a::task_state::input_required,
              "input_required -> TASK_STATE_INPUT_REQUIRED, a real native state (unlike AG-UI's need "
              "to end the run)");
        check(!a2a::is_terminal(a2a::task_state::input_required),
              "input_required's task_state is confirmed NON-terminal -- the task genuinely continues");

        auto auth = projector.project(make("run-1", 6, ae::run_event_kind::auth_required,
                                             ae::run_event_payload::InteractionRef{"i-2"}));
        check(auth.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(auth[0]).status.state ==
                                        a2a::task_state::auth_required,
              "auth_required -> TASK_STATE_AUTH_REQUIRED, A2A's own native auth state");

        auto appr = projector.project(make("run-1", 7, ae::run_event_kind::approval_requested,
                                             ae::run_event_payload::ApprovalRequested{"call-1"}));
        check(appr.size() == 1 && std::get<a2a::TaskStatusUpdateEvent>(appr[0]).status.state ==
                                        a2a::task_state::input_required,
              "approval_requested collapses onto TASK_STATE_INPUT_REQUIRED -- A2A has no distinct "
              "\"confirmation\" state, honestly mapped rather than fabricated");
    }

    // --- artifact_produced -> TaskArtifactUpdateEvent, honestly empty parts[] ------------------------
    {
        auto out = projector.project(make("run-1", 8, ae::run_event_kind::artifact_produced,
                                            ae::run_event_payload::ArtifactProduced{"art-1"}));
        check(out.size() == 1 && std::holds_alternative<a2a::TaskArtifactUpdateEvent>(out[0]),
              "artifact_produced -> TaskArtifactUpdateEvent");
        if (out.size() == 1 && std::holds_alternative<a2a::TaskArtifactUpdateEvent>(out[0])) {
            auto const& e = std::get<a2a::TaskArtifactUpdateEvent>(out[0]);
            check(e.artifact.artifact_id == "art-1" && e.artifact.parts.empty(),
                  "the artifact carries the real artifact_id, with HONESTLY empty parts[] -- "
                  "ArtifactProduced's own payload has no content to populate them from");
        }
    }

    // --- every other kind has NO A2A streaming slot -- an empty vector, not a fabricated update -----
    {
        auto turn = projector.project(
            make("run-1", 9, ae::run_event_kind::turn_started, ae::run_event_payload::Turn{0}));
        auto delta = projector.project(
            make("run-1", 10, ae::run_event_kind::model_delta,
                 ae::run_event_payload::ModelDelta{ae::run_event_payload::ModelTextDelta{"text"}}));
        auto warn = projector.project(
            make("run-1", 11, ae::run_event_kind::warning, ae::run_event_payload::Warning{"w"}));
        check(turn.empty() && delta.empty() && warn.empty(),
              "turn/model/warning events have no A2A task-lifecycle wire slot -- honestly empty, "
              "not a fabricated status transition (A2A's task model is coarser-grained than AG-UI's)");
    }

    // --- wire wrapping: the oneof picks exactly one key per alternative ------------------------------
    {
        a2a::TaskStatusUpdateEvent e;
        e.task_id      = "t";
        e.context_id   = "c";
        e.status.state = a2a::task_state::working;
        a2a::StreamResponse r = e;
        json::Value j = a2a::to_json(r);
        check(j.find("statusUpdate") != nullptr && j.find("task") == nullptr &&
                  j.find("message") == nullptr && j.find("artifactUpdate") == nullptr,
              "a TaskStatusUpdateEvent wraps under exactly the \"statusUpdate\" key on the wire, "
              "no others present");
    }

    if (g_failures == 0) {
        std::printf("test_a2a_streaming: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_streaming: %d failure(s)\n", g_failures);
    return 1;
}
