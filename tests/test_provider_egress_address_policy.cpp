// Proves decisions/ADR-016-provider-egress-address-policy.md's gates G1-G4.
//
// ADR-016 relaxes the SSRF blocked-range table on the HOST-INITIATED provider path (004 §3) while
// leaving it fully enforced on the WASM GUEST egress path (ADR-011 claims C4/C5). A change of that
// shape is only defensible if BOTH halves are demonstrated, against the SAME address, in the SAME
// run -- otherwise "we relaxed it only where it was safe" is an assertion, not a result. G1 is
// therefore a POSITIVE CONTROL, not an afterthought: it is the test that fails if this ADR was
// actually a hole in the guest defense rather than a scoped correction.
//
// Everything here is deterministic and offline: a plain-HTTP loopback test server (no TLS, no
// certificate, no external dependency) plus pure-function resolver assertions. Nothing in this file
// needs a live model, a credential, or egress -- unlike tests/test_llamacpp_live_e2e.cpp, which
// proves ADR-016's G5 (the motivating case actually works) against a real local inference server.
//
// The loopback server is the point, not an inconvenience: 127.0.0.1 is simultaneously (a) an address
// the guest path must always refuse and (b) an address the provider path must now reach. One server,
// both assertions, no ambiguity about whether they were talking about the same thing.

#ifdef AGENTENGINE_WITH_HTTPS

#include "agentengine/pal/net.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

constexpr std::uint32_t make_ipv4(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}
constexpr std::uint32_t kLoopbackHostOrder = make_ipv4(127, 0, 0, 1);

constexpr char const* kResponseBody = R"({"ok":true,"who":"plain-http-loopback"})";

// A minimal plain-HTTP/1.1 server on loopback. Deliberately NOT a TLS server: half this file's job
// is to show that the DEFAULT provider transport still insists on TLS (G4), which is only observable
// against a peer that cannot speak it.
class PlainHttpTestServer {
public:
    PlainHttpTestServer() {
        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~PlainHttpTestServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    PlainHttpTestServer(PlainHttpTestServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int connections_served() const { return connections_served_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a, st);
            agentengine::pal::close_fd(*a);
        }
    }

    void serve_one(agentengine::pal::fd_t fd, std::stop_token const& st) {
        // Drain whatever arrives until the request head is complete (or the peer gives up). A TLS
        // ClientHello arriving here is binary garbage that never contains "\r\n\r\n" -- which is
        // exactly the G4 case, and why this loop must be bounded rather than waiting forever.
        std::string buf;
        std::byte chunk[1024];
        for (int i = 0; i < 200 && !st.stop_requested(); ++i) {
            auto r = agentengine::pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == agentengine::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                return;
            }
            if (*r == 0) return;  // peer closed
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        }
        if (buf.find("\r\n\r\n") == std::string::npos) return;  // never a valid HTTP request

        std::string const body = kResponseBody;
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            auto w = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(response.data() + sent),
                                            response.size() - sent);
            if (!w) {
                if (w.error() == agentengine::pal::would_block()) continue;
                return;
            }
            sent += *w;
        }
        ++connections_served_;
    }

    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    int connections_served_ = 0;
    std::jthread thread_;
};

[[nodiscard]] sandbox::NetEgressRequest simple_get() {
    sandbox::NetEgressRequest req;
    req.method = "GET";
    req.path = "/v1/models";
    return req;
}

// Every family in `kBlockedRanges` (net_egress_proxy.cpp), one representative each. The guest path
// must still refuse all of them; the provider path is expected to resolve them without complaint.
struct BlockedSample {
    char const* literal;
    char const* what;
};
constexpr BlockedSample kBlockedSamples[] = {
    {"127.0.0.1", "loopback"},
    {"169.254.169.254", "link-local / cloud metadata"},
    {"10.1.2.3", "RFC 1918 10/8"},
    {"172.16.5.4", "RFC 1918 172.16/12"},
    {"192.168.1.1", "RFC 1918 192.168/16"},
    {"100.64.0.1", "CGNAT"},
    {"224.0.0.1", "multicast"},
    {"240.0.0.1", "reserved"},
    {"0.0.0.0", "unspecified"},
};

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    // ---- G1 (POSITIVE CONTROL): the guest resolver still refuses every blocked range --------------
    // If ADR-016 had been implemented by weakening `is_blocked_address` or by pointing both callers
    // at the same permissive resolver, this block is what would catch it.
    {
        for (auto const& sample : kBlockedSamples) {
            auto r = sandbox::resolve_and_validate(sample.literal, 8080);
            check(!r.has_value() && r.error().code == "net.address_blocked",
                  sample.what);
            if (r.has_value()) {
                std::fprintf(stderr, "       G1 BREACH: resolve_and_validate accepted %s (%s)\n",
                             sample.literal, sample.what);
            }
        }
        std::fprintf(stderr,
                     "  (G1: the nine checks above are the guest path -- every kBlockedRanges family "
                     "still refused after ADR-016)\n");
    }

    // ---- G2: the provider resolver reaches exactly those same addresses ---------------------------
    {
        for (auto const& sample : kBlockedSamples) {
            auto r = sandbox::resolve_host(sample.literal, 8080);
            check(r.has_value(),
                  "G2: resolve_host accepts the address resolve_and_validate just refused");
            if (!r) {
                std::fprintf(stderr, "       %s (%s): %s\n", sample.literal, sample.what,
                             r.error().message.c_str());
            }
        }
        auto loopback = sandbox::resolve_host("127.0.0.1", 8080);
        check(loopback && loopback->ipv4_host_order == kLoopbackHostOrder && loopback->port == 8080,
              "G2: resolve_host returns the literal address and port unchanged -- it resolves, it does "
              "not rewrite");
    }

    // ---- G3: the two resolvers share one resolution implementation and cannot drift ----------------
    {
        // A non-blocked literal: both must answer identically, since the only difference is the
        // filter, not the resolution.
        auto validated = sandbox::resolve_and_validate("8.8.8.8", 443);
        auto plain = sandbox::resolve_host("8.8.8.8", 443);
        check(validated && plain && validated->ipv4_host_order == plain->ipv4_host_order &&
                  validated->port == plain->port,
              "G3: on a non-blocked literal both resolvers return the identical VerifiedEndpoint");

        // CORRECTED (2026-08-29, decisions/ADR-011-first-party-egress-proxy.md §9): this check
        // originally asserted BOTH resolvers reject "0177.0.0.1" outright, on the theory that
        // `inet_pton`'s strict dotted-quad-only fast path rejecting the string (true, confirmed just
        // above, on BOTH platforms) meant the whole resolution pipeline would treat it as unparseable.
        // That assumption was empirically wrong on Linux, and the real behavior turns out to be a
        // genuine PLATFORM DIFFERENCE in the underlying `getaddrinfo`, not an AgentEngine bug on
        // either side (CONVENTIONS.md: "isolation parity is a gate, not a goal" -- the two platforms'
        // own system resolvers genuinely differ here, upstream of anything this codebase controls):
        //   - Linux (glibc): `getaddrinfo`'s own internal numeric-address recognition is more lenient
        //     than `inet_pton` and DOES resolve "0177.0.0.1" -- via octal interpretation of the
        //     leading zero -- to 127.0.0.1, once the fast path falls through to it. Reproduced
        //     directly: a standalone `getaddrinfo("0177.0.0.1", ...)` call returns 127.0.0.1.
        //   - Windows: `getaddrinfo("0177.0.0.1", ...)` fails outright with `WSAHOST_NOT_FOUND`
        //     (winsock error 11001) -- no lenient numeric-address fallback exists here. Reproduced
        //     directly with a standalone MSVC-compiled probe against the real winsock `getaddrinfo`.
        //
        // Neither behavior is an SSRF hole: claim C5 (net_egress_proxy.hpp's own comment on
        // `is_blocked_address`: "runs on the resolved 32-bit address... never a string") is about what
        // happens to whatever numeric address DOES come out of resolution, and holds unaffected on
        // both platforms -- on Linux, `resolve_and_validate` correctly blocks the resolved 127.0.0.1;
        // on Windows, resolution fails before the blocked-range check is even reached, which is failing
        // closed by a different, earlier mechanism, not a weaker one. `resolve_host` (host-initiated
        // provider path, no filtering by design) has no reason to differ from `resolve_and_validate`'s
        // own resolution outcome here, since NEITHER applies a filter to a string that never yields
        // a resolved address on Windows in the first place -- only Linux's lenient resolution ever
        // reaches the point where a filtering decision is even possible.
#ifdef _WIN32
        auto validated_octal = sandbox::resolve_and_validate("0177.0.0.1", 80);
        check(!validated_octal.has_value() && validated_octal.error().code == "net.host_unresolvable",
              "G3 (Windows): resolve_and_validate fails to resolve an octal-encoded loopback literal at "
              "all -- winsock's own getaddrinfo has no lenient numeric-address fallback for this form");
        auto plain_octal = sandbox::resolve_host("0177.0.0.1", 80);
        check(!plain_octal.has_value() && plain_octal.error().code == "net.host_unresolvable",
              "G3 (Windows): resolve_host fails identically -- the two resolvers share one resolution "
              "half, so an unresolvable string is unresolvable through EITHER, filtered or not");
#else
        auto validated_octal = sandbox::resolve_and_validate("0177.0.0.1", 80);
        check(!validated_octal.has_value() && validated_octal.error().code == "net.address_blocked",
              "G3 (Linux): resolve_and_validate blocks an octal-encoded loopback literal -- glibc's "
              "getaddrinfo resolves it to 127.0.0.1, and the blocked-range check runs on that RESOLVED "
              "address, not the input string, so the unusual spelling does not bypass it");
        auto plain_octal = sandbox::resolve_host("0177.0.0.1", 80);
        check(plain_octal.has_value() && plain_octal->ipv4_host_order == kLoopbackHostOrder,
              "G3 (Linux): resolve_host resolves an octal-encoded loopback literal to 127.0.0.1, "
              "unfiltered -- correct per its own documented no-filtering contract (an operator's own "
              "configured provider endpoint may legitimately be loopback, however it is spelled)");
#endif
    }

    PlainHttpTestServer server;
    check(server.ok(), "the plain-HTTP loopback test server started");
    if (!server.ok()) {
        std::fprintf(stderr, "test_provider_egress_address_policy: %d FAILURE(S)\n", g_failures + 1);
        return 1;
    }
    std::string const host = "127.0.0.1";

    // ---- G4: TLS is still the default -- a defaulted call cannot reach a plaintext server ----------
    // The whole exchange defaults through: default resolver (`resolve_host`, so the ADDRESS is fine
    // now) and default transport (`tls`). It must still fail, because the peer speaks plain HTTP.
    // That isolates the transport as the reason: same address, same code path, only the transport
    // differs from the next case.
    {
        auto resp = sandbox::perform_provider_https_exchange(host, server.port(), simple_get());
        check(!resp.has_value(),
              "G4: with everything defaulted, the provider exchange still attempts TLS and fails "
              "against a plaintext peer -- ADR-016 did not quietly make plaintext the default, and "
              "the address is demonstrably NOT the reason (the next case succeeds on it)");
    }

    // ---- G2/G4: the opt-in plaintext transport reaches the same loopback server successfully -------
    {
        auto resp = sandbox::perform_provider_https_exchange(
            host, server.port(), simple_get(), /*stop=*/{}, /*byte_cap=*/std::nullopt,
            /*resolver=*/sandbox::resolve_host, /*ca_bundle_pem_override=*/{},
            ProviderTransport::plaintext_http);
        check(resp.has_value(),
              "G2/G4: with ProviderTransport::plaintext_http explicitly selected, a host-initiated "
              "provider exchange completes against a real plain-HTTP server on loopback -- the exact "
              "endpoint class (llama.cpp/vLLM/Ollama) that was unreachable before ADR-016");
        if (resp) {
            check(resp->status == 200, "the plaintext exchange returns the real status line");
            check(resp->body == kResponseBody,
                  "the plaintext exchange returns the real response body byte for byte");
        } else {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        }
    }

    // ---- G1 END TO END: the GUEST path is still refused at that very same live server --------------
    // The strongest form of the control. This is not a pure-function assertion about a literal -- it
    // is a real, reachable, currently-serving endpoint that the provider path just talked to
    // successfully, and the guest path must still refuse to connect to it. Note the guest request is
    // denied on ADDRESS policy, not on the grant: the allowlist deliberately NAMES this exact target,
    // so a passing result here could not be dismissed as a mis-specified capability.
    {
        sandbox::HostEgressProxy proxy;  // real resolver, exactly as production constructs it
        cap::NetOut granted;
        granted.host_allowlist.push_back(host + ":" + std::to_string(server.port()) + ":http");
        auto resp = proxy.fetch(simple_get(), granted);
        check(!resp.has_value(),
              "G1 (end to end): a WASM guest holding a cap::NetOut grant that explicitly allowlists "
              "this exact live loopback target is STILL refused -- ADR-016 relaxed the provider path "
              "and demonstrably nothing else");
        if (!resp) {
            check(resp.error().code == "net.address_blocked",
                  "G1: the guest refusal is specifically the blocked-address rule, not an incidental "
                  "grant/scheme/method failure that would have masked an unenforced table");
            check(resp.error().klass == failure_class::policy,
                  "G1: the guest refusal is classified `policy`");
        }
    }

    // The server counts only connections it answered with a real HTTP response. Exactly one case
    // above should have got that far -- if the TLS-default or guest-path cases had also reached it,
    // this would be higher, and one of those two assertions above would be passing for the wrong
    // reason.
    check(server.connections_served() == 1,
          "exactly ONE of the three attempts above actually completed an HTTP exchange with the "
          "server -- the TLS-default attempt and the guest attempt did not merely return errors, "
          "they never got a response out of it");

    if (g_failures == 0) {
        std::fprintf(stderr, "test_provider_egress_address_policy: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_provider_egress_address_policy: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
