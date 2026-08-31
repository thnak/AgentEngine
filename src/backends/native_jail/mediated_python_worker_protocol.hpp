#pragma once
// The jailed-Python-worker wire message catalog (008-Sandbox-and-Isolation.md §1b/§3's native-jail
// layer-3 backstop, wired for real per the ADR superseding native_jail_backend.hpp's former
// "Correction (2026-08-23)" comment; 010-Python-Code-Interpreter.md §2/§6). Every message is a
// `json::Value` object carrying a `"type"` string tag; this header is the single source of truth for
// those tags and for the handful of fields both peers (native_jail_backend.cpp on the host side,
// python_worker_main.cpp/python_worker_mediation.cpp on the worker side) need to agree on by NAME.
//
// SCOPE, stated plainly: Slice 1 built the envelope and the `call_tool` kind only. Slice 2
// (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md,
// decisions/ADR-085-jailed-python-worker-slice-2-handle-relay.md) adds real host-side HANDLING for
// "open"/"listdir"/"connect_authorize"/"connect_send"/"connect_recv"/"connect_close" --
// native_jail_backend.cpp's dispatch_worker_query -- not a wire SHAPE change here: the envelope
// (call_id/exec_seq/kind/payload) already covered them from Slice 1 onward. This header still does
// not pre-declare typed payload structs for those five kinds -- every field they need is expressible
// as a generic `json::Value` payload object, and dispatch_worker_query/python_worker_mediation.cpp
// build/read them directly by key, matching kQueryCallTool's own existing shape.
//
// exec_seq (RT1 Finding 1's fix, both directions): a worker_query's exec_seq MUST equal the exec_seq
// of the exec_request currently in flight -- the host enforces this (native_jail_backend.cpp's
// exec_session() dispatch loop), never trusts it from the wire alone. Its presence here is the wire
// CONTRACT; the enforcement lives at the host's dispatch point, per the design's own §8a.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/json_value.hpp"

namespace agentengine::native_jail::worker_protocol {

// ---- type tags --------------------------------------------------------------------------------
inline constexpr char const* kInitRequest = "init_request";
inline constexpr char const* kInitResponse = "init_response";
inline constexpr char const* kExecRequest = "exec_request";
inline constexpr char const* kExecResponse = "exec_response";
inline constexpr char const* kWorkerQuery = "worker_query";
inline constexpr char const* kWorkerQueryResponse = "worker_query_response";
inline constexpr char const* kRefreshToolsRequest = "refresh_tools_request";
inline constexpr char const* kRefreshToolsResponse = "refresh_tools_response";
inline constexpr char const* kShutdown = "shutdown";

// ---- worker_query kinds -------------------------------------------------------------------------
inline constexpr char const* kQueryCallTool = "call_tool";
inline constexpr char const* kQueryOpen = "open";
inline constexpr char const* kQueryListdir = "listdir";
inline constexpr char const* kQueryConnectAuthorize = "connect_authorize";
inline constexpr char const* kQueryConnectSend = "connect_send";
inline constexpr char const* kQueryConnectRecv = "connect_recv";
inline constexpr char const* kQueryConnectClose = "connect_close";
// HandleRelay design draft §1, revised during implementation: `open` returns an opaque `file_id`
// (never a raw HANDLE -- DuplicateHandle into the AppContainer'd worker is real but I/O against it
// fails with ERROR_INVALID_HANDLE, a reproduced Windows AppContainer limitation, not a design choice)
// -- these three kinds relay the actual I/O, the same shape connect_send/connect_recv/connect_close
// already use for sockets.
inline constexpr char const* kQueryFileRead = "file_read";
inline constexpr char const* kQueryFileWrite = "file_write";
inline constexpr char const* kQueryFileClose = "file_close";
// decisions/ADR-155-agent-progress-codeact-module.md, 026 §5's `agent.progress`. Deliberately reuses
// the EXISTING worker_query/worker_query_response request-response envelope (kWorkerQuery/
// kWorkerQueryResponse above) rather than a genuinely new frame TYPE the way GitHub issue #31's own
// first sketch proposed: RT1 Finding 1's exec_seq-mismatch fail-closed check (native_jail_backend.cpp's
// exec_session()) already applies uniformly to every worker_query KIND, so adding one more kind here
// gets that same protection for free, with no new branch needed in exec_session()'s own frame-type
// dispatch loop at all -- the identical "small, boring, reuse the pipe already proven safe" bar 026 §5
// itself sets for the Python surface, applied to the transport layer too.
inline constexpr char const* kQueryProgress = "progress";

// Returned by the host's dispatch handler for a `call_tool` query when no tool bridge is configured
// for this session at all (native_jail_backend.cpp's dispatch_worker_query) -- named once here so the
// worker-side translation to a Python exception (python_worker_mediation.cpp's raise_mapped_denial)
// matches on the SAME string the host emits, not a re-typed copy that could drift. Slice 1 also used
// this as a fixed deny for every open/listdir/connect_* kind; Slice 2 gives those kinds real handling
// (see this header's own SCOPE note above), so this code's live use is now `call_tool`-only.
inline constexpr char const* kErrorNotImplementedThisSlice = "not_implemented_this_slice";
// RT1 Finding 1 / §8a: a protocol violation observed by the host (stale/mismatched exec_seq, an
// unparseable frame, a frame of the wrong type for the current dispatch state) -- the worker is
// terminated, never trusted to self-correct.
inline constexpr char const* kErrorProtocolViolation = "protocol_violation";
// Slice 2 (HandleRelay design draft §2/§4): a `connect_send`/`connect_recv`/`connect_close` naming a
// `socket_id` this worker's own `live_sockets` map does not (or no longer) hold -- maps to `OSError`,
// the ordinary Python exception for "you used an already-closed socket".
inline constexpr char const* kErrorNetSocketClosed = "net.socket_closed";
// Slice 2 (HandleRelay design draft §4 item 5): `connect_authorize` refused because this worker's
// session already holds kMaxLiveSockets live relayed sockets -- maps to OSError ("Too many open
// files"), the same errno CPython itself would raise for the analogous real-socket exhaustion case.
inline constexpr char const* kErrorNetTooManySockets = "net.too_many_sockets";
// Reused from the tool-bridged egress path (sandbox/net_egress_proxy.hpp's own error codes) for the
// raw-socket relay's own denial/failure cases (HandleRelay design draft §2 item 1) -- named here so
// both call sites (native_jail_backend.cpp's dispatch_worker_query, this project's tool-bridged
// egress code) are visibly using the SAME vocabulary, not two independently-typed near-duplicates.
inline constexpr char const* kErrorNetAddressBlocked = "net.address_blocked";
inline constexpr char const* kErrorNetHostUnresolvable = "net.host_unresolvable";
// Slice 2 (HandleRelay design draft §1): a host-side file-op failure with no more specific mapped
// code -- `native_code` (see `get_native_code` below), when nonzero, takes priority over this string
// in `raise_mapped_denial` (python_worker_mediation.cpp), matching the pre-worker-process design's own
// `raise_os_error` ("a real, win32-code-sourced exception, never a hand-authored approximation").
// `native_code == 0` with this error_code is the one synthetic, policy-decided case that has no win32
// code at all (026 §3's "Quota exhausted" row, `OSError("No space left on device")`).
inline constexpr char const* kErrorPythonOpenOsError = "python.open_os_error";

// ---- small typed accessors (avoid repeating json::Value::find/as_* boilerplate at every call site) --

[[nodiscard]] inline std::string get_string(json::Value const& obj, char const* key,
                                             std::string fallback = {}) {
    json::Value const* v = obj.find(key);
    return (v && v->is_string()) ? v->as_string() : fallback;
}

[[nodiscard]] inline double get_number(json::Value const& obj, char const* key, double fallback = 0.0) {
    json::Value const* v = obj.find(key);
    return (v && v->is_number()) ? v->as_number() : fallback;
}

// `worker_query_response`'s optional Win32 `GetLastError()` value, when a host-side file operation
// failed with a real OS error -- 0 means "no native code, use error_code's own mapping instead".
[[nodiscard]] inline int get_native_code(json::Value const& obj) {
    return static_cast<int>(get_number(obj, "native_code", 0.0));
}

[[nodiscard]] inline bool get_bool(json::Value const& obj, char const* key, bool fallback = false) {
    json::Value const* v = obj.find(key);
    return (v && v->is_bool()) ? v->as_bool() : fallback;
}

// exec_seq travels as a JSON number (json::Value has no separate integer kind, core/json_value.hpp's
// own documented "numbers are double only" scope) -- always a small monotonically increasing counter
// in practice, so double's 53-bit exact-integer range is not a real constraint.
[[nodiscard]] inline std::uint64_t get_exec_seq(json::Value const& obj) {
    return static_cast<std::uint64_t>(get_number(obj, "exec_seq", 0.0));
}

[[nodiscard]] inline std::vector<std::string> get_string_array(json::Value const& obj, char const* key) {
    std::vector<std::string> out;
    json::Value const* v = obj.find(key);
    if (!v || !v->is_array()) return out;
    for (json::Value const& item : v->as_array()) {
        if (item.is_string()) out.push_back(item.as_string());
    }
    return out;
}

[[nodiscard]] inline json::Value make_string_array(std::vector<std::string> const& items) {
    std::vector<json::Value> arr;
    arr.reserve(items.size());
    for (auto const& s : items) arr.push_back(json::Value::make_string(s));
    return json::Value::make_array(std::move(arr));
}

// {key: value} string map -> a JSON object -- used for ExecState::env in both directions.
[[nodiscard]] inline json::Value make_string_map(
        std::unordered_map<std::string, std::string> const& m) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.reserve(m.size());
    for (auto const& [k, v] : m) obj.emplace_back(k, json::Value::make_string(v));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> get_string_map(json::Value const& obj,
                                                                                  char const* key) {
    std::unordered_map<std::string, std::string> out;
    json::Value const* v = obj.find(key);
    if (!v || !v->is_object()) return out;
    for (auto const& [k, val] : v->as_object()) {
        if (val.is_string()) out.emplace(k, val.as_string());
    }
    return out;
}

}  // namespace agentengine::native_jail::worker_protocol
