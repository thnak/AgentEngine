#pragma once
// Implements 008-Sandbox-and-Isolation.md §10 Q3 and decisions/ADR-011-first-party-egress-proxy.md:
// the host-mediated egress mechanism `NetOut` (007 §3) is enforced through. See that ADR for the
// design rationale (why resolve-once-connect-to-verified-literal, why no redirect-following, why
// plain HTTP only this milestone) and docs/research/2026-08-05-ssrf-dns-rebinding-defense.md for the
// external claims this design rests on.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::sandbox {

// One verified IPv4 endpoint -- the ONLY thing `perform_http_exchange` can connect to. A plain value
// type, never a hostname: there is no call path by which a second, attacker-timed DNS resolution
// could happen between validation and connect (ADR-011 claim C6). IPv4-only, matching the vendored
// PAL's own current locator (`third_party/quark/pal/*/net.hpp`); an IPv6-only host is a resolution
// failure, not a silently narrower blocklist (ADR-011 §3, research note §5).
struct VerifiedEndpoint {
    std::uint32_t ipv4_host_order = 0;
    std::uint16_t port = 0;
};

// A single unit of a plain-HTTP/1.1 request the guest actually controls (WIT `http-request-data`,
// wit/ae-tool.wit) -- host/port/scheme are never here, they come from the granted `cap::NetOut`
// (006's own design choice: "the guest supplies only the part of a call the host must still
// validate per-call... never the grant's own parameters").
struct NetEgressRequest {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct NetEgressResponse {
    std::uint16_t status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct NetEgressTraits {
    std::string_view name;
    bool supports_https = false;  // M2: always false. Flips once a follow-up ADR adds a TLS client.
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

// `\r` or `\n` anywhere in `value` -- this proxy builds the raw HTTP/1.1 request line and header
// block itself (no vetted HTTP library exists to lean on, ADR-011 §2), so this is the only thing
// standing between a guest-controlled `path`/header value and request-splitting/header injection
// onto the wire (ADR-011 claim C9).
[[nodiscard]] result<std::monostate> reject_crlf(std::string_view field_name, std::string_view value);

// Exactly one resolution attempt: an `inet_pton` fast path first (so a numeric-literal allowlist
// host never touches the resolver, and a non-canonical numeric encoding of a blocked address fails
// to parse as a dotted-quad at all rather than needing to be filtered downstream), then
// `getaddrinfo(..., AF_INET, ...)` for a genuine hostname. The first candidate `is_blocked_address`
// does not reject is returned; if every candidate is blocked, or nothing resolves, this fails closed.
[[nodiscard]] result<VerifiedEndpoint> resolve_and_validate(std::string_view host, std::uint16_t port);

// Connects to exactly `endpoint` (never a hostname -- see `VerifiedEndpoint`'s own comment), speaks
// plain HTTP/1.1 (request line + headers + optional body; reads a status line + headers + body back,
// Content-Length-framed only -- no chunked transfer-encoding support this milestone, a documented cut
// since this proxy's own test server never emits it), and enforces `min(byte_cap.value_or(inf),
// kHardResponseCeilingBytes)` DURING the read loop, never after full buffering (ADR-011 claim C8). A
// 3xx response is returned to the caller exactly as received -- no redirect is ever followed
// (ADR-011 claim C10, §3 Design B).
[[nodiscard]] result<NetEgressResponse> perform_http_exchange(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::optional<std::uint64_t> byte_cap);

// concept, not a base class (mirrors `SandboxBackend`, 008 §2a) -- a first-party default ships
// (`HostEgressProxy` below); a deployer's own conforming type (a corporate proxy client, an existing
// SSRF appliance, an mTLS-terminating gateway) is usable with no engine change, per 008 §10 Q3's
// resolution ("stays a seam underneath... for a deployer who wants to substitute").
template <class T>
concept NetEgressBackend = requires(T backend, cap::NetOut const& granted, NetEgressRequest const& req) {
    { T::traits } -> std::convertible_to<NetEgressTraits const&>;
    { backend.fetch(req, granted) } -> std::same_as<result<NetEgressResponse>>;
};

// The first-party default `NetEgressBackend`. `fetch()` composes, in order: single-target-grant gate
// (C1) -> scheme gate (C3) -> CRLF gate (C9) -> method-restriction gate (C7) -> `resolve_and_validate`
// (C4/C5/C6) -> `perform_http_exchange` (C2/C8/C10). See ADR-011 §3 for why each gate runs before any
// network activity.
struct HostEgressProxy {
    static constexpr NetEgressTraits traits{"host-egress-proxy", false};

    // Real by default. A test may inject a fake to prove `fetch()`'s POST-RESOLUTION composition
    // (does it correctly hand `resolve_and_validate`'s answer to `perform_http_exchange` and return
    // a real response) without needing a live target outside every range production must always
    // block -- every address reachable from this project's own test environment is itself loopback
    // or RFC 1918. The fake answers "what does this hostname resolve to," the same question DNS
    // answers; it does not skip `is_blocked_address`, which is proven exhaustively on its own, as a
    // pure function, with zero network dependency. This is a testability seam, not a security
    // bypass: production code never constructs a `HostEgressProxy` with a non-default resolver.
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> resolver = resolve_and_validate;

    [[nodiscard]] result<NetEgressResponse> fetch(NetEgressRequest const& req, cap::NetOut const& granted) const;
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
