// AgentEngine "get started" examples, 4 of 4 -- a two-node workflow.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/05_first_workflow: a small text
// pipeline of two executors connected by an edge -- uppercase the input, then reverse it. For
// input "Hello, World!" the workflow produces "!DLROW ,OLLEH", exactly like the MAF sample it
// mirrors.
//
// AgentEngine's workflow model (014-Workflow-and-Orchestration.md) is its own thing, not a
// `WorkflowBuilder` fluent API: a `Workflow` is DATA (`executors`/`edges`/`start`/
// `output_selection`/`bound`, agentengine/workflow/graph.hpp), executed round-by-round (a
// "superstep" per round, 014 §2). ADR-037's Quark-free `agentengine::rt::WorkflowSupervisor`
// (agentengine/rt/workflow_supervisor.hpp) drives that same graph directly through plain
// coroutines -- no `quark::Engine`, no actor placement, no message pool: a
// `rt::WorkflowSupervisor` local `.initialize()`d with the graph and a
// `std::vector<rt::ExecutorBody>` (parallel to `graph.executors` by index), then driven with
// `drive(sup.run_workflow(RunWorkflow{...}))`.
//
// Run: ./agentengine_example_04_first_workflow

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

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

// Two executor bodies -- plain functions over a `Message`, no agent, no model call. `rt::ExecutorBody`
// (workflow_supervisor.hpp) is exactly this: `std::function<result<ExecutorOutcome>(Message const&,
// EffectContext&)>`, and a `Message` converts implicitly into an `ExecutorOutcome` for the common
// "just pass a message downstream" case -- these bodies need no changes at all, only the wiring
// around them does.
[[nodiscard]] agentengine::result<Message> uppercase(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::transform(text.begin(), text.end(), text.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text_message(std::move(text));
}

[[nodiscard]] agentengine::result<Message> reverse_text(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::reverse(text.begin(), text.end());
    return text_message(std::move(text));
}

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
    // The graph itself: two executors, one edge, "Uppercase" first, "Reverse"'s output selected.
    Workflow wf;
    wf.id        = "uppercase-then-reverse";
    wf.executors = {Executor{"Uppercase", executor_kind::function, "Text", "Text"},
                     Executor{"Reverse", executor_kind::function, "Text", "Text"}};
    wf.edges.push_back(Edge{"Uppercase", "Reverse", edge_kind::chain, {}});
    wf.start = "Uppercase";
    wf.output_selection.push_back("Reverse");
    wf.bound.max_rounds = 4;
    check(validate_workflow(wf).has_value(), "the graph validates");

    // `bodies` is parallel to `wf.executors` by index -- bodies[0] is "Uppercase"'s body,
    // bodies[1] is "Reverse"'s.
    std::vector<ExecutorBody> bodies = {uppercase, reverse_text};
    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("Hello, World!")}));
    check(r.status == workflow_status::completed, "the chain terminates by running dry (014 §2)");
    check(r.rounds == 2, "exactly one round per edge in the chain");
    std::string const output = text_of(r.output);
    std::printf("%s\n", output.c_str());
    check(output == "!DLROW ,OLLEH", "input threaded through both nodes in graph order");

    std::fprintf(stderr, g_failures == 0 ? "example_04_first_workflow: OK\n"
                                          : "example_04_first_workflow: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
