#pragma once
// Implements 013-UI-and-Streaming-Surfaces.md §1 -- the one internal run event stream, projected
// onto every external surface (AG-UI, A2A streaming, MCP progress, OpenAI-compatible SSE, §2-§3).
// Milestone 7 Phase A (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// The VOCABULARY here is total -- every event kind §1's own twenty-line list names has a real enum
// value and a real payload type, because 013 §2's AG-UI mapping table and later phases' MCP/A2A
// projections need the whole shape to bind against, not a subset re-designed per consumer (§1: "one
// internal run event stream ... so that adding a surface is writing a projection, not a second
// event model"). What is NOT total yet is WIRING: this phase's own emitter (agent_session.hpp) fires
// real events at AgentSession's actual turn-loop boundaries (Run/Turn/ModelCall/StateChanged) --
// ToolCall*/SandboxExec*/ArtifactProduced/ModelDelta/RunCanceled have no real producer inside
// AgentSession today. Its turn loop makes exactly one synchronous, non-streaming
// `chat_client_->chat()` call and never reaches the tool pipeline at all (confirmed by grep -- a
// pre-existing gap this phase does not silently paper over; `core/tool_pipeline.hpp`'s `invoke_tool`
// is called only from `agent_registry.hpp::invoke_agent_tool`, never from `AgentSession::handle`).
// Their kinds and payload shapes exist now so a later real producer (Phase B's `background_task`,
// Phase C's MCP tool exposure calling `invoke_agent_tool`, a future streaming turn loop) has
// something to emit into rather than inventing the type at the point of first use.
//
// `StandingEffect` visibility (006 §6b, Phase B) rides `state_changed` per 013 §1's own rule --
// "not a new event pair" -- so Phase B needs no addition here, only a real caller of
// `emit_event(..., state_changed, ...)`.
//
// NOT durably serialized (no QUARK_SERIALIZE) -- this is a live, in-memory stream ridden over
// `ae::stream<T>` (boxed, non-trivially-copyable T is fine, core/stream.hpp), not a persisted log.
// Unlike `Message`/`ContentItem` (content_record.hpp, Milestone 6 Phase F) a `std::variant` payload
// here is NOT the same hazard -- nothing calls `quark_describe` on it, so `FingerprintFolder`'s
// one-sample-branch problem does not apply. 013 §6 G4 ("a recorded stream replays into a UI
// identically") needs a durable form eventually; that is a named gap for whichever phase builds
// recording, not solved here.

#include <cstdint>
#include <string>
#include <variant>

namespace agentengine {

enum class run_event_kind {  // ae-naming-lint: allow run_event_kind — 013 §1 names this vocabulary normatively; 027 has not been updated to list it
    run_started, run_finished, run_failed, run_canceled,
    turn_started, turn_finished,
    model_call_started, model_delta, model_call_finished,
    tool_call_started, tool_call_delta, tool_call_finished,
    sandbox_exec_started, sandbox_exec_finished,
    state_changed, artifact_produced,
    input_required, input_resolved,
    auth_required, auth_resolved,
    approval_requested, approval_resolved,
    warning, policy_decision,
};

namespace run_event_payload {

// RunStarted, RunCanceled, ModelCallStarted, ModelCallFinished: nothing beyond the common
// run_id/seq/kind fields is real yet.
struct Empty {};

struct RunFailed {
    std::string error_code;
    std::string message;
};

// TurnStarted/TurnFinished share this shape (001 §2's turn_index).
struct Turn {
    std::uint64_t turn_index = 0;
};

// The *model's* incrementally-produced text -- distinct from ToolCallDelta below (013 §1: "that is
// the model incrementally producing a call's arguments before invocation starts").
struct ModelDelta {
    std::string text_delta;
};

struct ToolCallStarted {
    std::string call_id;
    std::string tool_name;
};

// 013 §1: "a call to EffectContext.report_progress during invoke() (006 §6a) is the only source."
struct ToolCallDelta {
    std::string call_id;
    std::string progress_text;
};

struct ToolCallFinished {
    std::string call_id;
    bool        ok = false;
};

// SandboxExecStarted/Finished share this shape.
struct SandboxExec {
    std::string exec_id;
};

// 013 §1: "a run's set of active StandingEffects is exposed as part of [StateChanged]" (006 §6b) --
// this same generic payload also covers ordinary token-budget/state-mutation visibility, one shape
// for both rather than a StandingEffect-specific sibling.
struct StateChanged {
    std::string description;
};

struct ArtifactProduced {
    std::string artifact_id;
};

// InputRequired/InputResolved and AuthRequired/AuthResolved all key off one Interaction identity
// (001 §2, 013 §2.2's interaction_id mapping) -- one payload shape for all four kinds, distinguished
// by run_event_kind alone.
struct InteractionRef {
    std::string interaction_id;
};

// ADR-029: `interaction_id` correlates these back to the `Interaction` (interaction.hpp)
// `AgentSession::handle()` mints when suspending a round for real human approval -- without it, a
// live consumer has no way to build the eventual `ResolveInteraction{interaction_id, approved}`
// resume call. Appended... except these two payloads shipped with only `call_id` before ADR-029
// and had zero real producers (run_event.hpp's own prior top comment), so widening the struct here
// is a genuine field addition, not a break of any real wire contract yet exercised.
struct ApprovalRequested {
    std::string call_id;
    std::string interaction_id;
};

struct ApprovalResolved {
    std::string call_id;
    bool        approved = false;
    std::string interaction_id;
};

struct Warning {
    std::string message;
};

struct PolicyDecision {
    std::string description;
};

}  // namespace run_event_payload

using RunEventPayload = std::variant<run_event_payload::Empty, run_event_payload::RunFailed,
                                      run_event_payload::Turn, run_event_payload::ModelDelta,
                                      run_event_payload::ToolCallStarted, run_event_payload::ToolCallDelta,
                                      run_event_payload::ToolCallFinished, run_event_payload::SandboxExec,
                                      run_event_payload::StateChanged, run_event_payload::ArtifactProduced,
                                      run_event_payload::InteractionRef, run_event_payload::ApprovalRequested,
                                      run_event_payload::ApprovalResolved, run_event_payload::Warning,
                                      run_event_payload::PolicyDecision>;

// 013 §1: "Ordered and monotonic per run, with a sequence number." `seq` starts at 1 for the first
// event a given run emits -- 0 is never a real sequence number, so a default-constructed RunEvent is
// recognizably not a real one.
struct RunEvent {  // ae-naming-lint: allow RunEvent — 013 §1 names this concept normatively; 027 has not been updated to list it
    std::string     run_id;
    std::uint64_t   seq = 0;
    run_event_kind  kind = run_event_kind::run_started;
    RunEventPayload payload = run_event_payload::Empty{};
};

}  // namespace agentengine
