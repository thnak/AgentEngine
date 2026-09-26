// A-S1 (ADR-001 §5.2) — parser safety against adversarial input. Calls `agentengine::shell::parse`
// DIRECTLY (the fake-free, `bytes -> result<ParsedScript>` surface §3's steelman #1 and this
// ADR's §10 recommendation both lean on) against a hand-written adversarial corpus covering, at
// minimum, what ADR-001's prove-phase task explicitly names:
//   1. `if` nested 10,000 deep with no closing `fi`.
//   2. combined nesting: near-max `if` nesting whose body ALSO contains near-max `${...}` width
//      (this grammar's `${NAME}` cannot literally nest inside another `${...}` — NAME is a plain
//      identifier — so "combined nesting" here is near-max if-DEPTH combined with wide, non-
//      nested `${...}` CONTENT in the same body; see shell_parser.cpp's file header for why word/
//      atom scanning is iterative and contributes zero depth by construction. Both a failing
//      (over-cap) and a succeeding (at-cap) variant are included as a positive/negative pair, so
//      "correctly rejected" isn't indistinguishable from "the wide content was wrongly counted
//      against depth and rejected for the wrong reason.")
//   3. wide-not-deep: many sequential, shallow (non-nested) `if` statements — proves the shared
//      depth counter's RAII decrement actually fires on every exit, not just that it's monotonic.
//   4. max-token-count / minimum-per-token-content — plus the ADR's own named 100k-stage-pipeline
//      probe.
// Every case asserts: (a) parse() returns within a generous wall-clock bound (no hang), (b) it
// never throws past its own boundary, (c) on invalid input it returns a clean `resource`- or
// `contract`-classed error, never a crash. Run under ASan+UBSan (build-asan/) is the actual
// sanitizer evidence recorded in ADR-001 §8; this executable is what that build compiles and runs.
//
// Resource-capped per CLAUDE.md's Machine Safety section: single-threaded, no spawned threads by
// default, arena size is the fixed compile-time kArenaBytes (~12 MiB), and every loop below has a
// fixed, bounded iteration count computed from the module's own published constants — nothing here
// scales unboundedly with a hostile input, by construction, since kMaxSourceBytes/kMaxTokens are
// enforced by the code under test itself.

#include <chrono>
#include <iostream>
#include <string>

#include "backends/native_jail/shell_grammar.hpp"
#include "backends/native_jail/shell_parser.hpp"

using namespace agentengine::shell;
using Clock = std::chrono::steady_clock;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

template <class Fn>
auto timed(Fn&& fn, char const* label, std::chrono::milliseconds bound) {
    auto start  = Clock::now();
    auto result = fn();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    std::cout << "    " << label << ": " << elapsed.count() << " ms\n";
    AE_CHECK(elapsed <= bound,
             std::string(label) + " completes within " + std::to_string(bound.count()) + " ms");
    return result;
}

// 1. `if` nested N deep, never closed.
void probe_if_nested_no_fi(std::size_t depth) {
    std::string src;
    for (std::size_t i = 0; i < depth; ++i) src += "if true then ";
    auto r = timed([&] { return parse(src); }, "if-nested-no-fi", std::chrono::milliseconds(2000));
    AE_CHECK(!r.has_value(), "if-nested-10000-deep-no-fi: parse fails (never crashes/hangs)");
    if (!r.has_value()) {
        std::cout << "    error code: " << r.error().code << "\n";
        AE_CHECK(r.error().code == "shell.nesting_too_deep",
                 "if-nested-10000-deep-no-fi: fails via the shared depth counter, not some other "
                 "error class");
    }
}

// 2a. Combined: if-nesting OVER the cap, each level's body also carrying wide (non-nested)
//     ${...} content — must still fail via the depth counter, not succeed because the width
//     "used up" some unrelated per-kind budget instead.
void probe_combined_over_cap() {
    std::size_t const over_cap_depth = kMaxNestingDepth + 8;
    std::string wide_word;
    for (int i = 0; i < 20; ++i) wide_word += "${v" + std::to_string(i) + "}";
    // Genuine nesting: each level's `then`-body is immediately the next `if`, so depth actually
    // accumulates `over_cap_depth` times before the innermost body (the wide ${...} word) is
    // reached — NOT `over_cap_depth` sibling words tacked onto one echo (which would never
    // recurse at all and wouldn't exercise the depth counter).
    std::string src;
    for (std::size_t i = 0; i < over_cap_depth; ++i) src += "if true then ";
    src += "echo " + wide_word + " ";
    for (std::size_t i = 0; i < over_cap_depth; ++i) src += "fi ";
    auto r = parse(src);
    AE_CHECK(!r.has_value(),
             "combined nesting (over cap, wide ${...} content): parse fails cleanly");
    if (!r.has_value()) {
        AE_CHECK(r.error().code == "shell.nesting_too_deep",
                 "combined nesting (over cap): fails via the shared depth counter specifically");
    }
}

// 2b. Positive control: if-nesting AT the cap (legal), same wide ${...} content — must SUCCEED,
//     proving the wide content isn't incorrectly counted against the shared depth counter.
void probe_combined_at_cap() {
    std::size_t const at_cap_depth = kMaxNestingDepth;
    std::string wide_word;
    for (int i = 0; i < 20; ++i) wide_word += "${v" + std::to_string(i) + "}";
    std::string src;
    for (std::size_t i = 0; i < at_cap_depth; ++i) src += "if true then ";
    src += "echo " + wide_word + " ";
    for (std::size_t i = 0; i < at_cap_depth; ++i) src += "fi ";
    auto r = parse(src);
    AE_CHECK(r.has_value(),
             "combined nesting (AT cap, wide ${...} content): parses successfully — wide, "
             "non-nested ${...} content is not wrongly charged against the depth counter");
}

// 3. Wide-not-deep: many SEQUENTIAL, shallow if-statements. Proves the RAII depth guard's
//    decrement actually fires — a monotonic (never-decremented) counter would fail this identically
//    to how it "passes" probe_if_nested_no_fi, which is exactly the ambiguity finding 12 flagged.
void probe_wide_not_deep(std::size_t count) {
    std::string src;
    for (std::size_t i = 0; i < count; ++i) src += "if true then echo hi fi; ";
    auto r = timed([&] { return parse(src); }, "wide-not-deep", std::chrono::milliseconds(2000));
    AE_CHECK(r.has_value(),
             "wide-not-deep (many sequential shallow ifs): parses successfully — proves the depth "
             "counter's decrement fires on every exit, not just that it's monotonic");
}

// 4a. Max token count, minimum per-token content (finding 13) — exceeding the cap.
void probe_max_tokens_min_content_over() {
    std::string src = "echo ";
    for (std::size_t i = 0; i < kMaxTokens + 1000; ++i) src += "a ";
    auto r = timed([&] { return parse(src); }, "max-token-min-content (over)",
                    std::chrono::milliseconds(2000));
    AE_CHECK(!r.has_value(), "max-token-min-content (over budget): parse fails cleanly");
    if (!r.has_value()) {
        std::cout << "    error code: " << r.error().code << " klass="
                  << static_cast<int>(r.error().klass) << " message=" << r.error().message << "\n";
        AE_CHECK(r.error().code == "shell.too_many_tokens" ||
                     r.error().code == "shell.source_too_large" ||
                     r.error().code == "shell.arena_exhausted",
                 "max-token-min-content (over budget): fails via a resource-classed budget error");
    }
}

// 4b. Positive control: comfortably under the token cap — must succeed.
void probe_max_tokens_min_content_under() {
    std::string src = "echo ";
    std::size_t const n = kMaxTokens / 4;
    for (std::size_t i = 0; i < n; ++i) src += "a ";
    if (src.size() > kMaxSourceBytes) {
        std::cout << "  SKIPPED: under-budget token probe would itself exceed kMaxSourceBytes\n";
        return;
    }
    auto r = timed([&] { return parse(src); }, "max-token-min-content (under)",
                    std::chrono::milliseconds(2000));
    AE_CHECK(r.has_value(),
             "max-token-min-content (comfortably under budget): parses successfully");
}

// 5. The ADR's own named 100k-stage-pipeline probe — must be rejected cleanly and fast.
//
// A real, unanticipated finding from this pass: this probe, run in this project's DEFAULT build
// configuration (no explicit CMAKE_BUILD_TYPE — links the MSVC debug CRT, /MDd, with
// _ITERATOR_DEBUG_LEVEL=2's "checked iterators" active), takes many SECONDS-TO-MINUTES rather than
// the milliseconds the claim expects, purely from MSVC Debug STL's per-container-destruction
// overhead once `PipelineNode::commands` (a `pmr::vector<SimpleCommandNode>`, each element itself
// owning 3 more pmr::vectors) crosses roughly 12,000 live elements and starts reallocating —
// confirmed NOT an algorithmic issue in the parser (which is linear: the SAME source, built with
// `cmake -S . -B build-clang-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
// -DCMAKE_BUILD_TYPE=Release`, parses and rejects the full 100,000-stage input in single-digit
// milliseconds — see ADR-001 §8 for the recorded numbers). Rather than register a test in the
// default CTest suite that can hang for minutes under the debug CRT (CLAUDE.md's Machine Safety
// section rules this out), this function uses a width safely below that debug-mode cliff — still
// a genuinely wide (not deep) pipeline, still measured for time, just not the literal 100,000
// figure. The literal 100k case IS executed and measured — in the Release configuration, reported
// separately, not laundered into this function's default-build result.
void probe_wide_pipeline_debug_safe(std::size_t stages) {
    std::string src = "a";
    src.reserve(stages * 2);
    for (std::size_t i = 1; i < stages; ++i) src += "|a";
    auto r = timed([&] { return parse(src); }, "wide-pipeline (debug-safe width)",
                    std::chrono::milliseconds(5000));
    // At this width (well under kMaxTokens), the pipeline is legal and should parse successfully —
    // this is a positive control for "wide pipelines parse correctly and quickly," complementing
    // probe_max_tokens_min_content_over's already-covered "width exceeding the token cap is
    // rejected cleanly" claim (that probe uses many WORDS in one command rather than many pipeline
    // STAGES, and was NOT affected by the debug-mode cliff described above, so it remains the
    // CTest-wired evidence for the cap-exceeded case).
    AE_CHECK(r.has_value(), "wide pipeline (debug-safe width) parses successfully and quickly");
}

#if defined(AE_RUN_100K_PIPELINE_PROBE)
// The literal ADR-named probe, compiled in only for a Release configuration build (see
// tests/CMakeLists.txt) where the MSVC debug-CRT cliff described above does not apply — this is
// the actual evidence recorded in ADR-001 §8 for the "100k-stage pipeline" sub-claim.
void probe_100k_stage_pipeline_release_evidence() {
    std::string src = "a";
    std::size_t const stages = 100'000;
    src.reserve(stages * 2);
    for (std::size_t i = 1; i < stages; ++i) src += "|a";
    auto r = timed([&] { return parse(src); }, "100k-stage-pipeline (Release build, real figure)",
                    std::chrono::milliseconds(2000));
    AE_CHECK(!r.has_value(),
             "100k-stage pipeline (Release, real 100,000 figure): rejected cleanly, fast");
}
#endif

// 6. Unterminated quote.
void probe_unterminated_quote() {
    auto r = parse("echo 'unterminated");
    AE_CHECK(!r.has_value() && r.error().code == "shell.unterminated_quote",
             "unterminated quote: clean contract error");
}

// 7. Oversized source (over kMaxSourceBytes) — rejected in O(1), before any tokenizing.
void probe_oversized_source() {
    std::string src(kMaxSourceBytes + (2 * 1024 * 1024), 'a'); // >1 MiB over the cap
    auto r = timed([&] { return parse(src); }, "oversized-source", std::chrono::milliseconds(200));
    AE_CHECK(!r.has_value() && r.error().code == "shell.source_too_large",
             "oversized source (well over kMaxSourceBytes): rejected immediately");
}

// Positive control for the whole suite: an ordinary, small, well-formed script parses.
void probe_positive_control() {
    auto r = parse("mkdir /work/x && ls /work | cat");
    AE_CHECK(r.has_value(), "positive control: an ordinary well-formed script parses successfully");
}

} // namespace

int main() {
    probe_positive_control();
    probe_if_nested_no_fi(10'000);
    probe_combined_over_cap();
    probe_combined_at_cap();
    probe_wide_not_deep(5'000);
    probe_max_tokens_min_content_over();
    probe_max_tokens_min_content_under();
    probe_wide_pipeline_debug_safe(8'000);
#if defined(AE_RUN_100K_PIPELINE_PROBE)
    probe_100k_stage_pipeline_release_evidence();
#endif
    probe_unterminated_quote();
    probe_oversized_source();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "test_shell_parser_adversarial: PASS\n";
    return 0;
}
