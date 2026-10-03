#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.1/§6.3 (with §4.4's cancellation
// rules) -- the reactor's TCP stream: connect, read, write and a zero-byte readiness wait, as operations on
// `pal::Reactor` (pal/reactor.hpp), the same shape as `start_timer`. It is the foundation the HTTP/SSE client
// moves onto (§6.3); TLS and DNS are not here.
//
// Std-only like reactor.hpp: no Asio type appears. The one backend is the Asio reactor
// (src/backends/reactor_asio/reactor_tcp_asio.cpp); `make_tcp_stream` requires a reactor made by
// `make_default_reactor()` and throws `std::invalid_argument` for any other.
//
// RULES (beyond reactor.hpp's threading contract, which every operation here follows):
//   - ADDRESSES ARE ALREADY RESOLVED. `start_connect` takes a numeric IPv4/IPv6 address (ADR-011: resolve
//     once, verify, connect to the verified address). Nothing in this seam resolves a name.
//   - BUFFERS BELONG TO THE OPERATION RECORD (§6.3, round-3 gap 6). A read fills the op's own buffer and a
//     write sends from it; no start function takes a pointer or span, so the kernel can never be handed
//     memory in a coroutine frame. The buffer is refcounted with the record (and shareable between the ops
//     of one `write_all`), so it stays alive until the backend's completion, whatever happens to the frame.
//   - ONE SERIAL CONTEXT PER STREAM (§4.4, round-2 M2): every initiation, completion and cancellation of a
//     stream's operations runs on the reactor thread. At most one read-side operation (read_some or
//     wait_readable) and one write_some may be outstanding at a time, plus a connect alone; a second one
//     completes at once with `tcp_error::busy`.
//   - TWO-PHASE CANCEL, REAL OUTCOME (§4.4, round-1 M1): `Reactor::cancel(op)` works on these ops. The op
//     completes from the backend's own outcome: a read that had already transferred bytes reports them as
//     `completed` even if a cancel raced it. An operation that was in flight and ended `canceled` leaves the
//     stream unusable -- the backend closes it (§4.4: "a socket ... whose operation was canceled is unusable
//     afterwards and is closed"); later operations complete with `tcp_error::closed`. A cancel that lands
//     before the start (sticky) never touched the socket and closes nothing.
//   - DESCRIPTOR LIMITS (§6.3, round-2 G-g): EMFILE / ENFILE / WSAEMFILE map to
//     `tcp_error::too_many_open_files`, a `resource` error (`is_resource_error`). Nothing raises the limit.
//   - LIFETIME: dropping a handle posts the socket's close to the reactor thread; operations still pending on
//     it complete (`closed`) and the socket is freed there. Operations must not be STARTED once the reactor's
//     destruction has begun, but a handle may be DROPPED after its reactor is gone -- a waiter whose read the
//     shutdown completed typically does exactly that (§4.5 rule 4) -- and then only frees memory: the
//     reactor's io_context shutdown has already destroyed every socket still alive.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor.hpp"

namespace agentengine::pal {

// ae-naming-lint: allow tcp_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class tcp_error : std::uint8_t {
    none,                 // success
    canceled,             // stopped by a cancel request or reactor shutdown before it finished
    eof,                  // the peer closed its sending side (read only); nothing was read
    connection_refused,   // nobody listening at the address (connect)
    connection_reset,     // reset / aborted by the peer, or a write to a closed peer (EPIPE)
    timed_out,            // the OS gave up (connect / keep-alive); engine deadlines arrive as `canceled`
    unreachable,          // network or host unreachable
    too_many_open_files,  // EMFILE / ENFILE / WSAEMFILE: a `resource` error (§6.3, round-2 G-g)
    invalid_argument,     // e.g. connecting to an address of no supported family, a read of zero bytes
    busy,                 // another operation of the same direction is still outstanding on this stream
    closed,               // the stream was closed (by close(), or after a canceled operation, §4.4)
    other,                // anything else; `TcpOpResult::native` carries the OS error
};

// True for errors the engine classifies `resource` (§6.3 / §9 D6): descriptor exhaustion.
[[nodiscard]] constexpr bool is_resource_error(tcp_error e) noexcept { return e == tcp_error::too_many_open_files; }

// Maps an OS / backend error code to `tcp_error`. Exposed for the classification tests; defined by the backend.
[[nodiscard]] tcp_error classify_tcp_error(std::error_code ec) noexcept;

// A numeric IP address. No name resolution, ever (ADR-011).
// ae-naming-lint: allow IpAddress — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct IpAddress {
    enum class family_kind : std::uint8_t { v4, v6 };
    family_kind                  family = family_kind::v4;
    std::array<std::uint8_t, 16> bytes{};  // network order; v4 uses the first 4
    std::uint32_t                scope_id = 0;  // v6 link-local only

    [[nodiscard]] static constexpr IpAddress v4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                                std::uint8_t d) noexcept {
        IpAddress r;
        r.bytes[0] = a;
        r.bytes[1] = b;
        r.bytes[2] = c;
        r.bytes[3] = d;
        return r;
    }
    [[nodiscard]] static constexpr IpAddress v6(std::array<std::uint8_t, 16> b, std::uint32_t scope = 0) noexcept {
        IpAddress r;
        r.family   = family_kind::v6;
        r.bytes    = b;
        r.scope_id = scope;
        return r;
    }
    [[nodiscard]] static constexpr IpAddress loopback_v4() noexcept { return v4(127, 0, 0, 1); }
    [[nodiscard]] static constexpr IpAddress loopback_v6() noexcept {
        std::array<std::uint8_t, 16> b{};
        b[15] = 1;
        return v6(b);
    }
    friend constexpr bool operator==(IpAddress const&, IpAddress const&) noexcept = default;
};

// Parses a NUMERIC address ("127.0.0.1", "::1", "fe80::1%3"); a host name yields nullopt. Defined by the backend.
[[nodiscard]] std::optional<IpAddress> parse_ip_address(std::string_view text) noexcept;

// What one operation did. Written by the backend on the reactor thread before `on_complete`; read by the
// waiter after it is resumed (the wake-up's post orders the two).
// ae-naming-lint: allow TcpOpResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct TcpOpResult {
    tcp_error       error       = tcp_error::canceled;
    std::size_t     transferred = 0;  // bytes read or written (0 for connect / wait_readable)
    std::error_code native{};         // the backend's own code, when there was one
};

// The byte storage an operation reads into or writes from: refcounted, owned by the op record(s).
using TcpBuffer = std::shared_ptr<std::vector<std::byte>>;  // ae-naming-lint: allow TcpBuffer — ADR-237 §6.3 round-3 gap 6

// One TCP operation's record. The waiter derives from it (rt/tcp.hpp), as from ReactorOp for a timer.
// ae-naming-lint: allow TcpOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class TcpOp : public ReactorOp {
public:
    // The op's buffer: a read fills `(*buffer)[offset .. size())`, a write sends that range. Set before the
    // start; never touched by the waiter again until the op has completed.
    void set_buffer(TcpBuffer buffer, std::size_t offset = 0) noexcept {
        buffer_ = std::move(buffer);
        offset_ = offset;
    }
    [[nodiscard]] TcpBuffer const& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t      buffer_offset() const noexcept { return offset_; }

    // Valid once `on_complete` has been called.
    [[nodiscard]] TcpOpResult const& result() const noexcept { return result_; }

    // Backend only, reactor thread, immediately before `on_complete` (public for the same reason as
    // `ReactorOp::backend_state`: the backend is another translation unit).
    void set_result(TcpOpResult r) noexcept { result_ = r; }

protected:
    TcpOp() noexcept = default;

private:
    TcpBuffer   buffer_{};
    std::size_t offset_ = 0;
    TcpOpResult result_{};
};

// A TCP stream on one reactor. Every method is thread-safe and never blocks: it posts to the reactor
// thread. Each started op's `on_complete` is called exactly once, on the reactor thread, with `completed`
// when `result().error == tcp_error::none` (or a backend outcome other than a cancel) and `canceled` when
// the op ended by a cancel request or by reactor shutdown.
// ae-naming-lint: allow TcpStream — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class TcpStream {
public:
    virtual ~TcpStream() = default;
    TcpStream(TcpStream const&)            = delete;
    TcpStream& operator=(TcpStream const&) = delete;

    [[nodiscard]] virtual Reactor& reactor() const noexcept = 0;

    // Connects to an already-resolved address. Allowed once, on a stream that is not connected.
    virtual void start_connect(std::shared_ptr<TcpOp> op, IpAddress address, std::uint16_t port) = 0;
    // Reads at least one byte into the op's buffer range (which must be non-empty), or reports eof.
    virtual void start_read_some(std::shared_ptr<TcpOp> op) = 0;
    // Waits until a read would not block (data, eof or an error pending), consuming nothing and committing
    // no buffer -- the parked-read primitive (§6.3, round-1 G6: IOCP zero-byte WSARecv, epoll readiness).
    virtual void start_wait_readable(std::shared_ptr<TcpOp> op) = 0;
    // Writes at least one byte of the op's buffer range (which must be non-empty).
    virtual void start_write_some(std::shared_ptr<TcpOp> op) = 0;
    // Closes the socket: outstanding operations complete (`canceled` if they had been asked to stop, else
    // `tcp_error::closed`), later ones complete with `tcp_error::closed`. Idempotent.
    virtual void close() = 0;

protected:
    TcpStream() noexcept = default;
};

// A new, unconnected stream on `reactor`, which must be the default (Asio) reactor -- `std::invalid_argument`
// otherwise. Defined in src/backends/reactor_asio/reactor_tcp_asio.cpp.
[[nodiscard]] std::shared_ptr<TcpStream> make_tcp_stream(Reactor& reactor);

}  // namespace agentengine::pal
