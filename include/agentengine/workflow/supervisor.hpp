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
    executor_failed,    // an executor reported failure (§6's policies are Phase D; this is the
                        // honest Phase B behaviour: stop and say so, never continue silently)
    invalid,            // the graph did not validate, or the supervisor was never initialized
};

struct RunWorkflow {
    Message input;  // the payload delivered to the start executor in round 0
};

struct WorkflowResult {
    workflow_status status = workflow_status::invalid;
    std::uint32_t   rounds = 0;   // rounds actually executed
    Message         output;       // the last output-selected payload, empty when none was produced
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

        // One round's work list: which executor, and what payload it receives.
        struct Delivery {
            std::size_t executor_index;
            Message     payload;
        };

        std::vector<Delivery> pending;
        pending.push_back(Delivery{index_of(graph_.start), m.query.input});

        workflow_status status = workflow_status::completed;
        Message         selected_output;
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
            std::vector<quark::AskFuture<ExecuteReply>> in_flight;
            in_flight.reserve(pending.size());
            for (auto const& delivery : pending) {
                in_flight.push_back(
                    refs_[delivery.executor_index].template ask<ExecuteReply>(
                        ExecuteRequest{delivery.payload, rounds_}));
            }

            // ---- decision 5, second half: COLLECT in fixed index order ------------------------
            // Completion order is whatever the scheduler produces; assembly order is this loop's.
            std::vector<ExecuteReply> replies;
            replies.reserve(in_flight.size());
            bool round_failed = false;
            for (auto& future : in_flight) {
                quark::result<ExecuteReply> r = co_await future;
                if (!r.has_value()) {
                    round_failed = true;
                    replies.push_back(ExecuteReply{Message{}, false, failure_class::transient});
                    continue;
                }
                if (!r->ok) round_failed = true;
                replies.push_back(std::move(*r));
            }

            ++rounds_;

            // 014 §1's output_selection: an executor named there contributes the workflow's result.
            // Read from THIS round's replies in index order, so a graph selecting two outputs gets
            // the later index, deterministically -- never whichever finished last.
            for (std::size_t i = 0; i < pending.size(); ++i) {
                if (!replies[i].ok) continue;
                if (is_output_selected(pending[i].executor_index)) {
                    selected_output = replies[i].payload;
                }
            }

            if (round_failed) {
                // Phase B stops here and says so. 014 §6's per-edge policies (propagate/retry/
                // fallback/fail) are Phase D; inventing one now would be choosing a default the RFC
                // deliberately leaves to the edge.
                status = workflow_status::executor_failed;
                break;
            }

            // ---- the superstep barrier (014 §2) ------------------------------------------------
            // Round N+1's work list is not even CONSTRUCTED until every round-N reply is collected.
            // "All messages delivered in round n are processed before round n+1 begins" is therefore
            // a structural property of this loop, not a scheduling hope.
            std::vector<Delivery> next;
            for (std::size_t i = 0; i < pending.size(); ++i) {
                std::string const& from_id = graph_.executors[pending[i].executor_index].id;
                for (auto const& edge : graph_.edges) {
                    if (edge.from != from_id) continue;
                    next.push_back(Delivery{index_of(edge.to), replies[i].payload});
                }
            }
            pending = std::move(next);
            // An empty work list means every branch reached an executor with no outgoing edges --
            // 014 §2's "an explicit terminal executor". `status` is already `completed`.
        }

        m.respond(WorkflowResult{status, rounds_, std::move(selected_output)});
        co_return;
    }

private:
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
