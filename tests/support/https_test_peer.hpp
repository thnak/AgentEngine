#pragma once
// Test support: an in-memory test CA (ECDSA P-256) and a loopback mbedTLS HTTPS peer, shared by tests/rt/test_rt_https.cpp
// (ADR-237 SS6.3) and the ADR-241 G11 TLS rows (tests/sandbox/test_net_egress_tls.cpp). Factored out of test_rt_https.cpp
// verbatim (ADR-241 follow-up) so the two cannot drift. Test-only: needs mbedTLS headers and AGENTENGINE_WITH_HTTPS.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"

#if !defined(_WIN32)
#include <sys/select.h>
#endif

namespace agentengine::test_support {

using namespace std::chrono_literals;

constexpr std::uint64_t kLoopback = 0x7F000001;

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
                  std::string const& issuer, bool is_ca, std::string const& san = {},
                  std::string const& ip_san_bytes = {}) {
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
        // A DNS-name SAN and/or an IP-address SAN (raw bytes: 4 for v4), at most one of each.
        mbedtls_x509_san_list dns_node{};
        mbedtls_x509_san_list ip_node{};
        mbedtls_x509_san_list* head = nullptr;
        if (!ip_san_bytes.empty()) {
            ip_node.node.type                      = MBEDTLS_X509_SAN_IP_ADDRESS;
            ip_node.node.san.unstructured_name.p   = reinterpret_cast<unsigned char*>(const_cast<char*>(ip_san_bytes.data()));
            ip_node.node.san.unstructured_name.len = ip_san_bytes.size();
            ip_node.next                           = head;
            head                                   = &ip_node;
        }
        if (!san.empty()) {
            dns_node.node.type                      = MBEDTLS_X509_SAN_DNS_NAME;
            dns_node.node.san.unstructured_name.p   = reinterpret_cast<unsigned char*>(const_cast<char*>(san.data()));
            dns_node.node.san.unstructured_name.len = san.size();
            dns_node.next                           = head;
            head                                    = &dns_node;
        }
        if (head != nullptr) mbedtls_x509write_crt_set_subject_alternative_name(&ctx, head);
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
// mbedTLS is thread-safe in this build (MBEDTLS_THREADING_C, ADR-237 §6.3), so the peer needs no lock of its
// own to run alongside the client; only readiness waits bound each WANT_*.

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
            int const rc = f();
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
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_own_cert(&conf, &cert_, &key_);
        mbedtls_ssl_setup(&ssl, &conf);
        mbedtls_ssl_set_bio(&ssl, &fd, server_send, server_recv, nullptr);
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

}  // namespace agentengine::test_support
