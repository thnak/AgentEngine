// decisions/ADR-205-strict-relay-base64.md: `base64::decode_strict` (the native-jail HandleRelay wire's decode rule)
// accepts canonical padded base64 and nothing else.
//
// The reference is written from the rule's definition, not from the decoder's code: a string is canonical exactly
// when it is `encode(b)` for some bytes `b`, and then `decode_lenient` (which skips '=' and line breaks) gives that
// `b`. So `reference(s)` = `decode_lenient(s)` when re-encoding it reproduces `s`, else nullopt.
//
// 1. Round trips: every length 0..64 (zeros, 0xFF, counting, 3 random seeds), every 1- and 2-byte string, and random
//    buffers up to 64 KiB, through encode -> decode_strict.
// 2. The last quantum, exhaustively: every `ab==` and `abc=` over the alphabet; exactly the ones with zero leftover
//    bits are accepted, and there are exactly 256 and 65,536 of them (one per 1- and 2-byte string).
// 3. Every string of length 0..4 over a 14-character pool and of length 5..8 over a 4-character pool, against the
//    reference.
// 4. A malformed corpus (bad length, junk, line breaks, padding in the wrong place, content after padding, non-zero
//    leftover bits, the ADR-205 example `QQ==!!garbage`) that must be rejected.
// 5. 200,000 seeded fuzz inputs (random text weighted toward the alphabet, '=' and line breaks; mutated encodings)
//    against the reference.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/base64.hpp"

namespace {

namespace b64 = agentengine::base64;
using Bytes = std::vector<std::byte>;

int g_failures = 0;
long g_checks = 0;
long g_accepted = 0;
long g_rejected = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        if (g_failures <= 50) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string printable(std::string_view s) {
    std::string out;
    for (char c : s) {
        auto const u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F && c != '\\') {
            out += c;
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", u);
            out += buf;
        }
    }
    return out.size() > 80 ? out.substr(0, 80) + "...(" + std::to_string(s.size()) + " chars)" : out;
}

std::string encode(Bytes const& b) { return b64::encode(std::span<std::byte const>(b)); }

std::optional<Bytes> reference(std::string_view s) {
    auto d = b64::decode_lenient(s);
    if (!d || encode(*d) != s) return std::nullopt;
    return d;
}

// decode_strict(s) against the reference.
void against_reference(std::string_view s, std::string const& label) {
    auto const got = b64::decode_strict(s);
    auto const want = reference(s);
    if (got) {
        ++g_accepted;
    } else {
        ++g_rejected;
    }
    check(got == want, label + " '" + printable(s) + "': strict " + (got ? "accepts" : "rejects") + ", reference " +
                           (want ? "accepts" : "rejects"));
}

void round_trip(Bytes const& b, std::string const& label) {
    std::string const enc = encode(b);
    auto const got = b64::decode_strict(enc);
    ++g_accepted;
    check(got.has_value() && *got == b, "round trip " + label + " '" + printable(enc) + "'");
}

Bytes random_bytes(std::mt19937_64& rng, std::size_t n) {
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::byte>(rng() & 0xFF);
    return b;
}

// Calls f on every string of length `len` over `pool`.
template <class F>
void for_each_string(std::string_view pool, std::size_t len, F&& f) {
    std::vector<std::size_t> idx(len, 0);
    std::string s(len, '\0');
    while (true) {
        for (std::size_t i = 0; i < len; ++i) s[i] = pool[idx[i]];
        f(s);
        std::size_t i = 0;
        while (i < len && ++idx[i] == pool.size()) idx[i++] = 0;
        if (i == len) return;
    }
}

}  // namespace

int main() {
    // 1. Round trips.
    for (std::size_t n = 0; n <= 64; ++n) {
        round_trip(Bytes(n, std::byte{0x00}), "zeros len " + std::to_string(n));
        round_trip(Bytes(n, std::byte{0xFF}), "0xFF len " + std::to_string(n));
        Bytes counting(n);
        for (std::size_t i = 0; i < n; ++i) counting[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);
        round_trip(counting, "counting len " + std::to_string(n));
        for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
            std::mt19937_64 rng(seed * 1000 + n);
            round_trip(random_bytes(rng, n), "random len " + std::to_string(n));
        }
    }
    for (int a = 0; a < 256; ++a) {
        round_trip(Bytes{static_cast<std::byte>(a)}, "byte " + std::to_string(a));
        for (int b = 0; b < 256; ++b) {
            round_trip(Bytes{static_cast<std::byte>(a), static_cast<std::byte>(b)},
                       "bytes " + std::to_string(a) + "," + std::to_string(b));
        }
    }
    {
        std::mt19937_64 rng(42);
        for (std::size_t n : {std::size_t{65534}, std::size_t{65535}, std::size_t{65536}}) {
            round_trip(random_bytes(rng, n), "random len " + std::to_string(n));
        }
        for (int i = 0; i < 64; ++i) {
            std::size_t const n = rng() % (65536 + 1);
            round_trip(random_bytes(rng, n), "random len " + std::to_string(n));
        }
    }

    // 2. The last quantum, exhaustively.
    long one_byte = 0;
    long two_byte = 0;
    for (char a : b64::kAlphabet) {
        for (char b : b64::kAlphabet) {
            std::string const s2 = std::string{a, b} + "==";
            bool const zero_tail2 = (b64::detail::decode_char(b) & 0x0F) == 0;
            auto const d2 = b64::decode_strict(s2);
            check(d2.has_value() == zero_tail2, "'" + s2 + "' accepted iff its 4 leftover bits are zero");
            if (d2) {
                ++one_byte;
                check(d2->size() == 1 && encode(*d2) == s2, "'" + s2 + "' decodes to 1 byte that re-encodes to it");
            }
            for (char c : b64::kAlphabet) {
                std::string const s3 = std::string{a, b, c} + "=";
                bool const zero_tail3 = (b64::detail::decode_char(c) & 0x03) == 0;
                auto const d3 = b64::decode_strict(s3);
                check(d3.has_value() == zero_tail3, "'" + s3 + "' accepted iff its 2 leftover bits are zero");
                if (d3) {
                    ++two_byte;
                    check(d3->size() == 2 && encode(*d3) == s3, "'" + s3 + "' decodes to 2 bytes that re-encode to it");
                }
            }
        }
    }
    check(one_byte == 256, "exactly 256 accepted 'ab==' strings, got " + std::to_string(one_byte));
    check(two_byte == 65536, "exactly 65,536 accepted 'abc=' strings, got " + std::to_string(two_byte));

    // 3. Every short string over small pools, against the reference.
    constexpr std::string_view kPool14{"AQgw/+8=\n\r !\0\x80", 14};
    for (std::size_t len = 0; len <= 4; ++len) {
        for_each_string(kPool14, len, [&](std::string const& s) { against_reference(s, "pool14 len " + std::to_string(len)); });
    }
    constexpr std::string_view kPool4{"AQ=!"};
    for (std::size_t len = 5; len <= 8; ++len) {
        for_each_string(kPool4, len, [&](std::string const& s) { against_reference(s, "pool4 len " + std::to_string(len)); });
    }

    // 4. Malformed corpus: every entry must be rejected (and the reference must agree it is malformed).
    std::vector<std::string> malformed = {
        // ADR-205's example and other content after padding
        "QQ==!!garbage", "QQ==QQ==", "QQ==QUJD", "QUI=QUJD", "QQ==\n", "QQ==\r\n", "QQ== ", "QUI=!", "QQ==A===",
        // bad length
        "Q", "QQ", "QUJ", "QUJDR", "QUJDRA", "QUJDREU", "QQ=", "QQ===", "QUI", "=", "==", "===",
        // misplaced padding
        "====", "A===", "=QUJ", "Q=UJ", "QU=J", "=AAA", "AA=A", "QQ=A", "QUJD====", "====QUJD", "QUJD=QUJ",
        // characters outside the alphabet (line breaks, whitespace, URL-safe, NUL, high bytes)
        "QUJD\n", "\nQUJD", "QU\nJD\n\n\n", "QU\r\nJD==", "QUJ ", " QUJ", "QU\tJ", "-_-_", "QUJ-", "QUJ_",
        "QU!D", "QUJ.", std::string("QU\0D", 4), std::string("\0\0\0\0", 4), "QUJ\x80", "\xC3\xA9\xC3\xA9",
        // non-zero leftover bits
        "QR==", "QT==", "Q/==", "QUK=", "QUL=", "QU/=", "AB==", "AAB=", "QUJDRF==", "QUJDREX=",
    };
    {
        std::mt19937_64 rng(5);
        std::string const big = encode(random_bytes(rng, 65536));
        malformed.push_back(big + "!");
        malformed.push_back(big + "\n");
        malformed.push_back(big + "=");
        malformed.push_back(big.substr(0, big.size() - 1));
        std::string mid = big;
        mid[mid.size() / 2] = '=';
        malformed.push_back(mid);
        std::string const big_padded = encode(random_bytes(rng, 65534));  // ends in '='
        malformed.push_back(big_padded + "junk");
    }
    for (auto const& s : malformed) {
        ++g_rejected;
        check(!b64::decode_strict(s), "malformed input accepted: '" + printable(s) + "'");
        check(!reference(s), "corpus entry is not actually malformed: '" + printable(s) + "'");
    }

    // 5. Fuzz against the reference.
    for (std::uint64_t seed : {21ULL, 22ULL}) {
        std::mt19937_64 rng(seed);
        static constexpr std::string_view kPool =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
            "======\n\r -_!\x80\xFF";
        static constexpr std::string_view kEdits = "=\n\r !-_A/+Q\x80";
        for (int i = 0; i < 50000; ++i) {
            std::string text;
            std::size_t const n = rng() % 33;
            for (std::size_t k = 0; k < n; ++k) {
                text += rng() % 97 == 0 ? static_cast<char>(rng() & 0xFF) : kPool[rng() % kPool.size()];
            }
            against_reference(text, "fuzz text seed " + std::to_string(seed));

            std::string enc = encode(random_bytes(rng, rng() % 40));
            int const edits = 1 + static_cast<int>(rng() % 3);
            for (int e = 0; e < edits; ++e) {
                char const c = kEdits[rng() % kEdits.size()];
                std::size_t const at = enc.empty() ? 0 : rng() % (enc.size() + 1);
                switch (rng() % 4) {
                    case 0: enc.insert(enc.begin() + static_cast<std::ptrdiff_t>(at), c); break;
                    case 1: if (at < enc.size()) enc[at] = c; break;
                    case 2: if (at < enc.size()) enc.erase(at, 1); break;
                    default: enc += c; break;
                }
            }
            against_reference(enc, "fuzz mutation seed " + std::to_string(seed));
        }
    }

    std::printf("decode_strict: %ld accepted, %ld rejected (malformed corpus %zu); checks %ld\n", g_accepted,
                g_rejected, malformed.size(), g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "test_base64_strict: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_base64_strict: all checks passed\n");
    return 0;
}
