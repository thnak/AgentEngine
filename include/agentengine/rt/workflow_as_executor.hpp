#pragma once
// Adjacent to GitHub issue #35 (see docs/planning/workflow-as-executor-body-adapter-design-draft.md
// for the full design and an independent red-team pass this file's shape directly reflects): wraps a
// real, already-initialized rt::WorkflowSupervisor as a plain ExecutorBody, so a whole built Workflow
// can be reused as ONE ordinary participant inside ANOTHER workflow's builder -- "no graph node, no
// message-type wiring", mirroring MAF's workflow.as_agent() in spirit
// (python/samples/03-workflows/agents/sequential_workflow_as_agent.py, agent-framework clone, commit
// 4c0bff8), NOT issue #33's sub_workflow-as-graph-NODE mechanism. `agent_workflow_executor.hpp`
// (`agent_session_as_executor_body()`) is the nearest sibling this mirrors, for an
// `executor_kind::agent` node instead of an ordinary `function`-kind one.
//
// CONTRACT -- every call is an independent, fresh, complete run of the inner workflow from its own
// `start`: `WorkflowSupervisor::run_workflow()` unconditionally resets its own round/port state at
// the top of every call, so there is no cross-call memory here, by design -- MAF's own "one call in,
// one final output out" semantics, not `AgentSession`'s "accumulates history across rounds" contract.
//
// REQUEST_PORT REFUSED AT CONSTRUCTION -- both overloads below refuse (a real error, nothing built)
// any inner graph containing an `executor_kind::request_port` node. `ExecutorBody`'s contract is one
// synchronous call with no "still pending" concept, so a suspended inner run would otherwise have its
// open interaction silently and permanently discarded the next time `run_workflow()` resets `ports_`
// -- refusing the graph shape outright is what closes that hole, not documentation.
//
// CONCURRENCY -- the returned body owns its OWN `std::mutex`, independent of `WorkflowSupervisor`'s
// own `run_mutex_`. This is load-bearing, not decorative: `run_mutex_` is an `AsyncMutex` whose
// contended path parks the awaiting coroutine's own handle, to be resumed ONLY by the matching
// `unlock()`'s own trampoline -- a plain "resume until done" `drive()` loop (the only way to call a
// coroutine-returning `task<T>` from a synchronous `ExecutorBody`) has no way to honor that if a
// second delivery lands on this same wrapped node in the same outer round (an entirely ordinary
// topology for a `function`-kind node, which gets none of `execute()`'s agent-kind-only dedup).
// Without its own mutex, two concurrent calls would race on the SAME inner `WorkflowSupervisor`'s
// state and risk a double-resume of an already-destroyed coroutine frame. The mutex serializes
// concurrent deliveries safely instead -- each still gets its own fully independent fresh run, just
// one after another.
//
// CAPABILITY SOURCING (I2) -- the outer `EffectContext` this body receives is UNUSED, deliberately.
// The inner workflow's own executors run under whatever `EffectContext`s the caller already passed to
// `inner->initialize(..., contexts, ...)` before ever wrapping it -- entirely decoupled from the
// outer node's own capabilities. An embedded `Workflow` has N executors each with their own
// `capability_ceiling`; there is no natural 1:1 substitution the way
// `agent_session_as_executor_body()` has for a single `AgentSession`, and
// `check_workflow_executable()` never engine-enforces `capability_ceiling` for `function`-kind nodes
// anyway (only `agent`-kind) -- so "zero implicit capability flow across this boundary" is both the
// safest answer and consistent with what a `function`-kind node already means everywhere else in this
// codebase.
//
// ROUTING / I3 -- the returned `ExecutorOutcome` always carries empty `routes` (no routing concept,
// the same disclosure `agent_session_as_executor_body()` makes for its own equivalent seam); the
// inner run's output `Message`/`ContentItem` metadata (`origin`, `tainted`) passes through completely
// unchanged.
//
// TYPE-ERASURE -- this returns a plain `ExecutorBody` (`Message` in, `ExecutorOutcome` out). Nothing
// here or in `WorkflowBuilder::connect()`'s own `static_assert` checks that the outer
// `TypedExecutor<In,Out>` declared for the wrapping node actually matches what the inner workflow's
// own `start` consumes or `output_selection` produces -- exactly as unchecked as any hand-written
// `ExecutorBody` lambda already is; a mismatch simply misbehaves at the inner workflow's own start
// executor.
//
// LIFETIME (reference overload only) -- `inner` is captured BY REFERENCE and must outlive every call
// dispatched against the returned body, in practice meaning it must outlive the `WorkflowSupervisor`
// this body is eventually wired into. Two patterns that look reasonable but are NOT safe with this
// overload:
//   - A factory function building a local `WorkflowSupervisor`, wrapping it, and returning the
//     resulting `ExecutorBody`: the local is destroyed when the factory returns, and the returned
//     body now holds a dangling reference.
//   - `std::vector<WorkflowSupervisor> pool; pool.emplace_back(); auto body =
//     workflow_as_executor_body(pool.back());` followed by another `pool.emplace_back()`: the vector
//     may reallocate, invalidating every previously captured reference. (`WorkflowSupervisor` embeds
//     an `AsyncMutex`, which deletes its copy operations and thereby suppresses the implicit move
//     constructor too -- it cannot be "moved into stable storage" after the fact either.)
//   Prefer the `shared_ptr` overload below for either pattern -- it owns a real keep-alive and has
//   neither hazard. Use the reference overload only when `inner` is already guaranteed stable for
//   independent reasons (e.g. a local in the same scope that also owns and drives the outer
//   `WorkflowSupervisor` for the whole run).

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::rt {

namespace workflow_as_executor_detail {

// Drives an agentengine::rt::task<T> to completion from a plain, non-coroutine call site -- the SAME
// hand-rolled "resume until done" loop agent_workflow_executor.hpp and examples/10/19/20 already
// duplicate. Safe for WorkflowSupervisor::run_workflow() specifically because every existing
// example/test in this codebase already drives it this way from a plain main() -- safe here ONLY
// because the file banner's own `call_mutex` prevents two of these loops ever running concurrently
// against the same `inner`.
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] inline char const* status_tag(workflow_status s) {
    switch (s) {
        case workflow_status::completed:        return "completed";
        case workflow_status::suspended:        return "suspended";
        case workflow_status::bound_max_rounds: return "bound_max_rounds";
        case workflow_status::bound_deadline:   return "bound_deadline";
        case workflow_status::executor_failed:  return "executor_failed";
        case workflow_status::routing_failed:   return "routing_failed";
        case workflow_status::merge_conflict:   return "merge_conflict";
        case workflow_status::bound_max_stalls: return "bound_max_stalls";
        case workflow_status::bound_max_resets: return "bound_max_resets";
        case workflow_status::invalid:          return "invalid";
    }
    return "unknown";
}

// See file banner's REQUEST_PORT REFUSED AT CONSTRUCTION paragraph.
[[nodiscard]] inline agentengine::result<void> reject_request_ports(
    agentengine::workflow::Workflow const& graph) {
    for (agentengine::workflow::Executor const& e : graph.executors) {
        if (e.kind == agentengine::workflow::executor_kind::request_port) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "workflow_as_executor_body: the wrapped workflow contains a request_port executor "
                "('" + e.id + "') -- a suspended inner run has no way to surface its open interaction "
                "to the outer graph, and the next call would silently discard it when the inner "
                "supervisor's state resets. Nested HITL proxying is real, unbuilt future work; this "
                "graph shape is refused, not silently mishandled.",
                "rt.workflow_as_executor.request_port_unsupported"});
        }
    }
    return {};
}

// One full, fresh run of `inner` against `in`. See file banner's CONTRACT paragraph for why this is
// always a fresh run, and the REQUEST_PORT paragraph for why `suspended` can never occur here.
[[nodiscard]] inline agentengine::result<ExecutorOutcome> run_once(WorkflowSupervisor& inner,
                                                                     agentengine::Message const& in) {
    WorkflowResult r = drive(inner.run_workflow(RunWorkflow{in}));
    if (r.status == workflow_status::completed) return ExecutorOutcome{r.output};
    return std::unexpected(agentengine::error{
        agentengine::failure_class::contract,
        std::string("workflow_as_executor_body: the wrapped workflow did not complete (status=") +
            status_tag(r.status) + ")",
        std::string("rt.workflow_as_executor.inner_run_not_completed.") + status_tag(r.status)});
}

}  // namespace workflow_as_executor_detail

// PRIMARY surface -- see file banner's LIFETIME paragraph for why this, not the reference overload
// below, is the one to reach for by default. `inner` must already be initialize()d.
[[nodiscard]] inline agentengine::result<ExecutorBody> workflow_as_executor_body(
    std::shared_ptr<WorkflowSupervisor> inner) {
    if (!inner) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "workflow_as_executor_body: inner is null",
                                                    "rt.workflow_as_executor.null_supervisor"});
    }
    if (auto ok = workflow_as_executor_detail::reject_request_ports(inner->graph()); !ok) {
        return std::unexpected(ok.error());
    }
    auto call_mutex = std::make_shared<std::mutex>();
    return [inner = std::move(inner), call_mutex](
               agentengine::Message const& in,
               agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::lock_guard<std::mutex> guard(*call_mutex);
        return workflow_as_executor_detail::run_once(*inner, in);
    };
}

// Advanced/reference overload -- see file banner's LIFETIME paragraph. `inner` must already be
// initialize()d and must outlive every call dispatched against the returned body.
[[nodiscard]] inline agentengine::result<ExecutorBody> workflow_as_executor_body(
    WorkflowSupervisor& inner) {
    if (auto ok = workflow_as_executor_detail::reject_request_ports(inner.graph()); !ok) {
        return std::unexpected(ok.error());
    }
    auto call_mutex = std::make_shared<std::mutex>();
    return [&inner, call_mutex](agentengine::Message const& in,
                                 agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::lock_guard<std::mutex> guard(*call_mutex);
        return workflow_as_executor_detail::run_once(inner, in);
    };
}

}  // namespace agentengine::rt
