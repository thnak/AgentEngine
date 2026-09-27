#pragma once
// Implements 004-Model-Provider-Plane.md §3/§4 -- ADR-206 (#120 S6): the HTTP plumbing the OpenAI-compatible
// (protocol/openai/chat_client.hpp) and Anthropic (protocol/anthropic/chat_client.hpp) ChatClient backends share.
// The two had carried identical copies of it, differing only in the vendor name each wrote into its error
// messages. What stays per vendor: the wire format (request body, headers, response and SSE event parsing).
//
// The streaming half (the per-fragment push loop and its terminal-error rules) is a template over each vendor's
// StreamingUpdateAccumulator and lives in src/protocol/sse_stream_pump.hpp, next to its only two callers.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::detail::provider_wire {

// Test seam for name resolution; production passes sandbox::resolve_host (see provider_http_client.hpp).
using Resolver = std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>;

// A non-2xx response as an `error`: 429 and 5xx are transient (004 §4: retry applies to Transient only), 401/403
// policy, any other 4xx contract, anything else fatal. The message is the body's `error.message` when the body is
// JSON that has one, else "<vendor> http status <N>"; the code is "<vendor>.http_<N>". `body` is empty on the
// streaming path, which never buffers the error document; the classification does not depend on it.
[[nodiscard]] error http_status_error(std::string_view vendor, std::uint16_t status, std::string const& body);

// chat()'s whole exchange after the request is built: one blocking POST (no stop token, no byte cap), a
// transport failure forwarded unchanged, a non-2xx mapped through http_status_error, and a 2xx body parsed as
// JSON.
[[nodiscard]] result<json::Value> exchange_json(std::string_view vendor, std::string_view host, std::uint16_t port,
                                                sandbox::NetEgressRequest const& req, Resolver const& resolver,
                                                std::string_view ca_bundle_pem_override,
                                                sandbox::ProviderTransport transport);

}  // namespace agentengine::detail::provider_wire

#endif  // AGENTENGINE_WITH_HTTPS
