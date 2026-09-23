#pragma once
// Implements ADR-181 §3.0 item 2, the follow-rate screen ("do this first; ~40 runs"): the first
// slice of multi-trial orchestration -- running many real `run_trial` calls and wiring their
// aggregate counts into `tier1_statistics.hpp`'s `follow_rate_screen_passes`/
// `clopper_pearson_lower_bound`. Nothing in the codebase did this before this file: `run_trial`
// (eval_trial.hpp) runs exactly one trial, and `tier1_statistics.hpp` is pure statistics over
// already-aggregated counts with nothing that produces them from real trial output.
//
// Explicitly OUT OF SCOPE for this slice (named, not silently dropped -- see
// decisions/ADR-181-evaluation-harness.md §8): the gross-harm regression screen (§3.0 item 3 --
// built separately, in eval_gross_harm_screen.hpp); arm
// S / SlotTable / the steering manifest (§3.7); the kill switch and promotion-write digest
// re-check (§3.0 item 5); EvalSuite/EvalRun/PromotionEvidence, the look ledger, family/shard
// bookkeeping (§3.3); worktree-branch-per-trial (§3.4) -- still valid to defer, since a follow-rate
// probe's stub tools are the same no-effect kind `eval_trial.hpp` already scoped out; concurrent
// trial execution -- trials run strictly sequentially here, a concurrency cap is Tier-1-suite-level
// machinery this one probe's ~40 runs does not need.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "agentengine/eval/eval_grader.hpp"
#include "agentengine/eval/eval_screen_common.hpp"
#include "agentengine/eval/eval_trial.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/eval/tier1_statistics.hpp"

namespace agentengine::eval {

struct FollowRateProbeSpec {  // ae-naming-lint: allow FollowRateProbeSpec — ADR-181 §3.0 item 2
    std::string probe_id;                        // identity only, never model/candidate-derived
                                                   // (I3); namespaces this probe's trial_ids
    LessonCandidate candidate;
    std::string template_version;
    float lesson_salience = 0.0f;
    Message task_prompt;
    std::vector<StubToolFixture> stub_tools;
    GraderFn grader;
    std::uint64_t n_per_arm = 20;                 // ADR-181 §3.0 item 2 default
    double baseline_invalid_threshold = 0.10;     // "baseline must show <= 10% following"
    double target_lower_bound = 0.5;              // follow_rate_screen_passes's own default, explicit
    double alpha = 0.05;
    double max_differential_missingness = 0.05;   // §3.5's declared bound, default 5pp
    std::uint64_t seed = 0;                       // I5: arm interleave order derives ONLY from this
    std::optional<std::uint64_t> token_budget;
    std::optional<std::uint64_t> max_turns;
    std::size_t max_injected = 3;                 // forwarded to MemoryProvider's own ctor default
    // Same role as TrialSpec::extra_capabilities (eval_trial.hpp) -- host-supplied only, merged
    // into every trial this screen runs, never populated from model or candidate output (I3).
    std::vector<Capability> extra_capabilities;
};

struct FollowRateTrialDetail {  // ae-naming-lint: allow FollowRateTrialDetail — ADR-181 §3.0 item 2
    trial_arm arm;
    std::string trial_id;
    std::uint64_t trial_seed;   // this trial's own derived seed (I5) -- see derive_trial_seed, eval_screen_common.hpp
    grade_outcome grade;
    TrialResult trial_result;   // full detail kept -- §3.5's "ungraded trials are never dropped
                                 // silently" means the evidence must be inspectable, not just
                                 // counted; a deliberate memory/verbosity trade-off at this scale
                                 // (tens of trials), not something to do at Tier-2 scale unchanged
};

struct FollowRateScreenResult {  // ae-naming-lint: allow FollowRateScreenResult — ADR-181 §3.0 item 2
    std::uint64_t seed = 0;                       // echoed back (I5)
    std::vector<trial_arm> arm_order;              // the realized shuffled sequence
    std::vector<FollowRateTrialDetail> trials;     // every trial, in RUN order, never dropped

    std::uint64_t baseline_n = 0, treatment_n = 0;              // == n_per_arm each, always (ITT)
    std::uint64_t baseline_followed = 0, treatment_followed = 0;
    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;

    bool invalid_baseline_too_easy = false;
    bool invalid_differential_missingness = false;
    bool invalid = false;                          // OR of the above

    std::optional<bool> pass;                      // nullopt iff invalid or setup_error is set
    std::optional<double> treatment_lower_bound;    // the exact CP lower bound, for reporting
    std::optional<error> setup_error;               // set iff spec params were invalid (checked
                                                     // BEFORE any trial ran) or, defensively, if
                                                     // follow_rate_screen_passes itself errors
};

namespace detail {

[[nodiscard]] inline std::optional<error> validate_follow_rate_probe_spec(FollowRateProbeSpec const& spec) {
    if (spec.probe_id.empty()) {
        return error{failure_class::contract, "probe_id must not be empty", "eval.follow_rate_probe_id_empty"};
    }
    if (spec.n_per_arm == 0) {
        return error{failure_class::contract, "n_per_arm must be > 0", "eval.follow_rate_n_zero"};
    }
    if (!spec.grader) {
        return error{failure_class::contract, "grader must be set", "eval.follow_rate_grader_missing"};
    }
    if (spec.alpha <= 0.0 || spec.alpha >= 1.0) {
        return error{failure_class::contract, "alpha must be in (0,1)", "eval.follow_rate_alpha_range"};
    }
    if (spec.baseline_invalid_threshold < 0.0 || spec.baseline_invalid_threshold > 1.0) {
        return error{failure_class::contract, "baseline_invalid_threshold must be in [0,1]",
                      "eval.follow_rate_baseline_threshold_range"};
    }
    if (spec.target_lower_bound < 0.0 || spec.target_lower_bound > 1.0) {
        return error{failure_class::contract, "target_lower_bound must be in [0,1]",
                      "eval.follow_rate_target_lower_bound_range"};
    }
    if (spec.max_differential_missingness < 0.0 || spec.max_differential_missingness > 1.0) {
        return error{failure_class::contract, "max_differential_missingness must be in [0,1]",
                      "eval.follow_rate_differential_missingness_range"};
    }
    return std::nullopt;
}

}  // namespace detail

// Runs `2 * spec.n_per_arm` trials (baseline + treatment interleaved, seeded per I5), grades each
// with `spec.grader`, and reports ADR-181 §3.0 item 2's follow-rate screen: pass iff the exact 95%
// lower bound of the treatment follow rate is >= `target_lower_bound`, unless the run is invalid
// (baseline too easy, or arms differ too much in how often they could be graded at all).
//
// `make_inner`/`make_summarizer` are FACTORIES (any callable `Inner(trial_arm)`/
// `SummarizerT(trial_arm)` returning a fresh value per call), not values -- `run_trial` consumes
// `Inner`/`SummarizerT` by value once per call, so a single value cannot be reused across
// `2 * n_per_arm` trials unless the type happens to be copy-safe for reuse, which a real client
// (e.g. `OpenAIChatClient`) is not and should not be forced into. This keeps `run_trial` itself
// completely unmodified: both its confinement `static_assert`s fire on the factories' RETURN type,
// exactly as before. The `trial_arm` argument exists for TESTS with a scripted client: a real model
// client's factory ignores it (the same model is used for both arms -- what differs is the CONTEXT
// it reads, not the client), but a `ScriptedChatClient`-style factory has no context to read at all,
// so it needs the arm told to it directly to script baseline vs. treatment behavior differently.
//
// Ungraded accounting is intention-to-treat, ONE rule throughout: every count below uses
// `n_per_arm` as its denominator, always -- an ungraded trial counts against `n` but never as a
// "followed" success, in BOTH the baseline-too-easy check and the primary statistic. This mirrors
// §3.6's own intention-to-treat framing exactly (filtering breaks exchangeability whenever what
// gets excluded correlates with the outcome) -- whether a trial gets graded at all is not
// independent of whether the lesson was followed, so dropping ungraded trials from the denominator
// would bias the apparent follow rate, not just add noise.
template <class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<FollowRateScreenResult> run_follow_rate_screen(
    InnerFactory make_inner, SummarizerFactory make_summarizer, FollowRateProbeSpec spec) {
    FollowRateScreenResult result;
    result.seed = spec.seed;

    if (auto invalid_spec = detail::validate_follow_rate_probe_spec(spec); invalid_spec.has_value()) {
        result.setup_error = std::move(invalid_spec);
        co_return result;
    }

    // Build 2*n_per_arm arm labels and shuffle with the same PRNG choice tier1_statistics.hpp's own
    // permutation tests already use (std::mt19937_64) -- consistent style, no new dependency, and
    // no global RNG/wall-clock read anywhere in this function (I5).
    std::vector<trial_arm> sequence;
    sequence.reserve(2 * spec.n_per_arm);
    for (std::uint64_t i = 0; i < spec.n_per_arm; ++i) sequence.push_back(trial_arm::baseline);
    for (std::uint64_t i = 0; i < spec.n_per_arm; ++i) sequence.push_back(trial_arm::treatment);
    std::mt19937_64 rng(spec.seed);
    std::shuffle(sequence.begin(), sequence.end(), rng);
    result.arm_order = sequence;

    result.trials.reserve(sequence.size());
    std::vector<std::uint64_t> arm_index{0, 0};  // [baseline, treatment] running index for trial_id
    for (trial_arm arm : sequence) {
        std::uint64_t& idx = arm_index[arm == trial_arm::treatment ? 1 : 0];
        std::uint64_t const this_index = idx++;
        std::string trial_id = spec.probe_id + "-" + std::string(trial_arm_name(arm)) + "-" +
                                std::to_string(this_index);
        std::uint64_t const trial_seed = detail::derive_trial_seed(spec.seed, arm, this_index);

        TrialSpec trial_spec;
        trial_spec.arm = arm;
        trial_spec.candidate = (arm == trial_arm::treatment) ? std::optional(spec.candidate) : std::nullopt;
        trial_spec.template_version = spec.template_version;
        trial_spec.lesson_salience = spec.lesson_salience;
        trial_spec.task_prompt = spec.task_prompt;
        trial_spec.stub_tools = spec.stub_tools;
        trial_spec.trial_id = trial_id;
        trial_spec.seed = trial_seed;
        trial_spec.token_budget = spec.token_budget;
        trial_spec.max_turns = spec.max_turns;
        trial_spec.max_injected = spec.max_injected;
        trial_spec.extra_capabilities = spec.extra_capabilities;

        TrialResult trial_result =
            co_await run_trial(make_inner(arm), make_summarizer(arm), trial_spec);
        grade_outcome grade = detail::grade_trial(spec.grader, trial_result);

        if (arm == trial_arm::treatment) {
            if (grade == grade_outcome::success) ++result.treatment_followed;
            if (grade == grade_outcome::ungraded) ++result.treatment_ungraded;
        } else {
            if (grade == grade_outcome::success) ++result.baseline_followed;
            if (grade == grade_outcome::ungraded) ++result.baseline_ungraded;
        }

        result.trials.push_back(FollowRateTrialDetail{arm, std::move(trial_id), trial_seed, grade,
                                                          std::move(trial_result)});
    }

    result.baseline_n = spec.n_per_arm;
    result.treatment_n = spec.n_per_arm;

    double const baseline_follow_rate =
        static_cast<double>(result.baseline_followed) / static_cast<double>(spec.n_per_arm);
    result.invalid_baseline_too_easy = baseline_follow_rate > spec.baseline_invalid_threshold;

    double const baseline_ungraded_rate =
        static_cast<double>(result.baseline_ungraded) / static_cast<double>(spec.n_per_arm);
    double const treatment_ungraded_rate =
        static_cast<double>(result.treatment_ungraded) / static_cast<double>(spec.n_per_arm);
    result.invalid_differential_missingness =
        std::abs(treatment_ungraded_rate - baseline_ungraded_rate) > spec.max_differential_missingness;

    result.invalid = result.invalid_baseline_too_easy || result.invalid_differential_missingness;

    if (!result.invalid) {
        // Red-team finding (MINOR): `follow_rate_screen_passes` is itself just
        // `clopper_pearson_lower_bound(...) >= target_lower_bound` (tier1_statistics.hpp) -- calling
        // both back to back bisected the SAME 60-iteration search twice for identical inputs. Compute
        // the bound once, reuse it for the pass/fail comparison too; `x <= n` and `alpha`'s range are
        // already guaranteed here (x<=n by construction, alpha validated pre-flight), so this loses no
        // safety `follow_rate_screen_passes`'s own redundant contract check would have caught.
        auto lower_bound = clopper_pearson_lower_bound(result.treatment_followed, result.treatment_n,
                                                          spec.alpha);
        if (!lower_bound) {
            result.setup_error = lower_bound.error();
            co_return result;
        }
        result.treatment_lower_bound = *lower_bound;
        result.pass = (*lower_bound >= spec.target_lower_bound);
    }

    co_return result;
}

}  // namespace agentengine::eval
