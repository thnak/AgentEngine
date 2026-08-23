// Implements jailed_worker_rpc.hpp. See that header for the spec citation this satisfies.

#include "backends/native_jail/jailed_worker_rpc.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace agentengine::native_jail {

namespace {

result<void> write_all(HANDLE h, void const* data, std::size_t size) {
    auto const* p = static_cast<std::byte const*>(data);
    std::size_t written = 0;
    while (written < size) {
        DWORD chunk = 0;
        DWORD const to_write =
            static_cast<DWORD>((size - written) > 0xFFFF'FFFFull ? 0xFFFF'FFFFu
                                                                   : (size - written));
        if (!WriteFile(h, p + written, to_write, &chunk, nullptr) || chunk == 0) {
            return std::unexpected(error{failure_class::transient,
                                          "FramedChannel::send: WriteFile failed or wrote 0 bytes "
                                          "(peer pipe likely closed)",
                                          "jailed_worker_rpc.write_failed"});
        }
        written += chunk;
    }
    return {};
}

result<void> read_all(HANDLE h, void* data, std::size_t size) {
    auto* p = static_cast<std::byte*>(data);
    std::size_t got = 0;
    while (got < size) {
        DWORD chunk = 0;
        DWORD const to_read =
            static_cast<DWORD>((size - got) > 0xFFFF'FFFFull ? 0xFFFF'FFFFu : (size - got));
        if (!ReadFile(h, p + got, to_read, &chunk, nullptr) || chunk == 0) {
            // EOF/broken pipe (ERROR_BROKEN_PIPE) or any other read failure -- both mean "the peer
            // is gone," which is exactly what a caller mid-`exec_session()` needs to detect (the
            // worker died) rather than a crash inside ReadFile's caller.
            return std::unexpected(error{failure_class::transient,
                                          "FramedChannel::recv: ReadFile failed or returned 0 bytes "
                                          "(peer pipe closed)",
                                          "jailed_worker_rpc.read_failed"});
        }
        got += chunk;
    }
    return {};
}

}  // namespace

result<void> FramedChannel::send(json::Value const& msg) const {
    std::string body = json::dump(msg);
    if (body.size() > kMaxFrameBytes) {
        return std::unexpected(error{failure_class::resource,
                                      "FramedChannel::send: frame exceeds kMaxFrameBytes",
                                      "jailed_worker_rpc.frame_too_large"});
    }
    std::uint32_t const len_le = static_cast<std::uint32_t>(body.size());
    // Explicit little-endian byte layout -- do not rely on host endianness matching the wire
    // convention (this project's two peers always run on the SAME machine today, but the framing
    // itself should not silently depend on that).
    std::byte prefix[4] = {
        static_cast<std::byte>(len_le & 0xFF),
        static_cast<std::byte>((len_le >> 8) & 0xFF),
        static_cast<std::byte>((len_le >> 16) & 0xFF),
        static_cast<std::byte>((len_le >> 24) & 0xFF),
    };
    if (auto w = write_all(write_handle_, prefix, sizeof(prefix)); !w) return w;
    if (body.empty()) return {};
    return write_all(write_handle_, body.data(), body.size());
}

result<json::Value> FramedChannel::recv() const {
    std::byte prefix[4];
    if (auto r = read_all(read_handle_, prefix, sizeof(prefix)); !r) return std::unexpected(r.error());
    std::uint32_t const len = static_cast<std::uint32_t>(prefix[0]) |
                               (static_cast<std::uint32_t>(prefix[1]) << 8) |
                               (static_cast<std::uint32_t>(prefix[2]) << 16) |
                               (static_cast<std::uint32_t>(prefix[3]) << 24);
    if (len > kMaxFrameBytes) {
        return std::unexpected(error{failure_class::resource,
                                      "FramedChannel::recv: peer announced a frame exceeding "
                                      "kMaxFrameBytes -- treated as a protocol violation",
                                      "jailed_worker_rpc.frame_too_large"});
    }
    std::string body(len, '\0');
    if (len > 0) {
        if (auto r = read_all(read_handle_, body.data(), len); !r) return std::unexpected(r.error());
    }
    auto parsed = json::parse(body);
    if (!parsed) return std::unexpected(parsed.error());
    return parsed;
}

}  // namespace agentengine::native_jail
