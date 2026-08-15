#pragma once
// Implements 011-MCP-Conformance.md §3 -- consuming an MCP server's tools (client role). Milestone 7
// Phase C3 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Transport-agnostic, exactly like `McpServer` (server.hpp): this client is handed a "send a
// JsonRpcRequest, get a JsonRpcResponse back" callable (`RequestSender`), not a socket. A later
// transport sub-phase supplies a real Streamable-HTTP/stdio-backed sender; this phase's own test
// wires one directly into `McpServer::dispatch()`, the same "same process, two roles" shape this
// codebase's own server-role tests already established.
//
// Scope: `tools/list` with caching (§3.1's own rule -- "Cache key = method + the parameters that
// affect the result," `ttlMs` honoured, "an empty string is a valid cursor, not end-of-results") and
// `tools/call` (`isError` SURFACED to the caller, never thrown/treated as a transport failure --
// §3.1: "surfaced to the model for self-correction"). Digest-pinning (§8's rug-pull defense): a
// change to a tool's description/schema between two `tools/list` calls under the same cache key is
// DETECTED, never silently re-trusted.
//
// NOT built here (named, not claimed): the generic JSON-Schema-2020-12 validator §3.1 asks for
// against a THIRD PARTY server's `outputSchema` (a separate, scoped follow-up -- this client reads a
// discovered tool's schema but does not validate arbitrary JSON against it); `cacheScope`'s
// cross-PRINCIPAL isolation proof (needs a real multi-principal transport context this phase does not
// build); real pagination across multiple actual server-side pages (`McpServer`, C2, returns every
// tool in one page -- this client's own cursor handling is proven at the CACHE-KEY level: a
// different cursor is a different cache entry, including an empty-string one, never collapsed into
// "no cursor").
//
// Milestone 7 Phase C4 (011 §3.6/§12, docs/research/2026-mcp-protocol-detail.md §12): the client-side
// half of the `io.modelcontextprotocol/tasks` extension -- `call_tool_as_task()` opts a `tools/call`
// into the extension (server.hpp's own `kMcpTasksExtension` string, duplicated here rather than a
// shared header both files would need only for one literal), `get_task()` polls `tasks/get`,
// `cancel_task()` calls `tasks/cancel`. `tasks/update`/`notifications/tasks` are NOT built, mirroring
// server.hpp's own C4 scope note exactly (this client has no counterpart for either).

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/mcp/json_rpc.hpp"

namespace agentengine::mcp {

using RequestSender = std::function<JsonRpcResponse(JsonRpcRequest const&)>;

struct McpToolInfo {
    std::string name;
    std::string description;
    json::Value input_schema;
    json::Value output_schema;
};

struct McpToolCallOutcome {
    bool        is_error = false;
    json::Value content;  // the raw "content" array, §3.1's own shape
    // 011 §3.4 / SEP-2322 (MRTR): true when the server answered `resultType: "input_required"` and
    // this client did not retry -- either because no `InputRequestHandler` was set, or because the
    // handler declined. Surfaced rather than swallowed: the spec is explicit that "servers MUST NOT
    // assume the client will fulfil or retry", so not-retrying is a legitimate outcome a caller has
    // to be able to see, not an error.
    bool        input_required = false;
    json::Value input_requests;  // the raw `inputRequests` object, valid iff `input_required`
};

// 011 §3.4: "an MRTR input request is a server asking our host for something." Answering it means
// producing elicitation/sampling/roots data, which is a HOST decision -- a peer server must not be
// able to obtain data or authority by asking, and the engine must never invent user input on the
// host's behalf (I3). So this is injected, never defaulted to something that answers: with no
// handler set, an `input_required` result is returned to the caller unretried.
//
// Returns the `InputResponse` value for one server-assigned key, or an error to decline it.
using InputRequestHandler =
    std::function<result<json::Value>(std::string const& request_key, json::Value const& request)>;

// Phase C4: mirrors `server.hpp`'s own `kMcpTasksExtension` -- kept as a client-local literal rather
// than a shared constant, since server.hpp is server-role-only and this file has no dependency on it.
inline constexpr std::string_view kMcpTasksExtensionClient = "io.modelcontextprotocol/tasks";

struct McpTaskHandle {
    std::string task_id;
    std::string status;  // "working" at creation, per §12
};

struct McpTaskPoll {
    std::string         status;
    bool                has_result = false;  // true once the server attached a real outcome
    McpToolCallOutcome  outcome;              // valid iff has_result
};

namespace client_detail {

// FNV-1a over a tool listing's own identity-bearing fields -- the same deterministic-hash idiom
// `tool_pipeline.hpp`'s own `argument_digest()` already uses (019 §3), not a cryptographic
// commitment: §8 names no collision-resistance requirement, only change-DETECTION.
[[nodiscard]] inline std::string digest_of(std::vector<McpToolInfo> const& tools) {
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    auto mix = [&h](std::string_view s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x0000'0100'0000'01B3ULL;
        }
    };
    for (auto const& t : tools) {
        mix(t.name);
        mix(t.description);
        mix(json::dump(t.input_schema));
        mix(json::dump(t.output_schema));
    }
    return std::to_string(h);
}

struct CacheEntry {
    std::vector<McpToolInfo>              tools;
    std::chrono::steady_clock::time_point cached_at;
    std::string                           digest;
};

// ---- SEP-2243 `x-mcp-header`: custom headers from tool parameters ------------------------------
// 011 §8b calls this "a mandatory client-side surface on Streamable HTTP, and it is easy to miss."
// Spec: /specification/draft/basic/transports/streamable-http#custom-headers-from-tool-parameters,
// fetched 2026-08-15 (docs/research/2026-08-15-mcp-conformance-harness.md). Two obligations, and the
// codebase had neither:
//   1. VALIDATE every annotation; a violation means EXCLUDING that tool from `tools/list`'s result,
//      not warning -- so one malformed tool cannot deny the caller every other tool.
//   2. MIRROR designated argument values into `Mcp-Param-{Name}` headers on `tools/call`.

// RFC 9110 §5.1 `tchar`.
[[nodiscard]] inline bool is_http_tchar(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline std::string base64_encode(std::string_view in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        std::uint32_t const n = (static_cast<unsigned char>(in[i]) << 16) |
                                 (static_cast<unsigned char>(in[i + 1]) << 8) |
                                 static_cast<unsigned char>(in[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
    }
    if (i + 1 == in.size()) {
        std::uint32_t const n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        std::uint32_t const n = (static_cast<unsigned char>(in[i]) << 16) |
                                 (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline constexpr std::string_view kBase64SentinelPrefix = "=?base64?";
inline constexpr std::string_view kBase64SentinelSuffix = "?=";

// "HTTP header field values must consist of visible ASCII characters (0x21-0x7E), space (0x20), and
// horizontal tab (0x09). When a value cannot be safely represented as a plain ASCII header value
// (e.g., it contains non-ASCII characters, control characters, or has leading/trailing whitespace),
// clients MUST use Base64 encoding" -- plus the ambiguity rule: a plain value that itself matches
// the sentinel pattern MUST also be encoded.
[[nodiscard]] inline bool needs_base64(std::string_view v) noexcept {
    if (v.empty()) return false;
    if (v.front() == ' ' || v.front() == '\t' || v.back() == ' ' || v.back() == '\t') return true;
    for (char c : v) {
        auto const u = static_cast<unsigned char>(c);
        if (u == 0x20 || u == 0x09) continue;
        if (u < 0x21 || u > 0x7E) return true;
    }
    if (v.size() >= kBase64SentinelPrefix.size() + kBase64SentinelSuffix.size() &&
        v.substr(0, kBase64SentinelPrefix.size()) == kBase64SentinelPrefix &&
        v.substr(v.size() - kBase64SentinelSuffix.size()) == kBase64SentinelSuffix) {
        return true;
    }
    return false;
}

[[nodiscard]] inline std::string encode_header_value(std::string_view raw) {
    if (!needs_base64(raw)) return std::string(raw);
    return std::string(kBase64SentinelPrefix) + base64_encode(raw) +
           std::string(kBase64SentinelSuffix);
}

struct HeaderAnnotation {
    std::vector<std::string> path;         // the exact chain of `properties` keys
    std::string              header_name;  // the `x-mcp-header` value
    std::string              type;         // declared JSON type of the annotated property
    bool                     reachable = true;  // false if reached through a forbidden keyword
};

// Walks the WHOLE schema, not just the statically reachable part: an annotation sitting under
// `items`/`oneOf`/`$ref`/etc. does not merely get ignored, it invalidates the tool, so it must be
// found in order to be rejected.
inline void collect_header_annotations(json::Value const& node, std::vector<std::string>& path,
                                        bool reachable, std::vector<HeaderAnnotation>& out) {
    if (!node.is_object()) return;

    if (json::Value const* h = node.find("x-mcp-header")) {
        HeaderAnnotation a;
        a.path      = path;
        a.reachable = reachable;
        a.header_name = h->is_string() ? h->as_string() : std::string{};
        if (json::Value const* t = node.find("type"); t && t->is_string()) a.type = t->as_string();
        // A non-string annotation is a violation too; recorded with an empty name so the
        // not-empty/charset checks reject it rather than it slipping through unseen.
        out.push_back(std::move(a));
    }

    for (auto const& [key, child] : node.as_object()) {
        if (key == "properties" && child.is_object()) {
            for (auto const& [prop_name, prop_schema] : child.as_object()) {
                path.push_back(prop_name);
                collect_header_annotations(prop_schema, path, reachable, out);
                path.pop_back();
            }
            continue;
        }
        // Every other keyword breaks static reachability: `items` and other array keywords,
        // `oneOf`/`anyOf`/`allOf`/`not`, `if`/`then`/`else`, `$ref`. Descend anyway -- with
        // `reachable=false` -- precisely so an annotation hiding there is found and rejected.
        if (child.is_object()) {
            collect_header_annotations(child, path, false, out);
        } else if (child.is_array()) {
            for (json::Value const& item : child.as_array()) {
                collect_header_annotations(item, path, false, out);
            }
        }
    }
}

// Returns an empty string if the schema's annotations are all valid, else the reason the tool must
// be excluded from `tools/list` (SHOULD-logged by the caller, per the spec's own guidance).
[[nodiscard]] inline std::string x_mcp_header_violation(json::Value const& input_schema) {
    std::vector<HeaderAnnotation> found;
    std::vector<std::string>      path;
    collect_header_annotations(input_schema, path, /*reachable=*/true, found);

    std::vector<std::string> seen_lower;
    for (auto const& a : found) {
        if (!a.reachable) {
            return "x-mcp-header is not statically reachable through `properties` chains only";
        }
        if (a.header_name.empty()) return "x-mcp-header must not be empty";
        for (char c : a.header_name) {
            auto const u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F) return "x-mcp-header contains a control character";
            if (!is_http_tchar(c)) return "x-mcp-header is not valid HTTP token syntax";
        }
        // integer | string | boolean only -- `number` is explicitly NOT permitted.
        if (a.type != "integer" && a.type != "string" && a.type != "boolean") {
            return "x-mcp-header is only permitted on integer, string, or boolean properties";
        }
        std::string lower;
        lower.reserve(a.header_name.size());
        for (char c : a.header_name) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        for (auto const& prev : seen_lower) {
            if (prev == lower) return "x-mcp-header values must be case-insensitively unique";
        }
        seen_lower.push_back(std::move(lower));
    }
    return {};
}

// "Header extraction is defined as reading the instance value at the exact property path of the
// annotated property... If no value is present at that path in the call arguments, the header is
// omitted." Null is omitted for the same reason -- the spec's own table: "Parameter value is null ->
// Client MUST omit the header."
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> derive_param_headers(
    json::Value const& input_schema, json::Value const& arguments) {
    std::vector<HeaderAnnotation> found;
    std::vector<std::string>      path;
    collect_header_annotations(input_schema, path, /*reachable=*/true, found);

    std::vector<std::pair<std::string, std::string>> headers;
    for (auto const& a : found) {
        json::Value const* cursor = &arguments;
        bool               ok     = true;
        for (auto const& step : a.path) {
            if (!cursor->is_object()) { ok = false; break; }
            cursor = cursor->find(step);
            if (!cursor) { ok = false; break; }
        }
        if (!ok || cursor == nullptr || cursor->is_null()) continue;  // omit, per the spec's table

        std::string raw;
        if (cursor->is_string()) {
            raw = cursor->as_string();
        } else if (cursor->is_bool()) {
            raw = cursor->as_bool() ? "true" : "false";
        } else if (cursor->is_number()) {
            // Declared `integer`, so a decimal representation with no fractional part.
            double const   n = cursor->as_number();
            long long const as_int = static_cast<long long>(n);
            raw = std::to_string(as_int);
        } else {
            continue;
        }
        headers.emplace_back("Mcp-Param-" + a.header_name, encode_header_value(raw));
    }
    return headers;
}

}  // namespace client_detail

// 011 §12/§13 Q1: "as a server, 2026-07-28-only" -- the same fixed literal on the client side, and
// the value `_meta`'s required `protocolVersion` key carries. Named once here rather than repeated at
// each call site (server.hpp:220 still has its own copy for its own `server/discover` result).
inline constexpr std::string_view kMcpProtocolVersion = "2026-07-28";

// SEP-2243's `Mcp-Param-{Name}` headers are derived from the TOOL SCHEMA, which only `McpClient`
// knows -- a bare `RequestSender` cannot compute them. Rather than move schema knowledge into the
// transport (wrong layer) this adds an optional headers-carrying sender.
//
// Additive on purpose, matching this codebase's own established precedent for exactly this situation
// (`StartRun::caller`, `ChatClientRegistry const*`): every existing `RequestSender` call site keeps
// compiling and behaving identically, and only a transport that actually speaks Streamable HTTP need
// opt in. That mirrors the spec's own layering -- "Clients using other transports (e.g., stdio) MAY
// ignore `x-mcp-header` annotations entirely."
using RequestSenderWithHeaders = std::function<JsonRpcResponse(
    JsonRpcRequest const&, std::vector<std::pair<std::string, std::string>> const&)>;

class McpClient {
public:
    McpClient(RequestSender sender, std::string client_name)
        : sender_(std::move(sender)), client_name_(std::move(client_name)) {}

    McpClient(RequestSenderWithHeaders sender, std::string client_name)
        : sender_with_headers_(std::move(sender)), client_name_(std::move(client_name)) {}

    // Tools excluded from `tools/list` for an `x-mcp-header` violation, with the reason. The spec
    // says clients SHOULD log this; surfacing it rather than logging internally keeps the decision
    // about where diagnostics go with the host (026 §3's own "actionable, host-owned" discipline).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> const& rejected_tools() const noexcept {
        return rejected_tools_;
    }

    // §3.1: "ttlMs absent or negative -> treat as 0. TTL is not a polling interval." `ttl.count() <= 0`
    // therefore means "never serve from cache," the honest reading of that rule -- not "cache
    // forever," which a naive `ttl == 0 -> no expiry` reading would silently produce.
    void set_ttl(std::chrono::milliseconds ttl) noexcept { ttl_ = ttl; }

    // 011 §3.4: opting in to answering MRTR input requests. Without this, `call_tool` returns an
    // `input_required` outcome rather than retrying.
    void set_input_request_handler(InputRequestHandler h) { input_handler_ = std::move(h); }

    [[nodiscard]] result<std::vector<McpToolInfo>> list_tools(std::string const& cursor = "") {
        // §3.1: "Cache key = method + the parameters that affect the result." An empty-string
        // cursor is still a real, distinct parameter value (§3.1's own "not end-of-results" rule) --
        // folding it into the key, rather than treating "no cursor supplied" and "empty cursor" as
        // the same case, is what keeps a caller paging through real cursors from ever reading a
        // stale different-page's cached entry.
        std::string const cache_key = "tools/list:" + cursor;

        if (auto it = cache_.find(cache_key); it != cache_.end() && ttl_.count() > 0) {
            auto const age = std::chrono::steady_clock::now() - it->second.cached_at;
            if (age < ttl_) return it->second.tools;
        }

        JsonRpcRequest req{RpcId{next_id()}, "tools/list",
                            with_request_meta(json::Value::make_object(
                                {{"cursor", json::Value::make_string(cursor)}}))};
        JsonRpcResponse resp = send(req);
        if (resp.error.has_value()) {
            return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
        }
        json::Value const* tools_field = resp.result->find("tools");
        if (!tools_field || !tools_field->is_array()) {
            return std::unexpected(error{failure_class::contract,
                                          "tools/list result missing a \"tools\" array",
                                          "mcp.malformed_result"});
        }

        std::vector<McpToolInfo> tools;
        for (json::Value const& item : tools_field->as_array()) {
            json::Value const* name        = item.find("name");
            json::Value const* description = item.find("description");
            json::Value const* input       = item.find("inputSchema");
            json::Value const* output      = item.find("outputSchema");
            if (!name || !name->is_string()) {
                return std::unexpected(error{failure_class::contract,
                                              "a tools/list entry is missing \"name\"",
                                              "mcp.malformed_tool_entry"});
            }
            // SEP-2243: "Clients using the Streamable HTTP transport MUST reject tool definitions
            // where any `x-mcp-header` value violates these constraints. Rejection means the client
            // MUST exclude the invalid tool from the result of `tools/list`." Deliberately an
            // EXCLUSION, not a `std::unexpected`: the spec's own reason is that "a single malformed
            // tool definition does not prevent other valid tools from being used." The reason is
            // retained (SHOULD-log) rather than discarded, so a caller can report it.
            if (input) {
                std::string const violation = client_detail::x_mcp_header_violation(*input);
                if (!violation.empty()) {
                    rejected_tools_.emplace_back(name->as_string(), violation);
                    continue;
                }
            }
            tools.push_back(McpToolInfo{
                name->as_string(), description && description->is_string() ? description->as_string() : std::string{},
                input ? *input : json::Value{}, output ? *output : json::Value{}});
        }

        // §8: digest-pinning. A tool listing that changed under a cache key we've already seen is a
        // rug pull -- flagged, never silently re-trusted as if it had always looked this way.
        std::string const digest = client_detail::digest_of(tools);
        if (auto prior = cache_.find(cache_key); prior != cache_.end() && prior->second.digest != digest) {
            rug_pull_detected_ = true;
        }
        cache_[cache_key] = client_detail::CacheEntry{tools, std::chrono::steady_clock::now(), digest};

        return tools;
    }

    [[nodiscard]] result<McpToolCallOutcome> call_tool(std::string const& name, json::Value arguments) {
        // SEP-2243 client behaviour step 3/4/5: derive `Mcp-Param-{Name}` from the tool's OWN
        // schema before the arguments are moved into the request. Derived from the cached listing
        // rather than re-fetched -- the schema this client validated is the one it mirrors from, so a
        // server that changes the schema underneath is a rug pull (§8), not a silent header change.
        auto const param_headers = param_headers_for(name, arguments);
        json::Value base_params = json::Value::make_object(
            {{"name", json::Value::make_string(name)}, {"arguments", std::move(arguments)}});

        // 011 §3.4 / SEP-2322's MRTR loop. Each round is a RETRY OF THE ORIGINAL REQUEST carrying
        // `inputResponses`, never a new method -- and every round mints a fresh id, because "the
        // JSON-RPC `id` MUST differ between the original request and the retry."
        //
        // `inputRequests`/`requestState` are held in locals for the duration of one call and never
        // stored on the client: the spec says they "affect only that retry and MUST NOT be reused on
        // any parallel request", so an unrelated call made between rounds structurally cannot pick
        // them up.
        json::Value                params = with_request_meta(base_params);
        JsonRpcResponse            resp;
        constexpr int              kMaxMrtrRounds = 8;  // bounded: a server that keeps asking must not spin us forever
        for (int round = 0;; ++round) {
            JsonRpcRequest req{RpcId{next_id()}, "tools/call", params};
            resp = send(req, param_headers);
            if (resp.error.has_value()) {
                // A PROTOCOL error (unknown tool, malformed request) -- 011 §3.1's own split, the
                // client-side half symmetric with `McpServer`'s own server-side proof (server.hpp).
                return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
            }

            json::Value const* result_type = resp.result->find("resultType");
            // §3.4: "results from earlier-revision servers that omit it MUST be treated as complete."
            bool const is_input_required =
                result_type && result_type->is_string() && result_type->as_string() == "input_required";
            if (!is_input_required) break;

            json::Value const* input_requests = resp.result->find("inputRequests");
            json::Value const* request_state  = resp.result->find("requestState");

            if (!input_handler_ || round >= kMaxMrtrRounds) {
                McpToolCallOutcome out;
                out.input_required = true;
                out.input_requests = input_requests ? *input_requests : json::Value::make_object({});
                return out;  // surfaced, not an error -- the server may not assume we retry
            }

            // Answer each server-assigned key. A key the handler declines is simply left out: §9's
            // own rule is that under-answering earns another `InputRequiredResult`, not an error, so
            // partial answers are a legitimate move rather than a failure to report.
            std::vector<std::pair<std::string, json::Value>> responses;
            if (input_requests && input_requests->is_object()) {
                for (auto const& [key, request] : input_requests->as_object()) {
                    if (auto answered = input_handler_(key, request)) {
                        responses.emplace_back(key, std::move(*answered));
                    }
                }
            }

            auto next_fields = base_params.as_object();  // rebuild from the ORIGINAL params each round
            next_fields.emplace_back("inputResponses", json::Value::make_object(std::move(responses)));
            // "Clients MUST NOT inspect, parse, or modify `requestState`; if absent in the result,
            // the client MUST NOT include one in the retry." Copied opaquely, and only if present.
            if (request_state) next_fields.emplace_back("requestState", *request_state);
            params = with_request_meta(json::Value::make_object(std::move(next_fields)));
        }

        json::Value const* is_error = resp.result->find("isError");
        json::Value const* content  = resp.result->find("content");
        McpToolCallOutcome outcome;
        outcome.is_error = is_error && is_error->as_bool();
        outcome.content  = content ? *content : json::Value::make_array({});
        // §3.1: "surfaced to the model for self-correction" -- an execution failure is returned as a
        // SUCCESSFUL result<> here, never std::unexpected. This layer's own job ends at faithfully
        // relaying what the server said; deciding what to DO with isError:true is the caller's.
        return outcome;
    }

    [[nodiscard]] bool rug_pull_detected() const noexcept { return rug_pull_detected_; }

    // Phase C4: `tools/call` with the `io.modelcontextprotocol/tasks` extension requested (§12: a
    // server "MUST NOT return CreateTaskResult to a client that did not include the extension
    // capability on that request" -- this is that per-request opt-in). Returns the task handle a
    // Backgroundable tool call was started under; the caller then polls `get_task()`.
    [[nodiscard]] result<McpTaskHandle> call_tool_as_task(std::string const& name, json::Value arguments) {
        JsonRpcRequest req{
            RpcId{next_id()}, "tools/call",
            json::Value::make_object(
                {{"name", json::Value::make_string(name)},
                 {"arguments", std::move(arguments)},
                 {"extensions",
                  json::Value::make_array({json::Value::make_string(std::string(kMcpTasksExtensionClient))})}})};
        JsonRpcResponse resp = send(req);
        if (resp.error.has_value()) {
            return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
        }
        json::Value const* task = resp.result->find("task");
        json::Value const* task_id = task ? task->find("taskId") : nullptr;
        json::Value const* status  = task ? task->find("status") : nullptr;
        if (!task_id || !task_id->is_string() || !status || !status->is_string()) {
            return std::unexpected(error{failure_class::contract,
                                          "tools/call task response missing a well-formed \"task\"",
                                          "mcp.malformed_result"});
        }
        return McpTaskHandle{task_id->as_string(), status->as_string()};
    }

    // §12: "Status ∈ working | input_required | completed | cancelled | failed." A caller polls until
    // `poll.status != "working"`; `poll.outcome` is populated only once the server attaches a result
    // (§12: "The failed status MUST NOT represent a tool result with isError:true -- that is completed
    // with error details in result" -- so a real execution failure arrives as `status == "completed"`
    // with `outcome.is_error == true`, exactly `call_tool()`'s own isError contract, never as
    // `status == "failed"`).
    [[nodiscard]] result<McpTaskPoll> get_task(std::string const& task_id) {
        JsonRpcRequest req{RpcId{next_id()}, "tasks/get",
                            json::Value::make_object({{"taskId", json::Value::make_string(task_id)}})};
        JsonRpcResponse resp = send(req);
        if (resp.error.has_value()) {
            return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
        }
        json::Value const* task   = resp.result->find("task");
        json::Value const* status = task ? task->find("status") : nullptr;
        if (!status || !status->is_string()) {
            return std::unexpected(error{failure_class::contract,
                                          "tasks/get response missing a well-formed \"task\"",
                                          "mcp.malformed_result"});
        }
        McpTaskPoll poll;
        poll.status = status->as_string();
        json::Value const* content  = resp.result->find("content");
        json::Value const* is_error = resp.result->find("isError");
        if (content) {
            poll.has_result         = true;
            poll.outcome.is_error   = is_error && is_error->as_bool();
            poll.outcome.content    = *content;
        }
        return poll;
    }

    // §12: no `notifications/cancelled` here (spec-forbidden for tasks); a real `tasks/cancel` call
    // instead. The underlying worker thread on the server side cannot actually be stopped (server.hpp's
    // own documented limit) -- cancelling only guarantees a subsequent `get_task()` never reports
    // `"completed"` for this task again.
    [[nodiscard]] result<void> cancel_task(std::string const& task_id) {
        JsonRpcRequest req{RpcId{next_id()}, "tasks/cancel",
                            json::Value::make_object({{"taskId", json::Value::make_string(task_id)}})};
        JsonRpcResponse resp = send(req);
        if (resp.error.has_value()) {
            return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
        }
        return {};
    }

private:
    [[nodiscard]] std::string next_id() { return client_name_ + ":" + std::to_string(++next_id_); }

    // Looks the tool up in whatever listing this client has already cached. A tool never listed
    // yields no headers, which is the correct conservative answer: the client cannot know which
    // parameters a server designates without having seen the schema, and inventing headers from an
    // unseen schema is exactly the header/body mismatch `-32020` exists to catch.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> param_headers_for(
        std::string const& tool_name, json::Value const& arguments) const {
        for (auto const& [key, entry] : cache_) {
            (void)key;
            for (auto const& t : entry.tools) {
                if (t.name == tool_name) {
                    return client_detail::derive_param_headers(t.input_schema, arguments);
                }
            }
        }
        return {};
    }

    // One place both sender shapes funnel through, so no call site has to know which one the client
    // was constructed with. Headers are ignored by the plain sender -- correct per the spec's own
    // "other transports MAY ignore x-mcp-header annotations entirely".
    [[nodiscard]] JsonRpcResponse send(JsonRpcRequest const& req,
                                        std::vector<std::pair<std::string, std::string>> headers = {}) {
        if (sender_with_headers_) return sender_with_headers_(req, headers);
        return sender_(req);
    }

    // 011 §2's per-request `_meta`, which every outbound request carries. Added after the official
    // `@modelcontextprotocol/conformance` suite (0.2.0-alpha.11, spec 2026-07-28) rejected our
    // `tools/list` with `ListToolsRequest/params: must have required property '_meta'` and the mock
    // answered HTTP 400 -- exactly the behaviour the spec detail documents for a request missing a
    // required field ("malformed: -32602, and on HTTP 400 Bad Request",
    // docs/research/2026-mcp-protocol-detail.md:230). Phase C1 deliberately built a generic JSON-RPC
    // envelope with "no `_meta` (011 §2)" and Phase C3 never added it, so no outbound request this
    // client produced was schema-valid for this revision. Recorded in
    // docs/research/2026-08-15-mcp-conformance-harness.md.
    //
    // Required keys are `protocolVersion` and `clientCapabilities`; `clientInfo` is SHOULD and sent
    // (the research note flags that SEP-2575 marks it required while the spec page marks it optional,
    // and that the spec page governs -- sending it satisfies both readings). `logLevel` is
    // deliberately NOT sent: a server "MUST NOT emit notifications/message for a request that omitted
    // it" (011 §2), and this client has no notification sink, so omitting it is the correct request
    // rather than a gap. `traceparent`/`tracestate` are 016's job and are not invented here.
    [[nodiscard]] json::Value with_request_meta(json::Value params) const {
        json::Value meta = json::Value::make_object(
            {{"io.modelcontextprotocol/protocolVersion",
              json::Value::make_string(std::string(kMcpProtocolVersion))},
             // Declared capabilities. `extensions` is present and empty rather than absent: §3.6's
             // "never assumed enabled on either side" cuts both ways, and an explicit empty value is
             // the honest statement that this client opts into none by default. `call_tool_as_task`
             // adds the tasks extension per-request, which is where that opt-in belongs (§12).
             //
             // An OBJECT, not an array -- keyed by extension identifier. Corrected against the
             // official schema, which rejected an array with
             // `_meta/io.modelcontextprotocol~1clientCapabilities/extensions: must be object`. 011 §2
             // says only "our declared capabilities, including `extensions`" and does not give the
             // shape, so this was an assumption the suite caught; recorded rather than quietly fixed.
             {"io.modelcontextprotocol/clientCapabilities",
              json::Value::make_object({{"extensions", json::Value::make_object({})}})},
             {"io.modelcontextprotocol/clientInfo",
              json::Value::make_object({{"name", json::Value::make_string(client_name_)},
                                         {"version", json::Value::make_string("0.1.0")}})}});

        if (!params.is_object()) {
            return json::Value::make_object({{"_meta", std::move(meta)}});
        }
        auto fields = params.as_object();  // copy: `Value`'s accessor is const-ref
        fields.emplace_back("_meta", std::move(meta));
        return json::Value::make_object(std::move(fields));
    }

    RequestSender              sender_;
    RequestSenderWithHeaders   sender_with_headers_;
    InputRequestHandler        input_handler_;
    std::vector<std::pair<std::string, std::string>> rejected_tools_;
    std::string                client_name_;
    std::uint64_t               next_id_ = 0;
    std::chrono::milliseconds   ttl_{0};
    std::unordered_map<std::string, client_detail::CacheEntry> cache_;
    bool                        rug_pull_detected_ = false;
};

}  // namespace agentengine::mcp
