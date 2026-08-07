// Positive control for workflow_edge_type_mismatch.cpp (014 §1, Milestone 6 Phase A).
//
// MUST COMPILE. Identical in every respect to the mismatch file except that the edge's types line
// up (`writer` produces `Draft`, `critic` consumes `Draft`). Without this, a mismatch check that
// "passed" because the header was broken, an include path was wrong, or `TypedExecutor` failed to
// instantiate at all would be indistinguishable from the type rule actually working.

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

    TypedExecutor<Question, Draft> writer{"writer", executor_kind::agent};
    TypedExecutor<Draft, Verdict>  critic{"critic", executor_kind::agent};

    WorkflowBuilder b("match");
    auto built = b.add(writer).add(critic).connect(writer, critic).start_at("writer")
                  .select_output("critic").max_rounds(4).build();
    return built.has_value() ? 0 : 1;
}
