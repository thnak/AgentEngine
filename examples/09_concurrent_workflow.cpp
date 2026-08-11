// AgentEngine "get started" examples, 9 -- fan-out / fan-in (014 §3's "Concurrent" pattern).
//
// Mirrors MAF's samples/03-workflows/Concurrent: one source hands off to several workers that run
// in the SAME superstep round, and a single aggregator merges their results once ALL of them have
// replied. In AgentEngine this needs no separate "concurrent executor" type -- it's the same
// `Workflow` data 04_first_workflow.cpp used, just with `edge_kind::fan_out` (one source, many
// targets, all fired) and `edge_kind::fan_in` (many sources, one target, merged into ONE delivery,
// not one call per inbound edge -- 014 §2's "the superstep model makes fan-in well-defined").
//
// The aggregator receiving exactly one call, not three, is the property this example measures, not
// just infers from correct-looking output -- a fan-in that silently degraded into three separate
// calls would still produce a plausible answer.
//
// Run: ./agentengine_example_09_concurrent_workflow

#include <algorithm>
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

// Fan-in delivers one ContentItem per contributing branch, in graph-declared source order -- an
// aggregator reads them by joining every Text item it was handed.
[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += " + ";
            out += t->text;
        }
    }
    return out;
}

// A worker: uppercases whatever text it's handed and tags it with its own name.
[[nodiscard]] ExecutorBody worker(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        std::string text = text_of(in);
        std::transform(text.begin(), text.end(), text.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text_message(name + ":" + text);
    };
}

[[nodiscard]] agentengine::result<Message> source(Message const& in, EffectContext&) {
    return text_message(text_of(in));
}

[[nodiscard]] agentengine::result<Message> aggregate(Message const& in, EffectContext&) {
    return text_message(all_text_of(in));
}

constexpr std::uint64_t kSupervisorKey = 100;

// Same construction shape as 04_first_workflow.cpp, generalized to N nodes -- `Activation` is
// non-movable, so each node's own `unique_ptr` keeps its address stable for the Activation wrapping
// it.
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

    Harness h(*config, /*node_count=*/5);
    h.add_node("src", source);
    h.add_node("upper", worker("upper"));
    h.add_node("lower", worker("lower"));   // still uppercases -- the NAME is what distinguishes it
    h.add_node("title", worker("title"));
    h.add_node("agg", aggregate);

    Workflow wf;
    wf.id        = "fan-out-fan-in";
    wf.executors = {node_desc("src"), node_desc("upper"), node_desc("lower"), node_desc("title"),
                     node_desc("agg")};
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "src";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    check(validate_workflow(wf).has_value(), "the graph validates");

    WorkflowSupervisor           supervisor;
    std::unique_ptr<Activation>  sup_act;
    ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);

    h.engine.start();
    quark::result<WorkflowResult> r = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("hi")}));
    check(r.has_value(), "the workflow run completes");
    if (r.has_value()) {
        check(r->status == workflow_status::completed, "the graph terminates by running dry (014 §2)");
        check(r->rounds == 3, "src, workers, aggregator = 3 rounds -- the fan-out round runs ONCE, "
                               "not once per worker");
        check(h.nodes[4]->invocations() == 1,
              "the aggregator ran EXACTLY ONCE -- three inbound fan_in edges merged into one "
              "delivery, not three separate calls (014 §2's own claim: 'makes fan-in well-defined')");
        std::string const output = all_text_of(r->output);
        std::printf("%s\n", output.c_str());
        check(output == "upper:HI + lower:HI + title:HI",
              "the aggregator saw all three branches, in graph-declared order");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_09_concurrent_workflow: OK\n"
                                          : "example_09_concurrent_workflow: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
