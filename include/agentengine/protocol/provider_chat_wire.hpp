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
#include "agentengine/rt/http.hpp"     // HttpOptions / HttpContext / http_error (no TLS: the connector stays in src/)
#include "agentengine/rt/runtime.hpp"  // rt::Runtime (reactor + DNS pool + spawn)
#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::detail::provider_wire {

// Test seam for name resolution; production passes sandbox::resolve_host (see provider_http_client.hpp).
using Resolver = std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>;

// ADR-237 §5.1 (step 40): that same `Resolver` reused as the REACTOR's own DNS seam. `rt` runs a
// `HostResolver` on its DNS pool (rt/dns.hpp: "a blocking resolver, run on the DNS pool"), so production's
// `sandbox::resolve_host` keeps governing name resolution on the reactor path -- off the reactor thread --
// instead of being called inline by a blocking exchange. This is what lets the `Resolver` constructor
// parameter stay (meaningful) while the blocking client it used to feed is deleted.
[[nodiscard]] inline rt::HostResolver host_resolver_from(Resolver resolve, std::uint16_t port) {
    return [resolve = std::move(resolve), port](std::string const& host) -> pal::ResolveOutcome {
        pal::ResolveOutcome out;
        auto const         ep = resolve(host, port);
        if (!ep) {
            out.error = pal::resolve_error::not_found;  // resolve_host's own failure class for a dead name
            return out;
        }
        std::uint32_t const ip = ep->ipv4_host_order;
        out.addresses.push_back(pal::IpAddress::v4(static_cast<std::uint8_t>(ip >> 24),
                                                   static_cast<std::uint8_t>(ip >> 16),
                                                   static_cast<std::uint8_t>(ip >> 8),
                                                   static_cast<std::uint8_t>(ip)));
        return out;
    };
}

// ADR-237 §5.1/§6.3 (step 34): the reactor path's own inputs, replacing the synchronous `Resolver` seam for
// the backends' `chat_stream`. A NULL `runtime` keeps the blocking detached-thread path (every existing
// construction site, unchanged); a non-null one selects the coroutine path -- the producer is started on the
// reactor by `rt::Runtime::spawn` (the client owns it, §4.5 rule 2) and suspends instead of holding a thread.
// ae-naming-lint: allow ProviderReactorOptions — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProviderReactorOptions {
    rt::Runtime*      runtime = nullptr;  // null: the blocking path (today's behaviour)
    // Host code, never model output (I3). Empty: ACCEPT-ALL -- ADR-016's provider position (a host-configured
    // endpoint, typically loopback/RFC1918; there is no guest grant to mediate and no attacker to defend against).
    rt::AddressPolicy policy;
    // Test seam (rt/dns.hpp's HostResolver): lets a test bind a Host name to a loopback port with no DNS lookup.
    rt::HostResolver  resolver{};
    // Idle/stall/limits for the reactor path; `idle_timeout` is overwritten with sandbox::io_timeout_ms() (the
    // blocking client's bound) so a deployment's one setting governs both paths.
    rt::HttpOptions   options{};
};

// ADR-237 step 6's named leftover: map the reactor's `http_error` onto the engine's `failure_class`, keeping the
// blocking client's stable `net.*` codes. Mirrors `http_status_error`'s transient/policy/contract split.
[[nodiscard]] inline error http_error_to_error(rt::http_error e, std::string_view code, std::string_view message) {
    if (rt::is_resource_error(e)) {
        return error{failure_class::resource, std::string(message), std::string(code)};
    }
    failure_class klass = failure_class::fatal;
    switch (e) {
        case rt::http_error::canceled: klass = failure_class::canceled; break;
        case rt::http_error::deadline_exceeded:
        case rt::http_error::idle_timeout:
        case rt::http_error::truncated:
        case rt::http_error::connect_failed:
        case rt::http_error::tls_failed:
        case rt::http_error::transport: klass = failure_class::transient; break;
        case rt::http_error::no_policy:
        case rt::http_error::resolve_failed:
        case rt::http_error::address_rejected: klass = failure_class::policy; break;
        case rt::http_error::invalid_request:
        case rt::http_error::malformed_response:
        case rt::http_error::ambiguous_framing:
        case rt::http_error::busy:
        case rt::http_error::tls_unavailable: klass = failure_class::contract; break;
        case rt::http_error::none:
        case rt::http_error::consumer_stalled:
        case rt::http_error::header_too_large:
        case rt::http_error::body_too_large:
        case rt::http_error::event_too_large: klass = failure_class::fatal; break;
    }
    return error{klass, std::string(message), std::string(code)};
}

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
