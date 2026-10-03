// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.2/§6.3 (with §4.4) -- the Asio
// backend of `pal::TcpStream` (pal/reactor_tcp.hpp): asio::ip::tcp::socket on the AsioReactor's one
// io_context. Follows asio_reactor.hpp's recipe for every operation:
//   1. the start is posted to the reactor thread (the handle's methods only post);
//   2. there, a sticky cancel (or a reactor shutting down) completes the op `canceled` without touching the
//      socket (round-2 M2);
//   3. a `SocketOpState` -- a per-operation Asio cancellation signal plus a reference to the stream -- goes in
//      `op->backend_state`, the op is registered pending, and the Asio operation is started with its handler
//      bound to that signal (per-operation cancellation: IOCP CancelIoEx on the one OVERLAPPED, epoll removal
//      of the one op), so cancelling a read never disturbs a concurrent write on the same socket;
//   4. the handler unregisters the op, fills `TcpOp::set_result` from Asio's OWN outcome -- bytes it
//      transferred stay reported even if a cancel raced them (round-1 M1) -- and calls `on_complete`.
// The socket itself is created, used and destroyed only on the reactor thread (§4.4 "one serial context per
// I/O object"): the stream's state is created lazily there and the handle's destructor posts its release.
// A handle can outlive the reactor (a waiter dropping its stream after the shutdown completed its read): an
// Asio service (LifetimeService) destroys every live socket while the io_context is torn down and marks the
// registry dead, so a late drop neither posts to nor touches a destroyed io_context.
// The readiness wait is `async_wait(wait_read)`, which Asio implements on Windows as an overlapped zero-byte
// WSARecv on the IOCP and on Linux as epoll readiness -- no buffer is committed while parked (§6.3, G6).

#include "agentengine/pal/reactor_tcp.hpp"

#include <cerrno>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

#include "asio_reactor.hpp"

namespace agentengine::pal {

namespace asio_backend {
namespace {

using tcp = asio::ip::tcp;

struct StreamCore;

// The streams of one io_context, and whether it is still alive. Shared by the LifetimeService and every
// stream (so it outlives both). A stream handle can legitimately outlive a reactor that is being destroyed:
// a waiter whose pending read the shutdown completed drops its stream after the reactor's destructor has
// run (§4.5 rule 4). Without this, that drop would post to -- or destroy a socket of -- a dead io_context.
struct Registry {
    std::mutex                      m;
    bool                            dead = false;  // the io_context is being destroyed
    std::unordered_set<StreamCore*> live;
};

// One stream's state. Its fields are touched on the reactor thread only (the socket is created there lazily
// and used only there), except: the destructor, which runs wherever the last reference drops -- by then no
// other thread can reach it -- and LifetimeService::shutdown, which runs after the reactor thread is joined.
// Both take the registry lock.
struct StreamCore {
    StreamCore(AsioReactor& r, std::shared_ptr<Registry> reg) : reactor(&r), registry(std::move(reg)) {
        std::lock_guard lock(registry->m);
        registry->live.insert(this);
    }
    ~StreamCore() {
        std::lock_guard lock(registry->m);
        registry->live.erase(this);
        socket.reset();  // no-op if the io_context's shutdown already destroyed it
    }
    StreamCore(StreamCore const&)            = delete;
    StreamCore& operator=(StreamCore const&) = delete;

    tcp::socket& sock() {
        if (!socket) socket.emplace(reactor->io());
        return *socket;
    }
    void close_socket() noexcept {
        closed = true;
        if (socket && socket->is_open()) {
            std::error_code ignored;
            (void)socket->close(ignored);  // aborts every operation still outstanding on it
        }
    }

    AsioReactor*               reactor;
    std::shared_ptr<Registry>  registry;
    std::optional<tcp::socket> socket;  // reactor thread only (see above)
    bool                       connect_started = false;
    bool                       reading         = false;  // a read_some or wait_readable is outstanding
    bool                       writing         = false;
    bool                       closed          = false;
};

// Asio calls shutdown() while the io_context is destroyed -- after the AsioReactor joined its thread, and
// before the io_context's queued handlers are destroyed. Every stream's socket still alive is destroyed
// here, while Asio's services can still take it, and handles dropped later only release memory.
class LifetimeService final : public asio::execution_context::service {
public:
    using key_type = LifetimeService;
    static inline asio::execution_context::id id;  // NOLINT: Asio's service-identity idiom

    explicit LifetimeService(asio::execution_context& ctx)
        : asio::execution_context::service(ctx), registry(std::make_shared<Registry>()) {}

    void shutdown() override {
        std::lock_guard lock(registry->m);
        registry->dead = true;
        for (StreamCore* core : registry->live) {
            core->closed = true;
            core->socket.reset();
        }
    }

    std::shared_ptr<Registry> const registry;
};

struct SocketOpState final : BackendOpState {
    explicit SocketOpState(std::shared_ptr<StreamCore> c) noexcept : core(std::move(c)) {}
    void cancel() noexcept override {
        try {
            signal.emit(asio::cancellation_type::terminal);
        } catch (...) {  // NOLINT(bugprone-empty-catch): the op then completes on its own, still exactly once
        }
    }
    std::shared_ptr<StreamCore> core;
    asio::cancellation_signal   signal;
};

// Reactor thread. Completes an op that never reached the socket.
void complete_now(TcpOp& op, op_status status, tcp_error error) noexcept {
    op.set_result(TcpOpResult{error, 0, {}});
    op.on_complete(status);
}

// Reactor thread, step 2 of the recipe. True if the op was completed here and must not start.
bool refused_at_start(AsioReactor& reactor, StreamCore& core, TcpOp& op) noexcept {
    if (op.cancel_requested() || reactor.shutting_down()) {
        complete_now(op, op_status::canceled, tcp_error::canceled);
        return true;
    }
    if (core.closed) {
        complete_now(op, op_status::completed, tcp_error::closed);
        return true;
    }
    return false;
}

// Reactor thread, step 4. The outcome is Asio's: success (with its bytes) stays success whatever was asked.
void complete_from_backend(StreamCore& core, std::shared_ptr<TcpOp> const& op, std::error_code ec,
                           std::size_t transferred) noexcept {
    core.reactor->remove_pending(op);
    op->backend_state.reset();
    TcpOpResult r{tcp_error::none, transferred, ec};
    op_status   status = op_status::completed;
    if (ec) {
        if (ec == asio::error::operation_aborted &&
            (op->cancel_requested() || core.reactor->shutting_down())) {
            r.error = tcp_error::canceled;
            status  = op_status::canceled;
            core.close_socket();  // §4.4: a stream whose in-flight op was canceled is unusable; close it
        } else if (ec == asio::error::operation_aborted) {
            r.error = tcp_error::closed;  // aborted by close(), not by a cancel of this op
        } else {
            r.error = classify_tcp_error(ec);
        }
    }
    op->set_result(r);
    op->on_complete(status);
}

// Steps 3-4 shared by every operation: register, then start with a handler bound to the op's own
// cancellation slot (the OUTERMOST handler handed to Asio, or Asio would not see the slot). `done` runs
// first in the handler, for the stream's own bookkeeping. Handler signatures: (ec) or (ec, bytes).
template <class Initiate, class Done>
void start_registered(std::shared_ptr<StreamCore> const& core, std::shared_ptr<TcpOp> op, Initiate&& initiate,
                      Done done) {
    auto state        = std::make_shared<SocketOpState>(core);
    op->backend_state = state;
    core->reactor->add_pending(op);
    // The handler keeps `state` (and so the signal its slot belongs to) and `op` (and so its buffer) alive
    // until it has run.
    auto handler = [state, op, done](std::error_code ec, auto... n) {
        std::size_t const transferred = (std::size_t{0} + ... + static_cast<std::size_t>(n));
        done(*state->core, ec);
        complete_from_backend(*state->core, op, ec, transferred);
    };
    std::forward<Initiate>(initiate)(asio::bind_cancellation_slot(state->signal.slot(), std::move(handler)));
}

tcp::endpoint to_endpoint(IpAddress const& a, std::uint16_t port) {
    if (a.family == IpAddress::family_kind::v4) {
        asio::ip::address_v4::bytes_type b{};
        for (std::size_t i = 0; i < b.size(); ++i) b[i] = a.bytes[i];
        return {asio::ip::address_v4(b), port};
    }
    asio::ip::address_v6::bytes_type b{};
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = a.bytes[i];
    return {asio::ip::address_v6(b, a.scope_id), port};
}

// The handle. Every method posts; nothing here touches the socket.
class AsioTcpStream final : public TcpStream {
public:
    AsioTcpStream(AsioReactor& r, std::shared_ptr<Registry> registry)
        : reactor_(&r), core_(std::make_shared<StreamCore>(r, std::move(registry))) {}

    ~AsioTcpStream() override {
        // Close on the reactor thread (pending ops complete `closed`); the last reference -- the posted
        // lambda's or a pending handler's -- then frees the stream there. If the io_context is already being
        // destroyed its shutdown has destroyed the socket, and dropping our reference only frees memory. The
        // lambda gets a COPY, so it never holds the last reference while the registry lock is held here.
        {
            std::lock_guard lock(core_->registry->m);
            if (!core_->registry->dead) {
                try {
                    asio::post(reactor_->io(), [core = core_] { core->close_socket(); });
                } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory posting; dropped below instead
                }
            }
        }
        core_.reset();
    }
    AsioTcpStream(AsioTcpStream const&)            = delete;
    AsioTcpStream& operator=(AsioTcpStream const&) = delete;

    [[nodiscard]] Reactor& reactor() const noexcept override { return *reactor_; }

    void start_connect(std::shared_ptr<TcpOp> op, IpAddress address, std::uint16_t port) override {
        asio::post(reactor_->io(), [core = core_, op = std::move(op), address, port]() mutable {
            if (refused_at_start(*core->reactor, *core, *op)) return;
            if (core->connect_started) {
                complete_now(*op, op_status::completed, tcp_error::busy);
                return;
            }
            core->connect_started = true;
            tcp::endpoint const ep = to_endpoint(address, port);
            start_registered(
                core, std::move(op), [&](auto handler) { core->sock().async_connect(ep, std::move(handler)); },
                [](StreamCore& c, std::error_code ec) {
                    if (!ec) {
                        std::error_code ignored;
                        (void)c.sock().set_option(tcp::no_delay(true), ignored);
                    }
                });
        });
    }

    void start_read_some(std::shared_ptr<TcpOp> op) override { start_read_side(std::move(op), false); }
    void start_wait_readable(std::shared_ptr<TcpOp> op) override { start_read_side(std::move(op), true); }

    void start_write_some(std::shared_ptr<TcpOp> op) override {
        asio::post(reactor_->io(), [core = core_, op = std::move(op)]() mutable {
            if (refused_at_start(*core->reactor, *core, *op)) return;
            if (!has_range(*op)) {
                complete_now(*op, op_status::completed, tcp_error::invalid_argument);
                return;
            }
            if (core->writing) {
                complete_now(*op, op_status::completed, tcp_error::busy);
                return;
            }
            if (!core->socket || !core->socket->is_open()) {
                complete_now(*op, op_status::completed, tcp_error::closed);
                return;
            }
            core->writing = true;
            // The bytes live in the op's buffer, which the handler (holding `op`) keeps alive.
            auto const bytes = asio::buffer(op->buffer()->data() + op->buffer_offset(),
                                            op->buffer()->size() - op->buffer_offset());
            start_registered(
                core, std::move(op), [&](auto handler) { core->socket->async_write_some(bytes, std::move(handler)); },
                [](StreamCore& c, std::error_code) { c.writing = false; });
        });
    }

    void close() override {
        asio::post(reactor_->io(), [core = core_] { core->close_socket(); });
    }

private:
    static bool has_range(TcpOp const& op) noexcept {
        return op.buffer() && op.buffer_offset() < op.buffer()->size();
    }

    void start_read_side(std::shared_ptr<TcpOp> op, bool readiness_only) {
        asio::post(reactor_->io(), [core = core_, op = std::move(op), readiness_only]() mutable {
            if (refused_at_start(*core->reactor, *core, *op)) return;
            if (!readiness_only && !has_range(*op)) {
                complete_now(*op, op_status::completed, tcp_error::invalid_argument);
                return;
            }
            if (core->reading) {
                complete_now(*op, op_status::completed, tcp_error::busy);
                return;
            }
            if (!core->socket || !core->socket->is_open()) {
                complete_now(*op, op_status::completed, tcp_error::closed);
                return;
            }
            core->reading = true;
            if (readiness_only) {
                start_registered(
                    core, std::move(op),
                    [&](auto handler) { core->socket->async_wait(tcp::socket::wait_read, std::move(handler)); },
                    [](StreamCore& c, std::error_code) { c.reading = false; });
                return;
            }
            auto const bytes = asio::buffer(op->buffer()->data() + op->buffer_offset(),
                                            op->buffer()->size() - op->buffer_offset());
            start_registered(
                core, std::move(op), [&](auto handler) { core->socket->async_read_some(bytes, std::move(handler)); },
                [](StreamCore& c, std::error_code) { c.reading = false; });
        });
    }

    AsioReactor*                reactor_;
    std::shared_ptr<StreamCore> core_;  // moved into the release posted by the destructor
};

}  // namespace
}  // namespace asio_backend

tcp_error classify_tcp_error(std::error_code ec) noexcept {
    namespace e = asio::error;
    if (!ec) return tcp_error::none;
    if (ec == e::operation_aborted) return tcp_error::canceled;
    if (ec == e::eof) return tcp_error::eof;
    if (ec == e::connection_refused) return tcp_error::connection_refused;
    if (ec == e::connection_reset || ec == e::connection_aborted || ec == e::broken_pipe) {
        return tcp_error::connection_reset;
    }
    if (ec == e::timed_out) return tcp_error::timed_out;
    if (ec == e::network_unreachable || ec == e::host_unreachable || ec == e::network_down) {
        return tcp_error::unreachable;
    }
    // §6.3 round-2 G-g: descriptor exhaustion is a `resource` error. asio::error::no_descriptors is EMFILE
    // (POSIX) / WSAEMFILE (Windows).
    if (ec == e::no_descriptors) return tcp_error::too_many_open_files;
    if (ec.category() == std::system_category() || ec.category() == std::generic_category()) {
#if defined(_WIN32)
        switch (ec.value()) {
            case 10055:  // WSAENOBUFS: how Windows reports socket-handle / buffer exhaustion
                return tcp_error::too_many_open_files;
            case 1225:   // ERROR_CONNECTION_REFUSED (ConnectEx)
            case 1234:   // ERROR_PORT_UNREACHABLE
                return tcp_error::connection_refused;
            case 64:     // ERROR_NETNAME_DELETED
            case 1236:   // ERROR_CONNECTION_ABORTED
                return tcp_error::connection_reset;
            case 121:    // ERROR_SEM_TIMEOUT
                return tcp_error::timed_out;
            case 1231:   // ERROR_NETWORK_UNREACHABLE
            case 1232:   // ERROR_HOST_UNREACHABLE
                return tcp_error::unreachable;
            default:
                break;
        }
#else
        if (ec.value() == EMFILE || ec.value() == ENFILE) return tcp_error::too_many_open_files;
#endif
    }
    if (ec == std::errc::too_many_files_open || ec == std::errc::too_many_files_open_in_system) {
        return tcp_error::too_many_open_files;
    }
    if (ec == e::not_connected || ec == e::bad_descriptor || ec == e::not_socket || ec == e::shut_down) {
        return tcp_error::closed;
    }
    if (ec == e::invalid_argument || ec == e::address_family_not_supported) return tcp_error::invalid_argument;
    return tcp_error::other;
}

std::optional<IpAddress> parse_ip_address(std::string_view text) noexcept {
    try {
        std::error_code   ec;
        std::string const s(text);
        auto const        a = asio::ip::make_address(s.c_str(), ec);  // numeric only: inet_pton, never DNS
        if (ec) return std::nullopt;
        if (a.is_v4()) {
            auto const b = a.to_v4().to_bytes();
            return IpAddress::v4(b[0], b[1], b[2], b[3]);
        }
        auto const                   v6 = a.to_v6();
        std::array<std::uint8_t, 16> b{};
        auto const                   raw = v6.to_bytes();
        for (std::size_t i = 0; i < b.size(); ++i) b[i] = raw[i];
        return IpAddress::v6(b, static_cast<std::uint32_t>(v6.scope_id()));
    } catch (...) {
        return std::nullopt;
    }
}

std::shared_ptr<TcpStream> make_tcp_stream(Reactor& reactor) {
    auto* asio_reactor = asio_backend::AsioReactor::from(reactor);  // no RTTI (CONVENTIONS)
    if (asio_reactor == nullptr) {
        throw std::invalid_argument("pal::make_tcp_stream: the reactor is not the default (Asio) reactor");
    }
    auto& lifetime = asio::use_service<asio_backend::LifetimeService>(asio_reactor->io());  // thread-safe
    return std::make_shared<asio_backend::AsioTcpStream>(*asio_reactor, lifetime.registry);
}

}  // namespace agentengine::pal
