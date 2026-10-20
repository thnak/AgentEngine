#pragma once
// Implements rfcs/008-Sandbox-and-Isolation.md §10 Q3 and decisions/ADR-011-first-party-egress-proxy.md:
// the host-mediated egress mechanism `NetOut` (007 §3) is enforced through. See that ADR for the
// design rationale (why resolve-once-connect-to-verified-literal, why no redirect-following, why
// plain HTTP only this milestone) and docs/research/2026-08-05-ssrf-dns-rebinding-defense.md for the
// external claims this design rests on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/effect_context.hpp"  // ADR-241: egress_call_control() reads the call's EffectContext
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::sandbox {

// One verified IPv4 endpoint -- the ONLY thing `perform_http_exchange` can connect to. A plain value
// type, never a hostname: there is no call path by which a second, attacker-timed DNS resolution
// could happen between validation and connect (ADR-011 claim C6). IPv4-only, matching the vendored
// PAL's own current locator (`third_party/quark/pal/*/net.hpp`); an IPv6-only host is a resolution
// failure, not a silently narrower blocklist (ADR-011 §3, research note §5).
// ae-naming-lint: allow VerifiedEndpoint — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct VerifiedEndpoint {
    std::uint32_t ipv4_host_order = 0;
    std::uint16_t port = 0;
};

// A single unit of a plain-HTTP/1.1 request the guest actually controls (WIT `http-request-data`,
// wit/ae-tool.wit) -- host/port/scheme are never here, they come from the granted `cap::NetOut`
// (006's own design choice: "the guest supplies only the part of a call the host must still
// validate per-call... never the grant's own parameters").
// ae-naming-lint: allow NetEgressRequest — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct NetEgressRequest {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// ae-naming-lint: allow NetEgressResponse — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct NetEgressResponse {
    std::uint16_t status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// ae-naming-lint: allow NetEgressTraits — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct NetEgressTraits {
    std::string_view name;
    bool supports_https = false;
};

// The hard, host-side response-size ceiling that applies REGARDLESS of `cap::NetOut::byte_cap` --
// an absent guest-declared cap is not "the host has no protection either" (CLAUDE.md machine safety;
// 008 §2's "an unbounded stdout is a denial-of-service on the host", the same principle applied to a
// response body). `granted.byte_cap`, when set and smaller, is the tighter effective limit.
inline constexpr std::uint64_t kHardResponseCeilingBytes = 16u * 1024u * 1024u;

// Pure function, zero network dependency -- runs on the resolved 32-bit address `getaddrinfo`/
// `inet_pton` already produced, never a string (ADR-011 claim C5: immune to decimal/octal/hex/
// IPv4-mapped-IPv6 encoding-bypass classes by construction, not by enumerating encodings). Blocks:
// loopback (127.0.0.0/8), link-local (169.254.0.0/16, which contains the 169.254.169.254 cloud
// metadata address), the three RFC 1918 private ranges, CGNAT (100.64.0.0/10), multicast
// (224.0.0.0/4), reserved (240.0.0.0/4), and unspecified (0.0.0.0/8).
[[nodiscard]] bool is_blocked_address(std::uint32_t ipv4_host_order) noexcept;

// CR, LF or NUL anywhere in value (ADR-241 D2 added NUL) -- this proxy builds the raw HTTP/1.1 request line and header
// block itself (no vetted HTTP library exists to lean on, ADR-011 §2), so this is the only thing
// standing between a guest-controlled `path`/header value and request-splitting/header injection
// onto the wire (ADR-011 claim C9).
[[nodiscard]] result<std::monostate> reject_crlf(std::string_view field_name, std::string_view value);

// Exactly one resolution attempt: an `inet_pton` fast path first (so a numeric-literal allowlist
// host never touches the resolver, and a non-canonical numeric encoding of a blocked address fails
// to parse as a dotted-quad at all rather than needing to be filtered downstream), then
// `getaddrinfo(..., AF_INET, ...)` for a genuine hostname. The first candidate `is_blocked_address`
// does not reject is returned; if every candidate is blocked, or nothing resolves, this fails closed.
//
// This is the GUEST-egress resolver (`HostEgressProxy::fetch`, ADR-011 claims C4-C6): the host there
// is guest-supplied, so a private/loopback/metadata destination is exactly the SSRF attack the
// blocked-range table exists to stop.
[[nodiscard]] result<VerifiedEndpoint> resolve_and_validate(std::string_view host, std::uint16_t port);

// Every candidate `resolve_and_validate` would accept, in `getaddrinfo` order and de-duplicated, from the same
// single resolution attempt (shared implementation, so the two cannot drift; the first element is exactly what
// `resolve_and_validate` returns). Fails closed like it when none is acceptable. ADR-240: the Kata egress policy
// allows every validated record of a granted name (capped by the caller), not just the first.
[[nodiscard]] result<std::vector<VerifiedEndpoint>> resolve_and_validate_all(std::string_view host, std::uint16_t port);

// Same single resolution attempt and same resolve-once-connect-to-a-literal discipline as
// `resolve_and_validate` (they share one implementation so they cannot drift), but with NO
// blocked-range filtering: the first IPv4 candidate is returned whatever it is.
//
// decisions/ADR-016-provider-egress-address-policy.md: this is the HOST-INITIATED provider resolver
// (`perform_provider_https_exchange`, 004 §3). The threat models are genuinely different. On the
// guest path the destination is attacker-influenced, so private addresses are an attack. On the
// provider path the destination is a deployment's own configured inference endpoint -- never derived
// from model output (I3), never guest-supplied -- and a private address is the ORDINARY case: a
// llama.cpp/vLLM/Ollama server on loopback, a corporate gateway on RFC 1918, an in-cluster
// service. Refusing those was not defence, it was a false positive with no attacker on the other
// side of it.
//
// This deliberately does NOT weaken the guest path: `HostEgressProxy` (ADR-241) has no resolver or policy seam at all and runs
// `egress_address_policy` over every answer; `resolve_and_validate` above remains for the Kata backend and the relay, and
// test_net_egress_proxy.cpp still proves every blocked range.
[[nodiscard]] result<VerifiedEndpoint> resolve_host(std::string_view host, std::uint16_t port);

// Connects to exactly `endpoint` (never a hostname -- see `VerifiedEndpoint`'s own comment), speaks
// plain HTTP/1.1 (request line + headers + optional body; reads a status line + headers + body back,
// Content-Length-framed only -- no chunked transfer-encoding support this milestone, a documented cut
// since this proxy's own test server never emits it), and enforces `min(byte_cap.value_or(inf),
// kHardResponseCeilingBytes)` DURING the read loop, never after full buffering (ADR-011 claim C8). A
// 3xx response is returned to the caller exactly as received -- no redirect is ever followed
// (ADR-011 claim C10, §3 Design B).
//
// Milestone 5 Phase C2 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, 004 §1:
// "Cancellation is stop_token, propagated into the HTTP/socket layer"): `stop` is checked before
// connecting and at the top of every read-loop iteration -- a cancellation requested before any I/O
// starts never touches the network at all, and one requested mid-response aborts within one
// `kIoTimeoutMs` tick (or the next received chunk, whichever is sooner) rather than running to
// completion. Additive and backward-compatible: defaults to an unstoppable token, so
// `HostEgressProxy::fetch`'s existing WASM guest-egress call site (ADR-011) is unaffected until it
// chooses to pass a real one.
[[nodiscard]] result<NetEgressResponse> perform_http_exchange(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::optional<std::uint64_t> byte_cap, std::stop_token stop = {});

#ifdef AGENTENGINE_WITH_HTTPS
// decisions/ADR-013-https-egress-tls-client.md: identical to `perform_http_exchange` in every
// respect (same request-building, same byte-cap-enforced read loop, same no-redirect-following
// posture, same additive `stop` cancellation) except the transport -- a `TlsClientSession`
// (tls_client.hpp) wraps the connected socket, verifying the peer certificate against the vendored
// CA bundle and `host_header` (doubling as both the HTTP Host: header and the TLS hostname-
// verification target, exactly how a real HTTPS client uses the same name for both). Only declared
// when AGENTENGINE_WITH_HTTPS is ON -- the scheme gate in `HostEgressProxy::fetch` only reaches this
// call under the same build configuration. This is also the transport
// `sandbox/provider_http_client.hpp` (Milestone 5 Phase C, host-INITIATED ChatClient calls, 004 §3)
// calls directly -- no guest `cap::NetOut` grant to check there, so it bypasses `HostEgressProxy`
// entirely and calls this function itself.
//
// `ca_bundle_pem_override`: forwarded verbatim to `TlsClientSession::handshake` -- empty (the
// default) trusts the real vendored bundle. That function's own comment names this a testability
// seam, never used from production code; the same is true of every caller here.
[[nodiscard]] result<NetEgressResponse> perform_https_exchange(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::optional<std::uint64_t> byte_cap, std::stop_token stop = {},
    std::string_view ca_bundle_pem_override = {});
#endif

// ADR-019: streaming counterparts of the two exchange functions above -- identical in every respect
// (same request building, same byte-cap enforcement, same no-redirect posture, same `stop`
// cancellation) except that body bytes reach `on_body` AS THEY ARRIVE. The returned
// `NetEgressResponse` therefore carries status and headers only; its `body` is always empty.
//
// Separate entry points rather than a nullable sink parameter on the existing ones, because the
// returned value means something different here and a caller who got a silently-empty body would be
// badly served by a shared name.
//
// `on_body` returns false to stop reading early -- the consumer's "I have what I need" signal,
// distinct from cancellation; the response is still returned successfully. It is called on the
// reading thread, so it must not block for long: the read loop is stalled while it runs.
[[nodiscard]] result<NetEgressResponse> perform_http_exchange_streaming(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::function<bool(std::string_view)> const& on_body, std::optional<std::uint64_t> byte_cap,
    std::stop_token stop = {});

#ifdef AGENTENGINE_WITH_HTTPS
[[nodiscard]] result<NetEgressResponse> perform_https_exchange_streaming(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::function<bool(std::string_view)> const& on_body, std::optional<std::uint64_t> byte_cap,
    std::stop_token stop = {}, std::string_view ca_bundle_pem_override = {});
#endif

// ---------------------------------------------------------------------------------------------------------------
// ADR-241 S2: the egress proxy's async path on rt::http.
// ---------------------------------------------------------------------------------------------------------------

// Host-derived bucket labels for egress admission (ADR-241 D10, 8.2). A STRUCTURED triple, never a joined string (a
// joined `tenant + "/" + id` collided tenant `a/b` + root `c` with tenant `a` + root `b/c`). These are values that only
// SELECT which bucket to debit, so they can only narrow what a call may do -- exactly like the stop token (I2-neutral).
// Every component is host-derived, never model-derived (I3): `egress_call_control()` below fills them from the call's
// `EffectContext`. An EMPTY component is not "skip this level": it charges the UNATTRIBUTED bucket of that level
// (R5-1), so a start site nobody listed fails closed into a small shared bucket, not open into the whole pool.
// ae-naming-lint: allow AdmissionKey — ADR-241 §4: new egress vocabulary, 027 §4 row added when the ADR is Judged
struct AdmissionKey {
    std::string tenant;
    std::string principal;  // principal.root_id() (ADR-241 R5-2): a spawned child shares its root principal's bucket
    std::string root_run;   // EffectContext::root_run_id, else run_id
};

// What a conformer may be told besides the request and the grant: VALUES ONLY, none of them authority (I2). A token and
// a time point can only make the callee stop sooner; the admission key only selects a bucket. `EffectContext` (which
// carries `capabilities`) is deliberately NOT passed to a conformer. No default argument anywhere: forgetting to pass
// one is a compile error, not a silent "never cancelled" call.
// ae-naming-lint: allow EgressCallControl — ADR-241 §4: new egress vocabulary, 027 §4 row added when the ADR is Judged
struct EgressCallControl {
    std::stop_token                       stop;        // merged with rt::scope_stop_token() inside `fetch`
    std::chrono::steady_clock::time_point deadline{};  // default (the epoch) = unset
    AdmissionKey                          admission;
};

// The host-set admission limits (ADR-241 8.2): per ROOT RUN 8, per PRINCIPAL 32, global 64, two small UNATTRIBUTED
// buckets, and the request-body cap the gate enforces (D11). Plain atomics on one process-wide object, adjustable at any
// time by HOST code (never model output). A host that uses ONE service principal for everything gets per-run isolation
// under an effective per-principal 32 -- it should raise `per_principal`.
// ae-naming-lint: allow EgressLimits — ADR-241 §8.2: new egress vocabulary, 027 §4 row added when the ADR is Judged
struct EgressLimits {
    std::atomic<std::uint32_t> per_run{8};
    std::atomic<std::uint32_t> per_principal{32};
    std::atomic<std::uint32_t> global{64};
    std::atomic<std::uint32_t> unattributed_run{8};
    std::atomic<std::uint32_t> unattributed_principal{8};
    std::atomic<std::uint64_t> max_request_bytes{16u * 1024u * 1024u};
};
[[nodiscard]] EgressLimits& egress_limits() noexcept;

// The default overall deadline of one egress call (resolve + connect + TLS + body); `<= 0` clamps to it -- there is no
// unbounded mode (ADR-241 D5).
inline constexpr std::chrono::milliseconds kDefaultEgressTotalTimeout{120'000};

// The ONE place an `EffectContext` becomes an `EgressCallControl`: the cancellation token, the deadline hint, and the
// host-derived admission labels (`{tenant, principal.root_id(), root_run_id else run_id}`).
[[nodiscard]] inline EgressCallControl egress_call_control(EffectContext const& ctx) {
    EgressCallControl c;
    c.stop                = ctx.cancellation;
    c.deadline            = ctx.deadline;
    c.admission.tenant    = ctx.principal.tenant_id;
    c.admission.principal = ctx.principal.root_id();
    c.admission.root_run  = ctx.root_run_id.empty() ? ctx.run_id : ctx.root_run_id;
    return c;
}

// concept, not a base class (mirrors `SandboxBackend`, 008 §2a) -- a first-party default ships
// (`HostEgressProxy` below); a deployer's own conforming type (a corporate proxy client, an existing
// SSRF appliance, an mTLS-terminating gateway) is usable with no engine change, per 008 §10 Q3's
// resolution ("stays a seam underneath... for a deployer who wants to substitute").
//
// decisions/ADR-237 §5.1 (Phase D, #173): `fetch` returns `task<result<NetEgressResponse>>` -- a deployer's
// conformer is a proxy client, i.e. network I/O, and may suspend. The backend object must outlive the
// returned task; a conformer that keeps the request or grant past its first suspension takes them by value.
// ADR-241 D1: the third argument is the control value above (no default; the 2-argument form is gone).
template <class T>
// ae-naming-lint: allow NetEgressBackend — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
concept NetEgressBackend = requires(T backend, cap::NetOut const& granted, NetEgressRequest const& req,
                                    EgressCallControl const& control) {
    { T::traits } -> std::convertible_to<NetEgressTraits const&>;
    { backend.fetch(req, granted, control) } -> std::same_as<task<result<NetEgressResponse>>>;
};

// The first-party default `NetEgressBackend` (ADR-241). BOTH entry points are one implementation on `rt::http`:
//   gate (C1, C3, C7, C9, managed-header strip, GET/HEAD/TRACE/OPTIONS body refusal, request-body cap, wire validation)
//   -> admission (per root run / per principal / global, a bounded wait)
//   -> resolve ONCE through the IPv4-only resolver, filter EVERY address through `egress_address_policy`, connect to the
//      verified address, TLS verified against the allowlisted NAME, no redirect, response bytes bounded.
// There is NO seam on this type: no resolver, address policy or CA override can be set on it (ADR-241 5, G15). The
// test-only `TestEgressProxy` (sandbox/testing/net_egress_proxy_testing.hpp) is a separate type for that.
// ae-naming-lint: allow HostEgressProxy — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct HostEgressProxy {
#ifdef AGENTENGINE_WITH_HTTPS
    static constexpr NetEgressTraits traits{"host-egress-proxy", true};
#else
    static constexpr NetEgressTraits traits{"host-egress-proxy", false};
#endif

    // Host-set (never model output): the overall deadline of one call, narrowed by `control.deadline`. `<= 0` clamps to
    // `kDefaultEgressTotalTimeout`; there is no unbounded mode.
    std::chrono::milliseconds total_timeout = kDefaultEgressTotalTimeout;

    // The async entry point. Request, grant and control are taken by value so a task held past this call never reads a
    // dead temporary; `total_timeout` is read now, so the task does not reference this object either.
    [[nodiscard]] task<result<NetEgressResponse>> fetch(NetEgressRequest req, cap::NetOut granted,
                                                        EgressCallControl control) const;

    // The synchronous entry point, for the one caller that cannot suspend: the WASM host import `http-request`
    // (`wasm_backend.cpp`), whose guest is blocked inside the call. It runs the SAME `fetch` as a root on the egress
    // I/O context and waits on a latch (ADR-241 8.3): it parks the calling thread exactly as the old blocking exchange
    // did, but bounded by `min(total_timeout, control.deadline)` and by `control.stop`.
    [[nodiscard]] result<NetEgressResponse> fetch_blocking(NetEgressRequest req, cap::NetOut granted,
                                                           EgressCallControl control) const;
};

static_assert(NetEgressBackend<HostEgressProxy>);

// ADR-011 §9's named residual: reconciles a SandboxSpec-level `ResourceLimits::net_bytes` budget
// (008 §2, `sandbox/sandbox.hpp`) with a grant's own, narrower `cap::NetOut::byte_cap` -- the tighter
// of the two wins, matching 020 §1's "configuration may never widen" rule. A pure, testable function
// rather than logic inlined at each call site (`wasm_backend.cpp`'s `cb_http_request` is the first
// caller): `resource_net_bytes` is `std::nullopt`/0 when no SandboxSpec-level limit applies (the
// grant's own `byte_cap` governs alone, unchanged).
[[nodiscard]] inline cap::NetOut narrow_by_resource_limit(cap::NetOut grant,
                                                           std::optional<std::uint64_t> resource_net_bytes) {
    if (resource_net_bytes.has_value() && *resource_net_bytes > 0) {
        grant.byte_cap = grant.byte_cap.has_value() ? std::min(*grant.byte_cap, *resource_net_bytes)
                                                      : *resource_net_bytes;
    }
    return grant;
}

}  // namespace agentengine::sandbox
