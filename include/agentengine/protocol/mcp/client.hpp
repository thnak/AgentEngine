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

}  // namespace client_detail

class McpClient {
public:
    McpClient(RequestSender sender, std::string client_name)
        : sender_(std::move(sender)), client_name_(std::move(client_name)) {}

    // §3.1: "ttlMs absent or negative -> treat as 0. TTL is not a polling interval." `ttl.count() <= 0`
    // therefore means "never serve from cache," the honest reading of that rule -- not "cache
    // forever," which a naive `ttl == 0 -> no expiry` reading would silently produce.
    void set_ttl(std::chrono::milliseconds ttl) noexcept { ttl_ = ttl; }

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
                            json::Value::make_object({{"cursor", json::Value::make_string(cursor)}})};
        JsonRpcResponse resp = sender_(req);
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
        JsonRpcRequest req{RpcId{next_id()}, "tools/call",
                            json::Value::make_object({{"name", json::Value::make_string(name)},
                                                       {"arguments", std::move(arguments)}})};
        JsonRpcResponse resp = sender_(req);
        if (resp.error.has_value()) {
            // A PROTOCOL error (unknown tool, malformed request) -- 011 §3.1's own split, the
            // client-side half symmetric with `McpServer`'s own server-side proof (server.hpp).
            return std::unexpected(error{failure_class::contract, resp.error->message, "mcp.rpc_error"});
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

private:
    [[nodiscard]] std::string next_id() { return client_name_ + ":" + std::to_string(++next_id_); }

    RequestSender              sender_;
    std::string                client_name_;
    std::uint64_t               next_id_ = 0;
    std::chrono::milliseconds   ttl_{0};
    std::unordered_map<std::string, client_detail::CacheEntry> cache_;
    bool                        rug_pull_detected_ = false;
};

}  // namespace agentengine::mcp
