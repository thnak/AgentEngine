#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("TLS: keep mbedTLS (ADR-013). Its
// non-blocking BIO callbacks are driven by reactor socket readiness ... Handshake, read and write are all
// cancellable"; "TLS context ownership"; "TLS details") with §4.2/§4.4's rules -- `rt::TlsStream`, an
// asynchronous TLS client over an already-connected `pal::TcpStream`, on mbedTLS (D1 keeps mbedTLS for TLS):
//
//   rt::TlsStream tls(connected_tcp, {.hostname = "api.example.com"});
//   co_await tls.handshake(stop);   co_await tls.write_all(bytes, stop);
//   co_await tls.read_some(n, stop); co_await tls.close_notify(stop);
//
// Only built with AGENTENGINE_WITH_HTTPS (the `agentengine::rt_tls` library; its implementation is
// src/backends/tls_mbedtls/tls_stream_mbedtls.cpp). This header names no mbedTLS type.
//
// TRUST (ADR-013, unchanged): the same configuration as the blocking `sandbox::TlsClientSession` -- one shared
// helper (src/backends/tls_mbedtls/mbedtls_client_config.cpp): the vendored, compiled-in CA bundle, certificate
// verification REQUIRED, a TLS 1.2 floor, and real hostname verification against `TlsClientOptions::hostname`,
// the ORIGINAL name -- never the numeric address TCP connected to, and never re-resolved (ADR-011). Neither can
// be turned off; `ca_bundle_pem_override` is a test seam (a synthetic root), as in the blocking client.
//
// THE BIO (§6.3): mbedTLS never touches a socket. Its send callback appends ciphertext to an outgoing buffer and
// never blocks; its receive callback hands out ciphertext already read and otherwise returns
// MBEDTLS_ERR_SSL_WANT_READ. The coroutine then performs the I/O itself on the reactor: the pending ciphertext
// goes out through rt/tcp.hpp's write (a buffer the TCP operation owns -- §6.3 round-3 gap 6), and a WANT_READ
// becomes `co_await tcp_read_some` into an op-owned buffer that is then handed to the receive callback. So every
// mbedTLS call runs on the AWAITING COROUTINE'S HOME THREAD (its ADR-175 `block_on` home or ADR-219 host
// `Resumer`), never on the reactor thread and never on an offload worker.
//
// CONCURRENCY -- the rules this type enforces:
//   - ONE OPERATION PER DIRECTION. At most one read-side op (read_some) and one write-side op (write_all,
//     close_notify) at a time; the handshake holds both. A second op in a busy direction completes at once with
//     `tls_error::busy` (as rt/tcp.hpp's `busy`) -- it is not queued.
//   - A READ AND A WRITE MAY RUN CONCURRENTLY, from two coroutines, even on two different home threads (full
//     duplex: an SSE reader and a request writer). They are serialised where they share state, and only there:
//     every mbedTLS call -- and every touch of the stream's buffers and flags -- happens under one lock, held
//     only for that synchronous call (mbedTLS never blocks inside it, the BIO returns WANT_READ instead) and
//     NEVER across a `co_await`. At the TCP level the two directions are independent (rt/tcp.hpp allows one read
//     and one write outstanding). Only write-side ops send: ciphertext a read produces (an alert) waits in the
//     outgoing buffer for the next write-side op. A TLS 1.3 client's ssl_read produces no other output in
//     mbedTLS 3.6 (no KeyUpdate, no post-handshake auth; renegotiation is off).
//   - THE LOCK IS PROCESS-WIDE (`tls_detail::engine_mutex()`), not per stream: mbedTLS 3.6's TLS 1.3 code keeps
//     its keys in PSA's global key store, which is not thread-safe without MBEDTLS_THREADING_C -- off in the
//     vendored build (checked 2026-10-03, ADR-237 §6.3 "the current config is to be checked") -- so two streams
//     handshaking or encrypting on two threads would race inside PSA. Cost: mbedTLS work is serialised across all
//     TlsStreams of the process (the I/O is not). Enabling MBEDTLS_THREADING_C would let this become per stream;
//     that is a build-configuration decision, not taken here. Anything else in the process calling mbedTLS
//     concurrently must take the same lock -- the blocking `sandbox::TlsClientSession` does NOT (its BIO blocks
//     inside mbedTLS calls, so it cannot hold a shared lock): a pre-existing race, reported, not fixed here.
//
// CANCELLATION (§4.4): every operation takes a stop token. A stop already requested when an op is called
// completes it `canceled` without touching anything (the stream stays usable). A stop that lands while the op
// is under way cancels the TCP operation it is awaiting (two-phase, rt/tcp.hpp: the waiter resumes from the
// backend's own completion, promptly, on its home) and then CLOSES the stream: a TLS session interrupted
// mid-handshake or mid-record is not resumable (§6.3 "a write canceled mid-record drops the connection"), so
// every later op completes `closed`. The same holds for a coroutine frame destroyed mid-op (its TCP op is
// abandoned and cancelled; nothing is posted for the dead frame): the stream is closed.
//
// LIFETIME: `TlsStream` is a movable handle; its state is shared with the operations in flight, so dropping or
// moving the handle mid-op is safe -- dropping it closes the TCP stream, and the pending op completes (`closed`
// or `canceled`). A moved-from handle must not be used. The reactor must outlive every op (rt/tcp.hpp's rule).
//
// Results are values (001 §6): `TlsResult::error`, plus the blocking client's `message`/`code` for setup and
// handshake failures (`net.tls_setup_failed`, `net.tls_certificate_rejected`, `net.tls_handshake_failed`).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow tls_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class tls_error : std::uint8_t {
    none,                  // success
    canceled,              // the stop token (or the reactor shutting down) ended it; the stream is closed unless
                           // the stop was already requested when the op was called
    busy,                  // another op of the same direction is outstanding (nothing was touched)
    closed,                // the stream was closed (close(), a dropped handle, an earlier failure or cancel)
    invalid_state,         // read/write before a successful handshake, a second handshake, a write after
                           // close_notify, a zero-byte read
    eof,                   // the peer sent close_notify: no more data will come (read side only)
    truncated,             // the TCP stream ended without close_notify (a truncation attack or a crashed peer)
    certificate_rejected,  // the peer's certificate failed verification (chain or hostname); `message` says how
    handshake_failed,      // any other handshake failure; `message` / `engine_code` say how
    setup_failed,          // the TLS context could not be configured (RNG, CA bundle, no hostname)
    protocol,              // a record-layer error after the handshake (bad MAC, unexpected message, alert)
    transport,             // the TCP operation failed; `tcp` says how
};

// ae-naming-lint: allow TlsResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct TlsResult {
    tls_error              error = tls_error::none;
    std::size_t            bytes = 0;  // read: data.size(); write_all: plaintext bytes sent, also on failure
    std::vector<std::byte> data;       // read_some only
    pal::tcp_error         tcp         = pal::tcp_error::none;  // the TCP error behind `transport` / `canceled`
    int                    engine_code = 0;                     // the mbedTLS return code, when there was one
    std::uint32_t          verify_flags = 0;  // mbedTLS's certificate verification flags (certificate_rejected)
    std::string            message;           // human-readable detail (setup / handshake failures)
    std::string            code;              // the blocking client's stable code, when one applies

    [[nodiscard]] bool ok() const noexcept { return error == tls_error::none; }
};

// ae-naming-lint: allow TlsClientOptions — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct TlsClientOptions {
    // The ORIGINAL host name the server certificate is verified against (RFC 6125). Required: an empty name
    // fails the handshake `setup_failed` rather than skipping the check.
    std::string hostname;
    // Test seam (as `sandbox::TlsClientSession::handshake`'s): a PEM root set used INSTEAD of the vendored bundle.
    // Production code leaves it empty.
    std::string ca_bundle_pem_override;
    // Diagnostics / test seam: called (under the engine lock, on the calling thread) immediately before every
    // call into mbedTLS -- how the tests prove mbedTLS only ever runs on the awaiting coroutine's home thread.
    std::function<void()> on_engine_call;
};

// ae-naming-lint: allow TlsStream — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class TlsStream {
public:
    // `tcp` must already be connected (rt::connect_resolved / rt::tcp_connect) and is owned from here on.
    TlsStream(std::shared_ptr<pal::TcpStream> tcp, TlsClientOptions options);
    ~TlsStream();  // closes the TCP stream (see LIFETIME)
    TlsStream(TlsStream&&) noexcept;
    TlsStream& operator=(TlsStream&&) noexcept;
    TlsStream(TlsStream const&)            = delete;
    TlsStream& operator=(TlsStream const&) = delete;

    // Configures the client (ADR-013) and runs the handshake. Once, before anything else.
    [[nodiscard]] task<TlsResult> handshake(std::stop_token stop = {});
    // At least one byte of application data (at most `max_bytes`), or `eof` / an error.
    [[nodiscard]] task<TlsResult> read_some(std::size_t max_bytes, std::stop_token stop = {});
    // Encrypts and sends every byte. `bytes` is copied once, eagerly: the span need not outlive this call.
    [[nodiscard]] task<TlsResult> write_all(std::span<std::byte const> bytes, std::stop_token stop = {});
    // Sends close_notify (a write-side op). Reading stays possible; further writes are `invalid_state`.
    [[nodiscard]] task<TlsResult> close_notify(std::stop_token stop = {});
    // Closes the TCP stream (posted, never blocks): ops in flight complete `closed` (or `canceled`). Idempotent.
    void close() noexcept;

    [[nodiscard]] bool               handshake_done() const noexcept;  // a handshake succeeded and nothing failed since
    [[nodiscard]] pal::TcpStream&    tcp() const noexcept;
    // The negotiated protocol version ("TLSv1.3", "TLSv1.2"), or "" before a handshake.
    [[nodiscard]] std::string        protocol_version() const;

    struct Impl;  // public only so the implementation's free coroutines can name it

private:
    std::shared_ptr<Impl> impl_;
};

namespace tls_detail {
// The process-wide lock every mbedTLS call made by a TlsStream runs under (see CONCURRENCY). Exposed so other
// in-process mbedTLS users (a test's loopback server) can serialise with it; never hold it across a blocking call.
[[nodiscard]] std::mutex& engine_mutex() noexcept;
}  // namespace tls_detail

}  // namespace agentengine::rt
