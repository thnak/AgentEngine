#pragma once
// Implements 013-UI-and-Streaming-Surfaces.md §2.1/§2.2 -- the projection from the internal run event
// stream (`core/run_event.hpp`, real since Milestone 7 Phase A) onto AG-UI wire events
// (`protocol/agui/types.hpp`, Phase E1). Milestone 7 Phase E2
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// STATEFUL, unlike every prior Phase C/D projection in this milestone (MCP/A2A content mapping is a
// pure function of one item): AG-UI's `TEXT_MESSAGE_START/CONTENT/END` triad needs a `messageId` to
// bracket a model's incremental text output, and `RunEvent`'s own `ModelDelta` payload carries none
// (013 §1's vocabulary is silent on wire-projection bookkeeping, by design -- that is exactly this
// file's job to own). `RunEventProjector` tracks, per run_id, whether a text message is currently
// open, minting a synthesized `messageId` on `model_call_started` and closing it on
// `model_call_finished` -- the ONLY state this projector holds; every other internal event kind maps
// context-free.
//
// §2.2's own hard rule, implemented directly: a run entering `InputRequired`/`AuthRequired`/an
// approval gate does NOT pause in AG-UI's own vocabulary (it has no pause event) -- it ENDS the
// AG-UI-visible run with `RunFinishedInterrupt`, `interaction_id`/`call_id` becoming the
// `Interrupt.id` verbatim (013 §2.2: "AG-UI -- interruptId IS the interaction_id, verbatim").
// `auth_required` uses the `ae:auth_required` extension-namespaced reason (AG-UI's own `reason` enum
// has no native auth member, 013 §2.2); `approval_requested` uses the native `confirmation` reason.
//
// Honest divergences from a naive 1:1 reading of 013 §2.1's own summary table, each named at its own
// mapping below: `tool_call_delta` (the TOOL's own progress report, 013 §1 -- distinct from the
// MODEL's argument-construction streaming, which this codebase has no separate event kind for yet)
// projects to `CustomEvent`, not `TOOL_CALL_CHUNK` -- the cited research record
// (docs/research/2026-a2a-and-agui-detail.md Part B) gives no field shape for any `*_CHUNK` variant,
// and inventing one from an undocumented wire shape would be a worse violation of "research is dated
// and cited" than using the always-well-defined `CUSTOM` escape hatch 013 §2.1 itself sanctions for
// exactly this situation. `state_changed`'s payload is a free-text description, not a diffable value
// (no state-diffing engine exists anywhere in this codebase) -- it projects to `CustomEvent`, never a
// fabricated RFC 6902 patch.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/run_event.hpp"
#include "agentengine/protocol/agui/types.hpp"

namespace agentengine::agui {

class RunEventProjector {
public:
    // `thread_id` is AG-UI's own session-grouping identity (013 §2.2's "the same threadId") -- the
    // caller's session id, exactly parallel to `A2aServer`'s own `context_id` constructor parameter.
    explicit RunEventProjector(std::string thread_id) : thread_id_(std::move(thread_id)) {}

    // Projects ONE internal event into zero or more AG-UI wire events, in emission order. Zero for
    // `input_resolved`/`auth_resolved`/`approval_resolved`: by the time any of these fires, the
    // AG-UI-visible run already ended (via the matching `*_required`/`approval_requested` event's own
    // `RunFinishedInterrupt`) -- resolution feeds a NEW run's `RunAgentInput.resume[]`, a caller-side
    // concern this single-event projector does not own.
    [[nodiscard]] std::vector<AgUiEvent> project(RunEvent const& ev) {
        switch (ev.kind) {
            case run_event_kind::run_started:
                return {RunStarted{thread_id_, ev.run_id, std::nullopt}};
            case run_event_kind::run_finished:
                open_message_id_.erase(ev.run_id);
                return {RunFinishedSuccess{thread_id_, ev.run_id}};
            case run_event_kind::run_failed: {
                open_message_id_.erase(ev.run_id);
                auto const& p = std::get<run_event_payload::RunFailed>(ev.payload);
                return {RunError{p.message, p.error_code}};
            }
            case run_event_kind::run_canceled:
                open_message_id_.erase(ev.run_id);
                // §2.1: "RUN_ERROR is the SOLE error event; no other error shape exists" -- a
                // cancellation is not a success, so it is described, not silently folded into one.
                return {RunError{"run canceled", "run.canceled"}};

            case run_event_kind::turn_started: {
                auto const& p = std::get<run_event_payload::Turn>(ev.payload);
                return {StepStarted{"turn-" + std::to_string(p.turn_index)}};
            }
            case run_event_kind::turn_finished: {
                auto const& p = std::get<run_event_payload::Turn>(ev.payload);
                return {StepFinished{"turn-" + std::to_string(p.turn_index)}};
            }

            case run_event_kind::model_call_started: {
                std::string const message_id = mint_message_id(ev.run_id);
                open_message_id_[ev.run_id] = message_id;
                return {TextMessageStart{message_id, "assistant", std::nullopt}};
            }
            case run_event_kind::model_delta: {
                auto const& p = std::get<run_event_payload::ModelDelta>(ev.payload);
                std::string const& message_id = ensure_open_message(ev.run_id);
                return {TextMessageContent{message_id, p.text_delta}};
            }
            case run_event_kind::model_call_finished: {
                std::string const message_id = ensure_open_message(ev.run_id);
                open_message_id_.erase(ev.run_id);
                return {TextMessageEnd{message_id}};
            }

            case run_event_kind::tool_call_started: {
                auto const& p = std::get<run_event_payload::ToolCallStarted>(ev.payload);
                return {ToolCallStart{p.call_id, p.tool_name, std::nullopt}};
            }
            case run_event_kind::tool_call_delta: {
                // See file-top comment: CUSTOM, not TOOL_CALL_CHUNK (undocumented wire shape in the
                // cited research record).
                auto const& p = std::get<run_event_payload::ToolCallDelta>(ev.payload);
                return {CustomEvent{"ae:tool_call_progress",
                                     json::Value::make_object(
                                         {{"callId", json::Value::make_string(p.call_id)},
                                          {"progressText", json::Value::make_string(p.progress_text)}})}};
            }
            case run_event_kind::tool_call_finished: {
                // No ToolCallResult: `ToolCallFinished`'s payload carries only `{call_id, ok}`, no
                // real result CONTENT to report -- honest scoping, not a dropped event (ToolCallEnd
                // still fires).
                auto const& p = std::get<run_event_payload::ToolCallFinished>(ev.payload);
                return {ToolCallEnd{p.call_id}};
            }

            case run_event_kind::sandbox_exec_started:
            case run_event_kind::sandbox_exec_finished: {
                auto const& p = std::get<run_event_payload::SandboxExec>(ev.payload);
                bool const finished = ev.kind == run_event_kind::sandbox_exec_finished;
                return {ActivitySnapshot{
                    p.exec_id, "sandbox_exec",
                    json::Value::make_object(
                        {{"status", json::Value::make_string(finished ? "finished" : "started")}}),
                    std::nullopt}};
            }

            case run_event_kind::state_changed: {
                auto const& p = std::get<run_event_payload::StateChanged>(ev.payload);
                return {CustomEvent{"ae:state_changed", json::Value::make_string(p.description)}};
            }
            case run_event_kind::artifact_produced: {
                auto const& p = std::get<run_event_payload::ArtifactProduced>(ev.payload);
                return {CustomEvent{"ae:artifact_produced", json::Value::make_string(p.artifact_id)}};
            }

            case run_event_kind::input_required: {
                auto const& p = std::get<run_event_payload::InteractionRef>(ev.payload);
                Interrupt interrupt;
                interrupt.id     = p.interaction_id;
                interrupt.reason = "input_required";
                return {RunFinishedInterrupt{thread_id_, ev.run_id, {interrupt}}};
            }
            case run_event_kind::auth_required: {
                auto const& p = std::get<run_event_payload::InteractionRef>(ev.payload);
                Interrupt interrupt;
                interrupt.id     = p.interaction_id;
                interrupt.reason = "ae:auth_required";  // §2.2: no native auth member, extension ns
                return {RunFinishedInterrupt{thread_id_, ev.run_id, {interrupt}}};
            }
            case run_event_kind::approval_requested: {
                auto const& p = std::get<run_event_payload::ApprovalRequested>(ev.payload);
                Interrupt interrupt;
                interrupt.id           = p.call_id;  // no distinct interaction_id in this payload
                interrupt.reason       = "confirmation";
                interrupt.tool_call_id = p.call_id;
                return {RunFinishedInterrupt{thread_id_, ev.run_id, {interrupt}}};
            }

            case run_event_kind::input_resolved:
            case run_event_kind::auth_resolved:
            case run_event_kind::approval_resolved:
                return {};  // see this function's own top comment

            case run_event_kind::warning: {
                auto const& p = std::get<run_event_payload::Warning>(ev.payload);
                return {CustomEvent{"ae:warning", json::Value::make_string(p.message)}};
            }
            case run_event_kind::policy_decision: {
                auto const& p = std::get<run_event_payload::PolicyDecision>(ev.payload);
                return {CustomEvent{"ae:policy_decision", json::Value::make_string(p.description)}};
            }
        }
        return {};
    }

private:
    [[nodiscard]] std::string mint_message_id(std::string const& run_id) {
        return run_id + ":msg:" + std::to_string(++message_counter_);
    }

    // Defensive fallback: `model_delta` should always follow `model_call_started` in a well-formed
    // stream, but a caller feeding events out of order (or starting mid-stream) still gets a real,
    // synthesized message_id rather than a crash -- opened lazily, tracked exactly like the normal
    // path so `model_call_finished` closes the same id.
    [[nodiscard]] std::string const& ensure_open_message(std::string const& run_id) {
        auto it = open_message_id_.find(run_id);
        if (it == open_message_id_.end()) {
            it = open_message_id_.emplace(run_id, mint_message_id(run_id)).first;
        }
        return it->second;
    }

    std::string                                  thread_id_;
    std::unordered_map<std::string, std::string> open_message_id_;  // run_id -> currently-open messageId
    std::uint64_t                                 message_counter_ = 0;
};

}  // namespace agentengine::agui
