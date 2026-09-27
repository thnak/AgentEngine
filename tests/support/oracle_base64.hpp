#pragma once
// TEST ORACLE (ADR-203, #120 S8) -- do not use outside tests/core/json/test_base64_equivalence.cpp.
// Verbatim copies of three of the OLD base64 codecs as of aa2573f, extracted by script (ADR-203 §6), frozen here so
// the differential test can compare core/base64.hpp and the wrappers over it against what shipped before:
//   - include/agentengine/protocol/a2a/types.hpp lines 116-187 (`a2a::detail::base64_*`), inside namespace
//     `agentengine::oracle_a2a` (added);
//   - include/agentengine/protocol/mcp/client.hpp lines 143-173 (`mcp::client_detail::base64_encode`), inside
//     namespace `agentengine::oracle_mcp::client_detail` (added);
//   - src/backends/native_jail/relay_base64.hpp lines 22-92 (the whole namespace; its includes are
//     below), with its namespace line and closing comment renamed `agentengine::oracle_relay_base64`.
// Nothing else is changed. The fourth copy, the Message JSON codec's, is covered by the ADR-202 oracles
// (tests/support/oracle_message_codec.hpp, oracle_chat_recording_codec.hpp), which hold its two predecessors.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::oracle_a2a {

namespace detail {

// The same base64 idiom the Message JSON codec's own `base64_encode`/`base64_decode` (src/core/message_json.cpp,
// ADR-202; formerly `core/chat_recording.hpp`'s `recording_detail`) already establish for the one non-text `ContentItem` payload -- reproduced here
// rather than pulled in through an unrelated recording-codec header for what is otherwise a
// self-contained, dependency-free six-line table lookup (CONVENTIONS' own "core... no third-party
// dependency" posture extends to not manufacturing cross-feature header coupling for one function).
inline constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline std::string base64_encode(std::vector<std::byte> const& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                           static_cast<std::uint32_t>(bytes[i + 2]);
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += base64_alphabet[(n >> 6) & 0x3F];
        out += base64_alphabet[n & 0x3F];
        i += 3;
    }
    std::size_t const remaining = bytes.size() - i;
    if (remaining == 1) {
        std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += base64_alphabet[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

[[nodiscard]] inline result<std::vector<std::byte>> base64_decode(std::string_view text) {
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
        if (c == '=' || c == '\n' || c == '\r') continue;
        int const v = decode_char(c);
        if (v < 0) {
            return std::unexpected(
                error{failure_class::contract, "invalid base64 character", "a2a.bad_base64"});
        }
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

}  // namespace agentengine::oracle_a2a

namespace agentengine::oracle_mcp::client_detail {

[[nodiscard]] inline std::string base64_encode(std::string_view in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        std::uint32_t const n = (static_cast<unsigned char>(in[i]) << 16) |
                                 (static_cast<unsigned char>(in[i + 1]) << 8) |
                                 static_cast<unsigned char>(in[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
    }
    if (i + 1 == in.size()) {
        std::uint32_t const n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        std::uint32_t const n = (static_cast<unsigned char>(in[i]) << 16) |
                                 (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

}  // namespace agentengine::oracle_mcp::client_detail

namespace agentengine::oracle_relay_base64 {

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

}  // namespace agentengine::oracle_relay_base64
