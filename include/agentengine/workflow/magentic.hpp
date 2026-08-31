#pragma once
// ADR-149: a Magentic-shaped workflow convenience API (GitHub issue #28, items 1/3). Design draft:
// docs/planning/magentic-workflow-convenience-api-design-draft.md.
//
// PURE SUGAR, NOT A NEW ENGINE PRIMITIVE (014-Workflow-and-Orchestration.md §3/§9 Q5):
// `MagenticWorkflowBuilder::build()` produces exactly the graph shape `examples/17_planner_live.cpp`
// already hand-builds against a real model -- a cyclic manager/participant graph with `switch_case`
// routing out of the manager and `direct` edges back in. Falls through to the SAME shared
// `validate_workflow()` (workflow/graph.hpp) every other `WorkflowBuilder` graph does; no new
// validation path.
//
// TWO TYPE PARAMETERS, NOT ONE -- a real defect in this design's first draft, caught by an
// independent red-team pass before any of this compiled (ADR-149 §3 findings 2/3/9): a single
// shared `<In,Out>` pair applied to BOTH `.manager()` and `.participant()` makes
// `WorkflowBuilder::connect`'s `static_assert(FromOut == ToIn)` require `In == Out` for the very
// FIRST edge, collapsing the builder to one degenerate type for the whole graph. The correct shape
// mirrors what a real Magentic graph's message flow actually is: the manager reads a `ReportMsg`
// (a participant's report, or the host's own initial input) and emits a `TaskMsg` (an assignment,
// or the final answer routed to "done"); every participant reads that `TaskMsg` and emits a
// `ReportMsg` back. So `.manager()` takes `TypedExecutor<ReportMsg, TaskMsg>` and `.participant()`
// takes `TypedExecutor<TaskMsg, ReportMsg>` -- REVERSED relative to each other, which is exactly
// what makes `manager->participant` (`TaskMsg == TaskMsg`) and `participant->manager`
// (`ReportMsg == ReportMsg`) both type-check under `connect`'s real rule. A `require_plan_signoff()`
// port is wired with the SAME `TypedExecutor<TaskMsg, ReportMsg>` shape as a participant -- it is
// structurally one, from the graph's point of view, just never dispatched to a real body (`execute()`
// never invokes a `request_port`-kind executor's body).
//
// LAYERING, deliberately preserved: this file stays "the graph as data" (graph.hpp's own file
// banner), never `agentengine::rt::`. `MagenticGraph::build()` hands back the `Workflow` description
// plus the manager/participant/done-sink/plan-review-port ids the caller needs to build their own
// `bodies` vector -- it never constructs an `ExecutorBody` itself (the done-sink node's identity body
// is the CALLER's to supply, same "bodies always caller-supplied, parallel by index" convention every
// other graph in this codebase already follows).
//
// The typed plan-signoff request/response types below mirror the tool Args/Reply idiom
// (`AE_JSON_SCHEMA`, core/json_schema.hpp) rather than inventing a new typed-payload mechanism: they
// ride inside an ordinary `Custom` `ContentItem` inside the graph's own `TaskMsg`/`ReportMsg`-typed
// `Message`, orthogonal to `MessageTypeId` (a validation-time tag, not a runtime payload-shape
// constraint -- ADR-149 §3 finding 3).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

// The builder's own output: the validated `Workflow` plus the ids the caller needs to build a
// `bodies` vector (`rt::WorkflowSupervisor::initialize()`'s parallel-by-index convention) and to
// wire `designated_stall_reporter` (typically `manager_id`) if stall/reset bounds are in use.
// ae-naming-lint: allow MagenticGraph — ADR-149's own convenience-API family, mirrors WorkflowBuilder's naming
struct MagenticGraph {
    Workflow                  graph;
    std::string                manager_id;
    std::vector<std::string>   participant_ids;
    std::string                done_sink_id;
    std::optional<std::string> plan_review_port_id;
};

// ae-naming-lint: allow MagenticWorkflowBuilder — ADR-149: the convenience authoring handle for 014 §3's Planner (Magentic) pattern, deliberately named to match MAF's own MagenticWorkflowBuilder (issue #28's own comparison)
//
// `TaskMsg` -- what the manager sends a participant (or routes to "done"/the plan-review port).
// `ReportMsg` -- what a participant sends back to the manager (or what the host's initial
// `RunWorkflow{input}` must itself be, since the manager's declared input type is `ReportMsg`).
template <class TaskMsg, class ReportMsg>
    requires DeclaredMessage<TaskMsg> && DeclaredMessage<ReportMsg>
class MagenticWorkflowBuilder {
public:
    explicit MagenticWorkflowBuilder(std::string workflow_id) : inner_(std::move(workflow_id)) {}

    MagenticWorkflowBuilder& manager(TypedExecutor<ReportMsg, TaskMsg> const& e) {
        manager_id_ = e.id;
        inner_.add(e);
        return *this;
    }

    MagenticWorkflowBuilder& participant(TypedExecutor<TaskMsg, ReportMsg> const& e) {
        participant_ids_.push_back(e.id);
        inner_.add(e);
        return *this;
    }

    // Both the synthetic "done" sink's executor id AND the `switch_case` label the manager's
    // `ExecutorOutcome::routes` must produce to reach it -- one string, matching
    // `examples/17_planner_live.cpp`'s own precedent of using the same spelling for both.
    MagenticWorkflowBuilder& done_selector(std::string label) {
        done_label_ = std::move(label);
        return *this;
    }

    MagenticWorkflowBuilder& max_stalls(std::uint32_t n) { inner_.max_stalls(n); return *this; }
    MagenticWorkflowBuilder& max_resets(std::uint32_t n) { inner_.max_resets(n); return *this; }
    MagenticWorkflowBuilder& max_rounds(std::uint32_t n) { inner_.max_rounds(n); return *this; }
    MagenticWorkflowBuilder& deadline_ms(std::uint64_t ms) { inner_.deadline_ms(ms); return *this; }
    MagenticWorkflowBuilder& token_budget(std::uint64_t tokens) { inner_.token_budget(tokens); return *this; }
    MagenticWorkflowBuilder& description(std::string text) { inner_.description(std::move(text)); return *this; }
    MagenticWorkflowBuilder& version(std::string v) { inner_.version(std::move(v)); return *this; }

    // ADR-149 §3 finding 3: wired EXACTLY like a participant -- same `TypedExecutor<TaskMsg,
    // ReportMsg>` shape, `switch_case` in from the manager, `direct` back out -- not a dangling
    // special node. The typed `MagenticPlanSignoffRequest`/`Response` payload (below) rides inside
    // that ordinary `Message` via `Custom` content, so no new message type or adapter node is needed
    // for this to type-check under `WorkflowBuilder::connect`'s static equality rule.
    MagenticWorkflowBuilder& require_plan_signoff(std::string port_id = "plan_review") {
        plan_review_port_id_ = std::move(port_id);
        return *this;
    }

    [[nodiscard]] result<MagenticGraph> build() const {
        if (manager_id_.empty()) {
            return std::unexpected(error{failure_class::contract,
                "MagenticWorkflowBuilder::build: manager() was never called",
                "workflow.magentic.no_manager"});
        }

        WorkflowBuilder wf = inner_;  // copy -- inner_ stays reusable, matching WorkflowBuilder::build()'s own constness
        std::string const done_id   = done_label_.empty() ? "done" : done_label_;
        std::string const done_case = done_id;

        // Identity sink: declared `<TaskMsg, TaskMsg>` since it only ever receives the manager's
        // TaskMsg-typed "done" edge and has no outgoing edges of its own (its own Out type is
        // otherwise unconstrained -- reusing TaskMsg needs no third type parameter for one unused slot).
        wf.add(TypedExecutor<TaskMsg, TaskMsg>{.id = done_id, .capability_ceiling = {}});

        for (std::string const& pid : participant_ids_) {
            wf.connect(TypedExecutor<ReportMsg, TaskMsg>{.id = manager_id_, .capability_ceiling = {}},
                       TypedExecutor<TaskMsg, ReportMsg>{.id = pid, .capability_ceiling = {}}, edge_kind::switch_case, pid);
            wf.connect(TypedExecutor<TaskMsg, ReportMsg>{.id = pid, .capability_ceiling = {}},
                       TypedExecutor<ReportMsg, TaskMsg>{.id = manager_id_, .capability_ceiling = {}}, edge_kind::direct);
        }

        wf.connect(TypedExecutor<ReportMsg, TaskMsg>{.id = manager_id_, .capability_ceiling = {}},
                   TypedExecutor<TaskMsg, TaskMsg>{.id = done_id, .capability_ceiling = {}}, edge_kind::switch_case, done_case);

        if (plan_review_port_id_.has_value()) {
            wf.add(TypedExecutor<TaskMsg, ReportMsg>{.id = *plan_review_port_id_,
                                                       .kind = executor_kind::request_port,
                                                       .capability_ceiling = {}});
            wf.connect(TypedExecutor<ReportMsg, TaskMsg>{.id = manager_id_, .capability_ceiling = {}},
                       TypedExecutor<TaskMsg, ReportMsg>{.id = *plan_review_port_id_, .capability_ceiling = {}},
                       edge_kind::switch_case, *plan_review_port_id_);
            wf.connect(TypedExecutor<TaskMsg, ReportMsg>{.id = *plan_review_port_id_, .capability_ceiling = {}},
                       TypedExecutor<ReportMsg, TaskMsg>{.id = manager_id_, .capability_ceiling = {}}, edge_kind::direct);
        }

        wf.start_at(manager_id_);
        wf.select_output(done_id);

        result<Workflow> built = wf.build();
        if (!built) return std::unexpected(built.error());

        MagenticGraph out;
        out.graph              = std::move(*built);
        out.manager_id          = manager_id_;
        out.participant_ids     = participant_ids_;
        out.done_sink_id        = done_id;
        out.plan_review_port_id = plan_review_port_id_;
        return out;
    }

private:
    WorkflowBuilder             inner_;
    std::string                  manager_id_;
    std::vector<std::string>     participant_ids_;
    std::string                  done_label_;
    std::optional<std::string>   plan_review_port_id_;
};

// -- Typed plan-signoff HITL (issue #28 item 3) -------------------------------------------------

struct MagenticPlanSignoffRequest {  // ae-naming-lint: allow MagenticPlanSignoffRequest — ADR-149's typed-HITL family, mirrors the tool Args/Reply naming idiom
    std::string plan;
};
AE_JSON_SCHEMA(MagenticPlanSignoffRequest, plan)

struct MagenticPlanSignoffResponse {  // ae-naming-lint: allow MagenticPlanSignoffResponse — ADR-149's typed-HITL family, mirrors the tool Args/Reply naming idiom
    bool        approved = false;
    std::string feedback;
};
AE_JSON_SCHEMA(MagenticPlanSignoffResponse, approved, feedback)

inline constexpr std::string_view kMagenticPlanSignoffTypeId = "agentengine.workflow.magentic.plan_signoff/1";

// Wraps `req` in a `Custom` ContentItem inside an ordinary `Message` -- send this as the payload of
// the `switch_case` edge that routes to a `require_plan_signoff()` port (the manager's own
// `ExecutorOutcome`, with `routes = {port_id}`).
[[nodiscard]] inline agentengine::Message make_plan_signoff_request(MagenticPlanSignoffRequest const& req) {
    agentengine::ContentItem item{};
    item.origin = agentengine::content_origin::assistant;
    item.value  = agentengine::Custom{std::string(kMagenticPlanSignoffTypeId),
                                        agentengine::json::dump(agentengine::schema::to_json(req))};
    agentengine::Message m{};
    m.role = agentengine::role::assistant;
    m.content.push_back(std::move(item));
    return m;
}

// Wraps `resp` for the human/host side of the round-trip -- the `response` a caller passes to
// `rt::ResumeWorkflow{interaction_id, response, routes}` once a human has reviewed the plan.
[[nodiscard]] inline agentengine::Message make_plan_signoff_response(MagenticPlanSignoffResponse const& resp) {
    agentengine::ContentItem item{};
    item.origin = agentengine::content_origin::user;
    item.value  = agentengine::Custom{std::string(kMagenticPlanSignoffTypeId),
                                        agentengine::json::dump(agentengine::schema::to_json(resp))};
    agentengine::Message m{};
    m.role = agentengine::role::user;
    m.content.push_back(std::move(item));
    return m;
}

// Reads a `Custom` ContentItem of the plan-signoff type back out of a resolved port's response
// Message (the `response` a caller passes to `rt::ResumeWorkflow`). A `contract`-class error if no
// matching Custom item is present.
[[nodiscard]] inline agentengine::result<MagenticPlanSignoffResponse> parse_plan_signoff_response(
    agentengine::Message const& m) {
    for (agentengine::ContentItem const& item : m.content) {
        auto const* custom = std::get_if<agentengine::Custom>(&item.value);
        if (custom == nullptr || custom->type_id != kMagenticPlanSignoffTypeId) continue;
        agentengine::result<agentengine::json::Value> parsed = agentengine::json::parse(custom->payload_json);
        if (!parsed) return std::unexpected(parsed.error());
        return agentengine::schema::from_json<MagenticPlanSignoffResponse>(*parsed);
    }
    return std::unexpected(agentengine::error{
        agentengine::failure_class::contract,
        "parse_plan_signoff_response: no MagenticPlanSignoffResponse Custom content item found",
        "workflow.magentic.plan_signoff.missing"});
}

}  // namespace agentengine::workflow
