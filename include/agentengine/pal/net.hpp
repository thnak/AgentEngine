#pragma once
// ADR-037 (removing Quark entirely, including as a vendored utility dependency): a small, self-
// contained cross-platform socket PAL replacing `quark::pal`'s socket primitives -- the last real
// Quark dependency left in this project's own production code (src/sandbox/net_egress_proxy.cpp,
// src/sandbox/tls_client.cpp, and roughly a dozen test files' own canned TLS/HTTP test servers all
// used `quark::pal::{fd_t, ensure_winsock, tcp_connect, tcp_listen, connect_result, local_port,
// accept_one, close_fd, send_some, recv_some, would_block}` for portable, non-blocking TCP sockets --
// nothing else from Quark's PAL (its `IoContext` reactor, UDP primitives, wake-pair, or clock) was
// ever used here, so only that narrow slice is reproduced.
//
// Faithfully mirrors `third_party/quark/pal/{windows_x86_64,linux_x86_64}/net.hpp`'s own public
// surface and error-normalization contract exactly (same function names, same non-blocking/
// `would_block()`-sentinel semantics, same `std::expected<T, std::error_code>` return shape) so every
// call site that used to say `quark::pal::` needed nothing more than a namespace rename to
// `agentengine::pal::` -- this is a REPRODUCTION of a narrow, already-proven surface, not a redesign.
// `std::error_code` (not `agentengine::error`) is kept as the error type deliberately: it is what
// every call site's own `r.error() == pal::would_block()` comparison already relies on, and it is
// already a zero-dependency standard-library type -- translating to `agentengine::error` here would
// only add a conversion step with no benefit, since this is a leaf networking primitive, not a
// pipeline stage that itself gets composed with `agentengine::result<T>`-returning code.
//
// One file, not per-OS headers like Quark's own `pal/{windows_x86_64,linux_x86_64}/` split: the
// surface used here is narrow enough that inline `#if defined(_WIN32)` branches per function (the
// same idiom this project's own `src/sandbox/tls_client.cpp`/`net_egress_proxy.cpp` already use for
// their own small `wait_ready()` helper) are clearer than a second directory-level OS seam for one
// header.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace agentengine::pal {

#if defined(_WIN32)
using fd_t = SOCKET;
inline fd_t const invalid_fd = INVALID_SOCKET;
#else
using fd_t = int;
inline constexpr fd_t invalid_fd = -1;
#endif

// One-time WSAStartup, lazily on first socket use (function-local static -- thread-safe init since
// C++11). No matching WSACleanup: this is a long-lived process-wide resource other code in the same
// process may also depend on, so it is deliberately never torn down. A no-op on non-Windows.
inline void ensure_winsock() noexcept {
#if defined(_WIN32)
    static int const rc = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa);
    }();
    (void)rc;
#endif
}

[[nodiscard]] inline std::error_code last_error() noexcept {
#if defined(_WIN32)
    return std::error_code(::WSAGetLastError(), std::system_category());
#else
    return std::error_code(errno, std::system_category());
#endif
}

// The "operation would block" sentinel a non-blocking recv/send/accept returns -- a normal
// readiness edge, NOT a failure. Callers compare `r.error() == would_block()` to decide "stop, try
// again later" versus a genuine broken connection.
[[nodiscard]] inline std::error_code would_block() noexcept {
#if defined(_WIN32)
    return std::error_code(WSAEWOULDBLOCK, std::system_category());
#else
    return std::make_error_code(std::errc::operation_would_block);
#endif
}

namespace detail {

[[nodiscard]] inline std::expected<void, std::error_code> set_nonblocking(fd_t s) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) return std::unexpected(last_error());
#else
    int const flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return std::unexpected(last_error());
    if (::fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) return std::unexpected(last_error());
#endif
    return {};
}

// Disable Nagle: best-effort, a failure here is not fatal to correctness.
inline void set_nodelay(fd_t s) noexcept {
    int const one = 1;
#if defined(_WIN32)
    (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
#else
    (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

[[nodiscard]] inline ::sockaddr_in to_sockaddr_in(std::uint64_t addr, std::uint16_t port) noexcept {
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = ::htons(port);
    sa.sin_addr.s_addr = ::htonl(static_cast<std::uint32_t>(addr & 0xFFFF'FFFFULL));
    return sa;
}

}  // namespace detail

inline void close_fd(fd_t s) noexcept {
#if defined(_WIN32)
    if (s != invalid_fd) ::closesocket(s);
#else
    if (s >= 0) ::close(s);
#endif
}

// Open a listening socket bound to (addr,port). port==0 => an ephemeral port the OS picks (read it
// back with local_port). Returns the listening socket (non-blocking, SO_REUSEADDR).
[[nodiscard]] inline std::expected<fd_t, std::error_code> tcp_listen(std::uint64_t addr,
                                                                      std::uint16_t port,
                                                                      int backlog = 128) noexcept {
    ensure_winsock();
#if defined(_WIN32)
    fd_t const s = ::socket(AF_INET, SOCK_STREAM, 0);
#else
    fd_t const s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    if (s == invalid_fd) return std::unexpected(last_error());
    int const one = 1;
#if defined(_WIN32)
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&one), sizeof(one));
#else
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    ::sockaddr_in const sa = detail::to_sockaddr_in(addr, port);
    if (::bind(s, reinterpret_cast<::sockaddr const*>(&sa), sizeof(sa)) != 0) {
        auto const e = last_error();
        close_fd(s);
        return std::unexpected(e);
    }
    if (::listen(s, backlog) != 0) {
        auto const e = last_error();
        close_fd(s);
        return std::unexpected(e);
    }
    if (auto r = detail::set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
    return s;
}

// The actual port a (possibly ephemeral) listener bound to -- so a caller can bind port 0 and dial
// back.
[[nodiscard]] inline std::expected<std::uint16_t, std::error_code> local_port(fd_t s) noexcept {
    ::sockaddr_in sa{};
#if defined(_WIN32)
    int len = sizeof(sa);
#else
    ::socklen_t len = sizeof(sa);
#endif
    if (::getsockname(s, reinterpret_cast<::sockaddr*>(&sa), &len) != 0)
        return std::unexpected(last_error());
    return ::ntohs(sa.sin_port);
}

// Begin a non-blocking connect. Returns the socket immediately; the connect is IN PROGRESS
// (WSAEWOULDBLOCK/EINPROGRESS is expected and NOT an error) -- completion is signalled by writable
// readiness, at which point connect_result(s) reports success/failure. A synchronous connect
// (loopback) may complete here too.
[[nodiscard]] inline std::expected<fd_t, std::error_code> tcp_connect(std::uint64_t addr,
                                                                       std::uint16_t port) noexcept {
    ensure_winsock();
#if defined(_WIN32)
    fd_t const s = ::socket(AF_INET, SOCK_STREAM, 0);
#else
    fd_t const s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    if (s == invalid_fd) return std::unexpected(last_error());
    if (auto r = detail::set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
    detail::set_nodelay(s);
    ::sockaddr_in const sa = detail::to_sockaddr_in(addr, port);
#if defined(_WIN32)
    if (::connect(s, reinterpret_cast<::sockaddr const*>(&sa), sizeof(sa)) != 0) {
        int const err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            close_fd(s);
            return std::unexpected(std::error_code(err, std::system_category()));
        }
    }
#else
    if (::connect(s, reinterpret_cast<::sockaddr const*>(&sa), sizeof(sa)) < 0 && errno != EINPROGRESS) {
        auto const e = last_error();
        close_fd(s);
        return std::unexpected(e);
    }
#endif
    return s;
}

// After writable-readiness on a connecting socket: was the connect successful? SO_ERROR holds the
// verdict on both backends.
[[nodiscard]] inline std::expected<void, std::error_code> connect_result(fd_t s) noexcept {
    int err = 0;
#if defined(_WIN32)
    int len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0)
        return std::unexpected(last_error());
#else
    ::socklen_t len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return std::unexpected(last_error());
#endif
    if (err != 0) return std::unexpected(std::error_code(err, std::system_category()));
    return {};
}

// Accept one pending connection (non-blocking listener). Returns the accepted socket (non-blocking,
// nodelay), or would_block() when the backlog is drained (the normal loop-exit condition).
[[nodiscard]] inline std::expected<fd_t, std::error_code> accept_one(fd_t lfd) noexcept {
#if defined(_WIN32)
    fd_t const s = ::accept(lfd, nullptr, nullptr);
    if (s == invalid_fd) {
        int const err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
    if (auto r = detail::set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
#else
    fd_t const s = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (s < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
#endif
    detail::set_nodelay(s);
    return s;
}

// Non-blocking recv. Ok(n>0) = bytes read; Ok(0) = orderly peer close (EOF); would_block() = nothing
// ready now; any other error = broken connection.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> recv_some(fd_t s, std::byte* buf,
                                                                            std::size_t n) noexcept {
#if defined(_WIN32)
    int const r = ::recv(s, reinterpret_cast<char*>(buf), static_cast<int>(n), 0);
    if (r < 0) {
        int const err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
#else
    ::ssize_t const r = ::recv(s, buf, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
#endif
    return static_cast<std::size_t>(r);
}

// Non-blocking send. Ok(n) = bytes accepted (may be < n); would_block() = the kernel send buffer is
// full, retry on the next writable-readiness. MSG_NOSIGNAL on POSIX: a write to a peer-closed socket
// returns EPIPE instead of raising SIGPIPE (which would kill the process); Windows sockets never
// raise SIGPIPE, so no analogous flag is needed there.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> send_some(fd_t s, std::byte const* buf,
                                                                            std::size_t n) noexcept {
#if defined(_WIN32)
    int const r = ::send(s, reinterpret_cast<char const*>(buf), static_cast<int>(n), 0);
    if (r < 0) {
        int const err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
#else
    ::ssize_t const r = ::send(s, buf, n, MSG_NOSIGNAL);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
#endif
    return static_cast<std::size_t>(r);
}

}  // namespace agentengine::pal
