// devsim/verify-act6 — objective verification of the graph documented in api/pizza-order.html's
// Act 6 ("a complaint is a routing problem"). Asserts the two claims that snippet makes:
//   1. validate_workflow(wf) accepts it            (the snippet asserts this)
//   2. a function-kind mesh with a request_port node initializes and runs
// It also demonstrates the namespace rule: `sharing_mode` needs `using namespace agentengine`, while
// Executor/edge_kind need `using namespace agentengine::workflow`.
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;            // sharing_mode lives here, NOT in workflow::
using namespace agentengine::workflow;  // Executor, Edge, edge_kind, executor_kind live here
using agentengine::rt::block_on;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what);
    if (!cond) ++g_failures;
}

Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

// ---- transcribed from the Act 6 snippet -------------------------------------------------------
Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "Text", .output_type = "Text",
                    .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

Workflow make_graph() {
    Workflow wf;
    wf.id        = "complaint-handoff";
    wf.executors = {node_desc("Triage"), node_desc("Kitchen"),
                    node_desc("Manager", executor_kind::request_port),  // a human, so no body
                    node_desc("Done")};

    wf.edges.push_back(Edge{"Triage", "Kitchen", edge_kind::switch_case, "kitchen"});
    wf.edges.push_back(Edge{"Triage", "Manager", edge_kind::switch_case, "manager"});
    wf.edges.push_back(Edge{"Triage", "Done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"Kitchen", "Done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"Manager", "Done", edge_kind::direct, {}});
    wf.start            = "Triage";
    wf.output_selection = {"Done"};
    wf.bound.max_rounds = 6;
    return wf;
}

// Triage picks the route from the complaint text; the case label on the edge decides where it goes.
ExecutorBody triage_body() {
    return [](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        std::string const text  = text_of(in);
        std::string const label = text.find("refund") != std::string::npos ? "kitchen" : "manager";
        co_return ExecutorOutcome{text_message(text), {label}};
    };
}

ExecutorBody kitchen_body() {
    return [](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        co_return ExecutorOutcome{text_message("[kitchen] " + text_of(in)), {"done"}};
    };
}

ExecutorBody identity_sink() {
    return [](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        co_return ExecutorOutcome{in};
    };
}

}  // namespace

int main() {
    Workflow wf = make_graph();

    // Claim 1: the snippet's own `check(validate_workflow(wf).has_value(), ...)`.
    auto valid = validate_workflow(wf);
    if (!valid.has_value()) {
        std::fprintf(stderr, "FAIL: validate_workflow rejected Act 6's graph: %s (%s)\n",
                     valid.error().message.c_str(), valid.error().code.c_str());
        return 1;
    }
    check(true, "validate_workflow accepts Act 6's graph (labelled switch_case + direct)");

    // Claim 2a: routing to Kitchen -- function-kind bodies through the two-argument initialize().
    {
        std::vector<ExecutorBody> bodies = {triage_body(), kitchen_body(), {}, identity_sink()};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult const r = block_on(sup.run_workflow(RunWorkflow{text_message("refund please")}));
        check(r.status == workflow_status::completed, "routing to Kitchen completes");
        check(text_of(r.output).find("[kitchen]") != std::string::npos,
              "the Kitchen branch actually ran");
    }

    // Claim 2b: routing to Manager (a request_port) -- the run suspends rather than guessing.
    {
        std::vector<ExecutorBody> bodies = {triage_body(), kitchen_body(), {}, identity_sink()};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult const r =
            block_on(sup.run_workflow(RunWorkflow{text_message("I want to speak to a manager")}));
        check(r.status == workflow_status::suspended, "routing to the Manager port suspends the run");
        check(!r.open_interactions.empty(), "a real interaction is open while suspended");
    }

    std::fprintf(stderr, g_failures == 0 ? "verify_act6: OK\n" : "verify_act6: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
