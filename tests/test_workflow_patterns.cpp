// Implements 014-Workflow-and-Orchestration.md §3; Milestone 6 Phase C
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// §3's claim is that its eight named patterns are "configurations of the graph, not separate
// subsystems". This file is that claim's test: every row of §3's table is built here from nothing
// but the §1 vocabulary (executors, the six edge kinds, a termination bound) and run on the Phase B
// superstep engine. If a pattern needed an engine primitive §1-§2 does not provide, §3 would be
// wrong -- so what is NOT here matters as much as what is (see the deferral note at the end).
//
// Phase C added exactly two things to the engine, and both are §1 edge kinds Phase A already
// declared and validated rather than new subsystems:
//   * switch/case + multi-selection ROUTING -- §1 lists both as edge kinds; Phase B fired every
//     edge unconditionally, so Handoff and Router were unbuildable.
//   * fan-in AGGREGATION -- §2 states outright that the superstep model "makes fan-in well-defined";
//     Phase B produced one delivery per inbound edge, so an aggregator ran N times instead of once
//     with N inputs.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded rounds, no sleeps in this file.

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

// Every content item's text, joined. Fan-in merges by appending content items, so an aggregator's
// input has one item per contributing branch -- this is how a reducer reads them.
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

// A plain node: appends its name to the incoming text, routes nowhere.
[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        return text_message(all_text_of(in) + ">" + name);
    };
}

// A routing node: emits its payload and selects case labels. `chooser` sees the incoming text, so a
// "classifier" here is an ordinary function of its input -- exactly the shape §3's Router row
// describes, with a model-backed classifier substituting for this lambda in Phase C's agent kind.
[[nodiscard]] ExecutorBody router_node(std::string name,
                                       std::function<std::vector<std::string>(std::string const&)> chooser) {
    return [name = std::move(name), chooser = std::move(chooser)](
               Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        std::string const text = all_text_of(in);
        return ExecutorOutcome{text_message(text + ">" + name), chooser(text)};
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
        // Decision 5b: sequential keys collide on one shard and silently serialize a round.
        keys = spread_executor_keys(node_count, engine.shard_count(), [this](std::uint64_t k) {
            return engine.shard_of(actor_id_of<FunctionExecutor>(k));
        });
    }

    void add_node(std::string name, ExecutorBody body) {
        auto node = std::make_unique<FunctionExecutor>();
        node->initialize(std::move(name), std::move(body), EffectContext{});
        auto act = std::make_unique<Activation>(node.get(), FunctionExecutor::dispatch_table(),
                                                pool.sink());
        engine.register_activation(actor_id_of<FunctionExecutor>(keys[nodes.size()]), *act);
        nodes.push_back(std::move(node));
        node_acts.push_back(std::move(act));
    }

    [[nodiscard]] ActorRef<WorkflowSupervisor> install(Workflow const& wf) {
        for (std::size_t i = 0; i < nodes.size(); ++i) refs.push_back(router.get<FunctionExecutor>(keys[i]));
        supervisor.initialize(wf, refs);
        sup_act = std::make_unique<Activation>(&supervisor, WorkflowSupervisor::dispatch_table(),
                                               pool.sink());
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

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!config) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    // ---- §3 row 1: SEQUENTIAL = chain ---------------------------------------------------------
    {
        Harness h(*config, 3);
        for (char const* n : {"a", "b", "c"}) h.add_node(n, appender(n));

        Workflow wf;
        wf.id        = "sequential";
        wf.executors = {node_desc("a"), node_desc("b"), node_desc("c")};
        wf.edges.push_back(Edge{"a", "b", edge_kind::chain, {}});
        wf.edges.push_back(Edge{"b", "c", edge_kind::chain, {}});
        wf.start = "a";
        wf.output_selection.push_back("c");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed && all_text_of(r->output) == "in>a>b>c",
              "§3 Sequential: a chain runs its nodes in order and terminates by running dry");
    }

    // ---- §3 row 2: CONCURRENT = fan-out + fan-in with an aggregator ------------------------------
    // The row that Phase B could not express. Without fan-in MERGING, `agg` would have run three
    // times -- once per inbound edge -- which is a different program that still produces a plausible
    // answer, so this is checked on the aggregator's INPUT CARDINALITY, not just its output text.
    {
        Harness h(*config, 5);
        h.add_node("src", appender("src"));
        for (char const* n : {"w1", "w2", "w3"}) h.add_node(n, appender(n));
        h.add_node("agg", appender("agg"));

        Workflow wf;
        wf.id = "concurrent";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                        node_desc("agg")};
        for (char const* w : {"w1", "w2", "w3"}) {
            wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
            wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
        }
        wf.start = "src";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "§3 Concurrent: the graph validates");

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed, "§3 Concurrent: the run completes");
        if (r) {
            note("aggregate", all_text_of(r->output));
            check(h.nodes[4]->invocations() == 1,
                  "§3 Concurrent (014 §2 'makes fan-in well-defined'): the aggregator ran EXACTLY "
                  "ONCE. Three inbound fan_in edges merged into one delivery -- without that it "
                  "would have run three times and still produced a plausible-looking answer");
            check(r->rounds == 3, "§3 Concurrent: source, workers, aggregator = 3 rounds");
        }
    }

    // ---- fan-in's merge is deterministic and lossless ---------------------------------------------
    // Asserted separately from the pattern above because it is the property everything downstream
    // relies on: the aggregator must see EVERY branch, in an order fixed by the graph rather than by
    // which branch finished first.
    {
        Harness h(*config, 5);
        h.add_node("src", appender("src"));
        for (char const* n : {"w1", "w2", "w3"}) h.add_node(n, appender(n));
        h.add_node("agg", [](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
            // Reports what it actually received, rather than transforming it.
            return text_message(std::to_string(content_count(in)) + ":" + all_text_of(in));
        });

        Workflow wf;
        wf.id = "faninmerge";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                        node_desc("agg")};
        for (char const* w : {"w1", "w2", "w3"}) {
            wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
            wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
        }
        wf.start = "src";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r.has_value(), "fan-in merge: the run completes");
        if (r) {
            note("aggregator saw", all_text_of(r->output));
            check(all_text_of(r->output) == "3:in>src>w1+in>src>w2+in>src>w3",
                  "fan-in merge: the aggregator received THREE content items, one per branch, in "
                  "graph index order -- lossless (nothing overwritten) and deterministic (source "
                  "index order, not completion order)");
        }
    }

    // ---- §3 row 3: HANDOFF = switch/case on a routing decision, control transfer -------------------
    {
        Harness h(*config, 3);
        h.add_node("triage", router_node("triage", [](std::string const&) {
                       return std::vector<std::string>{"billing"};
                   }));
        h.add_node("billing", appender("billing"));
        h.add_node("tech", appender("tech"));

        Workflow wf;
        wf.id        = "handoff";
        wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
        wf.start = "triage";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "§3 Handoff: the graph validates");

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed, "§3 Handoff: the run completes");
        if (r) {
            check(all_text_of(r->output) == "in>triage>billing",
                  "§3 Handoff: control transferred to the SELECTED branch");
            check(h.nodes[2]->invocations() == 0,
                  "§3 Handoff: the unselected branch never ran -- switch/case is exactly one, not a "
                  "fan-out whose extra results are discarded afterwards");
        }
    }

    // ---- §3 row 7: ROUTER = switch/case on a classifier's typed output ------------------------------
    // Same edge kind as Handoff; the pattern differs in that the label is COMPUTED from the input.
    // Both branches are exercised, so this proves the classifier's output actually steers.
    {
        Workflow wf;
        wf.id        = "router";
        wf.executors = {node_desc("classify"), node_desc("billing"), node_desc("tech")};
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"classify", "tech", edge_kind::switch_case, "tech"});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.output_selection.push_back("tech");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "§3 Router: the graph validates");

        // Both inputs through the SAME graph. One probe would not distinguish a working classifier
        // from a node hardcoded to one branch.
        for (auto const& input : {std::string("refund"), std::string("crash")}) {
            Harness h(*config, 3);
            h.add_node("classify", router_node("classify", [](std::string const& text) {
                           return std::vector<std::string>{
                               text.find("refund") != std::string::npos ? "billing" : "tech"};
                       }));
            h.add_node("billing", appender("billing"));
            h.add_node("tech", appender("tech"));

            auto r = h.run(wf, input);
            check(r && r->status == workflow_status::completed, "§3 Router: the run completes");
            if (r) {
                std::string const expected =
                    input + ">classify>" + (input == "refund" ? "billing" : "tech");
                check(all_text_of(r->output) == expected,
                      "§3 Router: the classifier's own typed output selected the branch");
                note("routed", all_text_of(r->output));
            }
        }
    }

    // ---- I3 BOUNDARY: an executor cannot invent a route target --------------------------------------
    // The check that keeps §3's Router row inside I3. A classifier is model-derived output in
    // production; it must be able to CHOOSE among declared edges and nothing more. A label naming a
    // node the author never wired must reach nothing -- and must be reported, not silently end the
    // branch with an empty result the caller cannot distinguish from success.
    {
        Harness h(*config, 3);
        h.add_node("classify", router_node("classify", [](std::string const&) {
                       // Names a real executor -- but by ID, and via a label no edge carries.
                       return std::vector<std::string>{"tech", "admin_override"};
                   }));
        h.add_node("billing", appender("billing"));
        h.add_node("tech", appender("tech"));

        Workflow wf;
        wf.id        = "i3";
        wf.executors = {node_desc("classify"), node_desc("billing"), node_desc("tech")};
        // NOTE: only ONE switch_case edge is declared, labelled "billing". The executor's "tech"
        // and "admin_override" labels match no declared edge.
        wf.edges.push_back(Edge{"classify", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"classify", "tech", edge_kind::direct, {}});
        wf.start = "classify";
        wf.output_selection.push_back("billing");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r.has_value(), "I3: the run returns");
        if (r) {
            check(r->status == workflow_status::routing_failed,
                  "I3: a switch/case node whose labels match NO declared edge fails the run with "
                  "`routing_failed` -- not `completed` with an empty output, which a caller could "
                  "not tell from success");
            check(h.nodes[1]->invocations() == 0,
                  "I3: the executor's invented label reached NOTHING. Routing selects among edges "
                  "the graph declares; an executor -- including one fed by a model -- cannot name a "
                  "target the author did not wire (I3: model output is data, never authority)");
        }
    }

    // ---- §3 row 8: REFLECTION / CRITIC = a cycle with an explicit iteration bound ---------------------
    {
        Harness h(*config, 2);
        h.add_node("writer", appender("w"));
        h.add_node("critic", appender("c"));

        Workflow wf;
        wf.id        = "reflection";
        wf.executors = {node_desc("writer"), node_desc("critic")};
        wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}});
        wf.start            = "writer";
        wf.output_selection.push_back("critic");
        wf.bound.max_rounds = 6;
        check(validate_workflow(wf).has_value(),
              "§3 Reflection: a cyclic graph with a bound validates (014 §9 Q2)");

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::bound_max_rounds && r->rounds == 6,
              "§3 Reflection: the critic cycle iterates until its explicit bound, and the caller is "
              "told the bound stopped it");
        if (r) note("after 6 rounds", all_text_of(r->output));
    }

    // ---- §3 rows 4/5: GROUP CHAT / DEBATE, and PLANNER (MAGENTIC) --------------------------------------
    // §3 gives these the SAME graph shape -- "a moderator executor cycling among participants" --
    // and distinguishes them by who owns the stopping decision. So they are built from one graph
    // here, twice, with only the moderator's body differing. Building them as two different graph
    // shapes would contradict §3's own text.
    {
        // Group chat: the CALLER's round bound is the termination contract (§3's own wording). The
        // moderator just cycles.
        Harness h(*config, 3);
        h.add_node("moderator", router_node("mod", [](std::string const& text) {
                       // Alternate participants by how many turns have happened.
                       std::size_t turns = 0;
                       for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                           if (text[i] == '>' && text[i + 1] == 'm') ++turns;
                       }
                       return std::vector<std::string>{turns % 2 == 0 ? "p1" : "p2"};
                   }));
        h.add_node("p1", appender("p1"));
        h.add_node("p2", appender("p2"));

        Workflow wf;
        wf.id        = "groupchat";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("p1");
        wf.output_selection.push_back("p2");
        wf.bound.max_rounds = 6;
        check(validate_workflow(wf).has_value(), "§3 Group chat: the moderator-cycle graph validates");

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::bound_max_rounds,
              "§3 Group chat: the CALLER's round bound is the termination contract -- the run ends "
              "at the bound, which is exactly what §3 says distinguishes this row from Planner");
        if (r) {
            check(h.nodes[1]->invocations() > 0 && h.nodes[2]->invocations() > 0,
                  "§3 Group chat: the moderator cycled among BOTH participants, not just one");
            note("group chat transcript", all_text_of(r->output));
        }
    }
    {
        // Planner (Magentic): the SAME graph shape, but the moderator owns completion -- it routes to
        // a terminal `done` node when its own ledger says the goal is met, and the round bound is a
        // safety valve it never reaches. That difference in OUTCOME is the whole distinction §3 draws.
        Harness h(*config, 4);
        h.add_node("moderator", router_node("mod", [](std::string const& text) {
                       std::size_t turns = 0;
                       for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                           if (text[i] == '>' && text[i + 1] == 'm') ++turns;
                       }
                       // The ledger: two participant turns and the goal is met.
                       if (turns >= 2) return std::vector<std::string>{"done"};
                       return std::vector<std::string>{turns % 2 == 0 ? "p1" : "p2"};
                   }));
        h.add_node("p1", appender("p1"));
        h.add_node("p2", appender("p2"));
        h.add_node("done", appender("done"));

        Workflow wf;
        wf.id = "planner";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2"), node_desc("done")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"moderator", "done", edge_kind::switch_case, "done"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 20;  // a safety valve, deliberately far above what the ledger needs
        check(validate_workflow(wf).has_value(), "§3 Planner: the same cyclic shape validates");

        auto r = h.run(wf, "in");
        check(r.has_value(), "§3 Planner: the run returns");
        if (r) {
            check(r->status == workflow_status::completed,
                  "§3 Planner: the MODERATOR decided completion and routed to a terminal node -- the "
                  "run ended by running dry, NOT at its bound. That is the one observable difference "
                  "from Group chat above, which shares this graph shape exactly");
            check(r->rounds < 20,
                  "§3 Planner: the round bound was a safety valve, never the termination contract");
            check(all_text_of(r->output).find(">done") != std::string::npos,
                  "§3 Planner: the terminal node produced the result");
            note("planner rounds", std::to_string(r->rounds));
            note("planner output", all_text_of(r->output));
        }
    }

    // ---- §3 row 6: MAP-REDUCE = fan-out over a collection + fan-in reducer -----------------------------
    // Built over a FIXED mapper set, each mapper taking one slice. What is deliberately NOT built is
    // a data-driven mapper COUNT (K mappers for K items, K known only at runtime): 014 §9 Q3 already
    // resolved that as "a runtime instance count... orthogonal to which kinds exist", i.e. per-node
    // instance multiplicity rather than a graph-structural feature. It is named in the breakdown as
    // not built rather than faked here -- K deliveries to one mapper actor would serialize on that
    // actor and quietly stop being a map.
    {
        Harness h(*config, 5);
        h.add_node("split", appender("split"));
        for (char const* n : {"m1", "m2", "m3"}) h.add_node(n, appender(n));
        h.add_node("reduce", [](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
            return text_message("reduced(" + std::to_string(content_count(in)) + ")");
        });

        Workflow wf;
        wf.id = "mapreduce";
        wf.executors = {node_desc("split"), node_desc("m1"), node_desc("m2"), node_desc("m3"),
                        node_desc("reduce")};
        for (char const* m : {"m1", "m2", "m3"}) {
            wf.edges.push_back(Edge{"split", m, edge_kind::fan_out, {}});
            wf.edges.push_back(Edge{m, "reduce", edge_kind::fan_in, {}});
        }
        wf.start = "split";
        wf.output_selection.push_back("reduce");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed &&
                  all_text_of(r->output) == "reduced(3)",
              "§3 Map-reduce: the reducer received one item per mapper and ran once");
    }

    // ---- multi-selection: a caller-chosen SUBSET fires ---------------------------------------------------
    // The sixth edge kind, and the one with no §3 row of its own. Distinguished from switch/case by
    // being allowed to select more than one -- and from fan-out by being allowed to select fewer than
    // all. Both halves are checked, since a multi-selection that always fired everything would be an
    // undetected fan-out.
    {
        Harness h(*config, 4);
        h.add_node("pick", router_node("pick", [](std::string const&) {
                       return std::vector<std::string>{"r1", "r3"};
                   }));
        for (char const* n : {"r1", "r2", "r3"}) h.add_node(n, appender(n));

        Workflow wf;
        wf.id = "multiselect";
        wf.executors = {node_desc("pick"), node_desc("r1"), node_desc("r2"), node_desc("r3")};
        wf.edges.push_back(Edge{"pick", "r1", edge_kind::multi_selection, "r1"});
        wf.edges.push_back(Edge{"pick", "r2", edge_kind::multi_selection, "r2"});
        wf.edges.push_back(Edge{"pick", "r3", edge_kind::multi_selection, "r3"});
        wf.start = "pick";
        wf.output_selection.push_back("r3");
        wf.bound.max_rounds = 8;

        auto r = h.run(wf, "in");
        check(r && r->status == workflow_status::completed, "multi-selection: the run completes");
        if (r) {
            check(h.nodes[1]->invocations() == 1 && h.nodes[3]->invocations() == 1,
                  "multi-selection: BOTH selected reviewers ran -- more than one, unlike switch/case");
            check(h.nodes[2]->invocations() == 0,
                  "multi-selection: the UNselected reviewer did not -- fewer than all, unlike fan-out");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_patterns: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_patterns: %d FAILURE(S)\n", g_failures);
    return 1;
}
