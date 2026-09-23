#pragma once
// Helpers shared by every ADR-181 Tier-1 screen that runs many `run_trial` calls: the follow-rate
// screen (eval_follow_rate_screen.hpp, §3.0 item 2) and the gross-harm regression screen
// (eval_gross_harm_screen.hpp, §3.0 item 3). Moved here unchanged from the follow-rate screen once
// it had a second caller.

#include <cstdint>
#include <string_view>

#include "agentengine/eval/eval_grader.hpp"
#include "agentengine/eval/eval_trial.hpp"

namespace agentengine::eval {

[[nodiscard]] inline std::string_view trial_arm_name(trial_arm arm) {
    return arm == trial_arm::treatment ? "treatment" : "baseline";
}

namespace detail {

// Classifies a converged trial via the caller's grader, and a non-converged one as `ungraded`
// without ever invoking the grader (see eval_grader.hpp's own doc comment for why) -- and maps a
// throwing grader to `ungraded` too (§3.5: "a grader error... is `ungraded`").
[[nodiscard]] inline grade_outcome grade_trial(GraderFn const& grader, TrialResult const& trial) {
    if (trial.setup_error.has_value() || !trial.outcome.has_value()) return grade_outcome::ungraded;
    try {
        return grader(trial);
    } catch (...) {
        return grade_outcome::ungraded;
    }
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
