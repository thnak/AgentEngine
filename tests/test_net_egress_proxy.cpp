// Proves decisions/ADR-011-first-party-egress-proxy.md's falsifiable claims (M2 Phase F task F1,
// 008-Sandbox-and-Isolation.md §9 G2 "containment... with a positive control"). Structure follows
// ADR-009's own precedent: pure-function claims (C4/C5) need no live socket at all; pre-network-gate
// claims (C1/C3/C7/C9, "rejected before any network activity") are proven by an injected resolver
// whose call count stays zero; only the genuinely I/O-dependent claims (C2/C8/C10, and G2's positive
// control) use a real loopback TestHttpServer -- every network-reachable address in this test
// environment is itself loopback, which is exactly what production must always block (ADR-011 §3),
// so those tests deliberately call `perform_http_exchange` directly (bypassing the blocklist) or use
// HostEgressProxy's injectable resolver, never a real DNS-resolved public host.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>

#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/pal/net.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::cap::NetOut;
using agentengine::sandbox::HostEgressProxy;
using agentengine::sandbox::is_blocked_address;
using agentengine::sandbox::narrow_by_resource_limit;
using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::perform_http_exchange;
using agentengine::sandbox::reject_crlf;
using agentengine::sandbox::resolve_and_validate;
using agentengine::sandbox::VerifiedEndpoint;

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

bool write_all(agentengine::pal::fd_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto r = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent),
                                        data.size() - sent);
        if (!r) {
            if (r.error() == agentengine::pal::would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return false;  // peer closed early -- fine for a flood/redirect handler, not an error here
        }
        sent += *r;
    }
    return true;
}

// Reads and discards the incoming request until its header terminator (or gives up after a bounded
// number of attempts). Load-bearing, not cosmetic: closing a socket while inbound data is still
// unread in the kernel receive buffer sends a TCP RST instead of a graceful FIN, which surfaces on
// the client as a spurious ECONNRESET on its OWN read of the response -- a real bug this test file
// hit while first exercising perform_http_exchange against a genuinely reachable server, not a
// hypothetical one.
void drain_request(agentengine::pal::fd_t fd) {
    std::string buf;
    std::array<std::byte, 4096> chunk{};
    for (int i = 0; i < 200; ++i) {
        if (buf.find("\r\n\r\n") != std::string::npos) return;
        auto r = agentengine::pal::recv_some(fd, chunk.data(), chunk.size());
        if (!r) {
            if (r.error() == agentengine::pal::would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return;
        }
        if (*r == 0) return;  // peer closed already
        buf.append(reinterpret_cast<char const*>(chunk.data()), *r);
    }
}

// A minimal, single-threaded, one-connection-at-a-time loopback HTTP test double. Never parses the
// incoming request (every test here only cares what the SERVER sends back, or whether it was
// connected to at all) -- `handler` just writes whatever bytes the test scenario needs.
class TestHttpServer {
public:
    explicit TestHttpServer(std::function<void(agentengine::pal::fd_t)> handler) : handler_(std::move(handler)) {
        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~TestHttpServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    TestHttpServer(TestHttpServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int accept_count() const { return accept_count_.load(); }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            accept_count_.fetch_add(1);
            drain_request(*a);
            handler_(*a);
            agentengine::pal::close_fd(*a);
        }
    }

    std::function<void(agentengine::pal::fd_t)> handler_;
    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> accept_count_{0};
    std::jthread thread_;
};

void handle_ok(agentengine::pal::fd_t fd) {
    write_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello");
}

void handle_flood(agentengine::pal::fd_t fd, std::size_t total_bytes) {
    std::string const header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total_bytes) + "\r\n\r\n";
    if (!write_all(fd, header)) return;
    std::string const chunk(65536, 'x');
    std::size_t sent = 0;
    while (sent < total_bytes) {
        std::size_t const n = std::min(chunk.size(), total_bytes - sent);
        if (!write_all(fd, std::string_view(chunk).substr(0, n))) return;
        sent += n;
    }
}

void handle_redirect(agentengine::pal::fd_t fd, std::uint16_t target_port) {
    std::string const body = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(target_port) +
                              "/nope\r\nContent-Length: 0\r\n\r\n";
    write_all(fd, body);
}

NetOut make_grant(std::string host_port_scheme, std::optional<std::uint64_t> byte_cap = std::nullopt,
                   std::vector<std::string> methods = {}) {
    NetOut n;
    n.host_allowlist = {std::move(host_port_scheme)};
    n.byte_cap = byte_cap;
    n.method_restrictions = std::move(methods);
    return n;
}

// A resolver that must never be called -- used to prove a gate rejects BEFORE any resolution is
// attempted (ADR-011 claims C1/C3/C7/C9's "before any network activity").
struct CountingResolver {
    std::atomic<int> calls{0};
    agentengine::result<VerifiedEndpoint> operator()(std::string_view, std::uint16_t) {
        calls.fetch_add(1);
        return VerifiedEndpoint{kLoopbackHostOrder, 0};  // never reached in the tests that use this
    }
};

// C11: NetEgressBackend is structural, not HostEgressProxy-only -- a conforming third-party type
// satisfies it with no engine change. Must be a namespace-scope (not local) type: a local class
// cannot carry the `static constexpr traits` member the concept requires.
struct FakeBackend {
    static constexpr agentengine::sandbox::NetEgressTraits traits{"fake", false};
    agentengine::result<agentengine::sandbox::NetEgressResponse> fetch(NetEgressRequest const&, NetOut const&) const {
        return agentengine::sandbox::NetEgressResponse{};
    }
};
static_assert(agentengine::sandbox::NetEgressBackend<FakeBackend>,
              "a conforming third-party type satisfies NetEgressBackend with no engine change");

}  // namespace

int main() {
    // -- C4/C5: is_blocked_address, and the encoding-bypass mechanism check -- zero network -------
    {
        check(is_blocked_address((127u << 24) | 1u), "loopback 127.0.0.1 is blocked");
        check(is_blocked_address((169u << 24) | (254u << 16) | (169u << 8) | 254u),
              "169.254.169.254 (cloud metadata) is blocked");
        check(is_blocked_address((169u << 24) | (254u << 16)), "169.254.0.0/16 link-local is blocked");
        check(is_blocked_address((10u << 24)), "10.0.0.0/8 is blocked");
        check(is_blocked_address((172u << 24) | (16u << 16)), "172.16.0.0/12 is blocked");
        check(is_blocked_address((172u << 24) | (31u << 16) | (255u << 8) | 255u),
              "172.31.255.255 (top of 172.16.0.0/12) is blocked");
        check(!is_blocked_address((172u << 24) | (32u << 16)), "172.32.0.0 (just past 172.16.0.0/12) is NOT blocked");
        check(is_blocked_address((192u << 24) | (168u << 16)), "192.168.0.0/16 is blocked");
        check(is_blocked_address((100u << 24) | (64u << 16)), "100.64.0.0/10 (CGNAT) is blocked");
        check(is_blocked_address((224u << 24)), "224.0.0.0/4 multicast is blocked");
        check(is_blocked_address((240u << 24)), "240.0.0.0/4 reserved is blocked");
        check(is_blocked_address(0u), "0.0.0.0/8 unspecified is blocked");
        check(!is_blocked_address((8u << 24) | (8u << 16) | (8u << 8) | 8u), "8.8.8.8 is NOT blocked (must not over-block)");
        check(!is_blocked_address((1u << 24) | (1u << 16) | (1u << 8) | 1u), "1.1.1.1 is NOT blocked (must not over-block)");

        // Mechanism check (deterministic, no DNS involved): inet_pton's strict dotted-quad-only
        // grammar is what makes decimal/octal/hex-encoded addresses fail to parse at all, rather
        // than needing to be canonicalized and filtered downstream (ADR-011 claim C5).
        ::in_addr probe{};
        check(::inet_pton(AF_INET, "2130706433", &probe) != 1, "decimal-encoded loopback (2130706433) is NOT accepted by inet_pton");
        check(::inet_pton(AF_INET, "0177.0000.0000.0001", &probe) != 1, "octal-encoded loopback is NOT accepted by inet_pton");
        check(::inet_pton(AF_INET, "0x7f000001", &probe) != 1, "hex-encoded loopback is NOT accepted by inet_pton");
        check(::inet_pton(AF_INET, "127.0.0.1", &probe) == 1, "canonical dotted-quad loopback IS accepted (sanity check)");
    }

    // -- resolve_and_validate: the numeric fast path blocks a literal loopback allowlist entry ----
    {
        auto r = resolve_and_validate("127.0.0.1", 80);
        check(!r.has_value(), "resolve_and_validate rejects a loopback IP literal");
        if (!r) check(r.error().code == "net.address_blocked", "specific diagnostic code");
    }
    {
        auto r = resolve_and_validate("8.8.8.8", 80);
        check(r.has_value(), "resolve_and_validate accepts a non-blocked IP literal via the numeric fast path");
        if (r) check(r->ipv4_host_order == ((8u << 24) | (8u << 16) | (8u << 8) | 8u), "resolved address matches the literal");
    }

    // -- C1: ambiguous grant -- rejected before any network activity ------------------------------
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        NetOut zero_entries;  // host_allowlist starts empty
        auto r0 = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, zero_entries);
        check(!r0.has_value(), "a NetOut grant with zero allowlist entries is rejected");
        if (!r0) check(r0.error().code == "net.ambiguous_grant", "specific diagnostic code (zero entries)");
        check(cr.calls.load() == 0, "resolver was never called for a zero-entry grant");

        NetOut two_entries;
        two_entries.host_allowlist = {"a.example:80:http", "b.example:80:http"};
        auto r2 = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, two_entries);
        check(!r2.has_value(), "a NetOut grant with two allowlist entries is rejected");
        if (!r2) check(r2.error().code == "net.ambiguous_grant", "specific diagnostic code (two entries)");
        check(cr.calls.load() == 0, "resolver was never called for a two-entry grant either");
    }

    // -- C3: https rejected before any network activity -- UNLESS a TLS client is vendored
    // (ADR-013, AGENTENGINE_WITH_HTTPS) -----------------------------------------------------------
#ifndef AGENTENGINE_WITH_HTTPS
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        auto grant = make_grant("api.example:443:https");
        auto r = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, grant);
        check(!r.has_value(), "an https allowlist entry is rejected when no TLS client is vendored");
        if (!r) check(r.error().code == "net.scheme_unsupported", "specific diagnostic code");
        check(cr.calls.load() == 0, "resolver was never called for an https grant");
    }
#else
    // ADR-013's own hostile test corpus (tests/test_https_egress.cpp) exercises the full TLS
    // handshake/certificate-validation surface against a real local TLS test server; this file only
    // needs to prove the scheme gate itself no longer blocks https pre-resolution once a TLS client
    // is vendored.
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        auto grant = make_grant("api.example:443:https");
        auto const r = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, grant);
        (void)r;  // outcome depends on live connect() behavior for port 0 -- only the gate matters here
        check(cr.calls.load() == 1, "the resolver IS called for an https grant once a TLS client is vendored");
    }
#endif

    // -- malformed allowlist entries -- before any network activity ---------------------------------
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        for (auto const& entry : {std::string("nohost-no-colons"), std::string("host:notaport:http"),
                                   std::string("host:80:")}) {
            auto grant = make_grant(entry);
            auto r = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, grant);
            check(!r.has_value(), ("malformed allowlist entry '" + entry + "' is rejected").c_str());
            if (!r) check(r.error().code == "net.malformed_allowlist_entry", "specific diagnostic code");
        }
        check(cr.calls.load() == 0, "resolver was never called for any malformed entry");
    }

    // -- C7: method_restrictions -- before any network activity for the disallowed case -----------
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        auto grant = make_grant("api.example:80:http", std::nullopt, {"GET", "HEAD"});
        auto disallowed = proxy.fetch(NetEgressRequest{"DELETE", "/", {}, ""}, grant);
        check(!disallowed.has_value(), "a method outside method_restrictions is rejected");
        if (!disallowed) check(disallowed.error().code == "net.method_not_allowed", "specific diagnostic code");
        check(cr.calls.load() == 0, "resolver was never called for a disallowed method");

        [[maybe_unused]] auto allowed = proxy.fetch(NetEgressRequest{"get", "/", {}, ""}, grant);  // case-insensitive
        check(cr.calls.load() == 1, "resolver WAS called once the method passed the restriction (case-insensitive match)");
    }
    {
        // empty method_restrictions == unrestricted (cap::NetOut's own documented default).
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        auto grant = make_grant("api.example:80:http");  // no method_restrictions
        [[maybe_unused]] auto r = proxy.fetch(NetEgressRequest{"PATCH", "/", {}, ""}, grant);
        check(cr.calls.load() == 1, "an unrestricted grant lets an arbitrary method reach the resolver");
    }

    // -- C9: CRLF/header-injection gate -- before any network activity ------------------------------
    {
        CountingResolver cr;
        HostEgressProxy proxy;
        proxy.resolver = std::ref(cr);
        auto grant = make_grant("api.example:80:http");

        auto bad_path = proxy.fetch(NetEgressRequest{"GET", "/x HTTP/1.1\r\nX-Injected: evil\r\n\r\nGET /y", {}, ""}, grant);
        check(!bad_path.has_value(), "CRLF in path is rejected");
        if (!bad_path) check(bad_path.error().code == "net.header_injection_rejected", "specific diagnostic code (path)");

        auto bad_method = proxy.fetch(NetEgressRequest{"GET\r\nX-Evil: 1", "/", {}, ""}, grant);
        check(!bad_method.has_value(), "CRLF in method is rejected");

        auto bad_header_value = proxy.fetch(NetEgressRequest{"GET", "/", {{"X-Test", "ok\r\nX-Injected: evil"}}, ""}, grant);
        check(!bad_header_value.has_value(), "CRLF in a header value is rejected");

        auto bad_header_name = proxy.fetch(NetEgressRequest{"GET", "/", {{"X-Test\r\nX-Injected", "evil"}}, ""}, grant);
        check(!bad_header_name.has_value(), "CRLF in a header name is rejected");

        check(cr.calls.load() == 0, "resolver was never called for any CRLF-bearing request");
    }
    check(reject_crlf("field", "clean value").has_value(), "reject_crlf accepts a clean value (positive control)");
    check(!reject_crlf("field", "dirty\r\nvalue").has_value(), "reject_crlf rejects an embedded CRLF");

    // -- live-socket tests: C2 (positive pipeline), G2 positive control, C8 (byte cap), C10 (no ----
    // -- redirect-following). Every reachable address here is loopback -- exactly what production --
    // -- must always block -- so these use HostEgressProxy's injectable resolver or call ------------
    // -- perform_http_exchange directly, never the real (production) resolver against 127.0.0.1. ----
    {
        TestHttpServer server(handle_ok);
        check(server.ok(), "test HTTP server started");
        if (server.ok()) {
            VerifiedEndpoint const ep{kLoopbackHostOrder, server.port()};

            // C2: fetch()'s post-resolution composition -- injected resolver stands in for DNS.
            HostEgressProxy proxy;
            proxy.resolver = [ep](std::string_view, std::uint16_t) -> agentengine::result<VerifiedEndpoint> {
                return ep;
            };
            auto grant = make_grant("test.invalid:1:http");  // host/port irrelevant -- resolver is faked
            auto r = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, grant);
            check(r.has_value(), "fetch() succeeds end-to-end against a real (faked-resolution) target");
            if (r) {
                check(r->status == 200, "status round-trips correctly");
                check(r->body == "hello", "body round-trips correctly");
                bool has_ct = false;
                for (auto const& [k, v] : r->headers) {
                    if (k == "Content-Type") has_ct = true;
                }
                check(has_ct, "headers round-trip correctly");
            }

            // G2 positive control: the SAME server, reached directly (bypassing the blocklist) --
            // proves the target was genuinely reachable, so the next check's rejection is a real
            // block, not an artifact of "nothing was listening."
            auto direct = perform_http_exchange(ep, "test.invalid", NetEgressRequest{"GET", "/", {}, ""}, std::nullopt);
            check(direct.has_value() && direct->status == 200, "positive control: direct connect to the loopback target succeeds");

            // Containment: fetch() with the REAL default resolver against a literal loopback
            // allowlist entry must be blocked, even though the target is demonstrably reachable
            // (proven immediately above).
            HostEgressProxy real_proxy;  // default resolver == resolve_and_validate
            std::string const loop_entry = "127.0.0.1:" + std::to_string(server.port()) + ":http";
            auto loop_grant = make_grant(loop_entry);
            auto blocked = real_proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, loop_grant);
            check(!blocked.has_value(), "fetch() with the real resolver refuses a loopback target");
            if (!blocked) check(blocked.error().code == "net.address_blocked", "specific diagnostic code");
        }
    }

    // -- C8: byte cap aborts mid-stream, both a declared cap and the hard 16 MiB ceiling -----------
    {
        TestHttpServer server([](agentengine::pal::fd_t fd) { handle_flood(fd, 4 * 1024 * 1024); });
        check(server.ok(), "flood test server started");
        if (server.ok()) {
            VerifiedEndpoint const ep{kLoopbackHostOrder, server.port()};
            auto r = perform_http_exchange(ep, "test.invalid", NetEgressRequest{"GET", "/", {}, ""},
                                            /*byte_cap=*/std::uint64_t{4096});
            check(!r.has_value(), "a response exceeding a small declared byte_cap is aborted mid-stream");
            if (!r) check(r.error().code == "net.byte_cap_exceeded", "specific diagnostic code");
        }
    }
    {
        // No declared cap -- the hard 16 MiB host-side ceiling must still apply (CLAUDE.md machine
        // safety: an absent guest-declared cap is not "the host has no protection either").
        TestHttpServer server([](agentengine::pal::fd_t fd) { handle_flood(fd, 17u * 1024u * 1024u); });
        check(server.ok(), "hard-ceiling flood test server started");
        if (server.ok()) {
            VerifiedEndpoint const ep{kLoopbackHostOrder, server.port()};
            auto r = perform_http_exchange(ep, "test.invalid", NetEgressRequest{"GET", "/", {}, ""}, std::nullopt);
            check(!r.has_value(), "a response exceeding the hard ceiling is aborted even with no declared byte_cap");
            if (!r) check(r.error().code == "net.byte_cap_exceeded", "specific diagnostic code");
        }
    }

    // -- C10: a 3xx response is returned as-is; the redirect target is never contacted --------------
    {
        TestHttpServer target(handle_ok);  // must receive ZERO connections
        check(target.ok(), "redirect-target test server started");
        std::uint16_t const target_port = target.port();
        TestHttpServer redirector([target_port](agentengine::pal::fd_t fd) { handle_redirect(fd, target_port); });
        check(redirector.ok(), "redirector test server started");
        if (target.ok() && redirector.ok()) {
            VerifiedEndpoint const ep{kLoopbackHostOrder, redirector.port()};
            HostEgressProxy proxy;
            proxy.resolver = [ep](std::string_view, std::uint16_t) -> agentengine::result<VerifiedEndpoint> {
                return ep;
            };
            auto grant = make_grant("test.invalid:1:http");
            auto r = proxy.fetch(NetEgressRequest{"GET", "/", {}, ""}, grant);
            check(r.has_value(), "a 3xx response is returned successfully, not treated as an error");
            if (r) {
                check(r->status == 302, "the 302 status is returned as-is");
                bool has_location = false;
                for (auto const& [k, v] : r->headers) {
                    if (k == "Location") has_location = true;
                }
                check(has_location, "the Location header is returned as-is");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let any stray connect land
            check(target.accept_count() == 0, "the redirect target received ZERO connections -- no auto-follow");
        }
    }

    // -- C12 (ADR-011 §9 residual, closed): narrow_by_resource_limit() -- the tighter of a grant's
    // own byte_cap and a SandboxSpec-level ResourceLimits::net_bytes always wins ---------------------
    {
        auto const with_grant_cap = make_grant("test.invalid:1:http", std::uint64_t{4096});
        {
            auto narrowed = narrow_by_resource_limit(with_grant_cap, std::nullopt);
            check(narrowed.byte_cap.has_value() && *narrowed.byte_cap == *with_grant_cap.byte_cap,
                  "no ResourceLimits::net_bytes declared -- the grant's own byte_cap is unchanged");
        }
        {
            auto narrowed = narrow_by_resource_limit(with_grant_cap, std::uint64_t{100});
            check(narrowed.byte_cap.has_value() && *narrowed.byte_cap == 100,
                  "ResourceLimits::net_bytes tighter than the grant's byte_cap -- the tighter one wins");
        }
        {
            auto narrowed = narrow_by_resource_limit(with_grant_cap, std::uint64_t{1'000'000});
            check(narrowed.byte_cap.has_value() && *narrowed.byte_cap == *with_grant_cap.byte_cap,
                  "ResourceLimits::net_bytes looser than the grant's byte_cap -- the grant's own cap still wins");
        }
        {
            NetOut no_grant_cap = with_grant_cap;
            no_grant_cap.byte_cap = std::nullopt;
            auto narrowed = narrow_by_resource_limit(no_grant_cap, std::uint64_t{100});
            check(narrowed.byte_cap.has_value() && *narrowed.byte_cap == 100,
                  "grant declares no byte_cap at all -- ResourceLimits::net_bytes alone becomes the cap");
        }
        {
            auto narrowed = narrow_by_resource_limit(with_grant_cap, std::uint64_t{0});
            check(narrowed.byte_cap.has_value() && *narrowed.byte_cap == *with_grant_cap.byte_cap,
                  "ResourceLimits::net_bytes == 0 means \"no SandboxSpec-level limit\", not \"zero bytes allowed\"");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_net_egress_proxy: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_net_egress_proxy: %d FAILURE(S)\n", g_failures);
    return 1;
}
