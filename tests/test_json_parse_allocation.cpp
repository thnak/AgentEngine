// Proof for the `parse_string_raw()` allocation defect: the parser sized EVERY string token to the
// whole remaining document.
//
// WHY IT MATTERED. The line read `out.reserve(text_.size() - pos_)`, commented "safe upper bound:
// remaining input can't be shorter". That is true, and it is not the relevant property. `Value` stores
// the `std::string` the parser hands it, so the over-reservation is not scratch space released at the
// end of the call -- it is RETAINED, once per string token, for the lifetime of the parsed tree. A
// document of N short tokens therefore costs O(N * document_size) live bytes instead of O(document
// size). This parser sits under MCP server responses, tool results and raw model tool-call arguments
// (the header says so itself), so the input size is attacker-influenced.
//
//   A1 -- every escape form still decodes, including an escape as the first byte, as the last byte,
//         two back to back, and `\u`. The fix replaced a byte-at-a-time loop with a run-scanning one,
//         so this is the correctness guard for that rewrite. It passes against the OLD code too, and
//         is recorded here as a regression guard, not as evidence of the bug.
//   A2 -- the four malformed-string error codes are unchanged, for the same reason.
//   A3 (positive control for the instrument) -- the tree walk A4 uses really can observe a large
//         retained capacity. Without this, A4 passing would prove nothing, since a walk that saw no
//         strings would also report no excess.
//   A4 (the guard) -- retained capacity across a 2000-pair document stays within a small multiple of
//         the bytes actually held. Measured on this machine, same binary, fix stashed and restored:
//         86,025,808 bytes retained before, 60,000 after, for 29,780 bytes of content. Three checks
//         fail against the unfixed parser and none against the fixed one, so this is a real guard.
//   A5 -- a single long token allocates its own length rather than the document's. That is the fast
//         path A4 exercises in aggregate, stated as a direct claim.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/json_value.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

using agentengine::json::Value;
using agentengine::json::value_kind;

struct StringAccounting {
    std::size_t bytes = 0;     // what the document actually holds
    std::size_t capacity = 0;  // what the process actually keeps
    std::size_t tokens = 0;
};

// Object KEYS come out of the same `parse_string_raw()` as values, so they are counted too --
// leaving them out would hide half the defect in a key/value document.
void account(Value const& v, StringAccounting& acc) {
    switch (v.kind()) {
        case value_kind::string: {
            auto const& s = v.as_string();
            acc.bytes += s.size();
            acc.capacity += s.capacity();
            ++acc.tokens;
            break;
        }
        case value_kind::array:
            for (auto const& e : v.as_array()) account(e, acc);
            break;
        case value_kind::object:
            for (auto const& [k, e] : v.as_object()) {
                acc.bytes += k.size();
                acc.capacity += k.capacity();
                ++acc.tokens;
                account(e, acc);
            }
            break;
        default:
            break;
    }
}

[[nodiscard]] std::string make_pairs_document(int pairs) {
    std::string doc = "{";
    for (int i = 0; i < pairs; ++i) {
        if (i != 0) doc += ',';
        doc += "\"key" + std::to_string(i) + "\":\"value" + std::to_string(i) + "\"";
    }
    doc += "}";
    return doc;
}

[[nodiscard]] std::string error_code_of(std::string const& doc) {
    auto parsed = agentengine::json::parse(doc);
    if (parsed.has_value()) return "<parsed>";
    return parsed.error().code;
}

}  // namespace

int main() {
    using agentengine::json::parse;

    // ---- A1: the escape decoder, which the run-scanning rewrite reorganized.
    {
        auto v = parse(R"("a\"b\\c\/d\be\ff\ng\rh\ti")");
        check(v.has_value() && v->as_string() == "a\"b\\c/d\be\ff\ng\rh\ti",
              "A1: every simple escape decodes");

        auto lead = parse(R"("\ntail")");
        check(lead.has_value() && lead->as_string() == "\ntail",
              "A1: an escape as the very first byte decodes -- the fast path must not claim this token");

        auto trail = parse(R"("head\n")");
        check(trail.has_value() && trail->as_string() == "head\n",
              "A1: an escape as the very last byte decodes");

        auto pair = parse(R"("a\n\tb")");
        check(pair.has_value() && pair->as_string() == "a\n\tb",
              "A1: two escapes back to back decode, so the run between them may be empty");

        // The two escape sequences are spelled from char(92) so that no source-level unicode
        // normalisation can fold them into the characters they denote -- that would silently
        // turn this into a UTF-8 passthrough check instead of an escape-decoder one.
        std::string u_doc = "\"caf";
        u_doc += static_cast<char>(92);
        u_doc += "u00e9 ";
        u_doc += static_cast<char>(92);
        u_doc += "u0041\"";
        auto uni = parse(u_doc);
        check(uni.has_value() && uni->as_string() == "caf\xc3\xa9 A",
              "A1: a \\u escape decodes to UTF-8, and \\u0041 to its ASCII character");

        auto empty = parse(R"("")");
        check(empty.has_value() && empty->as_string().empty(), "A1: the empty string parses");

        auto key = parse(R"({"a\nb":1})");
        check(key.has_value() && key->as_object().size() == 1 && key->as_object()[0].first == "a\nb",
              "A1: an escaped object KEY decodes -- keys go through the same function");

        auto plain = parse(R"({"plain":"no escapes here"})");
        check(plain.has_value() && plain->find("plain")->as_string() == "no escapes here",
              "A1 (positive control): the escape-free fast path returns the right bytes, so the "
              "checks above are measured against a path that demonstrably works");
    }

    // ---- A2: the malformed-string failures, unchanged.
    check(error_code_of(R"("unterminated)") == "json.unterminated_string",
          "A2: an unterminated string reports json.unterminated_string");
    check(error_code_of("\"abc\\") == "json.unterminated_escape",
          "A2: input ending on a backslash reports json.unterminated_escape");
    check(error_code_of(R"("bad \q escape")") == "json.bad_escape",
          "A2: an unknown escape reports json.bad_escape");
    check(error_code_of(R"("trunc \u00")") == "json.bad_unicode_escape",
          "A2: a truncated \\u escape reports json.bad_unicode_escape");

    // ---- A3: the positive control for A4's instrument.
    {
        std::vector<std::pair<std::string, Value>> obj;
        std::string fat = "short";
        fat.reserve(1u << 20);  // 1 MiB held behind 5 bytes -- exactly the shape A4 rules out
        obj.emplace_back("k", Value::make_string(std::move(fat)));
        Value const v = Value::make_object(std::move(obj));

        StringAccounting acc;
        account(v, acc);
        check(acc.tokens == 2, "A3 (positive control): the walk visits both the key and the value");
        check(acc.capacity >= (1u << 20),
              "A3 (positive control): the walk REPORTS a 1 MiB retained capacity behind a 5-byte "
              "string, so A4's assertion is one that is able to fail");
    }

    // ---- A4: the guard.
    {
        std::string const doc = make_pairs_document(2000);
        auto parsed = parse(doc);
        check(parsed.has_value(), "A4: the 2000-pair document parses");
        if (parsed.has_value()) {
            StringAccounting acc;
            account(*parsed, acc);
            check(acc.tokens == 4000, "A4: all 4000 tokens (2000 keys + 2000 values) are accounted");
            std::printf("       document %zu bytes; %zu tokens holding %zu bytes in %zu bytes of "
                        "capacity (%.1fx)\n",
                        doc.size(), acc.tokens, acc.bytes, acc.capacity,
                        acc.bytes == 0 ? 0.0
                                       : static_cast<double>(acc.capacity) /
                                             static_cast<double>(acc.bytes));
            // Before the fix each of the 4000 tokens reserved the whole remaining document, so this
            // sum measured 86,025,808 bytes against 29,780 bytes of content -- 2888x, versus 2.0x
            // after. The bound below is loose on purpose: this is not a small-string-optimisation
            // measurement, it is the difference between O(tokens * document) and O(document).
            check(acc.capacity <= 8 * acc.bytes,
                  "A4: retained capacity stays within 8x the bytes held -- before the fix this "
                  "measured 2888x, because every token reserved the whole remaining document");
            check(acc.capacity <= doc.size() * 2,
                  "A4: ... and the whole tree keeps less than twice the document, which a "
                  "per-token document-sized reservation cannot do");
        }
    }

    // ---- A5: one long token, stated directly.
    {
        std::string const payload(10000, 'x');
        std::string doc = "{\"big\":\"" + payload + "\",\"pad\":\"";
        doc += std::string(200000, 'y');
        doc += "\"}";
        auto parsed = parse(doc);
        check(parsed.has_value(), "A5: the long-token document parses");
        if (parsed.has_value()) {
            auto const& big = parsed->find("big")->as_string();
            check(big.size() == payload.size(), "A5: the long token round-trips its bytes");
            check(big.capacity() < payload.size() * 2,
                  "A5: a 10 KB token allocates its own length, not the 210 KB of document that "
                  "followed it");
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
