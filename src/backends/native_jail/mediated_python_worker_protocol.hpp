#pragma once
// The jailed-Python-worker wire message catalog (008-Sandbox-and-Isolation.md §1b/§3's native-jail
// layer-3 backstop, wired for real per the ADR superseding native_jail_backend.hpp's former
// "Correction (2026-08-23)" comment; 010-Python-Code-Interpreter.md §2/§6). Every message is a
// `json::Value` object carrying a `"type"` string tag; this header is the single source of truth for
// those tags and for the handful of fields both peers (native_jail_backend.cpp on the host side,
// python_worker_main.cpp/python_worker_mediation.cpp on the worker side) need to agree on by NAME.
//
// SCOPE, stated plainly: this is Slice 1's catalog. `worker_query`/`worker_query_response`'s `kind`
// field already accepts "open"/"listdir"/"connect_authorize"/"connect_send"/"connect_recv"/
// "connect_close" as ordinary strings -- Slice 2 (file-open/listdir/socket relay, deferred whole, see
// native_jail_backend.cpp's dispatch_worker_query) adds host-side HANDLING for those kinds, not a wire
// SHAPE change here: the envelope (call_id/exec_seq/kind/payload) already covers them. This header
// does not pre-declare typed payload structs for those five kinds (a reduction from the reviewed
// design's own plan, made deliberately: nothing in Slice 1 constructs or consumes them, and every
// field they would need is already expressible as a generic `json::Value` payload object) -- named
// here as a stated scope reduction, not a silent one.
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
inline constexpr char const* kQueryOpen = "open";                        // Slice 2
inline constexpr char const* kQueryListdir = "listdir";                  // Slice 2
inline constexpr char const* kQueryConnectAuthorize = "connect_authorize";  // Slice 2
inline constexpr char const* kQueryConnectSend = "connect_send";         // Slice 2
inline constexpr char const* kQueryConnectRecv = "connect_recv";         // Slice 2
inline constexpr char const* kQueryConnectClose = "connect_close";       // Slice 2

// Slice 2's fixed deny, returned by the host's dispatch handler for every not-yet-implemented `kind`
// (native_jail_backend.cpp's dispatch_worker_query) -- named once here so the worker-side translation
// from "denied" to the right Python exception (python_worker_mediation.cpp) matches on the SAME
// string the host emits, not a re-typed copy that could drift.
inline constexpr char const* kErrorNotImplementedThisSlice = "not_implemented_this_slice";
// RT1 Finding 1 / §8a: a protocol violation observed by the host (stale/mismatched exec_seq, an
// unparseable frame, a frame of the wrong type for the current dispatch state) -- the worker is
// terminated, never trusted to self-correct.
inline constexpr char const* kErrorProtocolViolation = "protocol_violation";

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
