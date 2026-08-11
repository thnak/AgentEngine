// AgentEngine "get started" examples, 10 -- conditional routing (014 §3's "Router" pattern).
//
// Mirrors MAF's samples/03-workflows/ConditionalEdges: one executor's own output decides which of
// several downstream branches actually runs. In AgentEngine this is `edge_kind::switch_case`: each
// candidate edge out of the router carries a `case_label` (`Edge::case_label`, graph.hpp), and the
// router signals which one fired by returning it in `ExecutorOutcome::routes` -- a
// `std::vector<std::string>` of the case labels THIS call selects, out of the ones the graph
// actually declares. Only the edge(s) whose label appears in `routes` fire; the rest never run,
// which is what this example measures (an invocation counter), not just infers from output text.
//
// Run: ./agentengine_example_10_conditional_routing

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

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(config.has_value(), "engine config builds (4 workers / 4 shards)");
    if (!config) return 1;

    Workflow const wf = make_graph();
    check(validate_workflow(wf).has_value(), "the graph validates");

    // Run the SAME graph against two different inputs -- proving the classifier's output steers the
    // route, not that one branch is hardcoded to always fire.
    for (std::string const& input : {std::string("my invoice is wrong"), std::string("the app crashed")}) {
        Harness h(*config, 3);
        h.add_node("triage", triage([](std::string const& text) {
                       return text.find("invoice") != std::string::npos ? "billing" : "tech";
                   }));
        h.add_node("billing", handler("billing"));
        h.add_node("tech", handler("tech"));

        WorkflowSupervisor           supervisor;
        std::unique_ptr<Activation>  sup_act;
        ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);

        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message(input)}));
        check(r.has_value(), ("the run completes for input: " + input).c_str());
        if (r.has_value()) {
            bool const expect_billing = input.find("invoice") != std::string::npos;
            std::printf("%s\n", all_text_of(r->output).c_str());
            check(h.nodes[1]->invocations() == (expect_billing ? 1u : 0u) &&
                      h.nodes[2]->invocations() == (expect_billing ? 0u : 1u),
                  "exactly ONE branch ran -- the unselected branch's invoke() was never called");
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "example_10_conditional_routing: OK\n"
                                          : "example_10_conditional_routing: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
