// Implements 014-Workflow-and-Orchestration.md §4 (human-in-the-loop / the request port), and §6
// (partial results) and §2 (deadline accounting) where the request port interacts with them.
//
// INDEPENDENT SECOND SUITE, deliberately. `test_workflow_request_port.cpp` is the implementer's own
// E0-E5 coverage, derived by reading `supervisor.hpp` while writing it. This file's assertions are
// derived instead from the RFC 014 §4 prose and the public struct/enum declarations in
// `supervisor.hpp` (`workflow_status`, `RunWorkflow`, `ResumeWorkflow`, `ExecutorOutput`,
// `WorkflowResult`, `WorkflowSupervisor`'s public surface) -- NOT from reading `execute()`'s body
// first. The goal is to catch a bug the implementer's own tests would not, because both would have
// been derived from the same code under test.
//
// Six scenarios, each naming the spec sentence it checks:
//   IQ1 -- "§2's deadline now accumulates over RUNNING time only" (commit message): a suspended run's
//          deadline clock must not tick while it waits on a human.
//   IQ2 -- a request port inside a CYCLE, opening twice: different `interaction_id`s (the round is
//          part of the derived id), and a stale/resolved id must not resolve the CURRENT opening.
//          Folds in interaction FIELD correctness (reason/run_id/timestamps) directly at the site.
//   IQ3 -- §6's "partial results are preserved" against a port's `routing_failed` outcome -- the RFC
//          does not scope that promise to failures before a suspension.
//   IQ4 -- a request port and a FAILING sibling reached in the SAME fan-out round: the round's
//          failure wins (Phase D's "a round's failure ends the workflow"), and the port's id is
//          named in `WorkflowResult::unopened_ports` rather than vanishing with no trace.
//   IQ5 -- `ResumeWorkflow::routes` on a port whose outgoing edges are plain `direct` (not
//          `switch_case`): must be ignored, never given power a `switch_case` label would have (I3).
//   IQ6 -- `WorkflowSupervisor::open_interactions()` (the const accessor) vs
//          `WorkflowResult::open_interactions` (the ask reply field) agreeing at every observation
//          point in the OQ-4 two-port case, including a partial resume and a rejected double-resolve.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded rounds/deadlines. IQ1 contains the one
// deliberate `sleep_for` in this file -- explained at its call site, it is not a substitute for
// polling an async condition, it is the only way to let real wall-clock time pass while a run holds
// no resources and there is nothing to poll.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/core/interaction.hpp"
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

[[nodiscard]] ExecutorBody failing(std::string name, failure_class klass) {
    return [name = std::move(name), klass](Message const&, EffectContext&) -> ae::result<ExecutorOutcome> {
        return std::unexpected(ae::error{klass, "injected failure in '" + name + "'", "test.injected"});
    };
}

[[nodiscard]] Executor func_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{id, executor_kind::request_port, "T", "T"};
}
[[nodiscard]] EdgeFailurePolicy fail_policy() { return EdgeFailurePolicy{}; }

[[nodiscard]] ExecutorOutput const* partial_for(WorkflowResult const& r, char const* id) {
    for (auto const& out : r.partial) {
        if (out.executor_id == id) return &out;
    }
    return nullptr;
}
constexpr std::uint64_t kSupervisorKey = 100;

// A thin rig, deliberately kept alive ACROSS multiple asks (unlike test_workflow_failure.cpp's
// Harness, which starts/stops the Engine inside a single `run()` call) -- a suspend/resume scenario
// needs the same live Engine and the same actor instances for the RunWorkflow ask, the sleep, and
// every subsequent ResumeWorkflow ask.
struct Rig {
    Engine<>            engine;
    detail::MessagePool pool{64};
    LocalRouter         router;

    std::vector<std::unique_ptr<FunctionExecutor>> nodes;
    std::vector<std::unique_ptr<Activation>>       node_acts;
    WorkflowSupervisor                             supervisor;  // locally owned -- direct accessor
                                                                 // calls between asks are the same
                                                                 // idiom test_agent_session_suspend_
                                                                 // resume.cpp uses on `actor.`
    std::unique_ptr<Activation>                    sup_act;
    std::vector<ActorRef<FunctionExecutor>>        refs;
    std::vector<std::uint64_t>                     keys;
    ActorRef<WorkflowSupervisor>                   sup;
    bool                                            started = false;

    explicit Rig(EngineConfig const& cfg) : engine(cfg), router(engine.post_courier(), pool) {}

    void add_node(std::string name, ExecutorBody body) {
        if (keys.empty()) {
            // Sized lazily on first add_node, since the test decides node_count by how many it adds.
        }
        auto node = std::make_unique<FunctionExecutor>();
        node->initialize(std::move(name), std::move(body), EffectContext{});
        nodes.push_back(std::move(node));
    }

    void init(Workflow const& wf) {
        keys = spread_executor_keys(nodes.size(), engine.shard_count(), [this](std::uint64_t k) {
            return engine.shard_of(actor_id_of<FunctionExecutor>(k));
        });
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto act = make_workflow_activation(*nodes[i], pool.sink());
            engine.register_activation(actor_id_of<FunctionExecutor>(keys[i]), *act);
            node_acts.push_back(std::move(act));
            refs.push_back(router.get<FunctionExecutor>(keys[i]));
        }
        supervisor.initialize(wf, refs);
        sup_act = make_workflow_activation(supervisor, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        sup = router.get<WorkflowSupervisor>(kSupervisorKey);
        engine.start();
        started = true;
    }

    [[nodiscard]] quark::result<WorkflowResult> run(Message input) {
        return block_on(sup.ask<WorkflowResult>(RunWorkflow{std::move(input)}));
    }
    [[nodiscard]] quark::result<WorkflowResult> resume(std::string interaction_id, Message response,
                                                       std::vector<std::string> routes = {}) {
        return block_on(sup.ask<WorkflowResult>(
            ResumeWorkflow{std::move(interaction_id), std::move(response), std::move(routes)}));
    }

    ~Rig() {
        if (started) engine.stop();
    }
};

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // =========================================================================================
    // IQ1 -- a suspended run's deadline clock does not tick while it waits on a human.
    // Spec: 014 §2's deadline bounds the workflow; the commit under review claims suspended time is
    // excluded. Construct a workflow with a deadline far shorter than the real time we let elapse
    // while it sits suspended, then resume and require it still COMPLETES.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("start", appender("start"));
        rig.add_node("port", appender("port-should-never-run"));  // request_port node: must see 0
                                                                   // invocations if the engine ever
                                                                   // asked it, which it must not (§4:
                                                                   // "reaching it IS the event")
        rig.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id                = "iq1-deadline-suspend";
        wf.executors         = {func_desc("start"), port_desc("port"), func_desc("sink")};
        wf.edges             = {Edge{"start", "port", edge_kind::direct, {}, {}},
                                Edge{"port", "sink", edge_kind::direct, {}, {}}};
        wf.start             = "start";
        wf.output_selection  = {"sink"};
        wf.bound.deadline_ms = 80;  // short: running work here is microseconds, so this is generous
                                    // for the two real supersteps but would be blown by any accounting
                                    // that also charged the sleep below

        rig.init(wf);
        auto r1 = rig.run(text_message("in"));
        check(r1.has_value() && r1->status == workflow_status::suspended,
              "IQ1: the run reaches the port and suspends before the deadline (sanity)");
        check(rig.nodes[1]->invocations() == 0,
              "IQ1: the port node's registered actor was never asked -- reaching a port is not an "
              "invocation (§4)");
        check(r1.has_value() && r1->open_interactions.size() == 1,
              "IQ1: exactly one Interaction is open");
        std::string const iid = r1 ? r1->open_interactions.front().interaction_id : std::string{};

        // The one deliberate sleep in this file. There is no async condition to poll here -- the run
        // holds no resources while suspended (014 §4), so nothing observable changes state during
        // this wait; a bounded `wait_until` would just spin doing nothing until its own timeout. We
        // sleep past the 80ms deadline by a wide margin (300ms) so the assertion below is unambiguous
        // either way.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        auto r2 = rig.resume(iid, text_message("resp"));
        check(r2.has_value() && r2->status == workflow_status::completed,
              "IQ1: resuming after real time well past the deadline still COMPLETES -- the deadline "
              "did not accumulate suspended wall-clock time");
        check(r2.has_value() && all_text_of(r2->output) == "resp>sink",
              "IQ1: the human's response routed through to the downstream node normally");
        if (r2) note("IQ1 status", r2->status == workflow_status::completed ? "completed" : "OTHER");
    }

    // =========================================================================================
    // IQ2 -- a request port inside a CYCLE opens twice: the round-qualified id differs each time, and
    // the FIRST (now-stale) id must not resolve the second opening. Interaction field correctness
    // (reason, run_id, zeroed timestamps) is checked at the same two sites since both are already
    // captured here.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("port", appender("port-should-never-run"));
        rig.add_node("after", appender("after"));

        Workflow wf;
        wf.id                = "iq2-cycle";
        wf.executors         = {port_desc("port"), func_desc("after")};
        wf.edges             = {Edge{"port", "after", edge_kind::direct, {}, {}},
                                Edge{"after", "port", edge_kind::direct, {}, {}}};
        wf.start             = "port";
        wf.bound.max_rounds  = 6;

        rig.init(wf);
        auto r1 = rig.run(text_message("seed"));
        check(r1.has_value() && r1->status == workflow_status::suspended,
              "IQ2: the run opens the port on its first reach");
        check(r1.has_value() && r1->open_interactions.size() == 1,
              "IQ2: exactly one Interaction open at the first opening");
        Interaction const first = r1 ? r1->open_interactions.front() : Interaction{};

        check(first.run_id == rig.supervisor.run_id(),
              "IQ2: the Interaction's run_id matches the supervisor's own run_id()");
        check(first.reason == interaction_reason::input,
              "IQ2: a request port's Interaction reason is `input` (014 §4 emits InputRequired, not "
              "AuthRequired)");
        check(first.opened_at_ns == 0 && first.expires_at_ns == 0,
              "IQ2: no wall-clock capability is wired anywhere in this project yet, so timestamps stay "
              "at their documented zero rather than a meaningless steady_clock reading");
        check(first.interaction_id == rig.supervisor.run_id() + ":port:port:0",
              "IQ2: the derived id matches the documented <run>:port:<executor>:<round> shape for the "
              "FIRST opening (round 0)");

        auto r2 = rig.resume(first.interaction_id, text_message("answer-1"));
        check(r2.has_value() && r2->status == workflow_status::suspended,
              "IQ2: after the cycle runs 'after' and routes back to 'port', the SAME port opens a "
              "SECOND time and the run suspends again");
        check(rig.nodes[1]->invocations() == 1,
              "IQ2: 'after' ran exactly once between the two openings");
        check(r2.has_value() && r2->open_interactions.size() == 1,
              "IQ2: exactly one Interaction open at the second opening (not two -- the first is fully "
              "resolved and gone, not merely superseded)");
        Interaction const second = r2 ? r2->open_interactions.front() : Interaction{};

        check(second.interaction_id != first.interaction_id,
              "IQ2 -- CORE CLAIM: the two openings of the same port produce DIFFERENT interaction ids");
        check(second.interaction_id == rig.supervisor.run_id() + ":port:port:2",
              "IQ2: the second opening's id embeds the round it actually happened in (round 2, not "
              "round 0 again) -- this is precisely what makes a cycling port's two requests "
              "distinguishable");

        // Resuming the FIRST (stale) id again must NOT resolve the currently-open (second) port.
        auto r3 = rig.resume(first.interaction_id, text_message("replay-of-old-answer"));
        check(r3.has_value() && r3->status == workflow_status::invalid,
              "IQ2 -- CORE CLAIM: resuming the stale first interaction_id again fails closed (invalid) "
              "rather than silently resolving the second, still-open port");
        check(rig.supervisor.open_interactions().size() == 1 &&
                  rig.supervisor.open_interactions().front().interaction_id == second.interaction_id,
              "IQ2: the second port is still open and untouched after the stale resume attempt");

        // Sanity: the CURRENT id does resolve.
        auto r4 = rig.resume(second.interaction_id, text_message("answer-2"));
        check(r4.has_value() && r4->status != workflow_status::invalid,
              "IQ2: resuming the CURRENT (second) interaction_id is accepted");
        if (r4) note("IQ2 final status after resuming the current id",
                     r4->status == workflow_status::suspended ? "suspended (third opening)"
                     : r4->status == workflow_status::bound_max_rounds ? "bound_max_rounds"
                     : r4->status == workflow_status::completed        ? "completed"
                                                                        : "other");
    }

    // =========================================================================================
    // IQ3 -- §6's "partial results are preserved" against a `routing_failed` outcome reached by a
    // resumed port's answer. The RFC's partial-results sentence in §6 is not scoped to "only when the
    // failure happens before a suspension" -- so it should still hold when a HUMAN's routing answer
    // is what fails.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("start", appender("start"));
        rig.add_node("port", appender("port-should-never-run"));
        rig.add_node("approve", appender("approve"));
        rig.add_node("reject", appender("reject"));

        Workflow wf;
        wf.id        = "iq3-routing-failed-partial";
        wf.executors = {func_desc("start"), port_desc("port"), func_desc("approve"), func_desc("reject")};
        wf.edges     = {Edge{"start", "port", edge_kind::direct, {}, {}},
                        Edge{"port", "approve", edge_kind::switch_case, "approve", {}},
                        Edge{"port", "reject", edge_kind::switch_case, "reject", {}}};
        wf.start             = "start";
        wf.output_selection  = {"start"};  // so §6's "last output-selected payload" has something to
                                            // check survives the routing failure untouched
        wf.bound.max_rounds  = 6;

        rig.init(wf);
        auto r1 = rig.run(text_message("in"));
        check(r1.has_value() && r1->status == workflow_status::suspended,
              "IQ3: the run reaches the switch_case port and suspends");
        std::string const iid = r1 ? r1->open_interactions.front().interaction_id : std::string{};

        // A human's answer that selects neither declared case label -- the I3-shaped failure mode:
        // the run cannot invent a target, so this must be `routing_failed`, not silently ignored.
        auto r2 = rig.resume(iid, text_message("human says something unrouted"), {"neither_label"});
        check(r2.has_value() && r2->status == workflow_status::routing_failed,
              "IQ3: a routing answer matching no declared case label ends the run `routing_failed`");
        check(r2.has_value() && r2->failed_executor == "port",
              "IQ3: the result names the PORT as the one whose routing contract failed (I4)");
        check(rig.nodes[2]->invocations() == 0 && rig.nodes[3]->invocations() == 0,
              "IQ3: neither switch_case target ran -- an unmatched label reaches nothing");

        auto const* p_start = r2 ? partial_for(*r2, "start") : nullptr;
        check(p_start != nullptr && all_text_of(p_start->payload) == "in>start",
              "IQ3 -- CORE CLAIM: the executor that completed BEFORE the port (start) still has its "
              "partial result after a routing failure discovered only at resume time");
        check(r2.has_value() && all_text_of(r2->output) == "in>start",
              "IQ3: the last output-selected payload (start's, since the port never selects output) "
              "survives the routing failure unchanged -- §6's promise holds across a suspend/resume "
              "boundary, not just within one round");
        if (r2) note("IQ3 partial count", std::to_string(r2->partial.size()));
    }

    // =========================================================================================
    // IQ4 -- a request port and a FAILING sibling reached in the SAME fan-out round. Neither §4 nor
    // §6 states a priority between "a port opens" and "a sibling fails" when both happen in one
    // superstep; the project owner's resolution is that the round's failure wins, and the port's id
    // is recorded rather than silently dropped -- see `unopened_ports`' own comment in supervisor.hpp.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("start", appender("start"));
        rig.add_node("port", appender("port-should-never-run"));
        rig.add_node("failer", failing("failer", failure_class::fatal));
        rig.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id        = "iq4-port-vs-sibling-failure";
        wf.executors = {func_desc("start"), port_desc("port"), func_desc("failer"), func_desc("sink")};
        wf.edges     = {Edge{"start", "port", edge_kind::fan_out, {}, fail_policy()},
                        Edge{"start", "failer", edge_kind::fan_out, {}, fail_policy()},
                        Edge{"port", "sink", edge_kind::direct, {}, {}}};
        wf.start             = "start";
        wf.output_selection  = {"start"};
        wf.bound.max_rounds  = 6;

        rig.init(wf);
        auto r = rig.run(text_message("in"));
        check(r.has_value(), "IQ4: the ask itself completes (no hang) when a port and a failing "
                             "sibling are reached in the same round");

        // Report the observed status precisely -- this is the finding, whichever way it comes out.
        char const* status_name = "?";
        if (r) {
            switch (r->status) {
                case workflow_status::suspended:       status_name = "suspended"; break;
                case workflow_status::executor_failed:  status_name = "executor_failed"; break;
                case workflow_status::completed:        status_name = "completed"; break;
                case workflow_status::routing_failed:   status_name = "routing_failed"; break;
                case workflow_status::bound_max_rounds:status_name = "bound_max_rounds"; break;
                case workflow_status::bound_deadline:   status_name = "bound_deadline"; break;
                case workflow_status::invalid:          status_name = "invalid"; break;
            }
        }
        note("IQ4 observed status", status_name);

        check(r.has_value() && r->status == workflow_status::executor_failed &&
                  r->failed_executor == "failer",
              "IQ4: the sibling's `fail` policy ends the run `executor_failed` -- consistent with §6");
        check(r.has_value() && r->open_interactions.empty(),
              "IQ4: WorkflowResult reports no open interactions for this outcome");
        check(rig.supervisor.open_interactions().empty(),
              "IQ4: the live accessor agrees -- no Interaction was left dangling on the actor either");
        auto const* p_start = r ? partial_for(*r, "start") : nullptr;
        check(p_start != nullptr && all_text_of(p_start->payload) == "in>start",
              "IQ4: the round BEFORE the failure (start) still preserves its partial result");
        check(rig.nodes[1]->invocations() == 0,
              "IQ4: the port node itself was still never asked (unaffected by the sibling's failure)");

        // RESOLVED (post-QA fix): the round's failure wins -- no Interaction is minted for a port
        // reached the same round a sibling's `fail`-policy failure ends the run, consistent with
        // Phase D's "a round's failure ends the workflow". What was a silent vanish is now named:
        // `WorkflowResult::unopened_ports` records the port's id explicitly rather than the run
        // simply forgetting it was ever reached.
        check(r.has_value() && r->unopened_ports.size() == 1 && r->unopened_ports[0] == "port",
              "IQ4: the port reached this same round is named in `unopened_ports`, not silently "
              "dropped -- an operator reading `executor_failed` can now see a human question also "
              "went unanswered");
    }

    // =========================================================================================
    // IQ5 -- `ResumeWorkflow::routes` on a port whose outgoing edges are plain `direct` (not
    // `switch_case`). I3: a routing label can only ever select among edges the graph declares as
    // LABELLED; it must never be read as an instruction on an edge kind that carries no label at all.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("port", appender("port-should-never-run"));
        rig.add_node("sink", appender("sink"));

        Workflow wf;
        wf.id                = "iq5-routes-on-direct-edge";
        wf.executors         = {port_desc("port"), func_desc("sink")};
        wf.edges             = {Edge{"port", "sink", edge_kind::direct, {}, {}}};
        wf.start             = "port";
        wf.output_selection  = {"sink"};
        wf.bound.max_rounds  = 6;

        rig.init(wf);
        auto r1 = rig.run(text_message("in"));
        check(r1.has_value() && r1->status == workflow_status::suspended, "IQ5: the run suspends at start");
        std::string const iid = r1 ? r1->open_interactions.front().interaction_id : std::string{};

        // A spurious, unwired-for routes vector -- values that name nothing the graph declares as a
        // case label (there ARE no case labels on this graph at all).
        auto r2 = rig.resume(iid, text_message("payload"),
                             {"admin_override", "sink", "port", "not_a_real_label"});
        check(r2.has_value() && r2->status == workflow_status::completed,
              "IQ5 -- CORE CLAIM: a spurious `routes` vector on a port with only `direct` outgoing "
              "edges is harmless -- the edge fires exactly as it always would, `routes` carries no "
              "authority here");
        check(r2.has_value() && all_text_of(r2->output) == "payload>sink",
              "IQ5: the response reached 'sink' via the one declared direct edge, unaffected by the "
              "routes content (including a label that happens to spell another node's own id)");
    }

    // =========================================================================================
    // IQ6 -- OQ-4's two-port case: `WorkflowSupervisor::open_interactions()` (the live accessor) vs
    // `WorkflowResult::open_interactions` (the ask reply) must AGREE at every observation point,
    // including mid-way through resolving the two ports one at a time, and a rejected double-resolve
    // of the same port must not disturb either view.
    // =========================================================================================
    {
        Rig rig(*config);
        rig.add_node("start", appender("start"));
        rig.add_node("portA", appender("portA-should-never-run"));
        rig.add_node("portB", appender("portB-should-never-run"));

        Workflow wf;
        wf.id        = "iq6-two-ports-parity";
        wf.executors = {func_desc("start"), port_desc("portA"), port_desc("portB")};
        wf.edges     = {Edge{"start", "portA", edge_kind::fan_out, {}, {}},
                        Edge{"start", "portB", edge_kind::fan_out, {}, {}}};
        wf.start             = "start";
        wf.bound.max_rounds  = 6;

        rig.init(wf);
        auto r1 = rig.run(text_message("in"));
        check(r1.has_value() && r1->status == workflow_status::suspended,
              "IQ6: both ports are reached in the same round and the run suspends once");
        check(r1.has_value() && r1->open_interactions.size() == 2,
              "IQ6 -- OQ-4's own case: two Interactions open concurrently on one run");
        check(rig.supervisor.open_interactions().size() == 2,
              "IQ6: the live accessor sees the same two, immediately after RunWorkflow's reply");

        std::string idA, idB;
        for (auto const& i : r1->open_interactions) {
            if (i.interaction_id.find(":portA:") != std::string::npos) idA = i.interaction_id;
            if (i.interaction_id.find(":portB:") != std::string::npos) idB = i.interaction_id;
        }
        check(!idA.empty() && !idB.empty() && idA != idB,
              "IQ6: both ids are present and distinct (differ by executor id, same round)");

        // Resolve A only. B stays open.
        auto r2 = rig.resume(idA, text_message("A-answer"));
        check(r2.has_value() && r2->status == workflow_status::suspended,
              "IQ6: with B still open, the run stays suspended (001 §2's own rule, reused verbatim by "
              "AgentSession, now exercised here for a workflow for the first time)");
        check(r2.has_value() && r2->open_interactions.size() == 1 &&
                  r2->open_interactions.front().interaction_id == idB,
              "IQ6: WorkflowResult after the partial resume reports exactly B, not A and not both");
        check(rig.supervisor.open_interactions().size() == 1 &&
                  rig.supervisor.open_interactions().front().interaction_id == idB,
              "IQ6 -- CORE CLAIM: the live accessor agrees with the ask reply after a PARTIAL resume "
              "-- both say 'only B is open', neither still lists A nor reports zero");

        // Resolving A again (double-resolve) must be rejected, not silently accepted a second time --
        // an unbounded double-answer would let a stale response overwrite what already resolved.
        auto r3 = rig.resume(idA, text_message("A-answer-again"));
        check(r3.has_value() && r3->status == workflow_status::invalid,
              "IQ6: resuming an ALREADY-resolved interaction_id a second time fails closed");
        check(rig.supervisor.open_interactions().size() == 1 &&
                  rig.supervisor.open_interactions().front().interaction_id == idB,
              "IQ6: the rejected double-resolve left the live accessor's view of B untouched");

        // Now resolve B: both are answered, the run should advance past `suspended`.
        auto r4 = rig.resume(idB, text_message("B-answer"));
        check(r4.has_value() && r4->status != workflow_status::suspended,
              "IQ6: once BOTH ports are resolved the run advances (here: to `completed`, since "
              "neither port has an outgoing edge)");
        check(r4.has_value() && r4->open_interactions.empty(),
              "IQ6: no open interactions remain in the final reply");
        check(rig.supervisor.open_interactions().empty(),
              "IQ6: the live accessor agrees -- empty at the end too");
        if (r4) note("IQ6 final status", r4->status == workflow_status::completed ? "completed" : "other");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_request_port_independent_qa: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_request_port_independent_qa: %d FAILURE(S)\n", g_failures);
    return 1;
}
