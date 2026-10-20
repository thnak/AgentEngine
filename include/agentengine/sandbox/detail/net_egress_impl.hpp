#pragma once
// Implements decisions/ADR-241-egress-proxy-async-path-on-rt-http.md §5 (D2 the one gate, D3 the address policy),
// §7 (D5 the deadline), §8.2 (the egress I/O context and admission) and §9 (the error table): the shared egress
// implementation that `HostEgressProxy` (production seams) and `TestEgressProxy` (test seams) both call.
//
// WHO MAY INCLUDE THIS HEADER: only `src/sandbox/`, `include/agentengine/sandbox/testing/` and `tests/`
// (tools/egress_seam_lint.py, ADR-241 G15). These functions take the resolver, the address policy and the CA override
// as PARAMETERS, so they are exactly the seam the production type deliberately does not have -- host code that could
// call `egress_fetch` with a permit-all policy would have a way around the SSRF boundary without naming
// `TestEgressProxy`. The claim ADR-241 5 makes is therefore only "the production TYPE has no seam to flip"; keeping this
// header behind the same lint as the testing header is what makes the functions unreachable from ordinary host code.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/dns.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::sandbox::egress_detail {

// The gate's verdict: where the call goes. `host` is the allowlisted NAME (what TLS verifies against), never an address.
// ae-naming-lint: allow GatedEgressTarget — ADR-241 §5: egress implementation vocabulary, not exported to users
struct GatedEgressTarget {
    std::string   host;
    std::uint16_t port  = 0;
    bool          https = false;
};

// D2: the ONE pure gate, no I/O, in place. In order: C1 (exactly one allowlist entry), the entry's grammar, C3 (scheme;
// https only with AGENTENGINE_WITH_HTTPS), C9 (CR / LF / NUL anywhere in method, path, header names and values), C7
// (method restriction), the request-body cap (`egress_limits().max_request_bytes`), then the managed headers (Host,
// Content-Length, Transfer-Encoding, Connection; any case) are STRIPPED -- the proxy owns framing -- then a body on
// GET/HEAD/TRACE/OPTIONS is refused (the CL.0 desync class), then `rt::http_wire::validate_request` (the same rules
// `rt::http_request` applies; defence in depth). A rejected request has touched nothing: no resolver, no socket.
[[nodiscard]] result<GatedEgressTarget> egress_gate(NetEgressRequest& req, cap::NetOut const& granted);

// D3: IPv4 only. EVERY non-v4 family is refused by family, never decoded (so `::ffff:127.0.0.1`, NAT64, 6to4, Teredo,
// link-local, ULA ... are all refused without a v6 table); a v4 address is refused by the existing blocked-range table.
[[nodiscard]] bool egress_address_policy(pal::IpAddress const& address) noexcept;

// Drops every non-v4 answer of `inner` and reports `not_found` when none is left, so an AAAA-only name stays
// `net.host_unresolvable` (transient), as it was with the blocking resolver.
[[nodiscard]] rt::HostResolver ipv4_only_resolver(rt::HostResolver inner);

// The production resolver: `ipv4_only_resolver(pal::resolve_host_blocking)`.
[[nodiscard]] rt::HostResolver production_host_resolver();

// D5: the overall deadline of one call. `configured <= 0` clamps to `kDefaultEgressTotalTimeout`; a set `deadline`
// narrows it to the time left (at least 1 ms, because 0 would mean "no timer" downstream); nullopt = the deadline has
// already passed (the caller fails `net.deadline_exceeded` before resolving anything).
[[nodiscard]] std::optional<std::chrono::milliseconds> effective_total_timeout(
    std::chrono::milliseconds configured, std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now) noexcept;

// ADR-241 9: `rt::HttpResponse` -> the engine's error, by class and code. Messages never carry the resolved address or
// any topology (R2-13). TLS failures use the TLS client's own class (`HttpResponse::failure`).
[[nodiscard]] error egress_error(rt::HttpResponse const& response);

// The test-only seams (and nothing else) a call is made with. Production passes `production_seams()`.
// ae-naming-lint: allow EgressSeams — ADR-241 §5: egress implementation vocabulary, not exported to users
struct EgressSeams {
    rt::HostResolver host_resolver;
    rt::AddressPolicy address_policy;
    std::string       tls_ca_override;  // empty = the vendored CA bundle
    bool              allow_non_v4 = false;  // a test must opt into v6 by name; otherwise any policy is narrowed to v4
};
[[nodiscard]] EgressSeams production_seams();

// Per-INSTANCE counters a test proxy owns (production passes null): how many times the gate ran, and how many wire bytes
// the client read for the last completed exchange (`rt::HttpResponse::wire_bytes`), so a byte-bound gate counts on the
// client, never at the peer.
// ae-naming-lint: allow GateObserver — ADR-241 §5: egress implementation vocabulary, not exported to users
struct GateObserver {
    std::atomic<int>           gate_calls{0};
    std::atomic<std::uint64_t> last_wire_bytes{0};
};

// Test hooks for the one process-wide I/O context (G13): how many times it was constructed (it must be 0 until a call
// passes the gate, then exactly 1 forever), and an injected construction failure for the next `count` attempts
// (`net.egress_unavailable`, after which the next call retries).
[[nodiscard]] std::uint64_t egress_context_constructions() noexcept;
void                        egress_inject_context_failures(int count) noexcept;

// The shared async implementation both entry points run. By value everywhere: nothing here references the caller.
[[nodiscard]] task<result<NetEgressResponse>> egress_fetch(NetEgressRequest req, cap::NetOut granted,
                                                           EgressCallControl control, EgressSeams seams,
                                                           std::chrono::milliseconds total_timeout,
                                                           GateObserver* observer);

// The synchronous entry: `egress_fetch` as a root on the egress I/O context, waited on with a latch (ADR-241 8.3).
[[nodiscard]] result<NetEgressResponse> egress_fetch_blocking(NetEgressRequest req, cap::NetOut granted,
                                                              EgressCallControl control, EgressSeams seams,
                                                              std::chrono::milliseconds total_timeout,
                                                              GateObserver* observer);

}  // namespace agentengine::sandbox::egress_detail
