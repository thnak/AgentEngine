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
//
// decisions/ADR-196-per-call-approval-decisions.md §7 (issue #111 A2/A3): `make_call_ids_unique()` -- the engine,
// not the model, owns the identity of the calls in one response.

#include <cstdint>
#include <string>
#include <unordered_set>
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

// ADR-196 §7 (issue #111 A2/A3): a call id is model output (I3), yet approvals, per-call decisions, hook answers,
// audit records and tool results are all addressed by it. Two calls in one response with the same id made one
// approval cover two effects, and a denial of one call deny another. So the engine makes the ids of one response
// unique before anything records them: the first call with an id keeps it; each later call with the same id (or a
// second empty id) is renamed `<id>_ae<n>`, choosing the smallest n that no call in the message uses. The renamed id
// is what every event, decision and result then names. Returns one {original, renamed} pair per rename, in order.
struct CallIdRename {  // ae-naming-lint: allow CallIdRename — ADR-196 §7
    std::string from;
    std::string to;
};
[[nodiscard]] inline std::vector<CallIdRename> make_call_ids_unique(Message& m) {
    std::vector<CallIdRename> renames;
    std::unordered_set<std::string> taken;  // every id the message carries, plus every id minted here
    for (ContentItem const& item : m.content) {
        if (auto const* tc = std::get_if<ToolCall>(&item.value)) taken.insert(tc->call_id);
    }
    std::unordered_set<std::string> used;  // ids already given to an earlier call
    for (ContentItem& item : m.content) {
        auto* tc = std::get_if<ToolCall>(&item.value);
        if (tc == nullptr) continue;
        if (used.insert(tc->call_id).second) continue;
        std::string const base = tc->call_id.empty() ? std::string{"call"} : tc->call_id;
        std::string minted;
        for (std::uint64_t n = 1;; ++n) {
            minted = base + "_ae" + std::to_string(n);
            if (!taken.contains(minted)) break;
        }
        taken.insert(minted);
        used.insert(minted);
        renames.push_back(CallIdRename{tc->call_id, minted});
        tc->call_id = std::move(minted);
    }
    return renames;
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
//
// ADR-197 (006 §3 step 2, "reject; do not coerce"): argument text that does not parse is NOT turned
// into `{}` any more. The request carries the parser's message in `arguments_parse_error`, and the
// pipeline refuses the call as `tool.malformed_arguments` before the tool runs. `arguments` stays an
// empty object only as a placeholder (hooks and approval prompts still need a value to show).
// Empty or all-whitespace text is the one exception, and it is deliberate: providers send "" for a
// call with no arguments, and "no text" carries nothing that could be misread, so it means `{}`.
[[nodiscard]] inline ToolCallRequest tool_call_request_of(ToolCall const& call, std::uint64_t call_index) {
    ToolCallRequest req{call.call_id, call.tool_name, json::Value::make_object({}),
                        /*arguments_tainted=*/true, call_index, call.provenance};
    if (call.arguments_json.find_first_not_of(" \t\r\n") == std::string::npos) return req;
    auto parsed = json::parse(call.arguments_json);
    if (parsed && parsed->is_object()) {
        req.arguments = std::move(*parsed);
    } else if (parsed) {
        // Issue #112 B2 (ADR-197 §5): tool arguments are a JSON object on every wire this engine speaks. A number,
        // string, array, `null` or boolean is refused here, at the one choke point, rather than left to each tool's
        // codec -- a tool whose argument type accepts any value would otherwise run on it. This is also what makes an
        // adapter's non-string OpenAI `arguments` (re-serialized as its JSON text) a malformed call, not `{}`.
        static constexpr char const* kKind[] = {"null", "boolean", "number", "string", "array", "object"};
        req.arguments_parse_error =
            std::string("got a JSON ") + kKind[static_cast<std::size_t>(parsed->kind())] + ", not an object";
    } else {
        req.arguments_parse_error = parsed.error().message.empty() ? std::string{"parse failed"}
                                                                   : parsed.error().message;
    }
    return req;
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
