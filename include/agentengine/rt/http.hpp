#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("HTTP/1.1 + SSE: keep the in-house
// client ... rewritten as coroutines. select() disappears"), with §4.4 (deadlines are timers; per-operation stop
// sources; two-phase cancel), §4.5 rule 5 (the consumer-stall deadline fails the STREAM with `resource`) and
// §6.3's round-3 gaps 6/7 (op-owned buffers; one streamed read outstanding) -- the asynchronous HTTP/1.1 client
// and its SSE layer, on the reactor:
//
//   HttpResponse       r = co_await rt::http_request(ctx, request, stop);        // buffered, body size-capped
//   HttpStreamResponse s = co_await rt::http_stream(ctx, request, stop);         // head, then a body reader
//   HttpBodyChunk      c = co_await s.body.next(stop);                           // data / end / error
//   SseStream        sse = rt::sse_events(std::move(s.body));
//   SseNext            e = co_await sse.next(stop);                              // event / end / error
//
// The wire format (request building, head parsing, framing, SSE) is rt/http_wire.hpp; this header only drives it.
// It replaces, later, the blocking `perform_http_exchange[_streaming]` / `perform_https_exchange[_streaming]`
// (sandbox/net_egress_proxy.cpp) and `pump_sse_stream` (src/protocol/sse_stream_pump.hpp); the switch-over of
// their callers is a later step. What it keeps from them, and where it deliberately differs:
//
//   SAME
//   - ADR-011 resolve-once, connect-to-the-verified-address: the connection is made ONLY by `rt::connect_resolved`
//     with the caller's `AddressPolicy` (`HttpContext::policy`), which fails closed -- no policy is `no_policy`
//     before any lookup (I2), a throwing policy rejects, no accepted address means nothing is connected. The
//     guest path's blocked-range table (ADR-011 C4) and the provider path's "allow private addresses" (ADR-016)
//     are both just policies the caller passes; this client has no default.
//   - NO REDIRECT IS FOLLOWED (ADR-011 claim C10, §3 Design B rejected): a 3xx is returned exactly as received.
//   - TLS (ADR-013) is `rt::TlsStream`, so the vendored CA bundle, verification REQUIRED, the TLS 1.2 floor and
//     hostname verification against the ORIGINAL host name (never the address) are the blocking client's. It is
//     plugged in through `HttpContext::tls` (rt/http_tls.hpp, only with AGENTENGINE_WITH_HTTPS); without it an
//     https request is `tls_unavailable` before any connection (the blocking client's `net.scheme_unsupported`).
//   - `Connection: close` on every request (one exchange per connection). Keep-alive pooling is NOT implemented:
//     §6.3 names it a performance follow-on, not a correctness requirement.
//   - The body byte cap: min(`HttpLimits::max_body_bytes`, 16 MiB hard ceiling) on the body's WIRE bytes, enforced
//     during the read loop, never after buffering (ADR-011 claim C8), for buffered AND streamed bodies (the
//     blocking streaming path caps them too). `body_too_large` carries `net.byte_cap_exceeded`.
//   - A 2xx/4xx body cut short of its Content-Length is `truncated` (`net.stream_truncated`); a head cut short is
//     `truncated` with `net.protocol_error`; the status and headers stay readable on the result.
//   - The idle bound defaults to the blocking client's 90 s (`sandbox::kDefaultIoTimeoutMs`); the switch-over passes
//     `sandbox::io_timeout_ms()` (rt/ is L0 and may not read that L1 setting itself).
//   STRICTER (all request-smuggling / injection hardening, see rt/http_wire.hpp)
//   - CR/LF/NUL injection is rejected at build time for EVERY field, on every path -- the blocking client checks it
//     only on the guest path (HostEgressProxy::fetch), trusting provider callers.
//   - A caller-supplied Host / Content-Length / Transfer-Encoding / Connection is rejected; the blocking client
//     forwarded a caller Content-Length unchecked.
//   - Head: CRLF only, no obs-fold, no whitespace before a colon, header bytes and count capped (the blocking
//     client capped only the total). Framing: CL + TE, conflicting CLs and any TE but exactly `chunked` rejected
//     (the blocking client took the first CL and any TE containing "chunked").
//   - `Host:` carries the port when it is not the scheme's default (RFC 9110 §7.2); the blocking client never did.
//   - Cancellation is `canceled` (a value), not `transient` "net.cancelled".
//   - An https body framed by "until close" that ends WITHOUT close_notify is `truncated` (as the blocking client's
//     read error); a body framed by Content-Length / chunked is complete when its framing is, so a server that
//     skips close_notify after a complete body is fine on both.
//
// DEADLINES ARE TIMERS (§4.4). Every connect/handshake/write/read runs under its own `std::stop_source` (per-
// operation, red-team M5), stopped by: the caller's token; the request's OVERALL deadline
// (`HttpOptions::total_timeout`, 0 = none), a reactor timer stopping a request-level source; and that operation's
// IDLE deadline (`idle_timeout`), a reactor timer of its own. The stop cancels the TCP/TLS operation two-phase
// (rt/tcp.hpp: the waiter resumes from the backend's own completion, on its home), the stream is closed, and the
// outcome is a value: `canceled`, `deadline_exceeded` or `idle_timeout` -- in that precedence. DEVIATION, the
// same one rt/dns.hpp records: M5 posts a timer-driven stop to the owning strand; here the timers request it on
// the reactor thread. Safe for the same reason: the sources are private to this client and the only callbacks
// ever registered on them forward to another private source or are the TCP awaiter's `Canceler`, which only
// posts a cancel -- no engine code runs on the reactor thread (§4.2).
//
// STREAMING (§6.3 round-3 gaps 6/7). A reader has at most ONE read outstanding: a second `next()` while one is
// pending completes `busy` at once and touches nothing. Each read lands in a buffer the TCP operation owns
// (rt/tcp.hpp), never in a coroutine frame. NOT YET: the reactor re-arming the next read into a POOLED ring buffer
// ahead of the consumer, and waking the strand once per batch -- each `next()` here starts its own read (one
// allocation per read). That is the §8.2 gate-8 performance step, behind the same API.
//
// CONSUMER-STALL DEADLINE (§4.5 rule 5.3: "fails the stream with `resource`, not the run"). After `http_stream`
// returns the head, and after every chunk/event handed to the consumer, a reactor timer
// (`HttpOptions::consumer_stall_timeout`, 0 = none) starts; asking for the next chunk/event cancels it. If it
// fires first, the connection is closed (from the reactor thread: `pal::TcpStream::close` only posts) and the
// next call -- and every later one -- returns `consumer_stalled` (`net.consumer_stalled`, a `resource` error).
// A consumer that is actively draining queued SSE events is not stalled: the SSE layer re-arms per event.
//
// LIFETIME. `HttpBodyReader` and `SseStream` are movable handles on shared state; a `next()` in flight keeps the
// state alive, so dropping or moving a handle mid-read is safe. A coroutine frame destroyed while its `next()` is
// pending abandons the TCP read (nothing is posted for the dead frame, rt/tcp.hpp) and closes the stream. The
// reactor and the DNS pool must outlive every request and reader (rt/tcp.hpp's and rt/dns.hpp's rule); the last
// handle dropped closes the connection and cancels its timers.
//
// THREADS. Every await resumes on the caller's home (rt/reactor_await.hpp); the address policy and every byte of
// parsing run there; TLS runs where rt/tls.hpp says. Coroutines are free functions over shared state (ADR-237
// §14: no lambda coroutines with captures).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/dns.hpp"
#include "agentengine/rt/http_wire.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/reactor_await.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tcp.hpp"

namespace agentengine::rt {

// ---- the transport seam (plain TCP here; TLS in rt/http_tls.hpp) -----------------------------------------------

// ae-naming-lint: allow http_io — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class http_io : std::uint8_t { ok, eof, canceled, failed };

// ae-naming-lint: allow HttpIo — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpIo {
    http_io                status = http_io::ok;
    std::vector<std::byte> data;                 // read only
    bool                   unclean_eof = false;  // TLS: the TCP stream ended without close_notify
    std::string            code;                 // `failed`: a stable net.* code
    std::string            message;
};

// One connection's byte stream. The declared type-erasure seam of this client (CONVENTIONS: "type erasure only at
// declared seams"): one virtual call per I/O operation, beside a system call. At most one read and one write
// outstanding (the HTTP layer never overlaps them). A read or write that ends `canceled` leaves the connection
// closed (rt/tcp.hpp, rt/tls.hpp).
// ae-naming-lint: allow HttpTransport — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    HttpTransport(HttpTransport const&)            = delete;
    HttpTransport& operator=(HttpTransport const&) = delete;

    [[nodiscard]] virtual task<HttpIo> read_some(std::size_t max_bytes, std::stop_token stop) = 0;
    [[nodiscard]] virtual task<HttpIo> write_all(std::vector<std::byte> bytes, std::stop_token stop) = 0;
    // Posted, never blocks; operations in flight complete (`closed` / `canceled`). Idempotent.
    virtual void close() noexcept = 0;
    // The TCP stream underneath, for the consumer-stall timer's close from the reactor thread.
    [[nodiscard]] virtual std::weak_ptr<pal::TcpStream> tcp() const noexcept = 0;

protected:
    HttpTransport() noexcept = default;
};

namespace http_detail {

[[nodiscard]] inline char const* tcp_error_name(pal::tcp_error e) noexcept {
    switch (e) {
        case pal::tcp_error::none: return "none";
        case pal::tcp_error::canceled: return "canceled";
        case pal::tcp_error::eof: return "eof";
        case pal::tcp_error::connection_refused: return "connection refused";
        case pal::tcp_error::connection_reset: return "connection reset";
        case pal::tcp_error::timed_out: return "timed out";
        case pal::tcp_error::unreachable: return "unreachable";
        case pal::tcp_error::too_many_open_files: return "too many open files";
        case pal::tcp_error::invalid_argument: return "invalid argument";
        case pal::tcp_error::busy: return "busy";
        case pal::tcp_error::closed: return "closed";
        case pal::tcp_error::other: return "other";
    }
    return "other";
}

class PlainTransport final : public HttpTransport {
public:
    explicit PlainTransport(std::shared_ptr<pal::TcpStream> tcp) noexcept : tcp_(std::move(tcp)) {}
    ~PlainTransport() override { close(); }

    task<HttpIo> read_some(std::size_t max_bytes, std::stop_token stop) override {
        return plain_read(tcp_, max_bytes, std::move(stop));
    }
    task<HttpIo> write_all(std::vector<std::byte> bytes, std::stop_token stop) override {
        return plain_write(tcp_, std::move(bytes), std::move(stop));
    }
    void close() noexcept override {
        try {
            if (tcp_) tcp_->close();
        } catch (...) {  // NOLINT(bugprone-empty-catch): a failed post leaves the reactor's shutdown to close it
        }
    }
    [[nodiscard]] std::weak_ptr<pal::TcpStream> tcp() const noexcept override { return tcp_; }

private:
    // Free coroutines over a shared_ptr: the transport object may be dropped while a read is in flight.
    static task<HttpIo> plain_read(std::shared_ptr<pal::TcpStream> tcp, std::size_t max_bytes, std::stop_token stop) {
        TcpResult r = co_await tcp_read_some(*tcp, max_bytes, std::move(stop));
        HttpIo    out;
        if (r.ok()) {
            out.data = std::move(r.data);
        } else if (r.error == pal::tcp_error::eof) {
            out.status = http_io::eof;
        } else if (r.error == pal::tcp_error::canceled) {
            out.status = http_io::canceled;
        } else {
            out.status  = http_io::failed;
            out.code    = pal::is_resource_error(r.error) ? "net.too_many_open_files" : "net.stream_read_failed";
            out.message = std::string("read failed: ") + tcp_error_name(r.error);
        }
        co_return out;
    }
    static task<HttpIo> plain_write(std::shared_ptr<pal::TcpStream> tcp, std::vector<std::byte> bytes,
                                    std::stop_token stop) {
        TcpResult r = co_await tcp_write_all(*tcp, bytes, std::move(stop));
        HttpIo    out;
        if (r.error == pal::tcp_error::canceled) {
            out.status = http_io::canceled;
        } else if (!r.ok()) {
            out.status  = http_io::failed;
            out.code    = "net.connect_failed";  // the blocking client's code for a failed send
            out.message = std::string("send failed: ") + tcp_error_name(r.error);
        }
        co_return out;
    }

    std::shared_ptr<pal::TcpStream> tcp_;
};

}  // namespace http_detail

// The TLS step: wraps a connected TCP stream (verifying the server as `hostname`, the ORIGINAL host name) and
// returns the transport, or a failure (`HttpIo::code` from the TLS client, e.g. net.tls_certificate_rejected).
// rt/http_tls.hpp provides the one implementation (`rt::tls_connector()`); tests may pass a CA override there.
// ae-naming-lint: allow HttpTlsHandshake — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpTlsHandshake {
    std::shared_ptr<HttpTransport> transport;  // non-null iff the handshake succeeded
    http_io                        status = http_io::failed;  // ok / canceled / failed
    std::string                    code;
    std::string                    message;
};
// ae-naming-lint: allow HttpTlsConnector — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
using HttpTlsConnector =
    std::function<task<HttpTlsHandshake>(std::shared_ptr<pal::TcpStream>, std::string hostname, std::stop_token)>;

// ---- requests, options, results -------------------------------------------------------------------------------

// ae-naming-lint: allow http_scheme — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class http_scheme : std::uint8_t { http, https };

// ae-naming-lint: allow HttpRequest — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpRequest {
    http_scheme   scheme = http_scheme::http;
    std::string   host;      // a name (resolved once, ADR-011) or a numeric address, no brackets
    std::uint16_t port = 0;  // 0: the scheme's default (80 / 443)
    std::string   method = "GET";
    std::string   target = "/";  // origin-form: path and query
    HttpHeaders   headers;       // Host, Content-Length, Transfer-Encoding and Connection are the client's
    std::string   body;
};

// ae-naming-lint: allow HttpOptions — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpOptions {
    std::chrono::milliseconds total_timeout{0};               // whole exchange, incl. a streamed body; 0 = none
    std::chrono::milliseconds idle_timeout{90'000};           // per connect/handshake/write/read; 0 = none
    std::chrono::milliseconds consumer_stall_timeout{90'000};  // streams only; 0 = none
    std::size_t               read_chunk_bytes = 16u * 1024u;  // one read's buffer (op-owned)
    std::size_t               max_event_bytes  = SseDecoder::kDefaultMaxEventBytes;
    HttpLimits                limits{};
    ConnectOptions            connect{};  // per-attempt connect deadline; `resolver` is rt/dns.hpp's test seam
};

// Everything a request needs besides the request itself. Copied into the request's coroutine (never referenced).
// ae-naming-lint: allow HttpContext — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpContext {
    pal::Reactor*    reactor  = nullptr;
    OffloadPool*     dns_pool = nullptr;  // the DEDICATED DNS pool (§4.6), e.g. Runtime's
    AddressPolicy    policy;              // required; host code, never model output (I3). Empty: `no_policy`
    HttpTlsConnector tls;                 // required for https (rt/http_tls.hpp); empty: `tls_unavailable`
    HttpOptions      options;
};

// ae-naming-lint: allow HttpResponse — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpResponse {
    http_error     error = http_error::none;
    std::string    code;     // net.* code when error != none
    std::string    message;  // human-readable detail
    std::uint16_t  status = 0;  // set once a head was parsed (also when the body then failed)
    std::string    reason;
    HttpHeaders    headers;
    std::string    body;
    pal::IpAddress address{};  // the verified address the request went to (once connected)

    [[nodiscard]] bool ok() const noexcept { return error == http_error::none; }
};

// ae-naming-lint: allow body_status — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class body_status : std::uint8_t { data, end, error };

// ae-naming-lint: allow HttpBodyChunk — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpBodyChunk {
    body_status status = body_status::end;
    std::string data;  // decoded payload (chunk framing removed); non-empty iff status == data
    http_error  error = http_error::none;
    std::string code;
    std::string message;
};

namespace http_detail {

// A reactor timer that, when it fires, requests stop on its own source (the dns.hpp AttemptDeadline pattern).
class StopTimer final : public pal::ReactorOp {
public:
    void on_complete(pal::op_status status) noexcept override {
        if (status != pal::op_status::completed) return;  // cancelled (the operation ended) or reactor shutdown
        fired_.store(true, std::memory_order_release);
        source_.request_stop();  // runs only ForwardStop callbacks and TCP Cancelers, which post
    }
    [[nodiscard]] bool            fired() const noexcept { return fired_.load(std::memory_order_acquire); }
    [[nodiscard]] std::stop_token token() const noexcept { return source_.get_token(); }

private:
    std::stop_source  source_;
    std::atomic<bool> fired_{false};
};

// Links one stop token to a private source.
struct ForwardStop {
    std::stop_source target;
    void operator()() noexcept { target.request_stop(); }
};

// The consumer-stall timer: when it fires, the connection is closed and the stream marked stalled.
class StallTimer final : public pal::ReactorOp {
public:
    StallTimer(std::weak_ptr<pal::TcpStream> tcp, std::shared_ptr<std::atomic<bool>> stalled) noexcept
        : tcp_(std::move(tcp)), stalled_(std::move(stalled)) {}
    void on_complete(pal::op_status status) noexcept override {
        if (status != pal::op_status::completed) return;
        stalled_->store(true, std::memory_order_release);
        if (auto t = tcp_.lock()) {
            try {
                t->close();  // only posts (pal/reactor_tcp.hpp): allowed on the reactor thread
            } catch (...) {  // NOLINT(bugprone-empty-catch): the stalled flag already fails the stream
            }
        }
    }

private:
    std::weak_ptr<pal::TcpStream>      tcp_;
    std::shared_ptr<std::atomic<bool>> stalled_;
};

inline void cancel_timer(pal::Reactor* reactor, std::shared_ptr<pal::ReactorOp> op) noexcept {
    if (op) reactor_detail::Canceler{reactor, std::move(op)}();
}

// One exchange's deadline state: the overall timer and its request-level stop source.
struct Exchange {
    pal::Reactor*              reactor = nullptr;
    std::shared_ptr<StopTimer> overall;  // null: no overall deadline
    ~Exchange() { cancel_timer(reactor, overall); }
    Exchange()                           = default;
    Exchange(Exchange const&)            = delete;
    Exchange& operator=(Exchange const&) = delete;
};

// One operation's stop source: stopped by the caller, the overall deadline and the operation's own idle timer.
// Lives in the awaiting frame; its destructor cancels the idle timer however the operation ended.
class OpGuard {
public:
    OpGuard(Exchange const& ex, std::stop_token const& caller, std::chrono::milliseconds idle)
        : reactor_(ex.reactor), overall_(ex.overall) {
        from_caller_.emplace(caller, ForwardStop{source_});  // runs inline if the caller already stopped
        if (overall_) from_overall_.emplace(overall_->token(), ForwardStop{source_});
        if (idle.count() > 0 && !source_.stop_requested()) {
            idle_ = std::make_shared<StopTimer>();
            from_idle_.emplace(idle_->token(), ForwardStop{source_});
            reactor_->start_timer(idle_, pal::Reactor::clock::now() + idle);
        }
    }
    ~OpGuard() { cancel_timer(reactor_, idle_); }
    OpGuard(OpGuard const&)            = delete;
    OpGuard& operator=(OpGuard const&) = delete;

    [[nodiscard]] std::stop_token token() const noexcept { return source_.get_token(); }

    // Why a canceled operation stopped, in precedence: the caller, the overall deadline, the idle deadline.
    [[nodiscard]] http_wire::WireError why(std::stop_token const& caller) const {
        if (caller.stop_requested()) return http_wire::fail(http_error::canceled, "net.cancelled", "canceled by the caller");
        if (overall_ && overall_->fired()) {
            return http_wire::fail(http_error::deadline_exceeded, "net.deadline_exceeded",
                                   "the request's overall deadline passed");
        }
        if (idle_ && idle_->fired()) {
            return http_wire::fail(http_error::idle_timeout, "net.idle_timeout",
                                   "no progress within the idle timeout");
        }
        return http_wire::fail(http_error::canceled, "net.cancelled", "canceled (the reactor is shutting down)");
    }

private:
    pal::Reactor*                                   reactor_;
    std::shared_ptr<StopTimer>                      overall_;
    std::shared_ptr<StopTimer>                      idle_;
    std::stop_source                                source_;
    std::optional<std::stop_callback<ForwardStop>> from_caller_;
    std::optional<std::stop_callback<ForwardStop>> from_overall_;
    std::optional<std::stop_callback<ForwardStop>> from_idle_;
};

// The body side of one exchange, shared by the reader handle and every next() in flight.
struct BodyState {
    pal::Reactor*                      reactor = nullptr;
    std::shared_ptr<Exchange>          exchange;
    std::shared_ptr<HttpTransport>     transport;
    http_wire::BodyDecoder             decoder;
    std::string                        leftover;  // wire bytes that arrived with the head
    HttpOptions                        options;
    std::atomic<bool>                  busy{false};
    bool                               finished = false;  // end or a sticky error reached
    HttpBodyChunk                      terminal;          // what every next() returns once finished
    std::shared_ptr<std::atomic<bool>> stalled = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<StallTimer>        stall;

    BodyState()                            = default;
    BodyState(BodyState const&)            = delete;
    BodyState& operator=(BodyState const&) = delete;
    ~BodyState() {
        cancel_timer(reactor, stall);
        if (transport) transport->close();
    }

    void arm_stall() {
        if (finished || options.consumer_stall_timeout.count() <= 0 || !transport) return;
        stall = std::make_shared<StallTimer>(transport->tcp(), stalled);
        reactor->start_timer(stall, pal::Reactor::clock::now() + options.consumer_stall_timeout);
    }
    void disarm_stall() noexcept {
        cancel_timer(reactor, std::move(stall));
        stall.reset();
    }
    [[nodiscard]] bool is_stalled() const noexcept { return stalled->load(std::memory_order_acquire); }

    // Ends the stream (sticky): closes the connection, stops the timers.
    HttpBodyChunk finish(HttpBodyChunk c) {
        finished = true;
        terminal = c;
        terminal.data.clear();
        disarm_stall();
        if (transport) transport->close();
        if (exchange) cancel_timer(reactor, exchange->overall);
        return c;
    }
    HttpBodyChunk fail(http_wire::WireError const& e) {
        HttpBodyChunk c;
        c.status  = body_status::error;
        c.error   = e.error;
        c.code    = e.code;
        c.message = e.message;
        return finish(std::move(c));
    }
};

// Clears `busy` when a next() ends -- and, if its frame is destroyed mid-read (the TCP read was abandoned and
// cancelled, so the connection is closed), marks the stream finished `canceled` so later calls do not read a
// connection in an unknown state.
struct BusyGuard {
    BodyState* state;
    bool       completed = false;
    explicit BusyGuard(BodyState* s) noexcept : state(s) {}
    BusyGuard(BusyGuard const&)            = delete;
    BusyGuard& operator=(BusyGuard const&) = delete;
    ~BusyGuard() {
        if (!completed && !state->finished) {
            try {
                (void)state->fail(http_wire::fail(http_error::canceled, "net.cancelled",
                                                  "the reading coroutine was destroyed mid-read"));
            } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory while recording; the stream is closed
            }
        }
        state->busy.store(false, std::memory_order_release);
    }
};

[[nodiscard]] inline http_wire::WireError stalled_error() {
    return http_wire::fail(http_error::consumer_stalled, "net.consumer_stalled",
                           "the consumer did not ask for the next chunk within the consumer-stall deadline");
}

[[nodiscard]] inline std::string_view as_chars(std::vector<std::byte> const& v) noexcept {
    return {reinterpret_cast<char const*>(v.data()), v.size()};
}

// One body step: the next decoded payload, the end, or an error. `arm` re-arms the consumer-stall timer on data
// (the SSE layer passes false and arms per event instead).
inline task<HttpBodyChunk> body_next(std::shared_ptr<BodyState> st, std::stop_token stop, bool arm) {
    if (st->busy.exchange(true, std::memory_order_acq_rel)) {
        HttpBodyChunk b;
        b.status  = body_status::error;
        b.error   = http_error::busy;
        b.code    = "net.busy";
        b.message = "another next() is outstanding on this reader";
        co_return b;
    }
    BusyGuard guard(st.get());
    st->disarm_stall();
    if (st->finished) {
        guard.completed = true;
        co_return st->terminal;
    }
    if (st->is_stalled()) {
        guard.completed = true;
        co_return st->fail(stalled_error());
    }
    std::string out;
    if (!st->leftover.empty()) {
        std::string const wire = std::move(st->leftover);
        st->leftover.clear();
        if (auto e = st->decoder.feed(wire, &out); !e.ok()) {
            guard.completed = true;
            co_return st->fail(e);
        }
    }
    while (out.empty() && !st->decoder.complete()) {
        if (stop.stop_requested()) {
            guard.completed = true;
            co_return st->fail(http_wire::fail(http_error::canceled, "net.cancelled", "canceled by the caller"));
        }
        OpGuard op(*st->exchange, stop, st->options.idle_timeout);
        HttpIo  r = co_await st->transport->read_some(st->options.read_chunk_bytes, op.token());
        if (r.status == http_io::canceled) {
            guard.completed = true;
            co_return st->fail(st->is_stalled() ? stalled_error() : op.why(stop));
        }
        if (r.status == http_io::failed) {
            guard.completed = true;
            co_return st->fail(st->is_stalled() ? stalled_error()
                                                : http_wire::fail(http_error::transport, r.code, r.message));
        }
        if (r.status == http_io::eof) {
            if (st->is_stalled()) {
                guard.completed = true;
                co_return st->fail(stalled_error());
            }
            http_wire::WireError e = st->decoder.on_eof();
            if (e.ok() && r.unclean_eof && st->decoder.framing().kind == http_wire::body_framing::until_close) {
                e = http_wire::fail(http_error::truncated, "net.stream_truncated",
                                    "the TLS connection ended without close_notify before a close-delimited body "
                                    "was known to be complete");
            }
            guard.completed = true;
            if (!e.ok()) co_return st->fail(e);
            break;
        }
        if (auto e = st->decoder.feed(as_chars(r.data), &out); !e.ok()) {
            guard.completed = true;
            co_return st->fail(e);
        }
    }
    guard.completed = true;
    if (!out.empty()) {
        if (arm) st->arm_stall();
        HttpBodyChunk b;
        b.status = body_status::data;
        b.data   = std::move(out);
        co_return b;
    }
    HttpBodyChunk b;
    b.status = body_status::end;
    co_return st->finish(std::move(b));
}

}  // namespace http_detail

// The body of a streamed response. Movable handle; see LIFETIME in the file comment.
// ae-naming-lint: allow HttpBodyReader — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class HttpBodyReader {
public:
    HttpBodyReader() = default;
    explicit HttpBodyReader(std::shared_ptr<http_detail::BodyState> state) noexcept : state_(std::move(state)) {}
    HttpBodyReader(HttpBodyReader&&) noexcept            = default;
    HttpBodyReader& operator=(HttpBodyReader&&) noexcept = default;
    HttpBodyReader(HttpBodyReader const&)                = delete;
    HttpBodyReader& operator=(HttpBodyReader const&)     = delete;

    // The next decoded payload, `end`, or an error (sticky). One outstanding at a time (`busy` otherwise).
    [[nodiscard]] task<HttpBodyChunk> next(std::stop_token stop = {}) {
        return http_detail::body_next(state_, std::move(stop), /*arm=*/true);
    }
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    // Closes the connection now (posted). Later next() calls return `end` or the error already reached. Not while a
    // next() is outstanding (stop it through its token instead).
    void close() {
        if (state_ && !state_->finished) {
            HttpBodyChunk b;
            b.status = body_status::end;
            (void)state_->finish(std::move(b));
        }
    }

    [[nodiscard]] std::shared_ptr<http_detail::BodyState> const& state() const noexcept { return state_; }

private:
    std::shared_ptr<http_detail::BodyState> state_;
};

// ae-naming-lint: allow HttpStreamResponse — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpStreamResponse {
    http_error     error = http_error::none;  // the head phase's outcome; body errors come from `body.next()`
    std::string    code;
    std::string    message;
    std::uint16_t  status = 0;
    std::string    reason;
    HttpHeaders    headers;
    pal::IpAddress address{};
    HttpBodyReader body;  // valid iff ok()

    [[nodiscard]] bool ok() const noexcept { return error == http_error::none; }
};

namespace http_detail {

inline void set_error(HttpStreamResponse& r, http_wire::WireError const& e) {
    r.error   = e.error;
    r.code    = e.code;
    r.message = e.message;
}

inline http_wire::WireError from_connect(ConnectedStream const& c) {
    switch (c.error) {
        case connect_error::none: return {};
        case connect_error::canceled: return http_wire::fail(http_error::canceled, "net.cancelled", "canceled");
        case connect_error::no_policy:
            return http_wire::fail(http_error::no_policy, "net.no_address_policy",
                                   "no address policy was given: nothing is connected without one");
        case connect_error::resolve_failed:
            return http_wire::fail(http_error::resolve_failed, "net.host_unresolvable", "could not resolve the host");
        case connect_error::every_address_rejected:
            return http_wire::fail(http_error::address_rejected, "net.address_blocked",
                                   "every resolved address was rejected by the address policy");
        case connect_error::connect_failed:
            return http_wire::fail(http_error::connect_failed, "net.connect_failed", "connect refused or failed");
    }
    return http_wire::fail(http_error::connect_failed, "net.connect_failed", "connect failed");
}

// Connect, (TLS,) send, read the head. `arm` starts the consumer-stall timer once the head is returned.
inline task<HttpStreamResponse> open_stream(HttpContext ctx, HttpRequest req, std::stop_token stop, bool arm) {
    HttpStreamResponse out;
    bool const         https = req.scheme == http_scheme::https;
    std::uint16_t const port = req.port != 0 ? req.port : (https ? std::uint16_t{443} : std::uint16_t{80});

    // 1. Build (and validate) the request before anything touches the network (ADR-011 §3: gates run first).
    std::string wire;
    http_wire::RequestLine const line{req.method, req.target, req.host, port,
                                      https ? std::uint16_t{443} : std::uint16_t{80}, &req.headers, req.body};
    if (auto e = http_wire::build_request(line, &wire); !e.ok()) {
        set_error(out, e);
        co_return out;
    }
    if (ctx.reactor == nullptr || ctx.dns_pool == nullptr) {
        set_error(out, http_wire::fail(http_error::invalid_request, "net.invalid_request",
                                       "the HttpContext has no reactor or DNS pool"));
        co_return out;
    }
    if (!ctx.policy) {
        set_error(out, http_wire::fail(http_error::no_policy, "net.no_address_policy",
                                       "no address policy was given: nothing is connected without one"));
        co_return out;
    }
    if (https && !ctx.tls) {
        set_error(out, http_wire::fail(http_error::tls_unavailable, "net.scheme_unsupported",
                                       "https requested but no TLS connector was given (rt/http_tls.hpp, "
                                       "AGENTENGINE_WITH_HTTPS)"));
        co_return out;
    }
    if (stop.stop_requested()) {
        set_error(out, http_wire::fail(http_error::canceled, "net.cancelled", "canceled by the caller"));
        co_return out;
    }

    // 2. The overall deadline.
    auto ex     = std::make_shared<Exchange>();
    ex->reactor = ctx.reactor;
    if (ctx.options.total_timeout.count() > 0) {
        ex->overall = std::make_shared<StopTimer>();
        ctx.reactor->start_timer(ex->overall, pal::Reactor::clock::now() + ctx.options.total_timeout);
    }

    // 3. Resolve once, filter through the policy, connect to a verified address (ADR-011, rt/dns.hpp).
    std::shared_ptr<pal::TcpStream> tcp;
    {
        OpGuard         op(*ex, stop, std::chrono::milliseconds{0});  // per-attempt bound: ConnectOptions
        ConnectedStream c = co_await connect_resolved(*ctx.reactor, *ctx.dns_pool, req.host, port, ctx.policy,
                                                      op.token(), ctx.options.connect);
        if (!c.ok()) {
            set_error(out, c.error == connect_error::canceled ? op.why(stop) : from_connect(c));
            co_return out;
        }
        out.address = c.address;
        tcp         = std::move(c.stream);
    }

    // 4. The transport: plain, or TLS verified against the ORIGINAL host name (ADR-013).
    std::shared_ptr<HttpTransport> transport;
    if (https) {
        OpGuard          op(*ex, stop, ctx.options.idle_timeout);
        HttpTlsHandshake h = co_await ctx.tls(tcp, req.host, op.token());
        if (h.status != http_io::ok || !h.transport) {
            tcp->close();
            set_error(out, h.status == http_io::canceled
                               ? op.why(stop)
                               : http_wire::fail(http_error::tls_failed, h.code, h.message));
            co_return out;
        }
        transport = std::move(h.transport);
    } else {
        transport = std::make_shared<PlainTransport>(tcp);
    }

    // 5. Send the request.
    {
        std::vector<std::byte> bytes(wire.size());
        std::memcpy(bytes.data(), wire.data(), wire.size());
        OpGuard op(*ex, stop, ctx.options.idle_timeout);
        HttpIo  w = co_await transport->write_all(std::move(bytes), op.token());
        if (w.status != http_io::ok) {
            transport->close();
            set_error(out, w.status == http_io::canceled ? op.why(stop)
                                                          : http_wire::fail(http_error::transport, w.code, w.message));
            co_return out;
        }
    }

    // 6. Read the head (skipping interim 1xx responses other than 101).
    http_wire::HeadParser parser(ctx.options.limits);
    for (;;) {
        while (!parser.done()) {
            OpGuard op(*ex, stop, ctx.options.idle_timeout);
            HttpIo  r = co_await transport->read_some(ctx.options.read_chunk_bytes, op.token());
            http_wire::WireError e;
            if (r.status == http_io::canceled) {
                e = op.why(stop);
            } else if (r.status == http_io::failed) {
                e = http_wire::fail(http_error::transport, r.code, r.message);
            } else if (r.status == http_io::eof) {
                e = http_wire::fail(http_error::truncated, "net.protocol_error", "response head was truncated");
            } else {
                e = parser.feed(as_chars(r.data));
            }
            if (!e.ok()) {
                transport->close();
                set_error(out, e);
                co_return out;
            }
        }
        std::uint16_t const status = parser.head().status;
        if (status >= 100 && status < 200 && status != 101) {
            std::string rest = std::move(parser.leftover());
            parser           = http_wire::HeadParser(ctx.options.limits);
            if (auto e = parser.feed(rest); !e.ok()) {
                transport->close();
                set_error(out, e);
                co_return out;
            }
            continue;
        }
        if (status == 101) {
            transport->close();
            set_error(out, http_wire::fail(http_error::malformed_response, "net.protocol_error",
                                           "101 Switching Protocols to a request that asked for no upgrade"));
            co_return out;
        }
        break;
    }

    http_wire::ResponseHead& head = parser.head();
    http_wire::Framing       framing;
    http_wire::WireError const fe = http_wire::determine_framing(req.method, head, ctx.options.limits, &framing);
    out.status                    = head.status;
    out.reason                    = std::move(head.reason);
    out.headers                   = std::move(head.headers);
    if (!fe.ok()) {
        transport->close();
        set_error(out, fe);
        co_return out;
    }

    auto st       = std::make_shared<BodyState>();
    st->reactor   = ctx.reactor;
    st->exchange  = std::move(ex);
    st->transport = std::move(transport);
    st->decoder   = http_wire::BodyDecoder(framing, ctx.options.limits);
    st->leftover  = std::move(parser.leftover());
    st->options   = ctx.options;
    if (arm) st->arm_stall();
    out.body = HttpBodyReader(std::move(st));
    co_return out;
}

inline task<HttpResponse> request_buffered(HttpContext ctx, HttpRequest req, std::stop_token stop) {
    HttpStreamResponse s = co_await open_stream(std::move(ctx), std::move(req), stop, /*arm=*/false);
    HttpResponse       out;
    out.error   = s.error;
    out.code    = std::move(s.code);
    out.message = std::move(s.message);
    out.status  = s.status;
    out.reason  = std::move(s.reason);
    out.headers = std::move(s.headers);
    out.address = s.address;
    if (!s.ok()) co_return out;
    std::shared_ptr<BodyState> st = s.body.state();
    s.body                        = HttpBodyReader{};
    for (;;) {
        HttpBodyChunk c = co_await body_next(st, stop, /*arm=*/false);
        if (c.status == body_status::data) {
            out.body += c.data;
            continue;
        }
        if (c.status == body_status::error) {
            out.error   = c.error;
            out.code    = std::move(c.code);
            out.message = std::move(c.message);
        }
        co_return out;
    }
}

}  // namespace http_detail

// One HTTP/1.1 exchange, body buffered (capped). See the file comment.
[[nodiscard]] inline task<HttpResponse> http_request(HttpContext ctx, HttpRequest request, std::stop_token stop = {}) {
    return http_detail::request_buffered(std::move(ctx), std::move(request), std::move(stop));
}

// One HTTP/1.1 exchange, body streamed: the head, then `body.next()`. See the file comment.
[[nodiscard]] inline task<HttpStreamResponse> http_stream(HttpContext ctx, HttpRequest request,
                                                          std::stop_token stop = {}) {
    return http_detail::open_stream(std::move(ctx), std::move(request), std::move(stop), /*arm=*/true);
}

// ---- server-sent events over a streamed body ----------------------------------------------------------------

// ae-naming-lint: allow sse_status — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class sse_status : std::uint8_t { event, end, error };

// ae-naming-lint: allow SseNext — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct SseNext {
    sse_status  status = sse_status::end;
    SseEvent    event;  // status == event
    http_error  error = http_error::none;
    std::string code;
    std::string message;
    bool        incomplete_dropped = false;  // `end`: the stream stopped inside an event, which was discarded
};

namespace http_detail {

struct SseState {
    std::shared_ptr<BodyState> body;
    SseDecoder                 decoder;
    std::deque<SseEvent>       queue;
    std::atomic<bool>          busy{false};
    explicit SseState(std::shared_ptr<BodyState> b, std::size_t max_event) : body(std::move(b)), decoder(max_event) {}
};

struct SseBusyGuard {
    std::atomic<bool>* busy;
    ~SseBusyGuard() { busy->store(false, std::memory_order_release); }
};

[[nodiscard]] inline SseNext sse_error(HttpBodyChunk const& c) {
    SseNext n;
    n.status  = sse_status::error;
    n.error   = c.error;
    n.code    = c.code;
    n.message = c.message;
    return n;
}

inline task<SseNext> sse_next(std::shared_ptr<SseState> st, std::stop_token stop) {
    if (st->busy.exchange(true, std::memory_order_acq_rel)) {
        SseNext n;
        n.status  = sse_status::error;
        n.error   = http_error::busy;
        n.code    = "net.busy";
        n.message = "another next() is outstanding on this event stream";
        co_return n;
    }
    SseBusyGuard guard{&st->busy};
    BodyState&   body = *st->body;
    body.disarm_stall();
    if (!body.finished && body.is_stalled()) co_return sse_error(body.fail(stalled_error()));
    while (st->queue.empty()) {
        HttpBodyChunk c = co_await body_next(st->body, stop, /*arm=*/false);
        if (c.status == body_status::error) co_return sse_error(c);
        if (c.status == body_status::end) {
            SseNext n;
            n.status             = sse_status::end;
            n.incomplete_dropped = st->decoder.incomplete();
            co_return n;
        }
        std::vector<SseEvent> events;
        if (auto e = st->decoder.feed(c.data, &events); !e.ok()) {
            co_return sse_error(body.fail(e));
        }
        for (auto& ev : events) st->queue.push_back(std::move(ev));
    }
    SseNext n;
    n.status = sse_status::event;
    n.event  = std::move(st->queue.front());
    st->queue.pop_front();
    body.arm_stall();
    co_return n;
}

}  // namespace http_detail

// Server-sent events decoded from a streamed body (rt/http_wire.hpp's SseDecoder). Movable handle.
// ae-naming-lint: allow SseStream — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class SseStream {
public:
    SseStream() = default;
    explicit SseStream(HttpBodyReader body, std::size_t max_event_bytes = SseDecoder::kDefaultMaxEventBytes)
        : state_(body.valid() ? std::make_shared<http_detail::SseState>(body.state(), max_event_bytes) : nullptr) {}

    // The next event, `end`, or an error (sticky). One outstanding at a time (`busy` otherwise).
    [[nodiscard]] task<SseNext> next(std::stop_token stop = {}) { return http_detail::sse_next(state_, std::move(stop)); }
    [[nodiscard]] bool          valid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] std::optional<std::uint64_t> retry_ms() const noexcept { return state_->decoder.retry_ms(); }
    [[nodiscard]] std::string const&           last_event_id() const noexcept { return state_->decoder.last_event_id(); }

private:
    std::shared_ptr<http_detail::SseState> state_;
};

// The events of a streamed response's body. `max_event_bytes`: one event's cap (`event_too_large` beyond it).
[[nodiscard]] inline SseStream sse_events(HttpBodyReader body,
                                          std::size_t max_event_bytes = SseDecoder::kDefaultMaxEventBytes) {
    return SseStream(std::move(body), max_event_bytes);
}

}  // namespace agentengine::rt
