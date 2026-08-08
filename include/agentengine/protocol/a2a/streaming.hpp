#pragma once
// Implements 012-A2A-Conformance.md §2.3's streaming projection ("Streaming projects the internal run
// event stream (013 §1) -- the same source as AG-UI") and 013-UI-and-Streaming-Surfaces.md §3's own
// table row ("A2A streaming | Task status + artifact updates from the same stream"). Milestone 7
// Phase E4 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// `TaskStatusUpdateEvent`/`TaskArtifactUpdateEvent`/`StreamResponse` are the wire types §A.4 names
// (docs/research/2026-a2a-and-agui-detail.md) that D1 (`protocol/a2a/types.hpp`) did not build --
// D1's own scope was the `Task`/`Message`/`Part`/`Artifact` object model, not the streaming envelope
// around it. Deliberately narrower than a full `RunEvent -> StreamResponse` total mapping: A2A's task
// model is COARSER-GRAINED than AG-UI's own event stream (012 §1: "Task <- Run", one task-lifecycle
// state machine per run, no wire slot for turn/model/tool-call granularity) -- `A2aStreamProjector`
// projects exactly the `RunEvent` kinds that map onto a real `task_state` transition or a real
// `Artifact`, and returns nothing for every other kind (model deltas, tool-call progress, turn
// boundaries, state/warning/policy events), the same "honestly named, not silently claimed" scoping
// `protocol/agui/projection.hpp` (E2) already established for its own out-of-vocabulary kinds.
//
// `approval_requested` collapses onto `TASK_STATE_INPUT_REQUIRED`: A2A's `task_state` enum (012 §A.5)
// has no distinct "confirmation" member the way AG-UI's `Interrupt.reason` does -- `input_required` is
// the closest real state, and using it is honest (a generic "waiting on the human" state), not a
// fabricated new one.
//
// `artifact_produced`'s projected `Artifact` carries an EMPTY `parts[]`: `ArtifactProduced`'s own
// payload (`core/run_event.hpp`) is `{artifact_id}` only -- no content exists anywhere in this
// codebase's pipeline to populate real `Part`s from yet. Named here, not silently claimed as a
// complete artifact.

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/run_event.hpp"
#include "agentengine/protocol/a2a/types.hpp"

namespace agentengine::a2a {

// §A.4: `TaskStatusUpdateEvent = {taskId, contextId, status, metadata?}`.
struct TaskStatusUpdateEvent {
    std::string                 task_id;
    std::string                 context_id;
    TaskStatus                  status;
    std::optional<json::Value>  metadata;
};

// §A.4: `TaskArtifactUpdateEvent = {taskId, contextId, artifact, append?, lastChunk?, metadata?}`.
struct TaskArtifactUpdateEvent {
    std::string                 task_id;
    std::string                 context_id;
    Artifact                    artifact;
    std::optional<bool>         append;
    std::optional<bool>         last_chunk;
    std::optional<json::Value>  metadata;
};

// §A.4: `StreamResponse = oneof {task | message | statusUpdate | artifactUpdate}`.
using StreamResponse = std::variant<Task, Message, TaskStatusUpdateEvent, TaskArtifactUpdateEvent>;

[[nodiscard]] inline json::Value to_json(TaskStatusUpdateEvent const& e) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"taskId", json::Value::make_string(e.task_id)},
        {"contextId", json::Value::make_string(e.context_id)},
        {"status", to_json(e.status)}};
    if (e.metadata) members.emplace_back("metadata", *e.metadata);
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline json::Value to_json(TaskArtifactUpdateEvent const& e) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"taskId", json::Value::make_string(e.task_id)},
        {"contextId", json::Value::make_string(e.context_id)},
        {"artifact", to_json(e.artifact)}};
    if (e.append) members.emplace_back("append", json::Value::make_bool(*e.append));
    if (e.last_chunk) members.emplace_back("lastChunk", json::Value::make_bool(*e.last_chunk));
    if (e.metadata) members.emplace_back("metadata", *e.metadata);
    return json::Value::make_object(std::move(members));
}

// `oneof` on the wire -- a single-key object naming which alternative this is (the same rendering
// convention protobuf-JSON uses for a oneof field, which is exactly what `StreamResponse` is, §A.4).
[[nodiscard]] inline json::Value to_json(StreamResponse const& r) {
    return std::visit(
        [](auto const& alt) -> json::Value {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Task>) {
                return json::Value::make_object({{"task", to_json(alt)}});
            } else if constexpr (std::is_same_v<T, Message>) {
                return json::Value::make_object({{"message", to_json(alt)}});
            } else if constexpr (std::is_same_v<T, TaskStatusUpdateEvent>) {
                return json::Value::make_object({{"statusUpdate", to_json(alt)}});
            } else {
                return json::Value::make_object({{"artifactUpdate", to_json(alt)}});
            }
        },
        r);
}

class A2aStreamProjector {
public:
    A2aStreamProjector(std::string task_id, std::string context_id)
        : task_id_(std::move(task_id)), context_id_(std::move(context_id)) {}

    [[nodiscard]] std::vector<StreamResponse> project(RunEvent const& ev) {
        switch (ev.kind) {
            case run_event_kind::run_started:
                return {status(task_state::working)};
            case run_event_kind::run_finished:
                return {status(task_state::completed)};
            case run_event_kind::run_failed:
                return {status(task_state::failed)};
            case run_event_kind::run_canceled:
                return {status(task_state::canceled)};
            case run_event_kind::input_required:
                return {status(task_state::input_required)};
            case run_event_kind::auth_required:
                return {status(task_state::auth_required)};
            case run_event_kind::approval_requested:
                // See file-top comment: no native "confirmation" task_state member.
                return {status(task_state::input_required)};
            case run_event_kind::artifact_produced: {
                auto const& p = std::get<run_event_payload::ArtifactProduced>(ev.payload);
                Artifact artifact;
                artifact.artifact_id = p.artifact_id;  // parts[] intentionally empty, see file-top comment
                return {TaskArtifactUpdateEvent{task_id_, context_id_, std::move(artifact), std::nullopt,
                                                 std::nullopt, std::nullopt}};
            }
            default:
                // Every other RunEvent kind has no A2A task-lifecycle wire slot (file-top comment) --
                // an empty vector, not a fabricated status transition.
                return {};
        }
    }

private:
    [[nodiscard]] StreamResponse status(task_state s) const {
        TaskStatusUpdateEvent e;
        e.task_id     = task_id_;
        e.context_id  = context_id_;
        e.status.state = s;
        return e;
    }

    std::string task_id_;
    std::string context_id_;
};

}  // namespace agentengine::a2a
