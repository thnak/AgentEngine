#pragma once
// Implements ADR-181 §3.0 item 3, the gross-harm regression screen: `tasks.size()` dev tasks x K
// trials x {baseline, treatment}, intention-to-treat, flagged if EITHER a one-sided sign-flip test on
// the SUM of per-task differences OR the round-4 hypergeometric MIN-TASK statistic (harm concentrated
// in a few tasks, §6 G3) comes in under its share of `alpha`. Both statistics already existed as pure
// functions in tier1_statistics.hpp with no caller; this is that caller.
//
// Conventions taken from the code, not assumed: tests/test_tier1_statistics.cpp treats "K/K in A,
// 0/K in B" as harm, so `a` = baseline and `b` = treatment (diff = T - B, negative = harm).
//
// `alpha` is the SCREEN's false-flag rate, not each test's. Red-team finding (MAJOR): running both
// tests at `alpha` each (as tools/adr181_sims/sim181g.py does, at 0.10) makes "either flags" a union
// of two tests with no correction -- measured under no effect at 30 tasks x K=5, the combined
// false-flag rate is ~10% (the sim's own functions agree at 3000 reps), and it grows with K: ~12.5% at
// K=10, ~16.6% at K=20, because the min-task statistic gains power to fire by chance as K grows. Each
// test now runs at `alpha / 2` (Bonferroni), which bounds the screen's combined false-flag rate by
// `alpha` whenever each test is valid at its own level, at any K. See the ADR for the re-measured
// false-flag and power figures.
//
// Closes R6-Num3 (§7/§8): `hypergeometric_min_task_lower_tail_pvalue`'s cost is
// O(num_permutations x tasks x K) with no I8 budget of its own, and the ADR placed that budget on
// its caller. `max_permutation_work` is that budget (both tests' work: perms x tasks x (2K + 1));
// `max_model_calls` (with `max_turns` now required) bounds the model calls, agent and summarizer
// alike; both are checked before any trial runs.
//
// The screen FAILS TOWARD FLAGGING (round-2 red-team, MAJOR): a lesson can make treatment trials
// unmeasurable -- a provider 5xx or read timeout is `transient` whatever caused it, and a grader can
// throw on a malformed model-chosen argument -- and enough of that used to trip a validity rule and
// turn a clear harm into `invalid`, i.e. "no verdict, run it again". Now, whenever a validity rule
// trips, the tests run under HARM-FAVOURING imputation instead (every ungraded treatment trial a
// failure, every ungraded baseline trial a success); if that flags, the screen reports `flagged`.
// `invalid` survives only when even that cannot flag, i.e. when the missing data could not have been
// hiding a harm. Exactly one analysis runs per screen (ITT when valid, harm-favouring when not), so
// the false-flag bound for valid runs is unchanged; invalid runs can only be flagged MORE often.
//
// Explicitly OUT OF SCOPE (named, not silently dropped -- decisions/ADR-181-evaluation-harness.md
// §8's "Still unbuilt" list is the authoritative one): the baseline canary; per-task variance and the task-level CI (Tier 2,
// §3.6); task generators; arm S / SlotTable (§3.7); the kill switch and promotion-write digest
// re-check (§3.0 item 5); EvalSuite/EvalRun/PromotionEvidence, the look ledger, family/shard
// bookkeeping (§3.3); worktree-branch-per-trial (§3.4 -- stub tools still have no real effect to
// confine); concurrent execution (trials run strictly sequentially).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/eval/eval_grader.hpp"
#include "agentengine/eval/eval_screen_common.hpp"
#include "agentengine/eval/eval_trial.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/eval/tier1_statistics.hpp"

namespace agentengine::eval {

// One dev task. Its grader scores TASK SUCCESS (did the agent do the task right), not whether the
// lesson was followed -- this screen asks whether the lesson HURTS ordinary work, not whether it is
// obeyed (that is the follow-rate screen's job).
struct RegressionTask {  // ae-naming-lint: allow RegressionTask — ADR-181 §3.0 item 3
    std::string task_id;                          // identity only, never model/candidate-derived (I3)
    Message task_prompt;
    std::vector<StubToolFixture> stub_tools;
    GraderFn grader;
};

struct GrossHarmScreenSpec {  // ae-naming-lint: allow GrossHarmScreenSpec — ADR-181 §3.0 item 3
    std::string suite_id;                         // namespaces trial_ids (identity only, I3)
    LessonCandidate candidate;
    std::string template_version;
    float lesson_salience = 0.0f;
    lesson_delivery delivery = lesson_delivery::fenced;  // ADR-183: the treatment arm's route, as the host ships it
    std::vector<RegressionTask> tasks;            // ADR default: 30
    std::uint32_t k_per_arm = 5;                  // ADR default
    double alpha = 0.10;                          // the SCREEN's false-flag bound; each test gets alpha/2
    std::uint32_t num_permutations = 2000;
    std::uint64_t max_model_calls = 10'000;       // I8: (2K * tasks + max_retried_trials) * max_turns * 2 must fit
    // Retry pool: a trial whose run hits a transient provider fault is re-run once (same seed), until the
    // pool is spent. Charged in full to max_model_calls up front.
    std::uint32_t max_retried_trials = 16;
    std::uint64_t max_permutation_work = 50'000'000;  // I8: num_permutations * tasks * (2K + 1) must fit
    double max_differential_missingness = 0.05;   // §3.5's declared bound
    // Below this graded fraction in EITHER arm the run is `invalid`: too little was measured.
    double min_graded_fraction = 0.9;
    // Below this baseline success rate the run is `invalid`: harm can only show where the baseline
    // succeeds, so a suite the agent mostly fails anyway cannot say "no harm". A declared floor, not a
    // derived one -- it exists to refuse the zero-information case, not to tune power.
    double min_baseline_success_rate = 0.25;
    bool retain_recordings = false;               // keep full transcripts per trial (memory: trials x turns^2)
    std::uint64_t seed = 0;                       // I5: run order, per-trial and permutation seeds
    std::optional<std::uint64_t> token_budget;
    std::optional<std::uint64_t> summarizer_token_budget;  // unset = token_budget (see TrialSpec)
    std::optional<std::uint64_t> max_turns;       // REQUIRED -- it bounds each trial's model calls
    std::size_t max_injected = 3;
    // Same role as TrialSpec::extra_capabilities -- host-supplied only, never model/candidate-
    // derived (I3), copied into every trial.
    std::vector<Capability> extra_capabilities;
};

struct GrossHarmTrialDetail {  // ae-naming-lint: allow GrossHarmTrialDetail — ADR-181 §3.0 item 3
    std::size_t task_index;
    trial_arm arm;
    std::string trial_id;
    std::uint64_t trial_seed;                     // this trial's own derived seed (I5)
    std::uint32_t attempts = 1;                   // 2 if it was retried after a transient fault
    std::optional<error> retried_error;           // the first attempt's error, when retried
    std::string counted_trial_id;                 // the id `trial_result` ran under (`<trial_id>-retry1` if retried)
    grade_outcome grade;                          // of the attempt that counts
    bool faulted = false;                         // the FIRST attempt failed to measure (ungraded, or retried)
    TrialResult trial_result;                     // every trial kept (§3.5); full transcripts
                                                  // (`recordings`) only if spec.retain_recordings
    std::optional<TrialResult> first_attempt;     // when retried; same transcript rule
};

struct GrossHarmTaskResult {  // ae-naming-lint: allow GrossHarmTaskResult — ADR-181 §3.0 item 3
    std::string task_id;
    std::uint32_t baseline_successes = 0, treatment_successes = 0;
    std::uint32_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::uint32_t baseline_faulted = 0, treatment_faulted = 0;  // first-attempt faults, recovered or not
    double diff = 0.0;                            // (treatment - baseline) / K, ITT
};

struct GrossHarmScreenResult {  // ae-naming-lint: allow GrossHarmScreenResult — ADR-181 §3.0 item 3
    std::uint64_t seed = 0;                       // echoed back (I5)
    std::vector<GrossHarmTrialDetail> trials;     // every trial, in RUN order, never dropped
    std::vector<GrossHarmTaskResult> per_task;    // in spec.tasks order

    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::uint64_t baseline_faulted = 0, treatment_faulted = 0;  // first-attempt faults (see `faulted`)
    double baseline_success_rate = 0.0;           // ITT, over all K * tasks baseline trials
    // Treatment trials in which the lesson reached the model by EITHER route. Informational only --
    // the screen is intention-to-treat and never filters on it -- but reported as one OR'd count so
    // no reader mistakes `TrialResult::delivered` (context injection alone) for "delivered" (§3.2,
    // the round-2 note on the trial-running slice).
    std::uint64_t treatment_delivered = 0;

    // Validity diagnostics. Any of them makes the ITT analysis uninterpretable, so the screen falls
    // back to harm-favouring imputation (`worst_case_imputation`) -- they do NOT by themselves mean
    // "no verdict"; `invalid` below does.
    bool invalid_differential_missingness = false;  // §3.5: arms' ungraded counts diverge
    bool invalid_insufficient_grading = false;      // either arm's graded fraction < min_graded_fraction
    bool invalid_uninformative_baseline = false;    // baseline success rate < min_baseline_success_rate
    // True iff a validity rule tripped AND even harm-favouring imputation does not flag: the run can
    // neither show harm nor rule it out. Exactly when `flagged` is nullopt (without a setup_error).
    bool invalid = false;

    double per_test_alpha = 0.0;                  // alpha / 2, what each p-value was compared against
    // False: the p-values are the ITT analysis (ungraded = failure in both arms). True: a validity rule
    // tripped, and they are the harm-favouring analysis (ungraded treatment = failure, ungraded
    // baseline = success) -- `per_task` still reports the observed ITT counts.
    bool worst_case_imputation = false;
    std::optional<double> sum_pvalue;
    std::optional<double> min_task_pvalue;
    bool flagged_by_sum = false;
    bool flagged_by_min_task = false;
    std::optional<bool> flagged;                  // nullopt iff `invalid` or setup_error is set
    std::optional<error> setup_error;             // spec invalid (checked BEFORE any trial ran), or
                                                  // a statistic itself returned an error
};

namespace detail {

// Pre-flight reachability: a spec is refused if a test could not reliably flag even the most extreme
// harm this suite shape allows (round-1 red-team finding: 3 or fewer tasks left the sum test unable to
// reach p < 0.10 at all, small K does the same to the min-task test, and too few permutations disables
// both, all while the screen still reported "not flagged").
//
// The true p-value at the most extreme data:
//   sum test: every task at the same extreme diff leaves one sign pattern in 2^tasks at or below it.
//   min-task: one task at K/K vs 0/K, every other task uninformative, gives 1 / C(2K, K).
[[nodiscard]] inline double floor_sum_pvalue(std::size_t tasks) {
    return std::ldexp(1.0, -static_cast<int>(std::min<std::size_t>(tasks, 1100)));  // 0 past 2^-1074
}
[[nodiscard]] inline double floor_min_task_pvalue(std::uint32_t K) {
    double const k = static_cast<double>(K);
    double const log_c = std::lgamma(2.0 * k + 1.0) - 2.0 * std::lgamma(k + 1.0);  // log C(2K, K)
    return std::exp(-log_c);
}

// Required probability that a test flags at that most extreme data.
inline constexpr double kReachabilityConfidence = 0.95;

// Round-2 red-team finding (MINOR): round 1 compared the Monte-Carlo p-value's MEAN,
// (1 + perms * p) / (perms + 1), against alpha/2 -- but the verdict uses the REALISED estimate
// (1 + X) / (perms + 1) with X ~ Binomial(perms, p), so a spec could pass pre-flight while total harm
// flagged only about half the time (5 tasks, 2000 perms, alpha 0.064: 61%), and a spec could be refused
// whose test would usually have flagged. Now: the realised p is < alpha/2 iff
// X <= ceil(alpha/2 * (perms + 1)) - 2, and that must hold with probability >= kReachabilityConfidence.
[[nodiscard]] inline bool test_reliably_reachable(double floor_p, std::uint32_t perms, double per_test_alpha) {
    double const bound = per_test_alpha * (static_cast<double>(perms) + 1.0);  // need 1 + X < bound
    if (!(bound > 1.0)) return false;                                           // even X = 0 misses
    auto const x_max = static_cast<std::uint64_t>(std::ceil(bound)) - 2;
    return binomial_cdf_le(x_max, perms, floor_p) >= kReachabilityConfidence;
}

[[nodiscard]] inline std::optional<error> validate_gross_harm_spec(GrossHarmScreenSpec const& spec) {
    auto contract = [](char const* msg, char const* code) {
        return error{failure_class::contract, msg, code};
    };
    if (!usable_trial_id_part(spec.suite_id)) {
        return contract("suite_id must be non-empty and contain no ':'", "eval.gross_harm_suite_id_invalid");
    }
    if (spec.tasks.empty()) return contract("tasks must not be empty", "eval.gross_harm_no_tasks");
    std::set<std::string> seen;
    for (RegressionTask const& t : spec.tasks) {
        if (!usable_trial_id_part(t.task_id)) {
            return contract("every task_id must be non-empty and contain no ':'", "eval.gross_harm_task_id_invalid");
        }
        if (!seen.insert(t.task_id).second) {
            return contract("task_ids must be unique", "eval.gross_harm_task_id_duplicate");
        }
        if (!t.grader) return contract("every task needs a grader", "eval.gross_harm_grader_missing");
    }
    if (spec.k_per_arm == 0) return contract("k_per_arm must be > 0", "eval.gross_harm_k_zero");
    if (spec.k_per_arm > kMaxHypergeometricK) {
        return contract("k_per_arm exceeds kMaxHypergeometricK", "eval.gross_harm_k_too_large");
    }
    // NaN-safe: each check is written so a NaN fails it (red-team finding -- a NaN alpha used to pass
    // and then made every `p < alpha` comparison false, so even total harm reported "not flagged").
    if (!in_open_unit_interval(spec.alpha)) return contract("alpha must be in (0,1)", "eval.gross_harm_alpha_range");
    if (spec.num_permutations == 0) {
        return contract("num_permutations must be > 0", "eval.gross_harm_permutations_zero");
    }
    if (!in_closed_unit_interval(spec.max_differential_missingness)) {
        return contract("max_differential_missingness must be in [0,1]",
                        "eval.gross_harm_differential_missingness_range");
    }
    if (!in_closed_unit_interval(spec.min_graded_fraction)) {
        return contract("min_graded_fraction must be in [0,1]", "eval.gross_harm_min_graded_fraction_range");
    }
    if (!in_closed_unit_interval(spec.min_baseline_success_rate)) {
        return contract("min_baseline_success_rate must be in [0,1]",
                        "eval.gross_harm_min_baseline_success_rate_range");
    }

    // Budgets (I8), computed so that no product can wrap: each multiplication is checked against the
    // remaining headroom before it happens. k_per_arm <= kMaxHypergeometricK (1e6), so 2K is safe.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t const two_k = 2ull * spec.k_per_arm;
    std::uint64_t const tasks = spec.tasks.size();
    if (tasks > kMax / two_k) {
        return error{failure_class::resource, "2 * k_per_arm * tasks overflows", "eval.screen_model_call_budget"};
    }
    std::uint64_t const trials = two_k * tasks;
    if (auto budget = validate_call_budget(spec.max_turns, trials, spec.max_retried_trials, spec.max_model_calls);
        budget.has_value()) {
        return budget;
    }
    // Both tests' work: the min-task test does perms x tasks x 2K, the sign-flip sum test perms x tasks
    // (round-2 NIT: the budget counted only the first). trials + tasks cannot wrap: trials fits the
    // call budget above, so it is far below kMax / 2.
    std::uint64_t const per_permutation = trials + tasks;
    if (spec.num_permutations > kMax / per_permutation ||
        spec.num_permutations * per_permutation > spec.max_permutation_work) {
        return error{failure_class::resource,
                     "num_permutations * tasks * (2 * k_per_arm + 1) exceeds max_permutation_work",
                     "eval.gross_harm_permutation_budget"};
    }

    // Each test must be able to flag at its own level, or the screen silently runs a test that can
    // never fire and still reports "not flagged".
    double const per_test_alpha = spec.alpha / 2.0;
    if (!test_reliably_reachable(floor_sum_pvalue(spec.tasks.size()), spec.num_permutations, per_test_alpha)) {
        return contract("too few tasks or permutations for the sum test to reliably reach alpha/2",
                        "eval.gross_harm_sum_test_unreachable");
    }
    if (!test_reliably_reachable(floor_min_task_pvalue(spec.k_per_arm), spec.num_permutations, per_test_alpha)) {
        return contract("K or num_permutations too small for the min-task test to reliably reach alpha/2",
                        "eval.gross_harm_min_task_test_unreachable");
    }

    // A candidate the lesson template rejects would fail setup in EVERY treatment trial -- after every
    // baseline trial had already spent its model calls. Refused here instead (round-2, MINOR).
    if (auto rendered = render_lesson(spec.candidate, spec.template_version, spec.lesson_salience); !rendered) {
        return rendered.error();
    }
    return std::nullopt;
}

}  // namespace detail

// Runs `2 * k_per_arm * tasks.size()` trials -- every (task, arm) slot K times, all of them shuffled
// together with one seeded engine so arms AND tasks are interleaved (§3.4) -- grades each with its
// task's own grader, and reports the screen.
//
// `make_inner`/`make_summarizer` are factories called as `f(TrialSlot const&)` (eval_screen_common.hpp), one
// fresh client per trial, for the same reason the follow-rate screen takes factories (run_trial
// consumes its clients by value, and a real client is not copy-safe for reuse). The slot's `arm`,
// `task_index` and `attempt` exist for scripted TEST clients, which have no context to read and so must
// be told what they are scripting; a real client's factory reads only `trial_seed`.
//
// Intention-to-treat, the same one rule as the follow-rate screen: a trial is a success only if its
// grader says so, an agent that failed to finish is a failure, and only a failed MEASUREMENT is
// `ungraded` (eval_screen_common.hpp's `grade_trial`) -- and even that never leaves the denominator K.
// `diff = (treatment_successes - baseline_successes) / K` per task. The ITT analysis decides only when
// the run is interpretable: enough of each arm graded, the arms' ungraded counts within
// `max_differential_missingness` (§3.5), and a baseline that succeeds often enough for harm to show.
// Otherwise the harm-favouring analysis decides (see the file-top comment): flagged if it flags,
// `invalid` if it does not.
template <class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<GrossHarmScreenResult> run_gross_harm_screen(
    InnerFactory make_inner, SummarizerFactory make_summarizer, GrossHarmScreenSpec spec) {
    GrossHarmScreenResult result;
    result.seed = spec.seed;
    result.per_test_alpha = spec.alpha / 2.0;

    if (auto invalid_spec = detail::validate_gross_harm_spec(spec); invalid_spec.has_value()) {
        result.setup_error = std::move(invalid_spec);
        co_return result;
    }

    std::size_t const n_tasks = spec.tasks.size();
    std::uint32_t const K = spec.k_per_arm;

    result.per_task.resize(n_tasks);
    for (std::size_t t = 0; t < n_tasks; ++t) result.per_task[t].task_id = spec.tasks[t].task_id;

    std::vector<std::pair<std::size_t, trial_arm>> slots;
    slots.reserve(2ull * K * n_tasks);
    for (std::size_t t = 0; t < n_tasks; ++t) {
        for (std::uint32_t k = 0; k < K; ++k) {
            slots.emplace_back(t, trial_arm::baseline);
            slots.emplace_back(t, trial_arm::treatment);
        }
    }
    std::mt19937_64 rng(spec.seed);
    detail::portable_shuffle(slots.begin(), slots.end(), rng);  // same order on every std lib (I5)

    result.trials.reserve(slots.size());
    std::vector<std::uint32_t> task_arm_index(2 * n_tasks, 0);  // per (task, arm): 0..K-1, for trial_id
    std::uint64_t arm_index[2] = {0, 0};                        // per arm, across tasks: for the seed
    std::uint64_t retries_left = spec.max_retried_trials;
    // Harm-favouring success counts per task: a faulted TREATMENT trial is a failure even if its retry
    // succeeded; a faulted BASELINE trial is a success even if its retry failed.
    std::vector<std::uint32_t> baseline_worst(n_tasks, 0), treatment_worst(n_tasks, 0);
    for (auto const& [t, arm] : slots) {
        std::size_t const a = (arm == trial_arm::treatment) ? 1 : 0;
        RegressionTask const& task_def = spec.tasks[t];

        std::uint32_t const k = task_arm_index[2 * t + a]++;
        std::string trial_id = spec.suite_id + "-" + task_def.task_id + "-" +
                                std::string(trial_arm_name(arm)) + "-" + std::to_string(k);
        std::uint64_t const trial_seed = detail::derive_trial_seed(spec.seed, arm, arm_index[a]++);

        TrialSpec trial_spec;
        trial_spec.arm = arm;
        trial_spec.candidate = (arm == trial_arm::treatment) ? std::optional(spec.candidate) : std::nullopt;
        trial_spec.template_version = spec.template_version;
        trial_spec.lesson_salience = spec.lesson_salience;
        trial_spec.delivery = spec.delivery;
        trial_spec.task_prompt = task_def.task_prompt;
        trial_spec.stub_tools = task_def.stub_tools;
        trial_spec.seed = trial_seed;
        trial_spec.token_budget = spec.token_budget;
        trial_spec.summarizer_token_budget =
            spec.summarizer_token_budget.has_value() ? spec.summarizer_token_budget : spec.token_budget;
        trial_spec.max_turns = spec.max_turns;
        trial_spec.max_injected = spec.max_injected;
        trial_spec.extra_capabilities = spec.extra_capabilities;

        // A throwing factory or `run_trial` is that trial's setup error (`ungraded`), and a transient
        // fault gets one retry from the pool (eval_screen_common.hpp); in the treatment arm, the
        // harm-favouring fallback keeps any lesson-induced fault from hiding harm.
        TrialSlot const slot{arm, t, 0, trial_id, trial_seed};
        detail::AttemptedTrial attempted =
            co_await detail::run_trial_with_retry(make_inner, make_summarizer, slot, std::move(trial_spec), retries_left);
        TrialResult trial_result = std::move(attempted.result);
        grade_outcome const grade = detail::grade_trial(task_def.grader, trial_result);
        bool const faulted = detail::was_faulted(attempted.attempts, grade);
        detail::drop_transcripts_unless_retained(trial_result, spec.retain_recordings);
        if (attempted.first_attempt.has_value()) {
            detail::drop_transcripts_unless_retained(*attempted.first_attempt, spec.retain_recordings);
        }

        GrossHarmTaskResult& tr = result.per_task[t];
        if (arm == trial_arm::treatment) {
            if (grade == grade_outcome::success) ++tr.treatment_successes;
            if (grade == grade_outcome::ungraded) ++tr.treatment_ungraded;
            if (faulted) ++tr.treatment_faulted;
            if (grade == grade_outcome::success && !faulted) ++treatment_worst[t];
            if (trial_result.delivered || trial_result.delivered_via_recall) ++result.treatment_delivered;
        } else {
            if (grade == grade_outcome::success) ++tr.baseline_successes;
            if (grade == grade_outcome::ungraded) ++tr.baseline_ungraded;
            if (faulted) ++tr.baseline_faulted;
            if (grade == grade_outcome::success || faulted) ++baseline_worst[t];
        }

        GrossHarmTrialDetail detail_row{t, arm, std::move(trial_id), trial_seed, attempted.attempts,
                                        std::move(attempted.retried_error), std::move(attempted.counted_trial_id),
                                        grade, faulted, std::move(trial_result), std::move(attempted.first_attempt)};
        result.trials.push_back(std::move(detail_row));
    }

    std::uint64_t baseline_success_total = 0;
    for (GrossHarmTaskResult& tr : result.per_task) {
        tr.diff = (static_cast<double>(tr.treatment_successes) - static_cast<double>(tr.baseline_successes)) /
                  static_cast<double>(K);
        baseline_success_total += tr.baseline_successes;
        result.baseline_ungraded += tr.baseline_ungraded;
        result.treatment_ungraded += tr.treatment_ungraded;
        result.baseline_faulted += tr.baseline_faulted;
        result.treatment_faulted += tr.treatment_faulted;
    }

    std::uint64_t const per_arm = static_cast<std::uint64_t>(K) * n_tasks;
    double const per_arm_n = static_cast<double>(per_arm);
    double const baseline_ungraded_rate = static_cast<double>(result.baseline_ungraded) / per_arm_n;
    double const treatment_ungraded_rate = static_cast<double>(result.treatment_ungraded) / per_arm_n;
    result.baseline_success_rate = static_cast<double>(baseline_success_total) / per_arm_n;

    // On FIRST-ATTEMPT faults, not on what is left ungraded after retries (PR #100 red team, MAJOR): a
    // retry is a fresh draw, so post-retry counts let a lesson whose faults do not recur every time look
    // exactly like one that causes none.
    result.invalid_differential_missingness = detail::differential_missingness_exceeds(
        result.treatment_faulted, result.baseline_faulted, per_arm, spec.max_differential_missingness);
    // Red-team finding (MAJOR): with every trial in both arms ungraded, the arms' ungraded rates were
    // EQUAL, so the run counted as valid, every diff was 0, and the screen said "no harm" from zero
    // data. The same happens more quietly at a baseline floor -- a suite the agent fails everywhere has
    // no success left for a lesson to take away. Neither may produce a "no harm" verdict now.
    result.invalid_insufficient_grading = (1.0 - baseline_ungraded_rate) < spec.min_graded_fraction ||
                                          (1.0 - treatment_ungraded_rate) < spec.min_graded_fraction;
    result.invalid_uninformative_baseline = result.baseline_success_rate < spec.min_baseline_success_rate;
    result.worst_case_imputation = result.invalid_differential_missingness ||
                                   result.invalid_insufficient_grading || result.invalid_uninformative_baseline;

    // ITT: the observed counts (an ungraded trial is already not a success). Harm-favouring: every
    // FAULTED baseline trial counts a success and every faulted treatment trial a failure, whatever its
    // retry did.
    std::vector<double> diffs;
    std::vector<std::uint32_t> baseline_successes, treatment_successes;
    diffs.reserve(n_tasks);
    baseline_successes.reserve(n_tasks);
    treatment_successes.reserve(n_tasks);
    for (std::size_t t = 0; t < n_tasks; ++t) {
        GrossHarmTaskResult const& tr = result.per_task[t];
        std::uint32_t const b = result.worst_case_imputation ? baseline_worst[t] : tr.baseline_successes;
        std::uint32_t const tt = result.worst_case_imputation ? treatment_worst[t] : tr.treatment_successes;
        baseline_successes.push_back(b);
        treatment_successes.push_back(tt);
        diffs.push_back((static_cast<double>(tt) - static_cast<double>(b)) / static_cast<double>(K));
    }

    auto sum_p = sign_flip_sum_lower_tail_pvalue(diffs, spec.num_permutations,
                                                  detail::derive_stream_seed(spec.seed, 1));
    if (!sum_p) {
        result.setup_error = sum_p.error();
        co_return result;
    }
    auto min_p = hypergeometric_min_task_lower_tail_pvalue(baseline_successes, treatment_successes, K,
                                                             spec.num_permutations,
                                                             detail::derive_stream_seed(spec.seed, 2));
    if (!min_p) {
        result.setup_error = min_p.error();
        co_return result;
    }
    result.sum_pvalue = *sum_p;
    result.min_task_pvalue = *min_p;
    result.flagged_by_sum = *sum_p < result.per_test_alpha;
    result.flagged_by_min_task = *min_p < result.per_test_alpha;
    bool const any_flag = result.flagged_by_sum || result.flagged_by_min_task;
    if (result.worst_case_imputation && !any_flag) {
        result.invalid = true;  // could not show harm, and cannot rule it out either
    } else {
        result.flagged = any_flag;
    }
    co_return result;
}

}  // namespace agentengine::eval
