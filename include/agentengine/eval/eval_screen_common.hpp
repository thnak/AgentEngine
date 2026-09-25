#pragma once
// Helpers shared by every ADR-195 Tier-1 screen that runs many `run_trial` calls: the follow-rate
// screen (eval_follow_rate_screen.hpp, §3.0 item 2) and the gross-harm regression screen
// (eval_gross_harm_screen.hpp, §3.0 item 3).

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/eval/eval_grader.hpp"
#include "agentengine/eval/eval_trial.hpp"

namespace agentengine::eval {

[[nodiscard]] inline std::string_view trial_arm_name(trial_arm arm) {
    return arm == trial_arm::treatment ? "treatment" : "baseline";
}

// What a screen tells its client factories about the trial they are building a client for. A real
// client's factory reads `trial_seed` (e.g. to pass as the provider's own sampling seed, so a trial's
// nondeterminism crosses the recorded seam, I5) and ignores the rest; a scripted TEST client, which has
// no context to read, uses `arm`/`task_index`/`attempt` to script its behaviour. Gross-harm round-2
// residual (closed): the factories used to receive only `(arm[, task_index])`, so the per-trial seed
// the screen derives and records could never reach anything that samples.
struct TrialSlot {  // ae-naming-lint: allow TrialSlot — ADR-195 §3.0 items 2-3
    trial_arm arm = trial_arm::baseline;
    std::size_t task_index = 0;   // always 0 in the follow-rate screen (one task)
    std::uint32_t attempt = 0;    // 0 for the first run, 1 for a retry after a transient fault
    std::string trial_id;         // identity only (I3), unique per attempt
    std::uint64_t trial_seed = 0; // the SAME for a retry: it re-runs the same trial
};

namespace detail {

// Did the run fail because the MEASUREMENT failed, rather than because of anything the agent did?
// Only an infrastructure fault (`failure_class::transient` -- a provider/transport error the caller
// could retry) or a host-initiated cancel qualifies. Every other run error -- the agent hitting its turn
// cap or token budget, or a malformed exchange that crashed the run -- is something the trial DID, so
// it is an outcome, and under intention-to-treat it counts against the arm it happened in. Defaulting
// unknown errors to "outcome" is deliberate: a lesson-induced crash must not be able to hide as
// missing data.
//
// Round-2 red-team finding (MAJOR): this classification alone does NOT keep the lesson out of the
// "ungraded" bucket. Every HTTP 429/5xx and the 90 s read timeout are `transient` whatever caused them,
// so a lesson that makes the model generate for longer, or send content a provider chokes on, reaches
// this class; so does a grader that throws on a malformed, model-chosen argument. No classification
// of a single error can tell "the provider failed" from "the lesson made the provider fail". The
// gross-harm screen therefore stops relying on it where it matters: when missingness makes a run
// uninterpretable, it re-tests under harm-favouring imputation (eval_gross_harm_screen.hpp), so
// lesson-induced missingness can only push that screen TOWARD a flag -- provided the rules see every
// fault, including one a retry recovered (`was_faulted`; PR #100 red team). The follow-rate screen needs no
// such rule -- there `invalid` means "no pass", already the conservative direction.
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
// grader is invoked only on converged trials. A grader should not throw on malformed model-chosen input
// (it reads model-supplied tool arguments) -- return `failure` instead; see is_measurement_fault above
// for why the gross-harm screen stays safe even when one does.
//
// A streamed call that ends without a clean terminal fails the run as `run.stream_incomplete`, which
// `AgentSession` deliberately labels `transient` whatever went wrong inside the stream -- including a
// provider's 400-class contract error such as a context overflow. Gross-harm round-2 residual (closed):
// were trials ever streamed, that label would turn lesson-caused contract failures into missing data.
// The provider's own error survives in the trial's recording (`ChatCallRecording::stream_error`,
// ADR-177), so it is what gets classified here; with no recorded error the session's label stands.
[[nodiscard]] inline error effective_run_error(TrialResult const& trial) {
    error const& e = trial.outcome.error();
    if (e.code == "run.stream_incomplete" && !trial.recordings.empty() &&
        trial.recordings.back().stream_error.has_value()) {
        return *trial.recordings.back().stream_error;
    }
    return e;
}

[[nodiscard]] inline grade_outcome grade_trial(GraderFn const& grader, TrialResult const& trial) {
    if (trial.setup_error.has_value()) return grade_outcome::ungraded;
    if (!trial.outcome.has_value()) {
        return is_measurement_fault(effective_run_error(trial)) ? grade_outcome::ungraded
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

// Model calls one trial can make per turn: one to the agent's model, and one to the memory summarizer
// (`MemoryProvider::on_turn_end` summarizes every turn). Round-2 red-team finding (MAJOR): the budget
// used to count only the first -- a spec at exactly `trials * max_turns == max_model_calls` made
// TWICE that many real model calls, measured (1,200 agent + 1,200 summarizer).
inline constexpr std::uint64_t kModelCallsPerTurn = 2;

// Pre-flight check shared by both screens: a screen must bound its own per-trial model calls, or
// neither its call budget nor its retained memory is bounded at all. `AgentSession` only stops a
// non-converging loop when `max_turns` is set -- the red team ran a default-spec screen of TWO trials
// to 3,002 model calls before it ended on its own. `retried_trials` is the screen's retry pool: each
// retry is one more full trial, so the budget covers the worst case of the pool fully spent.
[[nodiscard]] inline std::optional<error> validate_call_budget(std::optional<std::uint64_t> max_turns,
                                                                  std::uint64_t trials,
                                                                  std::uint64_t retried_trials,
                                                                  std::uint64_t max_model_calls) {
    if (!max_turns.has_value() || *max_turns == 0) {
        return error{failure_class::contract, "max_turns must be set and > 0 (it bounds every trial's model calls)",
                     "eval.screen_max_turns_required"};
    }
    if (retried_trials > std::numeric_limits<std::uint64_t>::max() - trials) {
        return error{failure_class::resource, "trials + retried trials overflows", "eval.screen_model_call_budget"};
    }
    trials += retried_trials;
    // Each guard runs before the product it protects, so no multiplication can wrap.
    std::uint64_t const per_trial_cap = max_model_calls / trials;
    if (*max_turns > per_trial_cap / kModelCallsPerTurn ||
        trials * kModelCallsPerTurn * *max_turns > max_model_calls) {
        return error{failure_class::resource,
                     "(trials + max_retried_trials) * max_turns * 2 (agent + summarizer call per turn) "
                     "exceeds max_model_calls",
                     "eval.screen_model_call_budget"};
    }
    return std::nullopt;
}

// Identifiers a screen builds trial_ids from must survive `mint_eval_trial_principal`, which rejects
// ':' (eval.id_colon). Round-2 red-team finding (MINOR): a task_id with a ':' passed pre-flight, then
// every one of that task's trials failed setup -- equally in both arms, so the run stayed "valid" and
// silently lost exactly the task the lesson broke. Rejected before any trial runs.
[[nodiscard]] inline bool usable_trial_id_part(std::string_view id) {
    return !id.empty() && id.find(':') == std::string_view::npos;
}

// §3.5's differential-missingness rule on COUNTS, not on floating-point rates. Round-2 red-team finding
// (MINOR): `|t/n - b/n| > bound` gave different verdicts for the same one-trial gap depending on the
// counts' own rounding (at n = 20, B=3/T=4 was invalid but B=0/T=1 was valid). Both arms have the same
// n in every screen, so the rule is "the counts differ by more than floor(bound * n) trials".
[[nodiscard]] inline bool differential_missingness_exceeds(std::uint64_t ungraded_a, std::uint64_t ungraded_b,
                                                             std::uint64_t n, double bound) {
    std::uint64_t const gap = ungraded_a > ungraded_b ? ungraded_a - ungraded_b : ungraded_b - ungraded_a;
    auto const allowed = static_cast<std::uint64_t>(std::floor(bound * static_cast<double>(n) + 1e-9));
    return gap > allowed;
}

// Drops a trial's full request/response transcripts once it has been graded, unless the host opted in.
// Each `ChatCallRecording` carries the whole request history up to that call, so retained transcripts
// grow with trials x turns^2 -- measured at 3.2 GB for 1,000 tiny scripted trials at 50 turns. Grading
// reads `tool_calls`/`outcome`, and delivery was already computed inside `run_trial`, so nothing the
// screen reports depends on the transcripts.
//
// What is kept without transcripts: every trial's id, seed, arm, grade, outcome and captured tool calls
// -- enough to re-grade a trial (graders read `tool_calls`/`outcome`) and to audit every count behind a
// verdict (I4). What is lost is replaying the model conversation (§3.8); a host that needs that sets
// `retain_recordings`.
inline void drop_transcripts_unless_retained(TrialResult& trial, bool retain) {
    if (retain) return;
    std::vector<ChatCallRecording>{}.swap(trial.recordings);
    std::vector<ChatCallRecording>{}.swap(trial.summarizer_recordings);
}

// Is this trial worth ONE retry? Only a provider/transport fault (`transient`) -- the one measurement
// failure that re-running can fix. A host cancel means stop; a setup error or a throwing grader would
// fail the same way again; an agent-caused failure (`resource`: its token budget; `contract`: its turn
// cap) is an OUTCOME and is never retried. A retry re-runs the same trial (same seed); its outcome is the
// trial's result, and a fault that recurs stays `ungraded`.
//
// PR #100 red team (MAJOR, found independently by two reviewers): retrying ERASED harm a lesson causes
// as faults. A lesson that makes 8% of its runs time out, nondeterministically, had those faults retried
// into successes -- the validity rules saw only the few that recurred, the run read as valid, and the
// screen passed it 92% of the time (9% without retries). A retry is a fresh draw, so it can only ever
// reduce noise; it must never decide which analysis runs. So a retried trial stays FAULTED for the
// validity rules and for harm-favouring imputation (`was_faulted` below), and only the ITT counts and the
// graded fraction use the retry's outcome.
[[nodiscard]] inline bool worth_a_retry(TrialResult const& trial) {
    return !trial.setup_error.has_value() && !trial.outcome.has_value() &&
           effective_run_error(trial).klass == failure_class::transient;
}

// Runs one attempt of a trial through the screen's factories. A throwing factory or `run_trial` becomes
// that attempt's setup error (`ungraded`), not the loss of every trial already run (round-2, MINOR).
template <class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<TrialResult> run_trial_guarded(InnerFactory& make_inner, SummarizerFactory& make_summarizer,
                                                  TrialSlot const& slot, TrialSpec spec) {
    TrialResult trial_result;
    try {
        trial_result = co_await run_trial(make_inner(slot), make_summarizer(slot), std::move(spec));
    } catch (...) {
        trial_result = TrialResult{};
        trial_result.setup_error =
            error{failure_class::fatal, "the trial threw instead of returning", "eval.screen_trial_threw"};
    }
    co_return trial_result;
}

// One trial with at most one retry from the screen's shared pool. Returns the attempt that counts and,
// if it was retried, the first attempt itself -- its tool calls, token counts and (if retained)
// transcripts, not just its error (PR #100 red team: the first version overwrote it, so a retried
// trial's first model calls, and the summarizer tokens they spent, appeared nowhere; I4, §3.8).
struct AttemptedTrial {
    TrialResult result;
    std::uint32_t attempts = 1;
    std::optional<error> retried_error;       // the first attempt's error, when retried
    std::optional<TrialResult> first_attempt; // the first attempt, when retried
    std::string counted_trial_id;             // the id `result` actually ran under (`-retry1` if retried)
};

// Did this trial's FIRST attempt fail to measure? True for a trial still `ungraded`, and for one whose
// first attempt faulted and was retried. The validity rules and harm-favouring imputation read this,
// never the post-retry grade (see `worth_a_retry`).
[[nodiscard]] inline bool was_faulted(std::uint32_t attempts, grade_outcome grade) {
    return attempts > 1 || grade == grade_outcome::ungraded;
}

template <class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<AttemptedTrial> run_trial_with_retry(InnerFactory& make_inner, SummarizerFactory& make_summarizer,
                                                        TrialSlot slot, TrialSpec spec,
                                                        std::uint64_t& retries_left) {
    AttemptedTrial out;
    spec.trial_id = slot.trial_id;
    out.counted_trial_id = slot.trial_id;
    out.result = co_await run_trial_guarded(make_inner, make_summarizer, slot, spec);
    if (retries_left > 0 && worth_a_retry(out.result)) {
        --retries_left;
        out.retried_error = effective_run_error(out.result);
        out.first_attempt = std::move(out.result);
        slot.attempt = 1;
        slot.trial_id += "-retry1";
        spec.trial_id = slot.trial_id;
        out.counted_trial_id = slot.trial_id;
        out.result = co_await run_trial_guarded(make_inner, make_summarizer, slot, std::move(spec));
        out.attempts = 2;
    }
    co_return out;
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
