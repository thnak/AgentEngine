// Proof for ADR-149 (GitHub issue #28 items 1/3): `agentengine::workflow::MagenticWorkflowBuilder`
// and `require_plan_signoff()` (include/agentengine/workflow/magentic.hpp).
//   B1 -- build() fails cleanly (workflow.magentic.no_manager) when manager() was never called.
//   B2 -- a well-formed builder produces the exact graph shape 014 §3's Planner pattern describes:
//         switch_case manager->{participants, done}, direct participant->manager, start=manager,
//         output_selection=[done] -- and it validates against the shared validate_workflow().
//   B3 -- require_plan_signoff() wires the port EXACTLY like a participant (ADR-149 §3 finding 3),
//         not a dangling node -- proven by inspecting the produced edges, not just trusting the code.
//   B4 -- a done_selector() colliding with a real participant id is caught by the SAME
//         workflow.duplicate_executor_id validation any hand-built graph would hit (ADR-149 §3
//         finding 10, an accepted residual, not silently swallowed).
//   B5 -- END TO END: the produced graph, using TWO GENUINELY DISTINCT declared message types
//         (TaskMsg/ReportMsg -- ADR-149 §3 finding 9's fix for the type-parameterization defect an
//         earlier draft of this file's own source had), actually drives to completion through a
//         real rt::WorkflowSupervisor -- not just "looks right" as data.
//   B6 -- the builder's max_stalls()/max_resets() forwarding composes correctly with
//         WorkflowSupervisor's designated_stall_reporter enforcement (ADR-149 item 2), proven on
//         the SAME builder-produced graph, not a separately hand-built one.

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/magentic.hpp"

using agentengine::result;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::MagenticGraph;
using agentengine::workflow::MagenticWorkflowBuilder;
using agentengine::workflow::TypedExecutor;
using agentengine::workflow::validate_workflow;

using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Same safety argument as test_rt_workflow_supervisor.cpp's own drive(): every graph below has no
// genuinely suspending co_await in its executor bodies.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::role;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

[[nodiscard]] bool has_edge(std::vector<agentengine::workflow::Edge> const& edges, std::string const& from,
                             std::string const& to, edge_kind kind, std::string const& case_label = {}) {
    for (auto const& e : edges) {
        if (e.from == from && e.to == to && e.kind == kind && e.case_label == case_label) return true;
    }
    return false;
}

}  // namespace

// Two GENUINELY DISTINCT declared message types -- see ADR-149 §3 finding 9. TaskMsg: what the
// manager sends a participant. ReportMsg: what a participant sends back.
//
// NAMED, AND STRING-DECLARED, IDENTICALLY across this file, test_workflow_magentic_plan_signoff.cpp,
// and examples/19_magentic_builder_live.cpp -- deliberately, not by oversight. A post-implementation
// audit flagged an EARLIER version of this (same type names, but a DIFFERENT AE_WORKFLOW_MESSAGE
// string per file) as a latent ODR violation: three non-identical explicit specializations of
// `agentengine::workflow::message_type<TaskMsg>` for what a linker would see as the SAME entity, had
// these TUs ever been linked together. An anonymous-namespace fix (giving each TU's own `TaskMsg` an
// internal-linkage, genuinely distinct type) does NOT compile here: an explicit specialization of
// `agentengine::workflow::message_type<T>` must be declared in a namespace enclosing
// `agentengine::workflow`, and an anonymous namespace nested in the global namespace is not one --
// MSVC (correctly) rejects it with C2888. The fix actually used instead matches this codebase's own
// existing precedent (`Question`/`Draft`/`Verdict`, reused verbatim -- same name, same string -- at
// file scope across `test_workflow_graph_validation.cpp` and the `compile_fail/workflow_edge_type_*`
// pair): IDENTICAL definitions of the same entity across multiple TUs are NOT an ODR violation, only
// DIFFERING ones are -- so unifying the string closes the risk exactly the way the pre-existing
// pattern already safely does, without needing anything structurally new.
struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Magentic.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Magentic.ReportMsg");

int main() {
    // ---- B1: no manager() -> a clean, named error, not a garbage graph ---------------------------
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("no-manager");
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
        result<MagenticGraph> built = b.build();
        check(!built.has_value(), "B1: build() fails when manager() was never called");
        if (!built) {
            check(built.error().code == "workflow.magentic.no_manager", "B1: the specific error code");
        }
    }

    // ---- B2: the produced graph shape -------------------------------------------------------------
    MagenticGraph shape;
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("shape-test");
        b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p2", .capability_ceiling = {}});
        b.max_rounds(20);
        result<MagenticGraph> built = b.build();
        check(built.has_value(), "B2: a well-formed manager+participants builder builds");
        if (built) {
            shape = std::move(*built);
            check(validate_workflow(shape.graph).has_value(),
                  "B2: the produced graph validates against the shared validate_workflow()");
            check(shape.graph.executors.size() == 4, "B2: mgr + p1 + p2 + the synthetic done sink");
            check(shape.manager_id == "mgr", "B2: manager_id is reported back");
            check(shape.participant_ids.size() == 2, "B2: both participant ids are reported back");
            check(shape.done_sink_id == "done", "B2: the default done sink id is \"done\"");
            check(shape.graph.start == "mgr", "B2: start_at(manager) was set");
            check(shape.graph.output_selection.size() == 1 && shape.graph.output_selection[0] == "done",
                  "B2: select_output(done) was set");

            check(has_edge(shape.graph.edges, "mgr", "p1", edge_kind::switch_case, "p1"),
                  "B2: manager->p1 switch_case, case label == participant id");
            check(has_edge(shape.graph.edges, "mgr", "p2", edge_kind::switch_case, "p2"),
                  "B2: manager->p2 switch_case, case label == participant id");
            check(has_edge(shape.graph.edges, "p1", "mgr", edge_kind::direct),
                  "B2: p1->manager direct");
            check(has_edge(shape.graph.edges, "p2", "mgr", edge_kind::direct),
                  "B2: p2->manager direct");
            check(has_edge(shape.graph.edges, "mgr", "done", edge_kind::switch_case, "done"),
                  "B2: manager->done switch_case, case label == done_selector()");
        }
    }

    // ---- B3: require_plan_signoff() is wired exactly like a participant --------------------------
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("signoff-test");
        b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
        b.require_plan_signoff("plan_review");
        b.max_rounds(20);
        result<MagenticGraph> built = b.build();
        check(built.has_value(), "B3: a builder with require_plan_signoff() builds");
        if (built) {
            check(built->plan_review_port_id.has_value() && *built->plan_review_port_id == "plan_review",
                  "B3: the port id is reported back");
            bool found_port = false;
            for (auto const& e : built->graph.executors) {
                if (e.id == "plan_review" && e.kind == executor_kind::request_port) found_port = true;
            }
            check(found_port, "B3: a request_port-kind executor named \"plan_review\" exists");
            check(has_edge(built->graph.edges, "mgr", "plan_review", edge_kind::switch_case, "plan_review"),
                  "B3: manager->plan_review switch_case (reachable, ADR-149 §3 finding 3)");
            check(has_edge(built->graph.edges, "plan_review", "mgr", edge_kind::direct),
                  "B3: plan_review->manager direct (the human's answer is never silently swallowed)");
            check(validate_workflow(built->graph).has_value(),
                  "B3: the graph -- including the typed port sharing the participant type pair -- "
                  "validates (proves the \"no new engine primitive\" claim actually holds)");
        }
    }

    // ---- B4: a done_selector() colliding with a participant id is a real, caught error -----------
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("collision-test");
        b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "done", .capability_ceiling = {}});  // collides with the default sink id
        result<MagenticGraph> built = b.build();
        check(!built.has_value(), "B4: a participant id colliding with the done sink id fails to build");
        if (!built) {
            check(built.error().code == "workflow.duplicate_executor_id",
                  "B4: caught by the SAME validation any hand-built graph would hit, not a special case");
        }
    }

    // ---- B5: end to end through a real WorkflowSupervisor ------------------------------------------
    if (shape.graph.executors.size() == 4) {
        std::atomic<int> step{0};
        std::vector<ExecutorBody> bodies = {
            // mgr: p1, then p2, then done -- a deterministic ledger, matching examples/17's own shape.
            [&step](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                int const n = step.fetch_add(1);
                std::vector<std::string> route = n == 0 ? std::vector<std::string>{"p1"}
                                                 : n == 1 ? std::vector<std::string>{"p2"}
                                                          : std::vector<std::string>{"done"};
                return ExecutorOutcome{text_message(text_of(in)), route};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">p1")};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">p2")};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(shape.graph, bodies, {}, shape.manager_id);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));

        check(r.status == workflow_status::completed,
              "B5: the builder-produced graph, with two genuinely distinct declared message types, "
              "runs to completion through a real WorkflowSupervisor");
        check(text_of(r.output) == "start>p1>p2", "B5: the payload threaded through mgr->p1->mgr->p2->mgr->done");
    }

    // ---- B6: max_stalls()/max_resets() forwarding composes with the builder's own graph -----------
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("stall-compose-test");
        b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
        b.max_rounds(50);
        b.max_stalls(2);
        result<MagenticGraph> built = b.build();
        check(built.has_value(), "B6 setup: builder with max_stalls() builds");
        if (built) {
            check(built->graph.bound.max_stalls.has_value() && *built->graph.bound.max_stalls == 2,
                  "B6: max_stalls() forwards to TerminationBound");

            std::vector<ExecutorBody> bodies = {
                [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                    ExecutorOutcome out{text_message(text_of(in)), {"p1"}};
                    out.stalled = true;  // never makes progress
                    return out;
                },
                [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                    return ExecutorOutcome{text_message(text_of(in))};
                },
                [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                    return ExecutorOutcome{in};  // the synthetic done sink
                },
            };
            WorkflowSupervisor sup;
            sup.initialize(built->graph, bodies, {}, built->manager_id);
            WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
            check(r.status == workflow_status::bound_max_stalls,
                  "B6: the builder's max_stalls() and initialize()'s designated_stall_reporter compose "
                  "correctly on a builder-produced graph, tripping the safety valve as designed");
        }
    }

    if (g_failures == 0) {
        std::printf("test_workflow_magentic_builder: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_magentic_builder: %d failure(s)\n", g_failures);
    return 1;
}
