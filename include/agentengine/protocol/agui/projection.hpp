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
// `model_call_finished`. Issue #49 adds a second, independent piece of state -- per run_id, whether a
// REASONING_START/REASONING_MESSAGE_START bracket is currently open -- opened lazily on the first
// `ModelReasoningDelta` and closed before whatever comes next (a `Text`/`ModelToolCallArgumentDelta`
// delta, or `model_call_finished`), never left dangling past its own model call. Every other internal
// event kind still maps context-free.
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
#include "agentengine/rt/message_codec.hpp"

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
                open_reasoning_message_id_.erase(ev.run_id);
                return {RunFinishedSuccess{thread_id_, ev.run_id}};
            case run_event_kind::run_failed: {
                open_message_id_.erase(ev.run_id);
                open_reasoning_message_id_.erase(ev.run_id);
                auto const& p = std::get<run_event_payload::RunFailed>(ev.payload);
                return {RunError{p.message, p.error_code}};
            }
            case run_event_kind::run_canceled:
                open_message_id_.erase(ev.run_id);
                open_reasoning_message_id_.erase(ev.run_id);
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
                if (auto const* text = std::get_if<run_event_payload::ModelTextDelta>(&p.value)) {
                    // Issue #49: a reasoning trace that already opened its own REASONING_*
                    // bracket for this run must close BEFORE the answer's own TEXT_MESSAGE_CONTENT --
                    // §2.1's "the projection is total for what we emit" rule, and the same "close
                    // whatever's open before starting something else" discipline the file banner's own
                    // `model_call_started`/`model_call_finished` pairing already follows.
                    std::vector<AgUiEvent> out;
                    close_open_reasoning(ev.run_id, out);
                    std::string const& message_id = ensure_open_message(ev.run_id);
                    out.push_back(TextMessageContent{message_id, text->text});
                    return out;
                }
                if (auto const* reasoning = std::get_if<run_event_payload::ModelReasoningDelta>(&p.value)) {
                    // Issue #49: before `ModelReasoningDelta` existed, this branch was unreachable --
                    // a `Reasoning`-kind delta never survived far enough to reach a live `model_delta`
                    // RunEvent at all (rt/agent_session_trust.hpp's own gap). Brackets each run's
                    // reasoning as one REASONING_START/REASONING_MESSAGE_START/.../REASONING_END span,
                    // mirroring the TEXT_MESSAGE triad's own message-id bookkeeping exactly (013 §2.1:
                    // "Reasoning* -> ModelDelta (reasoning)").
                    std::vector<AgUiEvent> out;
                    std::string const& message_id = ensure_open_reasoning(ev.run_id, out);
                    out.push_back(ReasoningMessageContent{message_id, reasoning->text});
                    return out;
                }
                // ModelToolCallArgumentDelta -- AG-UI already has a native shape for exactly this
                // (TOOL_CALL_ARGS), unlike tool_call_delta's own progress-report case below (no
                // documented *_CHUNK wire shape existed for that one).
                std::vector<AgUiEvent> out;
                close_open_reasoning(ev.run_id, out);
                auto const& arg = std::get<run_event_payload::ModelToolCallArgumentDelta>(p.value);
                out.push_back(ToolCallArgs{arg.call_id, arg.arguments_fragment});
                return out;
            }
            case run_event_kind::model_call_finished: {
                // A turn that reasoned right up to its own end (no trailing Text delta) would
                // otherwise leave its REASONING_* bracket dangling past TEXT_MESSAGE_END -- close it
                // first, same discipline as the `ModelTextDelta`/`ModelToolCallArgumentDelta`
                // branches above.
                std::vector<AgUiEvent> out;
                close_open_reasoning(ev.run_id, out);
                // A COPY, not a reference: `erase()` on the very next line would otherwise leave
                // `message_id` dangling into a just-freed map node before `push_back` below reads it
                // (the original code's own `std::string const message_id = ...` was a copy for exactly
                // this reason -- lost when this branch was restructured to build `out` incrementally).
                std::string const message_id = ensure_open_message(ev.run_id);
                open_message_id_.erase(ev.run_id);
                out.push_back(TextMessageEnd{message_id});
                return out;
            }

            case run_event_kind::tool_call_started: {
                auto const& p = std::get<run_event_payload::ToolCallStarted>(ev.payload);
                return {ToolCallStart{p.call_id, p.tool_name, std::nullopt}};
            }
            case run_event_kind::tool_call_delta: {
                // See file-top comment: CUSTOM, not TOOL_CALL_CHUNK (undocumented wire shape in the
                // cited research record). `content` now carries the engine's whole ContentItem
                // vocabulary (unified-streaming-design-draft.md §5, Piece E) -- reuses the same
                // codec `ToolResult` content already goes through, rather than a bespoke shape.
                auto const& p = std::get<run_event_payload::ToolCallDelta>(ev.payload);
                return {CustomEvent{"ae:tool_call_progress",
                                     json::Value::make_object(
                                         {{"callId", json::Value::make_string(p.call_id)},
                                          {"content", rt::content_item_to_json(p.content)}})}};
            }
            case run_event_kind::tool_call_finished: {
                // unified-streaming-design-draft.md §2, Piece C: `ToolCallFinished` now carries the
                // real `ToolResult` -- project both ToolCallEnd (unchanged) and AG-UI's own
                // already-wire-typed ToolCallResult, closing this file's own previously-honest gap
                // ("no real result CONTENT to report").
                auto const& p = std::get<run_event_payload::ToolCallFinished>(ev.payload);
                std::vector<json::Value> content_items;
                content_items.reserve(p.result.content.size());
                for (ContentItem const& item : p.result.content) {
                    content_items.push_back(rt::content_item_to_json(item));
                }
                return {ToolCallEnd{p.call_id},
                        ToolCallResult{/*message_id=*/mint_message_id(ev.run_id), p.call_id,
                                       json::Value::make_array(std::move(content_items)), std::nullopt}};
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
                // If a `codeact_ask_requested` for this same interaction arrived first, its prompt
                // rides out on THIS event, in §2.2's own native `Interrupt.message` slot. See that
                // case below for why the prompt cannot be delivered as a separate later event.
                if (auto it = pending_ask_prompt_.find(p.interaction_id); it != pending_ask_prompt_.end()) {
                    interrupt.message = it->second;
                    pending_ask_prompt_.erase(it);
                }
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

            // Carries the one thing `input_required`'s `InteractionRef` payload cannot: the actual
            // question text. run_event.hpp states the purpose -- "so a live consumer sees the actual
            // prompt text without a second round trip" -- which this projector defeated by dropping
            // the kind entirely (a -Wswitch warning was reporting exactly that).
            //
            // It projects to NOTHING of its own, deliberately. AgentSession emits it immediately
            // BEFORE the paired `input_required`, whose projection is a terminal
            // `RunFinishedInterrupt`; §2.1 says a run "ends with exactly one of RUN_FINISHED /
            // RUN_ERROR", and §2.2 adds a hard ordering obligation -- whatever a resume will need
            // must be emitted BEFORE the interrupt-bearing RUN_FINISHED, because "emitting it after
            // is unrecoverable, the run is over". A separate CUSTOM event carrying the prompt is
            // therefore wrong in BOTH directions: after the interrupt it is unreachable, and before
            // it, it is a second event where §2.2's `Interrupt.message` is the native slot for
            // exactly this. So the prompt is stashed here and rides out ON the interrupt above.
            //
            // Keyed by `interaction_id`, not `run_id`: ADR-057 §9 reuses one interaction id across a
            // chained second/third `agent.ask()`, so a later prompt correctly overwrites an earlier
            // one for the same suspension rather than accumulating.
            case run_event_kind::codeact_ask_requested: {
                auto const& p = std::get<run_event_payload::CodeActAskRequested>(ev.payload);
                pending_ask_prompt_[p.interaction_id] = p.prompt;
                return {};
            }

            // OQ-21: deliberately NOT its own RunFinishedInterrupt, unlike approval_requested --
            // §2.1 permits exactly one terminal event per run, and the paired input_required event
            // (emitted right after this one, same interaction_id) already carries the interrupt.
            // Unlike codeact_ask_requested there is no free-text prompt to stash and ride out on
            // it -- the interaction_id alone is sufficient correlation for a consumer that also
            // reads this event's own {call_id, interaction_id, tool_name} out-of-band (e.g. via a
            // separate MCP/A2A channel a hook-aware host wires up itself). Without this `case`,
            // the switch's own lack of a `default:` means an unmatched run_event_kind falls
            // through to `return {}` below silently -- a real, previously-found AG-UI observability
            // gap, not merely a style choice to add the case explicitly.
            case run_event_kind::hook_decision_requested:
                return {};

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

    // Issue #49: opens the run's REASONING_START/REASONING_MESSAGE_START bracket (appended to `out`)
    // the first time a reasoning delta arrives for it; a subsequent delta for the same still-open span
    // just returns the existing id, appending nothing.
    [[nodiscard]] std::string const& ensure_open_reasoning(std::string const& run_id,
                                                             std::vector<AgUiEvent>& out) {
        auto it = open_reasoning_message_id_.find(run_id);
        if (it == open_reasoning_message_id_.end()) {
            std::string const message_id = mint_message_id(run_id);
            out.push_back(ReasoningStart{});
            out.push_back(ReasoningMessageStart{message_id});
            it = open_reasoning_message_id_.emplace(run_id, message_id).first;
        }
        return it->second;
    }

    // Closes a still-open reasoning bracket (REASONING_MESSAGE_END/REASONING_END, appended to `out`)
    // -- a no-op when none is open, so every call site can call this unconditionally before opening or
    // closing whatever comes next.
    void close_open_reasoning(std::string const& run_id, std::vector<AgUiEvent>& out) {
        auto it = open_reasoning_message_id_.find(run_id);
        if (it == open_reasoning_message_id_.end()) return;
        out.push_back(ReasoningMessageEnd{it->second});
        out.push_back(ReasoningEnd{});
        open_reasoning_message_id_.erase(it);
    }

    std::string                                  thread_id_;
    std::unordered_map<std::string, std::string> open_message_id_;  // run_id -> currently-open messageId
    // run_id -> currently-open reasoning messageId (issue #49) -- absent means no REASONING_* bracket
    // is currently open for that run, exactly parallel to `open_message_id_`'s own absence convention.
    std::unordered_map<std::string, std::string> open_reasoning_message_id_;
    // interaction_id -> the agent.ask() prompt awaiting the paired `input_required` that carries it
    // out on `Interrupt.message`. Erased on consumption; an entry only ever outlives its run if the
    // caller feeds a `codeact_ask_requested` with no matching `input_required`, which AgentSession
    // never emits.
    std::unordered_map<std::string, std::string> pending_ask_prompt_;
    std::uint64_t                                 message_counter_ = 0;
};

}  // namespace agentengine::agui
