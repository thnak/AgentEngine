// Implements net_egress_proxy.hpp. See decisions/ADR-011-first-party-egress-proxy.md and
// docs/research/2026-08-05-ssrf-dns-rebinding-defense.md for the design/citations this satisfies.

#include "agentengine/sandbox/net_egress_proxy.hpp"

#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/sandbox/tls_client.hpp"
#endif

#include "agentengine/pal/net.hpp"

#if defined(_WIN32)
// winsock2.h/ws2tcpip.h already pulled in transitively by pal/windows_x86_64/net.hpp (getaddrinfo,
// freeaddrinfo, inet_pton, select, FD_SET/FD_ZERO all live there on this platform).
#else
#include <netdb.h>     // getaddrinfo/freeaddrinfo/addrinfo -- not in pal/linux_x86_64/net.hpp
#include <sys/select.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>

namespace agentengine::sandbox {

namespace {

// -- blocked-range table (ADR-011 claim C4) -------------------------------------------------------

struct Cidr4 {
    std::uint32_t base;
    std::uint32_t mask;
};

constexpr std::uint32_t make_mask(int prefix) noexcept {
    return prefix <= 0 ? 0u : (prefix >= 32 ? 0xFFFF'FFFFu : static_cast<std::uint32_t>(~0u << (32 - prefix)));
}
constexpr std::uint32_t make_ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
}

constexpr Cidr4 kBlockedRanges[] = {
    {make_ipv4(127, 0, 0, 0), make_mask(8)},     // loopback
    {make_ipv4(169, 254, 0, 0), make_mask(16)},  // link-local -- contains 169.254.169.254 (metadata)
    {make_ipv4(10, 0, 0, 0), make_mask(8)},      // RFC 1918
    {make_ipv4(172, 16, 0, 0), make_mask(12)},   // RFC 1918
    {make_ipv4(192, 168, 0, 0), make_mask(16)},  // RFC 1918
    {make_ipv4(100, 64, 0, 0), make_mask(10)},   // CGNAT (RFC 6598)
    {make_ipv4(224, 0, 0, 0), make_mask(4)},     // multicast
    {make_ipv4(240, 0, 0, 0), make_mask(4)},     // reserved
    {make_ipv4(0, 0, 0, 0), make_mask(8)},       // "this network" / unspecified
};

bool equals_ci(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

constexpr int kIoTimeoutMs = 10'000;

bool wait_ready(agentengine::pal::fd_t fd, bool for_write, int timeout_ms) {
    ::fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    ::timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int const nfds = static_cast<int>(fd) + 1;
    int const rc = for_write ? ::select(nfds, nullptr, &set, nullptr, &tv)
                              : ::select(nfds, &set, nullptr, nullptr, &tv);
    return rc > 0;
}

struct FdGuard {
    agentengine::pal::fd_t fd;
    ~FdGuard() { agentengine::pal::close_fd(static_cast<int>(fd)); }
};

// Milestone 5 Phase C2 (net_egress_proxy.hpp's own comment on `perform_http_exchange`/
// `perform_https_exchange`'s new `stop` parameter): the one check both read loops share -- a plain
// error, not an exception (CONVENTIONS.md's no-exceptions-for-control-flow rule).
// `quark::errc::cancelled`'s own comment ("std::stop_token fired") is the established meaning for
// this failure_class already; reused here rather than inventing a second cancellation vocabulary.
result<std::monostate> check_not_cancelled(std::stop_token const& stop) {
    if (stop.stop_requested()) {
        return std::unexpected(error{failure_class::transient, "cancelled via stop_token", "net.cancelled"});
    }
    return std::monostate{};
}

result<std::monostate> send_all(agentengine::pal::fd_t fd, std::string const& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (!wait_ready(fd, /*for_write=*/true, kIoTimeoutMs)) {
            return std::unexpected(error{failure_class::transient, "timed out sending request", "net.connect_failed"});
        }
        auto r = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent),
                                        data.size() - sent);
        if (!r) {
            if (r.error() == agentengine::pal::would_block()) continue;
            return std::unexpected(error{failure_class::transient, "send failed", "net.connect_failed"});
        }
        sent += *r;
    }
    return std::monostate{};
}

std::optional<std::size_t> parse_content_length(std::string_view head) {
    std::size_t pos = 0;
    while (pos < head.size()) {
        auto const next = head.find("\r\n", pos);
        std::string_view const line = next == std::string_view::npos ? head.substr(pos) : head.substr(pos, next - pos);
        auto const colon = line.find(':');
        if (colon != std::string_view::npos && equals_ci(line.substr(0, colon), "content-length")) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            std::size_t n = 0;
            auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec == std::errc{}) return n;
        }
        if (next == std::string_view::npos) break;
        pos = next + 2;
    }
    return std::nullopt;
}

bool header_value_contains_token_ci(std::string_view value, std::string_view lowercase_token) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find(lowercase_token) != std::string::npos;
}

// ADR-011's own documented cut ("chunked transfer-encoding is not supported on the response side")
// undersold the actual risk: a server that speaks `Transfer-Encoding: chunked` with no
// `Content-Length` still terminates the read loop above cleanly via peer-close (every request this
// client sends carries `Connection: close`), so the raw chunk-size lines and terminators were
// landing in `resp.body` verbatim instead of timing out -- a silently corrupted body, not the
// documented "fails to terminate" failure mode. Dechunks for real rather than merely detecting the
// case and erroring, so a chunked-only peer (confirmed reachable via the official MCP conformance
// suite's mock server, docs/research/2026-08-15-mcp-conformance-harness.md) now round-trips
// correctly through this non-streaming path. NOT applied to `perform_http_exchange_streaming`'s own
// SSE path, which relays the body incrementally and never buffers it here -- see
// dechunk_response_body_if_needed()'s own comment for why calling this unconditionally from
// parse_http_response() broke that path on the first attempt.
result<std::string> dechunk_body(std::string_view chunked) {
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        auto const line_end = chunked.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            return std::unexpected(error{failure_class::transient,
                                          "malformed chunked body (missing chunk-size line terminator)",
                                          "net.protocol_error"});
        }
        std::string_view size_line = chunked.substr(pos, line_end - pos);
        auto const semi = size_line.find(';');  // chunk extensions, if any, are discarded unread
        if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);
        std::size_t chunk_size = 0;
        auto const [ptr, ec] =
            std::from_chars(size_line.data(), size_line.data() + size_line.size(), chunk_size, /*base=*/16);
        if (ec != std::errc{} || ptr != size_line.data() + size_line.size()) {
            return std::unexpected(error{failure_class::transient, "malformed chunked body (invalid chunk-size)",
                                          "net.protocol_error"});
        }
        pos = line_end + 2;
        if (chunk_size == 0) break;  // terminal chunk -- any trailer fields are discarded, not surfaced
        if (chunk_size > chunked.size() - pos || pos + chunk_size + 2 > chunked.size()) {
            return std::unexpected(error{failure_class::transient, "malformed chunked body (truncated chunk data)",
                                          "net.protocol_error"});
        }
        out.append(chunked.data() + pos, chunk_size);
        pos += chunk_size;
        if (chunked.compare(pos, 2, "\r\n") != 0) {
            return std::unexpected(error{failure_class::transient,
                                          "malformed chunked body (missing chunk-data terminator)",
                                          "net.protocol_error"});
        }
        pos += 2;
    }
    return out;
}

result<NetEgressResponse> parse_http_response(std::string const& buf) {
    auto const header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(error{failure_class::transient, "malformed HTTP response (no header terminator)", "net.protocol_error"});
    }
    std::string_view const head(buf.data(), header_end);
    auto const line_end = head.find("\r\n");
    std::string_view const status_line = line_end == std::string_view::npos ? head : head.substr(0, line_end);

    auto const sp1 = status_line.find(' ');
    if (sp1 == std::string_view::npos) {
        return std::unexpected(error{failure_class::transient, "malformed HTTP status line", "net.protocol_error"});
    }
    std::string_view rest = status_line.substr(sp1 + 1);
    auto const sp2 = rest.find(' ');
    std::string_view const code_sv = sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);
    std::uint16_t status = 0;
    auto const [ptr, ec] = std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status);
    if (ec != std::errc{}) {
        return std::unexpected(error{failure_class::transient, "malformed HTTP status code", "net.protocol_error"});
    }

    NetEgressResponse resp;
    resp.status = status;

    std::size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        auto const next = head.find("\r\n", pos);
        std::string_view const line = next == std::string_view::npos ? head.substr(pos) : head.substr(pos, next - pos);
        auto const colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = line.substr(0, colon);
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            resp.headers.emplace_back(std::string(name), std::string(value));
        }
        if (next == std::string_view::npos) break;
        pos = next + 2;
    }
    resp.body = buf.substr(header_end + 4);
    return resp;
}

// Dechunks `resp.body` in place when `resp` carries `Transfer-Encoding: chunked` -- split out of
// parse_http_response() itself because that function is also used by stream_response_body() to parse
// ONLY the header block (a deliberately body-less call: the streaming path relays the body to its own
// caller incrementally and never buffers it here), so dechunking unconditionally inside
// parse_http_response() dechunks an always-empty string on that path and fails closed, breaking every
// chunked SSE response. Only the two full-buffer exchange functions below, which have the complete
// body in hand by construction (their read loop doesn't exit until it does), call this.
result<std::monostate> dechunk_response_body_if_needed(NetEgressResponse& resp) {
    bool is_chunked = false;
    for (auto const& [name, value] : resp.headers) {
        if (equals_ci(name, "transfer-encoding") && header_value_contains_token_ci(value, "chunked")) {
            is_chunked = true;
            break;
        }
    }
    if (!is_chunked) return std::monostate{};
    auto dechunked = dechunk_body(resp.body);
    if (!dechunked) return std::unexpected(dechunked.error());
    resp.body = std::move(*dechunked);
    return std::monostate{};
}

struct AllowlistTarget {
    std::string host;
    std::uint16_t port;
    std::string scheme;
};

result<AllowlistTarget> parse_allowlist_entry(std::string_view entry) {
    auto const malformed = [] {
        return std::unexpected(error{failure_class::contract, "malformed NetOut allowlist entry (expected host:port:scheme)", "net.malformed_allowlist_entry"});
    };
    auto const scheme_sep = entry.rfind(':');
    if (scheme_sep == std::string_view::npos) return malformed();
    std::string_view const scheme = entry.substr(scheme_sep + 1);
    std::string_view const rest = entry.substr(0, scheme_sep);
    auto const port_sep = rest.rfind(':');
    if (port_sep == std::string_view::npos) return malformed();
    std::string_view const port_sv = rest.substr(port_sep + 1);
    std::string_view const host = rest.substr(0, port_sep);
    if (host.empty() || port_sv.empty() || scheme.empty()) return malformed();
    std::uint16_t port = 0;
    auto const [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), port);
    if (ec != std::errc{} || ptr != port_sv.data() + port_sv.size()) return malformed();
    return AllowlistTarget{std::string(host), port, std::string(scheme)};
}

}  // namespace

bool is_blocked_address(std::uint32_t ipv4_host_order) noexcept {
    for (auto const& r : kBlockedRanges) {
        if ((ipv4_host_order & r.mask) == (r.base & r.mask)) return true;
    }
    return false;
}

result<std::monostate> reject_crlf(std::string_view field_name, std::string_view value) {
    if (value.find('\r') != std::string_view::npos || value.find('\n') != std::string_view::npos) {
        return std::unexpected(error{failure_class::contract,
                                      "value for '" + std::string(field_name) + "' contains a CR or LF byte",
                                      "net.header_injection_rejected"});
    }
    return std::monostate{};
}

namespace {

// The resolution half, shared by `resolve_host` and `resolve_and_validate` so the two cannot drift:
// exactly one resolution attempt, IPv4 only, candidates returned in `getaddrinfo` order. Applies NO
// address policy of its own -- filtering is the caller's, which is the whole point of splitting it
// out (ADR-016: the two callers have genuinely different threat models).
//
// `literal` reports whether the answer came from the `inet_pton` fast path rather than the resolver.
// Only the error WORDING depends on it; both callers keep their own pre-existing messages verbatim.
struct ResolvedCandidates {
    std::vector<std::uint32_t> addresses;
    bool literal = false;
};

result<ResolvedCandidates> resolve_candidates(std::string const& host_str) {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif
    // Fast path: a numeric IPv4 literal never touches the resolver. inet_pton's strict, dotted-quad-
    // only grammar (unlike the legacy, lenient inet_addr/gethostbyname) is what makes a decimal/
    // octal/hex-encoded address fail to parse here rather than needing to be filtered downstream
    // (ADR-011 claim C5; docs/research/2026-08-05-ssrf-dns-rebinding-defense.md §4).
    ::in_addr direct{};
    if (::inet_pton(AF_INET, host_str.c_str(), &direct) == 1) {
        return ResolvedCandidates{{::ntohl(direct.s_addr)}, /*literal=*/true};
    }

    ::addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* results = nullptr;
    int const rc = ::getaddrinfo(host_str.c_str(), nullptr, &hints, &results);
    if (rc != 0 || results == nullptr) {
        return std::unexpected(error{failure_class::transient, "could not resolve an IPv4 address for this host", "net.host_unresolvable"});
    }
    ResolvedCandidates out;
    for (::addrinfo* p = results; p != nullptr; p = p->ai_next) {
        auto const* sin = reinterpret_cast<::sockaddr_in const*>(p->ai_addr);
        out.addresses.push_back(::ntohl(sin->sin_addr.s_addr));
    }
    ::freeaddrinfo(results);
    if (out.addresses.empty()) {
        return std::unexpected(error{failure_class::transient, "could not resolve an IPv4 address for this host", "net.host_unresolvable"});
    }
    return out;
}

}  // namespace

result<VerifiedEndpoint> resolve_host(std::string_view host, std::uint16_t port) {
    auto candidates = resolve_candidates(std::string(host));
    if (!candidates) return std::unexpected(candidates.error());
    return VerifiedEndpoint{candidates->addresses.front(), port};
}

result<VerifiedEndpoint> resolve_and_validate(std::string_view host, std::uint16_t port) {
    auto candidates = resolve_candidates(std::string(host));
    if (!candidates) return std::unexpected(candidates.error());
    for (std::uint32_t const addr : candidates->addresses) {
        if (!is_blocked_address(addr)) return VerifiedEndpoint{addr, port};
    }
    return std::unexpected(error{failure_class::policy,
                                  candidates->literal ? "address is in a blocked range"
                                                       : "every resolved address is in a blocked range",
                                  "net.address_blocked"});
}

// Shared between perform_http_exchange and perform_https_exchange (ADR-013) -- the raw HTTP/1.1
// request text is identical either way; only the transport it travels over differs. CRLF in
// method/path/headers is rejected by the caller's gate (HostEgressProxy::fetch) before either
// exchange function is ever reached (ADR-011 claim C9) -- this is the only place those bytes reach
// the wire, so that gate running first is load-bearing, not a formality.
std::string build_raw_request(std::string_view host_header, NetEgressRequest const& req) {
    std::string request;
    request += req.method;
    request += ' ';
    request += req.path;
    request += " HTTP/1.1\r\n";
    request += "Host: ";
    request += host_header;
    request += "\r\n";
    bool has_content_length = false;
    for (auto const& [k, v] : req.headers) {
        request += k;
        request += ": ";
        request += v;
        request += "\r\n";
        if (equals_ci(k, "content-length")) has_content_length = true;
    }
    if (!req.body.empty() && !has_content_length) {
        request += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    request += req.body;
    return request;
}

result<NetEgressResponse> perform_http_exchange(VerifiedEndpoint endpoint, std::string_view host_header,
                                                 NetEgressRequest const& req,
                                                 std::optional<std::uint64_t> byte_cap,
                                                 std::stop_token stop) {
    std::uint64_t const effective_cap = std::min<std::uint64_t>(byte_cap.value_or(kHardResponseCeilingBytes),
                                                                  kHardResponseCeilingBytes);
    if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    auto connect_r = agentengine::pal::tcp_connect(endpoint.ipv4_host_order, endpoint.port);
    if (!connect_r) {
        return std::unexpected(error{failure_class::transient, "connect failed", "net.connect_failed"});
    }
    FdGuard const guard{*connect_r};

    if (!wait_ready(guard.fd, /*for_write=*/true, kIoTimeoutMs)) {
        return std::unexpected(error{failure_class::transient, "connect timed out", "net.connect_failed"});
    }
    if (auto const cr = agentengine::pal::connect_result(guard.fd); !cr) {
        return std::unexpected(error{failure_class::transient, "connect refused or failed", "net.connect_failed"});
    }

    std::string const request = build_raw_request(host_header, req);
    if (auto const s = send_all(guard.fd, request); !s) return std::unexpected(s.error());

    // Read the response, enforcing effective_cap DURING the loop -- never buffer past it and check
    // afterward (ADR-011 claim C8; a "buffer everything then truncate" implementation would still
    // transiently over-allocate, which is exactly the host-memory-DoS property this guards against).
    std::string buf;
    std::array<std::byte, 4096> chunk{};
    for (;;) {
        auto const header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            auto const cl = parse_content_length(std::string_view(buf).substr(0, header_end));
            if (cl.has_value()) {
                std::size_t const body_have = buf.size() - (header_end + 4);
                if (body_have >= *cl) break;
            }
            // No Content-Length: keep reading until the peer closes (Connection: close was sent).
        }
        if (buf.size() >= effective_cap) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap", "net.byte_cap_exceeded"});
        }
        if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
        if (!wait_ready(guard.fd, /*for_write=*/false, kIoTimeoutMs)) {
            return std::unexpected(error{failure_class::transient, "timed out reading response", "net.connect_failed"});
        }
        auto const r = agentengine::pal::recv_some(guard.fd, chunk.data(), chunk.size());
        if (!r) {
            if (r.error() == agentengine::pal::would_block()) continue;
            return std::unexpected(error{failure_class::transient, "recv failed", "net.connect_failed"});
        }
        if (*r == 0) break;  // peer closed -- normal end of a Connection:-close response
        std::size_t const room = effective_cap > buf.size() ? effective_cap - buf.size() : 0;
        std::size_t const take = std::min(room, *r);
        buf.append(reinterpret_cast<char const*>(chunk.data()), take);
        if (take < *r) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap", "net.byte_cap_exceeded"});
        }
    }

    auto parsed = parse_http_response(buf);
    if (!parsed) return parsed;
    if (auto const d = dechunk_response_body_if_needed(*parsed); !d) return std::unexpected(d.error());
    return parsed;
}

#ifdef AGENTENGINE_WITH_HTTPS
// decisions/ADR-013-https-egress-tls-client.md. Identical structure to perform_http_exchange above
// (connect -> build the same raw request -> byte-cap-enforced read loop -> parse) except the
// transport: a TlsClientSession wraps the socket immediately after connect, and the read loop calls
// its `recv()` instead of raw `wait_ready`+`recv_some` -- the session already blocks/retries
// internally on its own BIO (tls_client.cpp), so there is no `would_block` case to handle here the
// way the plain-HTTP loop above has to.
result<NetEgressResponse> perform_https_exchange(VerifiedEndpoint endpoint, std::string_view host_header,
                                                  NetEgressRequest const& req,
                                                  std::optional<std::uint64_t> byte_cap,
                                                  std::stop_token stop,
                                                  std::string_view ca_bundle_pem_override) {
    std::uint64_t const effective_cap = std::min<std::uint64_t>(byte_cap.value_or(kHardResponseCeilingBytes),
                                                                  kHardResponseCeilingBytes);
    if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    auto connect_r = agentengine::pal::tcp_connect(endpoint.ipv4_host_order, endpoint.port);
    if (!connect_r) {
        return std::unexpected(error{failure_class::transient, "connect failed", "net.connect_failed"});
    }
    FdGuard const guard{*connect_r};

    if (!wait_ready(guard.fd, /*for_write=*/true, kIoTimeoutMs)) {
        return std::unexpected(error{failure_class::transient, "connect timed out", "net.connect_failed"});
    }
    if (auto const cr = agentengine::pal::connect_result(guard.fd); !cr) {
        return std::unexpected(error{failure_class::transient, "connect refused or failed", "net.connect_failed"});
    }

    auto session_r = TlsClientSession::handshake(guard.fd, host_header, ca_bundle_pem_override);
    if (!session_r) return std::unexpected(session_r.error());
    TlsClientSession session = std::move(*session_r);

    std::string const request = build_raw_request(host_header, req);
    if (auto const s = session.send(request); !s) return std::unexpected(s.error());

    std::string buf;
    std::array<char, 4096> chunk{};
    for (;;) {
        auto const header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            auto const cl = parse_content_length(std::string_view(buf).substr(0, header_end));
            if (cl.has_value()) {
                std::size_t const body_have = buf.size() - (header_end + 4);
                if (body_have >= *cl) break;
            }
        }
        if (buf.size() >= effective_cap) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap", "net.byte_cap_exceeded"});
        }
        if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
        auto const r = session.recv(chunk.data(), chunk.size());
        if (!r) return std::unexpected(r.error());
        if (*r == 0) break;  // clean TLS close -- normal end of a Connection:-close response
        std::size_t const room = effective_cap > buf.size() ? effective_cap - buf.size() : 0;
        std::size_t const take = std::min(room, *r);
        buf.append(chunk.data(), take);
        if (take < *r) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap", "net.byte_cap_exceeded"});
        }
    }

    auto parsed = parse_http_response(buf);
    if (!parsed) return parsed;
    if (auto const d = dechunk_response_body_if_needed(*parsed); !d) return std::unexpected(d.error());
    return parsed;
}
#endif

// ADR-019: the streaming counterparts of the two exchange functions above. Identical in every
// respect -- same request building, same byte-cap enforcement, same no-redirect posture, same
// stop_token cancellation -- except that body bytes are handed to `on_body` AS THEY ARRIVE instead
// of being accumulated into `NetEgressResponse::body`, which comes back empty.
//
// Deliberately separate entry points rather than an `on_body`-or-null parameter on the existing
// ones: the returned `NetEgressResponse` means something different here (status and headers only),
// and a caller that silently got an empty body because it passed a sink it had forgotten about
// would be a nasty failure mode. Two names, two contracts.
//
// `on_body` returning false stops the read promptly -- the consumer's own "I have everything I
// need" signal, distinct from cancellation. The response is still returned successfully.
namespace {

// Shared by both transports: everything after the response head is identical, so only the recv step
// differs. `recv_more(buf, len)` returns bytes read, 0 for a clean peer close, or an error.
template <class RecvFn>
result<NetEgressResponse> stream_response_body(RecvFn&& recv_more, std::uint64_t effective_cap,
                                                std::stop_token const& stop,
                                                std::function<bool(std::string_view)> const& on_body) {
    std::string head;
    std::uint64_t body_bytes = 0;
    std::size_t header_end = std::string::npos;
    std::array<char, 4096> chunk{};

    // Phase 1: read until the response head is complete. Anything past the head in that same read is
    // already the first body fragment and goes straight to the sink -- never held back waiting for
    // more, which is the whole point of this function.
    for (;;) {
        header_end = head.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (head.size() >= effective_cap) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap",
                                          "net.byte_cap_exceeded"});
        }
        if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
        auto const r = recv_more(chunk.data(), chunk.size());
        if (!r) return std::unexpected(r.error());
        if (*r == 0) break;  // peer closed before a complete head
        head.append(chunk.data(), *r);
    }
    if (header_end == std::string::npos) {
        return std::unexpected(error{failure_class::transient, "response head was truncated",
                                      "net.protocol_error"});
    }

    auto parsed = parse_http_response(head.substr(0, header_end + 4));
    if (!parsed) return std::unexpected(parsed.error());

    auto const declared = parse_content_length(std::string_view(head).substr(0, header_end));
    if (std::size_t const carried = head.size() - (header_end + 4); carried > 0) {
        body_bytes += carried;
        if (!on_body(std::string_view(head).substr(header_end + 4))) return parsed;
    }

    // Phase 2: relay the rest. Exit on the declared Content-Length when there is one, else on the
    // peer's close (the raw-request builder always sends `Connection: close`, so that is reliable --
    // and it is the only available terminator for a `Transfer-Encoding: chunked` SSE response, whose
    // own terminal 0-chunk is the CALLER's to notice, not this layer's).
    for (;;) {
        if (declared.has_value() && body_bytes >= *declared) break;
        if (body_bytes >= effective_cap) {
            return std::unexpected(error{failure_class::resource, "response exceeded the byte cap",
                                          "net.byte_cap_exceeded"});
        }
        if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
        auto const r = recv_more(chunk.data(), chunk.size());
        if (!r) return std::unexpected(r.error());
        if (*r == 0) break;
        body_bytes += *r;
        if (!on_body(std::string_view(chunk.data(), *r))) break;
    }
    return parsed;
}

}  // namespace

result<NetEgressResponse> perform_http_exchange_streaming(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::function<bool(std::string_view)> const& on_body, std::optional<std::uint64_t> byte_cap,
    std::stop_token stop) {
    std::uint64_t const effective_cap =
        std::min<std::uint64_t>(byte_cap.value_or(kHardResponseCeilingBytes), kHardResponseCeilingBytes);
    if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif
    auto connect_r = agentengine::pal::tcp_connect(endpoint.ipv4_host_order, endpoint.port);
    if (!connect_r) {
        return std::unexpected(error{failure_class::transient, "connect failed", "net.connect_failed"});
    }
    FdGuard const guard{*connect_r};
    if (!wait_ready(guard.fd, /*for_write=*/true, kIoTimeoutMs)) {
        return std::unexpected(error{failure_class::transient, "connect timed out", "net.connect_failed"});
    }
    if (auto const cr = agentengine::pal::connect_result(guard.fd); !cr) {
        return std::unexpected(
            error{failure_class::transient, "connect refused or failed", "net.connect_failed"});
    }
    std::string const request = build_raw_request(host_header, req);
    if (auto const s = send_all(guard.fd, request); !s) return std::unexpected(s.error());

    auto recv_more = [&](char* buf, std::size_t len) -> result<std::size_t> {
        for (;;) {
            if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
            if (!wait_ready(guard.fd, /*for_write=*/false, kIoTimeoutMs)) {
                return std::unexpected(
                    error{failure_class::transient, "timed out reading response", "net.connect_failed"});
            }
            auto const r = agentengine::pal::recv_some(guard.fd, reinterpret_cast<std::byte*>(buf), len);
            if (!r) {
                if (r.error() == agentengine::pal::would_block()) continue;
                return std::unexpected(error{failure_class::transient, "recv failed", "net.connect_failed"});
            }
            return *r;
        }
    };
    return stream_response_body(recv_more, effective_cap, stop, on_body);
}

#ifdef AGENTENGINE_WITH_HTTPS
result<NetEgressResponse> perform_https_exchange_streaming(
    VerifiedEndpoint endpoint, std::string_view host_header, NetEgressRequest const& req,
    std::function<bool(std::string_view)> const& on_body, std::optional<std::uint64_t> byte_cap,
    std::stop_token stop, std::string_view ca_bundle_pem_override) {
    std::uint64_t const effective_cap =
        std::min<std::uint64_t>(byte_cap.value_or(kHardResponseCeilingBytes), kHardResponseCeilingBytes);
    if (auto const c = check_not_cancelled(stop); !c) return std::unexpected(c.error());
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif
    auto connect_r = agentengine::pal::tcp_connect(endpoint.ipv4_host_order, endpoint.port);
    if (!connect_r) {
        return std::unexpected(error{failure_class::transient, "connect failed", "net.connect_failed"});
    }
    FdGuard const guard{*connect_r};
    if (!wait_ready(guard.fd, /*for_write=*/true, kIoTimeoutMs)) {
        return std::unexpected(error{failure_class::transient, "connect timed out", "net.connect_failed"});
    }
    if (auto const cr = agentengine::pal::connect_result(guard.fd); !cr) {
        return std::unexpected(
            error{failure_class::transient, "connect refused or failed", "net.connect_failed"});
    }
    auto session_r = TlsClientSession::handshake(guard.fd, host_header, ca_bundle_pem_override);
    if (!session_r) return std::unexpected(session_r.error());
    TlsClientSession session = std::move(*session_r);

    std::string const request = build_raw_request(host_header, req);
    if (auto const s = session.send(request); !s) return std::unexpected(s.error());

    auto recv_more = [&](char* buf, std::size_t len) -> result<std::size_t> {
        return session.recv(buf, len);
    };
    return stream_response_body(recv_more, effective_cap, stop, on_body);
}
#endif

result<NetEgressResponse> HostEgressProxy::fetch(NetEgressRequest const& req, cap::NetOut const& granted) const {
    // C1: the WIT `http-request` call gives the guest no way to name a host at all -- host/port/
    // scheme come entirely from the grant (see this header's own file-top comment) -- so a grant
    // with anything other than exactly one allowlist entry is genuinely ambiguous, not resolvable by
    // picking [0] silently.
    if (granted.host_allowlist.size() != 1) {
        return std::unexpected(error{failure_class::policy,
                                      "http-request needs a NetOut grant with exactly one allowlist entry",
                                      "net.ambiguous_grant"});
    }
    auto const target = parse_allowlist_entry(granted.host_allowlist.front());
    if (!target) return std::unexpected(target.error());

    // C3: http always; https only when AGENTENGINE_WITH_HTTPS vendors a TLS client (ADR-013). Off by
    // default (CONVENTIONS.md tier 2: never linked into a build that does not select this backend)
    // -- ADR-011 section 3 Design C's original "no TLS client exists" reasoning for rejecting https
    // no longer holds once this option is on, but the option itself still defaults off.
    bool const is_https = equals_ci(target->scheme, "https");
    if (!equals_ci(target->scheme, "http") && !is_https) {
        return std::unexpected(error{failure_class::policy,
                                      "only http/https schemes are supported (see ADR-011 section 3, ADR-013)",
                                      "net.scheme_unsupported"});
    }
#ifndef AGENTENGINE_WITH_HTTPS
    if (is_https) {
        return std::unexpected(error{failure_class::policy,
                                      "https requested but this build has no TLS client vendored (ADR-013, "
                                      "AGENTENGINE_WITH_HTTPS is off)",
                                      "net.scheme_unsupported"});
    }
#endif

    // C9: reject before any network activity -- this proxy builds the raw request itself.
    if (auto const c = reject_crlf("method", req.method); !c) return std::unexpected(c.error());
    if (auto const c = reject_crlf("path", req.path); !c) return std::unexpected(c.error());
    for (auto const& [k, v] : req.headers) {
        if (auto const c = reject_crlf("header name", k); !c) return std::unexpected(c.error());
        if (auto const c = reject_crlf("header value", v); !c) return std::unexpected(c.error());
    }

    // C7: method_restrictions empty == unrestricted (cap::NetOut's own documented default).
    if (!granted.method_restrictions.empty()) {
        bool const allowed = std::any_of(granted.method_restrictions.begin(), granted.method_restrictions.end(),
                                          [&](std::string const& m) { return equals_ci(m, req.method); });
        if (!allowed) {
            return std::unexpected(error{failure_class::policy, "method not permitted by this grant", "net.method_not_allowed"});
        }
    }

    auto const endpoint = resolver(target->host, target->port);
    if (!endpoint) return std::unexpected(endpoint.error());

#ifdef AGENTENGINE_WITH_HTTPS
    if (is_https) return perform_https_exchange(*endpoint, target->host, req, granted.byte_cap);
#endif
    return perform_http_exchange(*endpoint, target->host, req, granted.byte_cap);
}

}  // namespace agentengine::sandbox
