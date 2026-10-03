// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("HTTP/1.1 + SSE ... rewritten as
// coroutines"; "TLS: keep mbedTLS (ADR-013)") -- rt/http.hpp over rt/http_tls.hpp (rt::TlsStream). Only built with
// AGENTENGINE_WITH_HTTPS. The peer is a loopback mbedTLS server on a test thread with a CA generated in memory (as
// test_rt_tls.cpp). Also the field-level SSE differential against the EXISTING provider parsers (it links them).
// Each claim names the mutant that kills it (run 2026-10-03; outcomes in ADR-237 §14).
//
//   S1  An https request for "localhost" (resolved through the rt/dns.hpp test seam to 127.0.0.1, accepted by the
//       policy) verifies the server against the ORIGINAL name, sends `Host: localhost:<port>`, and decodes a chunked
//       SSE body; every await resumes on the block_on() thread. Mutant "TLS hostname = the address" (the connector
//       is handed the numeric address instead of the request's host -> certificate rejected).
//   S2  A certificate for another name is `tls_failed` with the TLS client's `net.tls_certificate_rejected`, and the
//       server never receives an HTTP request (nothing is sent before verification).
//   S3  A close-delimited body that ends with close_notify is complete; one that ends with a bare TCP close is
//       `truncated`; a Content-Length body is complete either way. Mutant "unclean EOF ignored".
//   S4  A stop mid-body over TLS resumes promptly `canceled`, on the home thread; the server sees the close.
//   S5  A server that never answers the ClientHello: the handshake ends `idle_timeout` (the handshake is bounded --
//       step 4's "no handshake deadline" is closed for this client). Mutant "no idle timer".
//   S6  Differential: 400 generated single-data-line streams at fuzzed splits through the EXISTING provider path
//       (sandbox::SseEventFramer + anthropic::split_sse_named_events, and + openai::split_sse_data_events) and through
//       rt::SseDecoder give identical (type, data) sequences; and the documented divergence (multi-line data: one
//       joined event here, one event per line there) is real.
//
// Coroutines are free functions taking their state by pointer (ADR-237 §14); result containers are declared before
// the reactor/pools that complete into them.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"

#if !defined(_WIN32)
#include <sys/select.h>
#endif

#include "agentengine/pal/reactor.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/rt/http_tls.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tls.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
#include "../support/crt_fail_fast.hpp"

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 10s) {
    auto const end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

constexpr std::uint64_t kLoopback = 0x7F000001;

// ---- test-only certificates (in memory; as test_rt_tls.cpp) ------------------------------------------------

struct KeyCert {
    std::string cert_pem;
    std::string key_pem;
};

class TestCa {
public:
    TestCa() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-rt-https-test-ca";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, reinterpret_cast<unsigned char const*>(pers),
                              std::strlen(pers));
    }
    ~TestCa() {
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TestCa(TestCa const&)            = delete;
    TestCa& operator=(TestCa const&) = delete;

    mbedtls_pk_context generate_key() {
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg_);
        return pk;
    }

    KeyCert issue(mbedtls_pk_context* subject_key, std::string const& subject, mbedtls_pk_context* issuer_key,
                  std::string const& issuer, bool is_ca, std::string const& san = {}) {
        mbedtls_x509write_cert ctx;
        mbedtls_x509write_crt_init(&ctx);
        mbedtls_x509write_crt_set_version(&ctx, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&ctx, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&ctx, subject_key);
        mbedtls_x509write_crt_set_issuer_key(&ctx, issuer_key);
        std::string const sdn = "CN=" + subject;
        std::string const idn = "CN=" + issuer;
        mbedtls_x509write_crt_set_subject_name(&ctx, sdn.c_str());
        mbedtls_x509write_crt_set_issuer_name(&ctx, idn.c_str());
        unsigned char serial = static_cast<unsigned char>(next_serial_++);
        mbedtls_x509write_crt_set_serial_raw(&ctx, &serial, 1);
        mbedtls_x509write_crt_set_validity(&ctx, "20250101000000", "20350101000000");
        mbedtls_x509write_crt_set_basic_constraints(&ctx, is_ca ? 1 : 0, is_ca ? -1 : 0);
        mbedtls_x509_san_list list{};
        if (!san.empty()) {
            list.node.type                      = MBEDTLS_X509_SAN_DNS_NAME;
            list.node.san.unstructured_name.p   = reinterpret_cast<unsigned char*>(const_cast<char*>(san.data()));
            list.node.san.unstructured_name.len = san.size();
            list.next                           = nullptr;
            mbedtls_x509write_crt_set_subject_alternative_name(&ctx, &list);
        }
        unsigned char cert_buf[4096];
        int const     cert_rc =
            mbedtls_x509write_crt_pem(&ctx, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_x509write_crt_free(&ctx);
        unsigned char key_buf[4096];
        int const     key_rc = mbedtls_pk_write_key_pem(subject_key, key_buf, sizeof(key_buf));
        KeyCert       out;
        if (cert_rc == 0) out.cert_pem.assign(reinterpret_cast<char*>(cert_buf));
        if (key_rc == 0) out.key_pem.assign(reinterpret_cast<char*>(key_buf));
        return out;
    }

private:
    mbedtls_entropy_context  entropy_{};
    mbedtls_ctr_drbg_context drbg_{};
    int                      next_serial_ = 1;
};

// ---- the loopback HTTPS peer --------------------------------------------------------------------------------
// Every mbedTLS call under rt::tls_detail::engine_mutex() (rt/tls.hpp), readiness waits outside it.

bool wait_ready(pal::fd_t fd, bool for_write, int timeout_ms) {
    ::fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    ::timeval tv{};
    tv.tv_sec      = timeout_ms / 1000;
    tv.tv_usec     = (timeout_ms % 1000) * 1000;
    int const nfds = static_cast<int>(fd) + 1;
    int const rc   = for_write ? ::select(nfds, nullptr, &set, nullptr, &tv) : ::select(nfds, &set, nullptr, nullptr, &tv);
    return rc > 0;
}

int server_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto r = pal::send_some(*static_cast<pal::fd_t*>(ctx), reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int server_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto r = pal::recv_some(*static_cast<pal::fd_t*>(ctx), reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(*r);
}

struct Script {
    std::vector<std::string> sends;               // sent in order, 1 ms apart
    bool                     close_notify = true;  // false: a bare TCP close (no close_notify)
    bool                     hold         = false;  // after the sends, wait for the client to close
    bool                     stall        = false;  // never answer the ClientHello
};

class HttpsServer {
public:
    HttpsServer(KeyCert const& kc, Script script) : script_(std::move(script)) {
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-rt-https-test-server";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, reinterpret_cast<unsigned char const*>(pers),
                              std::strlen(pers));
        mbedtls_x509_crt_parse(&cert_, reinterpret_cast<unsigned char const*>(kc.cert_pem.c_str()),
                               kc.cert_pem.size() + 1);
        mbedtls_pk_parse_key(&key_, reinterpret_cast<unsigned char const*>(kc.key_pem.c_str()), kc.key_pem.size() + 1,
                             nullptr, 0, mbedtls_ctr_drbg_random, &drbg_);
        auto l = pal::tcp_listen(kLoopback, 0, 16);
        if (l) {
            listen_ = *l;
            port_   = pal::local_port(listen_).value_or(0);
        }
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~HttpsServer() {
        thread_.request_stop();
        thread_.join();
        pal::close_fd(listen_);
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_pk_free(&key_);
        mbedtls_x509_crt_free(&cert_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    HttpsServer(HttpsServer const&)            = delete;
    HttpsServer& operator=(HttpsServer const&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int           accepted() const { return accepted_.load(); }
    [[nodiscard]] int           saw_close() const { return saw_close_.load(); }
    [[nodiscard]] std::string   request() {
        std::lock_guard lock(m_);
        return request_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_);
            if (!a) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            accepted_.fetch_add(1);
            pal::fd_t fd = *a;
            serve(fd, st);
            pal::close_fd(fd);
        }
    }

    template <class F>
    int retry(pal::fd_t fd, std::stop_token const& st, F&& f) {
        auto const end = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            int rc = 0;
            {
                std::lock_guard const lock(tls_detail::engine_mutex());
                rc = f();
            }
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return rc;
            if (st.stop_requested() || std::chrono::steady_clock::now() > end) return MBEDTLS_ERR_SSL_TIMEOUT;
            (void)wait_ready(fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 10);
        }
    }

    void serve(pal::fd_t fd, std::stop_token const& st) {
        if (script_.stall) {  // read the ClientHello, never answer; report the client hanging up
            std::byte buf[4096];
            auto const end = std::chrono::steady_clock::now() + 5s;
            while (!st.stop_requested() && std::chrono::steady_clock::now() < end) {
                auto r = pal::recv_some(fd, buf, sizeof(buf));
                if ((r && *r == 0) || (!r && r.error() != pal::would_block())) {
                    saw_close_.fetch_add(1);
                    return;
                }
                (void)wait_ready(fd, false, 10);
            }
            return;
        }
        mbedtls_ssl_config conf;
        mbedtls_ssl_context ssl;
        {
            std::lock_guard const lock(tls_detail::engine_mutex());
            mbedtls_ssl_config_init(&conf);
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg_);
            mbedtls_ssl_conf_own_cert(&conf, &cert_, &key_);
            mbedtls_ssl_setup(&ssl, &conf);
            mbedtls_ssl_set_bio(&ssl, &fd, server_send, server_recv, nullptr);
        }
        if (retry(fd, st, [&] { return mbedtls_ssl_handshake(&ssl); }) == 0) {
            std::string                req;
            std::vector<unsigned char> buf(16384);
            while (req.find("\r\n\r\n") == std::string::npos) {
                int const n = retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); });
                if (n <= 0) break;
                req.append(reinterpret_cast<char const*>(buf.data()), static_cast<std::size_t>(n));
            }
            {
                std::lock_guard lock(m_);
                request_ = req;
            }
            bool ok = !req.empty();
            for (std::string const& s : script_.sends) {
                std::size_t off = 0;
                while (ok && off < s.size()) {
                    int const n = retry(fd, st, [&] {
                        return mbedtls_ssl_write(&ssl, reinterpret_cast<unsigned char const*>(s.data()) + off,
                                                 s.size() - off);
                    });
                    if (n <= 0) ok = false;
                    else off += static_cast<std::size_t>(n);
                }
                std::this_thread::sleep_for(1ms);
            }
            if (ok && script_.hold) {
                int const n = retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); });
                if (n <= 0 && n != MBEDTLS_ERR_SSL_TIMEOUT) saw_close_.fetch_add(1);
            } else if (ok && script_.close_notify) {
                (void)retry(fd, st, [&] { return mbedtls_ssl_close_notify(&ssl); });
            }
        }
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }

    Script                   script_;
    mbedtls_x509_crt         cert_;
    mbedtls_pk_context       key_;
    mbedtls_entropy_context  entropy_;
    mbedtls_ctr_drbg_context drbg_;
    pal::fd_t                listen_ = pal::invalid_fd;
    std::uint16_t            port_   = 0;
    std::atomic<int>         accepted_{0};
    std::atomic<int>         saw_close_{0};
    std::mutex               m_;
    std::string              request_;
    std::jthread             thread_;  // last: started after everything above exists
};

// ---- the client side ------------------------------------------------------------------------------------------

HostResolver localhost_is_loopback() {
    return [](std::string const&) {
        pal::ResolveOutcome o;
        o.addresses.push_back(pal::IpAddress::loopback_v4());
        return o;
    };
}

HttpContext context(pal::Reactor* reactor, OffloadPool* dns, std::string const& root_pem, HttpOptions options = {}) {
    HttpContext c;
    c.reactor                  = reactor;
    c.dns_pool                 = dns;
    c.policy                   = [](pal::IpAddress const& a) { return a == pal::IpAddress::loopback_v4(); };
    c.tls                      = tls_connector({.ca_bundle_pem_override = root_pem});
    c.options                  = options;
    c.options.connect.resolver = localhost_is_loopback();
    return c;
}

HttpRequest get(std::uint16_t port, std::string target = "/") {
    HttpRequest r;
    r.scheme = http_scheme::https;
    r.host   = "localhost";
    r.port   = port;
    r.target = std::move(target);
    return r;
}

struct SseRun {
    HttpStreamResponse           head;
    std::vector<SseEvent>        events;
    SseNext                      last;
    std::vector<std::thread::id> resumed_on;
};
rt::task<SseRun> sse_all(HttpContext ctx, HttpRequest req, std::stop_token stop) {
    SseRun run;
    run.head = co_await http_stream(std::move(ctx), std::move(req), stop);
    run.resumed_on.push_back(std::this_thread::get_id());
    if (!run.head.ok()) co_return run;
    SseStream sse = sse_events(std::move(run.head.body));
    for (;;) {
        SseNext n = co_await sse.next(stop);
        run.resumed_on.push_back(std::this_thread::get_id());
        if (n.status != sse_status::event) {
            run.last = std::move(n);
            break;
        }
        run.events.push_back(std::move(n.event));
    }
    co_return run;
}

struct Timed {
    HttpResponse              r;
    std::chrono::milliseconds took{0};
};
rt::task<Timed> timed_request(HttpContext ctx, HttpRequest req, std::stop_token stop) {
    Timed      t;
    auto const start = std::chrono::steady_clock::now();
    t.r              = co_await http_request(std::move(ctx), std::move(req), std::move(stop));
    t.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    co_return t;
}
Timed bounded_request(HttpContext ctx, HttpRequest req, std::chrono::milliseconds limit = 5s) {
    std::stop_source        src;
    std::mutex              m;
    std::condition_variable cv;
    bool                    finished = false;
    std::thread             safety([&] {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, limit, [&] { return finished; })) src.request_stop();
    });
    Timed t = block_on(timed_request(std::move(ctx), std::move(req), src.get_token()));
    {
        std::lock_guard lock(m);
        finished = true;
    }
    cv.notify_one();
    safety.join();
    return t;
}

std::string chunked(std::vector<std::string> const& parts) {
    std::string out;
    char        hex[32];
    for (auto const& p : parts) {
        std::snprintf(hex, sizeof(hex), "%zx", p.size());
        out += hex;
        out += "\r\n" + p + "\r\n";
    }
    return out + "0\r\n\r\n";
}

// ---- the differential (S6) ------------------------------------------------------------------------------------

struct Pair {
    std::string type, data;
    friend bool operator==(Pair const&, Pair const&) = default;
};

// Single-data-line events, optional `event:`, comments, consistent LF or CRLF: what both parsers define.
std::string generate_simple(std::mt19937& rng) {
    auto        pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    std::string eol  = pick(2) == 0 ? "\n" : "\r\n";
    auto        word = [&]() {
        static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz{}\":,0123456789 []";
        std::string           w;
        int const             n = 1 + pick(16);
        for (int i = 0; i < n; ++i) w.push_back(kAlphabet[pick(static_cast<int>(sizeof(kAlphabet) - 1))]);
        if (w.front() == ' ') w.front() = 'q';
        if (w.back() == ' ') w.back() = 'q';
        return w;
    };
    std::string wire;
    int const   n = 1 + pick(10);
    for (int e = 0; e < n; ++e) {
        if (pick(3) == 0) wire += ":" + word() + eol;
        if (pick(2) == 0) wire += "event: " + word() + eol;
        wire += (pick(2) == 0 ? "data: " : "data:") + word() + eol + eol;
    }
    return wire;
}

std::vector<std::string_view> split_randomly(std::string_view s, std::mt19937& rng) {
    std::vector<std::string_view> parts;
    std::size_t                   i = 0;
    while (i < s.size()) {
        std::size_t const n = 1 + rng() % 9;
        parts.push_back(s.substr(i, n));
        i += n;
    }
    return parts;
}

// The existing provider path: SseEventFramer blocks, then the Anthropic per-block field scan.
std::vector<Pair> existing_named(std::vector<std::string_view> const& parts) {
    sandbox::SseEventFramer framer;
    std::vector<Pair>       out;
    for (std::string_view p : parts) {
        for (std::string const& block : framer.feed(p)) {
            for (anthropic::detail::SseEvent const& ev : anthropic::detail::split_sse_named_events(block)) {
                out.push_back(Pair{std::string(ev.type), std::string(ev.data)});
            }
        }
    }
    return out;
}
// ... and the OpenAI per-block scan (data only).
std::vector<std::string> existing_data(std::vector<std::string_view> const& parts) {
    sandbox::SseEventFramer  framer;
    std::vector<std::string> out;
    for (std::string_view p : parts) {
        for (std::string const& block : framer.feed(p)) {
            for (std::string_view d : openai::detail::split_sse_data_events(block)) out.emplace_back(d);
        }
    }
    return out;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(280s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 280 s\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main
    pal::ensure_winsock();

    // S6 first: pure, no I/O.
    {
        int mismatches = 0;
        for (unsigned seed = 1; seed <= 400; ++seed) {
            std::mt19937                        rng(seed);
            std::string const                   wire  = generate_simple(rng);
            std::vector<std::string_view> const parts = split_randomly(wire, rng);
            SseDecoder                          d;
            std::vector<SseEvent>               mine;
            for (std::string_view p : parts) (void)d.feed(p, &mine);
            std::vector<Pair>        mine_pairs;
            std::vector<std::string> mine_data;
            for (auto const& e : mine) {
                mine_pairs.push_back(Pair{e.event, e.data});
                mine_data.push_back(e.data);
            }
            if (mine_pairs != existing_named(parts) || mine_data != existing_data(parts)) {
                if (mismatches == 0) std::printf("  first mismatch at seed %u\n", seed);
                ++mismatches;
            }
        }
        check(mismatches == 0, "S6: 400 generated streams at fuzzed splits: the existing provider parsers (framer + "
                               "Anthropic / OpenAI scans) and rt::SseDecoder give identical events");
        std::string const     multi = "data: a\ndata: b\n\n";
        SseDecoder            d;
        std::vector<SseEvent> mine;
        (void)d.feed(multi, &mine);
        auto const theirs = existing_named({multi});
        check(mine.size() == 1 && mine[0].data == "a\nb" && theirs.size() == 2,
              "S6: the documented divergence is real -- multi-line data is one joined event here, one per line there");
    }

    TestCa             ca;
    mbedtls_pk_context root_key = ca.generate_key();
    KeyCert const      root = ca.issue(&root_key, "AE rt http Test Root", &root_key, "AE rt http Test Root", true);
    mbedtls_pk_context leaf_key = ca.generate_key();
    KeyCert const      leaf = ca.issue(&leaf_key, "localhost", &root_key, "AE rt http Test Root", false, "localhost");
    mbedtls_pk_context other_key = ca.generate_key();
    KeyCert const      other =
        ca.issue(&other_key, "elsewhere.test", &root_key, "AE rt http Test Root", false, "elsewhere.test");
    check(!root.cert_pem.empty() && !leaf.cert_pem.empty() && !other.cert_pem.empty(),
          "setup: test CA and certificates generated in memory");

    std::thread::id const home    = std::this_thread::get_id();
    auto                  reactor = pal::make_default_reactor();
    OffloadPool           dns{{.workers = 2}};

    // S1 ------------------------------------------------------------------------------------------------------
    {
        std::string const sse = "event: start\ndata: {\"x\":1}\n\n: keepalive\n\ndata: second\ndata: line\n\n";
        HttpsServer       srv(leaf, Script{{"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                            "Transfer-Encoding: chunked\r\n\r\n",
                                            chunked({sse.substr(0, 9), sse.substr(9, 20), sse.substr(29)})},
                                           true, false, false});
        SseRun r = block_on(sse_all(context(reactor.get(), &dns, root.cert_pem), get(srv.port(), "/v1/stream"), {}));
        check(r.head.ok() && r.head.status == 200, "S1: https head received (certificate verified as 'localhost')");
        check(r.events.size() == 2 && r.events[0].event == "start" && r.events[0].data == "{\"x\":1}" &&
                  r.events[1].data == "second\nline" && r.last.status == sse_status::end,
              "S1: the chunked SSE body over TLS decodes to its events, then end");
        check(srv.request().starts_with("GET /v1/stream HTTP/1.1\r\nHost: localhost:" + std::to_string(srv.port())),
              "S1: the request carries the original host name (and port) in Host");
        bool on_home = !r.resumed_on.empty();
        for (auto id : r.resumed_on) on_home = on_home && id == home;
        check(on_home, "S1: every await resumed on the block_on() thread");
    }

    // S2 ------------------------------------------------------------------------------------------------------
    {
        HttpsServer srv(other, Script{{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nno"}, true, false, false});
        Timed       t = bounded_request(context(reactor.get(), &dns, root.cert_pem), get(srv.port()));
        std::this_thread::sleep_for(100ms);
        check(t.r.error == http_error::tls_failed && t.r.code == "net.tls_certificate_rejected",
              "S2: a certificate for another name is tls_failed (net.tls_certificate_rejected)");
        check(srv.request().empty(), "S2: no HTTP request was sent before verification");
    }

    // S3 ------------------------------------------------------------------------------------------------------
    {
        HttpsServer clean(leaf, Script{{"HTTP/1.1 200 OK\r\n\r\n", "all of it"}, true, false, false});
        Timed       a = bounded_request(context(reactor.get(), &dns, root.cert_pem), get(clean.port()));
        check(a.r.ok() && a.r.body == "all of it", "S3: a close-delimited body ending in close_notify is complete");
        HttpsServer cut(leaf, Script{{"HTTP/1.1 200 OK\r\n\r\n", "some of it"}, false, false, false});
        Timed       b = bounded_request(context(reactor.get(), &dns, root.cert_pem), get(cut.port()));
        check(b.r.error == http_error::truncated && b.r.code == "net.stream_truncated" && b.r.body == "some of it",
              "S3: a close-delimited body ending in a bare TCP close (no close_notify) is truncated");
        HttpsServer framed(leaf, Script{{"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nframed"}, false, false, false});
        Timed       c = bounded_request(context(reactor.get(), &dns, root.cert_pem), get(framed.port()));
        check(c.r.ok() && c.r.body == "framed", "S3: a Content-Length body is complete without close_notify");
    }

    // S4 ------------------------------------------------------------------------------------------------------
    {
        HttpOptions o;
        o.total_timeout = 4s;  // the safety answer a broken stop would get instead
        HttpsServer      srv(leaf, Script{{"HTTP/1.1 200 OK\r\n\r\n", "partial"}, true, true, false});
        std::stop_source src;
        std::thread      stopper([&] {
            std::this_thread::sleep_for(300ms);
            src.request_stop();
        });
        auto const start = std::chrono::steady_clock::now();
        Timed      t     = block_on(timed_request(context(reactor.get(), &dns, root.cert_pem, o), get(srv.port()),
                                                  src.get_token()));
        auto const took  = std::chrono::steady_clock::now() - start;
        stopper.join();
        check(t.r.error == http_error::canceled && took < 2s,
              "S4: a stop mid-body over TLS resumes promptly `canceled` (took " +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms)");
        check(wait_until([&] { return srv.saw_close() == 1; }, 3s), "S4: ... and the server sees the close");
    }

    // S5 ------------------------------------------------------------------------------------------------------
    {
        HttpOptions o;
        o.idle_timeout  = 250ms;
        o.total_timeout = 4s;
        HttpsServer srv(leaf, Script{{}, true, false, true});
        Timed       t = bounded_request(context(reactor.get(), &dns, root.cert_pem, o), get(srv.port()), 6s);
        check(t.r.error == http_error::idle_timeout && t.took < 2s,
              "S5: a server that never answers the ClientHello ends the handshake idle_timeout (took " +
                  std::to_string(t.took.count()) + " ms)");
        check(wait_until([&] { return srv.saw_close() == 1; }, 3s), "S5: ... and the connection is closed");
    }

    {
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_pk_free(&root_key);
        mbedtls_pk_free(&leaf_key);
        mbedtls_pk_free(&other_key);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
