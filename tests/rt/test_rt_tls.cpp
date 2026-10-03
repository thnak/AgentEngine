// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("TLS: keep mbedTLS ... BIO callbacks
// driven by reactor socket readiness ... Handshake, read and write are all cancellable"; "TLS context
// ownership"; "TLS details") with §4.2/§4.4 -- rt::TlsStream (rt/tls.hpp, src/backends/tls_mbedtls/). Only built
// with AGENTENGINE_WITH_HTTPS. The peer is a loopback mbedTLS server on a test thread; its CA and certificates are
// generated in memory at start-up (no fixture files, no network). Each claim names the mutant that kills it.
//
//   L1  Handshake + echo round trip with a valid certificate for "localhost": every await resumes on the HOME
//       thread (the block_on thread), and every call into mbedTLS happens there too -- never on the reactor
//       thread (§4.2, §6.3 "TLS context ownership"). Mutant: resume inline on the reactor (reactor_await) ->
//       engine calls land on the reactor thread.
//   L2  Under an ADR-219 host Resumer, the TLS ops resume -- and call mbedTLS -- on the Resumer's thread.
//   L3  A certificate valid for another host name is REJECTED (`certificate_rejected`, CN-mismatch flag, the
//       blocking client's `net.tls_certificate_rejected` code) and the stream is closed. Mutant: verification
//       off (authmode NONE) -> the handshake succeeds; mutant: no hostname set -> idem.
//   L4  A certificate from a CA outside the trust set is REJECTED (`certificate_rejected`, NOT_TRUSTED flag).
//       Mutant: verification off.
//   L5  A stop mid-handshake (a peer that never answers the ClientHello) resumes `canceled` promptly, on the home
//       thread; the stream is closed (the peer sees EOF) and a later op reports `closed` (§4.4). Mutant: the
//       handshake's TCP read ignores the stop.
//   L6  A stop mid-read (no data coming) resumes `canceled` promptly; the stream is closed; a later write is
//       `closed`. Mutant: the read's TCP read ignores the stop.
//   L7  A frame destroyed while its read is pending: no continuation is ever posted, nothing crashes, the stream
//       is closed (the peer sees EOF) and the handle reports `closed` afterwards.
//   L8  8 MiB each way, intact: the client sends 8 MiB of a pattern, the server checks every byte and answers
//       with 8 MiB of another pattern, which the client checks.
//   L9  Concurrency rule: a read and a write run concurrently on one stream from two different home threads
//       (4 MiB through an echo peer, every byte intact, run under TSan); a second read while one is pending is
//       `busy` at once and disturbs nothing. Mutants: no engine lock (TSan race); no busy check.
//   L10 close_notify both ways: the peer's "bye" + close_notify reads as data then `eof`; after our own
//       close_notify a write is `invalid_state`.
//   L11 A stop already requested completes `canceled` without touching the stream (it stays usable); ops before
//       the handshake are `invalid_state`; an empty host name fails `setup_failed`.
//
// Coroutines are free functions taking their state by pointer, never immediately-invoked lambda coroutines with
// captures (ADR-237 §14); result containers are declared before the reactor that completes into them.

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
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"

#if !defined(_WIN32)
#include <sys/select.h>
#endif

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tcp.hpp"
#include "agentengine/rt/tls.hpp"
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

std::vector<std::byte> bytes_of(std::string const& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}
std::string string_of(std::vector<std::byte> const& v) {
    return std::string(reinterpret_cast<char const*>(v.data()), v.size());
}
std::byte pattern_a(std::size_t i) { return static_cast<std::byte>((i * 7 + 3) & 0xFF); }
std::byte pattern_b(std::size_t i) { return static_cast<std::byte>((i * 13 + 5) & 0xFF); }

// ---- test-only certificates (in memory; as tests/sandbox/test_https_egress.cpp) ---------------------------

struct KeyCert {
    std::string cert_pem;
    std::string key_pem;
};

class TestCa {
public:
    TestCa() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-rt-tls-test-ca";
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
            list.node.type                         = MBEDTLS_X509_SAN_DNS_NAME;
            list.node.san.unstructured_name.p      = reinterpret_cast<unsigned char*>(const_cast<char*>(san.data()));
            list.node.san.unstructured_name.len    = san.size();
            list.next                              = nullptr;
            mbedtls_x509write_crt_set_subject_alternative_name(&ctx, &list);
        }
        unsigned char cert_buf[4096];
        int const     cert_rc = mbedtls_x509write_crt_pem(&ctx, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random,
                                                          &drbg_);
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

// ---- the loopback TLS peer ---------------------------------------------------------------------------------
// Non-blocking BIO; every mbedTLS call under rt::tls_detail::engine_mutex() (PSA's key store is process-global,
// rt/tls.hpp), and the readiness waits OUTSIDE it, so the server never holds the lock while it waits.

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

class TlsServer {
public:
    enum class mode { echo, stall, sink_then_send, say_bye };

    TlsServer(KeyCert const& kc, mode m, std::size_t n = 0) : mode_(m), n_(n) {
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-rt-tls-test-server";
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
    ~TlsServer() {
        thread_.request_stop();
        thread_.join();
        pal::close_fd(listen_);
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_pk_free(&key_);
        mbedtls_x509_crt_free(&cert_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TlsServer(TlsServer const&)            = delete;
    TlsServer& operator=(TlsServer const&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int           peer_gone() const { return peer_gone_.load(); }       // client closed / failed
    [[nodiscard]] int           handshakes() const { return handshakes_.load(); }     // completed server-side
    [[nodiscard]] bool          sink_intact() const { return sink_intact_.load(); }  // sink_then_send verdict

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_);
            if (!a) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            pal::fd_t fd = *a;
            serve(fd, st);
            pal::close_fd(fd);
        }
    }

    // One mbedTLS call under the engine lock; WANT_* waits for readiness outside it. Returns the final rc.
    template <class F>
    int retry(pal::fd_t fd, std::stop_token const& st, F&& f) {
        for (;;) {
            int rc = 0;
            {
                std::lock_guard const lock(tls_detail::engine_mutex());
                rc = f();
            }
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return rc;
            if (st.stop_requested()) return MBEDTLS_ERR_SSL_TIMEOUT;
            (void)wait_ready(fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 10);
        }
    }

    bool write_all(mbedtls_ssl_context* ssl, pal::fd_t fd, std::stop_token const& st, unsigned char const* p,
                   std::size_t n) {
        std::size_t off = 0;
        while (off < n) {
            int const rc = retry(fd, st, [&] { return mbedtls_ssl_write(ssl, p + off, n - off); });
            if (rc <= 0) return false;
            off += static_cast<std::size_t>(rc);
        }
        return true;
    }

    void serve(pal::fd_t fd, std::stop_token const& st) {
        if (mode_ == mode::stall) {  // TCP only: never answer the ClientHello; report when the client hangs up
            std::byte buf[4096];
            while (!st.stop_requested()) {
                auto r = pal::recv_some(fd, buf, sizeof(buf));
                if ((r && *r == 0) || (!r && r.error() != pal::would_block())) {
                    peer_gone_.fetch_add(1);
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
        bool gone = false;
        int  rc   = retry(fd, st, [&] { return mbedtls_ssl_handshake(&ssl); });
        if (rc != 0) {
            gone = true;
        } else {
            handshakes_.fetch_add(1);
            std::vector<unsigned char> buf(16384);
            switch (mode_) {
                case mode::echo:
                    for (;;) {
                        int const n = retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); });
                        if (n <= 0) {
                            gone = true;
                            break;
                        }
                        if (!write_all(&ssl, fd, st, buf.data(), static_cast<std::size_t>(n))) {
                            gone = true;
                            break;
                        }
                    }
                    break;
                case mode::sink_then_send: {
                    std::size_t got = 0;
                    bool        ok  = true;
                    while (got < n_) {
                        int const r = retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); });
                        if (r <= 0) {
                            ok = false;
                            break;
                        }
                        for (int i = 0; i < r; ++i) ok = ok && static_cast<std::byte>(buf[static_cast<std::size_t>(i)]) == pattern_a(got + static_cast<std::size_t>(i));
                        got += static_cast<std::size_t>(r);
                    }
                    sink_intact_.store(ok && got == n_);
                    std::vector<unsigned char> reply(n_);
                    for (std::size_t i = 0; i < n_; ++i) reply[i] = static_cast<unsigned char>(pattern_b(i));
                    (void)write_all(&ssl, fd, st, reply.data(), reply.size());
                    (void)retry(fd, st, [&] { return mbedtls_ssl_close_notify(&ssl); });
                    while (retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); }) > 0) {
                    }
                    gone = true;
                    break;
                }
                case mode::say_bye: {
                    static constexpr unsigned char kBye[] = {'b', 'y', 'e'};
                    (void)write_all(&ssl, fd, st, kBye, sizeof(kBye));
                    (void)retry(fd, st, [&] { return mbedtls_ssl_close_notify(&ssl); });
                    while (retry(fd, st, [&] { return mbedtls_ssl_read(&ssl, buf.data(), buf.size()); }) > 0) {
                    }
                    gone = true;
                    break;
                }
                case mode::stall: break;
            }
        }
        if (gone) peer_gone_.fetch_add(1);
        std::lock_guard const lock(tls_detail::engine_mutex());
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }

    mode                     mode_;
    std::size_t              n_;
    mbedtls_x509_crt         cert_;
    mbedtls_pk_context       key_;
    mbedtls_entropy_context  entropy_;
    mbedtls_ctr_drbg_context drbg_;
    pal::fd_t                listen_ = pal::invalid_fd;
    std::uint16_t            port_   = 0;
    std::atomic<int>         peer_gone_{0};
    std::atomic<int>         handshakes_{0};
    std::atomic<bool>        sink_intact_{false};
    std::jthread             thread_;  // last: started after everything above exists
};

// ---- a host Resumer with its own thread (as in test_rt_reactor_tcp.cpp) -----------------------------------

class ThreadResumer final : public Resumer {
public:
    ThreadResumer() : worker_([this] { loop(); }) {}
    ~ThreadResumer() override { stop(); }
    ThreadResumer(ThreadResumer const&)            = delete;
    ThreadResumer& operator=(ThreadResumer const&) = delete;

    void post(ParkedContinuation c) override {
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(c));
            ++posted_;
        }
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
    [[nodiscard]] std::thread::id id() const { return worker_.get_id(); }
    [[nodiscard]] int             posted() {
        std::lock_guard lock(m_);
        return posted_;
    }

private:
    void loop() {
        for (;;) {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
        }
    }

    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    int                            posted_ = 0;
    bool                           stop_   = false;
    std::thread                    worker_;
};

// ---- engine-call recorder ---------------------------------------------------------------------------------

struct EngineLog {
    std::mutex                   m;
    std::vector<std::thread::id> ids;
    [[nodiscard]] std::vector<std::thread::id> snapshot() {
        std::lock_guard lock(m);
        return ids;
    }
};

TlsClientOptions options_for(std::string const& host, std::string const& root_pem,
                             std::shared_ptr<EngineLog> log = {}) {
    TlsClientOptions o;
    o.hostname               = host;
    o.ca_bundle_pem_override = root_pem;
    if (log) {
        o.on_engine_call = [log] {
            std::lock_guard lock(log->m);
            log->ids.push_back(std::this_thread::get_id());
        };
    }
    return o;
}

// ---- coroutines: free functions, state by pointer -----------------------------------------------------------

task<TcpResult> connect_to(pal::TcpStream* s, std::uint16_t port) {
    co_return co_await tcp_connect(*s, pal::IpAddress::loopback_v4(), port);
}

task<TlsResult> handshake_of(TlsStream* t, std::stop_token stop = {}) { co_return co_await t->handshake(std::move(stop)); }
task<TlsResult> read_of(TlsStream* t, std::size_t n, std::stop_token stop = {}) {
    co_return co_await t->read_some(n, std::move(stop));
}
task<TlsResult> write_of(TlsStream* t, std::vector<std::byte> const* data, std::stop_token stop = {}) {
    co_return co_await t->write_all(*data, std::move(stop));
}
task<TlsResult> close_notify_of(TlsStream* t) { co_return co_await t->close_notify(); }

struct EchoTrip {
    TlsResult                    hs, write;
    std::string                  echoed;
    tls_error                    read_error = tls_error::none;
    std::vector<std::thread::id> resumed_on;
};

task<EchoTrip> handshake_and_echo(TlsStream* t, std::string msg) {
    EchoTrip e;
    e.hs = co_await t->handshake();
    e.resumed_on.push_back(std::this_thread::get_id());
    if (!e.hs.ok()) co_return e;
    auto const out = bytes_of(msg);
    e.write        = co_await t->write_all(out);
    e.resumed_on.push_back(std::this_thread::get_id());
    while (e.echoed.size() < msg.size()) {
        TlsResult r = co_await t->read_some(4096);
        e.resumed_on.push_back(std::this_thread::get_id());
        if (!r.ok()) {
            e.read_error = r.error;
            break;
        }
        e.echoed += string_of(r.data);
    }
    co_return e;
}

task<void> echo_into(TlsStream* t, std::string msg, EchoTrip* out, std::atomic<bool>* finished) {
    *out = co_await handshake_and_echo(t, std::move(msg));
    finished->store(true, std::memory_order_release);
}

task<void> read_then_flag(TlsStream* t, std::atomic<bool>* ran) {
    (void)co_await t->read_some(4096);
    ran->store(true, std::memory_order_release);
}

// Reads until `n` bytes or an error; the bytes are appended to `*out`.
task<TlsResult> read_exactly(TlsStream* t, std::size_t n, std::vector<std::byte>* out) {
    while (out->size() < n) {
        TlsResult r = co_await t->read_some(64 * 1024);
        if (!r.ok()) co_return r;
        out->insert(out->end(), r.data.begin(), r.data.end());
    }
    co_return TlsResult{};
}

struct Timed {
    TlsResult       r;
    std::thread::id resumed_on{};
};
task<Timed> timed_handshake(TlsStream* t, std::stop_token stop) {
    Timed x;
    x.r          = co_await t->handshake(std::move(stop));
    x.resumed_on = std::this_thread::get_id();
    co_return x;
}
task<Timed> timed_read(TlsStream* t, std::stop_token stop) {
    Timed x;
    x.r          = co_await t->read_some(4096, std::move(stop));
    x.resumed_on = std::this_thread::get_id();
    co_return x;
}

// A connected TCP stream plus the TLS stream over it.
struct Client {
    std::shared_ptr<pal::TcpStream> tcp;
    std::unique_ptr<TlsStream>      tls;
    bool                            connected = false;
};

Client make_client(pal::Reactor& reactor, std::uint16_t port, TlsClientOptions opts) {
    Client c;
    c.tcp       = pal::make_tcp_stream(reactor);
    c.connected = block_on(connect_to(c.tcp.get(), port)).ok();
    c.tls       = std::make_unique<TlsStream>(c.tcp, std::move(opts));
    return c;
}

// Requests a stop after `delay`; also closes `t` after `net` as a safety net (so a mutant that loses the stop
// fails a check instead of hanging the run). Joined on destruction.
class StopAfter {
public:
    StopAfter(std::stop_source src, std::chrono::milliseconds delay, TlsStream* t, std::atomic<bool>* finished)
        : stopper_([src, delay]() mutable {
              std::this_thread::sleep_for(delay);
              src.request_stop();
          }),
          net_([t, finished] {
              if (!wait_until([&] { return finished->load(); }, 5s)) t->close();
          }) {}
    ~StopAfter() {
        stopper_.join();
        net_.join();
    }
    StopAfter(StopAfter const&)            = delete;
    StopAfter& operator=(StopAfter const&) = delete;

private:
    std::thread stopper_;
    std::thread net_;
};

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(280s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 280 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main
    pal::ensure_winsock();

    // Certificates: a trusted root, a leaf for "localhost", and a leaf from an untrusted root.
    TestCa             ca;
    mbedtls_pk_context root_key = ca.generate_key();
    KeyCert const      root     = ca.issue(&root_key, "AE rt::TlsStream Test Root", &root_key, "AE rt::TlsStream Test Root", true);
    mbedtls_pk_context leaf_key = ca.generate_key();
    KeyCert const      leaf     = ca.issue(&leaf_key, "localhost", &root_key, "AE rt::TlsStream Test Root", false, "localhost");
    mbedtls_pk_context other_key = ca.generate_key();
    (void)ca.issue(&other_key, "Other Root", &other_key, "Other Root", true);
    mbedtls_pk_context stray_key = ca.generate_key();
    KeyCert const      stray     = ca.issue(&stray_key, "localhost", &other_key, "Other Root", false, "localhost");
    check(!root.cert_pem.empty() && !leaf.cert_pem.empty() && !leaf.key_pem.empty() && !stray.cert_pem.empty(),
          "setup: test CA and certificates generated in memory");

    std::thread::id const home    = std::this_thread::get_id();
    auto                  reactor = pal::make_default_reactor();

    // L1 ------------------------------------------------------------------------------------------------
    {
        TlsServer server(leaf, TlsServer::mode::echo);
        auto      log = std::make_shared<EngineLog>();
        Client    c   = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem, log));
        check(c.connected, "L1 setup: TCP connected");
        EchoTrip e = block_on(handshake_and_echo(c.tls.get(), "hello over TLS"));
        check(e.hs.ok() && c.tls->handshake_done(), "L1: the handshake succeeds against a valid certificate for localhost");
        std::printf("[info] negotiated %s\n", c.tls->protocol_version().c_str());
        check(e.write.ok() && e.write.bytes == 14, "L1: write_all sent every byte");
        check(e.echoed == "hello over TLS", "L1: the echo came back intact through TLS");
        bool home_only = !e.resumed_on.empty();
        for (auto id : e.resumed_on) home_only = home_only && id == home;
        check(home_only, "L1: every await resumed on the home (block_on) thread");
        auto const ids         = log->snapshot();
        bool       engine_home = ids.size() >= 4;
        for (auto id : ids) engine_home = engine_home && id == home;
        check(engine_home, "L1: every call into mbedTLS (" + std::to_string(ids.size()) +
                               ") ran on the home thread -- never the reactor thread");
    }

    // L2 ------------------------------------------------------------------------------------------------
    {
        TlsServer         server(leaf, TlsServer::mode::echo);
        auto              log     = std::make_shared<EngineLog>();
        Client            c       = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem, log));
        auto              resumer = std::make_shared<ThreadResumer>();
        std::atomic<bool> finished{false};
        EchoTrip          e;
        auto              t = echo_into(c.tls.get(), "via resumer", &e, &finished);
        {
            ScopedResumer scope(resumer);
            t.resume();  // raw drive under a host resumer: the first call into mbedTLS runs here, then it parks
        }
        check(wait_until([&] { return finished.load(std::memory_order_acquire); }), "L2: the echo finished");
        check(e.hs.ok() && e.echoed == "via resumer", "L2: handshake + echo under a host Resumer");
        bool on_resumer = !e.resumed_on.empty();
        for (auto id : e.resumed_on) on_resumer = on_resumer && id == resumer->id();
        check(on_resumer, "L2: every await resumed on the host Resumer's thread");
        auto const ids          = log->snapshot();
        bool       allowed      = ids.size() >= 4;
        bool       left_starter = false;  // the calls before the first park run where it was started (this thread)
        std::size_t on_resumer_n = 0;
        for (auto id : ids) {
            if (id == resumer->id()) {
                left_starter = true;
                ++on_resumer_n;
            } else {
                allowed = allowed && !left_starter && id == home;
            }
        }
        check(allowed && on_resumer_n >= 2,
              "L2: mbedTLS ran only on the coroutine's homes: the starting thread until it first parked, then "
              "the Resumer (" + std::to_string(on_resumer_n) + " of " + std::to_string(ids.size()) + " calls)");
        resumer->stop();
    }

    // L3 ------------------------------------------------------------------------------------------------
    {
        TlsServer server(leaf, TlsServer::mode::echo);
        Client    c  = make_client(*reactor, server.port(), options_for("wrong.example", root.cert_pem));
        TlsResult hs = block_on(handshake_of(c.tls.get()));
        check(hs.error == tls_error::certificate_rejected && hs.code == "net.tls_certificate_rejected",
              "L3: a certificate for another host name is rejected (" + hs.message + ")");
        check((hs.verify_flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0, "L3: ... for the name mismatch");
        check(!c.tls->handshake_done(), "L3: the stream is not usable");
        TlsResult r = block_on(read_of(c.tls.get(), 16));
        check(r.error == tls_error::closed, "L3: a later read reports `closed`");
    }

    // L4 ------------------------------------------------------------------------------------------------
    {
        TlsServer server(stray, TlsServer::mode::echo);
        Client    c  = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult hs = block_on(handshake_of(c.tls.get()));
        check(hs.error == tls_error::certificate_rejected && hs.code == "net.tls_certificate_rejected",
              "L4: a certificate from an untrusted CA is rejected (" + hs.message + ")");
        check((hs.verify_flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0, "L4: ... as not trusted");
    }

    // L5 ------------------------------------------------------------------------------------------------
    {
        TlsServer         server(leaf, TlsServer::mode::stall);
        Client            c = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        std::stop_source  src;
        std::atomic<bool> finished{false};
        auto const        t0 = std::chrono::steady_clock::now();
        Timed             x;
        {
            StopAfter s(src, 100ms, c.tls.get(), &finished);
            x = block_on(timed_handshake(c.tls.get(), src.get_token()));
            finished.store(true);
        }
        auto const dt = std::chrono::steady_clock::now() - t0;
        check(x.r.error == tls_error::canceled && x.r.tcp == pal::tcp_error::canceled,
              "L5: a stop mid-handshake resumes `canceled` (the TCP read it awaited was cancelled)");
        check(dt < 2s, "L5: ... promptly (" +
                           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()) + " ms)");
        check(x.resumed_on == home, "L5: ... on its home");
        check(wait_until([&] { return server.peer_gone() == 1; }, 3s), "L5: the stream was closed (the peer sees EOF)");
        TlsResult again = block_on(handshake_of(c.tls.get()));
        check(again.error == tls_error::closed, "L5: a later op reports `closed` (not resumable)");
    }

    // L6 ------------------------------------------------------------------------------------------------
    {
        TlsServer         server(leaf, TlsServer::mode::echo);
        Client            c = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult         hs = block_on(handshake_of(c.tls.get()));
        std::stop_source  src;
        std::atomic<bool> finished{false};
        auto const        t0 = std::chrono::steady_clock::now();
        Timed             x;
        {
            StopAfter s(src, 100ms, c.tls.get(), &finished);
            x = block_on(timed_read(c.tls.get(), src.get_token()));
            finished.store(true);
        }
        auto const dt = std::chrono::steady_clock::now() - t0;
        check(hs.ok() && x.r.error == tls_error::canceled, "L6: a stop mid-read resumes `canceled`");
        check(dt < 2s, "L6: ... promptly (" +
                           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()) + " ms)");
        check(x.resumed_on == home, "L6: ... on its home");
        check(wait_until([&] { return server.peer_gone() == 1; }, 3s), "L6: the stream was closed (the peer sees EOF)");
        auto const      payload = bytes_of("late");
        TlsResult const w       = block_on(write_of(c.tls.get(), &payload));
        check(w.error == tls_error::closed, "L6: a later write reports `closed`");
    }

    // L7 ------------------------------------------------------------------------------------------------
    {
        TlsServer server(leaf, TlsServer::mode::echo);
        Client    c       = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult hs      = block_on(handshake_of(c.tls.get()));
        auto      resumer = std::make_shared<ThreadResumer>();
        {
            std::atomic<bool> ran{false};
            auto              t = read_then_flag(c.tls.get(), &ran);
            ScopedResumer     scope(resumer);
            t.resume();
            std::this_thread::sleep_for(30ms);  // the TCP read under the TLS read is pending in the kernel
        }  // frame destroyed while the read is pending
        check(hs.ok() && wait_until([&] { return server.peer_gone() == 1; }, 3s),
              "L7: destroying the frame mid-read closed the stream (the peer sees EOF)");
        std::this_thread::sleep_for(100ms);
        check(resumer->posted() == 0, "L7: no continuation was ever posted for the destroyed frame");
        TlsResult r = block_on(read_of(c.tls.get(), 16));
        check(r.error == tls_error::closed, "L7: the handle reports `closed` afterwards");
        resumer->stop();
    }

    // L8 ------------------------------------------------------------------------------------------------
    {
        constexpr std::size_t kBig = 8u * 1024u * 1024u;
        TlsServer             server(leaf, TlsServer::mode::sink_then_send, kBig);
        Client                c  = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult             hs = block_on(handshake_of(c.tls.get()));
        std::vector<std::byte> out(kBig);
        for (std::size_t i = 0; i < kBig; ++i) out[i] = pattern_a(i);
        TlsResult w = block_on(write_of(c.tls.get(), &out));
        check(hs.ok() && w.ok() && w.bytes == kBig, "L8: 8 MiB written through TLS");
        std::vector<std::byte> in;
        in.reserve(kBig);
        TlsResult r = block_on(read_exactly(c.tls.get(), kBig, &in));
        check(wait_until([&] { return server.sink_intact(); }, 10s), "L8: the peer received all 8 MiB, every byte intact");
        bool intact = r.ok() && in.size() == kBig;
        for (std::size_t i = 0; intact && i < kBig; ++i) intact = in[i] == pattern_b(i);
        check(intact, "L8: 8 MiB read back through TLS, every byte intact");
        TlsResult end = block_on(read_of(c.tls.get(), 16));
        check(end.error == tls_error::eof, "L8: then the peer's close_notify reads as `eof`");
    }

    // L9 ------------------------------------------------------------------------------------------------
    {
        constexpr std::size_t kSize = 4u * 1024u * 1024u;
        TlsServer             server(leaf, TlsServer::mode::echo);
        auto                  log = std::make_shared<EngineLog>();
        Client                c   = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem, log));
        TlsResult             hs  = block_on(handshake_of(c.tls.get()));
        std::vector<std::byte> out(kSize);
        for (std::size_t i = 0; i < kSize; ++i) out[i] = pattern_a(i);
        std::vector<std::byte> in;  // declared before the threads that write into it
        TlsResult              read_result;
        std::thread::id        reader_id;
        std::thread            reader([&] {
            reader_id   = std::this_thread::get_id();
            read_result = block_on(read_exactly(c.tls.get(), kSize, &in));
        });
        TlsResult w = block_on(write_of(c.tls.get(), &out));
        reader.join();
        check(hs.ok() && w.ok() && w.bytes == kSize, "L9: the writer sent 4 MiB while the reader read concurrently");
        bool intact = read_result.ok() && in.size() == kSize;
        for (std::size_t i = 0; intact && i < kSize; ++i) intact = in[i] == pattern_a(i);
        check(intact, "L9: the concurrent reader got the 4 MiB echo, every byte intact");
        auto const ids        = log->snapshot();
        bool       saw_reader = false;
        bool       saw_writer = false;
        bool       only_homes = true;
        for (auto id : ids) {
            saw_reader = saw_reader || id == reader_id;
            saw_writer = saw_writer || id == home;
            only_homes = only_homes && (id == reader_id || id == home);
        }
        check(saw_reader && saw_writer && only_homes,
              "L9: mbedTLS ran on both homes (reader thread and writer thread), and only there");
    }
    {
        TlsServer         server(leaf, TlsServer::mode::echo);
        Client            c  = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult         hs = block_on(handshake_of(c.tls.get()));
        TlsResult         first;
        std::atomic<bool> first_done{false};
        std::thread       reader([&] {
            first = block_on(read_of(c.tls.get(), 4096));
            first_done.store(true);
        });
        std::this_thread::sleep_for(50ms);  // the first read is pending
        TlsResult second = block_on(read_of(c.tls.get(), 4096));
        check(hs.ok() && second.error == tls_error::busy && !first_done.load(),
              "L9: a second read while one is pending is `busy` at once");
        auto const ping = bytes_of("ping");
        TlsResult  w    = block_on(write_of(c.tls.get(), &ping));
        reader.join();
        check(w.ok() && first.ok() && string_of(first.data) == "ping",
              "L9: ... and disturbed nothing: the pending read gets the echo of a concurrent write");
    }

    // L10 -----------------------------------------------------------------------------------------------
    {
        TlsServer server(leaf, TlsServer::mode::say_bye);
        Client    c   = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult hs  = block_on(handshake_of(c.tls.get()));
        TlsResult bye = block_on(read_of(c.tls.get(), 64));
        TlsResult eof = block_on(read_of(c.tls.get(), 64));
        check(hs.ok() && bye.ok() && string_of(bye.data) == "bye", "L10: the peer's data arrives");
        check(eof.error == tls_error::eof, "L10: then its close_notify reads as `eof`");
        TlsResult cn = block_on(close_notify_of(c.tls.get()));
        check(cn.ok(), "L10: our own close_notify is sent");
        auto const more = bytes_of("more");
        TlsResult  w    = block_on(write_of(c.tls.get(), &more));
        check(w.error == tls_error::invalid_state, "L10: a write after close_notify is `invalid_state`");
    }

    // L11 -----------------------------------------------------------------------------------------------
    {
        TlsServer server(leaf, TlsServer::mode::echo);
        Client    c      = make_client(*reactor, server.port(), options_for("localhost", root.cert_pem));
        TlsResult before = block_on(read_of(c.tls.get(), 16));
        check(before.error == tls_error::invalid_state, "L11: a read before the handshake is `invalid_state`");
        std::stop_source stopped;
        stopped.request_stop();
        TlsResult pre = block_on(handshake_of(c.tls.get(), stopped.get_token()));
        check(pre.error == tls_error::canceled, "L11: a handshake with a stop already requested is `canceled`");
        EchoTrip e = block_on(handshake_and_echo(c.tls.get(), "still usable"));
        check(e.hs.ok() && e.echoed == "still usable", "L11: ... and touched nothing: the stream still works");
        TlsResult pre_read = block_on(read_of(c.tls.get(), 16, stopped.get_token()));
        auto const again   = bytes_of("again");
        TlsResult  w       = block_on(write_of(c.tls.get(), &again));
        TlsResult  r       = block_on(read_of(c.tls.get(), 64));
        check(pre_read.error == tls_error::canceled && w.ok() && r.ok() && string_of(r.data) == "again",
              "L11: a read with a stop already requested is `canceled` and leaves the stream usable");
        TlsResult second_hs = block_on(handshake_of(c.tls.get()));
        check(second_hs.error == tls_error::invalid_state, "L11: a second handshake is `invalid_state`");
    }
    {
        TlsServer server(leaf, TlsServer::mode::echo);
        Client    c  = make_client(*reactor, server.port(), options_for("", root.cert_pem));
        TlsResult hs = block_on(handshake_of(c.tls.get()));
        check(hs.error == tls_error::setup_failed && hs.code == "net.tls_setup_failed",
              "L11: an empty host name fails `setup_failed` (verification is never skipped)");
    }

    reactor.reset();
    mbedtls_pk_free(&root_key);
    mbedtls_pk_free(&leaf_key);
    mbedtls_pk_free(&other_key);
    mbedtls_pk_free(&stray_key);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
