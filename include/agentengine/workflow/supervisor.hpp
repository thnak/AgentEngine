#pragma once
// Implements 014-Workflow-and-Orchestration.md §2 (execution semantics: the superstep model,
// determinism obligations, termination), §6 (failure), and §4 (human-in-the-loop: the request port).
// Milestone 6 Phases B, D, and E
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// `WorkflowSupervisor` is 014 §1's "the workflow is a supervising actor owning them" and 001 §1's
// actor-table row "Workflow -- a supervising actor owning a graph of executor actors". It is named
// `WorkflowSupervisor` rather than `Workflow` because 014 §1 already binds `Workflow` to the
// DESCRIPTION (`Workflow = { executors[], edges[], start, output_selection, policies }`,
// workflow/graph.hpp). Two RFCs use the same word for the data and for the actor; 014 owns the type,
// so the actor takes the qualified name. Recorded in 027 §4 rather than left to collide.
//
// THREE PROPERTIES THIS FILE EXISTS TO GUARANTEE, all from the breakdown's own decisions:
//
// (4) `Sequential`, and NOT by preference. 030 §4/§8 Q4 requires `pause_project` to `.passivate()`
//     any workflow-supervising actor, and 030 §7 G1 measures its activation count reaching zero.
//     `ActorRef<A>::passivate()` static_asserts `max_concurrency_of<A>() == 1` -- so a `Reentrant`
//     supervisor would not misbehave, it would NOT COMPILE, and G1 would be unprovable. It also
//     happens to be what §2's superstep model wants (one round at a time), but the two RFCs agree
//     only by coincidence. If you are here to make this `Reentrant` for throughput: that is what
//     this paragraph is for.
//
// (5) Fan-out is ISSUE-ALL-THEN-COLLECT, and that is what makes §2's determinism obligation true
//     rather than merely tested. Quark has no `when_all`/`join`/`gather` (verified, Phase A). So the
//     supervisor issues every ask for round N before awaiting any of them -- each executor begins
//     work immediately on its own actor, so concurrency is real -- then collects the futures in
//     FIXED INDEX ORDER. The round's result assembly therefore cannot depend on completion order
//     BY CONSTRUCTION. §8 G3's shuffle test then becomes a genuine positive control (it must still
//     catch a deliberately order-dependent executor body) rather than the only thing standing
//     between this design and a heisenbug.
//
// (11) A SUSPENDED RUN RETURNS; it does not park inside a handler. This is Phase E's structural
//     decision and it is forced, not stylistic. 014 §4 requires a suspended workflow to hold no
//     resources -- "it is checkpointed, its activations passivate". The obvious implementation of
//     "suspend until a response arrives" is to `co_await` the response inside the running handler,
//     but `ActorRef::passivate()` drains in-flight work and is explicitly "never a mid-handler
//     interrupt" (Quark ADR-034), so a run parked mid-handler would keep its activation alive and
//     §8 G5 would be unprovable -- the same shape as decision 4's compile error, one layer up.
//     So the run state lives in the ACTOR (`RunState` below), `RunWorkflow` replies `suspended`
//     with the open `Interaction`s, and `ResumeWorkflow` continues from exactly where it stopped.
//     Between the two the activation is idle and passivatable, which is what G5 measures.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/workflow/checkpoint.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

// Why a run ended. Distinguishing these is not cosmetic: 014 §2 lists three legitimate terminations
// (output selection, an explicit terminal executor, a bound) and a caller that cannot tell
// "finished" from "ran out of rounds" has no way to know whether to trust the output.
enum class workflow_status {
    completed,          // the graph ran dry -- every branch reached a terminal executor (§2)
    suspended,          // Phase E: a request port (§4) is waiting on a human. NOT a termination --
                        // `WorkflowResult::open_interactions` says what it is waiting for, and
                        // `ResumeWorkflow` continues the same run
    bound_max_rounds,   // §2's MaxRounds stopped it
    bound_deadline,     // §2's deadline stopped it
    executor_failed,    // an executor failed and its edges' declared policy (§6) was `fail`, or was
                        // `retry` with the budget spent. NOT every executor failure reaches here:
                        // `propagate` and `fallback` are handled ways to fail, and a run that used
                        // them completes. `WorkflowResult::failed_executor` names the one that ended
                        // the run
    routing_failed,     // Phase C: a switch/case node selected no declared edge, or selected more
                        // than one. Distinct from `executor_failed` because the executor SUCCEEDED
                        // -- what failed is the author's routing contract, and an operator reading
                        // "executor failed" would go looking in the wrong place
    invalid,            // the graph did not validate, or the supervisor was never initialized
};

struct RunWorkflow {
    Message input;  // the payload delivered to the start executor in round 0
};

// Milestone 6 Phase F (014 §5): continue a run whose state was restored via
// `restore_from_record()` from a durable checkpoint, on a FRESH actor instance -- the "resume
// restores exactly, on any node in a cluster" half of §5 for a run that was NOT suspended at a
// request port when checkpointed (that case is `ResumeWorkflow`, already public since Phase E; it
// works unmodified once `ports_`/`state_`/`run_id_`/`rounds_` are restored, since it does not care
// how the actor came to hold that state). `RunWorkflow` cannot serve this role -- it always starts
// a FRESH run (resets `state_`, mints a new `run_id_`), which is the wrong thing to do to a
// restored actor's already-populated state. No payload: the restored `state_.pending` already
// names what runs next.
struct ContinueWorkflow {};

// 014 §4: a request port "suspends the workflow until a response arrives". This is that response.
// Addressed by `interaction_id` rather than by executor id because §4's whole point is that several
// ports can be open at once -- an executor id would be ambiguous the moment a port sits inside a
// cycle and opens twice.
struct ResumeWorkflow {
    std::string interaction_id;
    Message     response;  // becomes the port executor's output, routed along its outgoing edges
    // The case labels the answer selects, for a port whose outgoing edges are `switch_case` --
    // approve/reject, which is the canonical human-in-the-loop shape and would be unbuildable
    // without this. It reuses `ExecuteReply::routes` verbatim, so the I3 boundary is the SAME one:
    // a label can only select among edges the graph already declares and Phase A already
    // type-checked. A human cannot name a node the author did not wire, any more than a classifier
    // can -- which is the property that lets both feed routing without holding authority.
    std::vector<std::string> routes;
};

// 014 §6: "Partial results are preserved -- a failed workflow's completed executor outputs are
// available in its final state, not discarded." One entry per executor that ever completed.
struct ExecutorOutput {
    std::string   executor_id;
    std::uint32_t round = 0;  // the round this output was produced in
    Message       payload;
};

struct WorkflowResult {
    workflow_status status = workflow_status::invalid;
    std::uint32_t   rounds = 0;   // rounds actually executed
    Message         output;       // the last output-selected payload, empty when none was produced
    // §6's third bullet. LAST output per executor, not the full history: keyed by id it is bounded
    // by the node count, whereas an append-only log would grow with rounds -- and a deadline-bounded
    // cyclic graph has no bound on those. The full per-round history is 014 §5's checkpoint record
    // (Phase F), which is the thing designed to be durable; this is the final state a caller reads.
    std::vector<ExecutorOutput> partial;
    // Which executor ended the run, for `executor_failed` and `routing_failed`. I4: a status alone
    // tells an operator what happened but not where, and "somewhere in your graph" is not
    // attribution.
    std::string failed_executor;
    // 014 §4 / OQ-4: "Multiple request ports open concurrently in different branches produce
    // multiple concurrent `Interaction` records on the same run -- the case that makes
    // `interaction_id` a SET rather than a singleton." A vector, therefore, and not an optional.
    // Non-empty exactly when `status == suspended`.
    std::vector<Interaction> open_interactions;
    // Neither §4 nor §6 states a priority between a request port and a `fail`-policy sibling
    // failure reached in the SAME round -- decision: the round's failure wins (consistent with
    // Phase D's "a round's failure ends the workflow"), so a port reached that round never opens
    // an Interaction. Named explicitly here (I4) rather than the port silently vanishing with no
    // trace: the ids of every request-port executor reached this round whose Interaction was never
    // minted because `executor_failed`/`routing_failed` ended the run first. Empty in every other
    // case, including a normal `suspended` result.
    std::vector<std::string> unopened_ports;
};

class WorkflowSupervisor : public quark::Actor<WorkflowSupervisor, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<RunWorkflow, WorkflowResult>,
                                     quark::Ask<ResumeWorkflow, WorkflowResult>,
                                     quark::Ask<ContinueWorkflow, WorkflowResult>>;

    // Milestone 6 Phase F: called once per superstep boundary (right after a round's results are
    // folded into `state_`/`ports_`, before the next round -- or a suspension -- begins), if set.
    // OPTIONAL and nullptr by default so every pre-Phase-F test's behaviour is unchanged. Takes
    // `to_record()`'s OWN OUTPUT rather than `Store`/`Activation`/`FenceToken` types directly: this
    // actor stays store-agnostic (I2 -- an explicitly injected hook, never ambient authority) and
    // does not need to become a template over a `Store` type, which would have forced every
    // existing Phase B-E test to grow a template parameter it has no use for. A host that wants
    // real durability closes over its own `Store&`/`Activation&`/`FenceToken` in the hook and calls
    // `save_workflow_checkpoint` (checkpoint.hpp) from inside it.
    using CheckpointHook = std::function<void(std::uint32_t round, RunStateRecord const&)>;
    void set_checkpoint_hook(CheckpointHook hook) { checkpoint_hook_ = std::move(hook); }

    // `refs` is parallel to `graph.executors` by INDEX. Index-addressed rather than
    // id-string-addressed because the index is what crosses the wire in `ExecuteRequest` (the
    // 192-byte pool cell), and because resolving a string per message per round would be real work
    // on the hot path for no benefit the graph description does not already provide.
    void initialize(Workflow graph, std::vector<quark::ActorRef<FunctionExecutor>> refs) {
        graph_ = std::move(graph);
        refs_  = std::move(refs);
        // Two questions, both required: is the graph well-formed, and can THIS build execute it
        // (`check_workflow_executable` -- see graph.hpp for why they are separate).
        valid_ = validate_workflow(graph_).has_value() &&
                 check_workflow_executable(graph_).has_value() &&
                 refs_.size() == graph_.executors.size();
    }

    [[nodiscard]] Workflow const& graph() const noexcept { return graph_; }
    [[nodiscard]] std::uint32_t   rounds_executed() const noexcept { return rounds_; }
    [[nodiscard]] std::string const& run_id() const noexcept { return run_id_; }
    // The run's open request ports (014 §4). Readable between asks precisely because a suspended run
    // is not parked inside a handler -- decision 11.
    // OPEN means still waiting. An answered port is dropped from this list the moment it is
    // answered, even though the run holds onto its response until the last port is in: a caller
    // reading "2 open" after answering one of two would have no way to tell which it still owes.
    [[nodiscard]] std::vector<Interaction> open_interactions() const {
        std::vector<Interaction> out;
        for (auto const& p : ports_) {
            if (!p.resolved) out.push_back(p.interaction);
        }
        return out;
    }

    quark::task<> handle(quark::Ask<RunWorkflow, WorkflowResult> const& m) {
        if (!valid_) {
            m.respond(WorkflowResult{workflow_status::invalid});
            co_return;
        }

        // A fresh run. `run_counter_` mirrors `AgentSession`'s own `<id>:run:<n>` convention rather
        // than inventing a second one -- 001 §2's correlation identity is one vocabulary.
        ++run_counter_;
        run_id_ = graph_.id + ":run:" + std::to_string(run_counter_);
        state_  = RunState{};
        ports_.clear();
        rounds_ = 0;
        state_.pending.push_back(Delivery{index_of(graph_.start), m.query.input});

        m.respond(co_await execute());
        co_return;
    }

    quark::task<> handle(quark::Ask<ResumeWorkflow, WorkflowResult> const& m) {
        if (!valid_) {
            m.respond(WorkflowResult{workflow_status::invalid});
            co_return;
        }

        // Fails closed on an unknown id rather than silently no-op'ing -- the precedent
        // `AgentSession::resolve_interaction` already set for exactly this call. A resume naming a
        // port that is not open is a caller mistake, and answering it with "suspended, still waiting"
        // would look identical to a lost response.
        OpenPort* port = nullptr;
        for (auto& p : ports_) {
            if (p.interaction.interaction_id == m.query.interaction_id) { port = &p; break; }
        }
        if (port == nullptr || port->resolved) {
            WorkflowResult r{workflow_status::invalid};
            r.rounds            = rounds_;
            r.open_interactions = open_interactions();
            m.respond(std::move(r));
            co_return;
        }

        port->resolved = true;
        port->response = m.query.response;
        port->routes   = m.query.routes;

        // 001 §2, and `AgentSession` implements the same sentence: "A run does not leave
        // InputRequired/Suspended for its 'waiting' reason until EVERY Interaction a given
        // resolution call names is resolved." With several ports open (§4/OQ-4's case), answering
        // one leaves the run suspended on the rest.
        for (auto const& p : ports_) {
            if (p.resolved) continue;
            WorkflowResult r{workflow_status::suspended};
            r.rounds            = rounds_;
            r.partial           = state_.partial;
            r.output            = state_.selected_output;
            r.open_interactions = open_interactions();
            m.respond(std::move(r));
            co_return;
        }

        m.respond(co_await execute());
        co_return;
    }

    // Milestone 6 Phase F: continue a restored (via `restore_from_record()`), non-suspended run.
    // `execute()` does not care how `state_.pending` came to be populated -- it is the SAME shared
    // run body `RunWorkflow`/`ResumeWorkflow` already use, per this file's own header note on why
    // that sharing matters (a resumed run must route by exactly the same rules as any other).
    quark::task<> handle(quark::Ask<ContinueWorkflow, WorkflowResult> const& m) {
        if (!valid_) {
            m.respond(WorkflowResult{workflow_status::invalid});
            co_return;
        }
        m.respond(co_await execute());
        co_return;
    }

    // Milestone 6 Phase F (014 §5): the run's durable projection -- mirrors
    // `AgentSession::to_record()`/`restore_from_record()` exactly (agent_session.hpp:613-648), the
    // only two places that cross between this actor's live fields and `RunStateRecord`
    // (checkpoint.hpp) so the field list can't drift between them silently. Does NOT cover `graph_`/
    // `refs_` -- 014 §5's "resume restores exactly, on any node in a cluster" presumes the graph
    // itself is redeployed by the host from the same description, the same relationship
    // `AgentSessionRecord` has to a session's static configuration.
    [[nodiscard]] RunStateRecord to_record() const {
        // Every `content_record.hpp` call below is explicitly qualified (`agentengine::to_record`),
        // not left unqualified: an unqualified `to_record(msg)` from INSIDE this member function of
        // the same name would find `WorkflowSupervisor::to_record()` (this function itself) via
        // ordinary unqualified lookup first -- which, per [basic.lookup.argdep], SUPPRESSES ADL
        // entirely once a class member of that name is found, regardless of the member's own
        // signature. Left unqualified this would be a hard compile error (wrong argument count
        // against the zero-arg member), not a silent recursive call.
        RunStateRecord rec;
        rec.run_counter      = run_counter_;
        rec.run_id           = run_id_;
        rec.rounds           = rounds_;
        rec.pending.reserve(state_.pending.size());
        for (auto const& d : state_.pending) {
            rec.pending.push_back(DeliveryRecord{static_cast<std::uint64_t>(d.executor_index),
                                                  agentengine::to_record(d.payload)});
        }
        rec.partial.reserve(state_.partial.size());
        for (auto const& p : state_.partial) {
            rec.partial.push_back(
                ExecutorOutputRecord{p.executor_id, p.round, agentengine::to_record(p.payload)});
        }
        rec.selected_output = agentengine::to_record(state_.selected_output);
        rec.failed_executor = state_.failed_executor;
        rec.unopened_ports  = state_.unopened_ports;
        rec.elapsed_ns      = state_.elapsed_ns;
        rec.ports.reserve(ports_.size());
        for (auto const& p : ports_) {
            rec.ports.push_back(OpenPortRecord{p.interaction, static_cast<std::uint64_t>(p.executor_index),
                                               agentengine::to_record(p.response), p.routes, p.resolved});
        }
        return rec;
    }

    void restore_from_record(RunStateRecord const& rec) {
        run_counter_ = rec.run_counter;
        run_id_      = rec.run_id;
        rounds_      = rec.rounds;
        state_       = RunState{};
        state_.pending.reserve(rec.pending.size());
        for (auto const& d : rec.pending) {
            state_.pending.push_back(Delivery{static_cast<std::size_t>(d.executor_index),
                                              from_record(d.payload)});
        }
        state_.partial.reserve(rec.partial.size());
        for (auto const& p : rec.partial) {
            state_.partial.push_back(ExecutorOutput{p.executor_id, p.round, from_record(p.payload)});
        }
        state_.selected_output = from_record(rec.selected_output);
        state_.failed_executor = rec.failed_executor;
        state_.unopened_ports  = rec.unopened_ports;
        state_.elapsed_ns      = rec.elapsed_ns;
        ports_.clear();
        ports_.reserve(rec.ports.size());
        for (auto const& p : rec.ports) {
            ports_.push_back(OpenPort{p.interaction, static_cast<std::size_t>(p.executor_index),
                                      from_record(p.response), p.routes, p.resolved});
        }
    }

private:
    // One round's work list: which executor, and what payload it receives. At class scope so the
    // helpers below can take it (a member type is in scope for every member function regardless of
    // declaration order).
    struct Delivery {
        std::size_t executor_index;
        Message     payload;
    };

    // Everything a run carries ACROSS a suspension (decision 11). A coroutine local would be lost
    // the moment the handler returns, which is exactly what a suspended run has to do.
    struct RunState {
        std::vector<Delivery>       pending;
        std::vector<ExecutorOutput> partial;
        Message                     selected_output;
        std::string                 failed_executor;
        // WorkflowResult::unopened_ports' own storage -- populated only when a round's failure
        // preempts that same round's port-opening (see that field's comment for why).
        std::vector<std::string> unopened_ports;
        // §2's deadline, accumulated over RUNNING time only. A workflow waiting on a human is not
        // spending its execution budget: charging human latency to a deadline the author set to
        // bound *computation* would kill every human-in-the-loop workflow that declares one, and
        // would make the bound mean something other than what §2 says it means.
        std::int64_t elapsed_ns = 0;
    };

    // An open request port (014 §4). Holds the `Interaction` -- 001 §2's correlation identity, the
    // one the run knows -- plus which executor it belongs to and, once answered, the response that
    // becomes that executor's output.
    struct OpenPort {
        Interaction              interaction;
        std::size_t              executor_index = 0;
        Message                  response;
        std::vector<std::string> routes;
        bool                     resolved = false;
    };

    enum class route_result { ok, routing_failed, workflow_failed };

    // The shared run body. Entered by `RunWorkflow` and re-entered by `ResumeWorkflow`; it cannot
    // tell which, because a resumed run is the SAME run continuing and any difference between the
    // two paths would be a difference §4 does not describe.
    quark::task<WorkflowResult> execute() {
        auto const entered_at = std::chrono::steady_clock::now();

        workflow_status status = workflow_status::completed;

        // Resuming: every answered port's response is that executor's output, routed by exactly the
        // rules any other output gets (switch/case, fan-in, the §6 failure policies). Routing a
        // port's response through a second, simpler path would mean a request port silently could
        // not be a router -- and nothing would say so.
        if (!ports_.empty()) {
            std::vector<Delivery> next = state_.pending;
            for (auto const& p : ports_) {
                ExecuteReply const reply{p.response, p.routes, true, failure_class::fatal};
                record_partial(state_.partial, p.executor_index, rounds_ - 1, p.response);
                if (is_output_selected(p.executor_index)) state_.selected_output = p.response;
                route_result const rr = route_from(p.executor_index, reply, next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[p.executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                            : workflow_status::executor_failed;
                ports_.clear();
                co_return finish(status, entered_at);
            }
            ports_.clear();
            state_.pending = std::move(next);
        }

        while (!state_.pending.empty()) {
            // Bounds are checked BEFORE the round runs, so a bound of N means at most N rounds
            // execute -- not N+1 with the last one discarded, which would still have spent the work
            // and any effects inside it.
            if (graph_.bound.max_rounds.has_value() && rounds_ >= *graph_.bound.max_rounds) {
                status = workflow_status::bound_max_rounds;
                break;
            }
            if (graph_.bound.deadline_ms.has_value()) {
                auto const elapsed = std::chrono::nanoseconds{state_.elapsed_ns} +
                                     (std::chrono::steady_clock::now() - entered_at);
                auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (static_cast<std::uint64_t>(ms) >= *graph_.bound.deadline_ms) {
                    status = workflow_status::bound_deadline;
                    break;
                }
            }

            // ---- 014 §4: a request port is NOT asked -----------------------------------------
            // It has no body to run; reaching it IS the event. Partitioned out before any ask is
            // issued so a port cannot be handed to `FunctionExecutor` by accident -- a port node's
            // registered actor must observe zero invocations, which the Phase E test asserts.
            std::vector<Delivery> exec_deliveries;
            std::vector<Delivery> port_deliveries;
            for (auto& d : state_.pending) {
                if (graph_.executors[d.executor_index].kind == executor_kind::request_port) {
                    port_deliveries.push_back(std::move(d));
                } else {
                    exec_deliveries.push_back(std::move(d));
                }
            }

            std::vector<ExecuteReply> replies(exec_deliveries.size());

            // `todo` is which deliveries still need an invocation. It starts as all of them and
            // shrinks to the retryable failures (014 §6's `retry`), so the attempt loop keeps the
            // whole round's concurrency shape: a retried node re-runs ALONGSIDE any other node
            // being retried, never one after another.
            std::vector<std::size_t> todo(exec_deliveries.size());
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) todo[i] = i;

            for (std::uint32_t attempt = 0; !todo.empty(); ++attempt) {
                // ---- decision 5, first half: ISSUE every ask before awaiting any ---------------
                // Each executor starts work the moment its ask is posted, so a round's nodes
                // genuinely overlap. Writing this as `co_await ref.ask(...)` inside the loop instead
                // would serialize the whole round and quietly turn 014 §3's Concurrent pattern into
                // Sequential -- the bug that would still pass a correctness-only test.
                std::vector<quark::AskFuture<ExecuteReply>> in_flight;
                in_flight.reserve(todo.size());
                for (std::size_t const i : todo) {
                    in_flight.push_back(
                        refs_[exec_deliveries[i].executor_index].template ask<ExecuteReply>(
                            ExecuteRequest{exec_deliveries[i].payload, rounds_}));
                }

                // ---- decision 5, second half: COLLECT in fixed index order --------------------
                // Completion order is whatever the scheduler produces; assembly order is this
                // loop's.
                std::vector<std::size_t> retry_next;
                for (std::size_t k = 0; k < in_flight.size(); ++k) {
                    std::size_t const           i = todo[k];
                    quark::result<ExecuteReply> r = co_await in_flight[k];
                    if (!r.has_value()) {
                        // ---- 014 §6's SECOND channel: the ACTOR failed ------------------------
                        // The handler threw, or the actor was stopped. Quark 007 has already run
                        // the executor's `OnFailure` directive (executor.hpp) and dead-lettered
                        // this ask rather than leaving it to hang -- so the workflow is still
                        // running and can still choose. Classified `transient`: the actor was
                        // restarted, so a further attempt meets a fresh instance rather than the
                        // one that just faulted, and Quark's own poison-loop detection bounds the
                        // pathological case.
                        replies[i] = ExecuteReply{Message{}, {}, false, failure_class::transient};
                    } else {
                        replies[i] = std::move(*r);
                    }
                    if (replies[i].ok) continue;

                    EdgeFailurePolicy const pol = policy_for(exec_deliveries[i].executor_index);
                    if (pol.kind == edge_failure_policy::retry && attempt < pol.attempts &&
                        is_retryable(replies[i].klass)) {
                        retry_next.push_back(i);
                    }
                }
                todo = std::move(retry_next);
            }

            ++rounds_;

            // 014 §1's output_selection: an executor named there contributes the workflow's result.
            // Read from THIS round's replies in index order, so a graph selecting two outputs gets
            // the later index, deterministically -- never whichever finished last.
            //
            // §6's partial results are recorded in the SAME pass and unconditionally: a completed
            // output is preserved whether or not the run goes on to fail, because at this point
            // nobody knows yet whether it will.
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                if (!replies[i].ok) continue;
                record_partial(state_.partial, exec_deliveries[i].executor_index, rounds_ - 1,
                               replies[i].payload);
                if (is_output_selected(exec_deliveries[i].executor_index)) {
                    state_.selected_output = replies[i].payload;
                }
            }

            // ---- the superstep barrier (014 §2) ------------------------------------------------
            // Round N+1's work list is not even CONSTRUCTED until every round-N reply is collected.
            // "All messages delivered in round n are processed before round n+1 begins" is therefore
            // a structural property of this loop, not a scheduling hope.
            std::vector<Delivery> next;
            bool                  broke = false;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                route_result const rr = route_from(exec_deliveries[i].executor_index, replies[i], next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[exec_deliveries[i].executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                            : workflow_status::executor_failed;
                broke = true;
                break;
            }
            if (broke) {
                // A sibling's `fail`-policy failure ends the run THIS round, before any port
                // reached the SAME round gets to open (see WorkflowResult::unopened_ports' own
                // comment for why this priority, not the reverse). Name what was lost rather than
                // let it vanish: the run is ending regardless, so there is no live Interaction to
                // mint, but an operator reading `executor_failed` should not have to infer that a
                // pending human question also went unrecorded.
                for (auto const& d : port_deliveries) {
                    state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                }
                break;
            }

            // ---- 014 §4: open one Interaction per port reached this round ---------------------
            // Minted AFTER the round's ordinary work, so the superstep completes normally and only
            // then does the run stop. Several ports reached in the same round open several
            // Interactions -- OQ-4's case, which is reachable precisely because §2's barrier keeps
            // fanned-out branches in step.
            for (auto const& d : port_deliveries) {
                ports_.push_back(OpenPort{mint_interaction(d.executor_index), d.executor_index,
                                          d.payload, {}, false});
            }
            state_.pending = std::move(next);

            // 014 §5: "Checkpoint at superstep boundaries." Right here -- round N's results are
            // fully folded into `state_`/`ports_`, round N+1 (or a suspension) has not started yet.
            // Fires whether or not this round is about to suspend, so a checkpoint taken here always
            // has enough to resume from either way (decision 11's RunState already carries both
            // shapes). See `CheckpointHook`'s own comment for why this is a caller-injected callback
            // rather than an ambient `Store` this actor would have to hold.
            if (checkpoint_hook_) checkpoint_hook_(rounds_, to_record());

            if (!ports_.empty()) {
                // The OTHER branches' next-round work stays in `state_.pending` untouched: §4 says a
                // request port "suspends the workflow", not the branch. Running the rest while a
                // human is being asked would be a different execution model than the one §4
                // describes, and would let a workflow commit effects that the pending answer was
                // supposed to gate.
                status = workflow_status::suspended;
                break;
            }
            // An empty work list means every branch reached an executor with no outgoing edges --
            // 014 §2's "an explicit terminal executor". `status` is already `completed`.
        }

        co_return finish(status, entered_at);
    }

    // Assemble the reply and bank this stretch of RUNNING time against the deadline.
    [[nodiscard]] WorkflowResult finish(workflow_status status,
                                        std::chrono::steady_clock::time_point entered_at) {
        state_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - entered_at)
                                 .count();
        WorkflowResult r{};
        r.status          = status;
        r.rounds          = rounds_;
        r.output          = state_.selected_output;
        r.partial         = state_.partial;
        r.failed_executor = state_.failed_executor;
        if (status == workflow_status::suspended) r.open_interactions = open_interactions();
        r.unopened_ports = state_.unopened_ports;
        return r;
    }

    // 014 §4: "The request port's `InputRequired` carries the same `request_id`-shaped correlation
    // token defined in 001 §2; a checkpoint taken while suspended stores the pending request indexed
    // by that token."
    //
    // DERIVED, not random. §4 wants a checkpoint indexed by this token and I5 wants nondeterminism
    // to cross a recorded seam -- a UUID here would be an unrecorded nondeterministic input on the
    // one identifier a resume has to match, so it is composed from run id, port, and round instead.
    // The round is in it because a port inside a cycle opens more than once, and two openings of the
    // same port are different requests.
    [[nodiscard]] Interaction mint_interaction(std::size_t executor_index) const {
        Interaction i{};
        // `rounds_ - 1` because the round this port was reached in has already been counted by the
        // time ports are opened; the token names the round the request belongs to, not the next one.
        i.interaction_id = run_id_ + ":port:" + graph_.executors[executor_index].id + ":" +
                           std::to_string(rounds_ == 0 ? 0 : rounds_ - 1);
        i.run_id = run_id_;
        i.reason = interaction_reason::input;
        // `opened_at_ns`/`expires_at_ns` stay 0: no wall-clock source is a wired capability anywhere
        // in this project yet, and `AgentSession::open_interaction` leaves them 0 for the same
        // reason. Named rather than filled with a steady_clock reading that would not survive a
        // process restart meaningfully.
        return i;
    }

    // Route one executor's result into the next round's work list. ONE function, used by the
    // ordinary path and by the resume path, so a request port's response routes by exactly the same
    // rules -- switch/case, fan-in, and §6's failure policies -- as any other output.
    [[nodiscard]] route_result route_from(std::size_t from_index, ExecuteReply const& reply,
                                          std::vector<Delivery>& next) const {
        std::string const& from_id = graph_.executors[from_index].id;

        // ---- 014 §6: the failed executor's edge policy decides ------------------------------
        if (!reply.ok) {
            EdgeFailurePolicy const pol    = policy_for(from_index);
            Message const           marker = failure_marker(from_id, reply.klass);

            switch (pol.kind) {
                case edge_failure_policy::fail:
                case edge_failure_policy::retry:
                    // A `retry` that reached here has spent its budget, or the failure was never
                    // retryable. §6 lists four ALTERNATIVES, so an exhausted retry resolves to the
                    // strict one rather than silently continuing; a graph that wants recovery after
                    // retries says so with a fallback, once §6 grows a composed form.
                    return route_result::workflow_failed;

                case edge_failure_policy::propagate:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from == from_id) deliver_once(next, index_of(edge.to), marker);
                    }
                    return route_result::ok;

                case edge_failure_policy::fallback:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from != from_id) continue;
                        if (edge.on_failure.kind != edge_failure_policy::fallback) continue;
                        deliver_once(next, index_of(edge.on_failure.fallback), marker);
                    }
                    return route_result::ok;
            }
            return route_result::workflow_failed;
        }

        // Phase C: 014 §1's `switch_case` is "exactly one selected". Counted rather than assumed,
        // because both failure directions are real authoring mistakes and both would otherwise be
        // silent -- zero fired ends the branch with no explanation, more than one turns a switch
        // into a fan-out.
        std::size_t switch_edges = 0;
        std::size_t switch_fired = 0;

        for (auto const& edge : graph_.edges) {
            if (edge.from != from_id) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_edges;
            if (!edge_fires(edge, reply)) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_fired;

            std::size_t const target = index_of(edge.to);

            // ---- fan-in (014 §2: "This makes fan-in well-defined") -------------------------
            // A `fan_in` edge MERGES into whatever delivery this round already has for the same
            // target, so an aggregator runs ONCE with every input rather than once per inbound edge.
            // Merging is by content-item append in source-index order, which is deterministic (003's
            // content model is an ordered list, and index order is fixed by the graph, not by
            // completion) -- the same property decision 5 relies on for the round's own assembly.
            //
            // Any other edge kind does NOT merge: two `direct` edges into one node are two
            // independent messages, and §2's superstep model says a node processes both. Silently
            // collapsing them would discard a message the author sent.
            if (edge.kind == edge_kind::fan_in) {
                bool merged = false;
                for (auto& delivery : next) {
                    if (delivery.executor_index != target) continue;
                    for (auto const& item : reply.payload.content) {
                        delivery.payload.content.push_back(item);
                    }
                    merged = true;
                    break;
                }
                if (merged) continue;
            }
            next.push_back(Delivery{target, reply.payload});
        }

        if (switch_edges > 0 && switch_fired != 1) return route_result::routing_failed;
        return route_result::ok;
    }

    // 014 §6: "An executor failure is CLASSIFIED (001 §6) and handled by the edge's declared
    // policy." This function is where the classification does work rather than being decoration.
    //
    // A `contract` failure is deterministic by definition -- a malformed request, a schema
    // violation -- so retrying it re-runs the same computation and gets the same answer, spending
    // the budget and any effects inside it to learn nothing. A `policy` failure is a DENIAL, and
    // retrying a denial is asking the same question until a different answer comes back: an I2
    // concern, not merely a waste. `fatal` is fatal.
    //
    // So retry applies to `transient` and `resource` only, and a `retry` policy on a contract- or
    // policy-failing executor invokes it exactly ONCE.
    [[nodiscard]] static bool is_retryable(failure_class klass) noexcept {
        return klass == failure_class::transient || klass == failure_class::resource;
    }

    // One failure event produces at most one delivery per distinct target, even when several edges
    // out of the failed node name the same one. The failure is a property of the NODE, not of each
    // edge -- unlike a successful output, where two `direct` edges to one target are genuinely two
    // messages the author sent.
    static void deliver_once(std::vector<Delivery>& next, std::size_t target, Message const& marker) {
        for (auto const& d : next) {
            if (d.executor_index == target) return;
        }
        next.push_back(Delivery{target, marker});
    }

    // §6's third bullet, last-write-wins per executor (see `WorkflowResult::partial` for why).
    void record_partial(std::vector<ExecutorOutput>& partial, std::size_t executor_index,
                        std::uint32_t round, Message const& payload) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto& out : partial) {
            if (out.executor_id != id) continue;
            out.round   = round;
            out.payload = payload;
            return;
        }
        partial.push_back(ExecutorOutput{id, round, payload});
    }

    // The failure policy governing an executor: the one its outgoing edges declare. Reading the
    // FIRST is safe precisely because `validate_workflow` rejects a graph whose edges disagree --
    // this function would otherwise be silently picking a winner. An executor with no outgoing edges
    // is terminal, so it has no edge to declare anything and gets the strict default.
    [[nodiscard]] EdgeFailurePolicy policy_for(std::size_t executor_index) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto const& edge : graph_.edges) {
            if (edge.from == id) return edge.on_failure;
        }
        return EdgeFailurePolicy{};
    }

    // Phase C. Unlabelled edge kinds always fire; labelled ones fire only when the source executor
    // named their label.
    //
    // The I3 boundary lives in this function's shape. A label is matched AGAINST THE GRAPH's own
    // declared edges -- an executor that returns a label no edge carries selects nothing, and one
    // that returns a node id rather than a label selects nothing either. There is no path from an
    // executor's output to a target the author did not already wire and Phase A did not already
    // type-check. That is what lets §3's Router row ("switch/case on a CLASSIFIER's typed output")
    // take model-derived output as a routing input without it becoming authority.
    [[nodiscard]] static bool edge_fires(Edge const& edge, ExecuteReply const& reply) noexcept {
        switch (edge.kind) {
            case edge_kind::direct:
            case edge_kind::chain:
            case edge_kind::fan_out:
            case edge_kind::fan_in:
                return true;
            case edge_kind::switch_case:
            case edge_kind::multi_selection:
                for (auto const& route : reply.routes) {
                    if (route == edge.case_label) return true;
                }
                return false;
        }
        return false;
    }

    [[nodiscard]] std::size_t index_of(std::string_view executor_id) const noexcept {
        for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
            if (graph_.executors[i].id == executor_id) return i;
        }
        return 0;  // unreachable for a validated graph: validate_workflow rejects unknown endpoints
    }

    [[nodiscard]] bool is_output_selected(std::size_t executor_index) const noexcept {
        for (auto const& sel : graph_.output_selection) {
            if (sel == graph_.executors[executor_index].id) return true;
        }
        return false;
    }

    Workflow                                       graph_;
    std::vector<quark::ActorRef<FunctionExecutor>> refs_;
    bool                                           valid_  = false;
    std::uint32_t                                  rounds_ = 0;
    std::uint64_t                                  run_counter_ = 0;
    std::string                                    run_id_;
    RunState                                       state_;
    std::vector<OpenPort>                          ports_;
    CheckpointHook                                 checkpoint_hook_;  // Phase F: nullptr by default
};

}  // namespace agentengine::workflow
