// Implements 014-Workflow-and-Orchestration.md §8's promotion gate G1: "each §3 pattern is a
// runnable sample producing correct results under injected executor failures and delays."
// Milestone 6 Phase J (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md) -- the
// milestone's own exit criterion, half 1 of 2.
//
// Reuses test_workflow_patterns.cpp's own eight §3 graph constructions verbatim (Phase C already
// proved each one CORRECT in the happy path; this file is not re-deriving them) and injects, into
// ONE participating executor per pattern, either a TRANSIENT FAILURE (retry-recovered, the same
// `failing()` idiom test_workflow_failure.cpp's D2 already proved recovers) or a bounded DELAY (a
// real sleep inside the executor body -- test_workflow_superstep.cpp's own B2 precedent for
// simulating latency, not a new technique). Both injection kinds are exercised across the eight
// patterns, not one kind repeated eight times, since G1 names both nouns. Every assertion is the
// SAME correctness check the happy-path test already made -- G1's claim is that failure/delay does
// not change the RESULT, so the bar is identical output, not merely "the run didn't crash".
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded rounds. Delays are short (30-60ms),
// bounded, and live inside executor bodies (the established B2 precedent), never in this file's
// own top-level driving/polling code.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

[[nodiscard]] std::size_t content_count(Message const& m) { return m.content.size(); }

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        return text_message(all_text_of(in) + ">" + name);
    };
}

[[nodiscard]] ExecutorBody router_node(std::string name,
                                       std::function<std::vector<std::string>(std::string const&)> chooser) {
    return [name = std::move(name), chooser = std::move(chooser)](
               Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        std::string const text = all_text_of(in);
        return ExecutorOutcome{text_message(text + ">" + name), chooser(text)};
    };
}

// Fails its first `fail_first_n` invocations, then behaves like `appender` -- the exact D2 idiom
// (test_workflow_failure.cpp), reused rather than reinvented.
[[nodiscard]] ExecutorBody flaky_appender(std::string name, std::uint32_t fail_first_n) {
    auto seen = std::make_shared<std::uint32_t>(0);
    return [name, fail_first_n, seen](Message const& in,
                                      EffectContext&) -> ae::result<ExecutorOutcome> {
        if ((*seen)++ < fail_first_n) {
            return std::unexpected(
                ae::error{failure_class::transient, "injected G1 failure in '" + name + "'",
                         "test.injected"});
        }
        return text_message(all_text_of(in) + ">" + name);
    };
}

// A real, bounded sleep before producing an ordinary result -- G1's "delay" half, the same
// technique test_workflow_superstep.cpp's own B2 already uses to simulate latency.
[[nodiscard]] ExecutorBody delayed_appender(std::string name, std::chrono::milliseconds delay) {
    return [name, delay](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        std::this_thread::sleep_for(delay);
        return text_message(all_text_of(in) + ">" + name);
    };
}

constexpr std::uint64_t kSupervisorKey = 100;

struct Harness {
    Engine<>            engine;
    detail::MessagePool pool{64};
    LocalRouter          router;

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
        return router.get<WorkflowSupervisor>(kSupervisorKey);
    }

    [[nodiscard]] quark::result<WorkflowResult> run(Workflow const& wf, std::string input) {
        ActorRef<WorkflowSupervisor> sup = install(wf);
        engine.start();
        auto r = block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message(std::move(input))}));
        engine.stop();
        return r;
    }
};

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}

// A `retry` policy edge, budget 3 -- generous enough to absorb `flaky_appender`'s injected failures
// in every pattern below (each injects at most 2).
[[nodiscard]] EdgeFailurePolicy retry_policy() {
    return EdgeFailurePolicy{edge_failure_policy::retry, 3, {}};
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // ---- G1 / §3 row 1: SEQUENTIAL, failure injected on 'b' ----------------------------------------
    {
        Harness h(*config, 3);
        h.add_node("a", appender("a"));
        h.add_node("b", flaky_appender("b", /*fail_first_n=*/2));
        h.add_node("c", appender("c"));

        Workflow wf;
        wf.id        = "g1-sequential";
        wf.executors = {node_desc("a"), node_desc("b"), node_desc("c")};
        // b's OWN failure is governed by b's OWN outgoing edge, not a's.
        wf.edges.push_back(Edge{"a", "b", edge_kind::chain, {}, {}});
        wf.edges.push_back(Edge{"b", "c", edge_kind::chain, {}, retry_policy()});
        wf.start = "a";
        wf.output_selection.push_back("c");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed && all_text_of(r->output) == "in>a>b>c",
              "G1 Sequential: a transient failure on 'b', retried, still reaches the SAME output "
              "the happy-path test established");
    }

    // ---- G1 / §3 row 2: CONCURRENT, failure injected on one fan-out branch ('w2') ------------------
    {
        Harness h(*config, 5);
        h.add_node("src", appender("src"));
        h.add_node("w1", appender("w1"));
        h.add_node("w2", flaky_appender("w2", /*fail_first_n=*/1));
        h.add_node("w3", appender("w3"));
        h.add_node("agg", appender("agg"));

        Workflow wf;
        wf.id        = "g1-concurrent";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                        node_desc("agg")};
        // Policy for w2's OWN failure lives on w2's OWN outgoing edge (policy_for() reads a
        // node's OUTGOING edges -- "what happens when THIS executor fails" -- never its incoming
        // ones, which govern the SOURCE that delivered to it instead).
        wf.edges.push_back(Edge{"src", "w1", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"src", "w2", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"src", "w3", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"w1", "agg", edge_kind::fan_in, {}, {}});
        wf.edges.push_back(Edge{"w2", "agg", edge_kind::fan_in, {}, retry_policy()});
        wf.edges.push_back(Edge{"w3", "agg", edge_kind::fan_in, {}, {}});
        wf.start = "src";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "G1 Concurrent setup: the graph validates");

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed, "G1 Concurrent: the run completes despite w2's injected failure");
        if (r) {
            check(h.nodes[4]->invocations() == 1,
                  "G1 Concurrent: the aggregator STILL ran exactly once -- a retried branch does "
                  "not turn fan-in into a second delivery");
            note("aggregate", all_text_of(r->output));
        }
    }

    // ---- G1 / §3 row 3: HANDOFF, delay injected on the selected branch ('billing') -----------------
    {
        Harness h(*config, 3);
        h.add_node("triage", router_node("triage", [](std::string const&) {
                       return std::vector<std::string>{"billing"};
                   }));
        h.add_node("billing", delayed_appender("billing", std::chrono::milliseconds(40)));
        h.add_node("tech", appender("tech"));

        Workflow wf;
        wf.id        = "g1-handoff";
        wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing", {}});
        wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech", {}});
        wf.start = "triage";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed &&
                  all_text_of(r->output) == "in>triage>billing",
              "G1 Handoff: a 40ms delay on the selected branch changes WHEN it finishes, not WHAT "
              "it produces -- control still transferred to exactly the selected branch");
    }

    // ---- G1 / §3 row 7: ROUTER, failure injected on the CLASSIFIER itself -------------------------
    // `billing`/`tech` are terminal (no outgoing edges of their own, since they're only
    // output-selected) -- `policy_for()` reads a node's OWN outgoing edges, so a terminal node has
    // no edge to declare a retry policy on and would always get the strict default regardless of
    // what its INCOMING edge says. The node that CAN meaningfully retry here is `classify` itself
    // (both its outgoing edges), which also makes a more interesting claim: the routing
    // DECISION-MAKER recovering from a transient fault still routes correctly, not just a
    // downstream leaf recovering.
    {
        Workflow wf;
        wf.id        = "g1-router";
        wf.executors = {node_desc("classify"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing", retry_policy()});
        wf.edges.push_back(Edge{"classify", "tech", edge_kind::switch_case, "tech", retry_policy()});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "G1 Router setup: the graph validates");

        auto seen = std::make_shared<std::uint32_t>(0);
        Harness h(*config, 3);
        h.add_node("classify",
                  [seen](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
                      if ((*seen)++ < 2) {
                          return std::unexpected(ae::error{failure_class::transient,
                                                           "injected G1 failure in 'classify'",
                                                           "test.injected"});
                      }
                      std::string const text = all_text_of(in);
                      return ExecutorOutcome{
                          text_message(text + ">classify"),
                          {text.find("refund") != std::string::npos ? "billing" : "tech"}};
                  });
        h.add_node("billing", appender("billing"));
        h.add_node("tech", appender("tech"));

        auto r = h.run(wf, "refund");
        check(r && r->status == workflow_status::completed &&
                  all_text_of(r->output) == "refund>classify>billing",
              "G1 Router: the classifier itself transiently fails twice, retries, and still "
              "routes to the SAME branch the happy-path test established");
    }

    // ---- G1 / §3 rows 4/5: GROUP CHAT (delay) and PLANNER (failure) --------------------------------
    {
        Harness h(*config, 3);
        h.add_node("moderator", router_node("mod", [](std::string const& text) {
                       std::size_t turns = 0;
                       for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                           if (text[i] == '>' && text[i + 1] == 'm') ++turns;
                       }
                       return std::vector<std::string>{turns % 2 == 0 ? "p1" : "p2"};
                   }));
        h.add_node("p1", delayed_appender("p1", std::chrono::milliseconds(30)));
        h.add_node("p2", appender("p2"));

        Workflow wf;
        wf.id        = "g1-groupchat";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1", {}});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2", {}});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("p1");
        wf.output_selection.push_back("p2");
        wf.bound.max_rounds = 6;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::bound_max_rounds,
              "G1 Group chat: a 30ms delay on p1 still ends at the CALLER's round bound -- the "
              "termination contract §3 says distinguishes this row is unaffected by latency");
        if (r) {
            check(h.nodes[1]->invocations() > 0 && h.nodes[2]->invocations() > 0,
                  "G1 Group chat: the moderator still cycled among BOTH participants despite p1's delay");
        }
    }
    {
        Harness h(*config, 4);
        h.add_node("moderator", router_node("mod", [](std::string const& text) {
                       std::size_t turns = 0;
                       for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                           if (text[i] == '>' && text[i + 1] == 'm') ++turns;
                       }
                       if (turns >= 2) return std::vector<std::string>{"done"};
                       return std::vector<std::string>{turns % 2 == 0 ? "p1" : "p2"};
                   }));
        h.add_node("p1", appender("p1"));
        h.add_node("p2", flaky_appender("p2", /*fail_first_n=*/1));
        h.add_node("done", appender("done"));

        Workflow wf;
        wf.id = "g1-planner";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2"), node_desc("done")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1", {}});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2", {}});
        wf.edges.push_back(Edge{"moderator", "done", edge_kind::switch_case, "done", {}});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}, retry_policy()});
        wf.start = "moderator";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 20;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed,
              "G1 Planner: the moderator still reaches its OWN completion decision despite p2's "
              "injected failure -- ends by running dry, not by exhausting the safety-valve bound");
        if (r) {
            check(r->rounds < 20, "G1 Planner: the round bound remains a safety valve, never reached");
            check(all_text_of(r->output).find(">done") != std::string::npos,
                  "G1 Planner: the terminal node still produced the result");
            note("planner rounds", std::to_string(r->rounds));
        }
    }

    // ---- G1 / §3 row 6: MAP-REDUCE, failure injected on one mapper ('m2') -------------------------
    {
        Harness h(*config, 5);
        h.add_node("split", appender("split"));
        h.add_node("m1", appender("m1"));
        h.add_node("m2", flaky_appender("m2", /*fail_first_n=*/1));
        h.add_node("m3", appender("m3"));
        h.add_node("reduce", [](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
            return text_message("reduced(" + std::to_string(content_count(in)) + ")");
        });

        Workflow wf;
        wf.id = "g1-mapreduce";
        wf.executors = {node_desc("split"), node_desc("m1"), node_desc("m2"), node_desc("m3"),
                        node_desc("reduce")};
        // m2's OWN failure is governed by m2's OWN outgoing edge, same reasoning as Concurrent above.
        wf.edges.push_back(Edge{"split", "m1", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"split", "m2", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"split", "m3", edge_kind::fan_out, {}, {}});
        wf.edges.push_back(Edge{"m1", "reduce", edge_kind::fan_in, {}, {}});
        wf.edges.push_back(Edge{"m2", "reduce", edge_kind::fan_in, {}, retry_policy()});
        wf.edges.push_back(Edge{"m3", "reduce", edge_kind::fan_in, {}, {}});
        wf.start = "split";
        wf.output_selection.push_back("reduce");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed &&
                  all_text_of(r->output) == "reduced(3)",
              "G1 Map-reduce: the reducer still received one item per mapper (three), despite m2's "
              "injected failure being retried before the fan-in barrier closed");
    }

    // ---- G1 / §3 row 8: REFLECTION / CRITIC, delay injected on 'critic' ---------------------------
    {
        Harness h(*config, 2);
        h.add_node("writer", appender("w"));
        h.add_node("critic", delayed_appender("c", std::chrono::milliseconds(30)));

        Workflow wf;
        wf.id        = "g1-reflection";
        wf.executors = {node_desc("writer"), node_desc("critic")};
        wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}, {}});
        wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}, {}});
        wf.start            = "writer";
        wf.output_selection.push_back("critic");
        wf.bound.max_rounds = 6;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::bound_max_rounds && r->rounds == 6,
              "G1 Reflection: a 30ms delay on every critic turn still iterates to the SAME explicit "
              "bound the happy-path test established -- latency changes wall-clock time, not the "
              "round count the bound is measured in");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_patterns_resilience: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_patterns_resilience: %d FAILURE(S)\n", g_failures);
    return 1;
}
