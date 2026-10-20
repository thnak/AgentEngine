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

#include <utility>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/block_on.hpp"
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
    return agentengine::rt::block_on(std::move(t));
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
        [](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
            co_return ExecutorOutcome{text_message(text_of(in) + ">greet")};
        },
        [](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
            co_return ExecutorOutcome{text_message(text_of(in) + ">shout")};
        },
    };
}


// ADR-193 §9 / issue #196: a one-node inner workflow whose node spends 100 + 40 tokens, so the charge is visible.
struct Spend100Inner {
    Workflow wf;
    std::vector<ExecutorBody> bodies;
    WorkflowSupervisor sup;
    Spend100Inner() {
        wf.id        = "inner-spend";
        wf.executors = {node_desc("spend")};
        wf.start     = "spend";
        wf.output_selection.push_back("spend");
        wf.bound.max_rounds = 4;
        bodies.push_back([](Message const& in, agentengine::EffectContext&)
                             -> agentengine::task<agentengine::result<ExecutorOutcome>> {
            ExecutorOutcome o{text_message(text_of(in) + ">spent")};
            o.usage.input_tokens  = 100;
            o.usage.output_tokens = 40;
            co_return o;
        });
        sup.initialize(wf, bodies);
    }
};

struct ChargeLog {
    int charges = 0;
    agentengine::Usage total{};
    [[nodiscard]] agentengine::EffectContext context() {
        agentengine::EffectContext ctx;
        ctx.charge_delegated_usage = [this](agentengine::Usage const& u, std::uint64_t) {
            ++charges;
            total.input_tokens += u.input_tokens;
            total.output_tokens += u.output_tokens;
        };
        return ctx;
    }
};

bool g_hook_parked = false;
bool g_park_hook   = false;
struct NeverResumed {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept { g_hook_parked = true; }
    void await_resume() const noexcept {}
};

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
            result<ExecutorOutcome> out = agentengine::rt::block_on((*wrapped)(text_message("in"), ctx));
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
            result<ExecutorOutcome> first  = agentengine::rt::block_on((*wrapped)(text_message("call"), ctx));
            result<ExecutorOutcome> second = agentengine::rt::block_on((*wrapped)(text_message("call"), ctx));
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
            [](Message const&, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                co_return ExecutorOutcome{text_message("x"), {"unknown"}};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                co_return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        result<ExecutorBody> wrapped = workflow_as_executor_body(sup);
        check(wrapped.has_value(), "W4 setup: construction succeeds (no request_port here)");
        if (wrapped) {
            agentengine::EffectContext ctx;
            result<ExecutorOutcome> out = agentengine::rt::block_on((*wrapped)(text_message("in"), ctx));
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
            [call_count](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                call_count->fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));  // widen the race window
                co_return ExecutorOutcome{in};
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
                [](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                    co_return ExecutorOutcome{in};
                },
                [](Message const&, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                    co_return ExecutorOutcome{text_message("fromA")};
                },
                [](Message const&, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                    co_return ExecutorOutcome{text_message("fromB")};
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
            result<ExecutorOutcome> out = agentengine::rt::block_on((*wrapped)(text_message("factory"), ctx));
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
                [](Message const& in, agentengine::EffectContext&) -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                    co_return ExecutorOutcome{text_message(text_of(in) + ">prep")};
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

    // ---- C1-C5 (issue #196, ADR-193 section 9): the inner run is charged to the caller exactly once on every exit
    {
        // C1: the inner run spends, then a round-boundary hook throws out of run_workflow -- charged once.
        Spend100Inner inner;
        inner.sup.set_checkpoint_hook([](std::uint32_t, agentengine::rt::RunStateRecord) -> agentengine::task<void> {
            throw std::runtime_error("host hook failed");
            co_return;
        });
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner.sup);
        ChargeLog log;
        agentengine::EffectContext ctx = log.context();
        Message const input = text_message("in");
        bool threw = false;
        try {
            (void)agentengine::rt::block_on((*wrapped)(input, ctx));
        } catch (std::runtime_error const&) {
            threw = true;
        }
        check(threw && log.charges == 1 && log.total.input_tokens == 100 && log.total.output_tokens == 40,
              "C1: an inner run that throws after spending is charged to the caller exactly once (100 in, 40 out)");
    }
    {
        // C2: the inner run parks after spending and the outer task is destroyed while suspended.
        Spend100Inner inner;
        inner.sup.set_checkpoint_hook([](std::uint32_t, agentengine::rt::RunStateRecord) -> agentengine::task<void> {
            co_await NeverResumed{};
        });
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner.sup);
        ChargeLog log;
        agentengine::EffectContext ctx = log.context();
        g_hook_parked = false;
        {
            Message const input = text_message("in");  // named: the body is lazy and takes a const&
            auto t = (*wrapped)(input, ctx);
            t.resume();
            check(g_hook_parked && !t.done() && log.charges == 0,
                  "C2: setup -- the inner run is parked after spending and nothing is charged yet");
        }
        check(log.charges == 1 && log.total.input_tokens == 100 && log.total.output_tokens == 40,
              "C2: an inner run dropped while suspended is charged to the caller exactly once (100 in, 40 out)");
    }
    {
        // C3: a PREVIOUS completed run left usage() nonzero; the next call is dropped before it ever runs.
        Spend100Inner inner;
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner.sup);
        ChargeLog first;
        agentengine::EffectContext ctx1 = first.context();
        Message const input = text_message("in");
        result<ExecutorOutcome> done = agentengine::rt::block_on((*wrapped)(input, ctx1));
        check(done.has_value() && inner.sup.usage().input_tokens == 100 && first.charges == 0,
              "C3: setup -- a completed run left usage() at 100 input tokens (its outcome carries them)");
        ChargeLog log;
        agentengine::EffectContext ctx = log.context();
        {
            auto t = (*wrapped)(input, ctx);  // created, never resumed
        }
        check(log.charges == 0, "C3: dropped before the call began, the previous run's usage is NOT charged");
    }
    {
        // C3b: the call is inside run_once, parked on the inner supervisor's run mutex (another driver's run holds
        // it), so ITS run_workflow has not started; usage() holds the other run's partial spend and must not be charged.
        Spend100Inner inner;
        inner.sup.set_checkpoint_hook([](std::uint32_t, agentengine::rt::RunStateRecord) -> agentengine::task<void> {
            if (g_park_hook) co_await NeverResumed{};
        });
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner.sup);
        ChargeLog log;
        agentengine::EffectContext ctx = log.context();
        Message const input = text_message("in");
        g_park_hook = false;
        result<ExecutorOutcome> done = agentengine::rt::block_on((*wrapped)(input, ctx));
        check(done.has_value(), "C3b: setup -- a first run completed");
        g_park_hook   = true;
        g_hook_parked = false;
        auto other = inner.sup.run_workflow(RunWorkflow{input});  // a direct driver's run, parked holding the mutex
        other.resume();
        check(g_hook_parked && inner.sup.usage().input_tokens == 100,
              "C3b: setup -- another run holds the supervisor and has spent 100 input tokens");
        ChargeLog log2;
        agentengine::EffectContext ctx2 = log2.context();
        {
            auto t = (*wrapped)(input, ctx2);
            t.resume();  // inside run_once, parked on run_mutex_
            check(!t.done(), "C3b: setup -- the second call is parked on the busy supervisor");
        }
        check(log2.charges == 0, "C3b: dropped before its own inner run started, another run's spend is NOT charged");
    }
    {
        // C4/C5: no double charge on a normal completion (the outcome carries it) or a normal failure (the hook, once).
        Spend100Inner inner;
        result<ExecutorBody> wrapped = workflow_as_executor_body(inner.sup);
        ChargeLog log;
        agentengine::EffectContext ctx = log.context();
        Message const input = text_message("in");
        result<ExecutorOutcome> out = agentengine::rt::block_on((*wrapped)(input, ctx));
        check(out.has_value() && log.charges == 0 && out->usage.input_tokens == 100 && out->usage.output_tokens == 40,
              "C4: a normal completion is charged through its outcome, never also through the hook");

        Spend100Inner failing;
        failing.wf.executors.push_back(node_desc("tail"));
        failing.wf.edges.push_back(Edge{"spend", "tail", edge_kind::direct, {}});
        failing.wf.output_selection = {"tail"};
        failing.wf.bound.max_rounds = 1;
        failing.bodies.push_back([](Message const& in, agentengine::EffectContext&)
                                     -> agentengine::task<agentengine::result<ExecutorOutcome>> {
            co_return ExecutorOutcome{text_message(text_of(in))};
        });
        failing.sup.initialize(failing.wf, failing.bodies);
        result<ExecutorBody> failing_wrapped = workflow_as_executor_body(failing.sup);
        ChargeLog flog;
        agentengine::EffectContext fctx = flog.context();
        result<ExecutorOutcome> fout = agentengine::rt::block_on((*failing_wrapped)(input, fctx));
        check(!fout.has_value() && flog.charges == 1 && flog.total.input_tokens == 100 && flog.total.output_tokens == 40,
              "C5: a normal failure (bound_max_rounds) is charged through the hook exactly once (100 in, 40 out)");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_as_executor: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_as_executor: %d failure(s)\n", g_failures);
    return 1;
}
