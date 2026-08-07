// Implements 014-Workflow-and-Orchestration.md §2; Milestone 6 Phase B
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// Drives a REAL `quark::Engine` -- not `TestKit`, which has no async carrier and therefore cannot
// host a supervisor whose whole job is `co_await`ing cross-actor asks (the breakdown's decision 3;
// `agent_session.hpp:243-248` names that boundary). Construction shape follows
// `test_agent_session_suspend_resume.cpp`, the M4 precedent: locally-owned `Activation`s registered
// via `register_activation`, because `Engine::spawn<A>()` returns only an `ActorId` and each node
// needs `initialize()` called on it first.
//
// `quark::Activation` is non-movable BY DESIGN (its move constructor is explicitly deleted), so the
// N-node graphs here hold their activations and executors through `unique_ptr` -- a vector of them
// directly does not compile. The M4 precedent never hit this because it hosts exactly one actor on
// the stack; this is the first test in the project to register a variable number.
//
// The two claims worth the trouble of a real Engine are B1 and B2, and both are MEASURED rather than
// asserted from the code's shape:
//   * B1 -- the superstep barrier. Executors timestamp entry and exit; the check is that no
//     round-(n+1) entry precedes any round-n exit. A test that merely observed correct OUTPUT would
//     pass just as happily against a fully serialized implementation, or against one that let rounds
//     bleed into each other whenever the payloads happened not to collide.
//   * B2 -- fan-out really overlaps. Three nodes in one round each sleep; the round's wall-clock is
//     compared against the serial sum. This is the check that fails if someone writes the obvious
//     `co_await ref.ask(...)` inside the dispatch loop, which is correct and silently turns 014 §3's
//     Concurrent pattern into Sequential.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards, bounded round counts, sleeps in tens of ms.

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
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

// --- Message payload helpers -------------------------------------------------------------------

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

// --- The round-boundary trace that makes B1 a measurement ---------------------------------------

struct Span {
    std::string                           executor;
    std::chrono::steady_clock::time_point entered;
    std::chrono::steady_clock::time_point exited;
};

std::mutex        g_trace_mutex;
std::vector<Span> g_trace;

void record(Span s) {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace.push_back(std::move(s));
}

// A body that appends its own name to the payload text, optionally sleeping to make overlap (or its
// absence) measurable, and always recording its span.
[[nodiscard]] ExecutorBody tracing_body(std::string name, std::chrono::milliseconds work) {
    return [name = std::move(name), work](Message const& in, EffectContext&) -> ae::result<Message> {
        Span span;
        span.executor = name;
        span.entered  = std::chrono::steady_clock::now();
        if (work.count() > 0) std::this_thread::sleep_for(work);
        span.exited = std::chrono::steady_clock::now();
        record(span);
        return text_message(text_of(in) + ">" + name);
    };
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr std::uint64_t kSupervisorKey = 100;

// A whole engine + graph, assembled and torn down per case, so each case below reads as the graph it
// is testing rather than as twenty lines of Quark bring-up. Member order matters: `pool` must
// outlive `router`, and both must be constructed after `engine`.
struct Harness {
    Engine<>            engine;
    detail::MessagePool pool{64};
    LocalRouter         router;

    std::vector<std::unique_ptr<FunctionExecutor>> nodes;
    std::vector<std::unique_ptr<Activation>>       node_acts;
    WorkflowSupervisor                             supervisor;
    std::unique_ptr<Activation>                    sup_act;
    std::vector<ActorRef<FunctionExecutor>>        refs;

    // Keys are chosen by `spread_executor_keys`, NOT by counting 1,2,3,... The obvious sequential
    // choice put keys 2/3/4 on the same shard of a 4-shard engine, which silently serialized this
    // file's own B2 fan-out at 184 ms -- see workflow/placement.hpp's banner.
    std::vector<std::uint64_t> keys;

    explicit Harness(EngineConfig const& cfg, std::size_t node_count)
        : engine(cfg), router(engine.post_courier(), pool) {
        keys = spread_executor_keys(node_count, engine.shard_count(),
                                    [this](std::uint64_t k) {
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

    // Wires the supervisor over whatever nodes were added, in the same index order the graph
    // description declares them.
    [[nodiscard]] ActorRef<WorkflowSupervisor> install(Workflow const& wf) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            refs.push_back(router.get<FunctionExecutor>(keys[i]));
        }
        supervisor.initialize(wf, refs);
        sup_act = make_workflow_activation(supervisor, pool.sink());
        engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_act);
        return router.get<WorkflowSupervisor>(kSupervisorKey);
    }
};

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(config.has_value(), "engine config builds (4 workers / 4 shards, machine-safety cap)");
    if (!config) return 1;

    // ---- B0: spread_executor_keys, against synthetic placement functions ---------------------------
    // A pure function with three edge cases that would each be a real outage in a different way, so
    // they are checked against a SYNTHETIC shard_of rather than only through the engine: the engine's
    // real hash cannot be made pathological on demand, and "it worked on this hash" is not the claim.
    {
        // Perfect spread when count <= shard_count, under the identity placement.
        auto identity = [](std::uint64_t k) { return static_cast<std::uint32_t>(k); };
        auto keys     = spread_executor_keys(4, 4, identity);
        check(keys.size() == 4, "B0: returns exactly the requested number of keys");
        bool distinct = true;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            for (std::size_t j = i + 1; j < keys.size(); ++j) {
                if ((identity(keys[i]) % 4) == (identity(keys[j]) % 4)) distinct = false;
            }
        }
        check(distinct, "B0: 4 nodes over 4 shards land on 4 distinct shards");

        // More nodes than shards: doubling up is correct, refusing to return keys is not.
        auto many = spread_executor_keys(9, 4, identity);
        check(many.size() == 9,
              "B0: count > shard_count still yields every key -- the ceiling target lets shards "
              "double up rather than starving the caller");

        // A DEGENERATE hash: everything on shard 0. The bounded search cannot spread this, and the
        // only acceptable behaviour is to give up and return keys anyway. This is the case that
        // would hang under an unbounded 'search until balanced' loop.
        auto degenerate = [](std::uint64_t) { return std::uint32_t{0}; };
        auto forced     = spread_executor_keys(5, 4, degenerate);
        check(forced.size() == 5,
              "B0: a hash that maps EVERY key to one shard still returns the full key list -- the "
              "search is bounded, so a pathological placement costs a worse spread, never a hang");

        // Single shard: nothing to spread across.
        auto single = spread_executor_keys(3, 1, identity);
        check(single.size() == 3 && single[0] == 1 && single[1] == 2 && single[2] == 3,
              "B0: with one shard the keys are simply sequential -- no wasted key search");
    }

    // ---- B1: the superstep barrier, measured -----------------------------------------------------
    // A 3-round chain where each round's node sleeps 30 ms. If rounds could overlap, some entry
    // timestamp would precede an earlier round's exit. Deliberately a CHAIN (one node per round), so
    // any overlap observed is a round-boundary violation and not fan-out concurrency.
    {
        g_trace.clear();
        Harness h(*config, 3);
        for (char const* n : {"a", "b", "c"}) {
            h.add_node(n, tracing_body(n, std::chrono::milliseconds(30)));
        }

        Workflow wf;
        wf.id = "chain";
        wf.executors = {node_desc("a"), node_desc("b"), node_desc("c")};
        wf.edges.push_back(Edge{"a", "b", edge_kind::chain, {}});
        wf.edges.push_back(Edge{"b", "c", edge_kind::chain, {}});
        wf.start = "a";
        wf.output_selection.push_back("c");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "B1: the chain graph validates");

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value(), "B1: the workflow run completes");
        if (r) {
            check(r->status == workflow_status::completed,
                  "B1: a chain terminates by running dry at its terminal executor (014 §2), not by "
                  "hitting its bound");
            check(r->rounds == 3, "B1: exactly one round per chain link");
            check(text_of(r->output) == "in>a>b>c",
                  "B1: the payload threaded through every node in graph order");
            note("output", text_of(r->output));
        }

        check(g_trace.size() == 3, "B1: three spans recorded, one per round");
        bool barrier_held = g_trace.size() == 3;
        for (std::size_t i = 1; i < g_trace.size(); ++i) {
            if (g_trace[i].entered < g_trace[i - 1].exited) barrier_held = false;
        }
        check(barrier_held,
              "B1 (014 §2): no round's executor ENTERED before the previous round's executor "
              "EXITED -- the superstep barrier holds, measured on timestamps rather than inferred "
              "from correct output, which a fully serialized AND a fully overlapping implementation "
              "would both produce here");

        h.engine.stop();
    }

    // ---- B2: fan-out genuinely overlaps ------------------------------------------------------------
    // Three nodes in ONE round, each sleeping 60 ms. Serialized that is >= 180 ms; overlapped, ~60 ms.
    // The threshold is deliberately loose -- this asserts a DESIGN property, and an exact bound would
    // be measuring the dev box.
    {
        g_trace.clear();
        Harness h(*config, 4);
        h.add_node("src", tracing_body("src", std::chrono::milliseconds(0)));
        for (char const* n : {"w1", "w2", "w3"}) {
            h.add_node(n, tracing_body(n, std::chrono::milliseconds(60)));
        }

        Workflow wf;
        wf.id = "fanout";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3")};
        wf.edges.push_back(Edge{"src", "w1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w2", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w3", edge_kind::fan_out, {}});
        wf.start = "src";
        wf.output_selection.push_back("w3");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "B2: the fan-out graph validates");

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        // The prerequisite B2 is really testing, asserted directly rather than left implicit in the
        // timing. Sequential keys 1/2/3/4 put three of the four nodes on ONE shard of this same
        // 4-shard engine; `spread_executor_keys` is what makes the timing check below meaningful,
        // so if it ever stops spreading, this fails first and says so.
        {
            std::vector<std::uint32_t> shards;
            std::string                rendered;
            for (auto key : h.keys) {
                auto const shard = h.engine.shard_of(actor_id_of<FunctionExecutor>(key));
                shards.push_back(shard);
                rendered += std::to_string(key) + "->" + std::to_string(shard) + " ";
            }
            note("executor key -> shard", rendered);
            bool distinct = true;
            for (std::size_t i = 0; i < shards.size(); ++i) {
                for (std::size_t j = i + 1; j < shards.size(); ++j) {
                    if (shards[i] == shards[j]) distinct = false;
                }
            }
            check(distinct,
                  "B2 (workflow/placement.hpp): the four nodes occupy four DISTINCT shards. Quark "
                  "places by hash_combine(TypeKey, key), and sequential keys do not spread -- "
                  "1/2/3/4 measured as shards 3/1/1/1, which serializes a round onto one worker "
                  "while producing byte-identical output");
        }
        h.engine.start();
        auto const t0 = std::chrono::steady_clock::now();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        check(r.has_value() && r->status == workflow_status::completed, "B2: the fan-out run completes");
        check(r && r->rounds == 2, "B2: source round + worker round = 2 rounds");
        note("fan-out wall clock (ms)", std::to_string(elapsed.count()));
        check(elapsed < std::chrono::milliseconds(150),
              "B2 (014 §3 Concurrent): three 60 ms nodes in one round finished well inside their "
              "180 ms serial sum -- the round's asks are all ISSUED before any is awaited. A "
              "co_await-per-node dispatch loop would still produce correct output and fail here");

        // Overlap stated directly, rather than inferred from the total: the last worker to START did
        // so before the first one FINISHED.
        std::vector<Span> workers;
        for (auto const& s : g_trace) {
            if (s.executor != "src") workers.push_back(s);
        }
        check(workers.size() == 3, "B2: three worker spans recorded");
        bool overlapped = false;
        if (workers.size() == 3) {
            auto latest_entry  = workers[0].entered;
            auto earliest_exit = workers[0].exited;
            for (auto const& s : workers) {
                if (s.entered > latest_entry) latest_entry = s.entered;
                if (s.exited < earliest_exit) earliest_exit = s.exited;
            }
            overlapped = latest_entry < earliest_exit;
        }
        check(overlapped, "B2: all three worker spans were simultaneously in flight");

        h.engine.stop();
    }

    // ---- B3: 014 §2's bounds actually stop a cycle --------------------------------------------------
    // The case §2 exists for. A two-node cycle has no terminal executor, so ONLY the bound can stop
    // it -- without one this test would hang rather than fail, which is why the graph is cyclic
    // rather than merely long.
    {
        g_trace.clear();
        Harness h(*config, 2);
        for (char const* n : {"ping", "pong"}) {
            h.add_node(n, tracing_body(n, std::chrono::milliseconds(0)));
        }

        Workflow wf;
        wf.id       = "cycle";
        wf.executors = {node_desc("ping"), node_desc("pong")};
        wf.edges.push_back(Edge{"ping", "pong", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"pong", "ping", edge_kind::direct, {}});
        wf.start            = "ping";
        wf.bound.max_rounds = 5;
        check(validate_workflow(wf).has_value(),
              "B3: a cyclic graph with a bound validates (014 §9 Q2)");

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value(), "B3: the cyclic run returns rather than spinning forever");
        if (r) {
            check(r->status == workflow_status::bound_max_rounds,
                  "B3 (014 §2): an endless cycle is stopped by MaxRounds, and the caller is TOLD it "
                  "was bounded rather than handed a result indistinguishable from completion");
            check(r->rounds == 5,
                  "B3: exactly max_rounds rounds ran -- the bound is checked BEFORE a round, so N "
                  "means at most N executed, not N+1 with the last discarded after paying for it");
        }
        h.engine.stop();
    }

    // ---- B4: an executor failure stops the run and is reported ---------------------------------------
    // 014 §6's per-edge policies are Phase D. What Phase B must not do is continue silently.
    {
        g_trace.clear();
        Harness h(*config, 2);
        h.add_node("ok", tracing_body("ok", std::chrono::milliseconds(0)));
        h.add_node("boom", [](Message const&, EffectContext&) -> ae::result<Message> {
            return std::unexpected(
                ae::error{failure_class::contract, "deliberate executor failure", "test.executor_failed"});
        });

        Workflow wf;
        wf.id        = "failing";
        wf.executors = {node_desc("ok"), node_desc("boom")};
        wf.edges.push_back(Edge{"ok", "boom", edge_kind::direct, {}});
        wf.start = "ok";
        wf.output_selection.push_back("boom");
        wf.bound.max_rounds = 8;

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value(),
              "B4: a failing executor still yields a reply -- the supervisor's co_await is not "
              "stranded");
        if (r) {
            check(r->status == workflow_status::executor_failed,
                  "B4: the failure is REPORTED, not swallowed into a `completed` result the caller "
                  "would have no way to distinguish from success");
            check(r->rounds == 2, "B4: the run stopped at the failing round, not before or after it");
        }
        h.engine.stop();
    }

    // ---- B5: an invalid graph is refused at run time too ----------------------------------------------
    // `initialize()` validates; a supervisor handed a bad graph must not run it. Without this, the
    // Phase A validator would be bypassable simply by not calling the builder.
    {
        g_trace.clear();
        Harness h(*config, 1);
        h.add_node("solo", tracing_body("solo", std::chrono::milliseconds(0)));

        Workflow wf;
        wf.id        = "unbounded";
        wf.executors = {node_desc("solo")};
        wf.start     = "solo";
        // Deliberately NO bound -- 014 §2's "an unbounded workflow does not run".

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value() && r->status == workflow_status::invalid && r->rounds == 0,
              "B5 (014 §2): a supervisor holding an unbounded graph refuses to run it at all -- the "
              "Phase A validator is not bypassable by skipping the builder");
        check(h.nodes[0]->invocations() == 0,
              "B5: and no executor was invoked -- refused BEFORE any work, not after");
        h.engine.stop();
    }

    // ---- B6: the supervisor is passivatable, which decision 4 says it MUST be ---------------------------
    // 030 §4/§8 Q4 requires pause_project to .passivate() a workflow-supervising actor, and
    // ActorRef<A>::passivate() static_asserts max_concurrency_of<A>() == 1. The call below therefore
    // would not COMPILE against a Reentrant supervisor -- so this case is first a compile-time proof
    // that the constraint holds, and second a runtime census that it works.
    {
        g_trace.clear();
        Harness h(*config, 1);
        h.add_node("solo", tracing_body("solo", std::chrono::milliseconds(0)));

        Workflow wf;
        wf.id        = "solo";
        wf.executors = {node_desc("solo")};
        wf.start     = "solo";
        wf.output_selection.push_back("solo");
        wf.bound.max_rounds = 2;

        ActorRef<WorkflowSupervisor> sup = h.install(wf);
        h.engine.start();
        quark::result<WorkflowResult> r =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("in")}));
        check(r.has_value() && r->status == workflow_status::completed,
              "B6: the single-node workflow runs");

        check(sup.passivate(),
              "B6 (breakdown decision 4): .passivate() COMPILES against WorkflowSupervisor and is "
              "accepted -- the Sequential constraint 030 §7 G1 depends on holds. A Reentrant "
              "supervisor would have failed this line at compile time, not here");
        check(wait_until([&] { return h.sup_act->went_dormant(); }, std::chrono::seconds(2)),
              "B6: the supervisor's activation reaches Dormant, by census -- the property "
              "pause_project (030 §4) will measure across a whole Project");

        quark::result<WorkflowResult> again =
            block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("again")}));
        check(again.has_value() && again->status == workflow_status::completed,
              "B6: a message to the Dormant supervisor reactivates it and the graph runs again -- "
              "paused, not lost");
        h.engine.stop();
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_superstep: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_superstep: %d FAILURE(S)\n", g_failures);
    return 1;
}
