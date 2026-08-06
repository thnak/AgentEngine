// Proof for Milestone 3 Phase F3
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `src/backends/native_jail/output_discipline.hpp`'s `cap_output()`, the shared truncation-with-
// marker mechanism 010 §3 items 4/5 apply uniformly to `ExecOutcome::stdout_text`/`stderr_text`/
// `result_repr`, regardless of which Runner produced them. Pure C++, no Python/sandbox dependency --
// this proof is about the byte-accounting and marker logic in isolation; result_repr-specific
// capture and Python/Shell integration are covered in the Python-gated Runner smoke tests.

#include <cstdio>
#include <string>

#include "backends/native_jail/output_discipline.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stdout, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    using agentengine::native_jail::cap_output;
    using agentengine::native_jail::kDefaultOutputCapBytes;

    // ---- F3-D1: text already under the cap passes through unchanged, no marker, no truncation flag.
    {
        auto out = cap_output("hello", 100);
        check(!out.truncated, "F3-D1: text under the cap is not flagged as truncated");
        check(out.text == "hello", "F3-D1: text under the cap passes through byte-for-byte unchanged");
        check(out.original_bytes == 5, "F3-D1: original_bytes reflects the real input size");
    }

    // ---- F3-D2: text exactly AT the cap is also a no-op (boundary, not off-by-one).
    {
        std::string exact(64, 'x');
        auto out = cap_output(exact, 64);
        check(!out.truncated, "F3-D2: text exactly at the cap is not truncated (boundary correctness)");
        check(out.text == exact, "F3-D2: text exactly at the cap is returned unchanged");
    }

    // ---- F3-D3: text over the cap is truncated, flagged, and carries an explicit marker naming both
    // how much was kept and how much the original was -- never a silently shortened string.
    {
        std::string big(1000, 'a');
        auto out = cap_output(big, 100);
        check(out.truncated, "F3-D3: text over the cap is flagged as truncated");
        check(out.original_bytes == 1000, "F3-D3: original_bytes reflects the real, untruncated size");
        check(out.text.substr(0, 100) == big.substr(0, 100),
              "F3-D3: the kept prefix is byte-for-byte identical to the original's start");
        check(out.text.find("truncated") != std::string::npos,
              "F3-D3: the result carries an explicit truncation marker, not a silently shortened string");
        check(out.text.find("100") != std::string::npos && out.text.find("1000") != std::string::npos,
              "F3-D3: the marker names both how much was kept (100) and the original size (1000)");
    }

    // ---- F3-D4: a cut that would land mid-multi-byte-UTF-8-codepoint backs off to a valid boundary --
    // truncation never produces a string with a chopped-off partial character at the cut point.
    {
        // U+00E9 ('e' with acute accent) is a 2-byte UTF-8 sequence (0xC3 0xA9). Five copies is 10
        // bytes; a cap of 5 lands exactly in the middle of the 3rd codepoint's 2-byte sequence.
        std::string const eacute = "\xC3\xA9";
        std::string five_eacute;
        for (int i = 0; i < 5; ++i) five_eacute += eacute;
        auto out = cap_output(five_eacute, 5);
        check(out.truncated, "F3-D4: setup -- the 10-byte string exceeds the 5-byte cap");
        std::string const kept_prefix = out.text.substr(0, out.text.find('\n'));
        check(kept_prefix.size() == 4,
              "F3-D4: a cap landing mid-codepoint backs off to the last WHOLE codepoint boundary "
              "(4 bytes = 2 complete U+00E9 sequences), never splitting one down the middle");
        check(kept_prefix == eacute + eacute,
              "F3-D4: the backed-off prefix is exactly two complete, valid UTF-8 codepoints");
    }

    // ---- F3-D5: empty input is a trivial no-op, not a crash or an off-by-one underflow.
    {
        auto out = cap_output("", 100);
        check(!out.truncated && out.text.empty() && out.original_bytes == 0,
              "F3-D5: empty input caps to empty output, untruncated");
    }

    // ---- F3-D6: the default cap is a real, positive, named constant -- not zero (which would make
    // every non-empty output look truncated) and not unbounded (which would defeat the point).
    {
        check(kDefaultOutputCapBytes > 0, "F3-D6: kDefaultOutputCapBytes is a real, positive default");
        auto out = cap_output(std::string(kDefaultOutputCapBytes + 1, 'z'), kDefaultOutputCapBytes);
        check(out.truncated, "F3-D6: content one byte over the default cap is truncated under it");
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_output_discipline: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_output_discipline: %d check(s) failed\n", g_failures);
    return 1;
}
