#pragma once
// Implements ADR-181 §3.0 item 3, the gross-harm regression screen: `tasks.size()` dev tasks x K
// trials x {baseline, treatment}, intention-to-treat, flagged if EITHER a one-sided sign-flip test on
// the SUM of per-task differences OR the round-4 hypergeometric MIN-TASK statistic (harm concentrated
// in a few tasks, §6 G3) comes in under `alpha`. Both statistics already existed as pure functions in
// tier1_statistics.hpp with no caller; this is that caller.
//
// Conventions taken from the code, not assumed: tests/test_tier1_statistics.cpp treats "K/K in A,
// 0/K in B" as harm, so `a` = baseline and `b` = treatment (diff = T - B, negative = harm); and
// tools/adr181_sims/sim181g.py, the simulation behind §6 G3's measured rates, flags each statistic
// at a strict `p < 0.10`.
//
// Closes R6-Num3 (§7/§8): `hypergeometric_min_task_lower_tail_pvalue`'s cost is
// O(num_permutations x tasks x K) with no I8 budget of its own, and the ADR placed that budget on
// "the trial-running harness that will call it". `max_permutation_work` is that budget, checked
// before any trial runs, alongside `max_trials` for the model calls themselves.
//
// Explicitly OUT OF SCOPE (named, not silently dropped -- decisions/ADR-181-evaluation-harness.md
// §8): Tier-1 pre-registration hashing and the per-family attempt counter; the baseline canary;
// per-task variance and the task-level CI (Tier 2, §3.6); task generators; arm S / SlotTable (§3.7);
// the kill switch (§3.0 item 5); EvalSuite/EvalRun/the look ledger (§3.3); worktree-branch-per-trial
// (§3.4 -- stub tools still have no real effect to confine); concurrent execution (trials run
// strictly sequentially, as in the follow-rate screen).

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
    std::vector<RegressionTask> tasks;            // ADR default: 30
    std::uint32_t k_per_arm = 5;                  // ADR default
    double alpha = 0.10;                          // both statistics, strict `<` (sim181g.py)
    std::uint32_t num_permutations = 2000;
    std::uint64_t max_trials = 1000;              // I8: model-call budget, 2 * K * tasks
    std::uint64_t max_permutation_work = 50'000'000;  // I8: num_permutations * tasks * 2K
    double max_differential_missingness = 0.05;   // §3.5's declared bound
    std::uint64_t seed = 0;                       // I5: run order, per-trial and permutation seeds
    std::optional<std::uint64_t> token_budget;
    std::optional<std::uint64_t> max_turns;
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
    grade_outcome grade;
    TrialResult trial_result;                     // kept whole: §3.5, never dropped silently -- a
                                                  // memory trade-off at ~300 trials, disclosed
};

struct GrossHarmTaskResult {  // ae-naming-lint: allow GrossHarmTaskResult — ADR-181 §3.0 item 3
    std::string task_id;
    std::uint32_t baseline_successes = 0, treatment_successes = 0;
    std::uint32_t baseline_ungraded = 0, treatment_ungraded = 0;
    double diff = 0.0;                            // (treatment - baseline) / K, ITT
};

struct GrossHarmScreenResult {  // ae-naming-lint: allow GrossHarmScreenResult — ADR-181 §3.0 item 3
    std::uint64_t seed = 0;                       // echoed back (I5)
    std::vector<GrossHarmTrialDetail> trials;     // every trial, in RUN order, never dropped
    std::vector<GrossHarmTaskResult> per_task;    // in spec.tasks order

    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    // Treatment trials in which the lesson reached the model by EITHER route. Informational only --
    // the screen is intention-to-treat and never filters on it -- but reported as one OR'd count so
    // no reader mistakes `TrialResult::delivered` (context injection alone) for "delivered" (§3.2,
    // the round-2 note on the trial-running slice).
    std::uint64_t treatment_delivered = 0;

    bool invalid_differential_missingness = false;
    bool invalid = false;

    std::optional<double> sum_pvalue;
    std::optional<double> min_task_pvalue;
    bool flagged_by_sum = false;
    bool flagged_by_min_task = false;
    std::optional<bool> flagged;                  // nullopt iff invalid or setup_error is set
    std::optional<error> setup_error;             // spec invalid (checked BEFORE any trial ran), or
                                                  // a statistic itself returned an error
};

namespace detail {

[[nodiscard]] inline std::optional<error> validate_gross_harm_spec(GrossHarmScreenSpec const& spec) {
    auto contract = [](char const* msg, char const* code) {
        return error{failure_class::contract, msg, code};
    };
    if (spec.suite_id.empty()) return contract("suite_id must not be empty", "eval.gross_harm_suite_id_empty");
    if (spec.tasks.empty()) return contract("tasks must not be empty", "eval.gross_harm_no_tasks");
    std::set<std::string> seen;
    for (RegressionTask const& t : spec.tasks) {
        if (t.task_id.empty()) return contract("every task_id must be non-empty", "eval.gross_harm_task_id_empty");
        if (!seen.insert(t.task_id).second) {
            return contract("task_ids must be unique", "eval.gross_harm_task_id_duplicate");
        }
        if (!t.grader) return contract("every task needs a grader", "eval.gross_harm_grader_missing");
    }
    if (spec.k_per_arm == 0) return contract("k_per_arm must be > 0", "eval.gross_harm_k_zero");
    if (spec.k_per_arm > kMaxHypergeometricK) {
        return contract("k_per_arm exceeds kMaxHypergeometricK", "eval.gross_harm_k_too_large");
    }
    if (spec.alpha <= 0.0 || spec.alpha >= 1.0) return contract("alpha must be in (0,1)", "eval.gross_harm_alpha_range");
    if (spec.num_permutations == 0) {
        return contract("num_permutations must be > 0", "eval.gross_harm_permutations_zero");
    }
    if (spec.max_differential_missingness < 0.0 || spec.max_differential_missingness > 1.0) {
        return contract("max_differential_missingness must be in [0,1]",
                        "eval.gross_harm_differential_missingness_range");
    }

    // Budgets (I8), computed so that no product can wrap: each multiplication is checked against
    // the remaining headroom before it happens. k_per_arm <= kMaxHypergeometricK (1e6) and
    // tasks.size() fits in size_t, so 2*K is always safe; the task-count products are the ones to
    // guard.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t const two_k = 2ull * spec.k_per_arm;
    std::uint64_t const tasks = spec.tasks.size();
    if (tasks > kMax / two_k || two_k * tasks > spec.max_trials) {
        return error{failure_class::resource, "2 * k_per_arm * tasks exceeds max_trials",
                     "eval.gross_harm_trial_budget"};
    }
    std::uint64_t const trials = two_k * tasks;
    if (spec.num_permutations > kMax / trials || spec.num_permutations * trials > spec.max_permutation_work) {
        return error{failure_class::resource,
                     "num_permutations * tasks * 2 * k_per_arm exceeds max_permutation_work",
                     "eval.gross_harm_permutation_budget"};
    }
    return std::nullopt;
}

}  // namespace detail

// Runs `2 * k_per_arm * tasks.size()` trials -- every (task, arm) slot K times, all of them shuffled
// together with one seeded `std::mt19937_64` so arms AND tasks are interleaved (§3.4) -- grades each
// with its task's own grader, and reports the screen.
//
// `make_inner`/`make_summarizer` are factories called as `f(trial_arm, std::size_t task_index)`, one
// fresh client per trial, for the same reason the follow-rate screen takes factories (run_trial
// consumes its clients by value, and a real client is not copy-safe for reuse). The extra
// `task_index` exists for scripted TEST clients, which have no context to read and so must be told
// which task they are scripting; a real client's factory ignores both arguments.
//
// Intention-to-treat, the same one rule as the follow-rate screen: an ungraded trial is never a
// success and never leaves the denominator K. `diff = (treatment_successes - baseline_successes) / K`
// per task. If the two arms' pooled ungraded rates differ by more than `max_differential_missingness`
// (§3.5) the run is `invalid` and neither statistic is computed.
template <class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<GrossHarmScreenResult> run_gross_harm_screen(
    InnerFactory make_inner, SummarizerFactory make_summarizer, GrossHarmScreenSpec spec) {
    GrossHarmScreenResult result;
    result.seed = spec.seed;

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
    std::shuffle(slots.begin(), slots.end(), rng);

    result.trials.reserve(slots.size());
    std::vector<std::uint32_t> task_arm_index(2 * n_tasks, 0);  // per (task, arm): 0..K-1, for trial_id
    std::uint64_t arm_index[2] = {0, 0};                        // per arm, across tasks: for the seed
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
        trial_spec.task_prompt = task_def.task_prompt;
        trial_spec.stub_tools = task_def.stub_tools;
        trial_spec.trial_id = trial_id;
        trial_spec.seed = trial_seed;
        trial_spec.token_budget = spec.token_budget;
        trial_spec.max_turns = spec.max_turns;
        trial_spec.max_injected = spec.max_injected;
        trial_spec.extra_capabilities = spec.extra_capabilities;

        TrialResult trial_result =
            co_await run_trial(make_inner(arm, t), make_summarizer(arm, t), trial_spec);
        grade_outcome const grade = detail::grade_trial(task_def.grader, trial_result);

        GrossHarmTaskResult& tr = result.per_task[t];
        if (arm == trial_arm::treatment) {
            if (grade == grade_outcome::success) ++tr.treatment_successes;
            if (grade == grade_outcome::ungraded) ++tr.treatment_ungraded;
            if (trial_result.delivered || trial_result.delivered_via_recall) ++result.treatment_delivered;
        } else {
            if (grade == grade_outcome::success) ++tr.baseline_successes;
            if (grade == grade_outcome::ungraded) ++tr.baseline_ungraded;
        }

        result.trials.push_back(
            GrossHarmTrialDetail{t, arm, std::move(trial_id), trial_seed, grade, std::move(trial_result)});
    }

    std::vector<double> diffs;
    std::vector<std::uint32_t> baseline_successes, treatment_successes;
    diffs.reserve(n_tasks);
    baseline_successes.reserve(n_tasks);
    treatment_successes.reserve(n_tasks);
    for (GrossHarmTaskResult& tr : result.per_task) {
        tr.diff = (static_cast<double>(tr.treatment_successes) - static_cast<double>(tr.baseline_successes)) /
                  static_cast<double>(K);
        diffs.push_back(tr.diff);
        baseline_successes.push_back(tr.baseline_successes);
        treatment_successes.push_back(tr.treatment_successes);
        result.baseline_ungraded += tr.baseline_ungraded;
        result.treatment_ungraded += tr.treatment_ungraded;
    }

    double const per_arm_n = static_cast<double>(K) * static_cast<double>(n_tasks);
    double const baseline_ungraded_rate = static_cast<double>(result.baseline_ungraded) / per_arm_n;
    double const treatment_ungraded_rate = static_cast<double>(result.treatment_ungraded) / per_arm_n;
    result.invalid_differential_missingness =
        std::abs(treatment_ungraded_rate - baseline_ungraded_rate) > spec.max_differential_missingness;
    result.invalid = result.invalid_differential_missingness;
    if (result.invalid) co_return result;

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
    result.flagged_by_sum = *sum_p < spec.alpha;
    result.flagged_by_min_task = *min_p < spec.alpha;
    result.flagged = result.flagged_by_sum || result.flagged_by_min_task;
    co_return result;
}

}  // namespace agentengine::eval
