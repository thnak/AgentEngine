// Proof that a declarative agent's `limits:` block cannot silently compile into no limit at all.
//
// WHAT WAS WRONG. `compile_agent_document()` read both budgets with a bare cast, guarded only by
// `is_number()`:
//
//     meta.max_turns    = static_cast<std::uint32_t>(mt->as_number());
//     meta.token_budget = static_cast<std::uint64_t>(tb->as_number());
//
// Every JSON number is a double, and `static_cast<std::uint32_t>(-1.0)` is undefined ([conv.fpint]);
// on these compilers it lands on 4294967295. So `max_turns: -1` -- which reads like "no limit" to
// anyone writing YAML, and is the natural thing to try -- compiled to the LARGEST representable
// limit, which `AgentSession::run_rounds()`'s `turn_index < *max_turns_` bound then treats as
// unbounded in practice. A declared budget became no budget. `token_budget: -1` did the same at 64
// bits. That is I8 (budgets are enforced) failing open through the declarative surface.
//
// It broke I6 (declarative and native surfaces are equivalent) too: the native form takes
// `std::optional<std::uint64_t>`, in which a negative limit cannot be spelled at all, so the YAML
// accepted something its supposed C++ equivalent rejects at compile time.
//
//   Y1 (positive control) -- a valid limits block still compiles and yields exactly the declared
//         numbers. Every check below is a rejection, so acceptance is established first.
//   Y2 (the guard) -- `max_turns: -1` is REFUSED. Unfixed it compiled, and to 4294967295, which Y2
//         asserts explicitly rather than merely checking for an error, so the test records what the
//         old behaviour actually was.
//   Y3 -- `token_budget: -1` is refused, same reason at 64 bits.
//   Y4 -- a value past the 32-bit ceiling is refused rather than cast as undefined behaviour.
//   Y5 -- a fractional value is refused rather than silently truncated: `max_turns: 2.7` is not a
//         number of turns, and quietly deciding it meant 2 is a guess.
//   Y6 -- a present-but-non-numeric value is refused rather than silently left at the default. A
//         `limits:` block someone wrote that quietly does nothing is the same failure, quieter.
//   Y7 -- an ABSENT limits block still yields 002 §3's documented defaults. This is the behaviour
//         the fix had to preserve, not the behaviour it removed.
//   Y8 -- `max_turns: 0` is ACCEPTED. Zero is a coherent "do nothing" budget and must not be
//         confused with "unset"; refusing it would make the guard overreach.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/agent_yaml_compiler.hpp"
#include "agentengine/core/yaml_value.hpp"

#include <cstdio>
#include <string>

namespace ae = agentengine;
namespace yaml = agentengine::yaml;

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

// The smallest document `compile_agent_document()` accepts, with a caller-chosen `limits:` body.
[[nodiscard]] std::string doc_with_limits(std::string const& limits_body) {
    return std::string(
               "apiVersion: agentengine/v1\n"
               "kind: Agent\n"
               "metadata:\n"
               "  id: limits_probe\n"
               "spec:\n"
               "  instructions: probe\n") +
           limits_body;
}

[[nodiscard]] ae::result<ae::AgentMetadata> compile(std::string const& text) {
    auto parsed = yaml::parse(text);
    if (!parsed.has_value()) {
        return std::unexpected(ae::error{ae::failure_class::contract, "the fixture itself did not parse",
                                         "test.fixture_unparseable"});
    }
    return ae::compile_agent_document(*parsed);
}

}  // namespace

int main() {
    // ---- Y1: the positive control.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: 12\n    token_budget: 200000\n"));
        check(meta.has_value(), "Y1 (positive control): a valid limits block compiles");
        check(meta.has_value() && meta->max_turns == 12,
              "Y1 (positive control): max_turns is exactly what was declared");
        check(meta.has_value() && meta->token_budget.has_value() && *meta->token_budget == 200000,
              "Y1 (positive control): token_budget is exactly what was declared, so the rejections "
              "below are measured against a path that demonstrably works");
    }

    // ---- Y2: the guard.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: -1\n"));
        check(!meta.has_value(),
              "Y2: max_turns: -1 is REFUSED -- unfixed it compiled to 4294967295, which the round "
              "loop treats as unbounded, so a declared budget became no budget");
        check(!meta.has_value() && meta.error().code == "agent_yaml_compiler.invalid_limit",
              "Y2: ... under a code a caller can match on");
        // Stated explicitly so the record shows what the old behaviour WAS, not merely that it has
        // changed: 4294967295 is what -1.0 lands on when cast to uint32 on these compilers.
        check(!meta.has_value() || meta->max_turns != 4294967295u,
              "Y2: ... and specifically does not yield 4294967295");
    }

    // ---- Y3: the same at 64 bits.
    {
        auto meta = compile(doc_with_limits("  limits:\n    token_budget: -1\n"));
        check(!meta.has_value(), "Y3: token_budget: -1 is refused rather than becoming UINT64_MAX");
    }

    // ---- Y4: past the 32-bit ceiling.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: 1e20\n"));
        check(!meta.has_value(),
              "Y4: a max_turns past the 32-bit ceiling is refused, not cast as undefined behaviour");
    }

    // ---- Y5: fractional.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: 2.7\n"));
        check(!meta.has_value(),
              "Y5: a fractional max_turns is refused rather than silently truncated to 2");
    }

    // ---- Y6: present but not a number.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: twelve\n"));
        check(!meta.has_value(),
              "Y6: a non-numeric max_turns is refused rather than silently leaving the default -- a "
              "limits block that quietly does nothing is the same failure in a quieter register");
    }

    // ---- Y7: the behaviour the fix had to KEEP.
    {
        auto meta = compile(doc_with_limits(""));
        check(meta.has_value(), "Y7: a document with no limits block still compiles");
        check(meta.has_value() && meta->max_turns == 16,
              "Y7: ... and keeps 002 §3's documented max_turns default of 16");
        check(meta.has_value() && !meta->token_budget.has_value(),
              "Y7: ... and leaves token_budget unset, which is the documented 'unbounded' state -- "
              "the guard rejects bad values, it does not make the block mandatory");
    }

    // ---- Y8: zero is a real budget, not a missing one.
    {
        auto meta = compile(doc_with_limits("  limits:\n    max_turns: 0\n"));
        check(meta.has_value() && meta->max_turns == 0,
              "Y8: max_turns: 0 is ACCEPTED and means zero turns -- refusing it would make the "
              "guard overreach into a coherent 'deny everything' setting");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
