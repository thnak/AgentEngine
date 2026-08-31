// AgentEngine "get started" examples, 10 -- conditional routing (014 §3's "Router" pattern).
//
// Mirrors MAF's samples/03-workflows/ConditionalEdges: one executor's own output decides which of
// several downstream branches actually runs. In AgentEngine this is `edge_kind::switch_case`: each
// candidate edge out of the router carries a `case_label` (`Edge::case_label`, graph.hpp), and the
// router signals which one fired by returning it in `ExecutorOutcome::routes` -- a
// `std::vector<std::string>` of the case labels THIS call selects, out of the ones the graph
// actually declares. Only the edge(s) whose label appears in `routes` fire; the rest never run,
// which is what this example measures (an invocation counter), not just infers from output text.
// ADR-037's Quark-free `rt::WorkflowSupervisor` (agentengine/rt/workflow_supervisor.hpp) drives the
// graph directly through plain coroutines, so there is no `FunctionExecutor::invocations()` to read
// anymore -- each branch handler is wrapped with a small invocation-counting lambda instead,
// wiring-only, the handler bodies themselves (`triage`/`handler`) are unchanged.
//
// Run: ./agentengine_example_10_conditional_routing

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

// A classifier node: an ordinary function of its input text, choosing exactly one case label. A
// model-backed classifier would fill this same slot -- the routing MECHANISM doesn't care what
// decided the label.
[[nodiscard]] ExecutorBody triage(std::function<std::string(std::string const&)> classify) {
    return [classify = std::move(classify)](
               Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text  = text_of(in);
        std::string const label = classify(text);
        return ExecutorOutcome{text_message(text + " -> " + label), {label}};
    };
}

[[nodiscard]] ExecutorBody handler(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + " (handled by " + name + ")");
    };
}

// Wraps `body` with an invocation counter -- the wiring-level replacement for the old
// `FunctionExecutor::invocations()` actor state, so this example can still measure (not just infer)
// that exactly one branch ran.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> count) {
    return [body = std::move(body), count](Message const& in,
                                           EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
        count->fetch_add(1, std::memory_order_relaxed);
        return body(in, ctx);
    };
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}

[[nodiscard]] Workflow make_graph() {
    Workflow wf;
    wf.id        = "triage-router";
    wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech")};
    wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
    wf.start = "triage";
    wf.output_selection.push_back("billing");
    wf.output_selection.push_back("tech");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::string all_text_of(Message const& m) { return text_of(m); }

// Drives an agentengine::rt::task<T> to completion. Safe here: nothing in a WorkflowSupervisor
// round genuinely suspends on an external wake (fan-out concurrency happens through
// std::future::get(), an ordinary blocking call, not a coroutine suspension) -- the same "safe
// because nothing here genuinely suspends" reasoning every rt:: test file's own drive<T>() relies
// on.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    Workflow const wf = make_graph();
    check(validate_workflow(wf).has_value(), "the graph validates");

    // Run the SAME graph against two different inputs -- proving the classifier's output steers the
    // route, not that one branch is hardcoded to always fire.
    for (std::string const& input : {std::string("my invoice is wrong"), std::string("the app crashed")}) {
        auto billing_invocations = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto tech_invocations    = std::make_shared<std::atomic<std::uint32_t>>(0);

        // `bodies` is parallel to `wf.executors` by index: triage, billing, tech.
        std::vector<ExecutorBody> bodies = {
            triage([](std::string const& text) {
                return text.find("invoice") != std::string::npos ? "billing" : "tech";
            }),
            counted(handler("billing"), billing_invocations),
            counted(handler("tech"), tech_invocations),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message(input)}));
        check(r.status == workflow_status::completed, ("the run completes for input: " + input).c_str());
        bool const expect_billing = input.find("invoice") != std::string::npos;
        std::printf("%s\n", all_text_of(r.output).c_str());
        check(billing_invocations->load(std::memory_order_relaxed) == (expect_billing ? 1u : 0u) &&
                  tech_invocations->load(std::memory_order_relaxed) == (expect_billing ? 0u : 1u),
              "exactly ONE branch ran -- the unselected branch's invoke() was never called");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_10_conditional_routing: OK\n"
                                          : "example_10_conditional_routing: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
