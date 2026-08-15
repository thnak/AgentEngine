// Implements ADR-023 §10 Tier 1 (decisions/ADR-023-host-provided-inbound-transport.md): the MCP
// CLIENT role driven by the official `@modelcontextprotocol/conformance` harness, which is 011 §10
// G2's gate.
//
// Why this exists at all, and why it needs no listener: the harness's two roles are asymmetric.
// `conformance server --url ...` requires an endpoint we would have to host; `conformance client
// --command ...` SPAWNS THIS BINARY and appends the URL of a test server IT runs as the final
// argument (empirically confirmed, see docs/research/2026-08-15-mcp-conformance-harness.md). So the
// client role needs no listener, no host adapter, no fixture, and none of ADR-023 §8.5's
// conformance-attribution apparatus -- every result is engine-attributable by construction, because
// the harness drives us. That is why Tier 1 is buildable while Tier 3 (the host-fronted server role)
// is still blocked on 33 open red-team findings.
//
// Nothing here is new engine machinery. It wires three already-proven pieces together:
//   - `McpClient` (protocol/mcp/client.hpp, M7 Phase C3) -- takes a `RequestSender` callable, which
//     is exactly the seam a transport plugs into.
//   - `perform_http_exchange` (sandbox/net_egress_proxy.hpp, ADR-011) -- plain HTTP/1.1, and
//     deliberately NOT behind AGENTENGINE_WITH_HTTPS (provider_http_client.hpp's own note: "the
//     plaintext branch reuses perform_http_exchange, which has no such gate").
//   - `resolve_host` (ADR-016) -- the HOST-INITIATED resolver, which does no blocked-range filtering.
//
// That last choice is load-bearing and is ADR-023 §10b claim 4. The harness serves on loopback, and
// `resolve_and_validate` (the GUEST resolver) blocks 127.0.0.0/8 as ADR-011's anti-SSRF control. This
// binary is not a guest path: the destination is supplied by the operator running the gate, on the
// command line, never derived from model output (I3) and never guest-supplied -- exactly the case
// ADR-016 judged. Swapping `resolve_host` for `resolve_and_validate` here MUST fail on loopback; that
// is the negative half of claim 4's two-way control, and `tests/test_mcp_conformance_transport.cpp`
// asserts both halves.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/mcp/client.hpp"
#include "agentengine/protocol/mcp/json_rpc.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

namespace {

namespace ae   = agentengine;
namespace mcp  = agentengine::mcp;
namespace json = agentengine::json;
namespace sb   = agentengine::sandbox;

// The harness appends `http://<host>:<port>/<path>` as the last argument. Deliberately a strict,
// scheme-checked parse rather than a permissive one: this binary is a conformance artifact, so a
// malformed endpoint must fail loudly at startup rather than be silently coerced into something that
// half-works and produces a misleading percentage (ADR-023 §10b claim 3).
struct ParsedUrl {
    std::string   host;
    std::uint16_t port = 0;
    std::string   path;
};

[[nodiscard]] std::optional<ParsedUrl> parse_http_url(std::string_view url) {
    constexpr std::string_view kScheme = "http://";
    if (url.substr(0, kScheme.size()) != kScheme) return std::nullopt;
    url.remove_prefix(kScheme.size());

    std::size_t const slash = url.find('/');
    std::string_view authority = (slash == std::string_view::npos) ? url : url.substr(0, slash);
    std::string_view path      = (slash == std::string_view::npos) ? std::string_view{"/"}
                                                                    : url.substr(slash);
    if (authority.empty()) return std::nullopt;

    ParsedUrl out;
    std::size_t const colon = authority.find(':');
    if (colon == std::string_view::npos) {
        out.host = std::string(authority);
        out.port = 80;
    } else {
        out.host = std::string(authority.substr(0, colon));
        std::string_view const port_text = authority.substr(colon + 1);
        if (port_text.empty()) return std::nullopt;
        unsigned long parsed = 0;
        for (char c : port_text) {
            if (c < '0' || c > '9') return std::nullopt;
            parsed = parsed * 10 + static_cast<unsigned long>(c - '0');
            if (parsed > 65535) return std::nullopt;
        }
        out.port = static_cast<std::uint16_t>(parsed);
    }
    if (out.host.empty()) return std::nullopt;
    out.path = std::string(path);
    return out;
}

// Streamable HTTP lets a server answer a POST with either a single JSON body or an SSE stream, so a
// client that only understands `application/json` is not a conformant client. Handled here rather
// than in `McpClient` because it is a TRANSPORT concern: `McpClient`'s contract is
// `JsonRpcRequest -> JsonRpcResponse`, and which framing carried those bytes is not its business.
// This extracts the LAST `data:` payload, which is the response to the request just issued -- any
// preceding events on that stream are notifications (011 §3.3's request-scoped channel), which this
// Tier-1 client does not consume.
[[nodiscard]] std::string extract_sse_last_data(std::string const& body) {
    std::string collected;
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line{body.data() + pos, eol - pos};
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        constexpr std::string_view kData = "data:";
        if (line.substr(0, kData.size()) == kData) {
            std::string_view payload = line.substr(kData.size());
            if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
            collected.assign(payload);
        }
        pos = eol + 1;
    }
    return collected;
}

// `perform_http_exchange` is "Content-Length-framed only -- no chunked transfer-encoding support this
// milestone, a documented cut since this proxy's own test server never emits it"
// (sandbox/net_egress_proxy.hpp). The conformance harness's mock DOES emit chunked
// (`b42\r\n{"jsonrpc"...`), so a real MCP server can too, and the cut is a genuine client-role
// conformance gap -- predicted by ADR-023 §9h's R19 and hit empirically here.
//
// Decoded HERE rather than inside the egress proxy on purpose: that function is ADR-011-judged code
// whose byte cap is enforced DURING the read loop (claim C8), and widening its framing would require
// re-running that proof. Decoding the already-capped buffer afterwards changes none of its proven
// properties. Adding real chunked support to the engine's own HTTP client is a named follow-up
// against ADR-011, not a drive-by edit.
//
// Returns std::nullopt if the body is not well-formed chunked, so a caller can fall back rather than
// silently truncate.
[[nodiscard]] std::optional<std::string> decode_chunked(std::string const& body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t const eol = body.find("\r\n", pos);
        if (eol == std::string::npos) return std::nullopt;
        std::string_view size_line{body.data() + pos, eol - pos};
        if (auto const semi = size_line.find(';'); semi != std::string_view::npos) {
            size_line = size_line.substr(0, semi);  // strip chunk extensions
        }
        if (size_line.empty()) return std::nullopt;
        std::size_t chunk = 0;
        for (char c : size_line) {
            std::size_t digit = 0;
            if (c >= '0' && c <= '9')      digit = static_cast<std::size_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::size_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::size_t>(c - 'A' + 10);
            else return std::nullopt;
            chunk = chunk * 16 + digit;
        }
        pos = eol + 2;
        if (chunk == 0) return out;              // terminal chunk; trailers ignored
        if (pos + chunk > body.size()) return std::nullopt;
        out.append(body, pos, chunk);
        pos += chunk;
        if (body.compare(pos, 2, "\r\n") == 0) pos += 2;
    }
    return out;
}

[[nodiscard]] bool header_contains(std::vector<std::pair<std::string, std::string>> const& headers,
                                    std::string_view name, std::string_view needle) {
    for (auto const& [k, v] : headers) {
        if (k.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < k.size(); ++i) {
            char const a = static_cast<char>(std::tolower(static_cast<unsigned char>(k[i])));
            char const b = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            if (a != b) { same = false; break; }
        }
        if (same && v.find(needle) != std::string::npos) return true;
    }
    return false;
}

// 011 §7: "Requests MUST carry the standard MCP request headers (`Mcp-Method`, `Mcp-Name`) on POST."
// `Mcp-Name` is the primitive's own name where the call names one -- the tool for `tools/call` -- and
// is omitted where the method names no primitive, rather than being sent empty.
[[nodiscard]] std::optional<std::string> mcp_name_for(mcp::JsonRpcRequest const& req) {
    json::Value const* name = req.params.find("name");
    if (name && name->is_string()) return name->as_string();
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                      "usage: %s [options] <server-url>\n"
                      "  The MCP conformance harness appends the URL of the test server it runs.\n",
                      argc > 0 ? argv[0] : "agentengine_mcp_conformance_client");
        return 2;
    }

    // The harness appends the URL LAST; earlier argv entries (if a wrapper added any) are ignored.
    std::string const url_text = argv[argc - 1];
    auto const        parsed   = parse_http_url(url_text);
    if (!parsed) {
        std::fprintf(stderr, "FATAL: not a parseable http:// URL: %s\n", url_text.c_str());
        return 2;
    }

    // ADR-023 §10b claim 3: the scenario is contract, not decoration -- reported so a run's output
    // shows which scenario the harness selected without having to correlate by timestamp.
    char const* scenario = std::getenv("MCP_CONFORMANCE_SCENARIO");
    std::fprintf(stderr, "agentengine-mcp-conformance-client: scenario=%s endpoint=%s\n",
                  scenario ? scenario : "<unset>", url_text.c_str());

    // ADR-016's HOST-INITIATED resolver -- see this file's own top comment for why this, and not
    // `resolve_and_validate`, is the correct and non-weakening choice here.
    auto endpoint = sb::resolve_host(parsed->host, parsed->port);
    if (!endpoint) {
        std::fprintf(stderr, "FATAL: could not resolve %s:%u -- %s (%s)\n", parsed->host.c_str(),
                      static_cast<unsigned>(parsed->port), endpoint.error().message.c_str(),
                      endpoint.error().code.c_str());
        return 2;
    }

    std::string const host_header =
        parsed->host + ":" + std::to_string(static_cast<unsigned>(parsed->port));

    mcp::RequestSenderWithHeaders sender =
        [&](mcp::JsonRpcRequest const& req,
            std::vector<std::pair<std::string, std::string>> const& extra_headers)
        -> mcp::JsonRpcResponse {
        json::Value const body_json = mcp::to_json(req);
        std::string const body      = json::dump(body_json);

        sb::NetEgressRequest http_req;
        http_req.method = "POST";
        http_req.path   = parsed->path;
        http_req.body   = body;
        http_req.headers.emplace_back("Content-Type", "application/json");
        // Streamable HTTP: a server may answer with either framing, so a conformant client must
        // declare it accepts both.
        http_req.headers.emplace_back("Accept", "application/json, text/event-stream");
        http_req.headers.emplace_back("Mcp-Method", req.method);  // 011 §7 MUST
        if (auto name = mcp_name_for(req)) {
            // "The same encoding rule applies to the `Mcp-Name` header value. Tool and prompt names
            // are only SHOULD-constrained to header-safe characters, so a name (or resource URI)
            // outside the safe set is carried as `=?base64?...?=`."
            http_req.headers.emplace_back("Mcp-Name",
                                           mcp::client_detail::encode_header_value(*name));
        }
        // SEP-2243's `Mcp-Param-{Name}` set, derived by `McpClient` from the tool's own schema --
        // the transport carries them but does not compute them, since only the client knows which
        // parameters the server designated.
        for (auto const& [k, v] : extra_headers) http_req.headers.emplace_back(k, v);
        // Required by the Streamable HTTP transport, and NOT listed in 011 §7 -- which names only
        // `Mcp-Method`/`Mcp-Name`. Found by the official suite, whose mock answered
        // `-32020 "Missing MCP-Protocol-Version header"` with HTTP 400. Recorded in
        // docs/research/2026-08-15-mcp-conformance-harness.md as an RFC gap, not silently patched.
        http_req.headers.emplace_back("MCP-Protocol-Version",
                                       std::string(mcp::kMcpProtocolVersion));

        auto http_resp = sb::perform_http_exchange(*endpoint, host_header, http_req,
                                                    /*byte_cap=*/std::nullopt);
        if (!http_resp) {
            return mcp::JsonRpcResponse::make_error(
                req.id, mcp::JsonRpcError{mcp::kRpcInternalError,
                                           "transport failure: " + http_resp.error().message,
                                           json::Value{}});
        }
        if (http_resp->status < 200 || http_resp->status >= 300) {
            // Carry the body into the message: a conformance run's whole value is the server telling
            // us precisely what it objected to, and swallowing that turns a diagnosable failure into
            // a bare status code.
            return mcp::JsonRpcResponse::make_error(
                req.id,
                mcp::JsonRpcError{mcp::kRpcInternalError,
                                   "HTTP " + std::to_string(static_cast<unsigned>(http_resp->status)) +
                                       " body=" + http_resp->body,
                                   json::Value{}});
        }

        // Un-chunk first if needed, then apply SSE framing to whatever the body really is.
        std::string framed = http_resp->body;
        if (header_contains(http_resp->headers, "transfer-encoding", "chunked")) {
            if (auto decoded = decode_chunked(framed)) framed = std::move(*decoded);
        }
        std::string const payload =
            header_contains(http_resp->headers, "content-type", "text/event-stream")
                ? extract_sse_last_data(framed)
                : framed;

        auto parsed_json = json::parse(payload);
        if (!parsed_json) {
            // Include the raw prefix and the content-type: a conformance failure is only useful if
            // it says what actually arrived.
            std::string ctype = "<none>";
            for (auto const& [k, v] : http_resp->headers) {
                if (k.size() == 12) {
                    bool same = true;
                    char const* want = "content-type";
                    for (std::size_t i = 0; i < 12; ++i) {
                        if (std::tolower(static_cast<unsigned char>(k[i])) != want[i]) { same = false; break; }
                    }
                    if (same) ctype = v;
                }
            }
            return mcp::JsonRpcResponse::make_error(
                req.id, mcp::JsonRpcError{mcp::kRpcInternalError,
                                           "response body is not JSON: " + parsed_json.error().message +
                                               " content-type=" + ctype +
                                               " raw[0:160]=" + http_resp->body.substr(0, 160),
                                           json::Value{}});
        }
        auto response = mcp::parse_response(*parsed_json);
        if (!response) {
            return mcp::JsonRpcResponse::make_error(
                req.id, mcp::JsonRpcError{mcp::kRpcInternalError,
                                           "not a JSON-RPC response: " + response.error().message,
                                           json::Value{}});
        }
        return *response;
    };

    mcp::McpClient client(sender, "agentengine-conformance");

    auto tools = client.list_tools();
    if (!tools) {
        std::fprintf(stderr, "FAIL: tools/list -- %s (%s)\n", tools.error().message.c_str(),
                      tools.error().code.c_str());
        return 1;
    }
    std::fprintf(stderr, "tools/list: %zu tool(s)\n", tools->size());
    for (auto const& t : *tools) std::fprintf(stderr, "  - %s\n", t.name.c_str());

    // Scenario behaviour. `tools_call` requires the client to actually invoke `add_numbers`; the
    // harness reports "Tool was not called by client" otherwise. Arguments are built from the tool's
    // OWN advertised schema rather than hardcoded, so this does not silently depend on parameter
    // names the harness is free to change.
    int exit_code = 0;
    for (auto const& t : *tools) {
        json::Value args = json::Value::make_object({});
        if (auto const* props = t.input_schema.find("properties"); props && props->is_object()) {
            std::vector<std::pair<std::string, json::Value>> filled;
            for (auto const& [key, spec] : props->as_object()) {
                auto const* type = spec.find("type");
                if (type && type->is_string() && type->as_string() == "number") {
                    filled.emplace_back(key, json::Value::make_number(filled.empty() ? 5.0 : 7.0));
                } else if (type && type->is_string() && type->as_string() == "integer") {
                    filled.emplace_back(key, json::Value::make_number(filled.empty() ? 5.0 : 7.0));
                } else if (type && type->is_string() && type->as_string() == "string") {
                    filled.emplace_back(key, json::Value::make_string("conformance"));
                } else if (type && type->is_string() && type->as_string() == "boolean") {
                    filled.emplace_back(key, json::Value::make_bool(true));
                }
            }
            args = json::Value::make_object(std::move(filled));
        }

        auto outcome = client.call_tool(t.name, args);
        if (!outcome) {
            std::fprintf(stderr, "FAIL: tools/call %s -- %s (%s)\n", t.name.c_str(),
                          outcome.error().message.c_str(), outcome.error().code.c_str());
            exit_code = 1;
            continue;
        }
        // 011 §3.1: an EXECUTION error is a result with isError:true, never a client-side failure --
        // surfaced, not converted into an exception (McpClient already honours this; reported here so
        // a conformance run's log shows it was observed rather than swallowed).
        std::fprintf(stderr, "tools/call %s: ok (isError=%s)\n", t.name.c_str(),
                      outcome->is_error ? "true" : "false");
    }

    return exit_code;
}
