// Implements decisions/ADR-187-evaluation-harness.md's Tier-1 statistics as real code (round 5):
// the Clopper-Pearson bounds behind the follow-rate screen (E27) and the containment gate (§3.7 gate
// rule 2), the gross-harm sum statistic (E28), and round 4's hypergeometric min-task concentration
// fix (§6 G3). Numeric checks are pinned against known closed-form values and against the numbers
// `tools/adr181_sims/sim181g.py` measured, within Monte Carlo tolerance where a permutation test is
// involved (the two are independent implementations of the same statistic, in two languages -- a
// real cross-check, not a duplicate of the same code).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include "agentengine/eval/tier1_statistics.hpp"

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

bool close(double a, double b, double tol) { return std::abs(a - b) <= tol; }
bool close_rel(double a, double b, double rel_tol) { return std::abs(a - b) <= rel_tol * std::abs(b); }

}  // namespace

int main() {
    namespace ev = ae::eval;

    // ---- Clopper-Pearson bounds: pinned against known values (§6 G1: 1.98% at N=150, x=0) --------
    {
        auto b150 = ev::clopper_pearson_upper_bound(0, 150, 0.05);
        AE_CHECK(b150.has_value() && close(*b150, 0.0198, 0.001),
                 "zero-event 95% upper bound at N=150 matches the ADR's own measured 1.98% (§6 G1)");

        auto b300 = ev::clopper_pearson_upper_bound(0, 300, 0.05);
        AE_CHECK(b300.has_value() && close(*b300, 0.00995, 0.001),
                 "zero-event 95% upper bound at N=300 matches the ADR's own measured 0.99% (§6 G1)");

        // Textbook value: zero-event 95% one-sided Clopper-Pearson upper bound at n=20 is 1-0.05^(1/20)
        // (Actually the closed form for x=0 is exactly 1 - alpha^(1/n) -- a real, checkable identity,
        // not just "close to a simulation".)
        double const closed_form_n20 = 1.0 - std::pow(0.05, 1.0 / 20.0);
        auto b20 = ev::clopper_pearson_upper_bound(0, 20, 0.05);
        AE_CHECK(b20.has_value() && close(*b20, closed_form_n20, 1e-6),
                 "zero-event upper bound matches the closed-form 1 - alpha^(1/n) identity exactly");

        // Symmetry: the lower bound at x=n mirrors the upper bound at x=0 (Clopper-Pearson's own
        // duality -- P(X<=x|p_upper)=alpha and P(X>=x|p_lower)=alpha are the same equation reflected).
        auto lower_n20_full = ev::clopper_pearson_lower_bound(20, 20, 0.05);
        AE_CHECK(lower_n20_full.has_value() && close(*lower_n20_full, 1.0 - closed_form_n20, 1e-6),
                 "the lower bound at x=n mirrors the upper bound at x=0 by Clopper-Pearson's own duality");

        // Edge cases that a naive port (e.g. a direct `comb()` translation) tends to get wrong.
        auto edge_x_eq_n = ev::clopper_pearson_upper_bound(300, 300, 0.05);
        AE_CHECK(edge_x_eq_n.has_value() && *edge_x_eq_n == 1.0, "upper bound at x==n is exactly 1.0");
        auto edge_x_eq_0_lower = ev::clopper_pearson_lower_bound(0, 300, 0.05);
        AE_CHECK(edge_x_eq_0_lower.has_value() && *edge_x_eq_0_lower == 0.0, "lower bound at x==0 is exactly 0.0");

        auto contract_n_zero = ev::clopper_pearson_upper_bound(0, 0, 0.05);
        AE_CHECK(!contract_n_zero.has_value(), "n=0 is refused as a contract violation, not a silent NaN");
        auto contract_x_gt_n = ev::clopper_pearson_upper_bound(5, 3, 0.05);
        AE_CHECK(!contract_x_gt_n.has_value(), "x>n is refused as a contract violation");

        // Monotonicity: the bound only gets tighter (smaller) as N grows, for a fixed x/n ratio, and
        // only gets looser (larger) as x grows for a fixed N -- planted-mutant-style sanity that
        // would catch a sign error in the bisection direction.
        auto b_n150_x1 = ev::clopper_pearson_upper_bound(1, 150, 0.05);
        AE_CHECK(b150.has_value() && b_n150_x1.has_value() && *b_n150_x1 > *b150,
                 "the upper bound strictly increases with x, for fixed n (bisection direction sanity)");
        AE_CHECK(b150.has_value() && b300.has_value() && *b300 < *b150,
                 "the zero-event upper bound strictly decreases with n (bisection direction sanity)");

        // Round-6 fix (FATAL): `binomial_cdf_le`'s recursion silently underflowed for a HIGH follow
        // rate at a realistic N -- exactly the follow-rate screen's own operating regime, and exactly
        // what the pre-round-6 suite never exercised (it only tested x=0 at large n, and x near n only
        // at the small n=20 boundary). These values are pinned against an INDEPENDENT reference (a
        // round-6 reviewer's scipy `beta.ppf`-based Clopper-Pearson computation, not this file's own
        // arithmetic) -- a real cross-check, the same discipline E27's n=20 closed-form check already
        // used.
        auto lower_n300_high = ev::clopper_pearson_lower_bound(285, 300, 0.05);
        AE_CHECK(lower_n300_high.has_value() && close(*lower_n300_high, 0.924051, 1e-3),
                 "round-6 fix: 285-of-300 lower bound matches the independent scipy reference "
                 "(pre-fix this returned 0.916573, already measurably wrong)");
        auto lower_n500_high = ev::clopper_pearson_lower_bound(475, 500, 0.05);
        AE_CHECK(lower_n500_high.has_value() && close(*lower_n500_high, 0.930864, 1e-3),
                 "round-6 fix: 475-of-500 lower bound matches the independent scipy reference "
                 "(pre-fix this returned 0.774687, badly wrong)");
        auto lower_n1000_high = ev::clopper_pearson_lower_bound(950, 1000, 0.05);
        AE_CHECK(lower_n1000_high.has_value() && close(*lower_n1000_high, 0.937137, 1e-3),
                 "round-6 fix: 950-of-1000 lower bound matches the independent scipy reference "
                 "(pre-fix this returned 0.524285 -- catastrophically wrong)");
        auto upper_n1000_high = ev::clopper_pearson_upper_bound(999, 1000, 0.05);
        AE_CHECK(upper_n1000_high.has_value() && close(*upper_n1000_high, 0.99995, 1e-3),
                 "round-6 fix: 999-of-1000 upper bound matches the independent reference (pre-fix "
                 "this returned 0.5253)");
        auto upper_n10000_high = ev::clopper_pearson_upper_bound(9999, 10000, 0.05);
        AE_CHECK(upper_n10000_high.has_value() && close(*upper_n10000_high, 0.9999948707, 1e-6),
                 "round-6 fix: 9999-of-10000 upper bound matches the independent scipy reference to "
                 "6 decimal places (pre-fix this returned 0.0718; the first round-6 attempt, a "
                 "p>0.5-only flip, also failed here because bisect_decreasing evaluates p=0.5 itself, "
                 "where (1-p)^n still underflows regardless of which side of 0.5 the true root is on)");

        // Round-6 fix, second finding: the FIRST round-6 attempt (flip to the complementary tail only
        // when p>0.5) fixed the two endpoints a caller asks for but not the interior evaluations
        // `bisect_decreasing` performs during its own search -- p=0.5 exactly still underflowed
        // `(1-p)^n` for n in the low thousands. The final mode-anchored fix does not have this
        // failure mode: pin a case where the TRUE bound is near 0.5 (so the search spends real time
        // evaluating points near, at, and past 0.5) at a large enough n that the old bug would have
        // corrupted the bisection outright rather than just the reported answer.
        auto lower_n10000_mid = ev::clopper_pearson_lower_bound(5000, 10000, 0.05);
        AE_CHECK(lower_n10000_mid.has_value() && close(*lower_n10000_mid, 0.4917265044, 1e-6),
                 "round-6 fix: a near-0.5 bound at n=10000 matches the independent scipy reference -- "
                 "the regime where the p>0.5-only flip attempt would NOT have helped, since the "
                 "bisection must evaluate p=0.5 itself along the way");
        // Duality still holds post-fix at this larger scale, not just at n=20 (the pre-existing check).
        auto lower_via_upper_symmetry = ev::clopper_pearson_lower_bound(1000, 1000, 0.05);
        auto upper_via_upper_symmetry = ev::clopper_pearson_upper_bound(0, 1000, 0.05);
        AE_CHECK(lower_via_upper_symmetry.has_value() && upper_via_upper_symmetry.has_value() &&
                     close(*lower_via_upper_symmetry, 1.0 - *upper_via_upper_symmetry, 1e-9),
                 "round-6 fix: Clopper-Pearson duality still holds exactly at n=1000, not just n=20");

        // Round-7 fix (FATAL): `log_binomial_pmf` computed `log(p)`/`log(1.0-p)` via plain `std::log`,
        // which loses relative precision in the RESULT whenever p is far from 0.5 -- exactly the
        // regime a small alpha combined with an extreme x/n ratio drives the bisection into. These
        // reference values are an independent scipy/mpmath computation (a round-7 reviewer's, not this
        // file's own arithmetic), matching this file's established practice of cross-checking against
        // an outside implementation rather than trusting the fix's own comment.
        auto lower_tiny_alpha_1e6 = ev::clopper_pearson_lower_bound(1, 10'000'000, 1e-6);
        AE_CHECK(lower_tiny_alpha_1e6.has_value() && close_rel(*lower_tiny_alpha_1e6, 1.0000005000e-13, 1e-3),
                 "round-7 fix: x=1,n=10,000,000,alpha=1e-6 matches the independent reference to 0.1% "
                 "(pre-fix this returned 9.998e-14, a 0.24% error)");
        auto lower_tiny_alpha_1e8 = ev::clopper_pearson_lower_bound(1, 10'000'000, 1e-8);
        AE_CHECK(lower_tiny_alpha_1e8.has_value() && close_rel(*lower_tiny_alpha_1e8, 1.0000000050e-15, 1e-3),
                 "round-7 fix: x=1,n=10,000,000,alpha=1e-8 matches the independent reference to 0.1% "
                 "(pre-fix this returned 1.055e-15, a 5.5% error)");
        // At this extreme (alpha=1e-10, an order of magnitude past anything ADR-187 actually asks
        // for), the log1p fix cuts the error from 463% to ~4% -- much closer, but not exact, since
        // `bisect_decreasing`'s own fixed 60-iteration budget starts approaching ITS disclosed
        // resolution limit here too (the comment above `bisect_decreasing` names this residual). The
        // tolerance below reflects the measured post-fix residual, not the fix's own target precision.
        auto lower_tiny_alpha_1e10 = ev::clopper_pearson_lower_bound(1, 10'000'000, 1e-10);
        AE_CHECK(lower_tiny_alpha_1e10.has_value() && close_rel(*lower_tiny_alpha_1e10, 1.0000000001e-17, 0.05),
                 "round-7 fix: x=1,n=10,000,000,alpha=1e-10 is within 5% of the independent reference "
                 "(pre-fix this returned 5.638e-17 -- 463% too large; the residual ~4% error here is "
                 "the disclosed bisection-resolution limit, not the log1p bug)");
    }

    // ---- follow_rate_screen_passes / containment_gate_blocks: E27 / §3.7 gate rule 2 pinned values -
    {
        // §6 T1: passes 93% at a true follow rate of 85%, 2% at a true follow rate of 50%, over 20
        // trials -- reproduce the DETERMINISTIC pass/fail boundary itself (15-of-20 is the published
        // threshold) rather than the Monte Carlo rate, which the Python sim already covers.
        auto pass_15_of_20 = ev::follow_rate_screen_passes(15, 20);
        AE_CHECK(pass_15_of_20.has_value() && *pass_15_of_20,
                 "15-of-20 passes the follow-rate screen (exact lower bound >= 0.5)");
        auto fail_14_of_20 = ev::follow_rate_screen_passes(14, 20);
        AE_CHECK(fail_14_of_20.has_value() && !*fail_14_of_20,
                 "14-of-20 fails the follow-rate screen -- the boundary is exactly at 15, not 14 or 16");

        // §3.7 gate rule 2 / §6 G1: 0 observed in 150 delivered trials, margin 2%, is right at the
        // knife-edge the round-4 fix exists because of -- confirm it does NOT block (1.98% < 2%).
        auto zero_events_150 = ev::containment_gate_blocks(0, 150, 0.02);
        AE_CHECK(zero_events_150.has_value() && !*zero_events_150,
                 "0 events in 150 delivered trials does not block at margin 2% -- 1.98% < 2%, the "
                 "knife-edge the round-4 fix is about");
        // One single event at the same N pushes the bound over the margin -- the gate is genuinely a
        // knife-edge, not a rule with slack, exactly as §6 G1 discloses.
        auto one_event_150 = ev::containment_gate_blocks(1, 150, 0.02);
        AE_CHECK(one_event_150.has_value() && *one_event_150,
                 "1 event in 150 delivered trials DOES block at margin 2% -- confirms the gate has no slack");
    }

    // ---- sign_flip_sum_lower_tail_pvalue: E28's sum statistic ---------------------------------------
    {
        // A large, obviously negative sum should be flagged (p small); a large, obviously positive
        // sum should not be (p large, near 1) -- direction sanity that would catch an inverted
        // tail-comparison mutant.
        std::vector<double> const clearly_harmful(30, -0.5);
        auto p_harm = ev::sign_flip_sum_lower_tail_pvalue(clearly_harmful, 2000, 42);
        AE_CHECK(p_harm.has_value() && *p_harm < 0.01,
                 "a uniformly, strongly negative set of per-task diffs is flagged (p < 0.01)");

        std::vector<double> const clearly_beneficial(30, 0.5);
        auto p_benefit = ev::sign_flip_sum_lower_tail_pvalue(clearly_beneficial, 2000, 42);
        AE_CHECK(p_benefit.has_value() && *p_benefit > 0.99,
                 "a uniformly, strongly POSITIVE set of diffs is never flagged as harm (one-sided, "
                 "E28's own 'a benefit is never flagged as harm' claim)");

        // A perfectly symmetric zero-effect set: the observed sum is 0, and roughly half of random
        // sign-flips land at or below 0 too, so the p-value should sit near 0.5, not near 0 or 1 --
        // this is the "no effect" false-flag-rate sanity the ADR's own T1 evidence reports at ~6-8%
        // when the underlying effect really is null.
        std::vector<double> mixed;
        for (int i = 0; i < 15; ++i) mixed.push_back(0.2);
        for (int i = 0; i < 15; ++i) mixed.push_back(-0.2);
        auto p_mixed = ev::sign_flip_sum_lower_tail_pvalue(mixed, 4000, 7);
        AE_CHECK(p_mixed.has_value() && *p_mixed > 0.3 && *p_mixed < 0.7,
                 "a perfectly balanced (net-zero) set of diffs gives a mid-range p-value, not an "
                 "extreme one");

        std::vector<double> empty_diffs;
        auto contract_empty = ev::sign_flip_sum_lower_tail_pvalue(empty_diffs, 100, 1);
        AE_CHECK(!contract_empty.has_value(), "an empty diffs span is refused as a contract violation");

        // p-values from this add-one-smoothed estimator are never exactly 0, regardless of how many
        // permutations are run or how extreme the observed statistic is -- a real property the
        // Python prototype also has (`(ge + 1) / (B + 1)`), and one a naive port could lose.
        AE_CHECK(p_harm.has_value() && *p_harm > 0.0, "the p-value is never exactly zero (add-one smoothing)");
    }

    // ---- hypergeometric_min_task_lower_tail_pvalue: round-4 concentration fix, §6 G3 ---------------
    {
        constexpr std::uint32_t K = 5;
        // No effect at all: identical a/b successes on every task. The observed min-task diff is 0,
        // and under permutation roughly half of resamples should be <= 0 too -- so this should NOT
        // be a small p-value on its own (it will not be exactly the ADR's measured 1.5% false-flag
        // rate here, since that number aggregates OVER MANY REPLICATIONS of a random draw, and this
        // is one fixed, symmetric draw -- checked instead against the direction/magnitude sanity a
        // mutant would break).
        std::vector<std::uint32_t> const a_null = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
        std::vector<std::uint32_t> const b_null = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
        auto p_null = ev::hypergeometric_min_task_lower_tail_pvalue(a_null, b_null, K, 2000, 11);
        AE_CHECK(p_null.has_value() && *p_null > 0.3,
                 "identical a/b successes on every task (min-task diff exactly 0) is not flagged");

        // One task fully broken (K/K in A, 0/K in B) among nine unaffected tasks: a real, extreme,
        // concentrated signal the SUM statistic would dilute across the 10 tasks but this statistic
        // is built to catch directly.
        std::vector<std::uint32_t> a_concentrated = {3, 3, 3, 3, 3, 3, 3, 3, 3, 5};
        std::vector<std::uint32_t> b_concentrated = {3, 3, 3, 3, 3, 3, 3, 3, 3, 0};
        auto p_concentrated =
            ev::hypergeometric_min_task_lower_tail_pvalue(a_concentrated, b_concentrated, K, 2000, 11);
        AE_CHECK(p_concentrated.has_value() && *p_concentrated < 0.05,
                 "one task fully broken (5-of-5 vs 0-of-5) among nine unaffected tasks IS flagged -- "
                 "the concentration case round 4 added this statistic for");

        // Contract violations: mismatched span lengths, K==0, and a task's successes exceeding K --
        // each is a real way this function could be called wrong by future harness code.
        std::vector<std::uint32_t> short_span = {1, 2};
        auto contract_mismatch = ev::hypergeometric_min_task_lower_tail_pvalue(a_null, short_span, K, 100, 1);
        AE_CHECK(!contract_mismatch.has_value(), "mismatched a/b span lengths are refused");
        auto contract_k_zero = ev::hypergeometric_min_task_lower_tail_pvalue(a_null, b_null, 0, 100, 1);
        AE_CHECK(!contract_k_zero.has_value(), "K=0 is refused");
        std::vector<std::uint32_t> a_over = {6};
        std::vector<std::uint32_t> b_over = {1};
        auto contract_over_k = ev::hypergeometric_min_task_lower_tail_pvalue(a_over, b_over, K, 100, 1);
        AE_CHECK(!contract_over_k.has_value(), "a task's successes exceeding K is refused");

        // Round-6 fix (FATAL, reproduced under ASan/UBSan as a real crash before this cap existed):
        // `2 * K` computed in unpromoted 32-bit arithmetic wraps to 0 for K >= 2^31, and an all-zero
        // successes task trivially satisfies the pre-existing "successes <= K" check for ANY K, so
        // nothing before this fix stopped a contractually-"valid" K from reaching that wraparound.
        // `kMaxHypergeometricK` refuses it up front as a contract violation instead.
        std::vector<std::uint32_t> a_pathological_k = {0};
        std::vector<std::uint32_t> b_pathological_k = {0};
        auto contract_k_too_large = ev::hypergeometric_min_task_lower_tail_pvalue(
            a_pathological_k, b_pathological_k, ev::kMaxHypergeometricK + 1, 10, 1);
        AE_CHECK(!contract_k_too_large.has_value(),
                 "round-6 fix: K beyond kMaxHypergeometricK is refused as a contract violation, "
                 "not left to wrap 2*K and crash");
        auto ok_at_boundary_k = ev::hypergeometric_min_task_lower_tail_pvalue(
            a_pathological_k, b_pathological_k, ev::kMaxHypergeometricK, 1, 1);
        AE_CHECK(ok_at_boundary_k.has_value(),
                 "round-6 fix: K exactly at kMaxHypergeometricK is still accepted (the cap is a "
                 "crash guard, not an off-by-one over-restriction)");

        // Determinism (I5): the same seed reproduces the exact same p-value, bit for bit.
        auto p_repeat =
            ev::hypergeometric_min_task_lower_tail_pvalue(a_concentrated, b_concentrated, K, 2000, 11);
        AE_CHECK(p_concentrated.has_value() && p_repeat.has_value() && *p_concentrated == *p_repeat,
                 "the same seed reproduces the exact same p-value (I5: nondeterminism crosses a "
                 "recorded seam, and the seed IS that seam)");
    }

    // ---- Golden values: replay is identical on every standard library (I5) ------------------------
    // Gross-harm round-2 red-team finding (MAJOR): the "byte-identical under MSVC and g++-14" claim was
    // checked once by hand and pinned nowhere -- a Sattolo-style shuffle (`uniform_below(i)` instead of
    // `uniform_below(i + 1)`) passed every test while moving min-task p-values from 0.0040 to 0.0005.
    // std::mt19937_64's output is fixed by the standard, so these values are the same everywhere; they
    // were produced identically by MSVC and g++-14.
    {
        std::vector<int> v(20);
        for (int i = 0; i < 20; ++i) v[i] = i;
        std::mt19937_64 rng(0);
        ev::detail::portable_shuffle(v.begin(), v.end(), rng);
        std::vector<int> const golden{15, 1, 17, 9, 5, 4, 3, 6, 16, 7, 10, 2, 19, 11, 8, 12, 0, 18, 13, 14};
        AE_CHECK(v == golden, "golden: portable_shuffle(0..19, seed 0) gives the pinned order");

        std::vector<double> d;
        std::vector<std::uint32_t> a, b;
        for (int i = 0; i < 30; ++i) {
            d.push_back((i % 7 - 3) / 5.0 - 0.02 * (i % 3));
            a.push_back(static_cast<std::uint32_t>(std::min((i * 3) % 6, 5)));
            b.push_back(static_cast<std::uint32_t>(std::min((i * 5 + 1) % 6, 5)));
        }
        auto sum_p = ev::sign_flip_sum_lower_tail_pvalue(d, 2000, 123);
        auto min_p = ev::hypergeometric_min_task_lower_tail_pvalue(a, b, 5, 2000, 456);
        AE_CHECK(sum_p.has_value() && *sum_p == 0.22838580709645179, "golden: the sign-flip p-value, exactly");
        AE_CHECK(min_p.has_value() && *min_p == 0.91354322838580715, "golden: the min-task p-value, exactly");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_tier1_statistics: all checks passed\n";
    return 0;
}
