#pragma once
// A small, self-contained base64 codec for HandleRelay's socket-byte relay
// (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md §2 items 2/3):
// `connect_send`/`connect_recv` payloads carry arbitrary guest bytes across the worker_query wire,
// which is JSON text (core/json_value.hpp strings are not a byte-safe transport on their own).
//
// Deliberately a fresh, narrow implementation rather than reusing
// `agentengine::rt::message_codec_detail::base64_encode/decode` -- that pair's own file header
// argues explicitly for duplicating a small, already-proven codec over pulling in an unrelated
// module's dependency chain (there, `core/content.hpp`/`core/chat_client.hpp`; here, this file would
// otherwise become the ONE thing coupling the native-jail worker-mediation TUs to `rt/`). Shared by
// both sides of the relay (native_jail_backend.cpp, the host; python_worker_mediation.cpp, the
// worker) so the encoding is not implemented twice within this same directory.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentengine::native_jail::relay_base64 {

inline constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline std::string encode(std::byte const* data, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= n) {
        std::uint32_t const v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                 (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                 static_cast<std::uint32_t>(data[i + 2]);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
        i += 3;
    }
    std::size_t const remaining = n - i;
    if (remaining == 1) {
        std::uint32_t const v = static_cast<std::uint32_t>(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        std::uint32_t const v =
            (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

[[nodiscard]] inline std::string encode(std::vector<std::byte> const& bytes) {
    return encode(bytes.data(), bytes.size());
}

// Returns nullopt on malformed input (a character outside the alphabet/padding set) -- callers treat
// that as a protocol violation (RT1's own "never trust an unparseable frame" posture), not a value to
// silently truncate.
[[nodiscard]] inline std::optional<std::vector<std::byte>> decode(std::string_view text) {
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        int const v = decode_char(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace agentengine::native_jail::relay_base64
