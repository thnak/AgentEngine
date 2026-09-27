// #120 S8 (decisions/ADR-203-one-base64-one-process-identity.md): proves the one base64 codec (core/base64.hpp) and
// every wrapper over it behave exactly like the copies they replaced.
//
// Oracles, all verbatim copies of the old code:
//   - tests/support/oracle_base64.hpp: a2a/types.hpp's `detail::base64_*`, mcp/client.hpp's
//     `client_detail::base64_encode`, native_jail/relay_base64.hpp (extracted by script, ADR-203 §6);
//   - tests/support/oracle_message_codec.hpp, oracle_chat_recording_codec.hpp (ADR-202): the two codecs the Message
//     JSON codec's private base64 came from.
//
// 1. Encode: every encoder (new core, new wrappers, six old ones) must give byte-identical text for byte strings of
//    every length 0..300 (random, all-zero, all-0xFF, counting) plus every single byte value.
// 2. Decode: for a hand-built malformed corpus, round trips of every encoding, and a seeded fuzz loop,
//      - lenient: `base64::decode_lenient` against both Message-codec oracles, `a2a::detail::base64_decode` against
//        its oracle (value, or failure_class + code + message), and the Message JSON codec end to end (a `media`
//        item's `bytes_base64`, both profiles) against the ADR-202 oracles;
//      - relay (ADR-205 changed it on purpose): `relay_base64::decode` is `base64::decode_strict`; wherever the old
//        relay decoder accepted, strict gives the same bytes on a canonical input and rejects a non-canonical one
//        (each rejection classified: content after '=', bad length, misplaced '=', non-zero leftover bits);
//        wherever the old one rejected, strict rejects too; anything strict accepts re-encodes to itself.
// 3. Self-control: the two OLD decode rules must compare as different on inputs where they really differ, so the
//    comparison demonstrably can fail; and the old relay rule and strict differ on ADR-205's examples.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/base64.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/a2a/types.hpp"
#include "agentengine/protocol/mcp/client.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "backends/native_jail/relay_base64.hpp"

#include "../../support/oracle_base64.hpp"
#include "../../support/oracle_chat_recording_codec.hpp"
#include "../../support/oracle_message_codec.hpp"

namespace {

namespace ae = agentengine;
using Bytes = std::vector<std::byte>;

int g_failures = 0;
long g_checks = 0;
long g_encodings = 0;
long g_decodings = 0;
long g_decode_errors = 0;

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
    return out.size() > 120 ? out.substr(0, 120) + "...(" + std::to_string(s.size()) + " chars)" : out;
}

std::string_view as_chars(Bytes const& b) {
    return {reinterpret_cast<char const*>(b.data()), b.size()};
}

// ---- 1. encode -------------------------------------------------------------------------------------

void diff_encode(Bytes const& b, std::string const& label) {
    std::string const expect = ae::base64::encode(std::span<std::byte const>(b));
    std::vector<std::pair<char const*, std::string>> got = {
        // new
        {"base64::encode(string_view)", ae::base64::encode(as_chars(b))},
        {"a2a::detail::base64_encode", ae::a2a::detail::base64_encode(b)},
        {"mcp::client_detail::base64_encode", ae::mcp::client_detail::base64_encode(as_chars(b))},
        {"relay_base64::encode(ptr,n)", ae::native_jail::relay_base64::encode(b.data(), b.size())},
        {"relay_base64::encode(vector)", ae::native_jail::relay_base64::encode(b)},
        // old
        {"old a2a", ae::oracle_a2a::detail::base64_encode(b)},
        {"old mcp", ae::oracle_mcp::client_detail::base64_encode(as_chars(b))},
        {"old relay(ptr,n)", ae::oracle_relay_base64::encode(b.data(), b.size())},
        {"old relay(vector)", ae::oracle_relay_base64::encode(b)},
        {"old message_codec", ae::oracle_state::message_codec_detail::base64_encode(b)},
        {"old chat_recording", ae::oracle_recording::recording_detail::base64_encode(b)},
    };
    for (auto const& [who, text] : got) {
        ++g_encodings;
        check(text == expect, "encode " + label + ": " + who + " gave '" + printable(text) + "', core gave '" +
                                  printable(expect) + "'");
    }
    // The Message JSON codec end to end: a media item's `bytes_base64`, both profiles, against the ADR-202 oracles.
    ae::ContentItem item;
    ae::Media m;
    m.payload = b;
    m.media_type = "application/octet-stream";
    item.value = m;
    g_encodings += 2;
    check(ae::json::dump(ae::content_item_to_json(item)) ==
              ae::json::dump(ae::oracle_recording::content_item_to_json(item)),
          "encode media item (recording) " + label);
    check(ae::json::dump(ae::rt::content_item_to_json(item)) == ae::json::dump(ae::oracle_state::content_item_to_json(item)),
          "encode media item (state) " + label);
}

// ---- 2. decode -------------------------------------------------------------------------------------

std::string describe(std::optional<Bytes> const& r) {
    if (!r) return "nullopt";
    return "ok(" + std::to_string(r->size()) + " bytes)";
}

template <class R>
std::string describe_result(R const& r) {
    if (r) return "ok(" + std::to_string(r->size()) + " bytes)";
    return "err{" + r.error().code + ", " + r.error().message + "}";
}

// Old `result<Bytes>` vs new `optional<Bytes>`: equal value on success, nullopt exactly when the old one erred.
template <class R>
bool same_as_optional(R const& old, std::optional<Bytes> const& now) {
    if (old.has_value() != now.has_value()) return false;
    return !old.has_value() || *old == *now;
}

template <class R>
bool same_result(R const& a, R const& b) {
    if (a.has_value() != b.has_value()) return false;
    if (a.has_value()) return *a == *b;
    return a.error().klass == b.error().klass && a.error().code == b.error().code &&
           a.error().message == b.error().message;
}

using ItemResult = ae::result<ae::ContentItem>;

bool same_item(ItemResult const& a, ItemResult const& b) {
    if (a.has_value() != b.has_value()) return false;
    if (a.has_value()) return *a == *b;
    return a.error().klass == b.error().klass && a.error().code == b.error().code &&
           a.error().message == b.error().message;
}

// Why an input the old relay decoder accepted is not canonical base64. Written from the rule's statement
// (ADR-205 §3), independently of `decode_strict`'s code; kCanonical means none of the reasons applies.
enum class NonCanonical { kCanonical, kContentAfterPadding, kBadLength, kMisplacedPadding, kNonZeroTrailingBits };

NonCanonical classify(std::string_view s) {
    std::size_t const first_pad = s.find('=');
    if (first_pad != std::string_view::npos && s.find_first_not_of('=', first_pad) != std::string_view::npos) {
        return NonCanonical::kContentAfterPadding;
    }
    if (s.size() % 4 != 0) return NonCanonical::kBadLength;
    std::size_t const pad = first_pad == std::string_view::npos ? 0 : s.size() - first_pad;
    if (pad > 2) return NonCanonical::kMisplacedPadding;
    if (pad > 0) {
        int const last = ae::base64::detail::decode_char(s[s.size() - pad - 1]);
        if ((last & (pad == 1 ? 0x03 : 0x0F)) != 0) return NonCanonical::kNonZeroTrailingBits;
    }
    return NonCanonical::kCanonical;
}

long g_relay_same = 0;                // old relay accepted, strict accepted, same bytes
long g_relay_both_reject = 0;         // old relay rejected, strict rejected
long g_relay_newly_rejected[5] = {};  // old relay accepted, strict rejected; indexed by NonCanonical

void diff_decode(std::string const& s, std::string const& label) {
    std::string const what = label + " '" + printable(s) + "'";
    // Lenient.
    auto const lenient = ae::base64::decode_lenient(s);
    auto const old_state = ae::oracle_state::message_codec_detail::base64_decode(s);
    auto const old_rec = ae::oracle_recording::recording_detail::base64_decode(s);
    g_decodings += 3;
    if (!lenient) ++g_decode_errors;
    check(same_as_optional(old_state, lenient),
          "lenient vs old message_codec: " + what + " new=" + describe(lenient) + " old=" + describe_result(old_state));
    check(same_as_optional(old_rec, lenient),
          "lenient vs old chat_recording: " + what + " new=" + describe(lenient) + " old=" + describe_result(old_rec));

    auto const a2a_new = ae::a2a::detail::base64_decode(s);
    auto const a2a_old = ae::oracle_a2a::detail::base64_decode(s);
    ++g_decodings;
    check(same_result(a2a_new, a2a_old),
          "a2a decode: " + what + " new=" + describe_result(a2a_new) + " old=" + describe_result(a2a_old));

    // The Message JSON codec end to end (its private wrapper maps nullopt to the profile's bad_base64).
    ae::json::Value const j = ae::json::Value::make_object({{"kind", ae::json::Value::make_string("media")},
                                                            {"payload_kind", ae::json::Value::make_string("bytes")},
                                                            {"bytes_base64", ae::json::Value::make_string(s)}});
    g_decodings += 2;
    check(same_item(ae::content_item_from_json(j), ae::oracle_recording::content_item_from_json(j)),
          "media item decode (recording): " + what);
    check(same_item(ae::rt::content_item_from_json(j), ae::oracle_state::content_item_from_json(j)),
          "media item decode (state): " + what);

    // The relay: strict now (ADR-205), compared with the old stop-at-padding relay decoder.
    auto const strict = ae::base64::decode_strict(s);
    auto const relay_new = ae::native_jail::relay_base64::decode(s);
    auto const relay_old = ae::oracle_relay_base64::decode(s);
    g_decodings += 2;
    if (!strict) ++g_decode_errors;
    check(relay_new == strict, "relay_base64::decode is decode_strict: " + what);
    if (strict) {
        // Anything strict accepts is canonical: it is exactly the encoding of what it decodes to.
        check(ae::base64::encode(std::span<std::byte const>(*strict)) == s,
              "strict accepted a non-canonical input: " + what);
    }
    if (!relay_old) {
        check(!strict, "strict accepts what the old relay rejected: " + what + " strict=" + describe(strict));
        ++g_relay_both_reject;
        return;
    }
    NonCanonical const why = classify(s);
    if (strict) {
        check(*strict == *relay_old, "strict vs old relay on a canonical input: " + what + " new=" + describe(strict) +
                                         " old=" + describe(relay_old));
        check(why == NonCanonical::kCanonical, "classifier calls a strict-accepted input non-canonical: " + what);
        ++g_relay_same;
    } else {
        // Newly rejected: non-canonical both by the rule's statement and by re-encoding the old result.
        check(why != NonCanonical::kCanonical,
              "strict rejects an input the old relay accepted that the classifier calls canonical: " + what);
        check(ae::base64::encode(std::span<std::byte const>(*relay_old)) != s,
              "strict rejects the canonical encoding of its old decoding: " + what);
        ++g_relay_newly_rejected[static_cast<int>(why)];
    }
}

Bytes random_bytes(std::mt19937_64& rng, std::size_t n) {
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::byte>(rng() & 0xFF);
    return b;
}

std::vector<std::string> malformed_corpus() {
    std::vector<std::string> c = {
        "", "=", "==", "===", "====", "A", "AA", "AAA", "AAAA", "AAAAA",
        "QQ==", "QUI=", "QUJD", "QQ", "QUI", "Q===", "QQ=", "QUI==",
        // padding everywhere
        "=QUJD", "Q=UJD", "QU=JD", "QUJ=D", "QUJD=", "QUJD==QUJD", "QQ==QQ==", "QQ==QUJD", "=Q=U=J=D=",
        "QUJD====", "====QUJD",
        // line breaks and other whitespace
        "QUJD\n", "\nQUJD", "QU\nJD", "QU\r\nJD", "QUJD\r", "\r\n", "\n", "\r", "QU JD", " QUJD", "QUJD ",
        "QU\tJD", "\t", "QQ==\n", "QQ=\n=", "QQ\n==", "QQ\r==", "QQ==\r\n", "\n=\n=", "QU\vJD", "QU\fJD",
        // invalid characters before and after '='
        "!QUJD", "QU!JD", "QUJD!", "QQ==!", "QQ==!!", "QQ=!", "QQ!=", "QQ==\x01", "QQ==\x80", "QQ==\xFF",
        "QQ==QU$D", "QUJD=%", "-_", "QU-_", "QUJD-", "QUJD_", "QQ==-_", "a+b/", "a+b/c=", "@@@@", "QUJ.",
        "QUJ,", "QUJ:", "QUJ;", "QUJ\\", "QUJ\"", "QUJ'", "QUJ[", "QUJ`", "QUJ{", "QUJ~", "QUJ\x7F",
        // non-ASCII (UTF-8, Latin-1, and a NUL)
        "QUJD\xC3\xA9", "\xE2\x82\xAC", "QUJD\xF0\x9F\x98\x80", "\xC0\x80", "QUJD\xE9", std::string("QU\0JD", 5),
        std::string("\0", 1), std::string("QQ==\0", 5),
        // odd lengths and partial groups
        "Q", "QU", "QUJ", "QUJDR", "QUJDRA", "QUJDRA=", "QUJDRE", "QUJDREU", "QUJDREVG", "QUJDREVGR",
        "////", "++++", "/+/+/", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        // high bits in trailing partial groups
        "//", "///", "/w", "/w=", "/w==", "//8", "//8=", "__8=",
        // non-zero leftover bits under otherwise canonical padding (ADR-205), and their canonical neighbours
        "QR==", "QT==", "Q/==", "QUK=", "QUL=", "QU/=", "QUJDRF==", "QUJDREX=", "QQ==", "QUI=",
    };
    // Long inputs: 4,001 characters, and a long valid run with one bad character at the end / after padding.
    c.push_back(std::string(4001, 'A'));
    c.push_back(std::string(4000, 'Q') + "!");
    c.push_back(std::string(4000, 'Q') + "==!");
    c.push_back(std::string(4000, '='));
    c.push_back(std::string(4000, '\n'));
    return c;
}

// A random string over an alphabet weighted toward base64 characters, padding and line breaks.
std::string random_text(std::mt19937_64& rng) {
    static constexpr std::string_view kPool =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        "======\n\n\r\r \t-_!.\x80\xFF";
    std::size_t const n = rng() % 65;
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (rng() % 97 == 0) {
            s += static_cast<char>(rng() & 0xFF);  // any byte, NUL included
        } else {
            s += kPool[rng() % kPool.size()];
        }
    }
    return s;
}

// A valid encoding with 1-3 random edits (insert, replace, delete) of padding, line breaks or junk.
std::string mutated_encoding(std::mt19937_64& rng) {
    std::string s = ae::base64::encode(std::span<std::byte const>(random_bytes(rng, rng() % 40)));
    static constexpr std::string_view kEdits = "=\n\r \t!-_A/+\x80";
    int const edits = 1 + static_cast<int>(rng() % 3);
    for (int e = 0; e < edits; ++e) {
        char const c = kEdits[rng() % kEdits.size()];
        std::size_t const at = s.empty() ? 0 : rng() % (s.size() + 1);
        switch (rng() % 3) {
            case 0: s.insert(s.begin() + static_cast<std::ptrdiff_t>(at), c); break;
            case 1: if (at < s.size()) s[at] = c; break;
            default: if (at < s.size()) s.erase(at, 1); break;
        }
    }
    return s;
}

// ---- 3. self-control -------------------------------------------------------------------------------

void self_control(std::vector<std::string> const& corpus) {
    // The two old decode rules really differ: the harness above would report it if one were swapped for the other.
    int differ = 0;
    for (auto const& s : corpus) {
        auto const lenient_old = ae::oracle_state::message_codec_detail::base64_decode(s);
        auto const stop_old = ae::oracle_relay_base64::decode(s);
        if (!same_as_optional(lenient_old, stop_old)) ++differ;
    }
    std::printf("self-control: the old lenient and old stop-at-padding decoders differ on %d of %zu corpus inputs\n",
                differ, corpus.size());
    check(differ > 0, "self-control: the two old decode rules must differ somewhere in the corpus");
    // And on the specific inputs that define each rule.
    auto differs = [](std::string const& s) {
        return !same_as_optional(ae::oracle_state::message_codec_detail::base64_decode(s),
                                 ae::oracle_relay_base64::decode(s));
    };
    check(differs("QQ==QQ=="), "self-control: data after '=' (lenient decodes it, stop ignores it)");
    check(differs("QQ==!!"), "self-control: junk after '=' (lenient rejects, stop ignores)");
    check(differs("QU\nJD"), "self-control: a line break (lenient skips, stop rejects)");
    check(differs("QU\rJD"), "self-control: a carriage return (lenient skips, stop rejects)");
    check(!differs("QUJD"), "self-control: a plain valid encoding decodes the same under both");
    // The new functions keep the lenient/relay difference.
    check(ae::base64::decode_lenient("QQ==QQ==") != ae::base64::decode_strict("QQ==QQ=="),
          "self-control: the two new decoders differ on data after '='");
    // ADR-205's examples: the old relay rule accepted each but the last (partly), strict rejects all of them, and
    // both agree on a canonical encoding.
    for (std::string_view s : {"QQ==!!garbage", "QQ==QQ==", "QQ", "QUJ", "QR==", "QUK=", "Q===", "QUJD\n"}) {
        bool const old_accepts = ae::oracle_relay_base64::decode(s).has_value();
        check(old_accepts == (s != "QUJD\n"), "self-control: the old relay rule's verdict on '" + printable(s) + "'");
        check(!ae::base64::decode_strict(s), "self-control: strict rejects '" + printable(s) + "'");
    }
    check(ae::base64::decode_strict("QUJD") == ae::oracle_relay_base64::decode("QUJD"),
          "self-control: strict and the old relay rule agree on a canonical encoding");
}

}  // namespace

int main() {
    // 1. encode
    for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        std::mt19937_64 rng(seed);
        for (std::size_t n = 0; n <= 300; ++n) diff_encode(random_bytes(rng, n), "random seed " + std::to_string(seed) + " len " + std::to_string(n));
    }
    for (std::size_t n = 0; n <= 300; ++n) {
        diff_encode(Bytes(n, std::byte{0x00}), "zeros len " + std::to_string(n));
        diff_encode(Bytes(n, std::byte{0xFF}), "0xFF len " + std::to_string(n));
        Bytes counting(n);
        for (std::size_t i = 0; i < n; ++i) counting[i] = static_cast<std::byte>(i & 0xFF);
        diff_encode(counting, "counting len " + std::to_string(n));
    }
    for (int v = 0; v < 256; ++v) {
        for (std::size_t n = 1; n <= 3; ++n) diff_encode(Bytes(n, static_cast<std::byte>(v)), "byte " + std::to_string(v) + " x" + std::to_string(n));
    }

    // 2. decode: malformed corpus, round trips, fuzz.
    auto const corpus = malformed_corpus();
    for (std::size_t i = 0; i < corpus.size(); ++i) diff_decode(corpus[i], "corpus #" + std::to_string(i));
    {
        std::mt19937_64 rng(7);
        for (std::size_t n = 0; n <= 300; ++n) {
            std::string const enc = ae::base64::encode(std::span<std::byte const>(random_bytes(rng, n)));
            diff_decode(enc, "round trip len " + std::to_string(n));
            diff_decode(enc + "\n", "round trip + LF len " + std::to_string(n));
        }
    }
    long fuzz_cases = 0;
    for (std::uint64_t seed : {11ULL, 12ULL, 13ULL}) {
        std::mt19937_64 rng(seed);
        for (int i = 0; i < 2000; ++i) {
            diff_decode(random_text(rng), "fuzz text seed " + std::to_string(seed) + " #" + std::to_string(i));
            diff_decode(mutated_encoding(rng), "fuzz mutation seed " + std::to_string(seed) + " #" + std::to_string(i));
            fuzz_cases += 2;
        }
    }

    // 3. self-control
    self_control(corpus);

    std::printf("encodings compared: %ld; decodings compared: %ld (%ld core-decoder rejections); corpus %zu, fuzz %ld; "
                "checks %ld\n",
                g_encodings, g_decodings, g_decode_errors, corpus.size(), fuzz_cases, g_checks);
    std::printf("relay (ADR-205): same bytes %ld; both reject %ld; newly rejected: content after '=' %ld, bad length "
                "%ld, misplaced '=' %ld, non-zero leftover bits %ld, canonical (must be 0) %ld\n",
                g_relay_same, g_relay_both_reject,
                g_relay_newly_rejected[static_cast<int>(NonCanonical::kContentAfterPadding)],
                g_relay_newly_rejected[static_cast<int>(NonCanonical::kBadLength)],
                g_relay_newly_rejected[static_cast<int>(NonCanonical::kMisplacedPadding)],
                g_relay_newly_rejected[static_cast<int>(NonCanonical::kNonZeroTrailingBits)],
                g_relay_newly_rejected[static_cast<int>(NonCanonical::kCanonical)]);
    // Every class must actually occur, or the comparison above proves less than it claims.
    for (auto k : {NonCanonical::kContentAfterPadding, NonCanonical::kBadLength, NonCanonical::kMisplacedPadding,
                   NonCanonical::kNonZeroTrailingBits}) {
        check(g_relay_newly_rejected[static_cast<int>(k)] > 0,
              "relay: newly rejected class " + std::to_string(static_cast<int>(k)) + " never occurred");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "test_base64_equivalence: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_base64_equivalence: all checks passed\n");
    return 0;
}
