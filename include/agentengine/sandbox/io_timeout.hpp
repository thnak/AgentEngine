#pragma once
// ADR-177 §10 (the "90 s silent provider" gap): how long the egress transports wait on a socket that has
// stopped producing bytes -- connecting, sending, or reading the next piece of a response.
//
// It was two identical `constexpr int kIoTimeoutMs = 90'000` (net_egress_proxy.cpp, tls_client.cpp), and
// that is why the stall a real session hit could not be tested: a test would have to be silent for the
// whole 90 s. It is now ONE function, read once from `AGENTENGINE_NET_IO_TIMEOUT_MS`, default 90 000 ms.
//
// A HOST-owned setting, read from the process environment, never from model output (I3), and it can only
// shorten or lengthen how long the host waits -- it grants no reach (I2). Clamped to [250 ms, 10 min] so
// a typo cannot turn every request into an instant timeout or an unbounded hang; a value that is not a
// plain integer keeps the default rather than being guessed at.
//
// Read ONCE (function-local static): changing the variable after the first network call has no effect,
// which is what a timeout that sockets already in flight were configured with should do.

#include <charconv>
#include <string>

#include "agentengine/pal/env.hpp"

namespace agentengine::sandbox {

inline constexpr int kDefaultIoTimeoutMs = 90'000;
inline constexpr int kMinIoTimeoutMs     = 250;
inline constexpr int kMaxIoTimeoutMs     = 600'000;

[[nodiscard]] inline int io_timeout_ms() {
    static int const value = [] {
        auto const raw = ::agentengine::pal::env_var("AGENTENGINE_NET_IO_TIMEOUT_MS");
        if (!raw || raw->empty()) return kDefaultIoTimeoutMs;
        int parsed = 0;
        auto const [end, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), parsed);
        if (ec != std::errc{} || end != raw->data() + raw->size()) return kDefaultIoTimeoutMs;
        if (parsed < kMinIoTimeoutMs) return kMinIoTimeoutMs;
        if (parsed > kMaxIoTimeoutMs) return kMaxIoTimeoutMs;
        return parsed;
    }();
    return value;
}

}  // namespace agentengine::sandbox
