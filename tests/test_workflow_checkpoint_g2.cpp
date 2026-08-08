// Implements 014-Workflow-and-Orchestration.md §8 G2 (the promotion gate): "checkpoint/resume: kill
// at each superstep boundary of a 20-node workflow; every resume completes with output identical
// to the uninterrupted control." Milestone 6 Phase F
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// A 20-node sequential chain, run once as the CONTROL (capturing every superstep-boundary
// checkpoint along the way via `set_checkpoint_hook`), then for EVERY one of those 20 checkpoints,
// a genuinely NEW `WorkflowSupervisor` instance is `restore_from_record()`'d from exactly that
// checkpoint and driven to completion via `ContinueWorkflow` -- simulating "the process died right
// after this superstep's checkpoint was written, and something else picked the run back up." G2's
// own bar is that EVERY one of those 20 restarts reaches the SAME final output as the control, not
// just one or two convenient points -- this is the test that would catch a design that only
// happened to survive a restart at ONE boundary (e.g. only the very last one) while silently
// corrupting the rest.
//
// A sequential chain rather than a request port (test_workflow_checkpoint.cpp's own graph) is
// deliberate: G2's text is about the ORDINARY superstep boundary every round crosses, not the
// HITL-specific one Phase E already proved recovers (test_workflow_checkpoint.cpp's F2b). Together
// the two files cover both shapes checkpoint.hpp's own record has to restore exactly.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards per engine, 21 short-lived engines in sequence
// (never concurrent), bounded rounds, no sleeps in this file.

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/workflow/checkpoint.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/placement.hpp"
#include "agentengine/workflow/supervisor.hpp"

using namespace quark;
using namespace agentengine;
using namespace agentengine::workflow;

namespace {

int  g_failures = 0;
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
    item.value = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += ">";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        return text_message(all_text_of(in) + ">" + name);
    };
}

constexpr std::uint64_t kSupervisorKey = 100;
constexpr std::size_t   kNodeCount     = 20;

struct Harness {
    Engine<>            engine;
    detail::MessagePool pool{64};
    LocalRouter         router;

    std::vector<std::unique_ptr<FunctionExecutor>> nodes;
    std::vector<std::unique_ptr<Activation>>       node_acts;
    WorkflowSupervisor                             supervisor;
    std::unique_ptr<Activation>                    sup_act;
    std::vector<ActorRef<FunctionExecutor>>        refs;
    std::vector<std::uint64_t>                     keys;

    explicit Harness(EngineConfig const& cfg)
        : engine(cfg), router(engine.post_courier(), pool) {
        keys = spread_executor_keys(kNodeCount, engine.shard_count(), [this](std::uint64_t k) {
            return engine.shard_of(actor_id_of<FunctionExecutor>(k));
        });
    }

    void add_node(std::string name) {
        auto node = std::make_unique<FunctionExecutor>();
        node->initialize(name, appender(name), EffectContext{});
        auto act = make_workflow_activation(*node, pool.sink());
        engine.register_activation(actor_id_of<FunctionExecutor>(keys[nodes.size()]), *act);
        nodes.push_back(std::move(node));
        node_acts.push_back(std::move(act));
    }

    [[nodiscard]] ActorRef<WorkflowSupervisor> install(Workflow const& wf) {
        for (std::size_t i = 0; i < nodes.size(); ++i) refs.push_back(router.get<FunctionExecutor>(keys[i]));
        supervisor.initialize(wf, refs);
        sup_act = make_workflow_activation(supervisor, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        engine.start();
        return router.get<WorkflowSupervisor>(kSupervisorKey);
    }
};

[[nodiscard]] Workflow build_chain_graph() {
    Workflow wf;
    wf.id = "g2-chain";
    for (std::size_t i = 0; i < kNodeCount; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "n%02zu", i);
        wf.executors.push_back(Executor{buf, executor_kind::function, "T", "T"});
    }
    for (std::size_t i = 0; i + 1 < kNodeCount; ++i) {
        wf.edges.push_back(Edge{wf.executors[i].id, wf.executors[i + 1].id, edge_kind::direct, {}, {}});
    }
    wf.start            = wf.executors.front().id;
    wf.output_selection = {wf.executors.back().id};
    wf.bound.max_rounds = static_cast<std::uint32_t>(kNodeCount) + 5;
    return wf;
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    Workflow const wf = build_chain_graph();
    check(validate_workflow(wf).has_value(), "G2 setup: the 20-node chain validates");

    // =========================================================================================
    // The CONTROL: one uninterrupted run, capturing every superstep-boundary checkpoint along
    // the way (checkpoint.hpp's own RunStateRecord, via to_record()).
    // =========================================================================================
    std::vector<RunStateRecord> checkpoints;
    WorkflowResult               control_result;
    {
        auto h = std::make_unique<Harness>(*config);
        for (std::size_t i = 0; i < kNodeCount; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "n%02zu", i);
            h->add_node(buf);
        }
        h->supervisor.set_checkpoint_hook(
            [&](std::uint32_t /*round*/, RunStateRecord const& rec) { checkpoints.push_back(rec); });

        auto sup = h->install(wf);
        auto r   = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value(), "G2 control: the run completes");
        if (r) control_result = *r;
        h->engine.stop();
    }

    check(control_result.status == workflow_status::completed,
          "G2 control: the chain runs dry and completes, not bounded/failed");
    check(checkpoints.size() == kNodeCount,
          "G2 control: exactly one checkpoint per node/round -- a 20-node chain crosses 20 "
          "superstep boundaries");
    std::string const control_output = all_text_of(control_result.output);
    std::fprintf(stderr, "  .. G2 control output = %s\n", control_output.c_str());
    check(control_output == "in>n00>n01>n02>n03>n04>n05>n06>n07>n08>n09>n10>n11>n12>n13>n14>n15>"
                            "n16>n17>n18>n19",
          "G2 control: the chain threaded the payload through every node in graph order (sanity)");

    // =========================================================================================
    // G2 ITSELF: for EVERY one of the 20 checkpoints, a genuinely NEW instance restores from
    // exactly that one and continues -- simulating a kill right after that superstep's checkpoint
    // was durable, and something else picking the run back up from nothing but the record.
    // =========================================================================================
    std::size_t matched = 0;
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        auto h = std::make_unique<Harness>(*config);
        for (std::size_t n = 0; n < kNodeCount; ++n) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "n%02zu", n);
            h->add_node(buf);
        }
        auto sup = h->install(wf);
        h->supervisor.restore_from_record(checkpoints[i]);

        auto r = block_on(sup.ask<WorkflowResult>(ContinueWorkflow{}));
        bool const ok = r.has_value() && r->status == workflow_status::completed &&
                        all_text_of(r->output) == control_output;
        if (ok) ++matched;
        if (!ok) {
            std::fprintf(stderr, "  .. G2 mismatch at checkpoint %zu (round=%u): status=%d output=%s\n",
                        i, checkpoints[i].rounds, r ? static_cast<int>(r->status) : -1,
                        r ? all_text_of(r->output).c_str() : "<ask failed>");
        }
        h->engine.stop();
    }
    check(matched == checkpoints.size(),
          "G2 (014 §8 G2): killed at EVERY one of the 20 superstep boundaries, every single resume "
          "completed with output IDENTICAL to the uninterrupted control -- not just the convenient "
          "first or last one");
    std::fprintf(stderr, "  .. G2: %zu/%zu checkpoints resumed to the identical control output\n",
                matched, checkpoints.size());

    if (g_failures == 0) {
        std::printf("test_workflow_checkpoint_g2: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_checkpoint_g2: %d failure(s)\n", g_failures);
    return 1;
}
