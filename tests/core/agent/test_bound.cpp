// Proves decisions/ADR-244-required-choices-step-1b.md §2.1: `Bound<T>` and the two error shapes every
// required choice uses (G-C2's `choice.zero_bound`, G-C3's `choice.missing.<resource>`).
//
//   B1 (positive control) -- at_most(n) reports n, unbounded() reports none; both describe themselves.
//   B2 -- covers(): unbounded covers everything, a value covers only values no larger than itself, and a
//         value never covers unbounded (a child cannot widen a parent's budget).
//   B3 -- tighter() picks the narrower bound in either argument order, unbounded included.
//   B4 -- check_bound() refuses at_most(0) with choice.zero_bound and accepts at_most(1) and unbounded().
//   B5 -- choice_missing() names the resource in its code and lists every option it was given.
//
// That an omitted Bound does not compile is tests/compile_fail/bound_*.cpp, not this file.

#include "agentengine/core/bound.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace ae = agentengine;

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

using B64 = ae::Bound<std::uint64_t>;

}  // namespace

int main() {
    // B1
    constexpr B64 five = B64::at_most(5);
    constexpr B64 none = B64::unbounded();
    static_assert(five.limit() == std::optional<std::uint64_t>{5});
    check(five.limit() == std::optional<std::uint64_t>{5} && !five.is_unbounded(), "B1: at_most(5) reports 5");
    check(!none.limit().has_value() && none.is_unbounded(), "B1: unbounded() reports no limit");
    check(five.describe() == "5" && none.describe() == "unbounded", "B1: describe() spells both");

    // B2
    check(none.covers(five) && none.covers(none), "B2: unbounded covers a value and unbounded");
    check(five.covers(B64::at_most(5)) && five.covers(B64::at_most(1)), "B2: 5 covers 5 and 1");
    check(!five.covers(B64::at_most(6)), "B2: 5 does not cover 6");
    check(!five.covers(none), "B2: a value never covers unbounded");

    // B3
    check(B64::tighter(five, none) == five && B64::tighter(none, five) == five, "B3: unbounded loses either way");
    check(B64::tighter(five, B64::at_most(3)) == B64::at_most(3) &&
              B64::tighter(B64::at_most(3), five) == B64::at_most(3),
          "B3: the smaller value wins either way");
    check(B64::tighter(none, none) == none, "B3: two unbounded stay unbounded");

    // B4
    auto const zero = ae::check_bound("token_budget", B64::at_most(0));
    check(!zero.has_value() && zero.error().code == "choice.zero_bound" &&
              zero.error().klass == ae::failure_class::contract,
          "B4: at_most(0) is refused with choice.zero_bound (contract)");
    check(!zero.has_value() && zero.error().message.find("token_budget") != std::string::npos &&
              zero.error().message.find("unbounded()") != std::string::npos,
          "B4: the message names the choice and points at unbounded()");
    check(ae::check_bound("token_budget", B64::at_most(1)).has_value(), "B4: at_most(1) is accepted");
    check(ae::check_bound("token_budget", none).has_value(), "B4: unbounded() is accepted");
    check(!ae::check_bound("max_turns", ae::Bound<std::uint32_t>::at_most(0)).has_value(),
          "B4: the 32-bit form refuses 0 too");

    // B5
    static constexpr std::array<std::string_view, 2> kOptions{"per_request", "single_trusted_caller"};
    ae::error const missing = ae::choice_missing("authority", kOptions);
    check(missing.code == "choice.missing.authority" && missing.klass == ae::failure_class::contract,
          "B5: code is choice.missing.<resource>, class contract");
    check(missing.message.find("per_request") != std::string::npos &&
              missing.message.find("single_trusted_caller") != std::string::npos,
          "B5: the message lists every option");

    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
