#pragma once
// Implements 013-UI-and-Streaming-Surfaces.md §3's own table row ("MCP progress (011) |
// notifications/progress on the originating request's response stream, when serving a tool call --
// sourced from ToolCallDelta (006 §6a) exactly like the AG-UI projection above") and 011 §1's request-
// scoped notification split ("notifications/progress... continue to flow on the response stream of
// the request they belong to"). Milestone 7 Phase E4
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Deliberately narrow, matching what 013 §3 itself scopes MCP progress to: ONLY `tool_call_delta`
// projects to a progress notification. Every other `RunEvent` kind has no MCP progress wire slot at
// all (MCP's own notification vocabulary has no analog for a model text delta, a turn boundary, or a
// state change) -- unlike `RunEventProjector` (E2)/`A2aStreamProjector` (E4), this is not a
// `switch`-over-every-kind projector, because the RFC's own table names exactly one source, not a
// vocabulary to map exhaustively.
//
// Field shape: `progressToken`/`progress` are cited directly (docs/research/2026-mcp-protocol-detail.md
// §10: "progressToken MUST be unique across active requests; the progress value MUST increase with
// each notification"). `message` is this project's own reasonable choice to carry `ToolCallDelta`'s
// own content (MCP's wider notification ecosystem commonly carries a human-readable message
// alongside progress/total, but that specific field is not independently confirmed in this project's
// own dated citation) -- included because dropping the only real content `ToolCallDelta` carries would
// make the projection pointless, named here as a documented choice rather than asserted as spec text.
// `total` is NOT built: nothing in `ToolCallDelta`'s payload (`core/run_event.hpp`) carries a total,
// so there is no real value to put there.
//
// unified-streaming-design-draft.md §5 (Piece E), Finding 11: `ToolCallDelta::content` is now the
// engine's whole `ContentItem` vocabulary, not a plain string -- MCP's real wire shape genuinely caps
// `message` to text (no structured-content slot exists to widen into), so this collapses structured
// content down to a short line per variant rather than reusing `rt::content_item_to_json()` wholesale
// (that codec already double-quotes `Custom::payload_json` and adds irrelevant origin/tainted
// bookkeeping no MCP client needs -- only `Text`/`Data`/`Custom` get a tight, semantically relevant
// rendering; everything else gets a short, variant-specific human-readable line).

#include <optional>
#include <string>
#include <unordered_map>

#include "agentengine/core/run_event.hpp"
#include "agentengine/protocol/mcp/json_rpc.hpp"

namespace agentengine::mcp {

struct ProgressNotification {
    std::string progress_token;
    double      progress;
    std::string message;
};

[[nodiscard]] inline JsonRpcNotification to_json_rpc(ProgressNotification const& p) {
    return JsonRpcNotification{
        "notifications/progress",
        json::Value::make_object({{"progressToken", json::Value::make_string(p.progress_token)},
                                   {"progress", json::Value::make_number(p.progress)},
                                   {"message", json::Value::make_string(p.message)}})};
}

// Tracks the monotonically-increasing `progress` value per `progressToken` (the spec's own MUST,
// cited above) -- one call_id's own progress counter is independent of every other's, matching
// `progressToken`'s own "MUST be unique across active requests" scoping.
class McpProgressProjector {
public:
    // Returns `std::nullopt` for every `RunEvent` kind other than `tool_call_delta` -- see file-top
    // comment for why this is not a total, every-kind mapping.
    [[nodiscard]] std::optional<ProgressNotification> project(RunEvent const& ev) {
        if (ev.kind != run_event_kind::tool_call_delta) return std::nullopt;
        auto const& p = std::get<run_event_payload::ToolCallDelta>(ev.payload);
        double& counter = progress_by_token_[p.call_id];
        counter += 1.0;  // a real increasing sequence -- ToolCallDelta carries no numeric progress
                          // itself, only text, so "one more delta happened" is what actually increased
        return ProgressNotification{p.call_id, counter, render_message(p.content)};
    }

private:
    // See file-top comment (Finding 11): a tight, per-variant text rendering, not a wholesale codec
    // dump.
    [[nodiscard]] static std::string render_message(ContentItem const& item) {
        if (auto const* text = std::get_if<Text>(&item.value)) {
            return text->text;
        }
        if (auto const* custom = std::get_if<Custom>(&item.value)) {
            return custom->type_id + ": " + custom->payload_json;
        }
        if (auto const* data = std::get_if<Data>(&item.value)) {
            return data->json;
        }
        if (std::holds_alternative<Media>(item.value)) return "[media content]";
        if (std::holds_alternative<ToolCall>(item.value)) return "[tool call content]";
        if (auto const* tool_result = std::get_if<ToolResult>(&item.value)) {
            return tool_result->is_error ? "[tool result: error]" : "[tool result]";
        }
        if (std::holds_alternative<Citation>(item.value)) return "[citation]";
        if (auto const* err = std::get_if<Error>(&item.value)) return "[error] " + err->message;
        if (std::holds_alternative<Reasoning>(item.value)) return "[reasoning]";
        return "[content]";
    }

    std::unordered_map<std::string, double> progress_by_token_;
};

}  // namespace agentengine::mcp
