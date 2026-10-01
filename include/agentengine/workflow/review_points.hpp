#pragma once
// Implements 014-Workflow-and-Orchestration.md §4 "Review points" (issue #45): a builder-level way to put a
// human review (a `request_port`) after a step, instead of hand-wiring one port and its edges per step.
// The patterns are 014 §3's Sequential (chain) and Concurrent (fan-out + fan-in with an aggregator).
//
// Modelled on Microsoft Agent Framework's `SequentialBuilder.with_request_info()` /
// `ConcurrentBuilder.with_request_info()` (agent-framework checkout:
// python/packages/orchestrations/agent_framework_orchestrations/_sequential.py, _concurrent.py,
// _orchestration_request_info.py; samples python/samples/03-workflows/human-in-the-loop/
// sequential_request_info.py, concurrent_request_info.py). MAF wraps each selected agent in an
// `AgentApprovalExecutor` (a nested workflow: agent <-> request-info executor). Here a review point is NOT a
// new kind of node: it is an ordinary `request_port` executor plus ordinary edges, spliced into the graph
// as data. So everything that already holds for request ports holds for review points -- one interaction
// per delivery (#157), answers checked before they are accepted (#155), admission (ADR-169), cancel
// (ADR-214), checkpoints -- with no new engine code.
//
// ONE EXPANSION, SHARED BY BOTH SURFACES (I6). `insert_review_points()` below is a plain function over the
// layer-1 `Workflow` (graph.hpp). The C++ pattern builders here call it, and the declarative compiler
// (yaml_compiler.hpp, `spec.review_points`) calls the SAME function -- so a YAML document and the C++
// builders cannot expand a review point differently. The expanded graph is also exactly what a hand-wired
// graph looks like (tests/workflow/test_workflow_review_points.cpp proves `==` against hand-built ones), so
// writing the ports and edges out by hand remains a fully equivalent declarative form.
//
// WHAT "REVIEW POINT AFTER STEP S" EXPANDS TO (the whole rule; nothing else changes):
//   1. A new executor `review_port_id(S)` == S + ".review": kind `request_port`, input and output type both
//      S's output type. APPENDED to `executors` (after every original executor, in the order the points
//      are given) -- so the caller's bodies vector (parallel by index, rt::WorkflowSupervisor::initialize())
//      keeps its indices and only needs one empty body appended per review port.
//   2. Every edge out of S now leaves from the review port instead (same target, kind, label; position in
//      `edges` unchanged), with the default failure policy -- a port runs no body, so it never fails.
//   3. A new `S -> review` direct edge, carrying S's own failure policy (014 §6: the policy is about S), is
//      inserted where S's first outgoing edge was (appended if S had none).
//   4. If S was in `output_selection`, the review port replaces it: the reviewed message is S's result.
//   With `revisable`, additionally:
//   5. S's one forward edge (review -> next) becomes the review port's `switch_default` (ADR-215), and a
//      `switch_case` edge review -> S labelled `kReviseRoute` ("revise") is inserted right after it.
//
// HOW A REVIEWER ANSWERS (014 §4's normal resume path, `ResumeWorkflow`):
//   - Approve: no routes. The response message is what flows on, so answering with the ask itself
//     (`WorkflowSupervisor::open_interaction_asks()`) passes S's output through unchanged, and answering with
//     an edited message amends it -- a review port, like every request port, forwards the answer.
//   - Revise (revisable points only): routes {"revise"}. The response goes back to S as S's next input
//     (S must therefore take its own output type), S runs again, and the review port opens again in a later
//     round with a new interaction id. The loop is bounded by the workflow's round bound (014 §2).
//   - Anything else is refused before it is accepted (#155, `invalid_routes`): on an approve-only point any
//     route is undeclared; on a revisable point only "revise" is declared.
//
// WHAT IS REFUSED (a `contract` error naming the step, never a silently different graph):
//   - an unknown step, or a step that is itself a request port (a review of a review);
//   - a step whose outgoing edges ROUTE (switch_case / multi_selection / switch_default): moving them behind
//     the port would let the reviewer's routes replace the step's own routing decision;
//   - a review port id already taken (also catches the same step selected twice);
//   - a step whose edges use the `fallback` policy with more than one recovery target (step 3 has one edge,
//     so it can carry only one);
//   - `revisable` on a step whose input and output types differ, or that does not have exactly one forward
//     edge of kind direct/chain. In particular a step that feeds a fan_in (the Concurrent pattern) cannot be
//     revisable: a fan_in edge always fires, so it cannot also be the "no case chosen" default; and a step
//     with NO successor (the last step of a chain) cannot be, because "approve" there would have no default
//     target to fire. Both still get approve/amend review points. This is narrower than MAF, whose nested
//     approval workflow can iterate anywhere; doing that here needs a construct the graph does not have
//     (a fan_in edge gated by a route), which is out of scope for issue #45.
//
// This header is "the graph as data" (graph.hpp's banner): no execution, no `rt::`.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

// The suffix a review port's id gets, and the one route a revisable review point declares.
inline constexpr std::string_view kReviewPortSuffix = ".review";
inline constexpr std::string_view kReviseRoute      = "revise";

[[nodiscard]] inline std::string review_port_id(std::string_view step_id) {
    std::string id(step_id);
    id += kReviewPortSuffix;
    return id;
}

// One requested review point: review the output of executor `after`. `revisable` adds the "revise" answer
// (see the banner for when that is allowed).
// ae-naming-lint: allow ReviewPoint — issue #45's builder-convenience concept (014 §4 "Review points"); 027 does not list it yet
struct ReviewPoint {
    std::string after;
    bool        revisable = false;

    friend bool operator==(ReviewPoint const&, ReviewPoint const&) = default;
};

// Splices the review points into `wf`, in order, per the banner's rule. Does NOT validate the result -- call
// `validate_workflow()` on it, exactly like the declarative compiler's output (so both surfaces report the
// same diagnostics for the rest of the graph). Only the review-specific refusals above are reported here.
[[nodiscard]] inline result<Workflow> insert_review_points(Workflow wf, std::vector<ReviewPoint> const& points) {
    auto fail = [](std::string message, std::string code) -> result<Workflow> {
        return std::unexpected(error{failure_class::contract, std::move(message), std::move(code)});
    };

    for (ReviewPoint const& point : points) {
        Executor const* step = wf.find(point.after);
        if (step == nullptr) {
            return fail("review point after undeclared executor '" + point.after + "'",
                        "workflow.review_unknown_step");
        }
        if (step->kind == executor_kind::request_port) {
            return fail("review point after '" + point.after +
                            "', which is itself a request port; a review point reviews a step's output",
                        "workflow.review_after_request_port");
        }
        std::string const rid = review_port_id(point.after);
        if (wf.find(rid) != nullptr) {
            return fail("review point after '" + point.after + "': executor id '" + rid +
                            "' is already taken (a step selected twice, or an existing executor of that name)",
                        "workflow.review_port_id_taken");
        }
        Executor const step_copy = *step;  // `wf.executors` grows below; never hold `step` across that

        std::vector<std::size_t> outgoing;
        for (std::size_t i = 0; i < wf.edges.size(); ++i) {
            if (wf.edges[i].from == point.after) outgoing.push_back(i);
        }

        EdgeFailurePolicy step_policy{};
        for (std::size_t const i : outgoing) {
            Edge const& e = wf.edges[i];
            if (e.kind == edge_kind::switch_case || e.kind == edge_kind::multi_selection ||
                e.kind == edge_kind::switch_default) {
                return fail("review point after '" + point.after +
                                "': its outgoing edges route (switch/case or multi-selection); a review port "
                                "in front of them would replace the step's own routing with the reviewer's",
                            "workflow.review_after_routing_source");
            }
        }
        if (!outgoing.empty()) {
            step_policy = wf.edges[outgoing.front()].on_failure;
            if (step_policy.kind == edge_failure_policy::fallback) {
                for (std::size_t const i : outgoing) {
                    if (wf.edges[i].on_failure.fallback != step_policy.fallback) {
                        return fail("review point after '" + point.after +
                                        "': its edges name more than one fallback executor, and the single "
                                        "step -> review edge can carry only one",
                                    "workflow.review_ambiguous_fallback");
                    }
                }
            }
        }

        if (point.revisable) {
            if (step_copy.input_type != step_copy.output_type) {
                return fail("revisable review point after '" + point.after +
                                "': a revision sends the reviewer's message back to the step as input, but "
                                "the step takes " + step_copy.input_type + " and emits " +
                                step_copy.output_type,
                            "workflow.review_revision_type_mismatch");
            }
            bool const one_forward = outgoing.size() == 1 && (wf.edges[outgoing.front()].kind == edge_kind::direct ||
                                                              wf.edges[outgoing.front()].kind == edge_kind::chain);
            if (!one_forward) {
                return fail("revisable review point after '" + point.after +
                                "': needs exactly one direct/chain successor to be the approve default (a "
                                "fan_in edge always fires and a last step has no successor) -- use an "
                                "approve-only review point there",
                            "workflow.review_revision_needs_single_successor");
            }
        }

        // (1) the port, appended.
        wf.executors.push_back(Executor{.id                 = rid,
                                        .kind               = executor_kind::request_port,
                                        .input_type         = step_copy.output_type,
                                        .output_type        = step_copy.output_type,
                                        .worktree_mode      = sharing_mode::branch,
                                        .capability_ceiling = {}});

        // (2) the step's edges now leave from the port.
        for (std::size_t const i : outgoing) {
            wf.edges[i].from       = rid;
            wf.edges[i].on_failure = EdgeFailurePolicy{};
            if (point.revisable) wf.edges[i].kind = edge_kind::switch_default;  // (5)
        }

        // (3) step -> port, where the step's first edge was.
        std::size_t const at = outgoing.empty() ? wf.edges.size() : outgoing.front();
        wf.edges.insert(wf.edges.begin() + static_cast<std::ptrdiff_t>(at),
                        Edge{point.after, rid, edge_kind::direct, {}, step_policy});

        // (5) port -> step on "revise", right after the (now default) forward edge at `at + 1`.
        if (point.revisable) {
            wf.edges.insert(wf.edges.begin() + static_cast<std::ptrdiff_t>(at + 2),
                            Edge{rid, point.after, edge_kind::switch_case, std::string(kReviseRoute), {}});
        }

        // (4) the reviewed message is the step's result.
        for (std::string& sel : wf.output_selection) {
            if (sel == point.after) sel = rid;
        }
    }
    return wf;
}

// -- The pattern builders (014 §3), with MAF's `with_request_info()` ------------------------------------

// Which participants get a review point. Empty `only` means every participant (MAF's default).
// ae-naming-lint: allow RequestInfoOptions — issue #45: the options of MAF's with_request_info(), mirrored by name
struct RequestInfoOptions {
    std::vector<std::string> only{};
    // Sequential only: let the reviewer send a step back for another pass (route "revise"). The LAST step's
    // review point stays approve/amend -- it has no successor to be the approve default (see the banner).
    bool allow_revision = false;
};

namespace review_points_detail {

[[nodiscard]] inline result<std::vector<ReviewPoint>> select_points(std::vector<std::string> const& participants,
                                                                    RequestInfoOptions const& opts,
                                                                    bool revisable_except_last) {
    for (std::string const& id : opts.only) {
        bool known = false;
        for (std::string const& p : participants) known = known || p == id;
        if (!known) {
            return std::unexpected(error{failure_class::contract,
                                         "with_request_info names '" + id + "', which is not a participant",
                                         "workflow.review_unknown_step"});
        }
    }
    std::vector<ReviewPoint> points;
    for (std::size_t i = 0; i < participants.size(); ++i) {
        bool selected = opts.only.empty();
        for (std::string const& id : opts.only) selected = selected || id == participants[i];
        if (!selected) continue;
        bool const last = i + 1 == participants.size();
        points.push_back(ReviewPoint{participants[i], revisable_except_last && !last});
    }
    return points;
}

[[nodiscard]] inline result<Workflow> finish(Workflow wf, std::vector<ReviewPoint> const& points) {
    auto expanded = insert_review_points(std::move(wf), points);
    if (!expanded) return expanded;
    auto valid = validate_workflow(*expanded);
    if (!valid) return std::unexpected(valid.error());
    return expanded;
}

}  // namespace review_points_detail

// 014 §3 Sequential: participants run in order, linked by `chain` edges; the first is the start, the last
// is the output. One message type end to end (MAF's sequential conversation), which is also what makes
// "revise" type-correct for every step. Bodies are the caller's, parallel by index: the participants in the
// order added, then one empty body per review port (in participant order).
template <class Msg>
    requires DeclaredMessage<Msg>
class SequentialWorkflowBuilder {  // ae-naming-lint: allow SequentialWorkflowBuilder — issue #45: 014 §3's Sequential pattern, named after MAF's SequentialBuilder
public:
    explicit SequentialWorkflowBuilder(std::string workflow_id) : inner_(std::move(workflow_id)) {}

    SequentialWorkflowBuilder& participant(TypedExecutor<Msg, Msg> const& step) {
        steps_.push_back(step);
        return *this;
    }

    SequentialWorkflowBuilder& with_request_info(RequestInfoOptions opts = {}) {
        request_info_ = std::move(opts);
        return *this;
    }

    SequentialWorkflowBuilder& description(std::string text) { inner_.description(std::move(text)); return *this; }
    SequentialWorkflowBuilder& max_rounds(std::uint32_t n) { inner_.max_rounds(n); return *this; }
    SequentialWorkflowBuilder& deadline_ms(std::uint64_t ms) { inner_.deadline_ms(ms); return *this; }

    [[nodiscard]] result<Workflow> build() const {
        if (steps_.empty()) {
            return std::unexpected(error{failure_class::contract, "a sequential workflow needs a participant",
                                         "workflow.no_executors"});
        }
        WorkflowBuilder wf = inner_;  // copy: the builder stays reusable
        for (auto const& s : steps_) wf.add(s);
        for (std::size_t i = 0; i + 1 < steps_.size(); ++i) wf.connect(steps_[i], steps_[i + 1], edge_kind::chain);
        wf.start_at(steps_.front().id).select_output(steps_.back().id);

        std::vector<ReviewPoint> points;
        if (request_info_.has_value()) {
            std::vector<std::string> ids;
            for (auto const& s : steps_) ids.push_back(s.id);
            auto selected = review_points_detail::select_points(ids, *request_info_, request_info_->allow_revision);
            if (!selected) return std::unexpected(selected.error());
            points = std::move(*selected);
        }
        return review_points_detail::finish(wf.described(), points);
    }

private:
    WorkflowBuilder                          inner_;
    std::vector<TypedExecutor<Msg, Msg>>     steps_;
    std::optional<RequestInfoOptions>        request_info_;
};

// 014 §3 Concurrent: a dispatcher fans the input out to every participant, and every participant fans in to
// the aggregator. The dispatcher is the start and the aggregator the output. Review points go after the
// selected participants, i.e. BEFORE aggregation: the fan_in barrier then waits on the review port (#62's
// barrier counts the port as the source), so the aggregator runs once, after every review is answered and
// every unreviewed participant has finished. Approve/amend only (see the banner for why not "revise").
// Bodies: dispatcher, participants in order, aggregator, then one empty body per review port.
template <class In, class Out, class Result = Out>
    requires DeclaredMessage<In> && DeclaredMessage<Out> && DeclaredMessage<Result>
class ConcurrentWorkflowBuilder {  // ae-naming-lint: allow ConcurrentWorkflowBuilder — issue #45: 014 §3's Concurrent pattern, named after MAF's ConcurrentBuilder
public:
    explicit ConcurrentWorkflowBuilder(std::string workflow_id) : inner_(std::move(workflow_id)) {}

    ConcurrentWorkflowBuilder& dispatcher(TypedExecutor<In, In> const& d) { dispatcher_ = d; return *this; }
    ConcurrentWorkflowBuilder& participant(TypedExecutor<In, Out> const& p) { participants_.push_back(p); return *this; }
    ConcurrentWorkflowBuilder& aggregator(TypedExecutor<Out, Result> const& a) { aggregator_ = a; return *this; }

    // `only` selects participants; empty means all. Approve/amend review points (no revision).
    ConcurrentWorkflowBuilder& with_request_info(std::vector<std::string> only = {}) {
        request_info_ = RequestInfoOptions{std::move(only), false};
        return *this;
    }

    ConcurrentWorkflowBuilder& description(std::string text) { inner_.description(std::move(text)); return *this; }
    ConcurrentWorkflowBuilder& max_rounds(std::uint32_t n) { inner_.max_rounds(n); return *this; }
    ConcurrentWorkflowBuilder& deadline_ms(std::uint64_t ms) { inner_.deadline_ms(ms); return *this; }

    [[nodiscard]] result<Workflow> build() const {
        if (!dispatcher_ || !aggregator_ || participants_.empty()) {
            return std::unexpected(error{failure_class::contract,
                                         "a concurrent workflow needs a dispatcher, an aggregator and a participant",
                                         "workflow.no_executors"});
        }
        WorkflowBuilder wf = inner_;
        wf.add(*dispatcher_);
        for (auto const& p : participants_) wf.add(p);
        wf.add(*aggregator_);
        for (auto const& p : participants_) wf.connect(*dispatcher_, p, edge_kind::fan_out);
        for (auto const& p : participants_) wf.connect(p, *aggregator_, edge_kind::fan_in);
        wf.start_at(dispatcher_->id).select_output(aggregator_->id);

        std::vector<ReviewPoint> points;
        if (request_info_.has_value()) {
            std::vector<std::string> ids;
            for (auto const& p : participants_) ids.push_back(p.id);
            auto selected = review_points_detail::select_points(ids, *request_info_, false);
            if (!selected) return std::unexpected(selected.error());
            points = std::move(*selected);
        }
        return review_points_detail::finish(wf.described(), points);
    }

private:
    WorkflowBuilder                           inner_;
    std::optional<TypedExecutor<In, In>>      dispatcher_;
    std::vector<TypedExecutor<In, Out>>       participants_;
    std::optional<TypedExecutor<Out, Result>> aggregator_;
    std::optional<RequestInfoOptions>         request_info_;
};

}  // namespace agentengine::workflow
