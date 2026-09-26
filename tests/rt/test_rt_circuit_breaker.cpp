// Proof for ADR-037 Phase 1: agentengine::rt::CircuitBreaker, reproducing quark::CircuitBreaker's
// Closed -> Open -> Half-Open state machine (include/agentengine/rt/circuit_breaker.hpp) as new,
// self-contained AgentEngine code -- zero quark:: dependency anywhere in this file, that's the whole
// point of this type existing. Covers: starts Closed and admits; N-1 consecutive failures don't
// trip; the Nth consecutive failure trips (Closed -> Open); a shed while Open touches no attempt
// counter; after the cooldown elapses, exactly one half-open probe is admitted and a second on_send
// before that probe's own on_result lands still sheds; a successful probe closes the breaker and
// admits normally afterward; a FAILED probe reopens with a fresh cooldown clock (not immediately
// re-probable).

#include <cstdint>
#include <cstdio>

#include "agentengine/rt/circuit_breaker.hpp"

using agentengine::rt::Admit;
using agentengine::rt::CircuitBreaker;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    // B1: a freshly constructed breaker starts Closed and admits.
    {
        CircuitBreaker b(3, 1'000);
        check(b.state() == CircuitBreaker::State::Closed, "B1: a fresh breaker starts Closed");
        check(b.on_send(0) == Admit::Accept, "B1: on_send() admits while Closed");
        check(b.consecutive_failures() == 0, "B1: no failures observed yet");
    }

    // B2: N-1 consecutive failures do not trip a threshold-N breaker.
    {
        CircuitBreaker b(3, 1'000);
        std::int64_t now = 0;
        check(b.on_send(now) == Admit::Accept, "B2: attempt 1 is admitted");
        b.on_result(false, now);
        check(b.state() == CircuitBreaker::State::Closed, "B2: 1 failure (threshold 3) stays Closed");
        now += 1;
        check(b.on_send(now) == Admit::Accept, "B2: attempt 2 is admitted");
        b.on_result(false, now);
        check(b.state() == CircuitBreaker::State::Closed, "B2: 2 consecutive failures (threshold 3) still Closed");
        check(b.consecutive_failures() == 2, "B2: consecutive_failures() reflects the 2 real failures");
    }

    // B3: the Nth consecutive failure trips Closed -> Open.
    {
        CircuitBreaker b(3, 1'000);
        std::int64_t now = 0;
        for (int i = 0; i < 2; ++i) {
            check(b.on_send(now) == Admit::Accept, "B3: pre-trip attempts are admitted");
            b.on_result(false, now);
            ++now;
        }
        check(b.state() == CircuitBreaker::State::Closed, "B3: still Closed after 2 of 3 failures");
        check(b.on_send(now) == Admit::Accept, "B3: the 3rd attempt is still admitted (breaker not yet Open)");
        b.on_result(false, now);
        check(b.state() == CircuitBreaker::State::Open, "B3: the 3rd CONSECUTIVE failure trips Closed -> Open");
    }

    // B4: while Open (before the cooldown elapses), on_send() sheds repeatedly without incrementing
    // any attempt counter (consecutive_failures() must not move -- on_result() is never fed for a
    // shed attempt, and on_send() itself must not touch it either).
    {
        CircuitBreaker b(2, 10'000);
        std::int64_t now = 0;
        check(b.on_send(now) == Admit::Accept, "B4: 1st attempt (of 2 needed to trip) is admitted");
        b.on_result(false, now);
        ++now;
        check(b.on_send(now) == Admit::Accept, "B4: 2nd attempt is admitted");
        b.on_result(false, now);  // trips here (2nd consecutive failure), opened_at_ns_ == now
        check(b.state() == CircuitBreaker::State::Open, "B4: breaker is Open after 2 consecutive failures");
        std::uint32_t const fails_at_trip = b.consecutive_failures();

        // Several sheds, well before the 10'000ns cooldown elapses.
        for (int i = 0; i < 5; ++i) {
            ++now;
            check(b.on_send(now) == Admit::Shed, "B4: on_send() sheds while Open and cooling down");
        }
        check(b.state() == CircuitBreaker::State::Open, "B4: still Open -- a shed never transitions state");
        check(b.consecutive_failures() == fails_at_trip,
              "B4: shedding does not touch the consecutive-failure counter (no attempt happened)");
    }

    // B5: after the cooldown elapses, on_send() admits exactly ONE half-open probe; a second
    // on_send() call before that probe's own on_result() is fed back still sheds (only one probe in
    // flight at a time).
    {
        CircuitBreaker b(1, 1'000);  // threshold 1 -- trips on the very first failure
        std::int64_t now = 0;
        check(b.on_send(now) == Admit::Accept, "B5: the sole tripping attempt is admitted");
        b.on_result(false, now);  // trips; opened_at_ns_ == 0
        check(b.state() == CircuitBreaker::State::Open, "B5: breaker is Open after the single tripping failure");

        check(b.on_send(500) == Admit::Shed, "B5: still Shed before the cooldown (500 < 1000) elapses");

        Admit const probe = b.on_send(1'000);  // exactly at the cooldown boundary
        check(probe == Admit::Accept, "B5: the cooldown having elapsed admits exactly one half-open probe");
        check(b.state() == CircuitBreaker::State::HalfOpen, "B5: admitting the probe transitions to HalfOpen");

        check(b.on_send(1'001) == Admit::Shed,
              "B5: a second on_send() before the probe's own on_result() lands still sheds -- only "
              "one probe in flight at a time");
    }

    // B6: a SUCCESSFUL half-open probe closes the breaker, and a subsequent on_send() is admitted
    // normally (no lingering Shed/HalfOpen state).
    {
        CircuitBreaker b(1, 1'000);
        check(b.on_send(0) == Admit::Accept, "B6: the sole tripping attempt is admitted");
        b.on_result(false, 0);  // trips
        check(b.on_send(1'000) == Admit::Accept, "B6: the half-open probe is admitted");
        b.on_result(true, 1'000);  // the probe succeeds
        check(b.state() == CircuitBreaker::State::Closed, "B6: a successful probe closes the breaker");
        check(b.consecutive_failures() == 0, "B6: closing on a successful probe resets the failure counter");
        check(b.on_send(1'001) == Admit::Accept, "B6: a subsequent on_send() is admitted normally again");
    }

    // B7: a FAILED half-open probe reopens the breaker with a FRESH cooldown clock -- the breaker is
    // NOT immediately re-probable; it must wait out a whole new `open_ns` window from the probe's
    // failure time, not the original trip time.
    {
        CircuitBreaker b(1, 1'000);
        check(b.on_send(0) == Admit::Accept, "B7: the sole tripping attempt is admitted");
        b.on_result(false, 0);  // trips at t=0
        check(b.on_send(1'000) == Admit::Accept, "B7: the half-open probe is admitted at t=1000");
        b.on_result(false, 1'000);  // the probe FAILS -> reopen, cooldown restarts from t=1000
        check(b.state() == CircuitBreaker::State::Open, "B7: a failed probe reopens the breaker");

        // Using the ORIGINAL trip time's cooldown window (t=0 + 1000 = 1000) would wrongly suggest
        // t=1000 is already probable again -- prove that's NOT the case: the fresh window runs from
        // the probe's failure time (t=1000), so t=1000 itself must still shed.
        check(b.on_send(1'000) == Admit::Shed,
              "B7: immediately after the failed probe, on_send() still sheds -- not instantly re-probable");
        check(b.on_send(1'500) == Admit::Shed,
              "B7: still shedding mid-way through the FRESH cooldown (1500 < 1000+1000=2000)");
        check(b.on_send(2'000) == Admit::Accept,
              "B7: the fresh cooldown (restarted at the probe's failure time, t=1000) elapses at "
              "t=2000, admitting a new probe there");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_circuit_breaker: ALL PASS\n");
    return 0;
}
