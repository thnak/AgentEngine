// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 (TLS on the reactor; "TLS context
// ownership"; "TLS details") with §4.2/§4.4 -- `rt::TlsStream` (rt/tls.hpp) on mbedTLS, whose trust
// configuration is ADR-013's (mbedtls_client_config.cpp, shared with the blocking client). The rules -- the
// BIO, one op per direction, the process-wide engine lock never held across a co_await, close on cancel -- are
// stated in rt/tls.hpp; this file is their implementation:
//
//   - Every mbedTLS call goes through one of the small `step_*` functions below, which take the engine lock,
//     call mbedTLS once, move the ciphertext it produced out of the stream (to a buffer the TCP write will own)
//     and return. Coroutines never hold the lock: they `co_await` only outside those functions.
//   - BIO send appends to `Impl::out` and never blocks; BIO receive serves `Impl::in` (filled from TCP reads'
//     op-owned buffers, moved in, not copied) and otherwise returns WANT_READ, or 0 at TCP EOF.
//   - A `Claim` marks the op's direction(s) busy for the op's duration. If the op does not finish normally --
//     a stop mid-op, a failure, an exception, or the frame destroyed while suspended -- the claim's destructor
//     fails the stream and closes the TCP stream (§4.4: an interrupted TLS session is not resumable).
//   - `Impl` is shared by the handle and the coroutines in flight; its mbedTLS context is freed under the engine
//     lock (freeing touches PSA's key store too).

#include "agentengine/rt/tls.hpp"

#include <mbedtls/ssl.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "agentengine/rt/tcp.hpp"
#include "backends/tls_mbedtls/mbedtls_client_config.hpp"

namespace agentengine::rt {

namespace mbedtls_client = agentengine::detail::mbedtls_client;

namespace tls_detail {
std::mutex& engine_mutex() noexcept {
    static std::mutex m;
    return m;
}
}  // namespace tls_detail

namespace {

constexpr std::size_t kTcpReadChunk = 32 * 1024;  // two full TLS records
constexpr std::size_t kWriteChunk   = 16 * 1024;  // one record's plaintext per mbedtls_ssl_write

enum class phase : std::uint8_t { fresh, handshaking, ready, failed };

}  // namespace

struct TlsStream::Impl {
    std::shared_ptr<pal::TcpStream>        tcp;
    TlsClientOptions                       options;
    std::optional<mbedtls_client::Context> ctx;  // freed under the engine lock (~Impl)

    // Everything below is guarded by tls_detail::engine_mutex().
    std::vector<std::byte> in;              // ciphertext read from TCP, not yet consumed by mbedTLS
    std::size_t            in_pos = 0;
    bool                   in_eof = false;  // TCP reported EOF
    std::vector<std::byte> out;             // ciphertext mbedTLS produced, not yet handed to a TCP write
    phase                  state      = phase::fresh;
    bool                   closed     = false;  // close() / handle dropped
    bool                   reading    = false;  // a read-side op is in progress
    bool                   writing    = false;  // a write-side op is in progress
    bool                   write_shut = false;  // close_notify sent

    Impl(std::shared_ptr<pal::TcpStream> t, TlsClientOptions o) : tcp(std::move(t)), options(std::move(o)) {
        ctx.emplace();
    }
    Impl(Impl const&)            = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl() {
        std::lock_guard const lock(tls_detail::engine_mutex());
        ctx.reset();
    }

    // Engine lock held.
    void note_engine_call() const {
        if (options.on_engine_call) options.on_engine_call();
    }
    // Engine lock held. Marks the stream failed and closes the TCP stream (posted; never blocks).
    void fail_locked() noexcept {
        state = phase::failed;
        if (tcp) tcp->close();
    }
};

namespace {

using Impl = TlsStream::Impl;
using Bytes = std::shared_ptr<std::vector<std::byte>>;

// ---- the BIO: no socket, no blocking ---------------------------------------------------------------------

int bio_send(void* p, unsigned char const* buf, std::size_t len) {
    auto* im = static_cast<Impl*>(p);
    try {
        auto const* b = reinterpret_cast<std::byte const*>(buf);
        im->out.insert(im->out.end(), b, b + len);
    } catch (...) {
        return MBEDTLS_ERR_SSL_ALLOC_FAILED;
    }
    return static_cast<int>(len);  // len <= one record: mbedTLS never asks for more than its out buffer
}

int bio_recv(void* p, unsigned char* buf, std::size_t len) {
    auto* im = static_cast<Impl*>(p);
    if (im->in_pos < im->in.size()) {
        std::size_t const n = std::min(len, im->in.size() - im->in_pos);
        std::memcpy(buf, im->in.data() + im->in_pos, n);
        im->in_pos += n;
        if (im->in_pos == im->in.size()) {
            im->in.clear();
            im->in_pos = 0;
        }
        return static_cast<int>(n);
    }
    if (im->in_eof) return 0;  // mbedTLS: the transport ended
    return MBEDTLS_ERR_SSL_WANT_READ;
}

// ---- synchronous steps: the only places mbedTLS is called --------------------------------------------------

// The ciphertext waiting to go out, moved into a buffer the TCP write will own. Engine lock held.
Bytes take_out_locked(Impl& im) {
    if (im.out.empty()) return {};
    auto b = std::make_shared<std::vector<std::byte>>(std::move(im.out));
    im.out.clear();
    return b;
}

struct Step {
    int   ret = 0;
    Bytes to_send;
};

// Claims the direction(s) an op needs. Engine lock taken here. `none` on success.
tls_error claim(Impl& im, bool read, bool write, bool need_ready) {
    std::lock_guard const lock(tls_detail::engine_mutex());
    if (im.closed || im.state == phase::failed) return tls_error::closed;
    if ((read && im.reading) || (write && im.writing)) return tls_error::busy;
    if (need_ready && im.state != phase::ready) return tls_error::invalid_state;
    if (!need_ready && im.state != phase::fresh) return tls_error::invalid_state;  // a second handshake
    if (write && im.write_shut) return tls_error::invalid_state;
    if (read) im.reading = true;
    if (write) im.writing = true;
    return tls_error::none;
}

// Releases an op's directions; fails and closes the stream if the op did not finish normally (see top comment).
class Claim {
public:
    Claim(std::shared_ptr<Impl> im, bool read, bool write) noexcept : im_(std::move(im)), read_(read), write_(write) {}
    Claim(Claim const&)            = delete;
    Claim& operator=(Claim const&) = delete;
    ~Claim() {
        std::lock_guard const lock(tls_detail::engine_mutex());
        if (read_) im_->reading = false;
        if (write_) im_->writing = false;
        if (!finished_) im_->fail_locked();
    }
    // The op ended normally: the stream stays usable.
    void finish() noexcept { finished_ = true; }

private:
    std::shared_ptr<Impl> im_;
    bool                  read_;
    bool                  write_;
    bool                  finished_ = false;
};

TlsResult with_error(tls_error e) {
    TlsResult r;
    r.error = e;
    return r;
}

TlsResult from_tcp(TcpResult const& t) {
    TlsResult r;
    r.tcp   = t.error;
    r.error = t.error == pal::tcp_error::canceled ? tls_error::canceled
              : t.error == pal::tcp_error::closed ? tls_error::closed
                                                  : tls_error::transport;
    return r;
}

// Sends `bytes` (ciphertext) through the TCP stream; the buffer is the TCP operation's.
task<TcpResult> flush(Impl* im, Bytes bytes, std::stop_token stop) {
    co_return co_await tcp_detail::write_all(im->tcp.get(), std::move(bytes), std::move(stop));
}

// One TCP read into an op-owned buffer, handed to the BIO's receive side. `none` on data or EOF.
task<TcpResult> fill(Impl* im, std::stop_token stop) {
    TcpResult r = co_await tcp_read_some(*im->tcp, kTcpReadChunk, std::move(stop));
    if (r.ok() || r.error == pal::tcp_error::eof) {
        std::lock_guard const lock(tls_detail::engine_mutex());
        if (r.error == pal::tcp_error::eof) {
            im->in_eof = true;
        } else if (im->in_pos == im->in.size()) {
            im->in     = std::move(r.data);  // the op's buffer, moved in -- no copy
            im->in_pos = 0;
        } else {
            im->in.insert(im->in.end(), r.data.begin(), r.data.end());
        }
        r.error = pal::tcp_error::none;
        r.data.clear();
    }
    co_return r;
}

// ADR-013's configuration plus this stream's BIO. Takes the engine lock (setup touches the RNG and PSA).
result<void> configure_locked_scope(Impl& im) {
    std::lock_guard const lock(tls_detail::engine_mutex());
    im.note_engine_call();
    auto configured = mbedtls_client::configure(*im.ctx, im.options.hostname, im.options.ca_bundle_pem_override,
                                                "agentengine-rt-tls");
    if (!configured) return configured;
    mbedtls_ssl_set_bio(&im.ctx->ssl, &im, bio_send, bio_recv, nullptr);
    im.state = phase::handshaking;
    return {};
}

// ---- the operations ---------------------------------------------------------------------------------------

task<TlsResult> do_handshake(std::shared_ptr<Impl> im, std::stop_token stop) {
    if (stop.stop_requested()) co_return with_error(tls_error::canceled);
    if (tls_error const e = claim(*im, true, true, false); e != tls_error::none) co_return with_error(e);
    Claim claimed(im, true, true);

    if (im->options.hostname.empty()) {  // options are immutable: no lock needed
        TlsResult r = with_error(tls_error::setup_failed);
        r.message   = "TLS requires the server's host name to verify its certificate against";
        r.code      = "net.tls_setup_failed";
        co_return r;  // ~Claim fails the stream
    }
    if (auto configured = configure_locked_scope(*im); !configured) {
        TlsResult r = with_error(tls_error::setup_failed);
        r.message   = configured.error().message;
        r.code      = configured.error().code;
        co_return r;
    }

    for (;;) {
        Step s;
        {
            std::lock_guard const lock(tls_detail::engine_mutex());
            im->note_engine_call();
            s.ret     = mbedtls_ssl_handshake(&im->ctx->ssl);
            s.to_send = take_out_locked(*im);
        }
        // Flush first: the ClientHello before waiting for the ServerHello, and an alert before reporting a failure.
        if (s.to_send) {
            TcpResult const w = co_await flush(im.get(), std::move(s.to_send), stop);
            if (!w.ok()) co_return from_tcp(w);
        }
        if (s.ret == 0) {
            std::lock_guard const lock(tls_detail::engine_mutex());
            im->state = phase::ready;
            claimed.finish();
            co_return TlsResult{};
        }
        if (s.ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;  // the BIO never blocks; nothing to wait for
        if (s.ret == MBEDTLS_ERR_SSL_WANT_READ) {
            TcpResult const r = co_await fill(im.get(), stop);
            if (!r.ok()) co_return from_tcp(r);
            continue;
        }
        TlsResult r;
        {
            std::lock_guard const lock(tls_detail::engine_mutex());
            ae::error const e = mbedtls_client::handshake_error(im->ctx->ssl, s.ret);
            r.verify_flags    = mbedtls_ssl_get_verify_result(&im->ctx->ssl);
            r.message         = e.message;
            r.code            = e.code;
        }
        r.error       = r.code == "net.tls_certificate_rejected" ? tls_error::certificate_rejected
                                                                 : tls_error::handshake_failed;
        r.engine_code = s.ret;
        co_return r;
    }
}

task<TlsResult> do_read(std::shared_ptr<Impl> im, std::size_t max_bytes, std::stop_token stop) {
    if (stop.stop_requested()) co_return with_error(tls_error::canceled);
    if (max_bytes == 0) co_return with_error(tls_error::invalid_state);
    if (tls_error const e = claim(*im, true, false, true); e != tls_error::none) co_return with_error(e);
    Claim claimed(im, true, false);

    std::vector<std::byte> plain(max_bytes);
    for (;;) {
        int ret = 0;
        {
            std::lock_guard const lock(tls_detail::engine_mutex());
            im->note_engine_call();
            ret = mbedtls_ssl_read(&im->ctx->ssl, reinterpret_cast<unsigned char*>(plain.data()), plain.size());
        }
        if (ret > 0) {
            plain.resize(static_cast<std::size_t>(ret));
            TlsResult r;
            r.bytes = plain.size();
            r.data  = std::move(plain);
            claimed.finish();
            co_return r;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            claimed.finish();  // the peer is done sending; writing may continue
            co_return with_error(tls_error::eof);
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_CONN_EOF) co_return with_error(tls_error::truncated);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            TcpResult const r = co_await fill(im.get(), stop);
            if (!r.ok()) co_return from_tcp(r);
            continue;
        }
        // WANT_WRITE cannot happen (the BIO never blocks), and anything else ends the session.
        TlsResult r   = with_error(tls_error::protocol);
        r.engine_code = ret;
        r.message     = "TLS read failed: " + mbedtls_client::error_string(ret);
        co_return r;
    }
}

task<TlsResult> do_write(std::shared_ptr<Impl> im, Bytes plain, std::stop_token stop) {
    if (stop.stop_requested()) co_return with_error(tls_error::canceled);
    if (tls_error const e = claim(*im, false, true, true); e != tls_error::none) co_return with_error(e);
    Claim claimed(im, false, true);

    std::size_t sent = 0;
    while (sent < plain->size()) {
        Step              s;
        std::size_t const len = std::min(kWriteChunk, plain->size() - sent);
        {
            std::lock_guard const lock(tls_detail::engine_mutex());
            im->note_engine_call();
            s.ret = mbedtls_ssl_write(&im->ctx->ssl, reinterpret_cast<unsigned char const*>(plain->data() + sent),
                                      len);
            s.to_send = take_out_locked(*im);  // this chunk's records, after any alert a read queued (in order)
        }
        if (s.ret <= 0) {  // 0 for a non-empty chunk would loop forever; WANT_* cannot happen (see rt/tls.hpp)
            TlsResult r   = with_error(tls_error::protocol);
            r.bytes       = sent;
            r.engine_code = s.ret;
            r.message     = "TLS write failed: " + mbedtls_client::error_string(s.ret);
            co_return r;
        }
        if (s.to_send) {
            TcpResult const w = co_await flush(im.get(), std::move(s.to_send), stop);
            if (!w.ok()) {
                TlsResult r = from_tcp(w);
                r.bytes     = sent;
                co_return r;
            }
        }
        sent += static_cast<std::size_t>(s.ret);
    }
    claimed.finish();
    TlsResult r;
    r.bytes = sent;
    co_return r;
}

task<TlsResult> do_close_notify(std::shared_ptr<Impl> im, std::stop_token stop) {
    if (stop.stop_requested()) co_return with_error(tls_error::canceled);
    if (tls_error const e = claim(*im, false, true, true); e != tls_error::none) co_return with_error(e);
    Claim claimed(im, false, true);
    Step  s;
    {
        std::lock_guard const lock(tls_detail::engine_mutex());
        im->note_engine_call();
        s.ret     = mbedtls_ssl_close_notify(&im->ctx->ssl);
        s.to_send = take_out_locked(*im);
    }
    if (s.ret != 0) {
        TlsResult r   = with_error(tls_error::protocol);
        r.engine_code = s.ret;
        r.message     = "TLS close_notify failed: " + mbedtls_client::error_string(s.ret);
        co_return r;
    }
    if (s.to_send) {
        TcpResult const w = co_await flush(im.get(), std::move(s.to_send), stop);
        if (!w.ok()) co_return from_tcp(w);
    }
    {
        std::lock_guard const lock(tls_detail::engine_mutex());
        im->write_shut = true;
    }
    claimed.finish();
    co_return TlsResult{};
}

}  // namespace

// ---- the handle -------------------------------------------------------------------------------------------

TlsStream::TlsStream(std::shared_ptr<pal::TcpStream> tcp, TlsClientOptions options)
    : impl_(std::make_shared<Impl>(std::move(tcp), std::move(options))) {}

TlsStream::~TlsStream() { close(); }

TlsStream::TlsStream(TlsStream&&) noexcept = default;

TlsStream& TlsStream::operator=(TlsStream&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

task<TlsResult> TlsStream::handshake(std::stop_token stop) { return do_handshake(impl_, std::move(stop)); }

task<TlsResult> TlsStream::read_some(std::size_t max_bytes, std::stop_token stop) {
    return do_read(impl_, max_bytes, std::move(stop));
}

task<TlsResult> TlsStream::write_all(std::span<std::byte const> bytes, std::stop_token stop) {
    return do_write(impl_, std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end()), std::move(stop));
}

task<TlsResult> TlsStream::close_notify(std::stop_token stop) { return do_close_notify(impl_, std::move(stop)); }

void TlsStream::close() noexcept {
    if (!impl_) return;
    std::lock_guard const lock(tls_detail::engine_mutex());
    impl_->closed = true;
    if (impl_->tcp) impl_->tcp->close();
}

bool TlsStream::handshake_done() const noexcept {
    std::lock_guard const lock(tls_detail::engine_mutex());
    return impl_ && !impl_->closed && impl_->state == phase::ready;
}

pal::TcpStream& TlsStream::tcp() const noexcept { return *impl_->tcp; }

std::string TlsStream::protocol_version() const {
    std::lock_guard const lock(tls_detail::engine_mutex());
    if (!impl_ || impl_->state != phase::ready) return {};
    return mbedtls_ssl_get_version(&impl_->ctx->ssl);
}

}  // namespace agentengine::rt
