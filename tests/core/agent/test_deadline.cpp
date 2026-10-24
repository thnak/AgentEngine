// Proves decisions/ADR-244-required-choices-step-1b.md §2.2 (time limits): `Deadline`, the stated per-call limit.
//
//   D1 (positive control) -- at(t) expires at t and not before; none() never expires and has no time.
//   D2 -- after(now, at_most(n)) is now + n ms; after(now, unbounded()) is none().
//   D3 -- remaining() never goes negative; none() has no remaining.
//   D4 -- as_timeout_ms() rounds UP to a whole ms, is at least 1, and is unbounded for none() (a child never outlives
//         its parent, and never gets a zero-length timeout from a sliver of time left).
//   D5 -- earlier() picks the sooner and treats none() as "no limit".
//
// That an EffectContext with no deadline does not compile is tests/compile_fail/effect_context_deadline_unstated.cpp.

#include "agentengine/core/deadline.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ae = agentengine;
using B64    = ae::Bound<std::uint64_t>;
using ms     = std::chrono::milliseconds;
using us     = std::chrono::microseconds;

namespace {
int g_failed = 0;
void check(bool cond, std::string const& what) {
    std::printf("[%s] %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failed;
}
}  // namespace

int main() {
    auto const t0 = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);

    auto const at = ae::Deadline::at(t0 + ms(50));
    check(!at.is_none() && at.time() == t0 + ms(50), "D1: at(t) carries t");
    check(!at.expired(t0) && !at.expired(t0 + ms(49)) && at.expired(t0 + ms(50)), "D1: at(t) expires at t, not before");
    auto const none = ae::Deadline::none();
    check(none.is_none() && !none.time().has_value() && !none.expired(t0 + std::chrono::hours(1000)),
          "D1: none() has no time and never expires");

    check(ae::Deadline::after(t0, B64::at_most(250)) == ae::Deadline::at(t0 + ms(250)),
          "D2: after(now, at_most(250)) is now + 250ms");
    check(ae::Deadline::after(t0, B64::unbounded()).is_none(), "D2: after(now, unbounded()) is none()");

    check(at.remaining(t0).value() == ms(50), "D3: remaining is the time left");
    check(at.remaining(t0 + ms(500)).value() == std::chrono::nanoseconds{0}, "D3: remaining is never negative");
    check(!none.remaining(t0).has_value(), "D3: none() has no remaining");

    check(!none.as_timeout_ms(t0).limit().has_value(), "D4: none() states unbounded()");
    check(at.as_timeout_ms(t0).limit() == 50u, "D4: a whole-ms remainder is exact");
    check(at.as_timeout_ms(t0 + ms(49) + us(500)).limit() == 1u, "D4: 500us left rounds UP to 1 ms");
    check(at.as_timeout_ms(t0 + ms(500)).limit() == 1u, "D4: an expired deadline is still a stated, non-zero 1 ms");
    check(ae::check_bound("t", at.as_timeout_ms(t0 + ms(500))).has_value(), "D4: the stated timeout passes check_bound");

    auto const later = ae::Deadline::at(t0 + ms(80));
    check(ae::Deadline::earlier(at, later) == at && ae::Deadline::earlier(later, at) == at,
          "D5: earlier() picks the sooner");
    check(ae::Deadline::earlier(none, at) == at && ae::Deadline::earlier(at, none) == at, "D5: none() is no limit");
    check(ae::Deadline::earlier(none, none).is_none(), "D5: none() with none() is none()");

    if (g_failed == 0) std::printf("test_deadline: ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
