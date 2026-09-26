// Proof for the workflow-as-participant adapter (adjacent to GitHub issue #35; design draft:
// docs/planning/workflow-as-executor-body-adapter-design-draft.md) --
// agentengine::rt::workflow_as_executor_body() (include/agentengine/rt/workflow_as_executor.hpp).
//   W1 -- a completed inner run's output reaches the caller unchanged.
//   W2 -- a SECOND call to the same wrapped adapter starts fully fresh -- identical input produces
//         identical output both times, proving no cross-call state leaked from WorkflowSupervisor's
//         own internals (design draft §2's "every call is independent" contract).
//   W3 -- construction is REFUSED (both overloads) for an inner graph containing a request_port
//         executor -- design draft §4/§10 finding 3's fix, not a documented-but-unenforced limitation.
//   W4 -- an inner run that does not complete (routing_failed) fails closed with a status-specific
//         error code, not a crash or a default-valued success.
//   W5 -- REAL concurrency: two genuinely concurrent deliveries to the SAME wrapped node in one outer
//         round (root -fan_out-> srcA/srcB -direct-> wrapped, mirroring
//         test_rt_workflow_stall_reset_bounds.cpp's own S7 pattern) complete correctly, serialized by
//         the adapter's own mutex, with zero crash/corruption across repeated trials -- design draft
//         §6/§10 finding 1's fix, proven under actual contention, not just reasoned about.
//   W6 -- the shared_ptr (owning) overload, used from a factory function whose local
//         WorkflowSupervisor goes out of scope before the returned body is ever called -- design
//         draft §10 finding 4's fix for the reference-capture dangling hazard.
//   W7 -- END TO END: a whole inner Workflow, wrapped via workflow_as_executor_body(), used as ONE
//         ordinary node inside an OUTER WorkflowBuilder graph, driven through a real
//         WorkflowSupervisor::run_workflow() call on the OUTER graph -- the actual headline use case,
//         proven composable, not just unit-tested in isolation.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_as_executor.hpp"
#include "agentengine/workflow/graph.hpp"

using agentengine::result;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_as_executor_body;
using agentengine::rt::workflow_status;

using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;
using agentengine::workflow::Workflow;

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

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = agentengine::sharing_mode::branch, .capability_ceiling = {}};
}

// A two-node inner workflow: "greet" -> "shout", each appending to the payload -- multi-step, so
// W1/W2 prove the whole inner run genuinely executed, not just a single trivial hop.
[[nodiscard]] Workflow two_step_inner_graph() {
    Workflow wf;
    wf.id        = "inner-two-step";
    wf.executors = {node_desc("greet"), node_desc("shout")};
    wf.edges.push_back(Edge{"greet", "shout", edge_kind::direct, {}});
    wf.start = "greet";
    wf.output_selection.push_back("shout");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> two_step_inner_bodies() {
    return {
        [](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">greet")};
        },
        [](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">shout")};
        },
    };
}

}  // namespace

int main() {
    // ---- W1: completed inner run's output reaches the caller unchanged --------------------------
    {
        Workflow wf = two_step_inner_graph();
        std::vector<ExecutorBody> bodies = two_step_inner_bodies();
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        result<ExecutorBody> wrapped = workflow_as_executor_body(sup);
        check(wrapped.has_value(), "W1: construction succeeds for a request_port-free graph");
        if (wrapped) {
            agentengine::EffectContext ctx;
            result<ExecutorOutcome> out = (*wrapped)(text_message("in"), ctx);
            check(out.has_value(), "W1: the wrapped call succeeds");
            if (out) {
                check(text_of(out->payload) == "in>greet>shout",
                      "W1: the inner workflow's real multi-step output reaches the caller unchanged");
                check(out->routes.empty(), "W1: ROUTING NOTE -- no routing concept, routes is empty");
            }
        }
    }

    // ---- W2: a second call starts fully fresh, no leftover WorkflowSupervisor state ---------------
    {
        Workflow wf = two_step_inner_graph();
        std::vector<ExecutorBody> bodies = two_step_inner_bodies();
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        result<ExecutorBody> wrapped = workflow_as_executor_body(sup);
        check(wrapped.has_value(), "W2 setup: construction succeeds");
        if (wrapped) {
            agentengine::EffectContext ctx;
            result<ExecutorOutcome> first  = (*wrapped)(text_message("call"), ctx);
            result<ExecutorOutcome> second = (*wrapped)(text_message("call"), ctx);
            check(first.has_value() && second.has_value(), "W2: both calls succeed");
            if (first && second) {
                check(text_of(first->payload) == text_of(second->payload),
                      "W2: identical input produces identical output on the second call -- no "
                      "cross-call state accumulated (would differ, e.g. a doubled suffix, if it had)");
            }
        }
    }

    // ---- W3: construction refused for a request_port graph, both overloads ------------------------
    {
        Workflow wf;
        wf.id        = "inner-with-gate";
        wf.executors = {node_desc("gate", executor_kind::request_port)};
        wf.start     = "gate";
        wf.output_selection.push_back("gate");
        wf.bound.max_rounds = 4;
        std::vector<ExecutorBody> bodies = {ExecutorBody{}};

        WorkflowSupervisor ref_sup;
        ref_sup.initialize(wf, bodies);
        result<ExecutorBody> ref_wrapped = workflow_as_executor_body(ref_sup);
        check(!ref_wrapped.has_value(),
              "W3: the reference overload refuses a graph containing a request_port executor");
        if (!ref_wrapped) {
            check(ref_wrapped.error().code == "rt.workflow_as_executor.request_port_unsupported",
                  "W3: the specific error code (reference overload)");
        }

        auto shared_sup = std::make_shared<WorkflowSupervisor>();
        shared_sup->initialize(wf, bodies);
        result<ExecutorBody> shared_wrapped = workflow_as_executor_body(shared_sup);
        check(!shared_wrapped.has_value(),
              "W3: the shared_ptr overload ALSO refuses the same graph shape");
        if (!shared_wrapped) {
            check(shared_wrapped.error().code == "rt.workflow_as_executor.request_port_unsupported",
                  "W3: the specific error code (shared_ptr overload)");
        }
    }

    // ---- W4: an inner run that does not complete fails closed with a status-specific error code ---
    {
        Workflow wf;
        wf.id        = "inner-broken-route";
        wf.executors = {node_desc("classify"), node_desc("billing")};
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing"});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.bound.max_rounds = 4;
        check(validate_workflow(wf).has_value(), "W4 setup: the graph validates");

        std::vector<ExecutorBody> bodies = {
            // Succeeds, but names a label NO declared edge carries -> routing_failed, not a crash.
            [](Message const&, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message("x"), {"unknown"}};
            },
            [](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        result<ExecutorBody> wrapped = workflow_as_executor_body(sup);
        check(wrapped.has_value(), "W4 setup: construction succeeds (no request_port here)");
        if (wrapped) {
            agentengine::EffectContext ctx;
            result<ExecutorOutcome> out = (*wrapped)(text_message("in"), ctx);
            check(!out.has_value(), "W4: a routing_failed inner run fails closed, not a default value");
            if (!out) {
                check(out.error().code ==
                          "rt.workflow_as_executor.inner_run_not_completed.routing_failed",
                      "W4: the error code embeds the SPECIFIC inner status, not a generic one");
            }
        }
    }

    // ---- W5: real concurrency -- two genuinely concurrent deliveries to the SAME wrapped node ------
    {
        Workflow inner_wf;
        inner_wf.id        = "inner-delayed-echo";
        inner_wf.executors = {node_desc("echo")};
        inner_wf.start     = "echo";
        inner_wf.output_selection.push_back("echo");
        inner_wf.bound.max_rounds = 4;

        auto call_count = std::make_shared<std::atomic<int>>(0);
        std::vector<ExecutorBody> inner_bodies = {
            [call_count](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                call_count->fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));  // widen the race window
                return ExecutorOutcome{in};
            },
        };

        for (int trial = 0; trial < 5; ++trial) {
            call_count->store(0, std::memory_order_relaxed);
            WorkflowSupervisor inner_sup;
            inner_sup.initialize(inner_wf, inner_bodies);
            result<ExecutorBody> wrapped = workflow_as_executor_body(inner_sup);
            if (!wrapped) {
                check(false, "W5 setup: construction succeeds");
                break;
            }

            Workflow outer_wf;
            outer_wf.id        = "outer-fanout-to-one-wrapped-node";
            outer_wf.executors = {node_desc("root"), node_desc("srcA"), node_desc("srcB"),
                                   node_desc("wrapped")};
            outer_wf.edges.push_back(Edge{"root", "srcA", edge_kind::fan_out, {}});
            outer_wf.edges.push_back(Edge{"root", "srcB", edge_kind::fan_out, {}});
            outer_wf.edges.push_back(Edge{"srcA", "wrapped", edge_kind::direct, {}});
            outer_wf.edges.push_back(Edge{"srcB", "wrapped", edge_kind::direct, {}});
            outer_wf.start = "root";
            outer_wf.output_selection.push_back("wrapped");
            outer_wf.bound.max_rounds = 10;

            std::vector<ExecutorBody> outer_bodies = {
                [](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                    return ExecutorOutcome{in};
                },
                [](Message const&, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                    return ExecutorOutcome{text_message("fromA")};
                },
                [](Message const&, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                    return ExecutorOutcome{text_message("fromB")};
                },
                *wrapped,
            };
            WorkflowSupervisor outer_sup;
            outer_sup.initialize(outer_wf, outer_bodies);
            WorkflowResult r = drive(outer_sup.run_workflow(RunWorkflow{text_message("start")}));

            check(r.status == workflow_status::completed,
                  "W5: the outer run completes despite two concurrent deliveries to the wrapped node");
            check(call_count->load(std::memory_order_relaxed) == 2,
                  "W5: the wrapped node's inner workflow genuinely ran TWICE (both concurrent "
                  "deliveries served correctly, serialized -- not lost, not corrupted, not crashed)");
        }
    }

    // ---- W6: the shared_ptr overload survives its local WorkflowSupervisor going out of scope -----
    {
        auto make_wrapped_body = []() -> result<ExecutorBody> {
            auto local_sup = std::make_shared<WorkflowSupervisor>();
            Workflow wf = two_step_inner_graph();
            std::vector<ExecutorBody> bodies = two_step_inner_bodies();
            local_sup->initialize(wf, bodies);
            return workflow_as_executor_body(local_sup);
            // `local_sup` (the local shared_ptr variable) goes out of scope here -- the returned
            // closure's OWN copy of the shared_ptr is what keeps the WorkflowSupervisor alive.
        };

        result<ExecutorBody> wrapped = make_wrapped_body();
        check(wrapped.has_value(), "W6: the factory returns successfully");
        if (wrapped) {
            agentengine::EffectContext ctx;
            result<ExecutorOutcome> out = (*wrapped)(text_message("factory"), ctx);
            check(out.has_value() && text_of(out->payload) == "factory>greet>shout",
                  "W6: the body still works correctly after its local WorkflowSupervisor's own "
                  "variable went out of scope -- the shared_ptr overload owns a real keep-alive, no "
                  "dangling reference");
        }
    }

    // ---- W7: END TO END -- a whole Workflow, wrapped, used as ONE node in an OUTER WorkflowBuilder -
    {
        Workflow inner_wf = two_step_inner_graph();
        std::vector<ExecutorBody> inner_bodies = two_step_inner_bodies();
        WorkflowSupervisor inner_sup;
        inner_sup.initialize(inner_wf, inner_bodies);
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner_sup);
        check(wrapped.has_value(), "W7 setup: the inner workflow wraps successfully");
        if (wrapped) {
            Workflow outer_wf;
            outer_wf.id        = "outer-composes-a-wrapped-inner-workflow";
            outer_wf.executors = {node_desc("prep"), node_desc("wrapped_participant")};
            outer_wf.edges.push_back(Edge{"prep", "wrapped_participant", edge_kind::direct, {}});
            outer_wf.start = "prep";
            outer_wf.output_selection.push_back("wrapped_participant");
            outer_wf.bound.max_rounds = 8;
            check(validate_workflow(outer_wf).has_value(), "W7: the outer graph validates");

            std::vector<ExecutorBody> outer_bodies = {
                [](Message const& in, agentengine::EffectContext&) -> result<ExecutorOutcome> {
                    return ExecutorOutcome{text_message(text_of(in) + ">prep")};
                },
                *wrapped,  // the whole inner Workflow, reused as an ORDINARY outer participant
            };
            WorkflowSupervisor outer_sup;
            outer_sup.initialize(outer_wf, outer_bodies);
            WorkflowResult r = drive(outer_sup.run_workflow(RunWorkflow{text_message("task")}));

            check(r.status == workflow_status::completed,
                  "W7: the OUTER graph, composing the wrapped inner workflow as one node, completes");
            check(text_of(r.output) == "task>prep>greet>shout",
                  "W7: the final output reflects BOTH the outer prep step AND the full inner "
                  "workflow's own two-step processing -- genuinely composable end to end, not just "
                  "unit-tested in isolation");
        }
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_as_executor: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_as_executor: %d failure(s)\n", g_failures);
    return 1;
}
