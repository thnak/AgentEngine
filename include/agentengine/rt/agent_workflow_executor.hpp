#pragma once
// (Also implements ADR-193: an upstream agent's reply reaches the next agent node as a delegated task.)
// OQ-19 (OpenQuestions.md; docs/planning/agent-as-workflow-executor-design-draft.md, red-teamed
// twice before any code existed, per CLAUDE.md's design -> red-team -> prove -> judge discipline for
// contested/security-adjacent changes): the runtime bridge wrapping a real rt::AgentSession as an
// `executor_kind::agent` workflow node. `AgentExecutorBodyTag` itself (the structural marker
// `WorkflowSupervisor::initialize()` checks for) lives in rt/workflow_supervisor.hpp, not here --
// see that file's own comment on the type for why (avoiding a header cycle with this file, which
// needs both workflow_supervisor.hpp and the much heavier agent_session.hpp).
//
// CALLER CONTRACT -- "one AgentSession per workflow RUN" (design draft §1/§6, matching MAF's own
// `declareCrossRunShareable: false`): construct a FRESH AgentSession for each
// WorkflowSupervisor::run_workflow() call this body will ever be dispatched under, and never reuse
// one AgentSession instance across two different runs. `WorkflowSupervisor::initialize()` binds
// `bodies` ONCE and they persist across repeat run_workflow() calls on the same supervisor instance
// (nothing about this adapter enforces per-run freshness structurally -- it cannot, without owning
// the supervisor itself) -- a caller that wants a fresh session per run must re-initialize() a fresh
// WorkflowSupervisor with a fresh agent_session_as_executor_body(fresh_session) before every run.
// REUSE ACROSS ROUNDS WITHIN one run is intentional, not a bug: a cyclic node revisited several times
// in the same run accumulates real conversation history on the ONE AgentSession bound to it.
// LIFETIME: `session` is captured BY REFERENCE into the returned body -- the same "the caller's own
// stack/heap object outlives every call" discipline `examples/16_group_chat_live.cpp`'s `debater()`
// already establishes for `ExecutorBody` closures generally -- so `session` must outlive every
// `WorkflowSupervisor::run_workflow()`/`resume_workflow()`/`continue_workflow()` call dispatched
// against the graph this body is bound into, which in practice means outliving the
// `WorkflowSupervisor` instance itself.
//
// CHECKPOINT/RESUME -- a documented, tested limitation, not a design gap (design draft §5 item 1):
// `WorkflowSupervisor::restore_from_record()` never touches `bodies_` (caller-supplied fresh at
// `initialize()`, matched to `graph_.executors` purely by array index, with zero structural link to
// a checkpoint record -- confirmed against `tests/test_rt_workflow_checkpoint_g2.cpp`'s own "a
// genuinely NEW WorkflowSupervisor instance" pattern). So an agent-kind node's AgentSession on a
// resumed run is WHATEVER fresh session the caller binds via a fresh
// agent_session_as_executor_body() call -- its conversation history does NOT survive a checkpoint/
// resume cycle, the same already-accepted gap `AgentSessionRecord` itself names (no `history_`
// field; examples/12_session_checkpoint.cpp's own comment: "conversation history is NOT restored by
// this snapshot"). tests/test_rt_agent_workflow_executor.cpp proves this directly rather than leaving
// it merely asserted here.
//
// CONCURRENCY CONTRACT -- this body's own drive() loop (below) is safe ONLY because
// `WorkflowSupervisor::execute()` (rt/workflow_supervisor.hpp) never dispatches two concurrent
// deliveries to the SAME agent-kind executor within one round -- see that file's own
// "duplicate_delivery_same_round" quarantine, right where `exec_deliveries` is gathered.
// `AgentSession::start_run()`'s `session_mutex_` genuinely parks a contended waiter and resumes it
// from a DIFFERENT thread (a real cross-thread `AsyncMutex` wakeup, agent_session.hpp/
// async_mutex.hpp), which a naive "resume until done" drive loop cannot survive -- this is exactly
// why `rt::drive_leaf_task()` (rt/drive_leaf_task.hpp) is NOT used here: that function's own top
// comment names this precise hazard as the reason its `synchronous_leaf` contract excludes
// AgentSession. Do not call this adapter's returned callable directly, concurrently, against the
// same AgentSession from more than one thread outside WorkflowSupervisor's own guarantee.
//
// decisions/ADR-175 changed what the paragraph above depends on: the drive loop is now `rt::block_on()`, and
// a contended waiter is posted back to its own `block_on()` thread rather than resumed from the releasing
// thread, so a round that parks -- on `session_mutex_` or anything else -- is waited for correctly. The
// contract still matters for I1's own sake (two deliveries to one session in one round would queue behind
// each other on that session's mutex, holding two workers), not for memory safety.

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/delegation.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/tool_call_extraction.hpp"  // text_of
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

namespace agent_executor_detail {

// Drives an rt::task<T> to completion from a plain, non-coroutine call site. This was a hand-rolled
// `while (!t.done()) t.resume();` loop, argued safe under the CONCURRENCY CONTRACT above. The contract
// covers `session_mutex_` only: a round that contends anything else -- a shared `AsyncQuota` debited by a
// parallel node -- parks, and the loop resumed the parked handle a second time (decisions/ADR-175 §1).
// `block_on()` waits for the waker to hand the task back instead.
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    return agentengine::rt::block_on(std::move(t));  // ADR-175: was `while (!t.done()) t.resume();`
}

}  // namespace agent_executor_detail

// The adapter. Capability sourcing (design draft §5): the actual granted CapabilitySet this call
// runs `session` under comes from `ctx` -- whatever EffectContext
// `WorkflowSupervisor::initialize()`'s caller populated into `contexts[i]` for this node, the
// existing, previously-unused seam, not a parameter here. `check_workflow_executable(graph, contexts)`
// (workflow/graph.hpp) is what verifies `contexts[i]` actually satisfies this node's declared
// `Executor::capability_ceiling` before a graph carrying this body is ever accepted as runnable.
//
// ROUTING NOTE: `AgentResponse` has no routing concept, so the returned `ExecutorOutcome` always
// carries empty `routes` -- an agent-kind node reaches its outgoing edges via `direct`/`fan_out`/
// `fan_in` (which fire unconditionally) exactly like a plain function node; a `switch_case`/
// `multi_selection` edge OUT of an agent-kind node can never fire, since nothing here ever proposes a
// route label. A real "model output selects a route" capability is separate, not-yet-built work.
template <class ChatClientT, class StateT, class HistoryProviderT>
[[nodiscard]] AgentExecutorBodyTag agent_session_as_executor_body(
        AgentSession<ChatClientT, StateT, HistoryProviderT>& session) {
    return AgentExecutorBodyTag(
        [&session](agentengine::Message const& in,
                   agentengine::EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
            // Re-affirmed on every call (idempotent when `ctx.capabilities` is unchanged across a
            // run, the common case) rather than once at bind time, so a caller that mutates
            // `WorkflowSupervisor`'s own `contexts_[i]` between rounds (not done by any code in this
            // codebase today, but nothing here forbids it) is honored rather than silently stale.
            session.set_capabilities(ctx.capabilities.get());

            // ADR-152 (issue #29): forward this call's real RunEvents live -- model_delta,
            // tool_call_*, everything emit_run_event_for() fires -- through whatever
            // ctx.agent_turn_sink currently points at. Default no-op when no workflow event stream
            // is attached (WorkflowSupervisor only wires a real forwarding closure onto `ctx` when
            // enable_event_stream() has been called; see workflow_supervisor.hpp's
            // run_executor_job). Bracketed to exactly this one call via AgentSession's own
            // set_run_event_tap() (call-scoped, mirroring report_progress's ADR-060 discipline) --
            // reset with a genuinely EMPTY function (not another no-op lambda) immediately after,
            // so AgentSession::run_event_tap_attached_ correctly flips back to false and a second,
            // unrelated call into this same session (a cyclic node revisited later, or an app
            // calling start_run() directly outside any workflow) pays zero cost and never inherits
            // a closure captured by reference into this since-returned EffectContext.
            // ADR-193 round 2 (ADR-178): cancelling the workflow (`ctx.cancellation`, issue #37) cancels this node's
            // run too -- the node used to run to completion on its own fresh stop source. The flag is re-applied from
            // the tap because `start_run()` replaces the session's stop source (the same race
            // `run_child_agent_session` closes for agent.spawn).
            auto const canceled = std::make_shared<std::atomic<bool>>(false);
            session.set_run_event_tap([&ctx, &session, canceled](agentengine::RunEvent const& ev) {
                if (canceled->load(std::memory_order_acquire)) session.cancel();
                ctx.agent_turn_sink(ev);
            });
            std::stop_callback const cancel_bridge(ctx.cancellation, [&session, canceled] {
                canceled->store(true, std::memory_order_release);
                session.cancel();
            });

            // ADR-193: every part of the input another agent wrote (an upstream node's reply, or its items merged in
            // by a fan-in) reaches this agent as DELEGATED content -- tainted and external, with a host line when it
            // arrives as a request -- never as this agent's own assistant turn or as host-authored text. Decided per
            // item, not by the message's role (red team round 1). Host-authored input goes through unchanged.
            agentengine::Message const input = agentengine::delegate_foreign_items(
                in, agentengine::DelegationSource{"workflow node", "an upstream agent node", 1});
            agentengine::result<AgentResponse> driven =
                agent_executor_detail::drive(session.start_run(StartRun{input}));

            session.set_run_event_tap({});

            if (!driven) {
                // ADR-193 round 2: a failed run still spent what it spent (red team: a node that blew its budget
                // reported nothing, and the workflow's usage stayed 0). An error carries no usage, so it goes out
                // through `ctx.charge_delegated_usage`, which the supervisor's job binds to this node's reply.
                ctx.charge_delegated_usage(session.run_usage(), session.discarded_tokens_estimate());
                return std::unexpected(driven.error());
            }
            // GitHub issue #35 follow-up (ADR-163): `session.run_usage()` -- NOT `driven->usage` --
            // is the real per-call total. `AgentResponse::usage` (`driven->usage`) only ever reflects
            // the FINAL round's own model call (`agent_session.hpp:2357`'s own construction site,
            // `AgentResponse{response->message, response->usage, ...}` inside the "no more tool
            // calls" branch) -- for a multi-round tool-calling turn that under-counts every earlier
            // round's real cost. `run_usage()` is reset to zero at the top of EVERY `start_run()`
            // call (`agent_session.hpp`'s own `run_counter_ += 1; run_tokens_consumed_ = 0;` site,
            // mirrored for `run_usage_`), so reading it HERE, right after this one call, is already
            // exactly this call's own total with no delta needed.
            ExecutorOutcome outcome{driven->message};
            outcome.usage = session.run_usage();
            return outcome;
        });
}

}  // namespace agentengine::rt
