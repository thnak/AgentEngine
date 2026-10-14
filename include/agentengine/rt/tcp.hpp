#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.2/§4.4/§6.3 -- coroutine TCP on
// the I/O reactor: `co_await rt::tcp_connect / tcp_read_some / tcp_wait_readable`, and `rt::tcp_write_all`
// (a task looping over write_some). Built on pal/reactor_tcp.hpp; the foundation the HTTP/SSE client is
// rewritten on (§6.3). TLS and DNS are not here: connect takes an already-resolved, verified address (ADR-011).
//
// Every awaitable follows rt/sleep.hpp's discipline exactly (that header is the reference; the op record
// is the shared one in rt/reactor_await.hpp):
//   - the waiter resumes on the home it parked from (ADR-175 block_on home or ADR-219 host Resumer), never on
//     the reactor thread; a homeless waiter is REFUSED and counted (`Reactor::homeless_refusals`, §4.2);
//   - a stop request posts a cancel to the reactor; the waiter resumes from the backend's own completion,
//     which reports what really happened -- bytes a read had already transferred stay reported (two-phase,
//     round-1 M1); the request is sticky, so a stop landing before the start is not lost (round-2 M2);
//   - the stop_callback is registered BEFORE the start, and nothing in the frame is touched after the start;
//   - a frame destroyed while waiting abandons its operation (the completion touches nothing in it) and
//     cancels it early; a continuation already queued at a host Resumer is claimed so it never runs.
// Buffers are the operation record's (§6.3, round-3 gap 6): a read lands in a vector the op owns and is
// MOVED into the result after completion; `tcp_write_all` copies the caller's bytes once into a buffer its
// ops share. The kernel is never handed frame memory, so destroying a frame mid-read is not a use-after-free.
//
// Results are values (001 §6), never exceptions for control flow: `TcpResult::error`. A stream whose
// in-flight operation was canceled is closed by the backend (§4.4).

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/reactor_await.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// What a TCP awaitable reports.
// ae-naming-lint: allow TcpResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct TcpResult {
    pal::tcp_error         error = pal::tcp_error::none;
    std::size_t            bytes = 0;  // read: data.size(); write_all: bytes written, even on failure
    std::error_code        native{};   // the backend's own code, when there was one
    std::vector<std::byte> data;       // tcp_read_some only

    [[nodiscard]] bool ok() const noexcept { return error == pal::tcp_error::none; }
};

namespace tcp_detail {

enum class kind : std::uint8_t { connect, read_some, wait_readable, write_some };

// The shared reactor operation record (rt/reactor_await.hpp), on a TcpOp.
using TcpAwaitOp = reactor_detail::AwaitedOp<pal::TcpOp>;
using Canceler   = reactor_detail::Canceler;


class TcpAwaiter {
public:
    TcpAwaiter(pal::TcpStream& stream, kind k, std::stop_token stop, pal::IpAddress address = {},
               std::uint16_t port = 0, pal::TcpBuffer buffer = {}, std::size_t offset = 0) noexcept
        : stream_(&stream), kind_(k), stop_(std::move(stop)), address_(address), port_(port),
          buffer_(std::move(buffer)), offset_(offset) {}

    TcpAwaiter(TcpAwaiter const&)            = delete;
    TcpAwaiter& operator=(TcpAwaiter const&) = delete;

    ~TcpAwaiter() {
        if (!op_) return;
        cb_.reset();
        if (op_->abandon()) Canceler{&stream_->reactor(), op_}();  // frame destroyed mid-operation
    }

    [[nodiscard]] bool await_ready() noexcept { return stop_.stop_requested(); }

    void await_suspend(std::coroutine_handle<> h) {
        pal::Reactor& reactor = stream_->reactor();
        // Everything that can throw happens before the start, while nothing refers to `h` yet.
        auto op = std::make_shared<TcpAwaitOp>(reactor, detail::capture_parked(h));
        op->set_buffer(buffer_, offset_);
        if (stop_.stop_possible()) cb_.emplace(stop_, Canceler{&reactor, op});
        op_ = op;
        try {
            switch (kind_) {
                case kind::connect: stream_->start_connect(std::move(op), address_, port_); break;
                case kind::read_some: stream_->start_read_some(std::move(op)); break;
                case kind::wait_readable: stream_->start_wait_readable(std::move(op)); break;
                case kind::write_some: stream_->start_write_some(std::move(op)); break;
            }
        } catch (...) {
            cb_.reset();
            op_.reset();
            throw;
        }
        // Nothing in this frame may be touched from here on: the continuation may already be running.
    }

    [[nodiscard]] TcpResult await_resume() noexcept {
        TcpResult r;
        if (!op_) {  // stop already requested: never started
            r.error = pal::tcp_error::canceled;
            return r;
        }
        cb_.reset();
        pal::TcpOpResult const& res = op_->result();
        r.error  = res.error;
        r.bytes  = res.transferred;
        r.native = res.native;
        if (kind_ == kind::read_some && res.transferred > 0 && buffer_) {
            // The backend has finished with the op's buffer; this awaiter is its only other owner.
            buffer_->resize(offset_ + res.transferred);
            r.data = std::move(*buffer_);
            if (offset_ > 0) r.data.erase(r.data.begin(), r.data.begin() + static_cast<std::ptrdiff_t>(offset_));
        }
        op_.reset();
        buffer_.reset();
        return r;
    }

private:
    pal::TcpStream*                             stream_;
    kind                                        kind_;
    std::stop_token                             stop_;
    pal::IpAddress                              address_;
    std::uint16_t                               port_;
    pal::TcpBuffer                              buffer_;
    std::size_t                                 offset_;
    std::shared_ptr<TcpAwaitOp>                 op_;
    std::optional<std::stop_callback<Canceler>> cb_;
};

inline task<TcpResult> write_all(pal::TcpStream* stream, pal::TcpBuffer buffer, std::stop_token stop) {
    std::size_t const total = buffer->size();
    std::size_t       done  = 0;
    while (done < total) {
        TcpResult r = co_await TcpAwaiter(*stream, kind::write_some, stop, {}, 0, buffer, done);
        done += r.bytes;
        if (!r.ok()) {
            r.bytes = done;
            co_return r;
        }
        if (r.bytes == 0) co_return TcpResult{pal::tcp_error::other, done, {}, {}};  // no progress: not a loop
    }
    co_return TcpResult{pal::tcp_error::none, done, {}, {}};
}

}  // namespace tcp_detail

// Connects `stream` to an already-resolved address (ADR-011).
[[nodiscard]] inline tcp_detail::TcpAwaiter tcp_connect(pal::TcpStream& stream, pal::IpAddress address,
                                                        std::uint16_t port, std::stop_token stop = {}) noexcept {
    return tcp_detail::TcpAwaiter(stream, tcp_detail::kind::connect, std::move(stop), address, port);
}

// Reads up to `max_bytes` (at least one byte, or `eof`) into a buffer the operation owns; the bytes come back
// in `TcpResult::data`.
[[nodiscard]] inline tcp_detail::TcpAwaiter tcp_read_some(pal::TcpStream& stream, std::size_t max_bytes,
                                                          std::stop_token stop = {}) {
    return tcp_detail::TcpAwaiter(stream, tcp_detail::kind::read_some, std::move(stop), {}, 0,
                                  std::make_shared<std::vector<std::byte>>(max_bytes));
}

// Waits until a read would not block, consuming nothing and holding no buffer (§6.3, round-1 G6).
[[nodiscard]] inline tcp_detail::TcpAwaiter tcp_wait_readable(pal::TcpStream& stream,
                                                              std::stop_token stop = {}) noexcept {
    return tcp_detail::TcpAwaiter(stream, tcp_detail::kind::wait_readable, std::move(stop));
}

// Writes every byte of `bytes` (copied once, eagerly, into a buffer the operations own -- the caller's span
// need not outlive this call). `TcpResult::bytes` is how many were written, also on failure.
[[nodiscard]] inline task<TcpResult> tcp_write_all(pal::TcpStream& stream, std::span<std::byte const> bytes,
                                                   std::stop_token stop = {}) {
    return tcp_detail::write_all(&stream, std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end()),
                                 std::move(stop));
}

}  // namespace agentengine::rt
