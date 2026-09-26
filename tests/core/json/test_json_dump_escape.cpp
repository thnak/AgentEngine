// Proof that `json::detail::dump_escaped_string()` (core/json_value.hpp) -- the string writer behind every
// `json::dump`, which serializes message history, recordings and checkpoints -- produces exactly the
// bytes it did before it started appending unescaped runs in one call instead of one byte at a time.
//
//   E1 (positive control) -- the removed per-byte loop, kept below verbatim as `old_escape`, really does
//         escape what the comparisons rely on: '"', '\\', every byte below 0x20 (named escapes for
//         \b \f \n \r \t, \u00XX for the rest), and nothing else. Without this, agreement between two
//         broken escapers would prove nothing.
//   E2 -- all 256 single-byte strings: old and new agree.
//   E3 (the guard) -- 200000 random strings (lengths 0..80, bytes drawn so escapable bytes appear at
//         every position, including first and last, adjacent to each other, and in long runs of
//         plain bytes): old and new agree on every one, appended onto a non-empty buffer so a wrong
//         run offset into `out` would show.
//   E4 -- round trip: every E3 string, wrapped in a json::Value and dumped, parses back to itself.
//   E5 -- timing on a 4 MiB mostly-plain string, printed not asserted.
//
// MEMORY CAP. The test peaks at ~23 MiB, but a BROKEN escaper can grow quadratically on E5's 4 MiB input
// -- a planted-bug run that stopped advancing the run start did exactly that, reached many GiB, and nearly
// took the developer's machine down. So main() first caps its own memory (tests/support/memory_cap.hpp,
// 512 MiB; 2 GiB under a sanitizer), and M0 proves the cap is real: an allocation past it must throw.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/json_value.hpp"
#include "../../support/crt_fail_fast.hpp"
#include "../../support/memory_cap.hpp"

#include <chrono>
#include <cstdio>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace json = agentengine::json;

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

// The removed writer, verbatim apart from its name.
void old_escape(std::string const& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

[[nodiscard]] std::string printable(std::string const& s) {
    std::string r;
    for (unsigned char c : s) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), (c >= 0x20 && c < 0x7f) ? "%c" : "<%02x>", c);
        r += buf;
    }
    return r;
}

void run_escape_checks();  // E1-E5, below main()

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    constexpr std::size_t kCap = std::size_t{512} << 20;
    constexpr std::size_t kSanitizerCap = std::size_t{2048} << 20;
    bool const capped = agentengine::test_support::cap_process_memory(kCap, kSanitizerCap);

    // ---- M0: the cap is real. Skipped, and said so, where it is not in force, AND under any sanitizer
    //          even where it is (the Windows job limit still applies there): a sanitizer allocator
    //          aborts the process on an over-cap allocation instead of throwing -- the cap still
    //          contains a runaway, it just cannot be probed from inside.
    if (capped && !agentengine::test_support::running_under_sanitizer()) {
        std::size_t const over = kCap * 2;
        bool refused = false;
        try {
            std::vector<char> too_big(over, 'x');
            refused = too_big.empty();  // unreachable if the cap holds
        } catch (std::bad_alloc const&) {
            refused = true;
        }
        check(refused, "M0: the memory cap is in force -- an allocation of " + std::to_string(over >> 20) +
                           " MiB is refused");
    } else {
        std::printf("[info] M0 skipped: %s\n", capped ? "sanitizer build (its allocator aborts instead of throwing)"
                                                      : "no memory cap in force on this build");
    }

    // A broken escaper that grows without bound hits the cap as std::bad_alloc: reported as a failure
    // here, rather than as an abort with the checks that already ran still unprinted.
    try {
        run_escape_checks();
    } catch (std::bad_alloc const&) {
        check(false, "the escape checks exceeded the memory cap (std::bad_alloc) -- a correct escaper "
                     "needs ~23 MiB here, so something is growing without bound");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}

namespace {

void run_escape_checks() {
    // ---- E1: positive control on the oracle.
    {
        std::string out;
        old_escape(std::string("a\"b\\c\b\f\n\r\t\x01\x1f\x7f\xc3\xa9", 15), out);
        std::string const expected = "\"a\\\"b\\\\c\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xc3\xa9\"";
        check(out == expected, "E1 (positive control): the old writer escapes exactly '\"', '\\\\' and bytes below 0x20, "
                               "got " + printable(out));
    }

    // ---- E2: every single byte.
    {
        int mismatches = 0;
        for (int b = 0; b < 256; ++b) {
            std::string const s(1, static_cast<char>(b));
            std::string o;
            std::string n;
            old_escape(s, o);
            json::detail::dump_escaped_string(s, n);
            if (o != n) ++mismatches;
        }
        check(mismatches == 0, "E2: all 256 single-byte strings escape identically (" + std::to_string(mismatches) +
                                   " mismatches)");
    }

    // ---- E3 + E4: random strings.
    {
        std::mt19937 rng(20260914);  // fixed: a failure must be reproducible
        std::string const specials = std::string("\"\\\b\f\n\r\t\x01\x1f\x00", 10);
        int mismatches = 0;
        int round_trip_failures = 0;
        std::string first;
        int const trials = 200000;
        for (int t = 0; t < trials; ++t) {
            std::size_t const len = std::uniform_int_distribution<std::size_t>(0, 80)(rng);
            int const special_pct = std::uniform_int_distribution<int>(0, 3)(rng) * 15;  // 0, 15, 30, 45 %
            std::string s;
            for (std::size_t i = 0; i < len; ++i) {
                if (std::uniform_int_distribution<int>(0, 99)(rng) < special_pct) {
                    s += specials[std::uniform_int_distribution<std::size_t>(0, specials.size() - 1)(rng)];
                } else {
                    s += static_cast<char>(std::uniform_int_distribution<int>(0x20, 0xff)(rng));
                }
            }
            std::string o = "prefix:";
            std::string n = "prefix:";
            old_escape(s, o);
            json::detail::dump_escaped_string(s, n);
            if (o != n && mismatches++ == 0) first = printable(s) + " -> old " + printable(o) + " new " + printable(n);

            auto parsed = json::parse(json::dump(json::Value::make_string(s)));
            if (!parsed.has_value() || !parsed->is_string() || parsed->as_string() != s) ++round_trip_failures;
        }
        check(mismatches == 0, "E3 (the guard): " + std::to_string(trials) +
                                   " random strings escape byte-identically" +
                                   (mismatches == 0 ? std::string{} : " -- " + std::to_string(mismatches) + ", first " + first));
        check(round_trip_failures == 0,
              "E4: every one of them dumps and parses back to itself (" + std::to_string(round_trip_failures) + " failures)");
    }

    // ---- E5: timing, printed.
    {
        std::string big;
        big.reserve(4u << 20);
        while (big.size() < (4u << 20)) big += "The quick brown fox jumps over the lazy dog. \"quoted\"\n";
        using clock = std::chrono::steady_clock;
        auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::string o;
        std::string n;
        auto t0 = clock::now();
        old_escape(big, o);
        double const old_ms = ms(clock::now() - t0);
        t0 = clock::now();
        json::detail::dump_escaped_string(big, n);
        double const new_ms = ms(clock::now() - t0);
        check(o == n, "E5: the 4 MiB string escapes identically");
        std::printf("[info] 4 MiB mostly-plain string (this build): old %.1f ms, new %.1f ms\n", old_ms, new_ms);
    }
}

}  // namespace
