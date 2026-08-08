#pragma once
// Implements 013-UI-and-Streaming-Surfaces.md §2's AG-UI event vocabulary (exact identifiers cited
// from docs/research/2026-a2a-and-agui-detail.md Part B, checked against `@ag-ui/core` 0.0.57,
// 2026-07-31). Milestone 7 Phase E1 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// §2.0's own maturity note governs how this file is written: AG-UI has no clause-numbered RFC-2119
// spec, only Zod/protobuf schemas and prose -- "we pin to a schema version... and author our own
// conformance suite... we claim compatibility, not conformance." This file's own field shapes are
// therefore taken from the cited, dated research record, not from memory of "what AG-UI probably
// looks like" -- the same discipline CLAUDE.md requires ("Research is dated and cited... these moved
// twice in eight months").
//
// Deliberately narrower than the full `@ag-ui/core` surface: the `*_CHUNK` "auto-expanding
// convenience form" events (`TEXT_MESSAGE_CHUNK`, `TOOL_CALL_CHUNK`, `REASONING_MESSAGE_CHUNK`) are
// NOT built -- they are a client-side ergonomic sugar over the canonical START/CONTENT/END triad this
// file DOES emit, not a distinct wire concept a projector needs to produce. The deprecated
// `THINKING_*` family (removal targeted at 1.0.0, not shipped) is likewise not built, per §2.1's own
// "we do not emit it" rule. `MetaEvent` is explicitly Draft upstream and not built.

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/json_value.hpp"

namespace agentengine::agui {

namespace json = agentengine::json;

// ---- Lifecycle (B.2) ------------------------------------------------------------------------------

struct RunStarted {
    std::string                 thread_id;
    std::string                 run_id;
    std::optional<std::string>  parent_run_id;
};

// §2.2/B.4: a run that needs input does NOT pause -- it ENDS with `outcome: {type: "interrupt", ...}`.
// `RunFinished` therefore has two distinct shapes rather than one struct with an optional outcome, so
// a caller cannot construct the nonsensical "finished, no outcome at all" state this project's own
// "no ambient/implicit default" discipline avoids elsewhere.
struct RunFinishedSuccess {
    std::string thread_id;
    std::string run_id;
};

// B.4: `Interrupt = {id, reason, message?, toolCallId?, responseSchema?, expiresAt?, metadata?}`.
// `reason` is `tool_call` | `input_required` | `confirmation`, plus a `<framework>:<name>` extension
// namespace -- 013 §2.2 uses this namespace for `auth` (`ae:auth_required`), since AG-UI's own enum
// has no native auth member.
struct Interrupt {
    std::string                 id;
    std::string                 reason;
    std::optional<std::string>  message;
    std::optional<std::string>  tool_call_id;
    std::optional<json::Value>  response_schema;
    std::optional<std::string>  expires_at;
    std::optional<json::Value>  metadata;
};

struct RunFinishedInterrupt {
    std::string             thread_id;
    std::string             run_id;
    std::vector<Interrupt>  interrupts;  // B.4: every open interrupt, never a partial list
};

// B.4: "RUN_ERROR is the SOLE error event; no other error shape exists."
struct RunError {
    std::string                 message;
    std::optional<std::string>  code;
};

struct StepStarted { std::string step_name; };
struct StepFinished { std::string step_name; };

// ---- Text (B.2) -------------------------------------------------------------------------------------

struct TextMessageStart {
    std::string                 message_id;
    std::string                 role;
    std::optional<std::string>  name;
};
struct TextMessageContent {
    std::string message_id;
    std::string delta;
};
struct TextMessageEnd { std::string message_id; };

// ---- Tool call (B.2) --------------------------------------------------------------------------------

struct ToolCallStart {
    std::string                 tool_call_id;
    std::string                 tool_call_name;
    std::optional<std::string>  parent_message_id;
};
struct ToolCallArgs {
    std::string tool_call_id;
    std::string delta;
};
struct ToolCallEnd { std::string tool_call_id; };
struct ToolCallResult {
    std::string                 message_id;
    std::string                 tool_call_id;
    json::Value                 content;
    std::optional<std::string>  role;
};

// ---- State (B.2) ------------------------------------------------------------------------------------
// §2.1: "STATE_DELTA and ACTIVITY_DELTA use RFC 6902 JSON Patch." `patch`/`delta` here carry whatever
// JSON Patch array the caller supplies -- this project has no state-DIFFING engine yet (named,
// projection.hpp's own scope note), so these types are the wire SHAPE, not a diff generator.

struct StateSnapshot { json::Value snapshot; };
struct StateDelta { json::Value patch; };            // an RFC 6902 patch-operation array
struct MessagesSnapshot { json::Value messages; };   // §2.1: all-or-nothing per role

// ---- Activity (B.2) ---------------------------------------------------------------------------------

struct ActivitySnapshot {
    std::string           message_id;
    std::string           activity_type;
    json::Value            content;
    std::optional<bool>    replace;
};
struct ActivityDelta {
    std::string  message_id;
    std::string  activity_type;
    json::Value   patch;
};

// ---- Reasoning (B.2) --------------------------------------------------------------------------------

struct ReasoningStart {};
struct ReasoningMessageStart { std::string message_id; };
struct ReasoningMessageContent {
    std::string message_id;
    std::string delta;
};
struct ReasoningMessageEnd { std::string message_id; };
struct ReasoningEnd {};

// 003 §1: encrypted reasoning "passes through opaque" -- `encrypted_value` is stored/forwarded
// verbatim, never decrypted by this engine.
struct ReasoningEncryptedValue {
    std::string subtype;      // "tool-call" | "message"
    std::string entity_id;
    std::string encrypted_value;
};

// ---- Special (B.2) ----------------------------------------------------------------------------------

struct RawEvent {
    json::Value                  event;
    std::optional<std::string>   source;
};

// §2.1's own escape hatch: "anything AG-UI cannot express is carried in CUSTOM with a namespaced type
// id, never silently dropped." `projection.hpp` uses this for every internal `run_event_kind` with no
// first-class AG-UI slot.
struct CustomEvent {
    std::string  name;
    json::Value   value;
};

using AgUiEvent =
    std::variant<RunStarted, RunFinishedSuccess, RunFinishedInterrupt, RunError, StepStarted, StepFinished,
                 TextMessageStart, TextMessageContent, TextMessageEnd, ToolCallStart, ToolCallArgs,
                 ToolCallEnd, ToolCallResult, StateSnapshot, StateDelta, MessagesSnapshot, ActivitySnapshot,
                 ActivityDelta, ReasoningStart, ReasoningMessageStart, ReasoningMessageContent,
                 ReasoningMessageEnd, ReasoningEnd, ReasoningEncryptedValue, RawEvent, CustomEvent>;

namespace detail {

[[nodiscard]] inline json::Value opt_string(std::optional<std::string> const& s) {
    return s ? json::Value::make_string(*s) : json::Value{};
}

[[nodiscard]] inline json::Value interrupt_to_json(Interrupt const& i) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"id", json::Value::make_string(i.id)}, {"reason", json::Value::make_string(i.reason)}};
    if (i.message) members.emplace_back("message", json::Value::make_string(*i.message));
    if (i.tool_call_id) members.emplace_back("toolCallId", json::Value::make_string(*i.tool_call_id));
    if (i.response_schema) members.emplace_back("responseSchema", *i.response_schema);
    if (i.expires_at) members.emplace_back("expiresAt", json::Value::make_string(*i.expires_at));
    if (i.metadata) members.emplace_back("metadata", *i.metadata);
    return json::Value::make_object(std::move(members));
}

}  // namespace detail

// The exact identifier for each alternative (013 §2.1's own citation list, verbatim).
[[nodiscard]] inline std::string_view event_type_name(AgUiEvent const& event) noexcept {
    return std::visit(
        []<class T>(T const&) -> std::string_view {
            if constexpr (std::is_same_v<T, RunStarted>) return "RUN_STARTED";
            else if constexpr (std::is_same_v<T, RunFinishedSuccess>) return "RUN_FINISHED";
            else if constexpr (std::is_same_v<T, RunFinishedInterrupt>) return "RUN_FINISHED";
            else if constexpr (std::is_same_v<T, RunError>) return "RUN_ERROR";
            else if constexpr (std::is_same_v<T, StepStarted>) return "STEP_STARTED";
            else if constexpr (std::is_same_v<T, StepFinished>) return "STEP_FINISHED";
            else if constexpr (std::is_same_v<T, TextMessageStart>) return "TEXT_MESSAGE_START";
            else if constexpr (std::is_same_v<T, TextMessageContent>) return "TEXT_MESSAGE_CONTENT";
            else if constexpr (std::is_same_v<T, TextMessageEnd>) return "TEXT_MESSAGE_END";
            else if constexpr (std::is_same_v<T, ToolCallStart>) return "TOOL_CALL_START";
            else if constexpr (std::is_same_v<T, ToolCallArgs>) return "TOOL_CALL_ARGS";
            else if constexpr (std::is_same_v<T, ToolCallEnd>) return "TOOL_CALL_END";
            else if constexpr (std::is_same_v<T, ToolCallResult>) return "TOOL_CALL_RESULT";
            else if constexpr (std::is_same_v<T, StateSnapshot>) return "STATE_SNAPSHOT";
            else if constexpr (std::is_same_v<T, StateDelta>) return "STATE_DELTA";
            else if constexpr (std::is_same_v<T, MessagesSnapshot>) return "MESSAGES_SNAPSHOT";
            else if constexpr (std::is_same_v<T, ActivitySnapshot>) return "ACTIVITY_SNAPSHOT";
            else if constexpr (std::is_same_v<T, ActivityDelta>) return "ACTIVITY_DELTA";
            else if constexpr (std::is_same_v<T, ReasoningStart>) return "REASONING_START";
            else if constexpr (std::is_same_v<T, ReasoningMessageStart>) return "REASONING_MESSAGE_START";
            else if constexpr (std::is_same_v<T, ReasoningMessageContent>) return "REASONING_MESSAGE_CONTENT";
            else if constexpr (std::is_same_v<T, ReasoningMessageEnd>) return "REASONING_MESSAGE_END";
            else if constexpr (std::is_same_v<T, ReasoningEnd>) return "REASONING_END";
            else if constexpr (std::is_same_v<T, ReasoningEncryptedValue>) return "REASONING_ENCRYPTED_VALUE";
            else if constexpr (std::is_same_v<T, RawEvent>) return "RAW";
            else if constexpr (std::is_same_v<T, CustomEvent>) return "CUSTOM";
        },
        event);
}

[[nodiscard]] inline json::Value to_json(AgUiEvent const& event) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"type", json::Value::make_string(std::string(event_type_name(event)))}};

    std::visit(
        [&members]<class T>(T const& e) {
            if constexpr (std::is_same_v<T, RunStarted>) {
                members.emplace_back("threadId", json::Value::make_string(e.thread_id));
                members.emplace_back("runId", json::Value::make_string(e.run_id));
                if (e.parent_run_id) members.emplace_back("parentRunId", json::Value::make_string(*e.parent_run_id));
            } else if constexpr (std::is_same_v<T, RunFinishedSuccess>) {
                members.emplace_back("threadId", json::Value::make_string(e.thread_id));
                members.emplace_back("runId", json::Value::make_string(e.run_id));
                members.emplace_back("outcome", json::Value::make_object(
                                                     {{"type", json::Value::make_string("success")}}));
            } else if constexpr (std::is_same_v<T, RunFinishedInterrupt>) {
                members.emplace_back("threadId", json::Value::make_string(e.thread_id));
                members.emplace_back("runId", json::Value::make_string(e.run_id));
                std::vector<json::Value> interrupts;
                interrupts.reserve(e.interrupts.size());
                for (Interrupt const& i : e.interrupts) interrupts.push_back(detail::interrupt_to_json(i));
                members.emplace_back(
                    "outcome",
                    json::Value::make_object({{"type", json::Value::make_string("interrupt")},
                                               {"interrupts", json::Value::make_array(std::move(interrupts))}}));
            } else if constexpr (std::is_same_v<T, RunError>) {
                members.emplace_back("message", json::Value::make_string(e.message));
                if (e.code) members.emplace_back("code", json::Value::make_string(*e.code));
            } else if constexpr (std::is_same_v<T, StepStarted> || std::is_same_v<T, StepFinished>) {
                members.emplace_back("stepName", json::Value::make_string(e.step_name));
            } else if constexpr (std::is_same_v<T, TextMessageStart>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("role", json::Value::make_string(e.role));
                if (e.name) members.emplace_back("name", json::Value::make_string(*e.name));
            } else if constexpr (std::is_same_v<T, TextMessageContent>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("delta", json::Value::make_string(e.delta));
            } else if constexpr (std::is_same_v<T, TextMessageEnd>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
            } else if constexpr (std::is_same_v<T, ToolCallStart>) {
                members.emplace_back("toolCallId", json::Value::make_string(e.tool_call_id));
                members.emplace_back("toolCallName", json::Value::make_string(e.tool_call_name));
                if (e.parent_message_id)
                    members.emplace_back("parentMessageId", json::Value::make_string(*e.parent_message_id));
            } else if constexpr (std::is_same_v<T, ToolCallArgs>) {
                members.emplace_back("toolCallId", json::Value::make_string(e.tool_call_id));
                members.emplace_back("delta", json::Value::make_string(e.delta));
            } else if constexpr (std::is_same_v<T, ToolCallEnd>) {
                members.emplace_back("toolCallId", json::Value::make_string(e.tool_call_id));
            } else if constexpr (std::is_same_v<T, ToolCallResult>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("toolCallId", json::Value::make_string(e.tool_call_id));
                members.emplace_back("content", e.content);
                if (e.role) members.emplace_back("role", json::Value::make_string(*e.role));
            } else if constexpr (std::is_same_v<T, StateSnapshot>) {
                members.emplace_back("snapshot", e.snapshot);
            } else if constexpr (std::is_same_v<T, StateDelta>) {
                members.emplace_back("delta", e.patch);
            } else if constexpr (std::is_same_v<T, MessagesSnapshot>) {
                members.emplace_back("messages", e.messages);
            } else if constexpr (std::is_same_v<T, ActivitySnapshot>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("activityType", json::Value::make_string(e.activity_type));
                members.emplace_back("content", e.content);
                if (e.replace) members.emplace_back("replace", json::Value::make_bool(*e.replace));
            } else if constexpr (std::is_same_v<T, ActivityDelta>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("activityType", json::Value::make_string(e.activity_type));
                members.emplace_back("patch", e.patch);
            } else if constexpr (std::is_same_v<T, ReasoningStart> || std::is_same_v<T, ReasoningEnd>) {
                // no fields beyond {type}
            } else if constexpr (std::is_same_v<T, ReasoningMessageStart> ||
                                  std::is_same_v<T, ReasoningMessageEnd>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
            } else if constexpr (std::is_same_v<T, ReasoningMessageContent>) {
                members.emplace_back("messageId", json::Value::make_string(e.message_id));
                members.emplace_back("delta", json::Value::make_string(e.delta));
            } else if constexpr (std::is_same_v<T, ReasoningEncryptedValue>) {
                members.emplace_back("subtype", json::Value::make_string(e.subtype));
                members.emplace_back("entityId", json::Value::make_string(e.entity_id));
                members.emplace_back("encryptedValue", json::Value::make_string(e.encrypted_value));
            } else if constexpr (std::is_same_v<T, RawEvent>) {
                members.emplace_back("event", e.event);
                if (e.source) members.emplace_back("source", json::Value::make_string(*e.source));
            } else if constexpr (std::is_same_v<T, CustomEvent>) {
                members.emplace_back("name", json::Value::make_string(e.name));
                members.emplace_back("value", e.value);
            }
        },
        event);

    return json::Value::make_object(std::move(members));
}

}  // namespace agentengine::agui
