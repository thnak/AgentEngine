// Implements 014-Workflow-and-Orchestration.md §7's live-view bullet: "a running workflow exposes
// a live view of executor states, in-flight messages, and round number" over `ae::stream<T>`.
// Milestone 6 Phase G (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// Reuses test_workflow_checkpoint.cpp's own graph (draft -> ready -> [approve] -> publish, which
// crosses THREE superstep boundaries before suspending -- see that file's own header note on why
// reaching a port costs its own round) so the live-view event stream can be checked against the
// SAME already-proven round/executor-state shape, from a different angle: what a live viewer sees
// while the run is happening, not what a checkpoint restores afterward.

#include <chrono>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/live_view.hpp"
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

[[nodiscard]] ExecutorBody failer(std::string message) {
    return [message = std::move(message)](Message const&, EffectContext&) -> ae::result<ExecutorOutcome> {
        return std::unexpected(ae::error{failure_class::fatal, message, "test.injected_failure"});
    };
}

constexpr std::uint64_t kSupervisorKey = 100;

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

    Harness(EngineConfig const& cfg, std::size_t node_count)
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

    [[nodiscard]] ActorRef<WorkflowSupervisor> install(Workflow const& wf) {
        for (std::size_t i = 0; i < nodes.size(); ++i) refs.push_back(router.get<FunctionExecutor>(keys[i]));
        supervisor.initialize(wf, refs);
        sup_act = make_workflow_activation(supervisor, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        engine.start();
        return router.get<WorkflowSupervisor>(kSupervisorKey);
    }
};

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{id, executor_kind::request_port, "T", "T"};
}

// Drains up to `want` events with a bounded deadline -- pushes already happened synchronously
// inside the completed/suspended ask by the time this runs, so this is a formality against
// scheduling jitter, not a real wait for future work (mirrors test_chat_client_stream.cpp's own
// `while (!s.done()) { while (auto x = s.next()) ...; yield(); }` idiom, bounded by count instead
// of `done()` since the producer -- a WorkflowSupervisor member -- outlives any one run and never
// closes on its own).
[[nodiscard]] std::vector<WorkflowLiveEvent> drain(ae::stream<WorkflowLiveEvent>& s, std::size_t want) {
    std::vector<WorkflowLiveEvent> out;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
        if (auto ev = s.next()) {
            out.push_back(std::move(*ev));
        } else {
            std::this_thread::yield();
        }
    }
    return out;
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // =========================================================================================
    // LV1 -- draft -> ready -> [approve] -> publish. Three superstep boundaries; the live view
    // should report exactly what test_workflow_checkpoint.cpp's own F2 block already established
    // each one looks like, from the executor-state angle instead of the checkpoint angle.
    // =========================================================================================
    {
        Workflow wf;
        wf.id        = "lv-hitl";
        wf.executors = {node_desc("draft"), node_desc("ready"), port_desc("approve"), node_desc("publish")};
        wf.edges     = {Edge{"draft", "ready", edge_kind::direct, {}, {}},
                        Edge{"ready", "approve", edge_kind::direct, {}, {}},
                        Edge{"approve", "publish", edge_kind::direct, {}, {}}};
        wf.start            = "draft";
        wf.output_selection = {"publish"};
        wf.bound.max_rounds = 8;

        Harness h(*config, 4);
        h.add_node("draft", appender("draft"));
        h.add_node("ready", appender("ready"));
        h.add_node("approve", appender("approve"));
        h.add_node("publish", appender("publish"));

        auto sup    = h.install(wf);
        auto viewer = h.supervisor.enable_live_view(std::pmr::get_default_resource());

        auto r1 = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r1 && r1->status == workflow_status::suspended, "LV1 setup: the run suspends (sanity)");

        auto events = drain(viewer, 3);
        check(events.size() == 3, "LV1: exactly one live event per superstep boundary, same count "
                                  "test_workflow_checkpoint.cpp's F2 block established");

        if (events.size() == 3) {
            check(events[0].round == 1 && events[0].executor_states.size() == 1 &&
                      events[0].executor_states[0].executor_id == "draft" &&
                      events[0].executor_states[0].state == executor_live_state::ran_ok &&
                      events[0].in_flight_message_count == 1,
                  "LV1: event 1 -- draft ran ok, one message now in flight (to 'ready')");
            check(events[1].round == 2 && events[1].executor_states.size() == 1 &&
                      events[1].executor_states[0].executor_id == "ready" &&
                      events[1].executor_states[0].state == executor_live_state::ran_ok &&
                      events[1].in_flight_message_count == 1,
                  "LV1: event 2 -- ready ran ok, one message in flight (to 'approve', not yet "
                  "classified as a port this round)");
            check(events[2].round == 3 && events[2].executor_states.size() == 1 &&
                      events[2].executor_states[0].executor_id == "approve" &&
                      events[2].executor_states[0].state == executor_live_state::port_open &&
                      events[2].in_flight_message_count == 0,
                  "LV1: event 3 -- 'approve' opened as a port, nothing else in flight");
        }

        // Resuming for real should produce a FOURTH event (publish's own round) -- the live view
        // keeps working across a suspend/resume boundary, not just for the first ask.
        auto const open = h.supervisor.open_interactions();
        check(open.size() == 1, "LV1: one open port to resume (sanity)");
        std::string const interaction_id = open.empty() ? std::string{} : open.front().interaction_id;
        auto              r2             = block_on(
            sup.ask<WorkflowResult>(ResumeWorkflow{interaction_id, text_message("approved"), {}}));
        check(r2 && r2->status == workflow_status::completed, "LV1 resume: the run completes (sanity)");

        auto more = drain(viewer, 1);
        check(more.size() == 1 && more[0].round == 4 && more[0].executor_states.size() == 1 &&
                  more[0].executor_states[0].executor_id == "publish" &&
                  more[0].executor_states[0].state == executor_live_state::ran_ok,
              "LV1: resuming produces a further live event for publish's own round -- the SAME "
              "producer keeps working across a suspend/resume boundary");

        h.engine.stop();
    }

    // =========================================================================================
    // LV2 -- a round that ends the run via a `fail`-policy failure produces NO live event for
    // that round (the same `broke` branch checkpoint_hook_ already skips, live_view.hpp's own
    // header comment names this explicitly rather than leaving it to be discovered).
    // =========================================================================================
    {
        Workflow wf;
        wf.id                = "lv-fail";
        wf.executors         = {node_desc("ok_node"), node_desc("bad_node")};
        wf.edges             = {Edge{"ok_node", "bad_node", edge_kind::direct, {}, {}}};
        wf.start             = "ok_node";
        wf.bound.max_rounds  = 4;

        Harness h(*config, 2);
        h.add_node("ok_node", appender("ok"));
        h.add_node("bad_node", failer("boom"));

        auto sup    = h.install(wf);
        auto viewer = h.supervisor.enable_live_view(std::pmr::get_default_resource());

        auto r = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r && r->status == workflow_status::executor_failed,
              "LV2 setup: the second round fails the whole run (sanity)");

        // Exactly ONE event: round 1 (ok_node succeeding). Round 2 (bad_node failing, ending the
        // run) produces none.
        auto events = drain(viewer, 1);
        check(events.size() == 1 && events[0].round == 1 &&
                  events[0].executor_states[0].state == executor_live_state::ran_ok,
              "LV2: the successful first round IS reported");

        // Prove there really is nothing further sitting in the ring, rather than merely not having
        // waited long enough for it.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto const extra = viewer.next();
        check(!extra.has_value(),
              "LV2: the failing round produced NO live event -- not a timing artefact, nothing is "
              "buffered after a real wait");

        h.engine.stop();
    }

    if (g_failures == 0) {
        std::printf("test_workflow_live_view: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_live_view: %d failure(s)\n", g_failures);
    return 1;
}
