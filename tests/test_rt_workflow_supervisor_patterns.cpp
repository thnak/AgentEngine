// Proof for ADR-037 Phase 3: agentengine::rt::WorkflowSupervisor's ROUTING behavior for the
// edge_kind::fan_in/switch_case/multi_selection cases (include/agentengine/rt/workflow_supervisor.hpp,
// route_from()), against the same fixture/helper conventions test_rt_workflow_supervisor.cpp already
// established. That file proves the superstep loop's core shape (C1-C5); this file proves the
// routing claims two now-deleted Quark-actor-based files (test_workflow_patterns.cpp,
// test_workflow_patterns_resilience.cpp) proved against the OLD agentengine::workflow::WorkflowSupervisor,
// re-run here against the NEW rt:: one:
//   FI  -- fan-in merges MULTIPLE sources into ONE delivery (the aggregator runs exactly once per
//          round, not once per inbound edge), and the merge order is deterministic (source graph
//          index order), not completion order.
//   SW  -- switch_case fires exactly the ONE edge whose case label the executor's returned route
//          names; the other declared case's target does not run.
//   RF  -- routing_failed (the I3 boundary): a switch_case edge set where the returned route matches
//          ZERO declared labels, or where it matches MORE THAN ONE, both fail the RUN with
//          `routing_failed` -- never `executor_failed` -- because the executor body itself succeeded;
//          this is a routing/authoring problem, not an execution failure.
//   MS  -- multi_selection fires a caller-chosen SUBSET: more than one label, but fewer than all
//          declared edges.
//   CY  -- a switch_case loop back to an earlier node (Reflection/Critic shape), bounded by
//          graph.bound.max_rounds: converges when the critic approves within budget, and stops
//          cleanly via bound_max_rounds when it never does.
//   RES -- the SAME pattern shapes (fan-in, switch_case, a cyclic moderator loop) re-run with one
//          participant injecting a transient failure recovered by an edge `retry` policy, asserting
//          the run still reaches the SAME correct final output as a happy-path control -- retry
//          composing correctly WITH these routing kinds, not just in isolation (C4 already proves
//          isolated retry recovery).
//
// MACHINE SAFETY (CLAUDE.md): bounded round counts; sleeps (fan-in ordering probe only) are tens of ms.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

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
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

// Same "safe here" reasoning as test_rt_workflow_supervisor.cpp's own drive<T>(): execute()'s only
// suspension points are run_mutex_'s uncontended fast path and a nested co_await that never itself
// suspends -- concurrent fan-out resolves through std::future::get(), not coroutine suspension -- so
// the whole chain resolves on ONE resume() call in this single-caller test file.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::role;
using agentengine::workflow::Edge;
using agentengine::workflow::EdgeFailurePolicy;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_failure_policy;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

// Every content item's text, joined with '+'. Fan-in merges by appending content items, so an
// aggregator's input has one item per contributing branch -- this is how a reducer reads them
// (ported from test_workflow_patterns.cpp's own all_text_of()).
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

[[nodiscard]] std::size_t content_count(Message const& m) { return m.content.size(); }

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}

// A plain node: appends its name to the incoming (joined) text, routes nowhere.
[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in,
                                    agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

// A node that sleeps before appending -- used to make fan-in COMPLETION order differ from SOURCE
// order, so FI-2 below is proving something a lucky scheduling order could not fake.
[[nodiscard]] ExecutorBody delayed_appender(std::string name, std::chrono::milliseconds delay) {
    return [name = std::move(name), delay](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

// Fails its first `fail_first_n` invocations with a transient error, then behaves like `appender` --
// the same idiom test_rt_workflow_supervisor.cpp's C4 and the old test_workflow_patterns_resilience.cpp's
// flaky_appender() both use.
[[nodiscard]] ExecutorBody flaky_appender(std::string name, std::uint32_t fail_first_n,
                                          std::shared_ptr<std::atomic<int>> seen) {
    return [name, fail_first_n, seen](Message const& in, agentengine::EffectContext&)
               -> agentengine::result<ExecutorOutcome> {
        if (seen->fetch_add(1) < static_cast<int>(fail_first_n)) {
            return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                        "injected failure in '" + name + "'",
                                                        "test.injected"});
        }
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

// A routing node: emits its payload and selects case labels computed from the incoming text.
[[nodiscard]] ExecutorBody router_node(std::string name,
                                       std::function<std::vector<std::string>(std::string const&)> chooser) {
    return [name = std::move(name), chooser = std::move(chooser)](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = all_text_of(in);
        return ExecutorOutcome{text_message(text + ">" + name), chooser(text)};
    };
}

// A counting wrapper: increments `count` on every invocation, then delegates. Used everywhere a
// check needs "did this node run, and how many times" rather than just its output.
[[nodiscard]] ExecutorBody counting(std::shared_ptr<std::atomic<int>> count, ExecutorBody inner) {
    return [count, inner = std::move(inner)](
               Message const& in, agentengine::EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
        count->fetch_add(1);
        return inner(in, ctx);
    };
}

// A retry policy edge, generous enough to absorb one or two injected failures below.
[[nodiscard]] EdgeFailurePolicy retry_policy() {
    return EdgeFailurePolicy{edge_failure_policy::retry, 3, {}};
}

}  // namespace

int main() {
    // ---- FI-1 / FI-2: fan-in merges MULTIPLE sources into ONE delivery, in deterministic --------
    // ---- (source-index) order, NOT completion order -----------------------------------------------
    {
        auto agg_calls = std::make_shared<std::atomic<int>>(0);

        Workflow wf;
        wf.id        = "faninmerge";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                        node_desc("agg")};
        for (char const* w : {"w1", "w2", "w3"}) {
            wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
            wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
        }
        wf.start = "src";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "FI setup: the fan-in graph validates");

        // Completion order is deliberately the REVERSE of source (graph edge declaration) order --
        // w1 is slowest, w3 fastest -- so a merge that happened to follow completion order would
        // visibly differ from one that follows source-index order.
        std::vector<ExecutorBody> bodies = {
            appender("src"),
            delayed_appender("w1", std::chrono::milliseconds(50)),
            delayed_appender("w2", std::chrono::milliseconds(5)),
            delayed_appender("w3", std::chrono::milliseconds(25)),
            counting(agg_calls, [](Message const& in,
                                   agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{
                    text_message(std::to_string(content_count(in)) + ":" + all_text_of(in))};
            }),
        };

        // Run it 3 times: FI-2's claim is about scheduling order, so a single pass proves nothing a
        // lucky thread-scheduling outcome couldn't fake by accident.
        for (int trial = 0; trial < 3; ++trial) {
            agg_calls->store(0);
            WorkflowSupervisor sup;
            sup.initialize(wf, bodies);
            WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
            check(r.status == workflow_status::completed, "FI setup: the run completes");
            check(agg_calls->load() == 1,
                  "FI-1: the aggregator ran EXACTLY ONCE despite THREE fan_in edges landing on it in "
                  "the same round -- three inbound edges merged into one delivery, not three separate "
                  "invocations");
            check(all_text_of(r.output) == "3:in>src>w1+in>src>w2+in>src>w3",
                  "FI-2: the merged content is in SOURCE GRAPH INDEX order (w1, w2, w3) even though "
                  "w1 (50ms) finished LAST and w3 (25ms) finished before w1 -- the merge order is "
                  "fixed by the graph, not by which branch's thread-pool job happened to finish first");
            if (trial == 0) note("fan-in merged", all_text_of(r.output));
        }
    }

    // ---- SW-1: switch_case fires exactly the ONE edge whose label the route names ----------------
    {
        auto billing_calls = std::make_shared<std::atomic<int>>(0);
        auto tech_calls    = std::make_shared<std::atomic<int>>(0);

        Workflow wf;
        wf.id        = "handoff";
        wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
        wf.start = "triage";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "SW-1 setup: the switch_case graph validates");

        std::vector<ExecutorBody> bodies = {
            router_node("triage", [](std::string const&) { return std::vector<std::string>{"billing"}; }),
            counting(billing_calls, appender("billing")),
            counting(tech_calls, appender("tech")),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed, "SW-1 setup: the run completes");
        check(all_text_of(r.output) == "in>triage>billing",
              "SW-1: control transferred to exactly the SELECTED branch ('billing')");
        check(billing_calls->load() == 1, "SW-1: the selected branch ran exactly once");
        check(tech_calls->load() == 0,
              "SW-1: the OTHER declared case's target ('tech') never ran -- switch_case is exactly "
              "one, not a fan-out whose extra results are discarded afterwards");
    }

    // ---- RF-1: ZERO declared labels match -> routing_failed, not executor_failed ------------------
    // Only ONE switch_case edge is declared ("billing"); the executor's returned route matches
    // nothing declared. route_from()'s reply.ok branch is only reached when the body itself SUCCEEDED
    // (see workflow_supervisor.hpp's own route_from(): the !reply.ok branch is a completely separate
    // code path that can never produce routing_failed) -- so reaching routing_failed here is itself
    // the proof the executor succeeded; this is a routing/authoring problem, not an execution failure.
    {
        auto billing_calls = std::make_shared<std::atomic<int>>(0);

        Workflow wf;
        wf.id        = "rf-zero";
        wf.executors = {node_desc("classify"), node_desc("billing")};
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing"});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "RF-1 setup: the graph validates");

        std::vector<ExecutorBody> bodies = {
            // Succeeds (no throw, no error result) but names a label NO declared edge carries.
            router_node("classify", [](std::string const&) { return std::vector<std::string>{"unknown"}; }),
            counting(billing_calls, appender("billing")),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::routing_failed,
              "RF-1: a switch_case node whose returned route matches ZERO declared labels ends the "
              "run with `routing_failed` -- reachable ONLY when the executor body itself succeeded, "
              "so this is provably a routing/authoring problem, not an execution failure");
        check(billing_calls->load() == 0, "RF-1: the unreachable target never ran");
    }

    // ---- RF-2: MORE THAN ONE declared label matches -> routing_failed (the reverse authoring -------
    // ---- mistake) ------------------------------------------------------------------------------------
    {
        Workflow wf;
        wf.id        = "rf-multi";
        wf.executors = {node_desc("classify"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"classify", "tech", edge_kind::switch_case, "tech"});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "RF-2 setup: the graph validates");

        std::vector<ExecutorBody> bodies = {
            // Succeeds, but returns TWO labels, both of which match declared edges.
            router_node("classify",
                       [](std::string const&) { return std::vector<std::string>{"billing", "tech"}; }),
            appender("billing"),
            appender("tech"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::routing_failed,
              "RF-2: a switch_case node whose returned route matches MORE THAN ONE declared label "
              "(route_from()'s own `switch_fired != 1` check) also ends the run with `routing_failed` "
              "-- the reverse authoring mistake from RF-1, same outcome");
    }

    // ---- MS-1: multi_selection fires a caller-chosen SUBSET -- more than one, fewer than all -------
    {
        auto r1_calls = std::make_shared<std::atomic<int>>(0);
        auto r2_calls = std::make_shared<std::atomic<int>>(0);
        auto r3_calls = std::make_shared<std::atomic<int>>(0);

        Workflow wf;
        wf.id        = "multiselect";
        wf.executors = {node_desc("pick"), node_desc("r1"), node_desc("r2"), node_desc("r3")};
        wf.edges.push_back(Edge{"pick", "r1", edge_kind::multi_selection, "r1"});
        wf.edges.push_back(Edge{"pick", "r2", edge_kind::multi_selection, "r2"});
        wf.edges.push_back(Edge{"pick", "r3", edge_kind::multi_selection, "r3"});
        wf.start = "pick";
        wf.output_selection.push_back("r1");
        wf.output_selection.push_back("r3");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "MS-1 setup: the multi_selection graph validates");

        std::vector<ExecutorBody> bodies = {
            router_node("pick", [](std::string const&) { return std::vector<std::string>{"r1", "r3"}; }),
            counting(r1_calls, appender("r1")),
            counting(r2_calls, appender("r2")),
            counting(r3_calls, appender("r3")),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed, "MS-1 setup: the run completes");
        check(r1_calls->load() == 1 && r3_calls->load() == 1,
              "MS-1: BOTH selected targets ran -- more than one, unlike switch_case");
        check(r2_calls->load() == 0,
              "MS-1: the UNselected target did not run -- fewer than all, unlike fan_out");
    }

    // ---- CY-1: a switch_case loop back to an earlier node converges when the critic approves -----
    // ---- within budget (Reflection/Critic shape) --------------------------------------------------
    {
        auto critic_calls = std::make_shared<std::atomic<int>>(0);

        Workflow wf;
        wf.id        = "cy-converge";
        wf.executors = {node_desc("writer"), node_desc("critic"), node_desc("done")};
        wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"critic", "writer", edge_kind::switch_case, "revise"});
        wf.edges.push_back(Edge{"critic", "done", edge_kind::switch_case, "approve"});
        wf.start = "writer";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "CY-1 setup: the cyclic switch_case graph validates");

        std::vector<ExecutorBody> bodies = {
            appender("writer"),
            [critic_calls](Message const& in,
                          agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                std::string const text = all_text_of(in) + ">critic";
                // Revise twice, then approve.
                if (critic_calls->fetch_add(1) < 2) {
                    return ExecutorOutcome{text_message(text), {"revise"}};
                }
                return ExecutorOutcome{text_message(text), {"approve"}};
            },
            appender("done"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "CY-1: the critic's cycle converges (runs dry at 'done') once it approves within the "
              "round bound, rather than being stopped by the bound");
        check(critic_calls->load() == 3, "CY-1: the critic ran three times -- revise, revise, approve");
        note("CY-1 output", text_of(r.output));
    }

    // ---- CY-2: the SAME cyclic shape is stopped CLEANLY by bound_max_rounds when it never ----------
    // ---- converges --------------------------------------------------------------------------------
    {
        Workflow wf;
        wf.id        = "cy-noconverge";
        wf.executors = {node_desc("writer"), node_desc("critic"), node_desc("done")};
        wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"critic", "writer", edge_kind::switch_case, "revise"});
        wf.edges.push_back(Edge{"critic", "done", edge_kind::switch_case, "approve"});
        wf.start = "writer";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 6;
        check(validate_workflow(wf).has_value(), "CY-2 setup: the graph validates");

        std::vector<ExecutorBody> bodies = {
            appender("writer"),
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                // Never approves.
                return ExecutorOutcome{text_message(all_text_of(in) + ">critic"), {"revise"}};
            },
            appender("done"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::bound_max_rounds,
              "CY-2: a critic cycle that never approves is stopped CLEANLY by graph.bound.max_rounds "
              "(no crash, no hang) rather than running forever");
        check(r.rounds == 6, "CY-2: the run executed exactly the bound's number of rounds");
    }

    // ---- RES-FI: fan-in composes with retry -- one branch transiently fails, is retried, and the ---
    // ---- aggregator STILL runs exactly once with the SAME merged output as a happy-path control ----
    {
        Workflow wf;
        wf.id        = "res-fanin";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                        node_desc("agg")};
        wf.edges.push_back(Edge{"src", "w1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w2", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w3", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"w1", "agg", edge_kind::fan_in, {}});
        Edge w2_edge{"w2", "agg", edge_kind::fan_in, {}};
        w2_edge.on_failure = retry_policy();
        wf.edges.push_back(w2_edge);
        wf.edges.push_back(Edge{"w3", "agg", edge_kind::fan_in, {}});
        wf.start = "src";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "RES-FI setup: the graph validates");

        auto run_it = [&](bool inject_failure) {
            auto agg_calls = std::make_shared<std::atomic<int>>(0);
            auto seen      = std::make_shared<std::atomic<int>>(0);
            std::vector<ExecutorBody> bodies = {
                appender("src"),
                appender("w1"),
                inject_failure ? flaky_appender("w2", 1, seen) : appender("w2"),
                appender("w3"),
                counting(agg_calls, [](Message const& in, agentengine::EffectContext&)
                             -> agentengine::result<ExecutorOutcome> {
                    return ExecutorOutcome{
                        text_message(std::to_string(content_count(in)) + ":" + all_text_of(in))};
                }),
            };
            WorkflowSupervisor sup;
            sup.initialize(wf, bodies);
            WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
            return std::pair{r, agg_calls->load()};
        };

        auto [control, control_agg_calls] = run_it(false);
        auto [flaky, flaky_agg_calls]      = run_it(true);
        check(control.status == workflow_status::completed && flaky.status == workflow_status::completed,
              "RES-FI setup: both the control and the retried run complete");
        check(control_agg_calls == 1 && flaky_agg_calls == 1,
              "RES-FI: the aggregator STILL ran exactly once even though one fan_in branch was "
              "retried -- a retried branch does not turn fan-in into a second delivery");
        check(all_text_of(flaky.output) == all_text_of(control.output),
              "RES-FI: a transient failure on one fan_in branch, retried, reaches the SAME merged "
              "output as the happy-path control -- retry composes correctly inside fan-in, not just "
              "in isolation");
        note("RES-FI control", all_text_of(control.output));
        note("RES-FI flaky", all_text_of(flaky.output));
    }

    // ---- RES-SW: switch_case composes with retry -- the CLASSIFIER itself transiently fails, is ---
    // ---- retried, and still routes to the SAME branch as a happy-path control ---------------------
    {
        Workflow wf;
        wf.id        = "res-switch";
        wf.executors = {node_desc("classify"), node_desc("billing"), node_desc("tech")};
        Edge to_billing{"classify", "billing", edge_kind::switch_case, "billing"};
        to_billing.on_failure = retry_policy();
        Edge to_tech{"classify", "tech", edge_kind::switch_case, "tech"};
        to_tech.on_failure = retry_policy();
        wf.edges.push_back(to_billing);
        wf.edges.push_back(to_tech);
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "RES-SW setup: the graph validates (both of "
                                                  "classify's outgoing edges share the retry policy)");

        auto run_it = [&](bool inject_failure) {
            auto seen = std::make_shared<std::atomic<int>>(0);
            std::vector<ExecutorBody> bodies = {
                inject_failure
                    ? [seen](Message const& in, agentengine::EffectContext&)
                          -> agentengine::result<ExecutorOutcome> {
                          if (seen->fetch_add(1) < 2) {
                              return std::unexpected(agentengine::error{
                                  agentengine::failure_class::transient,
                                  "injected failure in 'classify'", "test.injected"});
                          }
                          std::string const text = all_text_of(in);
                          return ExecutorOutcome{
                              text_message(text + ">classify"),
                              {text.find("refund") != std::string::npos ? "billing" : "tech"}};
                      }
                    : router_node("classify",
                                 [](std::string const& text) {
                                     return std::vector<std::string>{
                                         text.find("refund") != std::string::npos ? "billing" : "tech"};
                                 }),
                appender("billing"),
                appender("tech"),
            };
            WorkflowSupervisor sup;
            sup.initialize(wf, bodies);
            return drive(sup.run_workflow(RunWorkflow{text_message("refund")}));
        };

        WorkflowResult control = run_it(false);
        WorkflowResult flaky   = run_it(true);
        check(control.status == workflow_status::completed && flaky.status == workflow_status::completed,
              "RES-SW setup: both runs complete");
        check(all_text_of(flaky.output) == all_text_of(control.output),
              "RES-SW: the classifier transiently fails TWICE, is retried, and still routes to the "
              "SAME branch ('billing', from the 'refund' input) as the happy-path control -- the "
              "routing DECISION-MAKER recovering from a transient fault, not just a downstream leaf");
        note("RES-SW control", all_text_of(control.output));
        note("RES-SW flaky", all_text_of(flaky.output));
    }

    // ---- RES-CY: a cyclic (Group-Chat/Planner-moderator-style) switch_case loop composes with -----
    // ---- retry -- one participant transiently fails once, is retried, and the loop still reaches --
    // ---- the SAME final output as a happy-path control ---------------------------------------------
    {
        Workflow wf;
        wf.id        = "res-cyclic";
        wf.executors = {node_desc("writer"), node_desc("critic"), node_desc("done")};
        Edge writer_to_critic{"writer", "critic", edge_kind::direct, {}};
        writer_to_critic.on_failure = retry_policy();
        wf.edges.push_back(writer_to_critic);
        wf.edges.push_back(Edge{"critic", "writer", edge_kind::switch_case, "revise"});
        wf.edges.push_back(Edge{"critic", "done", edge_kind::switch_case, "approve"});
        wf.start = "writer";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "RES-CY setup: the graph validates");

        auto run_it = [&](bool inject_failure) {
            auto critic_calls = std::make_shared<std::atomic<int>>(0);
            auto seen          = std::make_shared<std::atomic<int>>(0);
            std::vector<ExecutorBody> bodies = {
                inject_failure ? flaky_appender("writer", 1, seen) : appender("writer"),
                [critic_calls](Message const& in, agentengine::EffectContext&)
                    -> agentengine::result<ExecutorOutcome> {
                    std::string const text = all_text_of(in) + ">critic";
                    if (critic_calls->fetch_add(1) < 1) return ExecutorOutcome{text_message(text), {"revise"}};
                    return ExecutorOutcome{text_message(text), {"approve"}};
                },
                appender("done"),
            };
            WorkflowSupervisor sup;
            sup.initialize(wf, bodies);
            return drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        };

        WorkflowResult control = run_it(false);
        WorkflowResult flaky   = run_it(true);
        check(control.status == workflow_status::completed && flaky.status == workflow_status::completed,
              "RES-CY setup: both the control and the retried run complete");
        check(all_text_of(flaky.output) == all_text_of(control.output),
              "RES-CY: 'writer' transiently fails on its first call inside the moderator-style "
              "revise/approve loop, is retried, and the loop still converges to the SAME final "
              "output as the happy-path control");
        note("RES-CY control", all_text_of(control.output));
        note("RES-CY flaky", all_text_of(flaky.output));
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_supervisor_patterns: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_supervisor_patterns: %d failure(s)\n", g_failures);
    return 1;
}
