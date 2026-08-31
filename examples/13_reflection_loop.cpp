// AgentEngine "get started" examples, 13 -- a cycle with a bound (014 §3's "Reflection/Critic").
//
// Mirrors MAF's own reflection/critique-loop samples. Unlike the other workflow examples (04, 09,
// 10), the graph HERE has a cycle: writer -> critic -> writer, both plain `edge_kind::direct`
// edges. 014 §9 Q2 allows cycles outright ("the reflection and group-chat patterns need them"),
// and the superstep engine handles one with no special-cased "loop" construct -- it is exactly the
// same round-by-round execution every other pattern in this progression uses. What makes a cycle
// safe to run at all is 014 §2's real requirement: a `Workflow` MUST declare a `TerminationBound`,
// so an author cannot accidentally author an infinite loop -- `validate_workflow` rejects a
// `Workflow` with no bound at all.
//
// This example's writer/critic pair never converges on their own (they just keep appending text),
// so it runs until `bound.max_rounds` and stops there -- proving `workflow_status::bound_max_rounds`
// is a real, honest status distinct from `completed`: the caller is TOLD the bound is what stopped
// it, not left to guess whether the graph "finished" on its own.
//
// ADR-037: driven through `agentengine::rt::WorkflowSupervisor` -- a plain coroutine substrate, no
// Quark actor engine underneath. `run_workflow()` returns an `rt::task<WorkflowResult>`; the local
// `drive<T>()` helper below just resumes it to completion (its only suspension points are an
// uncontended mutex and a nested `co_await` that never itself suspends -- see
// `tests/test_rt_workflow_supervisor.cpp`'s own `drive<T>()` comment for why one `resume()` always
// finishes it here). No engine, no router, no actor refs, no placement dance.
//
// Run: ./agentengine_example_13_reflection_loop

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
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

// Both nodes just tag the running draft with their own name and round -- a stand-in for a real
// writer producing a draft and a real critic appending feedback. What matters for this example is
// the GRAPH shape (a bounded cycle), not what either node's body actually does.
[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + ">" + name);
    };
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}

// Safe here: run_workflow()'s only suspension points are the run mutex's uncontended fast path and
// a nested co_await whose own body never suspends either -- see
// tests/test_rt_workflow_supervisor.cpp's own drive<T>() comment for the full reasoning.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // The cycle: writer -> critic -> writer, both plain direct edges. A `Workflow` with no bound at
    // all fails `validate_workflow` (014 §2's real requirement) -- max_rounds below IS the safety
    // net that makes authoring a cycle at all acceptable.
    Workflow wf;
    wf.id        = "reflection";
    wf.executors = {node_desc("writer"), node_desc("critic")};
    wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}});
    wf.start = "writer";
    wf.output_selection.push_back("critic");
    wf.bound.max_rounds = 6;
    check(validate_workflow(wf).has_value(), "a cyclic graph with a bound validates (014 §9 Q2)");

    std::vector<ExecutorBody> bodies = {appender("write"), appender("critique")};
    WorkflowSupervisor         supervisor;
    supervisor.initialize(wf, bodies);

    WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("draft")}));
    check(r.status == workflow_status::bound_max_rounds,
          "the bound stopped the loop -- an honest status distinct from workflow_status::"
          "completed, telling the caller WHY it ended rather than leaving them to guess");
    check(r.rounds == 6, "exactly max_rounds rounds ran, no more");
    std::string const output = text_of(r.output);
    std::printf("%s\n", output.c_str());
    check(output == "draft>write>critique>write>critique>write>critique",
          "the draft threaded through 3 full writer/critic cycles before the bound stopped it");

    std::fprintf(stderr, g_failures == 0 ? "example_13_reflection_loop: OK\n"
                                          : "example_13_reflection_loop: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
