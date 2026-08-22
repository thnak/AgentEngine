// Milestone 7 Phase E4 (013-UI-and-Streaming-Surfaces.md §3, docs/research/2026-mcp-protocol-detail.md
// §10, docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves `McpProgressProjector`
// (protocol/mcp/progress.hpp) projects `tool_call_delta` -- and ONLY `tool_call_delta` -- into a
// `notifications/progress` JSON-RPC notification with a per-token monotonically-increasing `progress`
// value (the spec's own cited MUST).

#include <cstdio>
#include <string>

#include "agentengine/protocol/mcp/progress.hpp"

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

namespace mcp  = agentengine::mcp;
namespace json = agentengine::json;

}  // namespace

int main() {
    mcp::McpProgressProjector projector;

    // --- P-1: tool_call_delta -> a real progress notification ---------------------------------------
    {
        auto out = projector.project(
            ae::RunEvent{"run-1", 1, ae::run_event_kind::tool_call_delta,
                         ae::run_event_payload::ToolCallDelta{
                             "call-1", ae::ContentItem{ae::Text{"25% done"}}}});
        check(out.has_value(), "P-1: tool_call_delta produces a ProgressNotification");
        if (out.has_value()) {
            check(out->progress_token == "call-1" && out->message == "25% done" && out->progress == 1.0,
                  "P-1: progressToken is the real call_id, message carries the real progress text, "
                  "progress starts at 1");
        }
    }

    // --- P-2: progress MUST increase with each notification for the SAME token ----------------------
    {
        auto second = projector.project(
            ae::RunEvent{"run-1", 2, ae::run_event_kind::tool_call_delta,
                         ae::run_event_payload::ToolCallDelta{
                             "call-1", ae::ContentItem{ae::Text{"60% done"}}}});
        auto third = projector.project(
            ae::RunEvent{"run-1", 3, ae::run_event_kind::tool_call_delta,
                         ae::run_event_payload::ToolCallDelta{
                             "call-1", ae::ContentItem{ae::Text{"90% done"}}}});
        check(second.has_value() && third.has_value(), "P-2: subsequent deltas for the same token succeed");
        if (second.has_value() && third.has_value()) {
            check(second->progress == 2.0 && third->progress == 3.0 && third->progress > second->progress,
                  "P-2: progress strictly increases for the same progressToken, per the spec's own "
                  "cited MUST (docs/research/2026-mcp-protocol-detail.md §10)");
        }
    }

    // --- P-3: a DIFFERENT token gets its OWN independent counter, starting fresh at 1 ----------------
    {
        auto other = projector.project(
            ae::RunEvent{"run-2", 1, ae::run_event_kind::tool_call_delta,
                         ae::run_event_payload::ToolCallDelta{
                             "call-2", ae::ContentItem{ae::Text{"starting"}}}});
        check(other.has_value() && other->progress == 1.0,
              "P-3: a different progressToken (call-2) starts its own counter at 1, independent of "
              "call-1's already-advanced counter -- \"unique across active requests\" per the spec");
    }

    // --- P-4: every OTHER RunEvent kind produces NO progress notification -- narrow by design, ------
    // --- matching 013 §3's own scoping of MCP progress to ToolCallDelta specifically.               ---
    {
        auto turn = projector.project(
            ae::RunEvent{"run-1", 4, ae::run_event_kind::turn_started, ae::run_event_payload::Turn{0}});
        auto delta = projector.project(
            ae::RunEvent{"run-1", 5, ae::run_event_kind::model_delta,
                         ae::run_event_payload::ModelDelta{
                             ae::run_event_payload::ModelTextDelta{"text"}}});
        auto started = projector.project(
            ae::RunEvent{"run-1", 6, ae::run_event_kind::tool_call_started,
                         ae::run_event_payload::ToolCallStarted{"call-3", "search"}});
        check(!turn.has_value() && !delta.has_value() && !started.has_value(),
              "P-4: turn/model-delta/tool-call-started events produce no progress notification -- "
              "not a fabricated one, an honest nullopt");
    }

    // --- P-5: the JSON-RPC wire wrapping is real ------------------------------------------------------
    {
        mcp::ProgressNotification p{"call-9", 3.0, "almost there"};
        mcp::JsonRpcNotification n = mcp::to_json_rpc(p);
        check(n.method == "notifications/progress", "P-5: the method is exactly \"notifications/progress\"");
        json::Value j = n.params;
        check(j.find("progressToken")->as_string() == "call-9" &&
                  j.find("progress")->as_number() == 3.0 && j.find("message")->as_string() == "almost there",
              "P-5: the notification params carry progressToken/progress/message correctly");
    }

    if (g_failures == 0) {
        std::printf("test_mcp_progress: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_progress: %d failure(s)\n", g_failures);
    return 1;
}
