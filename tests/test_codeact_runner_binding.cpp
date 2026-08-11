// Proves `CodeActRunnerBinding<RunnerT>` (codeact_runner_binding.hpp) — the "prove" half of
// ADR-030's design → red-team → prove → judge record. Deterministic, offline, no CPython
// dependency: `RunnerT` is a trivial stand-in type, since this binding's own claim/fail-closed
// logic is independent of what kind of runner it wraps.
//
//   B1 — the first bind() for a given session_id succeeds.
//   B2 — a second bind() with the SAME session_id is idempotent (still succeeds, still bound).
//   B3 — a bind() with a DIFFERENT session_id fails closed, with the specific error code, and does
//        NOT change which session the binding is bound to.
//   B4 — is_bound_to() correctly reflects the bound session; is_bound() is false before any bind().
//   B5 — runner() returns a reference to the exact same object the binding was constructed with.

#include <iostream>
#include <string>

#include "agentengine/core/codeact_runner_binding.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // A trivial stand-in for a real runner -- this binding's logic never calls anything on it, only
    // ever returns a reference back.
    struct FakeRunner {
        int tag = 0;
    };

    // ---- B4 (before any bind): not bound at all -------------------------------------------------
    {
        FakeRunner runner{42};
        ae::CodeActRunnerBinding<FakeRunner> binding(runner);
        AE_CHECK(!binding.is_bound(), "B4: a fresh binding is not bound to anyone");
        AE_CHECK(!binding.is_bound_to("s-a"), "B4: is_bound_to() is false for any session before bind()");
    }

    // ---- B1/B5: first bind succeeds; runner() returns the exact same object ----------------------
    {
        FakeRunner runner{42};
        ae::CodeActRunnerBinding<FakeRunner> binding(runner);
        auto r = binding.bind("s-a");
        AE_CHECK(r.has_value(), "B1: the first bind() for a session_id succeeds");
        AE_CHECK(binding.is_bound(), "B1: the binding is now bound");
        AE_CHECK(binding.is_bound_to("s-a"), "B1: is_bound_to(\"s-a\") is now true");
        AE_CHECK(&binding.runner() == &runner,
                 "B5: runner() returns a reference to the exact object the binding was constructed "
                 "with, not a copy");
    }

    // ---- B2: re-binding the SAME session_id is idempotent -----------------------------------------
    {
        FakeRunner runner{42};
        ae::CodeActRunnerBinding<FakeRunner> binding(runner);
        auto r1 = binding.bind("s-a");
        AE_CHECK(r1.has_value(), "B2 setup: first bind succeeds");
        auto r2 = binding.bind("s-a");
        AE_CHECK(r2.has_value(),
                 "B2: a second bind() with the SAME session_id succeeds again (idempotent -- a "
                 "session reconfiguring itself must not be treated as a foreign claim)");
        AE_CHECK(binding.is_bound_to("s-a"), "B2: still bound to the same session afterward");
    }

    // ---- B3: a DIFFERENT session_id fails closed, doesn't change the binding ----------------------
    {
        FakeRunner runner{42};
        ae::CodeActRunnerBinding<FakeRunner> binding(runner);
        auto r1 = binding.bind("s-a");
        AE_CHECK(r1.has_value(), "B3 setup: session s-a binds first");

        auto r2 = binding.bind("s-b");
        AE_CHECK(!r2.has_value(),
                 "B3: a bind() attempt from a DIFFERENT session_id fails closed -- the whole point "
                 "of this type: two sessions must never both reach the same runner");
        if (!r2.has_value()) {
            AE_CHECK(r2.error().code == "codeact.runner_bound_to_other_session",
                     "B3: the failure carries the specific error code, not a generic denial");
        }
        AE_CHECK(binding.is_bound_to("s-a"),
                 "B3: the binding is STILL bound to s-a -- the rejected claimant never took over");
        AE_CHECK(!binding.is_bound_to("s-b"),
                 "B3: the rejected claimant s-b is definitely not considered bound");
    }

    std::cout << (g_failures == 0 ? "test_codeact_runner_binding: OK\n"
                                   : "test_codeact_runner_binding: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
