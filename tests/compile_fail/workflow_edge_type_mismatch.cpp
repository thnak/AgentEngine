// 014-Workflow-and-Orchestration.md §1's compile-time half, Milestone 6 Phase A:
// "An edge that connects incompatible types fails to build -- at compile time for the C++ form."
//
// MUST NOT COMPILE. `WorkflowBuilder::connect` static_asserts that the source executor's output
// message type IS the target's input message type; here `writer` produces `Draft` and `sink`
// consumes `Verdict`, so the edge is exactly the mismatch §1 requires to be rejected.
//
// Paired with workflow_edge_type_match_positive_control.cpp -- a fail-only check cannot distinguish
// "correctly rejected" from "this file never compiled for an unrelated reason".

#include "agentengine/workflow/graph.hpp"

namespace {
struct Question {};
struct Draft {};
struct Verdict {};
}  // namespace

AE_WORKFLOW_MESSAGE(Question, "Question");
AE_WORKFLOW_MESSAGE(Draft, "Draft");
AE_WORKFLOW_MESSAGE(Verdict, "Verdict");

int main() {
    using namespace agentengine::workflow;

    TypedExecutor<Question, Draft> writer{.id = "writer", .kind = executor_kind::agent, .capability_ceiling = {}};
    TypedExecutor<Verdict, Verdict> sink{.id = "sink", .kind = executor_kind::function, .capability_ceiling = {}};

    WorkflowBuilder b("mismatch");
    // Draft -> Verdict: incompatible. This line is the one that must not compile.
    b.add(writer).add(sink).connect(writer, sink);
    return 0;
}
