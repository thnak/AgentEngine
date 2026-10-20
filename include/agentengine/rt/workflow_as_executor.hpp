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
// any inner graph containing an `executor_kind::request_port` node. `ExecutorBody` is `task`-shaped
// (ADR-237 §5.1, Phase G) but has no "still pending" concept: a body that a caller awaits has nowhere
// to surface an interrupted inner state, so a suspended inner run would otherwise have its
// open interaction silently and permanently discarded the next time `run_workflow()` resets `ports_`
// -- refusing the graph shape outright is what closes that hole, not documentation.
//
// CONCURRENCY -- the returned body owns its OWN serialisation lock, independent of
// `WorkflowSupervisor`'s own `run_mutex_`. This is load-bearing, not decorative: `run_mutex_` is an
// `AsyncMutex` whose contended path parks the awaiting coroutine's own handle, to be resumed ONLY by
// the matching `unlock()`'s own trampoline, so two concurrent deliveries must never reach the same
// inner supervisor at once -- an entirely ordinary topology for a `function`-kind node, which gets
// none of `execute()`'s agent-kind-only dedup. Without its own lock, two concurrent calls would race
// on the SAME inner `WorkflowSupervisor`'s state and risk a double-resume of an already-destroyed
// coroutine frame. The lock serializes concurrent deliveries safely instead -- each still gets its own
// fully independent fresh run, just one after another.
// ADR-237 §5.1 (Phase G, §14 step 53): the body is a coroutine now, and the inner run it awaits can
// genuinely park, so this lock is an `rt::AsyncMutex` held across that await -- a `std::mutex` here
// would be held across a `co_await`, which ADR-237 forbids outright. Uncontended acquisition does not
// suspend (`AsyncMutex::await_ready` takes the free path), so a caller driving this body with a plain
// `drive()`/`block_on()` loop still works; only a genuinely contended call needs a real home.
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

#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/delegated_run_guard.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::rt {

namespace workflow_as_executor_detail {

// The `drive<T>` trampoline that used to live here is gone: decisions/ADR-237 §5.1 (Phase G) made the
// executor body `task`-shaped, so `run_once` below is a coroutine and awaits the inner run directly.

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
        // docs/planning/workflow-mid-run-cancellation-design-draft.md (issue #37, red-teamed): this
        // is a SECOND exhaustive switch(workflow_status) besides workflow_supervisor.hpp's own
        // finish() -- missed in that design draft's own first-pass file list, caught by an
        // independent red-team pass before implementation (this project builds under -Werror on
        // gcc/clang; an unhandled enumerator here would have broken that build).
        case workflow_status::cancelled:        return "cancelled";
        // ADR-169 (issue #65): same reason the `cancelled` case above names -- this is the SECOND
        // exhaustive switch(workflow_status) besides workflow_supervisor.hpp's own
        // `workflow_status_tag()`, and an unhandled enumerator breaks this project's -Werror build.
        // Unreachable in practice from `run_once()` below (that call passes no `caller`, and a
        // `workflow_as_executor_body`-wrapped inner is never given its own owner principal) --
        // handled anyway rather than left to the `return "unknown"` fallthrough, because "cannot
        // happen today" is not a property the compiler or a future caller is obliged to preserve.
        case workflow_status::admission_denied: return "admission_denied";
        // Issue #155: a refused resolve's status. Unreachable from `run_once()` (it never resumes) --
        // handled for the same exhaustive-switch reason as the two cases above.
        case workflow_status::invalid_routes:   return "invalid_routes";
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
//
// ADR-193 §9 (issue #113): the inner run's whole spend (`WorkflowResult::usage`) goes to the outer workflow exactly
// once -- in the outcome on success, through `ctx.charge_delegated_usage` (which the outer supervisor binds to this
// node's reply) on failure. This adapter used to report neither, so a nested workflow's model calls were invisible
// to the outer `usage()`.
[[nodiscard]] inline agentengine::task<agentengine::result<ExecutorOutcome>> run_once(
                                                                     WorkflowSupervisor& inner,
                                                                     agentengine::Message const& in,
                                                                     agentengine::EffectContext& ctx) {
    // ADR-235 (section 3.1, red-team finding 13): a wrapped run that suspends on a vendor batch would be reported as
    // a failure here and its paid jobs dropped by the next call's fresh run -- refused instead, on every call.
    if (inner.batch_coalescing_enabled()) {
        co_return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "workflow_as_executor_body: the wrapped workflow has batch coalescing enabled, which a nested "
            "workflow cannot be polled for",
            "rt.workflow_as_executor.batch_unsupported"});
    }
    // Issue #156 (ADR-214): the outer workflow's cancel (`ctx.cancellation`) is linked into the inner run, so a
    // wrapped workflow stops at its own next check instead of running to completion first.
    // ADR-237 §5.1 (Phase G): the inner run is awaited here -- this is a coroutine now, so no `drive()`.
    // ADR-193 §9 / issue #196: a throw from, or a drop (destruction while suspended) during, the inner run must still
    // charge what it spent, exactly once. `inner.usage()` is reset when a run STARTS, so before the run has started
    // it still holds the PREVIOUS run's total: the guard charges only if `run_id()` moved off the value read here
    // (the same test the agent adapter makes with `last_run_id()`). `run_workflow` bumps `run_id_` and resets
    // `total_usage_` together with no suspension between, and the guard runs after the inner run has unwound (throw)
    // or its frame was destroyed (drop, temporaries go before locals), so the read is not concurrent with the run.
    std::string const previous_run = inner.run_id();
    auto const charge_unfinished_run = [&inner, &ctx, &previous_run] {
        if (inner.run_id() == previous_run) return;
        ctx.charge_delegated_usage(inner.usage(), 0);
    };
    delegated_run_detail::ChargeOnUnwind charge_if_unfinished{charge_unfinished_run};
    WorkflowResult r = co_await inner.run_workflow(RunWorkflow{in, std::nullopt, ctx.cancellation});
    if (r.status == workflow_status::completed) {
        ExecutorOutcome outcome{r.output};
        outcome.usage = r.usage;
        // Disarmed only once the outcome (which carries the usage) is built: a throw from the copy above is charged.
        charge_if_unfinished.completed();
        co_return outcome;
    }
    charge_if_unfinished.completed();
    ctx.charge_delegated_usage(r.usage, 0);
    co_return std::unexpected(agentengine::error{
        inner_run_failure_class(r.status),  // ADR-237 D6: cancelled -> canceled, bound_deadline -> resource
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
    // ADR-237 §5.1 (Phase G): the body is `task`-shaped, so this is a coroutine. The mutual exclusion
    // the file banner documents must now survive a suspension, so it is an `rt::AsyncMutex` held across
    // the (possibly parking) inner run -- a `std::mutex` here would be held across a `co_await`, which
    // ADR-237 forbids outright.
    auto call_mutex = std::make_shared<AsyncMutex>();
    return [inner = std::move(inner), call_mutex](
               agentengine::Message const& in,
               agentengine::EffectContext& ctx) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        AsyncMutex::Guard guard = co_await call_mutex->lock();
        co_return co_await workflow_as_executor_detail::run_once(*inner, in, ctx);
    };
}

// Advanced/reference overload -- see file banner's LIFETIME paragraph. `inner` must already be
// initialize()d and must outlive every call dispatched against the returned body.
[[nodiscard]] inline agentengine::result<ExecutorBody> workflow_as_executor_body(
    WorkflowSupervisor& inner) {
    if (auto ok = workflow_as_executor_detail::reject_request_ports(inner.graph()); !ok) {
        return std::unexpected(ok.error());
    }
    // Same `AsyncMutex` reasoning as the shared_ptr overload above.
    auto call_mutex = std::make_shared<AsyncMutex>();
    return [&inner, call_mutex](agentengine::Message const& in,
                                 agentengine::EffectContext& ctx) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        AsyncMutex::Guard guard = co_await call_mutex->lock();
        co_return co_await workflow_as_executor_detail::run_once(inner, in, ctx);
    };
}

}  // namespace agentengine::rt
