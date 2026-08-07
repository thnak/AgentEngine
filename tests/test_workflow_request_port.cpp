// Implements 014-Workflow-and-Orchestration.md §4 (Human-in-the-loop: the request port), and the
// activation half of §8 G5. Milestone 6 Phase E
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md, decisions 1 and 11).
//
// §4 makes three claims and this file tests each one, plus the case OQ-4 was resolved on:
//   1. a request port "emits `InputRequired` (001 §2) and suspends the workflow until a response
//      arrives" -- E1;
//   2. "Multiple request ports open concurrently in different branches produce multiple concurrent
//      `Interaction` records on the same run -- the case that makes `interaction_id` a SET rather
//      than a singleton, resolving OQ-4" -- E3. This has never been exercised anywhere in this
//      codebase: every existing use of `Interaction` opens at most one;
//   3. "A suspended workflow HOLDS NO RESOURCES: it is checkpointed, its activations passivate" --
//      E4, by census, which is §8 G5's own bar and the reason decision 11 exists.
//
// E4 is the one that would be worthless as a comment. `ActorRef::passivate()` drains in-flight work
// and is never a mid-handler interrupt (Quark ADR-034), so the tempting implementation of §4 -- a
// `co_await` parked inside the running handler until the human answers -- would keep the activation
// alive and G5 would be false while every other test here still passed. The census is what
// distinguishes the two designs.
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
#include "quark/detail/message_pool.hpp"

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

    // Phase E runs several asks against ONE live engine, so start/stop are the caller's, unlike the
    // single-shot `run()` the Phase B-D harnesses use.
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
    // E0 -- "can this build EXECUTE it", the predicate Phase E had to separate from "is this a
    // well-formed graph". 014 §1 lists four executor kinds; Milestone 6 built `function` and
    // `request_port`. The supervisor asks every non-port node through `FunctionExecutor`, so an
    // `agent` node would be RUN AS A FUNCTION -- plausible output from a graph whose behaviour
    // differs from what its author declared. That refusal cannot live in `validate_workflow`, which
    // also serves §7's rendering and the 015 loader over graphs no build can run.
    // =========================================================================================
    {
        Workflow wf;
        wf.id               = "kinds";
        wf.executors        = {node_desc("a"), node_desc("b")};
        wf.edges            = {Edge{"a", "b", edge_kind::direct, {}, {}}};
        wf.start            = "a";
        wf.bound.max_rounds = 4;

        check(check_workflow_executable(wf).has_value(),
              "E0: a graph of function nodes is executable by this build");

        wf.executors[1].kind = executor_kind::request_port;
        check(check_workflow_executable(wf).has_value(),
              "E0 positive control: a `request_port` node is executable -- Phase E built that kind, "
              "so the refusal below is about what is UNIMPLEMENTED, not about non-function nodes");

        for (auto kind : {executor_kind::agent, executor_kind::sub_workflow}) {
            wf.executors[1].kind = kind;
            auto r = check_workflow_executable(wf);
            check(!r && r.error().code == "workflow.executor_kind_unsupported",
                  "E0: an unimplemented executor kind is REFUSED rather than silently run as a "
                  "plain function node");
            check(validate_workflow(wf).has_value(),
                  "E0: and the same graph is still VALID -- the two questions are separate, so §7 "
                  "can render a graph this build cannot execute");
        }
    }

    // =========================================================================================
    // E1 -- a request port suspends the run, and E4 -- the suspended run holds no activation.
    //
    //   draft -> [approve] -> publish
    //
    // The port sits mid-graph so the suspension is genuinely mid-run: `draft` has already produced
    // an output, and `publish` must not run until a human answers.
    // =========================================================================================
    {
        Harness h(*config, 3);
        h.add_node("draft", appender("draft"));
        h.add_node("approve", appender("approve"));  // registered, and must never be invoked
        h.add_node("publish", appender("publish"));

        Workflow wf;
        wf.id        = "hitl";
        wf.executors = {node_desc("draft"), port_desc("approve"), node_desc("publish")};
        wf.edges     = {Edge{"draft", "approve", edge_kind::direct, {}, {}},
                        Edge{"approve", "publish", edge_kind::direct, {}, {}}};
        wf.start            = "draft";
        wf.output_selection = {"publish"};
        wf.bound.max_rounds = 8;

        auto sup = h.install(wf);
        auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));

        check(r1 && r1->status == workflow_status::suspended,
              "E1: reaching a request port suspends the run -- a status of its own, not `completed` "
              "with a missing output the caller cannot distinguish from success");
        check(r1 && r1->open_interactions.size() == 1,
              "E1: exactly one Interaction is open (001 §2's durable correlation record)");
        check(h.nodes[1]->invocations() == 0,
              "E1: the port node's registered actor was never invoked -- a request port has no body "
              "to run; reaching it IS the event");
        check(h.nodes[2]->invocations() == 0,
              "E1: the downstream node did NOT run -- the workflow is suspended, not merely marked");
        check(r1 && r1->partial.size() == 1 && r1->partial[0].executor_id == "draft",
              "E1: work completed before the suspension is preserved in the suspended state");

        std::string interaction_id;
        if (r1 && !r1->open_interactions.empty()) {
            auto const& i  = r1->open_interactions.front();
            interaction_id = i.interaction_id;
            note("interaction", i.interaction_id);
            check(i.run_id == "hitl:run:1" && i.reason == interaction_reason::input,
                  "E1: the Interaction carries the run's own id and reason=input, minted from the "
                  "run/port/round rather than randomly -- §4 wants a checkpoint indexed by this "
                  "token, and I5 wants nondeterminism recorded rather than invented");
        }

        // ---- E4 / §8 G5 (activation half): the suspended run holds NO activation ---------------
        // Measured by census, exactly as M4 Phase E2 measured a suspended session. This is the
        // assertion decision 11 exists to make true: a run parked mid-handler would still be
        // draining and `went_dormant()` would never become true.
        check(sup.passivate(), "E4: passivate() is accepted on the suspended supervisor");
        check(wait_until([&] { return h.sup_act->went_dormant(); }, std::chrono::seconds(2)),
              "E4 (§8 G5, activation half): the suspended workflow's activation reaches Dormant -- "
              "it holds no activation, proven by census rather than asserted");
        check(!h.sup_act->armed_deactivate_entry(),
              "E4: no idle-timeout wheel entry was armed -- on-demand passivation, the same "
              "distinction Quark's own engine_passivate_test.cpp draws");

        // The pending request survives the Dormant round-trip: it is the run's state, not the
        // activation's.
        check(h.supervisor.open_interactions().size() == 1 &&
                  h.supervisor.open_interactions().front().interaction_id == interaction_id,
              "E4: the open Interaction survives passivation -- §4's 'checkpointed', held by the "
              "run and not by the activation");

        // ---- Resume: the response reactivates the Dormant activation and finishes the run -------
        auto r2 = block_on(sup.ask<WorkflowResult>(
            ResumeWorkflow{interaction_id, text_message("approved"), {}}));
        check(r2 && r2->status == workflow_status::completed,
              "E1: the response resumes the SAME run to completion, reactivating a Dormant "
              "activation on the way");
        check(r2 && all_text_of(r2->output) == "approved>publish",
              "E1: the human's answer became the port executor's output and routed along its "
              "declared edge");
        check(h.nodes[2]->invocations() == 1, "E1: the downstream node ran exactly once, after the "
                                              "answer");
        if (r2) note("resumed output", all_text_of(r2->output));

        h.engine.stop();
    }

    // =========================================================================================
    // E2 -- a resume naming an unknown or already-answered interaction FAILS CLOSED.
    // `AgentSession::resolve_interaction` set this precedent for the same call; answering "still
    // suspended" instead would be indistinguishable from a response that got lost.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("ask", appender("ask"));
        h.add_node("done", appender("done"));

        Workflow wf;
        wf.id        = "closed";
        wf.executors = {port_desc("ask"), node_desc("done")};
        wf.edges     = {Edge{"ask", "done", edge_kind::direct, {}, {}}};
        wf.start            = "ask";
        wf.output_selection = {"done"};
        wf.bound.max_rounds = 8;

        auto sup = h.install(wf);
        auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r1 && r1->status == workflow_status::suspended,
              "E2: a run whose START node is a request port suspends immediately");

        auto bogus = block_on(sup.ask<WorkflowResult>(
            ResumeWorkflow{"hitl:port:nope:0", text_message("x"), {}}));
        check(bogus && bogus->status == workflow_status::invalid,
              "E2: resuming an interaction the run never opened fails closed");
        check(h.nodes[1]->invocations() == 0,
              "E2: and it advanced nothing -- a rejected resume is not a partial resume");

        std::string const id = r1 ? r1->open_interactions.front().interaction_id : std::string{};
        auto r2 = block_on(sup.ask<WorkflowResult>(ResumeWorkflow{id, text_message("yes"), {}}));
        check(r2 && r2->status == workflow_status::completed, "E2: the real id resumes the run");

        auto replay = block_on(sup.ask<WorkflowResult>(ResumeWorkflow{id, text_message("yes"), {}}));
        check(replay && replay->status == workflow_status::invalid,
              "E2: answering the same interaction twice fails closed -- a resolved port is gone, so "
              "a duplicated response cannot re-run the branch behind it");
        check(h.nodes[1]->invocations() == 1,
              "E2: the downstream node ran ONCE across both resume attempts");

        h.engine.stop();
    }

    // =========================================================================================
    // E3 -- OQ-4: SEVERAL ports open at once on one run. Never exercised anywhere before this.
    //
    //          /-> [ok_a] -\
    //   fan --                >-> merge
    //          \-> [ok_b] -/
    //
    // Two ports in different branches of a fan-out. §2's superstep barrier is what makes this
    // reachable at all: it keeps the branches in step, so both reach their ports in the SAME round.
    // =========================================================================================
    {
        Harness h(*config, 4);
        h.add_node("fan", appender("fan"));
        h.add_node("ok_a", appender("ok_a"));
        h.add_node("ok_b", appender("ok_b"));
        h.add_node("merge", appender("merge"));

        Workflow wf;
        wf.id        = "two-ports";
        wf.executors = {node_desc("fan"), port_desc("ok_a"), port_desc("ok_b"), node_desc("merge")};
        wf.edges     = {Edge{"fan", "ok_a", edge_kind::fan_out, {}, {}},
                        Edge{"fan", "ok_b", edge_kind::fan_out, {}, {}},
                        Edge{"ok_a", "merge", edge_kind::fan_in, {}, {}},
                        Edge{"ok_b", "merge", edge_kind::fan_in, {}, {}}};
        wf.start            = "fan";
        wf.output_selection = {"merge"};
        wf.bound.max_rounds = 8;

        auto sup = h.install(wf);
        auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));

        check(r1 && r1->status == workflow_status::suspended && r1->open_interactions.size() == 2,
              "E3 (OQ-4): two request ports in different branches open TWO concurrent Interaction "
              "records on one run -- `interaction_id` is a set, not a singleton");
        std::string id_a, id_b;
        if (r1 && r1->open_interactions.size() == 2) {
            id_a = r1->open_interactions[0].interaction_id;
            id_b = r1->open_interactions[1].interaction_id;
            note("interaction a", id_a);
            note("interaction b", id_b);
            check(id_a != id_b && r1->open_interactions[0].run_id == r1->open_interactions[1].run_id,
                  "E3: distinct interaction ids, one shared run id");
        }

        // 001 §2, which `AgentSession` implements verbatim: a run does not leave Suspended until
        // EVERY open Interaction is resolved. Answering one is not answering the run.
        auto r2 = block_on(sup.ask<WorkflowResult>(ResumeWorkflow{id_a, text_message("A"), {}}));
        check(r2 && r2->status == workflow_status::suspended && r2->open_interactions.size() == 1,
              "E3: answering ONE port leaves the run suspended on the other -- 001 §2's 'does not "
              "leave Suspended until every Interaction is resolved'");
        check(h.nodes[3]->invocations() == 0,
              "E3: the aggregator did not run on a half-answered run");

        auto r3 = block_on(sup.ask<WorkflowResult>(ResumeWorkflow{id_b, text_message("B"), {}}));
        check(r3 && r3->status == workflow_status::completed,
              "E3: answering the last port completes the run");
        check(h.nodes[3]->invocations() == 1,
              "E3: the fan-in aggregator ran exactly ONCE with both answers -- the two responses "
              "merged like any other fan-in, so a request port is an ordinary node at an edge");
        check(r3 && all_text_of(r3->output) == "A+B>merge",
              "E3: and it saw both answers, in graph index order rather than answer order");
        if (r3) note("merged", all_text_of(r3->output));

        h.engine.stop();
    }

    // =========================================================================================
    // E5 -- the human's answer can ROUTE: approve/reject over `switch_case`, which is the canonical
    // human-in-the-loop shape. The I3 boundary is the same one a classifier gets (Phase C): a label
    // selects among edges the GRAPH declares, so a human cannot reach a node the author did not
    // wire either.
    // =========================================================================================
    {
        auto build = [] {
            Workflow wf;
            wf.id        = "approve-or-reject";
            wf.executors = {node_desc("draft"), port_desc("review"), node_desc("publish"),
                            node_desc("discard")};
            wf.edges     = {Edge{"draft", "review", edge_kind::direct, {}, {}},
                            Edge{"review", "publish", edge_kind::switch_case, "approve", {}},
                            Edge{"review", "discard", edge_kind::switch_case, "reject", {}}};
            wf.start            = "draft";
            wf.output_selection = {"publish", "discard"};
            wf.bound.max_rounds = 8;
            return wf;
        };

        for (auto const& answer : {std::string{"approve"}, std::string{"reject"}}) {
            Harness h(*config, 4);
            h.add_node("draft", appender("draft"));
            h.add_node("review", appender("review"));
            h.add_node("publish", appender("publish"));
            h.add_node("discard", appender("discard"));

            Workflow const wf = build();
            auto sup = h.install(wf);
            auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
            std::string const id = r1 ? r1->open_interactions.front().interaction_id : std::string{};
            auto r2 = block_on(sup.ask<WorkflowResult>(
                ResumeWorkflow{id, text_message(answer), {answer}}));

            bool const approved = answer == "approve";
            check(r2 && r2->status == workflow_status::completed,
                  "E5: the routed answer completes the run");
            check(h.nodes[approved ? 2 : 3]->invocations() == 1 &&
                      h.nodes[approved ? 3 : 2]->invocations() == 0,
                  "E5: the answer selected exactly one declared branch, and the other stayed cold");
            if (r2) note(approved ? "approve ->" : "reject  ->", all_text_of(r2->output));
            h.engine.stop();
        }

        // ---- I3 boundary, the negative control ------------------------------------------------
        // A label no edge carries reaches nothing. Same rule as Phase C's classifier check: routing
        // selects among pre-authorized options, so an answer -- however it was obtained, and a human
        // answer is no more trusted here than a model's -- cannot name its own target.
        Harness h(*config, 4);
        h.add_node("draft", appender("draft"));
        h.add_node("review", appender("review"));
        h.add_node("publish", appender("publish"));
        h.add_node("discard", appender("discard"));

        Workflow const wf = build();
        auto sup = h.install(wf);
        auto r1  = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        std::string const id = r1 ? r1->open_interactions.front().interaction_id : std::string{};
        auto r2 = block_on(sup.ask<WorkflowResult>(
            ResumeWorkflow{id, text_message("x"), {"publish", "admin_override"}}));

        check(r2 && r2->status == workflow_status::routing_failed && r2->failed_executor == "review",
              "E5 (I3): an answer naming a NODE id and an invented label selects no declared edge, "
              "and the run reports routing_failed rather than completing with an empty result");
        check(h.nodes[2]->invocations() == 0 && h.nodes[3]->invocations() == 0,
              "E5 (I3): neither branch ran -- a human's label is matched against the graph's own "
              "declared edges, exactly like a classifier's");
        h.engine.stop();
    }

    // =========================================================================================
    // WHAT PHASE E DOES NOT PROVE, named rather than implied.
    //
    // §8 G5 reads: "a workflow suspended at a request port holds no activation, NO SANDBOX, and NO
    // CONNECTION (measured), and resumes correctly AFTER A PROCESS RESTART."
    //   * no activation -- proven above, by census (E4).
    //   * no sandbox / no connection -- nothing in the workflow layer holds either yet. Both arrive
    //     with the agent-kind executor (008's sandbox, 004's provider connection), which is not
    //     built. Asserting them now would be a test that cannot fail.
    //   * resumes after a PROCESS RESTART -- needs 014 §5's checkpoint, which is Phase F. What is
    //     proven here is resume across a Dormant round-trip in one process, which is strictly less.
    // =========================================================================================

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_request_port: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_request_port: %d FAILURE(S)\n", g_failures);
    return 1;
}
