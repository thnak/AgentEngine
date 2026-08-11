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
// Run: ./agentengine_example_13_reflection_loop

#include <cstdio>
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

// Both nodes just tag the running draft with their own name and round -- a stand-in for a real
// writer producing a draft and a real critic appending feedback. What matters for this example is
// the GRAPH shape (a bounded cycle), not what either node's body actually does.
[[nodiscard]] ExecutorBody appender(std::string name) {
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

    Harness h(*config, /*node_count=*/2);
    h.add_node("writer", appender("write"));
    h.add_node("critic", appender("critique"));

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

    WorkflowSupervisor           supervisor;
    std::unique_ptr<Activation>  sup_act;
    ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);

    h.engine.start();
    quark::result<WorkflowResult> r = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("draft")}));
    check(r.has_value(), "the workflow run completes (in the sense of returning a result)");
    if (r.has_value()) {
        check(r->status == workflow_status::bound_max_rounds,
              "the bound stopped the loop -- an honest status distinct from workflow_status::"
              "completed, telling the caller WHY it ended rather than leaving them to guess");
        check(r->rounds == 6, "exactly max_rounds rounds ran, no more");
        std::string const output = text_of(r->output);
        std::printf("%s\n", output.c_str());
        check(output == "draft>write>critique>write>critique>write>critique",
              "the draft threaded through 3 full writer/critic cycles before the bound stopped it");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_13_reflection_loop: OK\n"
                                          : "example_13_reflection_loop: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
