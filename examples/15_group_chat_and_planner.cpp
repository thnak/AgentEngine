// AgentEngine "get started" examples, 15 -- Group Chat and Planner (Magentic), the SAME graph shape.
//
// 014 §3 gives these two named patterns the identical graph shape -- "a moderator executor cycling
// among participants" -- and distinguishes them by exactly one thing: WHO owns the stopping
// decision. This example builds that one shape twice, changing only the moderator's own body, to
// make that distinction observable rather than asserted:
//
//   - Group Chat: the CALLER's round bound is the termination contract. The moderator just keeps
//     cycling between participants; nothing inside the graph ever decides "we're done" -- the run
//     ends at `bound.max_rounds`, and `workflow_status::bound_max_rounds` says so honestly.
//   - Planner (Magentic): the SAME cyclic shape, but the moderator keeps its own ledger and routes
//     to a terminal `done` node once the goal is met. The round bound is only a safety valve, set
//     far above what the ledger should ever need -- the run ends by running dry
//     (`workflow_status::completed`), well before the bound.
//
// Both use `edge_kind::switch_case` for the moderator's own routing choice (which participant, or
// "done") and plain `edge_kind::direct` edges for each participant's reply back to the moderator --
// the same two edge kinds 10_conditional_routing.cpp and 13_reflection_loop.cpp already introduced,
// just composed into a cycle with a routing decision at one end of it.
//
// Run: ./agentengine_example_15_group_chat_and_planner

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

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + ">" + name);
    };
}

// A moderator: counts how many times "moderator" already appears in the transcript (i.e. how many
// participant turns have happened) and hands that count to `choose`, an ordinary function of the
// input -- a model-backed moderator would fill this same slot in a real agent-kind node.
[[nodiscard]] ExecutorBody moderator(std::function<std::string(std::size_t)> choose) {
    return [choose = std::move(choose)](Message const& in,
                                         EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        std::size_t       turns = 0;
        for (std::size_t i = 0; i + 1 < text.size(); ++i) {
            if (text[i] == '>' && text[i + 1] == 'm') ++turns;
        }
        return ExecutorOutcome{text_message(text + ">mod"), {choose(turns)}};
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

    // ---- Group Chat: the moderator cycles forever; the CALLER's bound stops it -------------------
    {
        Harness h(*config, /*node_count=*/3);
        h.add_node("moderator", moderator([](std::size_t turns) { return turns % 2 == 0 ? "p1" : "p2"; }));
        h.add_node("p1", appender("p1"));
        h.add_node("p2", appender("p2"));

        Workflow wf;
        wf.id        = "group-chat";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("p1");
        wf.output_selection.push_back("p2");
        wf.bound.max_rounds = 6;
        check(validate_workflow(wf).has_value(), "group chat: the moderator-cycle graph validates");

        WorkflowSupervisor           supervisor;
        std::unique_ptr<Activation>  sup_act;
        ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("discuss")}));
        check(r.has_value(), "group chat: the run completes (in the sense of returning a result)");
        if (r.has_value()) {
            check(r->status == workflow_status::bound_max_rounds,
                  "group chat: the CALLER's round bound is the termination contract -- nothing "
                  "inside the graph ever decided to stop");
            check(h.nodes[1]->invocations() > 0 && h.nodes[2]->invocations() > 0,
                  "group chat: the moderator cycled among BOTH participants, not just one");
            std::printf("[group chat] %s\n", text_of(r->output).c_str());
        }
    }

    // ---- Planner (Magentic): the SAME shape, but the moderator's own ledger owns completion ------
    {
        Harness h(*config, /*node_count=*/4);
        h.add_node("moderator", moderator([](std::size_t turns) {
                       if (turns >= 2) return std::string("done");  // the ledger: goal met after 2 turns
                       return turns % 2 == 0 ? std::string("p1") : std::string("p2");
                   }));
        h.add_node("p1", appender("p1"));
        h.add_node("p2", appender("p2"));
        h.add_node("done", appender("done"));

        Workflow wf;
        wf.id        = "planner";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2"), node_desc("done")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"moderator", "done", edge_kind::switch_case, "done"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 20;  // a safety valve, deliberately far above what the ledger needs
        check(validate_workflow(wf).has_value(), "planner: the same cyclic shape validates");

        WorkflowSupervisor           supervisor;
        std::unique_ptr<Activation>  sup_act;
        ActorRef<WorkflowSupervisor> sup = h.install(wf, supervisor, sup_act);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("plan")}));
        check(r.has_value(), "planner: the run returns");
        if (r.has_value()) {
            check(r->status == workflow_status::completed,
                  "planner: the MODERATOR decided completion and routed to the terminal node -- the "
                  "run ended by running dry, NOT at its bound (the one observable difference from "
                  "group chat above, which shares this graph shape exactly)");
            check(r->rounds < 20, "planner: the round bound was a safety valve, never reached");
            std::printf("[planner]    %s (after %llu rounds, bound was 20)\n",
                        text_of(r->output).c_str(), static_cast<unsigned long long>(r->rounds));
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "example_15_group_chat_and_planner: OK\n"
                                          : "example_15_group_chat_and_planner: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
