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
// them: `resolve_host`/`VerifiedEndpoint` (the resolve-once-connect-to-a-verified-literal mechanism
// of ADR-011 claim C6, which is about DNS rebinding and applies on BOTH paths -- but NOT the
// blocked-range table of claims C4/C5, which is guest-path-only, see ADR-016 and the `resolver` note
// below) and `perform_https_exchange` (ADR-013's TLS transport, byte-cap-enforced read loop, and
// Phase C2's stop_token cancellation) for the actual exchange.
//
// TLS is the default and the only thing a caller gets without asking. ADR-016 adds an OPT-IN
// plaintext transport for the case 004 §3 explicitly targets and TLS cannot serve: a local
// llama.cpp/vLLM/Ollama server, which speaks plain HTTP and has no certificate. See
// `ProviderTransport` below for the security note -- it is a real trade, not a free one.
//
// Only declared when AGENTENGINE_WITH_HTTPS is ON, the same gate `perform_https_exchange` itself is
// behind (the plaintext branch reuses `perform_http_exchange`, which has no such gate, but this
// header's whole purpose is the provider path and splitting it across two build configurations would
// buy nothing).

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

namespace agentengine::sandbox {

// decisions/ADR-016-provider-egress-address-policy.md §3.
//
// `tls` is the default everywhere and the only value production code should normally use.
//
// `plaintext_http` exists for exactly one situation: a local inference server that speaks plain HTTP
// and has no certificate to present (llama.cpp's `llama-server`, vLLM, Ollama -- all named targets of
// 004 §3's OpenAI-compatible backend). It is a REAL trade, not a formality: the request carries the
// provider credential in an `Authorization: Bearer`/`x-api-key` header, and on this transport that
// header crosses the wire in clear. On loopback that is uncontroversial -- an attacker who can read
// loopback traffic already owns the process. Over any network it is a credential disclosure.
//
// Which is why it is (a) never a default, (b) never inferred from a URL scheme or probed from the
// endpoint, and (c) a distinct named enumerator at the call site rather than a bool -- a reader of
// the construction site can see which transport was chosen without consulting this header. 004 §3's
// "capabilities are declared, not probed" rule applied to the transport itself.
enum class ProviderTransport {
    tls,             // ADR-013's TlsClientSession: certificate verified against the vendored CA bundle
    plaintext_http,  // no TLS -- the credential header is readable on the wire (see above)
};

// Resolves `host` (see `resolver` below), then performs the exchange over `transport` -- one call
// replacing the connect-then-exchange sequence a ChatClient
// backend would otherwise have to duplicate. `req.method`/`req.path`/`req.headers` are HOST-
// constructed (a backend's own translation of a ChatRequest into the vendor's wire shape, 004 §3),
// never guest- or model-supplied, so this performs no CRLF-injection gate the way
// `HostEgressProxy::fetch` must for guest-supplied fields (006 §7's taint boundary does not apply
// here -- nothing on this call path originates from model output).
//
// `resolver` defaults to `resolve_host`, NOT `resolve_and_validate` (ADR-016): a host-initiated
// provider call's destination is the deployment's own configured inference endpoint, never
// guest-supplied and never derived from model output, so the blocked-range table that defends the
// guest path has no attacker to defend against here -- it only rejected the ordinary local/private
// deployments 004 §3 explicitly targets (llama.cpp, vLLM, Ollama, in-cluster gateways). The guest
// path is untouched and still resolves through `resolve_and_validate`.
//
// The injectable-resolver seam itself is unchanged -- the same testability seam
// `HostEgressProxy::resolver` already establishes (net_egress_proxy.hpp's own comment: "a
// testability seam, not a security bypass"). Since ADR-016 it is no longer needed merely to reach a
// loopback test server (`resolve_host` resolves 127.0.0.1 fine); it remains for tests that want to
// answer the resolution question WITHOUT a real DNS lookup at all -- e.g. binding an arbitrary
// `Host:` name to an ephemeral loopback port a test server just opened. `ca_bundle_pem_override`
// likewise exists solely so a test can present a self-signed leaf; production passes neither.
//
// `transport` is APPENDED last, after `ca_bundle_pem_override`, never inserted earlier: every
// existing call site passes these positionally, and inserting a parameter anywhere but the end would
// silently misalign all of them. `ca_bundle_pem_override` is ignored when `transport` is
// `plaintext_http` (there is no certificate to verify) rather than being a construction error --
// a caller flipping transports to compare them should not also have to restructure its arguments.
[[nodiscard]] result<NetEgressResponse> perform_provider_https_exchange(
    std::string_view host, std::uint16_t port, NetEgressRequest const& req, std::stop_token stop = {},
    std::optional<std::uint64_t> byte_cap = std::nullopt,
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> const& resolver =
        resolve_host,
    std::string_view ca_bundle_pem_override = {},
    ProviderTransport transport = ProviderTransport::tls);


// ADR-019: the streaming provider exchange -- `perform_provider_https_exchange`'s counterpart, with
// the same resolver/transport/credential contract, delivering body bytes to `on_body` as they arrive
// instead of buffering the whole response first.
//
// This is what makes `ChatClient::chat_stream()` genuinely incremental. Before it, a streaming call
// performed one COMPLETE blocking fetch and only then replayed the already-received events onto the
// ring -- so the vendor's chunk boundaries were preserved in delivery ORDER (004 §7 G3) but not in
// TIME, and a consumer saw nothing until the whole completion had arrived.
[[nodiscard]] result<NetEgressResponse> perform_provider_streaming_exchange(
    std::string_view host, std::uint16_t port, NetEgressRequest const& req,
    std::function<bool(std::string_view)> const& on_body, std::stop_token stop = {},
    std::optional<std::uint64_t> byte_cap = std::nullopt,
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> const& resolver =
        resolve_host,
    std::string_view ca_bundle_pem_override = {},
    ProviderTransport transport = ProviderTransport::tls);

}  // namespace agentengine::sandbox

#endif  // AGENTENGINE_WITH_HTTPS
