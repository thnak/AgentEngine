// Implements 014-Workflow-and-Orchestration.md §5 (checkpoint, resume) and the milestone-6
// breakdown doc's decision 7 (two-phase pending->committed) and decision 11 (a suspended run's
// state lives in the actor, which is what makes it a plain durable record). Milestone 6 Phase F
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// Two layers, tested separately then together:
//   F1 -- checkpoint.hpp's own mechanics against a bare `quark::InMemoryStore`: round-trip a
//         hand-built `RunStateRecord`, and the two-phase discipline's actual reason for existing --
//         a crash injected BETWEEN `stage_pending_checkpoint` and `commit_checkpoint` must leave
//         `load_workflow_checkpoint` reporting the PRIOR fully-committed checkpoint, never the
//         dangling one (decision 7 / 014 §9's resolved Q4, proven single-node).
//   F2 -- `WorkflowSupervisor::to_record()`/`restore_from_record()` against a REAL run: a workflow
//         that reaches a request port (so `partial`/`pending`/`ports` are all genuinely populated,
//         not vacuously empty), checkpointed via `set_checkpoint_hook()` at real superstep
//         boundaries, reloaded onto a FRESH supervisor instance, and driven to completion two
//         different ways -- `ContinueWorkflow` from a pre-suspension checkpoint, and a real
//         `ResumeWorkflow` from the post-suspension one. The latter is Phase E's own G5 second half
//         ("resumes correctly after a process restart"), which had nothing to restore FROM before
//         this file: a genuinely NEW actor instance, not the original one continuing in memory.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded rounds, no sleeps in this file.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"
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
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::yield();
    }
    return p();
}

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "+";
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

// Mirrors test_workflow_request_port.cpp's own Harness -- one engine, several asks against the
// same live supervisor, start/stop are the caller's.
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

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // =========================================================================================
    // F1 -- checkpoint.hpp's own mechanics, no actor involved: hand-built RunStateRecords against
    // a bare InMemoryStore, mirroring test_effect_journal.cpp's own "no TestKit needed" precedent
    // for pure Store-level tests.
    // =========================================================================================
    {
        InMemoryStore store;
        std::string const run_id = "ckpt-run-1";
        auto const        id     = workflow_run_actor_id(run_id);
        auto const        fence  = store.acquire_fence(id);

        RunStateRecord rec1;
        rec1.run_counter = 1;
        rec1.run_id      = run_id;
        rec1.rounds      = 3;
        rec1.pending.push_back(DeliveryRecord{2, to_record(text_message("hello"))});
        rec1.partial.push_back(ExecutorOutputRecord{"a", 0, to_record(text_message("a-out"))});
        rec1.selected_output = to_record(text_message("sel"));
        rec1.elapsed_ns      = 12345;

        // --- F1a: no checkpoint yet ------------------------------------------------------------
        auto none = load_workflow_checkpoint(store, run_id);
        check(none.has_value() && !none->has_value(), "F1a: a run with no checkpoint loads nullopt");

        // --- F1b: a fully committed checkpoint round-trips exactly ------------------------------
        auto save1 = save_workflow_checkpoint(store, run_id, fence, /*checkpoint_index=*/1, rec1);
        check(save1.has_value(), "F1b: save_workflow_checkpoint (both phases) succeeds");
        auto loaded1 = load_workflow_checkpoint(store, run_id);
        check(loaded1.has_value() && loaded1->has_value() && **loaded1 == rec1,
              "F1b: the loaded RunStateRecord equals the one saved, field for field");

        // --- F1c: THE core claim -- a crash between pending and committed falls back -----------
        RunStateRecord rec2 = rec1;
        rec2.rounds = 4;
        rec2.pending.clear();
        rec2.partial.push_back(ExecutorOutputRecord{"b", 3, to_record(text_message("b-out"))});
        auto stage2 = stage_pending_checkpoint(store, run_id, fence, /*checkpoint_index=*/2, rec2);
        check(stage2.has_value(), "F1c: stage_pending_checkpoint (phase 1 alone) succeeds");
        // `commit_checkpoint` for index 2 is DELIBERATELY never called -- simulating a crash right
        // between the two phases decision 7 names.
        auto loaded2 = load_workflow_checkpoint(store, run_id);
        check(loaded2.has_value() && loaded2->has_value() && **loaded2 == rec1,
              "F1c (decision 7's own claim): with checkpoint 2's committed marker missing, load "
              "falls back to checkpoint 1 -- the prior FULLY committed one -- not to checkpoint 2's "
              "dangling pending state and not to nullopt");

        // --- F1d: completing the second phase makes checkpoint 2 the new latest -----------------
        auto commit2 = commit_checkpoint(store, run_id, fence, /*checkpoint_index=*/2);
        check(commit2.has_value(), "F1d: commit_checkpoint (phase 2) succeeds once actually called");
        auto loaded3 = load_workflow_checkpoint(store, run_id);
        check(loaded3.has_value() && loaded3->has_value() && **loaded3 == rec2,
              "F1d: once committed, checkpoint 2 becomes the latest fully-committed one");

        // --- F1e: every fully-committed checkpoint is retained, oldest first --------------------
        auto retained = retained_checkpoints(store, run_id);
        check(retained.has_value() && retained->size() == 2 && (*retained)[0].first == 1 &&
                  (*retained)[0].second == rec1 && (*retained)[1].first == 2 &&
                  (*retained)[1].second == rec2,
              "F1e (014 §5 'rewind to ANY retained checkpoint'): both checkpoints 1 and 2 are "
              "retained in index order -- the dangling attempt never got a slot, and neither did "
              "the never-attempted index 0");
    }

    // =========================================================================================
    // F2 -- WorkflowSupervisor::to_record()/restore_from_record() against a REAL run:
    //
    //   draft -> ready -> [approve] -> publish
    //
    // Two rounds of real work before the port, so `partial` has two entries and `rounds_` is
    // genuinely > 1 by the time the run suspends -- not a vacuous single-node checkpoint. THREE
    // superstep boundaries are hit, not two: reaching a port costs its OWN round (the pending/port
    // partition happens at the TOP of a loop iteration, one iteration after 'approve' first became
    // pending), so round 1 runs draft, round 2 runs ready, and round 3 is the one that reaches
    // 'approve' and suspends with no executor invoked in it -- a real, non-obvious shape only found
    // by running this and counting, not by reading the loop and assuming "one port = one round".
    // =========================================================================================
    Workflow wf;
    wf.id        = "ckpt-hitl";
    wf.executors = {node_desc("draft"), node_desc("ready"), port_desc("approve"), node_desc("publish")};
    wf.edges     = {Edge{"draft", "ready", edge_kind::direct, {}, {}},
                    Edge{"ready", "approve", edge_kind::direct, {}, {}},
                    Edge{"approve", "publish", edge_kind::direct, {}, {}}};
    wf.start            = "draft";
    wf.output_selection = {"publish"};
    wf.bound.max_rounds = 8;

    InMemoryStore store2;
    auto const    ckpt_fence = store2.acquire_fence(workflow_run_actor_id("ckpt-hitl:run:1"));

    std::vector<RunStateRecord> captured;  // every checkpoint this run's hook saw, in round order

    {
        Harness h(*config, 4);
        h.add_node("draft", appender("draft"));
        h.add_node("ready", appender("ready"));
        h.add_node("approve", appender("approve"));  // a port -- registered, never invoked
        h.add_node("publish", appender("publish"));

        h.supervisor.set_checkpoint_hook([&](std::uint32_t round, RunStateRecord const& rec) {
            captured.push_back(rec);
            auto rc = save_workflow_checkpoint(store2, rec.run_id, ckpt_fence,
                                               /*checkpoint_index=*/round, rec);
            check(rc.has_value(), "F2: the checkpoint hook's own durable write succeeds");
        });

        auto sup = h.install(wf);
        auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));

        check(r1 && r1->status == workflow_status::suspended,
              "F2: the run reaches the port and suspends (sanity, same shape as Phase E's own E1)");
        check(captured.size() == 3,
              "F2: the hook fired once per superstep boundary -- round 1 (draft), round 2 (ready), "
              "round 3 (reaches 'approve' and suspends, no executor invoked that round)");
        if (captured.size() == 3) {
            check(captured[0].rounds == 1 && captured[0].pending.size() == 1 &&
                      captured[0].partial.size() == 1,
                  "F2: checkpoint 1 -- one round done (draft), one delivery pending (ready)");
            check(captured[1].rounds == 2 && captured[1].pending.size() == 1 &&
                      captured[1].partial.size() == 2 && captured[1].ports.empty(),
                  "F2: checkpoint 2 -- two rounds done (draft, ready), 'approve' pending but not yet "
                  "classified as a port this round -- ports_ is still empty here");
            check(captured[2].rounds == 3 && captured[2].pending.empty() &&
                      captured[2].partial.size() == 2 && captured[2].ports.size() == 1 &&
                      !captured[2].ports[0].resolved,
                  "F2: checkpoint 3 -- 'approve' classified as a port, opened, and the run is about "
                  "to suspend; partial is unchanged from checkpoint 2 since a port round produces "
                  "no executor output");
        }
        // `elapsed_ns` deliberately excluded: `finish()` banks one more slice of running time AFTER
        // the last checkpoint hook call and BEFORE the ask replies (see `RunState::elapsed_ns`'s own
        // comment), so the live value is expected to be slightly LARGER than what the hook saw --
        // not a staleness bug, the exact reason §2's deadline accounting exists.
        RunStateRecord live = h.supervisor.to_record();
        RunStateRecord last = captured.back();
        live.elapsed_ns = 0;
        last.elapsed_ns = 0;
        check(live == last,
              "F2: the live supervisor's OWN to_record() (elapsed_ns aside) matches the last "
              "checkpoint the hook captured -- to_record() is a pure snapshot, not a side-effecting "
              "call");

        h.engine.stop();
    }

    // ---- F2a: restore from the PRE-suspension checkpoint (round 1), continue via ContinueWorkflow
    {
        Harness h2(*config, 4);
        h2.add_node("draft", appender("draft"));
        h2.add_node("ready", appender("ready"));
        h2.add_node("approve", appender("approve"));
        h2.add_node("publish", appender("publish"));

        auto sup2 = h2.install(wf);  // install() calls initialize(wf, refs) with this instance's own refs
        h2.supervisor.restore_from_record(captured[0]);

        check(h2.supervisor.rounds_executed() == 1,
              "F2a: the restored (fresh) instance reports the checkpoint's own round number");
        check(h2.supervisor.run_id() == "ckpt-hitl:run:1",
              "F2a: the restored instance carries the ORIGINAL run's id, not a freshly minted one");

        auto r2 = block_on(sup2.ask<WorkflowResult>(ContinueWorkflow{}));
        check(r2 && r2->status == workflow_status::suspended,
              "F2a: continuing a restored, pre-suspension checkpoint reaches the SAME port and "
              "suspends -- identical to the uninterrupted control's own first suspension");
        check(r2 && r2->partial.size() == 2,
              "F2a: both draft's and ready's partial results are present after resuming from a "
              "checkpoint that only had draft's -- ready re-ran forward from the restored state "
              "and its output was recorded exactly as the control's was");
        check(h2.nodes[3]->invocations() == 0,
              "F2a: publish still did not run -- the restored run is genuinely suspended, not "
              "silently completed");

        h2.engine.stop();
    }

    // ---- F2b: restore from the POST-suspension checkpoint (round 3), resume for real -- Phase E's
    // G5 second half ("resumes correctly after a process restart"), on a genuinely NEW instance.
    {
        Harness h3(*config, 4);
        h3.add_node("draft", appender("draft"));
        h3.add_node("ready", appender("ready"));
        h3.add_node("approve", appender("approve"));
        h3.add_node("publish", appender("publish"));

        auto sup3 = h3.install(wf);
        h3.supervisor.restore_from_record(captured[2]);

        auto const open = h3.supervisor.open_interactions();
        check(open.size() == 1, "F2b: the restored instance's open port survived the round-trip");
        std::string const interaction_id = open.empty() ? std::string{} : open.front().interaction_id;
        if (!open.empty()) note("F2b restored interaction", interaction_id);

        auto r3 = block_on(
            sup3.ask<WorkflowResult>(ResumeWorkflow{interaction_id, text_message("approved"), {}}));
        check(r3 && r3->status == workflow_status::completed,
              "F2b (§8 G5, second half): a genuinely NEW actor instance -- never having run a single "
              "round itself -- answers the SAME interaction id and completes the run, exactly as "
              "the original, uninterrupted instance did in the control above");
        // "approved>publish", not the full "in>draft>ready>approved>publish" chain -- a port has no
        // body, so its OWN "output" is defined as exactly the human's response (E1's own precedent,
        // test_workflow_request_port.cpp), which REPLACES the upstream accumulated text rather than
        // extending it. This is what an uninterrupted control run produces too; restoring from a
        // checkpoint changes WHERE the run continues, not the routing rule that decides WHAT a
        // port's downstream neighbour receives.
        check(r3 && all_text_of(r3->output) == "approved>publish",
              "F2b: the completed output matches what an uninterrupted run would have produced -- "
              "restoring from a checkpoint changed WHERE the run continues, not WHAT it computes");
        check(h3.nodes[3]->invocations() == 1, "F2b: publish ran exactly once, after the real resume");

        h3.engine.stop();
    }

    if (g_failures == 0) {
        std::printf("test_workflow_checkpoint: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_checkpoint: %d failure(s)\n", g_failures);
    return 1;
}
