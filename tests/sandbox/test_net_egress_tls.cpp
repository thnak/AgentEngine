// Proves decisions/archive/ADR-241-egress-proxy-async-path-on-rt-http.md SS11 G11 (TLS on the egress boundary), through BOTH entry
// points (`fetch` and `fetch_blocking`), against a real loopback mbedTLS peer with an in-memory CA
// (tests/support/https_test_peer.hpp, shared with tests/rt/test_rt_https.cpp). Only built with AGENTENGINE_WITH_HTTPS.
//
//   T1  Positive control: the allowlisted name `good.example` resolves (fake DNS) to the loopback peer whose certificate
//       is for `good.example`: 200, the peer saw `Host: good.example`, and the resolver saw exactly the allowlisted name.
//   T2  Wrong name: the peer's certificate is for `other.example` -> `net.tls_certificate_rejected`, class `policy`, and NO
//       HTTP request byte reached the peer (it saw the handshake only).
//   T3  Broken CA override (not a parseable certificate) -> `net.tls_setup_failed`, class `fatal`.
//   T4  IP-literal https allowlist entry (`127.0.0.1:<port>:https`, which the production policy refuses; the test policy
//       permits loopback): the certificate is verified against the LITERAL as the name -- a certificate with only a DNS
//       name is rejected; one with an IP SAN 127.0.0.1 -> 200. MEASURED (not a goal): mbedTLS also accepts a certificate
//       whose DNS SAN is the TEXT "127.0.0.1" for an IP-literal name; the row pins that so a change is noticed.
//
// Mutants (ADR-241 follow-up record): (a) egress_error maps tls_failed to transient; (b) the verify target is the resolved
// address text; (c) VERIFY_REQUIRED relaxed to OPTIONAL in the mbedTLS client config.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../support/crt_fail_fast.hpp"
#include "../support/https_test_peer.hpp"
#include "../support/memory_cap.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"

namespace {

using namespace agentengine;
using namespace agentengine::test_support;
using namespace std::chrono_literals;
using agentengine::cap::NetOut;
using agentengine::sandbox::EgressCallControl;
using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::NetEgressResponse;
using agentengine::sandbox::TestEgressProxy;

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) ++g_failures;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

using Entry = result<NetEgressResponse> (*)(TestEgressProxy const&, NetEgressRequest, NetOut, EgressCallControl);
result<NetEgressResponse> via_fetch(TestEgressProxy const& p, NetEgressRequest r, NetOut g, EgressCallControl c) {
    return rt::block_on(p.fetch(std::move(r), std::move(g), std::move(c)));
}
result<NetEgressResponse> via_blocking(TestEgressProxy const& p, NetEgressRequest r, NetOut g, EgressCallControl c) {
    return p.fetch_blocking(std::move(r), std::move(g), std::move(c));
}
constexpr std::array<std::pair<char const*, Entry>, 2> kEntries{{{"fetch", via_fetch}, {"fetch_blocking", via_blocking}}};

NetOut grant(std::string const& entry) {
    NetOut n;
    n.host_allowlist = {entry};
    return n;
}
std::string entry_for(std::string const& name, std::uint16_t port) { return name + ":" + std::to_string(port) + ":https"; }

// Resolves every name to loopback and records the names asked.
struct FakeDns {
    std::mutex               m;
    std::vector<std::string> names;
    pal::ResolveOutcome      operator()(std::string const& host) {
        {
            std::lock_guard lock(m);
            names.push_back(host);
        }
        pal::ResolveOutcome out;
        out.addresses.push_back(pal::IpAddress::loopback_v4());
        return out;
    }
    std::vector<std::string> seen() {
        std::lock_guard lock(m);
        return names;
    }
};

bool loopback_only(pal::IpAddress const& a) noexcept { return a == pal::IpAddress::loopback_v4(); }

Script const kOk{{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"}, true, false, false};

void expect_ok(std::string const& tag, result<NetEgressResponse> const& r) {
    check(r.has_value(), tag + ": the call succeeds");
    if (!r.has_value()) {
        std::printf("       (was %s: %s)\n", r.error().code.c_str(), r.error().message.c_str());
        return;
    }
    check(r->status == 200 && r->body == "ok", tag + ": status 200 and the body arrived");
}

void expect_tls_error(std::string const& tag, result<NetEgressResponse> const& r, failure_class klass, char const* code) {
    check(!r.has_value(), tag + ": the call fails");
    if (r.has_value()) return;
    check(r.error().code == code, tag + ": code is " + code + " (was " + r.error().code + ")");
    check(r.error().klass == klass, tag + ": class");
}

// The peer is single-threaded and serves one connection at a time. After a failed handshake the server thread may still
// be unwinding, so wait for the accept and give it a moment before asserting what it received.
bool peer_saw_no_request(HttpsServer& srv) {
    (void)std::chrono::steady_clock::now();
    auto const end = std::chrono::steady_clock::now() + 5s;
    while (srv.accepted() < 1 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(150ms);
    return srv.request().empty();
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    (void)agentengine::test_support::cap_process_memory(std::size_t{512} << 20, std::size_t{2048} << 20);
    std::thread([] {
        std::this_thread::sleep_for(240s);
        std::printf("[FAIL] WATCHDOG: the test did not finish within 240 s\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();
    pal::ensure_winsock();

    TestCa             ca;
    mbedtls_pk_context root_key = ca.generate_key();
    KeyCert const      root     = ca.issue(&root_key, "AE egress TLS Test Root", &root_key, "AE egress TLS Test Root", true);
    auto const         leaf     = [&](std::string const& cn, std::string const& dns_san, std::string const& ip_san = {}) {
        mbedtls_pk_context k = ca.generate_key();
        return ca.issue(&k, cn, &root_key, "AE egress TLS Test Root", false, dns_san, ip_san);
    };
    KeyCert const good       = leaf("good.example", "good.example");
    KeyCert const other      = leaf("other.example", "other.example");
    KeyCert const dns_literal_text = leaf("127.0.0.1", "127.0.0.1");  // a DNS SAN whose text is the IP literal
    KeyCert const ip_san     = leaf("ip.example", "", std::string("\x7f\x00\x00\x01", 4));
    check(!root.cert_pem.empty() && !good.cert_pem.empty() && !other.cert_pem.empty() && !dns_literal_text.cert_pem.empty() &&
              !ip_san.cert_pem.empty(),
          "setup: test CA and certificates generated in memory");

    for (auto const& [entry, go] : kEntries) {
        std::string const e = entry;
        auto const        wire = [&](TestEgressProxy& p, FakeDns& dns, std::string const& ca_pem) {
            p.host_resolver   = std::ref(dns);
            p.address_policy  = loopback_only;
            p.tls_ca_override = ca_pem;
        };
        {  // T1: positive control
            HttpsServer srv(good, kOk);
            FakeDns     dns;
            TestEgressProxy p;
            wire(p, dns, root.cert_pem);
            auto        r = go(p, NetEgressRequest{"GET", "/", {}, ""}, grant(entry_for("good.example", srv.port())),
                        EgressCallControl{});
            expect_ok(e + ": T1 certificate for the allowlisted name", r);
            std::string const req = srv.request();
            check(req.rfind("GET / HTTP/1.1\r\n", 0) == 0 && req.find("Host: good.example:" + std::to_string(srv.port())) !=
                                                                 std::string::npos,
                  e + ": T1 the peer saw the request with Host = the allowlisted name");
            auto const names = dns.seen();
            check(names.size() == 1 && names[0] == "good.example", e + ": T1 the resolver saw exactly the allowlisted name");
        }
        {  // T2: certificate for another name
            HttpsServer srv(other, kOk);
            FakeDns     dns;
            TestEgressProxy p;
            wire(p, dns, root.cert_pem);
            auto        r = go(p, NetEgressRequest{"GET", "/", {}, ""}, grant(entry_for("good.example", srv.port())),
                        EgressCallControl{});
            expect_tls_error(e + ": T2 certificate for other.example", r, failure_class::policy, "net.tls_certificate_rejected");
            check(peer_saw_no_request(srv), e + ": T2 NO HTTP request bytes reached the peer (handshake only)");
        }
        {  // T3: an unusable CA override
            HttpsServer srv(good, kOk);
            FakeDns     dns;
            TestEgressProxy p;
            wire(p, dns, "-----BEGIN CERTIFICATE-----\nbm90IGEgY2VydGlmaWNhdGU=\n-----END CERTIFICATE-----\n");
            auto        r = go(p, NetEgressRequest{"GET", "/", {}, ""}, grant(entry_for("good.example", srv.port())),
                        EgressCallControl{});
            expect_tls_error(e + ": T3 broken CA override", r, failure_class::fatal, "net.tls_setup_failed");
            check(peer_saw_no_request(srv), e + ": T3 NO HTTP request bytes reached the peer");
        }
        {  // T3b: a CA that is a valid certificate but not the peer's issuer -> the peer's chain is untrusted (policy)
            mbedtls_pk_context k2 = ca.generate_key();
            KeyCert const      stranger = ca.issue(&k2, "Stranger Root", &k2, "Stranger Root", true);
            HttpsServer        srv(good, kOk);
            FakeDns            dns;
            TestEgressProxy p;
            wire(p, dns, stranger.cert_pem);
            auto               r = go(p, NetEgressRequest{"GET", "/", {}, ""}, grant(entry_for("good.example", srv.port())),
                        EgressCallControl{});
            expect_tls_error(e + ": T3b untrusted issuer", r, failure_class::policy, "net.tls_certificate_rejected");
            check(peer_saw_no_request(srv), e + ": T3b NO HTTP request bytes reached the peer");
        }
        {  // T4: IP-literal https entry, three certificates
            struct Row {
                char const*     what;
                KeyCert const*  cert;
                bool            accepted;
            };
            for (Row const& row : {Row{"DNS-name-only certificate", &good, false},
                                   Row{"MEASURED: certificate whose DNS SAN is the text 127.0.0.1 (mbedTLS matches a DNS SAN textually for an IP-literal name; no public CA issues one)", &dns_literal_text, true},
                                   Row{"certificate with an IP SAN 127.0.0.1 (control)", &ip_san, true}}) {
                HttpsServer srv(*row.cert, kOk);
                FakeDns     dns;
                TestEgressProxy p;
            wire(p, dns, root.cert_pem);
                auto        r = go(p, NetEgressRequest{"GET", "/", {}, ""},
                            grant("127.0.0.1:" + std::to_string(srv.port()) + ":https"), EgressCallControl{});
                std::string const tag = e + ": T4 IP-literal entry, " + row.what;
                if (row.accepted) {
                    expect_ok(tag, r);
                } else {
                    expect_tls_error(tag, r, failure_class::policy, "net.tls_certificate_rejected");
                    check(peer_saw_no_request(srv), tag + ": NO HTTP request bytes reached the peer");
                }
            }
        }
    }

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "ALL OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
