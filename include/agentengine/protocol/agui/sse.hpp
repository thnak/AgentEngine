#pragma once
// Implements 013-UI-and-Streaming-Surfaces.md §4's Server-Sent Events framing for AG-UI
// (docs/research/2026-a2a-and-agui-detail.md §B.3: "SSE (default): text/event-stream, one
// `data: <JSON BaseEvent>` frame per event"). Milestone 7 Phase E3
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Transport-agnostic, matching every prior Phase C/D/E component's own scoping: this is the FRAMING
// logic only -- given an `AgUiEvent`, produce the exact bytes an SSE response body carries for it.
// There is no HTTP listener anywhere in this codebase yet (ADR-021/ADR-022 both explicitly defer that
// -- ADR-022 in particular decided the future listener's own I/O model without building it), so
// nothing here opens a socket or sets a `Content-Type` header; a future transport sub-phase writes
// these bytes onto whatever connection it owns.
//
// Binary protobuf framing (§4's other named encoding -- a 4-byte big-endian length prefix followed
// by a protobuf-encoded event, negotiated by `Accept: application/vnd.ag-ui.event+proto`) is NOT
// built: this project has no protobuf library vendored anywhere (CONVENTIONS' dependency-tier
// discipline -- core is std+Quark only, a seam backend may take ONE heavy dependency behind a CMake
// option, the same precedent mbedTLS/wasmtime/CPython already follow) -- adopting protobuf is a real,
// separate dependency decision this phase does not make as a drive-by. Named here, not silently
// dropped; SSE alone is a complete, spec-compliant default surface (§4: "widest client support, proxy
// friendly, one-way is enough").

#include <string>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/agui/types.hpp"

namespace agentengine::agui {

// §4/§B.3's exact framing: one `data: <JSON event>` line, terminated by a blank line (the SSE
// spec's own event terminator). Safe as a SINGLE `data:` line because `json::dump()`
// (core/json_value.hpp) escapes every embedded newline inside a JSON string -- the produced text
// never itself contains a literal `\n`, so there is nothing here that needs SSE's own multi-line
// `data:` continuation form.
[[nodiscard]] inline std::string to_sse_frame(AgUiEvent const& event) {
    return "data: " + json::dump(to_json(event)) + "\n\n";
}

// Convenience for a caller streaming a whole batch at once (e.g. a test, or a future transport
// writing everything currently buffered in one flush) -- exactly the concatenation of `to_sse_frame`
// over each event, in order. §1: "ordered and monotonic per run" -- this function preserves whatever
// order `events` is already in, it does not itself sort or deduplicate.
[[nodiscard]] inline std::string to_sse_stream(std::vector<AgUiEvent> const& events) {
    std::string out;
    for (AgUiEvent const& event : events) out += to_sse_frame(event);
    return out;
}

}  // namespace agentengine::agui
