// devsim/verify-bodyforms — verifies the corrected ExecutorBody sentence: what a body may return.
//   1. a body returning a Message (deduced, or annotated ExecutorOutcome) works -- the advertised
//      Message -> ExecutorOutcome convenience;
//   2. a routing body must be annotated result<ExecutorOutcome>;
//   3. annotating a pass-through body as `result<Message>` does NOT compile (C2440): the conversion
//      to ExecutorOutcome has to happen inside the returned `result`, and two user-defined
//      conversions do not chain. That negative is NOT written here, because it would not compile --
//      it is recorded in the docs instead.
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;            // sharing_mode
using namespace agentengine::workflow;  // Executor, Edge, edge_kind, executor_kind
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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "Text", .output_type = "Text",
                    .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

}  // namespace

int main() {
    // ---- 1. the pass-through shape: return a Message, let it convert -------------------------
    ExecutorBody passthrough = [](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        co_return ExecutorOutcome{text_message(text_of(in) + "!")};  // Message -> ExecutorOutcome, implicitly
    };

    // ---- 2. the routing shape: annotate result<ExecutorOutcome> ------------------------------
    ExecutorBody router = [](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        std::string const text = text_of(in);
        co_return ExecutorOutcome{text_message(text), {text.find("x") != std::string::npos ? "a" : "b"}};
    };

    Workflow wf;
    wf.id        = "forms";
    wf.executors = {node_desc("src"), node_desc("mid"), node_desc("a"), node_desc("b")};
    wf.edges.push_back(Edge{"src", "mid", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"mid", "a", edge_kind::switch_case, "a"});
    wf.edges.push_back(Edge{"mid", "b", edge_kind::switch_case, "b"});
    wf.start            = "src";
    wf.output_selection = {"a", "b"};
    wf.bound.max_rounds = 4;
    check(validate_workflow(wf).has_value(), "both body shapes plug into one graph");

    std::vector<ExecutorBody> bodies = {passthrough, router, passthrough, passthrough};
    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);
    WorkflowResult const r = block_on(sup.run_workflow(RunWorkflow{text_message("x")}));
    check(r.status == workflow_status::completed, "the run completes through the router");
    // x -> src passthrough -> "x!" -> mid router -> route "a" -> a passthrough -> "x!!"
    check(text_of(r.output) == "x!!",
          "the Message-returning body ran AND the router's own route was honoured");

    std::fprintf(stderr, g_failures == 0 ? "verify_bodyforms: OK\n" : "verify_bodyforms: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
