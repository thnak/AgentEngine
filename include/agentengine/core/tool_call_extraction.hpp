#pragma once
// Shared helpers for extracting `ToolCall`s from a model response and folding `ToolResult`s back
// into a `role::tool` message. Previously three independent, drifting copies existed (none in
// `core/`): `tools/cli_chat.cpp`, `tests/test_agent_session_live_multitool_e2e.cpp`,
// `tests/test_agent_session_skills_live_e2e.cpp` — each hand-rolling its own round loop OUTSIDE
// `AgentSession`, since `AgentSession::handle()` used to make exactly one model call per run and
// never resolved a tool call itself.
//
// Consolidated here as part of moving the tool-call loop INSIDE `AgentSession::handle()`
// (agent_session.hpp) — this is also where a real, previously-undetected security bug is fixed
// exactly once instead of independently in four places: none of the three original copies threaded
// `ToolCall::provenance` into `ToolCallRequest::provenance`, silently defaulting every call to
// `call_provenance::vendor_structured`. That default lets a `text_derived` call (a laundered,
// model-injected tool call — the confused-deputy shape ADR-023 §4b Finding 1 closed) bypass the
// strict `is_auto_declassifiable_text_derived_call` gate in `invoke_tool`'s step 5
// (tool_pipeline.hpp) and get evaluated under the target tool's own, possibly `never_require`,
// `approval_mode` instead. `tool_call_request_of` below is the one place this gets threaded
// correctly; every caller building a `ToolCallRequest` from a live `ToolCall` should go through it
// rather than hand-building the aggregate.

#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// Every `ToolCall` content item in a message, in declared order — a model's response may mix text
// and tool calls (or carry none), so this is a filter, not an exhaustiveness claim about the
// message's other content.
[[nodiscard]] inline std::vector<ToolCall> tool_calls_of(Message const& m) {
    std::vector<ToolCall> out;
    for (ContentItem const& item : m.content) {
        if (auto const* tc = std::get_if<ToolCall>(&item.value)) out.push_back(*tc);
    }
    return out;
}

// The concatenation of every `Text` content item's own text — used only for CLI/test display, never
// for anything that feeds back into a tool call or a capability decision.
[[nodiscard]] inline std::string text_of(Message const& m) {
    std::string out;
    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

// Builds the `ToolCallRequest` `invoke_tool`/`invoke_agent_tool` expect from a live `ToolCall`,
// threading `provenance` through correctly (the fix this header exists to centralize — see the
// file-top comment). `call_index` is the caller's own ordinal for this call within the current
// round (019 §3's idempotency-key derivation; the pipeline does not track this itself).
// `arguments_tainted` is always `true` here: every `ToolCall` this function ever sees originates
// from a model response, by construction (003 §2).
[[nodiscard]] inline ToolCallRequest tool_call_request_of(ToolCall const& call, std::uint64_t call_index) {
    auto parsed = json::parse(call.arguments_json);
    return ToolCallRequest{call.call_id, call.tool_name,
                            parsed ? *parsed : json::Value::make_object({}),
                            /*arguments_tainted=*/true, call_index, call.provenance};
}

// Folds every result from one round into a single `role::tool` message — the shape a `StartRun`'s
// next input must take, and the only structurally sound one when a turn resolves more than one
// parallel `ToolCall` (one `Message` per `StartRun`; `translate_message_to_wire`,
// protocol/openai/chat_client.hpp, is what lets N `ToolResult` items in one AE message become N
// correctly `tool_call_id`-addressed wire messages).
[[nodiscard]] inline Message tool_results_message(std::vector<ToolResult> results) {
    Message m;
    m.role = role::tool;
    for (ToolResult& r : results) {
        ContentItem item;
        item.origin = content_origin::tool;
        item.value  = std::move(r);
        m.content.push_back(std::move(item));
    }
    return m;
}

} // namespace agentengine
