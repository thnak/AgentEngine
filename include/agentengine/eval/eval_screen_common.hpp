#pragma once
// Helpers shared by every ADR-181 Tier-1 screen that runs many `run_trial` calls: the follow-rate
// screen (eval_follow_rate_screen.hpp, §3.0 item 2) and the gross-harm regression screen
// (eval_gross_harm_screen.hpp, §3.0 item 3).

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "agentengine/eval/eval_grader.hpp"
#include "agentengine/eval/eval_trial.hpp"

namespace agentengine::eval {

[[nodiscard]] inline std::string_view trial_arm_name(trial_arm arm) {
    return arm == trial_arm::treatment ? "treatment" : "baseline";
}

namespace detail {

// Did the run fail because the MEASUREMENT failed, rather than because of anything the agent did?
// Only an infrastructure fault (`failure_class::transient` -- a provider/transport error the caller
// could retry) or a host-initiated cancel qualifies. Every other run error -- the agent hitting its turn
// cap or token budget, or a malformed exchange that crashed the run -- is something the trial DID, so
// it is an outcome, and under intention-to-treat it counts against the arm it happened in. Defaulting
// unknown errors to "outcome" is deliberate: a lesson-induced crash must not be able to hide as
// missing data.
[[nodiscard]] inline bool is_measurement_fault(error const& e) {
    return e.klass == failure_class::transient || e.code == "run.canceled";
}

// Gross-harm screen red-team (both MAJOR findings share this root): "ungraded" used to mean "no grade
// for ANY reason" -- a trial that never converged, a grader that found no tool call, and a grader
// that threw were all the same missing datum. That was wrong in both directions:
//   * In the treatment arm, "never converged" and "never called the tool" are usually CAUSED by the
//     lesson -- exactly the harm being screened for. Counted as missing, enough of it tripped the
//     differential-missingness rule and turned a clear harm into `invalid` (no verdict): proven, a
//     lesson that made the model skip the tool in 2+ of 30 tasks was reported invalid, never flagged.
//     Since the candidate lesson is model-derived, that let a lesson's own content choose "no
//     verdict" over "flagged" (an I3 concern, not only a buggy-grader one).
//   * Every trial in BOTH arms ungraded made the two arms' ungraded rates equal, so the run counted
//     as valid, every diff was 0, and the screen said "no harm" from zero data.
// Now, intention-to-treat all the way down: a trial is `ungraded` only when the MEASUREMENT failed --
// the harness never ran the model (`setup_error`), the run hit a measurement fault (above), or the
// grader itself threw (§3.5: "a grader error ... is `ungraded`"). Any other failed run is `failure`. The
// grader is invoked only on converged trials.
[[nodiscard]] inline grade_outcome grade_trial(GraderFn const& grader, TrialResult const& trial) {
    if (trial.setup_error.has_value()) return grade_outcome::ungraded;
    if (!trial.outcome.has_value()) {
        return is_measurement_fault(trial.outcome.error()) ? grade_outcome::ungraded
                                                           : grade_outcome::failure;
    }
    try {
        return grader(trial);
    } catch (...) {
        return grade_outcome::ungraded;
    }
}

// Range checks written so a NaN FAILS them (red-team finding: `x <= 0.0 || x >= 1.0` is false for NaN,
// so a NaN alpha or threshold used to pass validation and then silently disable the comparison it fed).
[[nodiscard]] inline bool in_open_unit_interval(double x) { return x > 0.0 && x < 1.0; }
[[nodiscard]] inline bool in_closed_unit_interval(double x) { return x >= 0.0 && x <= 1.0; }

// Pre-flight check shared by both screens: a screen must bound its own per-trial model calls, or
// neither its call budget nor its retained memory is bounded at all. `AgentSession` only stops a
// non-converging loop when `max_turns` is set -- the red team ran a default-spec screen of TWO trials
// to 3,002 model calls before it ended on its own.
[[nodiscard]] inline std::optional<error> validate_call_budget(std::optional<std::uint64_t> max_turns,
                                                                  std::uint64_t trials,
                                                                  std::uint64_t max_model_calls) {
    if (!max_turns.has_value() || *max_turns == 0) {
        return error{failure_class::contract, "max_turns must be set and > 0 (it bounds every trial's model calls)",
                     "eval.screen_max_turns_required"};
    }
    if (*max_turns > max_model_calls / trials || trials * *max_turns > max_model_calls) {
        return error{failure_class::resource, "trials * max_turns exceeds max_model_calls",
                     "eval.screen_model_call_budget"};
    }
    return std::nullopt;
}

// Drops a trial's full request/response transcripts once it has been graded, unless the host opted in.
// Each `ChatCallRecording` carries the whole request history up to that call, so retained transcripts
// grow with trials x turns^2 -- measured at 3.2 GB for 1,000 tiny scripted trials at 50 turns. Grading
// reads `tool_calls`/`outcome`, and delivery was already computed inside `run_trial`, so nothing the
// screen reports depends on the transcripts.
inline void drop_transcripts_unless_retained(TrialResult& trial, bool retain) {
    if (!retain) std::vector<ChatCallRecording>{}.swap(trial.recordings);
}

// A simple, explicit, non-cryptographic mix (no reliance on `std::hash`'s implementation-defined
// behaviour) -- it only has to decorrelate sibling seeds, not resist an adversary.
[[nodiscard]] inline std::uint64_t mix_seed(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// Follow-rate screen round-1 red-team finding (MINOR, a latent design gap): forwarding one seed
// UNCHANGED into every trial's `TrialSpec::seed` is inert today (nothing consumes it stochastically),
// but the day something does, every trial in an arm becomes bit-for-bit correlated with every other,
// silently breaking the independent-trials assumption the screen statistics require -- and a scripted
// test client, which never reads the seed, could not catch it. Each trial therefore gets its own
// seed, derived from the screen's seed plus the trial's arm and a per-arm index (unique per arm),
// and recorded per trial (I5) so the derivation is auditable before anything consumes it.
[[nodiscard]] inline std::uint64_t derive_trial_seed(std::uint64_t base_seed, trial_arm arm,
                                                        std::uint64_t index) {
    return mix_seed(mix_seed(base_seed, arm == trial_arm::treatment ? 1u : 0u), index);
}

// A seed for one of a screen's own stochastic sub-computations (e.g. a permutation test), separated
// from its siblings by `tag`. The leading constant puts it on a different derivation path from
// `derive_trial_seed` (whose first mixed value is 0 or 1), so the two don't trivially share outputs
// for small tags -- decorrelation, not a no-collision guarantee.
[[nodiscard]] inline std::uint64_t derive_stream_seed(std::uint64_t base_seed, std::uint64_t tag) {
    return mix_seed(mix_seed(base_seed, 0x5354524541ull /* "STREA" */), tag);
}

}  // namespace detail
}  // namespace agentengine::eval
