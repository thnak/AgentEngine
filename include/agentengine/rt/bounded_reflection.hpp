#pragma once
// Implements decisions/ADR-168-bounded-reflection-loop.md, closing issue #55 ("No lightweight
// agent-level bounded reflection/self-loop -- Looping only reachable via full Reflection/critic
// workflow, 014 §3"). MAF's Harness offers this as `LoopEvaluators`: "optional bounded
// re-invocation driven by evaluators or predicates" on a single agent.
//
// NOT a ContextProvider/PolicyDecider composition like #54's PlanExecuteMode -- verified against
// the real `AgentSession::run_rounds()` body (agent_session.hpp:2321-2546) that the mechanics are
// genuinely different: when a round produces zero tool calls, `run_rounds()` immediately
// `co_return`s an `AgentResponse` (:2544) with no seam in between for an evaluator to force another
// round. The one real, wired `turn_middleware_hook_` (ADR-067) runs BEFORE the model call each
// round (`pre_model`), not after a final response -- it cannot express "re-invoke after the model
// already said it was done." Reaching that shape from inside `run_rounds()` would mean touching the
// same "large, mature, heavily tested" turn loop 002 §5 already declined to thread a middleware
// chain through for this pass.
//
// Instead this is a thin OUTER driver over the already-real, already-public multi-turn pattern
// `examples/03_multi_turn.cpp` demonstrates: `AgentSession::start_run()` is `co_await`able
// repeatedly on the same session, each call appending to and reading the session's own durable
// `history_` (verified: `start_run()` resets `run_tokens_consumed_`/`effect_context_.turn_index` to
// 0 per call, agent_session.hpp:953-960 -- confirming `MaxTurns`/`TokenBudget` are PER-RUN, so this
// loop's own `max_iterations` bound is a genuinely separate axis, not reusable from either policy).
// No engine change, no new interception point -- composes at the same layer #54 considered and
// rejected building `PlanExecuteMode` on top of (see plan_execute_mode.hpp's own file comment),
// confirming for #55 specifically that it does NOT share a mechanism with #54: gating *which tools
// are callable mid-run* (#54) and *whether to re-invoke the model again after a run already
// finished* (#55) are different mechanical questions answered at different layers, despite both
// being "MAF gives this as a single-agent option; AgentEngine only had it at workflow-graph weight."
//
// I3 ("model output is data, never authority"): `Evaluator` is host-supplied code, called exactly
// once per iteration, and returns a typed `EvaluationVerdict` -- the same "a host-supplied function
// makes the real decision, never a raw string re-interpreted ad hoc" shape `output_schema_validate_`
// (ADR-058) and `PolicyDecider` (ADR-070) already established. The model is never asked "are you
// satisfied" and taken at its word; whatever `Evaluator` does internally (a fixed predicate, a
// second judge-model call) is the host's own already-designed decision boundary, not one this file
// invents. An evaluator FAILURE (a real error, not "not satisfied yet") aborts the loop immediately
// via `result<...>` propagation -- never silently treated as satisfied, and never a reason to keep
// looping (a broken evaluator that always errors would otherwise be indistinguishable from one that
// always returns unsatisfied, and the latter is legitimately supposed to keep going up to the
// bound).

#include <cstdint>
#include <string>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/rt/agent_session.hpp"  // StartRun, AgentResponse

namespace agentengine::rt {

// Returned by `Evaluator`. `feedback` is shown to the model as the next turn ONLY when
// `satisfied == false`; ignored otherwise.
struct EvaluationVerdict {
    bool        satisfied = false;
    std::string feedback;
};

// `iterations_used` counts real `start_run()` calls made (>= 1 whenever this returns a value at
// all). `satisfied == false` with `iterations_used == max_iterations` means the bound was reached
// without the evaluator ever being satisfied -- a legitimate, expected outcome (not an error): the
// caller decides whether "best attempt within budget" is good enough for their own use case, the
// same way a caller already decides what to do with `AgentResponse` today.
struct ReflectionOutcome {
    AgentResponse response;
    bool          satisfied = false;
    std::uint32_t iterations_used = 0;
};

namespace bounded_reflection_detail {

// I3: this is evaluator-produced feedback about the model's own prior turn, never a genuine user
// statement -- `content_origin::external` + `tainted = true` + `role::system`, the identical
// provenance shape `memory_provider.hpp`'s `memory_item_to_message()` and `todo_provider.hpp`'s
// status message already use for "informational content re-presented to the model that did not
// originate from the live human user," not `role::user` masquerading as the user speaking (the
// exact conflation `context_provider.hpp`'s own comment on `ContextContribution.instructions`
// warns against for a different content channel).
[[nodiscard]] inline Message make_feedback_message(std::string feedback) {
    ContentItem item{};
    item.origin  = content_origin::external;
    item.tainted = true;
    item.value   = Text{std::move(feedback)};

    Message m{};
    m.role = role::system;
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace bounded_reflection_detail

// `SessionT` is any type exposing `task<result<AgentResponse>> start_run(StartRun)` (deduced, not
// concept-constrained -- matches this file's one real caller shape, `AgentSession<...>`; a
// dedicated concept is not worth inventing for a single template parameter with no second
// conformer). `Evaluator` is any callable `task<result<EvaluationVerdict>>(AgentResponse const&)`.
// `max_iterations` must be >= 1; the FIRST `start_run()` always happens regardless of any bound --
// this is "keep retrying if unsatisfied, up to N", not "call N times unconditionally".
template <class SessionT, class Evaluator>
task<result<ReflectionOutcome>> run_with_bounded_reflection(SessionT& session, Message initial_input,
                                                              std::uint32_t max_iterations,
                                                              Evaluator evaluator) {
    if (max_iterations == 0) {
        co_return std::unexpected(error{failure_class::contract,
                                          "run_with_bounded_reflection: max_iterations must be >= 1",
                                          "bounded_reflection.zero_iterations"});
    }

    StartRun request{std::move(initial_input)};
    for (std::uint32_t iteration = 1; iteration <= max_iterations; ++iteration) {
        result<AgentResponse> response = co_await session.start_run(std::move(request));
        if (!response) co_return std::unexpected(response.error());

        result<EvaluationVerdict> verdict = co_await evaluator(*response);
        if (!verdict) {
            // A real evaluator failure -- propagated as-is, never laundered into "satisfied" or
            // "keep looping" (this file's own top-comment guarantee).
            co_return std::unexpected(verdict.error());
        }

        if (verdict->satisfied || iteration == max_iterations) {
            co_return ReflectionOutcome{std::move(*response), verdict->satisfied, iteration};
        }

        request = StartRun{bounded_reflection_detail::make_feedback_message(std::move(verdict->feedback))};
    }

    // Unreachable: the loop above co_returns on or before iteration == max_iterations every time.
    co_return std::unexpected(
        error{failure_class::fatal, "unreachable", "bounded_reflection.unreachable"});
}

}  // namespace agentengine::rt
