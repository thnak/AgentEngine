// Implements 014-Workflow-and-Orchestration.md §6 (Failure); Milestone 6 Phase D
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// §6 has three bullets and this file has a section per bullet, plus the one thing the RFC does not
// say but the implementation must decide (which failure CLASSES a retry applies to).
//
// The distinction the whole phase turns on: there are TWO failure channels, and a test that only
// exercises one proves half of §6.
//   * a body that RETURNS an error  -> a reported failure. The actor is healthy. The edge's policy
//     decides (§6 bullet 1).
//   * a body that THROWS            -> an actor failure. Quark 007 runs the executor's `OnFailure`
//     and dead-letters the pending ask (§6 bullet 2). The supervisor must SEE a failure, not hang.
// D5 is the second channel, and it is the one that would have gone untested if `ok=false` were
// treated as "the failure case".
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded rounds, no sleeps in this file.

#include <cstdio>
#include <memory>
#include <stdexcept>
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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

// Every content item rendered: `Text` verbatim, `Error` as `!<message>`. A `propagate`/`fallback`
// target receives a failure MARKER rather than a payload, so a recovery node has to be able to read
// one -- which is exactly what this renders.
[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (!out.empty()) out += "+";
        if (auto const* t = std::get_if<Text>(&item.value)) {
            out += t->text;
        } else if (auto const* e = std::get_if<Error>(&item.value)) {
            out += "!" + e->message;
        } else {
            out += "?";
        }
    }
    return out;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        return text_message(all_text_of(in) + ">" + name);
    };
}

// A node that reports failure by RETURNING an error -- channel one. `fail_first_n` lets a node
// recover on a later attempt, which is what makes a retry test able to distinguish "retried" from
// "retried and still gave up".
[[nodiscard]] ExecutorBody failing(std::string name, failure_class klass, std::uint32_t fail_first_n) {
    auto seen = std::make_shared<std::uint32_t>(0);
    return [name = std::move(name), klass, fail_first_n, seen](
               Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        if ((*seen)++ < fail_first_n) {
            return std::unexpected(ae::error{klass, "injected failure in '" + name + "'",
                                            "test.injected"});
        }
        return text_message(all_text_of(in) + ">" + name);
    };
}

// A node that THROWS -- channel two. `throw_first_n` bounds it so the run can go on to prove the
// actor was restarted rather than stopped.
[[nodiscard]] ExecutorBody throwing(std::string name, std::uint32_t throw_first_n) {
    auto seen = std::make_shared<std::uint32_t>(0);
    return [name = std::move(name), throw_first_n, seen](
               Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        if ((*seen)++ < throw_first_n) throw std::runtime_error("injected throw in '" + name + "'");
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

    [[nodiscard]] quark::result<WorkflowResult> run(Workflow const& wf, std::string input) {
        for (std::size_t i = 0; i < nodes.size(); ++i) refs.push_back(router.get<FunctionExecutor>(keys[i]));
        supervisor.initialize(wf, refs);
        sup_act = make_workflow_activation(supervisor, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        auto sup = router.get<WorkflowSupervisor>(kSupervisorKey);
        engine.start();
        auto r = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message(std::move(input))}));
        engine.stop();
        return r;
    }
};

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}

// The partial-result entry for one executor, or nullptr.
[[nodiscard]] ExecutorOutput const* partial_for(WorkflowResult const& r, char const* id) {
    for (auto const& out : r.partial) {
        if (out.executor_id == id) return &out;
    }
    return nullptr;
}

[[nodiscard]] EdgeFailurePolicy retry_policy(std::uint32_t attempts) {
    return EdgeFailurePolicy{edge_failure_policy::retry, attempts, {}};
}
[[nodiscard]] EdgeFailurePolicy fallback_policy(std::string target) {
    return EdgeFailurePolicy{edge_failure_policy::fallback, 0, std::move(target)};
}
[[nodiscard]] EdgeFailurePolicy propagate_policy() {
    return EdgeFailurePolicy{edge_failure_policy::propagate, 0, {}};
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // =========================================================================================
    // D0 -- the validator (014 §6 declared on the edge). Every new rule, both directions.
    // =========================================================================================
    {
        auto base = [] {
            Workflow wf;
            wf.id                = "policies";
            wf.executors         = {node_desc("a"), node_desc("b"), node_desc("r")};
            wf.edges             = {Edge{"a", "b", edge_kind::direct, {}, {}},
                                    Edge{"b", "r", edge_kind::direct, {}, {}}};
            wf.start             = "a";
            wf.bound.max_rounds  = 8;
            return wf;
        };

        check(validate_workflow(base()).has_value(),
              "D0 control: the base graph with default (fail) policies validates");

        {   // retry with no budget
            Workflow wf = base();
            wf.edges[0].on_failure = EdgeFailurePolicy{edge_failure_policy::retry, 0, {}};
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.retry_attempts_required",
                  "D0: retry with a 0-attempt budget is rejected");
        }
        {   // budget without retry
            Workflow wf = base();
            wf.edges[0].on_failure = EdgeFailurePolicy{edge_failure_policy::propagate, 3, {}};
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.unexpected_retry_attempts",
                  "D0: an attempt budget on a non-retry policy is rejected, not ignored");
        }
        {   // fallback with no target
            Workflow wf = base();
            wf.edges[0].on_failure = EdgeFailurePolicy{edge_failure_policy::fallback, 0, {}};
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.fallback_target_required",
                  "D0: fallback naming no executor is rejected");
        }
        {   // target without fallback
            Workflow wf = base();
            wf.edges[0].on_failure = EdgeFailurePolicy{edge_failure_policy::fail, 0, "r"};
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.unexpected_fallback_target",
                  "D0: a fallback target on a non-fallback policy is rejected, not ignored");
        }
        {   // fallback naming a node that does not exist
            Workflow wf = base();
            wf.edges[0].on_failure = fallback_policy("nowhere");
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.unknown_fallback_target",
                  "D0: a fallback branch to an undeclared executor is rejected");
        }
        {   // the fallback branch is TYPE-CHECKED like any other edge out of `a`
            Workflow wf = base();
            wf.executors[2].input_type = "U";  // r now accepts U, but a emits T
            wf.edges[1]                = Edge{"b", "r", edge_kind::direct, {}, {}};
            wf.executors[1].output_type = "U";  // keep b -> r legal so only the fallback is wrong
            wf.edges[0].on_failure     = fallback_policy("r");
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.fallback_type_mismatch",
                  "D0: a fallback branch connecting incompatible types is rejected (014 §1 applies "
                  "to the edge that only runs when something already failed)");
        }
        {   // a node's outgoing edges must agree
            Workflow wf = base();
            wf.executors = {node_desc("a"), node_desc("b"), node_desc("r")};
            wf.edges     = {Edge{"a", "b", edge_kind::fan_out, {}, retry_policy(2)},
                            Edge{"a", "r", edge_kind::fan_out, {}, propagate_policy()}};
            auto r = validate_workflow(wf);
            check(!r && r.error().code == "workflow.conflicting_failure_policy",
                  "D0: an executor whose outgoing edges declare different policies is rejected");
        }
        {   // positive control for the agreement rule -- same kind, different fallback targets is OK
            Workflow wf;
            wf.id        = "two-recoveries";
            wf.executors = {node_desc("a"), node_desc("b"), node_desc("c"), node_desc("r1"),
                            node_desc("r2")};
            wf.edges     = {Edge{"a", "b", edge_kind::fan_out, {}, fallback_policy("r1")},
                            Edge{"a", "c", edge_kind::fan_out, {}, fallback_policy("r2")}};
            wf.start             = "a";
            wf.bound.max_rounds  = 8;
            check(validate_workflow(wf).has_value(),
                  "D0 positive control: same policy kind with different fallback targets validates, "
                  "and both recovery nodes count as reachable");
        }
    }

    // =========================================================================================
    // D1 -- §6 bullet 1, `fail`: partial results are PRESERVED (§6 bullet 3).
    // The falsifiable case the breakdown names: "a failed workflow discarding completed executor
    // outputs".
    // =========================================================================================
    {
        Harness h(*config, 4);
        h.add_node("src", appender("src"));
        h.add_node("w1", appender("w1"));
        h.add_node("w2", failing("w2", failure_class::fatal, 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "fail-preserves";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("sink")};
        wf.edges     = {Edge{"src", "w1", edge_kind::fan_out, {}, {}},
                        Edge{"src", "w2", edge_kind::fan_out, {}, {}},
                        Edge{"w1", "sink", edge_kind::direct, {}, {}},
                        Edge{"w2", "sink", edge_kind::direct, {}, {}}};
        wf.start             = "src";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed,
              "D1: a `fail` policy (the default) stops the workflow on executor failure");
        check(r && r->failed_executor == "w2",
              "D1: the result NAMES the executor that ended the run (I4)");

        // The round that failed is round 1 (`src` ran alone in round 0). `w1` succeeded in that same
        // round, and `src` in the one before -- both must survive.
        auto const* p_src = r ? partial_for(*r, "src") : nullptr;
        auto const* p_w1  = r ? partial_for(*r, "w1") : nullptr;
        auto const* p_w2  = r ? partial_for(*r, "w2") : nullptr;
        check(p_src != nullptr && all_text_of(p_src->payload) == "in>src",
              "D1: an output completed in an EARLIER round survives the failure");
        check(p_w1 != nullptr && all_text_of(p_w1->payload) == "in>src>w1",
              "D1: an output completed in the SAME round as the failure survives it -- 014 §6's "
              "'not discarded'");
        check(p_w2 == nullptr, "D1: the failed executor contributes no partial result");
        check(p_src != nullptr && p_src->round == 0 && p_w1 != nullptr && p_w1->round == 1,
              "D1: each partial result records the round it was produced in");
        if (r) note("partial results", std::to_string(r->partial.size()) + " entries");
    }

    // =========================================================================================
    // D2 -- §6 bullet 1, `retry`: bounded re-invocation, in-round.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("flaky", failing("flaky", failure_class::transient, 2));  // fails twice, then works
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "retry-recovers";
        wf.executors = {node_desc("flaky"), node_desc("sink")};
        wf.edges     = {Edge{"flaky", "sink", edge_kind::direct, {}, retry_policy(3)}};
        wf.start             = "flaky";
        wf.output_selection  = {"sink"};
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed && all_text_of(r->output) == "in>flaky>sink",
              "D2: a transient failure inside the retry budget recovers and the workflow completes");
        check(h.nodes[0]->invocations() == 3,
              "D2: the node was invoked exactly 3 times (1 + 2 retries), not once and not 4 times");
        check(r && r->rounds == 2,
              "D2: retries happen INSIDE the round -- 2 rounds ran, not one round per attempt");
    }

    // =========================================================================================
    // D2b -- retry EXHAUSTED resolves to `fail` (§6 lists four alternatives, not a composition).
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("flaky", failing("flaky", failure_class::transient, 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "retry-exhausted";
        wf.executors = {node_desc("flaky"), node_desc("sink")};
        wf.edges     = {Edge{"flaky", "sink", edge_kind::direct, {}, retry_policy(2)}};
        wf.start             = "flaky";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed && r->failed_executor == "flaky",
              "D2b: an exhausted retry budget resolves to `fail`, named");
        check(h.nodes[0]->invocations() == 3,
              "D2b: the budget is a BOUND -- exactly 1 + 2 invocations, then it stops");
        check(h.nodes[1]->invocations() == 0,
              "D2b: the failed node's normal edge never fired, so the sink never ran");
    }

    // =========================================================================================
    // D2c -- CLASSIFICATION does work: `contract` and `policy` failures are not retried AT ALL.
    // The positive control is D2 above, where the same policy retried a `transient` failure.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("bad", failing("bad", failure_class::contract, 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "no-retry-on-contract";
        wf.executors = {node_desc("bad"), node_desc("sink")};
        wf.edges     = {Edge{"bad", "sink", edge_kind::direct, {}, retry_policy(3)}};
        wf.start             = "bad";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed,
              "D2c: a contract failure under a retry policy still fails the workflow");
        check(h.nodes[0]->invocations() == 1,
              "D2c: a `contract` failure is deterministic, so it is invoked ONCE despite a budget of "
              "3 -- 014 §6's 'is classified' doing work rather than decorating");
    }
    {
        Harness h(*config, 2);
        h.add_node("denied", failing("denied", failure_class::policy, 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "no-retry-on-policy";
        wf.executors = {node_desc("denied"), node_desc("sink")};
        wf.edges     = {Edge{"denied", "sink", edge_kind::direct, {}, retry_policy(5)}};
        wf.start             = "denied";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed && h.nodes[0]->invocations() == 1,
              "D2c: a `policy` failure is a DENIAL and is never retried -- re-asking a denied "
              "request until the answer changes is an I2 concern, not a retry");
    }

    // =========================================================================================
    // D3 -- §6 bullet 1, `propagate`: the target decides, and the workflow keeps running.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("risky", failing("risky", failure_class::fatal, 99));
        h.add_node("handler", appender("handler"));

        Workflow wf;
        wf.id        = "propagate";
        wf.executors = {node_desc("risky"), node_desc("handler")};
        wf.edges     = {Edge{"risky", "handler", edge_kind::direct, {}, propagate_policy()}};
        wf.start             = "risky";
        wf.output_selection  = {"handler"};
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed,
              "D3: under `propagate` an executor failure does NOT take the workflow down");
        check(h.nodes[1]->invocations() == 1, "D3: the target ran exactly once");
        check(r && all_text_of(r->output).find("!executor 'risky' failed (fatal)") != std::string::npos,
              "D3: the target received an attributable failure marker naming the executor and its "
              "class (001 §6), not the failed node's absent output");
        if (r) note("propagated", all_text_of(r->output));
    }

    // =========================================================================================
    // D4 -- §6 bullet 1, `fallback`: a named recovery branch, and the NORMAL target stays cold.
    // =========================================================================================
    {
        Harness h(*config, 3);
        h.add_node("risky", failing("risky", failure_class::resource, 99));
        h.add_node("normal", appender("normal"));
        h.add_node("recovery", appender("recovery"));

        Workflow wf;
        wf.id        = "fallback";
        wf.executors = {node_desc("risky"), node_desc("normal"), node_desc("recovery")};
        wf.edges     = {Edge{"risky", "normal", edge_kind::direct, {}, fallback_policy("recovery")}};
        wf.start             = "risky";
        wf.output_selection  = {"normal", "recovery"};
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed,
              "D4: under `fallback` the workflow completes through the recovery branch");
        check(h.nodes[2]->invocations() == 1, "D4: the recovery executor ran");
        check(h.nodes[1]->invocations() == 0,
              "D4: the NORMAL target never ran -- a failed executor produced no output to carry "
              "along its ordinary edges");
        if (r) note("recovered", all_text_of(r->output));

        // Positive control on the same graph: no failure, no recovery.
        Harness h2(*config, 3);
        h2.add_node("risky", appender("risky"));
        h2.add_node("normal", appender("normal"));
        h2.add_node("recovery", appender("recovery"));
        auto r2 = h2.run(wf, "in");
        check(r2 && r2->status == workflow_status::completed &&
                  h2.nodes[1]->invocations() == 1 && h2.nodes[2]->invocations() == 0,
              "D4 positive control: with no failure the SAME graph runs the normal target and leaves "
              "the recovery branch cold");
    }

    // =========================================================================================
    // D5 -- §6 bullet 2: SUPERVISION. A throwing executor is restarted by Quark 007 without taking
    // the workflow down. This is the second failure channel, and the property that makes it usable
    // at all is that the pending ask is dead-lettered rather than left to hang.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("faulty", throwing("faulty", 1));  // throws on its first invocation only
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "supervised-throw";
        wf.executors = {node_desc("faulty"), node_desc("sink")};
        wf.edges     = {Edge{"faulty", "sink", edge_kind::direct, {}, retry_policy(2)}};
        wf.start             = "faulty";
        wf.output_selection  = {"sink"};
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        // If Quark did not dead-letter the faulting ask, this run would never return and the test
        // would time out rather than fail -- which is why the assertion below is on a COMPLETED run
        // and not merely on a non-crash.
        check(r && r->status == workflow_status::completed && all_text_of(r->output) == "in>faulty>sink",
              "D5: a THROWING executor is restarted by Quark 007 and the retried invocation "
              "succeeds -- 014 §6's 'without taking the workflow down'");
        check(h.nodes[1]->invocations() == 1,
              "D5: the workflow carried on past the fault and reached the downstream node");
    }
    {
        // Same fault, no retry policy: the workflow must still TERMINATE, reporting the failure.
        // A supervised throw that hung the supervisor would be the worst outcome of the two, and it
        // is the one a `completed`-only test would miss.
        Harness h(*config, 2);
        h.add_node("faulty", throwing("faulty", 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "unsupervised-throw";
        wf.executors = {node_desc("faulty"), node_desc("sink")};
        wf.edges     = {Edge{"faulty", "sink", edge_kind::direct, {}, {}}};
        wf.start             = "faulty";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed && r->failed_executor == "faulty",
              "D5: an always-throwing executor under the default `fail` policy ends the run with a "
              "reported failure -- the supervisor's ask completes, it does not stall");
    }

    // =========================================================================================
    // D6 -- the two channels reach the SAME policy. A throwing node under `fallback` recovers
    // exactly as a returning-error node does; §6 declares one policy per edge, not one per channel.
    // =========================================================================================
    {
        Harness h(*config, 3);
        h.add_node("faulty", throwing("faulty", 99));
        h.add_node("normal", appender("normal"));
        h.add_node("recovery", appender("recovery"));

        Workflow wf;
        wf.id        = "throw-into-fallback";
        wf.executors = {node_desc("faulty"), node_desc("normal"), node_desc("recovery")};
        wf.edges     = {Edge{"faulty", "normal", edge_kind::direct, {}, fallback_policy("recovery")}};
        wf.start             = "faulty";
        wf.output_selection  = {"recovery"};
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed && h.nodes[2]->invocations() == 1,
              "D6: an ACTOR failure routes through the edge's declared policy just as a reported "
              "failure does -- one policy per edge, not one per channel");
        check(r && all_text_of(r->output).find("!executor 'faulty' failed (transient)") != std::string::npos,
              "D6: the actor-failure marker is classified `transient` (the actor was restarted, so a "
              "further attempt meets a fresh instance)");
    }

    // =========================================================================================
    // D7 -- §6 bullet 2's "subject to a BOUNDED escalation", and the half of that sentence nothing
    // else here reaches: "restarted OR STOPPED".
    //
    // `FunctionExecutor` declares `OnFailure<Restart, MaxRestarts<3, Within<1000>>>`. A declared
    // bound nobody drives past is a claim without a test, so this node throws on every invocation
    // under a retry budget of 5 -- asking for 6 invocations, which is more than the actor's restart
    // budget allows. Quark charges 3 restarts, then escalates, and `do_escalate` ends in `do_stop`:
    // the actor is deactivated and every later message dead-letters WITHOUT entering the handler.
    //
    // Two bounds therefore compose here -- the workflow's retry budget (014 §6) and the actor's
    // restart budget (Quark 007) -- and the TIGHTER one wins. That is the property to pin down: an
    // edge cannot buy itself more executor attempts than the executor's own supervision policy
    // allows, and the run still terminates rather than stalling on a stopped actor.
    // =========================================================================================
    {
        Harness h(*config, 2);
        h.add_node("doomed", throwing("doomed", 99));
        h.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "restart-budget-exhausted";
        wf.executors = {node_desc("doomed"), node_desc("sink")};
        wf.edges     = {Edge{"doomed", "sink", edge_kind::direct, {}, retry_policy(5)}};
        wf.start             = "doomed";
        wf.bound.max_rounds  = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::executor_failed && r->failed_executor == "doomed",
              "D7: exhausting the actor's restart budget still ENDS the run -- a stopped executor's "
              "asks dead-letter, they do not hang the supervisor");
        std::uint32_t const entered = h.nodes[0]->invocations();
        note("handler entries under a 6-attempt retry budget", std::to_string(entered));
        check(entered == 4,
              "D7: the handler was entered 4 times, not the 6 the edge asked for -- 3 charged "
              "restarts and then Stop; the actor's own bound clamps the edge's retry budget");
        check(h.nodes[1]->invocations() == 0, "D7: the downstream node never ran");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_failure: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_failure: %d FAILURE(S)\n", g_failures);
    return 1;
}
