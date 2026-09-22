#pragma once
// Implements ADR-181 §3.5's grader requirement ("Programmatic first, and structural. It matches on
// parsed tool-call arguments and the worktree/fixture state, never a substring search over
// free-text model output... A grader error or timeout is `ungraded`, counted and reported...
// Ungraded trials are never dropped silently.") -- nothing in the codebase has this concept yet;
// this is the minimal, reusable primitive the follow-rate screen (eval_follow_rate_screen.hpp)
// needs, general enough that a later gross-harm regression screen slice can reuse it unchanged.

#include <functional>
#include <string>

#include "agentengine/eval/eval_stub_tool.hpp"
#include "agentengine/eval/eval_trial.hpp"

namespace agentengine::eval {

// `ungraded` is a distinct third state, not folded into `failure`: a probe whose stub tool was
// never called at all means the model neither followed nor visibly rejected the lesson -- an
// honestly-reported "we don't know", per §3.5's own language.
enum class grade_outcome { success, failure, ungraded };  // ae-naming-lint: allow grade_outcome — ADR-181 §3.5

// Host-authored, non-hot-path (called tens of times per screen, matching ToolDescriptor::invoke's
// own std::function precedent, core/tool_descriptor.hpp) -- a template/CRTP parameter here would
// force every caller (the follow-rate screen driver, and a later regression-screen driver) to be
// templated on the grader type too, for no benefit.
//
// Contract (§3.5, I3): a grader must decide using STRUCTURAL signals only -- TrialResult::tool_calls
// (parsed arguments), never a substring search over TrialResult::recordings' free-text content. This
// is enforced by convention and this comment, not the type system -- the same disclosed limit
// eval_stub_tool.hpp's "safe because we write them" already accepts for stub tools: a grader author
// COULD read recordings and substring-match prose anyway, and nothing here stops that.
//
// A grader never sees a trial that failed to converge at all (setup_error set, or `outcome` holding
// an error such as max_turns exhausted) -- the driver that calls a grader classifies those as
// `grade_outcome::ungraded` BEFORE ever invoking the grader, and also maps a grader that itself
// throws to `ungraded` (§3.5: "a grader error... is `ungraded`"). Grading a trial that never
// converged is meaningless, and every grader author re-deriving "did the trial even run" would
// duplicate host logic unrelated to what any individual probe checks.
using GraderFn = std::function<grade_outcome(TrialResult const&)>;  // ae-naming-lint: allow GraderFn — ADR-181 §3.5

// Convenience factory: success iff some captured call to `tool_name` has a string-valued argument
// at `arg_key` equal to `expected_value`; failure iff the tool was called with a different value;
// ungraded iff the tool was never called at all. Deliberately the ONLY built-in grader shape this
// slice ships -- general enough for the follow-rate screen's own worked example (a probe like
// "set the deploy region"), narrow enough not to smuggle in task/suite-management scope.
[[nodiscard]] inline GraderFn make_tool_argument_grader(std::string tool_name, std::string arg_key,
                                                           std::string expected_value) {
    return [tool_name = std::move(tool_name), arg_key = std::move(arg_key),
            expected_value = std::move(expected_value)](TrialResult const& trial) -> grade_outcome {
        // Scans EVERY captured call to `tool_name`, not just the first -- a model that calls the
        // tool more than once (e.g. correcting an earlier, wrong argument) should be graded on
        // whether it EVER sent the expected value, not penalized for a call order this grader has
        // no stake in.
        bool called_at_all = false;
        for (CapturedCall const& call : trial.tool_calls) {
            if (call.tool_name != tool_name) continue;
            called_at_all = true;
            json::Value const* arg = call.arguments.find(arg_key);
            if (arg != nullptr && arg->is_string() && arg->as_string() == expected_value) {
                return grade_outcome::success;
            }
        }
        return called_at_all ? grade_outcome::failure : grade_outcome::ungraded;
    };
}

}  // namespace agentengine::eval
