// Milestone 7 Phase E1 (013-UI-and-Streaming-Surfaces.md §2.1/§2.2, docs/research/2026-a2a-and-agui-
// detail.md Part B, docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves the AG-UI
// event vocabulary (protocol/agui/types.hpp) serializes with the exact wire identifiers/field names
// the cited research record specifies -- every alternative gets its own check, since this vocabulary
// IS the contract (013 §2.1: "Exact identifiers, since they are the contract").

#include <cstdio>
#include <string>

#include "agentengine/protocol/agui/types.hpp"

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

namespace agui = agentengine::agui;
namespace json = agentengine::json;

bool has(json::Value const& v, std::string const& key) { return v.find(key) != nullptr; }

}  // namespace

int main() {
    // --- Lifecycle ------------------------------------------------------------------------------------
    {
        agui::AgUiEvent e = agui::RunStarted{"thread-1", "run-1", std::nullopt};
        check(agui::event_type_name(e) == "RUN_STARTED", "RunStarted -> \"RUN_STARTED\"");
        json::Value j = agui::to_json(e);
        check(j.find("type")->as_string() == "RUN_STARTED" && j.find("threadId")->as_string() == "thread-1" &&
                  j.find("runId")->as_string() == "run-1" && !has(j, "parentRunId"),
              "RunStarted: type/threadId/runId present, unset parentRunId omitted");
    }
    {
        agui::AgUiEvent e = agui::RunFinishedSuccess{"thread-1", "run-1"};
        json::Value j = agui::to_json(e);
        check(j.find("type")->as_string() == "RUN_FINISHED", "RunFinishedSuccess -> \"RUN_FINISHED\"");
        auto const* outcome = j.find("outcome");
        check(outcome && outcome->find("type")->as_string() == "success",
              "RunFinishedSuccess: outcome.type == \"success\"");
    }
    {
        agui::Interrupt interrupt;
        interrupt.id     = "interaction-1";
        interrupt.reason = "input_required";
        agui::AgUiEvent e = agui::RunFinishedInterrupt{"thread-1", "run-1", {interrupt}};
        json::Value j = agui::to_json(e);
        check(j.find("type")->as_string() == "RUN_FINISHED",
              "RunFinishedInterrupt ALSO -> \"RUN_FINISHED\" -- same wire type, different outcome shape "
              "(013 §2.2: a run needing input ENDS, it does not pause)");
        auto const* outcome = j.find("outcome");
        check(outcome && outcome->find("type")->as_string() == "interrupt", "outcome.type == \"interrupt\"");
        auto const& interrupts = outcome->find("interrupts")->as_array();
        check(interrupts.size() == 1 && interrupts[0].find("id")->as_string() == "interaction-1" &&
                  interrupts[0].find("reason")->as_string() == "input_required",
              "the interrupt's id/reason survive -- id is the interaction_id, verbatim (013 §2.2)");
    }
    {
        agui::AgUiEvent e = agui::RunError{"something broke", std::nullopt};
        json::Value j = agui::to_json(e);
        check(agui::event_type_name(e) == "RUN_ERROR" && j.find("message")->as_string() == "something broke",
              "RunError -> \"RUN_ERROR\", the SOLE error event (013 §2.1)");
    }
    {
        agui::AgUiEvent started  = agui::StepStarted{"turn-0"};
        agui::AgUiEvent finished = agui::StepFinished{"turn-0"};
        check(agui::event_type_name(started) == "STEP_STARTED" &&
                  agui::event_type_name(finished) == "STEP_FINISHED",
              "StepStarted/StepFinished map to their exact identifiers");
    }

    // --- Text -----------------------------------------------------------------------------------------
    {
        agui::AgUiEvent start   = agui::TextMessageStart{"m-1", "assistant", std::nullopt};
        agui::AgUiEvent content = agui::TextMessageContent{"m-1", "hello"};
        agui::AgUiEvent end     = agui::TextMessageEnd{"m-1"};
        check(agui::event_type_name(start) == "TEXT_MESSAGE_START" &&
                  agui::event_type_name(content) == "TEXT_MESSAGE_CONTENT" &&
                  agui::event_type_name(end) == "TEXT_MESSAGE_END",
              "TextMessage START/CONTENT/END map to their exact identifiers");
        json::Value j = agui::to_json(content);
        check(j.find("messageId")->as_string() == "m-1" && j.find("delta")->as_string() == "hello",
              "TextMessageContent carries messageId/delta");
    }

    // --- Tool call --------------------------------------------------------------------------------------
    {
        agui::AgUiEvent start = agui::ToolCallStart{"tc-1", "search", std::nullopt};
        json::Value j = agui::to_json(start);
        check(agui::event_type_name(start) == "TOOL_CALL_START" &&
                  j.find("toolCallId")->as_string() == "tc-1" && j.find("toolCallName")->as_string() == "search",
              "ToolCallStart -> \"TOOL_CALL_START\" with toolCallId/toolCallName");
        agui::AgUiEvent result = agui::ToolCallResult{"m-2", "tc-1", json::Value::make_string("42"), std::nullopt};
        check(agui::event_type_name(result) == "TOOL_CALL_RESULT", "ToolCallResult -> \"TOOL_CALL_RESULT\"");
    }

    // --- State ------------------------------------------------------------------------------------------
    {
        agui::AgUiEvent snap =
            agui::StateSnapshot{json::Value::make_object({{"k", json::Value::make_string("v")}})};
        agui::AgUiEvent delta = agui::StateDelta{json::Value::make_array(
            {json::Value::make_object({{"op", json::Value::make_string("replace")},
                                        {"path", json::Value::make_string("/k")},
                                        {"value", json::Value::make_string("v2")}})})};
        check(agui::event_type_name(snap) == "STATE_SNAPSHOT" && agui::event_type_name(delta) == "STATE_DELTA",
              "StateSnapshot/StateDelta map to their exact identifiers");
        json::Value jd = agui::to_json(delta);
        check(jd.find("delta")->is_array(), "StateDelta carries its RFC 6902 patch array under \"delta\"");
        agui::AgUiEvent msgs = agui::MessagesSnapshot{json::Value::make_array({})};
        check(agui::event_type_name(msgs) == "MESSAGES_SNAPSHOT", "MessagesSnapshot -> \"MESSAGES_SNAPSHOT\"");
    }

    // --- Activity ---------------------------------------------------------------------------------------
    {
        agui::AgUiEvent snap = agui::ActivitySnapshot{"m-3", "sandbox_exec", json::Value::make_object({}),
                                                        std::nullopt};
        agui::AgUiEvent delta = agui::ActivityDelta{"m-3", "sandbox_exec", json::Value::make_array({})};
        check(agui::event_type_name(snap) == "ACTIVITY_SNAPSHOT" &&
                  agui::event_type_name(delta) == "ACTIVITY_DELTA",
              "ActivitySnapshot/ActivityDelta map to their exact identifiers");
    }

    // --- Reasoning --------------------------------------------------------------------------------------
    {
        check(agui::event_type_name(agui::AgUiEvent{agui::ReasoningStart{}}) == "REASONING_START" &&
                  agui::event_type_name(agui::AgUiEvent{agui::ReasoningEnd{}}) == "REASONING_END",
              "ReasoningStart/ReasoningEnd map to their exact identifiers");
        agui::AgUiEvent content = agui::ReasoningMessageContent{"m-4", "thinking..."};
        json::Value j = agui::to_json(content);
        check(agui::event_type_name(content) == "REASONING_MESSAGE_CONTENT" &&
                  j.find("delta")->as_string() == "thinking...",
              "ReasoningMessageContent carries its delta");
        agui::AgUiEvent enc = agui::ReasoningEncryptedValue{"message", "e-1", "opaque-blob"};
        json::Value je = agui::to_json(enc);
        check(agui::event_type_name(enc) == "REASONING_ENCRYPTED_VALUE" &&
                  je.find("encryptedValue")->as_string() == "opaque-blob",
              "ReasoningEncryptedValue's blob passes through verbatim, unexamined (003 §1)");
    }

    // --- Special ----------------------------------------------------------------------------------------
    {
        agui::AgUiEvent raw = agui::RawEvent{json::Value::make_object({}), std::nullopt};
        agui::AgUiEvent custom = agui::CustomEvent{"ae:state_changed", json::Value::make_string("desc")};
        check(agui::event_type_name(raw) == "RAW" && agui::event_type_name(custom) == "CUSTOM",
              "RawEvent/CustomEvent map to their exact identifiers");
        json::Value jc = agui::to_json(custom);
        check(jc.find("name")->as_string() == "ae:state_changed",
              "CustomEvent's namespaced name survives -- the escape hatch 013 §2.1 requires "
              "(\"never silently dropped\")");
    }

    if (g_failures == 0) {
        std::printf("test_agui_types: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agui_types: %d failure(s)\n", g_failures);
    return 1;
}
