#pragma once
// Implements 004-Model-Provider-Plane.md §1/§3 -- Milestone 5 Phase C
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): a general-purpose,
// host-INITIATED HTTPS client for ChatClient backends (004 §3's OpenAI-compatible/Anthropic
// backends, Phase D/E) to call a real inference API directly.
//
// Deliberately NOT sandbox/net_egress_proxy.hpp's HostEgressProxy: that type mediates a WASM
// GUEST's outbound call through a single-verified-target cap::NetOut grant (008 §10 Q3, ADR-011).
// A ChatClient backend calling the provider it was itself constructed with (host-side code, not
// something a sandboxed guest asked for) has no guest capability grant to check -- there is nothing
// to mediate. This is a new, additional entry point, not a bypass of the existing one: the guest
// egress path (HostEgressProxy::fetch) is untouched and still gates every WASM-originated call
// exactly as ADR-011 specifies.
//
// Reuses net_egress_proxy.hpp's already-proven primitives directly rather than re-implementing
// them: `resolve_and_validate`/`VerifiedEndpoint` (the SSRF/DNS-rebinding defense, ADR-011 claims
// C4-C6 -- worth keeping as defense-in-depth even though the threat model differs: a misconfigured
// or DNS-poisoned provider hostname landing on a private/loopback address is still worth rejecting)
// and `perform_https_exchange` (ADR-013's TLS transport, byte-cap-enforced read loop, and Phase C2's
// stop_token cancellation) for the actual exchange. HTTPS only -- no real inference API this project
// targets speaks plain HTTP, so there is no plain-HTTP path here the way net_egress_proxy.hpp has
// one for the (guest-declarable) WASM case. Only declared when AGENTENGINE_WITH_HTTPS is ON, the
// same gate `perform_https_exchange` itself is behind.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

namespace agentengine::sandbox {

// Resolves `host`, validates the resolved address is not in a blocked range
// (`resolve_and_validate`), then performs the HTTPS exchange
// (`perform_https_exchange`) -- one call replacing the connect-then-exchange sequence a ChatClient
// backend would otherwise have to duplicate. `req.method`/`req.path`/`req.headers` are HOST-
// constructed (a backend's own translation of a ChatRequest into the vendor's wire shape, 004 §3),
// never guest- or model-supplied, so this performs no CRLF-injection gate the way
// `HostEgressProxy::fetch` must for guest-supplied fields (006 §7's taint boundary does not apply
// here -- nothing on this call path originates from model output).
//
// `resolver` defaults to the real `resolve_and_validate` -- the same injectable-resolver
// testability seam `HostEgressProxy::resolver` already establishes (net_egress_proxy.hpp's own
// comment: "a testability seam, not a security bypass"), needed here for the identical reason: a
// loopback test server's address is itself in `resolve_and_validate`'s own blocked-range table (it
// IS loopback), so a test proving this function's OWN behavior (not re-proving address-blocking,
// already exhaustively covered by test_net_egress_proxy.cpp) needs to supply a fake answering
// exactly the question DNS answers, without touching `is_blocked_address`'s real enforcement.
// Production code never passes a non-default `resolver` or `ca_bundle_pem_override` -- both exist
// solely so a test can prove this function's own orchestration logic (resolve -> exchange, no
// capability grant, cancellation) against a real, deterministic, offline TLS server, the same
// reasoning `perform_https_exchange`'s own `ca_bundle_pem_override` parameter already documents.
[[nodiscard]] result<NetEgressResponse> perform_provider_https_exchange(
    std::string_view host, std::uint16_t port, NetEgressRequest const& req, std::stop_token stop = {},
    std::optional<std::uint64_t> byte_cap = std::nullopt,
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> const& resolver =
        resolve_and_validate,
    std::string_view ca_bundle_pem_override = {});

}  // namespace agentengine::sandbox

#endif  // AGENTENGINE_WITH_HTTPS
