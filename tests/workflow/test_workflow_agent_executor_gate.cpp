// OQ-19 (OpenQuestions.md; docs/planning/agent-as-workflow-executor-design-draft.md §5): proves the
// GRAPH-LAYER half of the agent-executor CapabilitySet gate -- `Executor::capability_ceiling`,
// `check_workflow_executable(wf, contexts)` (workflow/graph.hpp), and `compile_executor()`'s explicit
// refusal of an authored `capability_ceiling:` YAML key (workflow/yaml_compiler.hpp), rather than
// silently dropping it (the I6 drift hazard `test_workflow_graph_validation.cpp`'s own decision 6
// already exists to prevent). The RUNTIME half -- WorkflowSupervisor actually dispatching an
// agent-kind node through a real AgentSession, the structural body-marker check, and the
// concurrent-same-node quarantine -- is proven in tests/test_rt_agent_workflow_executor.cpp; this
// file never links against rt:: at all.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/yaml_value.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/yaml_compiler.hpp"

using namespace agentengine;
using namespace agentengine::workflow;

namespace yaml = agentengine::yaml;

namespace {

struct Q {};
struct A {};

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
void check_error(result<T> const& r, failure_class expected_klass, char const* expected_code,
                  char const* what) {
    if (r.has_value()) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (accepted, expected rejection)\n", what);
        return;
    }
    if (r.error().code != expected_code || r.error().klass != expected_klass) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (rejected with code='%s', expected '%s')\n", what,
                     r.error().code.c_str(), expected_code);
        return;
    }
    std::fprintf(stderr, "  ok: %s\n", what);
}

[[nodiscard]] Workflow one_agent_node(std::vector<Capability> ceiling = {}) {
    Workflow wf;
    wf.id = "gate";
    Executor agent{.id = "a", .kind = executor_kind::agent, .input_type = "Q", .output_type = "A",
                   .capability_ceiling = {}};
    agent.capability_ceiling = std::move(ceiling);
    wf.executors.push_back(agent);
    wf.start = "a";
    wf.output_selection.push_back("a");
    wf.bound.max_rounds = 1;
    return wf;
}

}  // namespace

AE_WORKFLOW_MESSAGE(Q, "Q");
AE_WORKFLOW_MESSAGE(A, "A");

int main() {
    // ---- G1: the single-arg overload's pre-existing refusal is unchanged -------------------------
    {
        Workflow wf = one_agent_node();
        check_error(check_workflow_executable(wf), failure_class::contract,
                    "workflow.executor_kind_unsupported",
                    "G1: the contexts-FREE overload still refuses agent-kind (unchanged regression)");

        Workflow sub = one_agent_node();
        sub.executors.front().kind = executor_kind::sub_workflow;
        check_error(check_workflow_executable(sub), failure_class::contract,
                    "workflow.executor_kind_unsupported",
                    "G1: the contexts-free overload still refuses sub_workflow-kind (unchanged)");
    }

    // ---- G2: contexts-aware overload -- no declared ceiling, no contexts -> accepted --------------
    {
        Workflow wf = one_agent_node();
        check(check_workflow_executable(wf, {}).has_value(),
              "G2: an agent-kind node with an empty capability_ceiling is accepted with no contexts");
    }

    // ---- G3: contexts-aware overload -- a satisfied ceiling is accepted ---------------------------
    {
        Workflow wf = one_agent_node({cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}});
        CapabilitySet const held = CapabilitySet::grant_root({cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}});
        std::vector<EffectContext> contexts(1);
        contexts[0].capabilities = borrow_capabilities(held);
        check(check_workflow_executable(wf, contexts).has_value(),
              "G3: a satisfied capability_ceiling is accepted");
    }

    // ---- G4: contexts-aware overload -- a declared ceiling with NO granted CapabilitySet fails ----
    {
        Workflow wf = one_agent_node({cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}});
        std::vector<EffectContext> contexts(1);  // default: contexts[0].capabilities == nullptr
        check_error(check_workflow_executable(wf, contexts), failure_class::policy,
                    "workflow.agent_capability_ceiling_unmet",
                    "G4: a declared ceiling with no granted CapabilitySet at all is rejected");
    }

    // ---- G5: contexts-aware overload -- a declared ceiling NOT subsumed by the grant fails --------
    {
        Workflow wf = one_agent_node({cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}});
        CapabilitySet const held = CapabilitySet::grant_root({cap::FsRead{.mount_id = "other-mount", .path_prefix = "", .size_cap_bytes = std::nullopt}});
        std::vector<EffectContext> contexts(1);
        contexts[0].capabilities = borrow_capabilities(held);
        check_error(check_workflow_executable(wf, contexts), failure_class::policy,
                    "workflow.agent_capability_ceiling_unmet",
                    "G5: a granted CapabilitySet that does not subsume the declared ceiling is "
                    "rejected");
    }

    // ---- G6: ADR-157 (issues #33/#38) -- sub_workflow now HAS a real runtime bridge
    // (WorkflowSupervisor::bind_sub_workflow()), so the contexts-aware overload no longer
    // unconditionally refuses the KIND -- mirroring agent-kind's own two-layer shape exactly. The
    // structural check that an executor DECLARED sub_workflow-kind is actually BOUND to a real
    // inner WorkflowSupervisor lives in WorkflowSupervisor::initialize() itself
    // (sub_workflow_kind_nodes_are_bound()), which this graph-only function has no way to see
    // (bindings are an rt:: concept, same reason agent-kind's own body-backing check lives there
    // too, not here). G1 above still proves the CONTEXTS-FREE overload's own unconditional refusal
    // is unchanged -- this function's own comment (workflow/graph.hpp) explains why that one stays
    // conservative.
    {
        Workflow sub = one_agent_node();
        sub.executors.front().kind = executor_kind::sub_workflow;
        std::vector<EffectContext> contexts(1);
        check(check_workflow_executable(sub, contexts).has_value(),
              "G6: sub_workflow is accepted by the contexts-aware overload (ADR-157) -- the real "
              "bound/unbound check now lives in WorkflowSupervisor::initialize() itself");
    }

    // ---- G7: TypedExecutor's capability_ceiling escape hatch round-trips through describe() -------
    {
        TypedExecutor<Q, A> te{.id = "a",
                                .kind = executor_kind::agent,
                                .capability_ceiling = {cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}}};
        Executor described = te.describe();
        check(described.capability_ceiling.size() == 1,
              "G7: TypedExecutor::capability_ceiling forwards through describe()");
    }

    // ---- G8: the YAML compiler refuses an authored capability_ceiling key, loudly -----------------
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: gate-yaml }
spec:
  start: a
  executors:
    - { id: a, kind: agent, input_type: Q, output_type: A, capability_ceiling: [fs_read] }
  edges: []
  limits: { max_rounds: 1 }
  output_from: a
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "G8 setup: the document parses");
        if (parsed.has_value()) {
            auto compiled = compile_workflow_document(*parsed);
            check_error(compiled, failure_class::contract,
                        "yaml_compiler.executor_capability_ceiling_unsupported",
                        "G8: an authored capability_ceiling: key is refused loudly, not silently "
                        "dropped (I6)");
        }
    }

    // ---- G9: the YAML compiler is unaffected when no capability_ceiling key is present (I6 --------
    // ---- equivalence baseline: an ordinary agent-kind node compiles identically either surface) ---
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: gate-yaml-2 }
spec:
  start: a
  executors:
    - { id: a, kind: agent, input_type: Q, output_type: A }
  edges: []
  limits: { max_rounds: 1 }
  output_from: a
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "G9 setup: the document parses");
        if (parsed.has_value()) {
            auto compiled = compile_workflow_document(*parsed);
            check(compiled.has_value(), "G9: no capability_ceiling key -> compiles fine");
            if (compiled.has_value()) {
                check(compiled->executors.size() == 1 &&
                          compiled->executors.front().capability_ceiling.empty(),
                      "G9: capability_ceiling stays empty, matching the C++ form's own default");
            }
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_agent_executor_gate: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_agent_executor_gate: %d FAILURE(S)\n", g_failures);
    return 1;
}
