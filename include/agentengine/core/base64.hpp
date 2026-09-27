#pragma once
// #120 S8 (decisions/ADR-203-one-base64-one-process-identity.md): the one base64 codec (RFC 4648 §4 alphabet,
// '=' padding on encode). Replaces four hand-copied codecs: src/core/message_json.cpp's private one (ADR-202),
// protocol/a2a/types.hpp's `a2a::detail::base64_*`, protocol/mcp/client.hpp's `mcp::client_detail::base64_encode`
// and src/backends/native_jail/relay_base64.hpp. Those names remain as thin wrappers over this file.
//
// Std only, header-inline and layer V (tools/layers.toml): every layer may include it, from the L1 native-jail
// relay to the L4 protocol headers, without a link dependency.
//
// One encoder. Exactly two decoders, because the old copies decoded two different ways and both are kept as they
// were (ADR-203 §4); each is named by what it does. Neither reports why it failed: each caller maps `nullopt` to
// its own error, exactly as its old copy did.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agentengine::base64 {

inline constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

namespace detail {

[[nodiscard]] inline std::string encode_raw(unsigned char const* data, std::size_t n) {
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

// The alphabet's value for `c`, or -1 for any other character (padding included).
[[nodiscard]] constexpr int decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

enum class PaddingRule { kSkipWithLineBreaks, kStop };

// The one decode loop. The two rules differ only in what the loop does on '=' (and on '\n'/'\r').
[[nodiscard]] inline std::optional<std::vector<std::byte>> decode(std::string_view text, PaddingRule rule) {
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (rule == PaddingRule::kStop) {
            if (c == '=') break;
        } else if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
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

}  // namespace detail

// Encodes bytes. `std::vector<std::byte>` and a (pointer, size) pair as `std::span{p, n}` both convert.
[[nodiscard]] inline std::string encode(std::span<std::byte const> bytes) {
    return detail::encode_raw(reinterpret_cast<unsigned char const*>(bytes.data()), bytes.size());
}

// Encodes the bytes of `text` (each `char` as its unsigned byte value).
[[nodiscard]] inline std::string encode(std::string_view text) {
    return detail::encode_raw(reinterpret_cast<unsigned char const*>(text.data()), text.size());
}

// Lenient decode (the Message JSON codec and A2A `raw` parts): '=', '\n' and '\r' are skipped wherever they appear,
// a trailing partial group is dropped, any length is accepted, and the result is nullopt only for a character
// outside the alphabet.
[[nodiscard]] inline std::optional<std::vector<std::byte>> decode_lenient(std::string_view text) {
    return detail::decode(text, detail::PaddingRule::kSkipWithLineBreaks);
}

// Stop-at-padding decode (the native-jail HandleRelay wire): decoding ends at the FIRST '=' and everything after it
// is ignored unread, whatever it is; before it, every character must be in the alphabet ('\n' and '\r' included in
// "not"), else nullopt. A trailing partial group is dropped and any length is accepted.
[[nodiscard]] inline std::optional<std::vector<std::byte>> decode_stop_at_padding(std::string_view text) {
    return detail::decode(text, detail::PaddingRule::kStop);
}

}  // namespace agentengine::base64
