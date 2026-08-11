// AgentEngine "get started" examples, 14 -- a caller-chosen SUBSET fires (multi_selection).
//
// The sixth edge kind, with no MAF sample of its own to mirror -- and no 014 §3 pattern row of its
// own either (it's the "sixth edge kind" the eight named patterns don't individually cover).
// `edge_kind::multi_selection` sits between the two edge kinds the other examples already show:
// `switch_case` (10_conditional_routing.cpp) selects exactly ONE branch; `fan_out`
// (09_concurrent_workflow.cpp) always fires ALL of them. `multi_selection` selects a caller-chosen
// SUBSET -- more than one is allowed (unlike switch_case), and fewer than all is allowed (unlike
// fan_out). The routing mechanism is identical to switch_case's: the source executor returns the
// case labels it selects in `ExecutorOutcome::routes`; only the edges whose label appears there
// fire.
//
// Scenario: a change classifier picks WHICH reviewers a change needs -- security and docs here,
// skipping performance -- rather than either asking one fixed reviewer or all three every time.
//
// Run: ./agentengine_example_14_multi_selection

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/placement.hpp"
#include "agentengine/workflow/supervisor.hpp"

using namespace quark;
using namespace agentengine;
using namespace agentengine::workflow;

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

// Picks which reviewers this change needs -- an ordinary function of the input here; a model-backed
// classifier would fill this same slot in a real agent-kind node.
[[nodiscard]] ExecutorBody pick_reviewers(std::vector<std::string> selected) {
    return [selected = std::move(selected)](Message const& in,
                                             EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ">triage"), selected};
    };
}

[[nodiscard]] ExecutorBody reviewer(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + ">" + name);
    };
}

constexpr std::uint64_t kSupervisorKey = 100;

struct Harness {
    Engine<>            engine;
    detail::MessagePool pool{64};
    LocalRouter          router;
    std::vector<std::unique_ptr<FunctionExecutor>> nodes;
    std::vector<std::unique_ptr<Activation>>       node_acts;
    std::vector<std::uint64_t>                     keys;

    explicit Harness(EngineConfig const& cfg, std::size_t node_count)
        : engine(cfg), router(engine.post_courier(), pool) {
        keys = spread_executor_keys(node_count, engine.shard_count(), [this](std::uint64_t k) {
            return engine.shard_of(actor_id_of<FunctionExecutor>(k));
        });
    }

    void add_node(std::string name, ExecutorBody body) {
        auto node = std::make_unique<FunctionExecutor>();
        node->initialize(std::move(name), std::move(body), EffectContext{});
        auto act = make_workflow_activation(*node, pool.sink());
        engine.register_activation(actor_id_of<FunctionExecutor>(keys[nodes.size()]), *act);
        nodes.push_back(std::move(node));
        node_acts.push_back(std::move(act));
    }

    [[nodiscard]] ActorRef<WorkflowSupervisor> install(Workflow const& wf, WorkflowSupervisor& sup,
                                                        std::unique_ptr<Activation>& sup_act) {
        std::vector<ActorRef<FunctionExecutor>> refs;
        for (std::size_t i = 0; i < nodes.size(); ++i) refs.push_back(router.get<FunctionExecutor>(keys[i]));
        sup.initialize(wf, refs);
        sup_act = make_workflow_activation(sup, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        return router.get<WorkflowSupervisor>(kSupervisorKey);
    }
};

[[nodiscard]] Executor node_desc(char const* id) { return Executor{id, executor_kind::function, "T", "T"}; }

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(config.has_value(), "engine config builds (4 workers / 4 shards)");
    if (!config) return 1;

    Harness h(*config, /*node_count=*/4);
    h.add_node("triage", pick_reviewers({"security", "docs"}));  // skips "performance"
    h.add_node("security", reviewer("security"));
    h.add_node("performance", reviewer("performance"));
    h.add_node("docs", reviewer("docs"));

    Workflow wf;
    wf.id        = "reviewer-selection";
    wf.executors = {node_desc("triage"), node_desc("security"), node_desc("performance"),
                     node_desc("docs")};
    wf.edges.push_back(Edge{"triage", "security", edge_kind::multi_selection, "security"});
    wf.edges.push_back(Edge{"triage", "performance", edge_kind::multi_selection, "performance"});
    wf.edges.push_back(Edge{"triage", "docs", edge_kind::multi_selection, "docs"});
    wf.start = "triage";
    wf.output_selection.push_back("docs");
    wf.bound.max_rounds = 8;
    check(validate_workflow(wf).has_value(), "the graph validates");

    WorkflowSupervisor           supervisor;
    std::unique_ptr<Activation>  sup_act;
    ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);

    h.engine.start();
    quark::result<WorkflowResult> r =
        block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("change #482")}));
    check(r.has_value(), "the run completes");
    if (r.has_value()) {
        check(r->status == workflow_status::completed, "the graph terminates by running dry");
        std::printf("%s\n", text_of(r->output).c_str());
        check(h.nodes[1]->invocations() == 1 && h.nodes[3]->invocations() == 1,
              "BOTH selected reviewers (security, docs) ran -- more than one, unlike switch_case");
        check(h.nodes[2]->invocations() == 0,
              "the unselected reviewer (performance) never ran -- fewer than all, unlike fan_out");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_14_multi_selection: OK\n"
                                          : "example_14_multi_selection: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
