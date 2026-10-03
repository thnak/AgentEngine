#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("TLS: keep mbedTLS (ADR-013)";
// "HTTP/1.1 + SSE ... rewritten as coroutines") -- the https transport of rt/http.hpp: `rt::tls_connector()`
// wraps the connected TCP stream in an `rt::TlsStream` (rt/tls.hpp) and hands rt/http.hpp an `HttpTransport`.
//
//   HttpContext ctx{.reactor = ..., .dns_pool = ..., .policy = ..., .tls = rt::tls_connector()};
//   co_await rt::http_request(ctx, {.scheme = http_scheme::https, .host = "api.example.com", ...}, stop);
//
// Only usable with AGENTENGINE_WITH_HTTPS: it links `agentengine::rt_tls` (mbedTLS). rt/http.hpp itself never
// includes this header, so a build without HTTPS has the plain client and the SSE decoder, and an https request
// there is `tls_unavailable` (the blocking client's `net.scheme_unsupported`), refused before any connection.
//
// TRUST is rt::TlsStream's, unchanged (ADR-013): vendored CA bundle, verification required, TLS 1.2 floor, and
// the certificate verified against the request's ORIGINAL host name -- `HttpRequest::host`, never the address the
// TCP stream connected to (ADR-011), never re-resolved. `TlsConnectorOptions::ca_bundle_pem_override` is the same
// test seam as the blocking client's and TlsStream's; production code leaves it empty.
//
// EOF: a TLS read that ends WITHOUT close_notify (rt::tls_error::truncated) is reported to rt/http.hpp as an
// unclean EOF. A body framed by Content-Length or chunked is complete when its framing is (many servers skip
// close_notify); a close-delimited body that ends that way is `truncated` -- the blocking client's read loop
// fails the same case.

#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tls.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow TlsConnectorOptions — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct TlsConnectorOptions {
    std::string ca_bundle_pem_override;  // test seam (a synthetic root); production leaves it empty
};

namespace http_detail {

[[nodiscard]] inline char const* tls_error_name(tls_error e) noexcept {
    switch (e) {
        case tls_error::none: return "none";
        case tls_error::canceled: return "canceled";
        case tls_error::busy: return "busy";
        case tls_error::closed: return "closed";
        case tls_error::invalid_state: return "invalid state";
        case tls_error::eof: return "eof";
        case tls_error::truncated: return "truncated";
        case tls_error::certificate_rejected: return "certificate rejected";
        case tls_error::handshake_failed: return "handshake failed";
        case tls_error::setup_failed: return "setup failed";
        case tls_error::protocol: return "protocol error";
        case tls_error::transport: return "transport error";
    }
    return "other";
}

class TlsTransport final : public HttpTransport {
public:
    TlsTransport(TlsStream tls, std::shared_ptr<pal::TcpStream> tcp) noexcept
        : tls_(std::move(tls)), tcp_(std::move(tcp)) {}
    ~TlsTransport() override { close(); }

    // TlsStream's operations run on its shared state, so this object may be dropped while one is in flight.
    task<HttpIo> read_some(std::size_t max_bytes, std::stop_token stop) override {
        return map_read(tls_.read_some(max_bytes, std::move(stop)));
    }
    task<HttpIo> write_all(std::vector<std::byte> bytes, std::stop_token stop) override {
        return map_write(tls_.write_all(bytes, std::move(stop)));  // copied eagerly by TlsStream
    }
    void close() noexcept override { tls_.close(); }
    [[nodiscard]] std::weak_ptr<pal::TcpStream> tcp() const noexcept override { return tcp_; }

private:
    static task<HttpIo> map_read(task<TlsResult> t) {
        TlsResult r = co_await t;
        HttpIo    out;
        switch (r.error) {
            case tls_error::none: out.data = std::move(r.data); break;
            case tls_error::eof: out.status = http_io::eof; break;
            case tls_error::truncated:
                out.status      = http_io::eof;
                out.unclean_eof = true;
                break;
            case tls_error::canceled: out.status = http_io::canceled; break;
            default:
                out.status  = http_io::failed;
                out.code    = "net.stream_read_failed";
                out.message = std::string("TLS read failed: ") + tls_error_name(r.error) +
                              (r.message.empty() ? std::string() : ": " + r.message);
                break;
        }
        co_return out;
    }
    static task<HttpIo> map_write(task<TlsResult> t) {
        TlsResult r = co_await t;
        HttpIo    out;
        if (r.error == tls_error::canceled) {
            out.status = http_io::canceled;
        } else if (!r.ok()) {
            out.status  = http_io::failed;
            out.code    = "net.connect_failed";  // the blocking client's code for a failed TLS write
            out.message = std::string("TLS write failed: ") + tls_error_name(r.error);
        }
        co_return out;
    }

    TlsStream                       tls_;
    std::shared_ptr<pal::TcpStream> tcp_;
};

inline task<HttpTlsHandshake> tls_handshake(std::shared_ptr<pal::TcpStream> tcp, std::string hostname,
                                            std::string ca_override, std::stop_token stop) {
    TlsClientOptions o;
    o.hostname               = std::move(hostname);
    o.ca_bundle_pem_override = std::move(ca_override);
    TlsStream       tls(tcp, std::move(o));
    task<TlsResult> t = tls.handshake(std::move(stop));
    TlsResult       r = co_await t;
    HttpTlsHandshake out;
    if (r.ok()) {
        out.status    = http_io::ok;
        out.transport = std::make_shared<TlsTransport>(std::move(tls), std::move(tcp));
    } else if (r.error == tls_error::canceled) {
        out.status = http_io::canceled;
    } else {
        out.status  = http_io::failed;
        out.code    = r.code.empty() ? std::string("net.tls_handshake_failed") : r.code;
        out.message = r.message.empty() ? std::string("TLS handshake failed: ") + tls_error_name(r.error) : r.message;
    }
    co_return out;
}

}  // namespace http_detail

// The https step for `HttpContext::tls`.
[[nodiscard]] inline HttpTlsConnector tls_connector(TlsConnectorOptions options = {}) {
    return [ca = std::move(options.ca_bundle_pem_override)](std::shared_ptr<pal::TcpStream> tcp, std::string hostname,
                                                            std::stop_token stop) -> task<HttpTlsHandshake> {
        return http_detail::tls_handshake(std::move(tcp), std::move(hostname), ca, std::move(stop));
    };
}

}  // namespace agentengine::rt
