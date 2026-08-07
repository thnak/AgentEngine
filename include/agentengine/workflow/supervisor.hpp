#pragma once
// Implements 014-Workflow-and-Orchestration.md §2 (execution semantics: the superstep model,
// determinism obligations, termination). Milestone 6 Phase B
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// `WorkflowSupervisor` is 014 §1's "the workflow is a supervising actor owning them" and 001 §1's
// actor-table row "Workflow -- a supervising actor owning a graph of executor actors". It is named
// `WorkflowSupervisor` rather than `Workflow` because 014 §1 already binds `Workflow` to the
// DESCRIPTION (`Workflow = { executors[], edges[], start, output_selection, policies }`,
// workflow/graph.hpp). Two RFCs use the same word for the data and for the actor; 014 owns the type,
// so the actor takes the qualified name. Recorded in 027 §4 rather than left to collide.
//
// TWO PROPERTIES THIS FILE EXISTS TO GUARANTEE, both from the breakdown's own decisions:
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

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

// Why a run ended. Distinguishing these is not cosmetic: 014 §2 lists three legitimate terminations
// (output selection, an explicit terminal executor, a bound) and a caller that cannot tell
// "finished" from "ran out of rounds" has no way to know whether to trust the output.
enum class workflow_status {
    completed,          // the graph ran dry -- every branch reached a terminal executor (§2)
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
};

class WorkflowSupervisor : public quark::Actor<WorkflowSupervisor, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<RunWorkflow, WorkflowResult>>;

    // `refs` is parallel to `graph.executors` by INDEX. Index-addressed rather than
    // id-string-addressed because the index is what crosses the wire in `ExecuteRequest` (the
    // 192-byte pool cell), and because resolving a string per message per round would be real work
    // on the hot path for no benefit the graph description does not already provide.
    void initialize(Workflow graph, std::vector<quark::ActorRef<FunctionExecutor>> refs) {
        graph_ = std::move(graph);
        refs_  = std::move(refs);
        valid_ = validate_workflow(graph_).has_value() && refs_.size() == graph_.executors.size();
    }

    [[nodiscard]] Workflow const& graph() const noexcept { return graph_; }
    [[nodiscard]] std::uint32_t   rounds_executed() const noexcept { return rounds_; }

    quark::task<> handle(quark::Ask<RunWorkflow, WorkflowResult> const& m) {
        if (!valid_) {
            m.respond(WorkflowResult{workflow_status::invalid, 0, Message{}});
            co_return;
        }

        auto const started_at = std::chrono::steady_clock::now();

        std::vector<Delivery> pending;
        pending.push_back(Delivery{index_of(graph_.start), m.query.input});

        workflow_status             status = workflow_status::completed;
        Message                     selected_output;
        std::vector<ExecutorOutput> partial;
        std::string                 failed_executor;
        rounds_ = 0;

        while (!pending.empty()) {
            // Bounds are checked BEFORE the round runs, so a bound of N means at most N rounds
            // execute -- not N+1 with the last one discarded, which would still have spent the work
            // and any effects inside it.
            if (graph_.bound.max_rounds.has_value() && rounds_ >= *graph_.bound.max_rounds) {
                status = workflow_status::bound_max_rounds;
                break;
            }
            if (graph_.bound.deadline_ms.has_value()) {
                auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started_at)
                                         .count();
                if (static_cast<std::uint64_t>(elapsed) >= *graph_.bound.deadline_ms) {
                    status = workflow_status::bound_deadline;
                    break;
                }
            }

            // ---- decision 5, first half: ISSUE every ask before awaiting any -------------------
            // Each executor starts work the moment its ask is posted, so a round's nodes genuinely
            // overlap. Writing this as `co_await ref.ask(...)` inside the loop instead would
            // serialize the whole round and quietly turn 014 §3's Concurrent pattern into
            // Sequential -- the bug that would still pass a correctness-only test.
            std::vector<ExecuteReply> replies(pending.size());

            // `todo` is which deliveries still need an invocation. It starts as all of them and
            // shrinks to the retryable failures (014 §6's `retry`), so the attempt loop keeps the
            // whole round's concurrency shape: a retried node re-runs ALONGSIDE any other node
            // being retried, never one after another.
            std::vector<std::size_t> todo(pending.size());
            for (std::size_t i = 0; i < pending.size(); ++i) todo[i] = i;

            for (std::uint32_t attempt = 0; !todo.empty(); ++attempt) {
                std::vector<quark::AskFuture<ExecuteReply>> in_flight;
                in_flight.reserve(todo.size());
                for (std::size_t const i : todo) {
                    in_flight.push_back(
                        refs_[pending[i].executor_index].template ask<ExecuteReply>(
                            ExecuteRequest{pending[i].payload, rounds_}));
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

                    EdgeFailurePolicy const pol = policy_for(pending[i].executor_index);
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
            for (std::size_t i = 0; i < pending.size(); ++i) {
                if (!replies[i].ok) continue;
                record_partial(partial, pending[i].executor_index, rounds_ - 1, replies[i].payload);
                if (is_output_selected(pending[i].executor_index)) {
                    selected_output = replies[i].payload;
                }
            }

            // ---- the superstep barrier (014 §2) ------------------------------------------------
            // Round N+1's work list is not even CONSTRUCTED until every round-N reply is collected.
            // "All messages delivered in round n are processed before round n+1 begins" is therefore
            // a structural property of this loop, not a scheduling hope.
            std::vector<Delivery> next;
            bool routing_broke  = false;
            bool workflow_broke = false;
            for (std::size_t i = 0; i < pending.size(); ++i) {
                std::string const& from_id = graph_.executors[pending[i].executor_index].id;

                // ---- 014 §6: the failed executor's edge policy decides ------------------------
                if (!replies[i].ok) {
                    EdgeFailurePolicy const pol = policy_for(pending[i].executor_index);
                    Message const marker = failure_marker(from_id, replies[i].klass);

                    switch (pol.kind) {
                        case edge_failure_policy::fail:
                        case edge_failure_policy::retry:
                            // A `retry` that reached here has spent its budget, or the failure was
                            // never retryable. §6 lists four ALTERNATIVES, so an exhausted retry
                            // resolves to the strict one rather than silently continuing; a graph
                            // that wants recovery after retries says so with a fallback, once §6
                            // grows a composed form.
                            workflow_broke  = true;
                            failed_executor = from_id;
                            break;

                        case edge_failure_policy::propagate:
                            for (auto const& edge : graph_.edges) {
                                if (edge.from == from_id) deliver_once(next, index_of(edge.to), marker);
                            }
                            break;

                        case edge_failure_policy::fallback:
                            for (auto const& edge : graph_.edges) {
                                if (edge.from != from_id) continue;
                                if (edge.on_failure.kind != edge_failure_policy::fallback) continue;
                                deliver_once(next, index_of(edge.on_failure.fallback), marker);
                            }
                            break;
                    }
                    // A failed executor's NORMAL edges never fire: it produced no output, so there
                    // is nothing to carry along them. Under `propagate` the same targets are reached
                    // above -- but with the marker, so the target can tell the two apart.
                    continue;
                }

                // Phase C: 014 §1's `switch_case` is "exactly one selected". Counted rather than
                // assumed, because both failure directions are real authoring mistakes and both
                // would otherwise be silent -- zero fired ends the branch with no explanation, more
                // than one turns a switch into a fan-out.
                std::size_t switch_edges = 0;
                std::size_t switch_fired = 0;

                for (auto const& edge : graph_.edges) {
                    if (edge.from != from_id) continue;
                    if (edge.kind == edge_kind::switch_case) ++switch_edges;
                    if (!edge_fires(edge, replies[i])) continue;
                    if (edge.kind == edge_kind::switch_case) ++switch_fired;

                    std::size_t const target = index_of(edge.to);

                    // ---- fan-in (014 §2: "This makes fan-in well-defined") ---------------------
                    // A `fan_in` edge MERGES into whatever delivery this round already has for the
                    // same target, so an aggregator runs ONCE with every input rather than once per
                    // inbound edge. Merging is by content-item append in source-index order, which
                    // is deterministic (003's content model is an ordered list, and index order is
                    // fixed by the graph, not by completion) -- the same property decision 5 relies
                    // on for the round's own assembly.
                    //
                    // Any other edge kind does NOT merge: two `direct` edges into one node are two
                    // independent messages, and §2's superstep model says a node processes both.
                    // Silently collapsing them would discard a message the author sent.
                    if (edge.kind == edge_kind::fan_in) {
                        bool merged = false;
                        for (auto& delivery : next) {
                            if (delivery.executor_index != target) continue;
                            for (auto const& item : replies[i].payload.content) {
                                delivery.payload.content.push_back(item);
                            }
                            merged = true;
                            break;
                        }
                        if (merged) continue;
                    }
                    next.push_back(Delivery{target, replies[i].payload});
                }

                if (switch_edges > 0 && switch_fired != 1) {
                    routing_broke = true;
                }
            }

            if (workflow_broke) {
                status = workflow_status::executor_failed;
                break;
            }
            if (routing_broke) {
                status = workflow_status::routing_failed;
                if (failed_executor.empty()) {
                    for (std::size_t i = 0; i < pending.size(); ++i) {
                        if (!replies[i].ok) continue;
                        if (routing_broken_at(pending[i].executor_index, replies[i])) {
                            failed_executor = graph_.executors[pending[i].executor_index].id;
                            break;
                        }
                    }
                }
                break;
            }
            pending = std::move(next);
            // An empty work list means every branch reached an executor with no outgoing edges --
            // 014 §2's "an explicit terminal executor". `status` is already `completed`.
        }

        m.respond(WorkflowResult{status, rounds_, std::move(selected_output), std::move(partial),
                                 std::move(failed_executor)});
        co_return;
    }

private:
    // One round's work list: which executor, and what payload it receives. At class scope so the
    // cold-path helpers below can take it (a member type is in scope for every member function
    // regardless of declaration order).
    struct Delivery {
        std::size_t executor_index;
        Message     payload;
    };

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

    // Which executor's routing contract broke, for the `routing_failed` attribution above. Repeats
    // the counting rule rather than sharing state with the routing loop, because the loop's job is
    // to build the next round and this one runs only on the cold path.
    [[nodiscard]] bool routing_broken_at(std::size_t executor_index,
                                         ExecuteReply const& reply) const noexcept {
        std::string const& id = graph_.executors[executor_index].id;
        std::size_t        edges = 0;
        std::size_t        fired = 0;
        for (auto const& edge : graph_.edges) {
            if (edge.from != id || edge.kind != edge_kind::switch_case) continue;
            ++edges;
            if (edge_fires(edge, reply)) ++fired;
        }
        return edges > 0 && fired != 1;
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

    Workflow                                      graph_;
    std::vector<quark::ActorRef<FunctionExecutor>> refs_;
    bool                                          valid_  = false;
    std::uint32_t                                 rounds_ = 0;
};

}  // namespace agentengine::workflow
