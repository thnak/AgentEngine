// Proof for ADR-037 Phase 3, Slice 1: agentengine::rt::WorkflowSupervisor's core superstep loop
// (include/agentengine/rt/workflow_supervisor.hpp), the Quark-actor-free replacement for
// agentengine::workflow::WorkflowSupervisor -- driven through agentengine::rt::task<T>/
// agentengine::rt::AsyncMutex/agentengine::rt::ThreadPool instead of quark::Actor<Sequential>/
// quark::ActorRef<FunctionExecutor>::ask<>. Deterministic where possible; two checks (C2/C3) are real
// wall-clock measurements, mirroring the Quark-based test_workflow_superstep.cpp's own B1/B2 -- the
// two claims this migration's whole design rests on (the superstep barrier still holds; fan-out is
// still GENUINELY concurrent now that it runs through rt::ThreadPool instead of separate Quark
// actors). MACHINE SAFETY (CLAUDE.md): bounded round counts, sleeps in tens of ms.
//   C1 -- a linear chain converges, payload threads through every node in graph order.
//   C2 -- the superstep barrier: no round-(n+1) entry precedes any round-n exit (mirrors B1).
//   C3 -- fan-out genuinely overlaps: three 60ms-sleeping siblings in one round finish in ~60ms wall-
//         clock, not ~180ms serialized (mirrors B2) -- the one measurement that proves rt::ThreadPool
//         is actually providing real concurrency, not just interleaved single-threaded async.
//   C4 -- a throwing executor body is contained (JobOutcome::faulted), classified transient, and the
//         existing retry policy (014 Sec6) retries it -- proving the fault-isolation redesign (no
//         actor-restart budget needed) actually works end to end, not just in the file banner's prose.
//   C5 -- a request-port suspend/resume round-trip.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ExecutorBody;
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

// Safe here: run_workflow()/resume_workflow()/continue_workflow()'s only suspension points are
// run_mutex_'s uncontended fast path (never genuinely parks in this single-caller test file) and a
// nested co_await execute(), whose OWN body never co_awaits anything either -- concurrent fan-out
// happens through std::future::get() (an ordinary blocking call, not a coroutine suspension), so the
// whole chain resolves on ONE resume() call. See workflow_supervisor.hpp's own file banner.
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
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::EdgeFailurePolicy;
using agentengine::workflow::edge_failure_policy;
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

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}

// --- C2's superstep-barrier trace ------------------------------------------------------------------

struct Span {
    std::string executor;
    std::chrono::steady_clock::time_point entered;
    std::chrono::steady_clock::time_point exited;
};
std::mutex        g_trace_mutex;
std::vector<Span> g_trace;
void record(Span s) {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace.push_back(std::move(s));
}

[[nodiscard]] ExecutorBody tracing_body(std::string name, std::chrono::milliseconds work) {
    return [name = std::move(name), work](Message const& in,
                                          agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        Span span;
        span.executor = name;
        span.entered  = std::chrono::steady_clock::now();
        if (work.count() > 0) std::this_thread::sleep_for(work);
        span.exited = std::chrono::steady_clock::now();
        record(span);
        return ExecutorOutcome{text_message(text_of(in) + ">" + name)};
    };
}

}  // namespace

int main() {
    // ---- C1: a linear chain converges, payload threads through every node ----------------------
    {
        Workflow wf;
        wf.id        = "chain";
        wf.executors = {node_desc("a"), node_desc("b"), node_desc("c")};
        wf.edges.push_back(Edge{"a", "b", edge_kind::chain, {}});
        wf.edges.push_back(Edge{"b", "c", edge_kind::chain, {}});
        wf.start = "a";
        wf.output_selection.push_back("c");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "C1: the chain graph validates");

        std::vector<ExecutorBody> bodies = {
            tracing_body("a", std::chrono::milliseconds(0)),
            tracing_body("b", std::chrono::milliseconds(0)),
            tracing_body("c", std::chrono::milliseconds(0)),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "C1: a chain terminates by running dry at its terminal executor, not by hitting its bound");
        check(r.rounds == 3, "C1: exactly one round per chain link");
        check(text_of(r.output) == "in>a>b>c", "C1: the payload threaded through every node in graph order");
        note("output", text_of(r.output));
    }

    // ---- C2: the superstep barrier, measured (mirrors the Quark test's own B1) -------------------
    {
        g_trace.clear();
        Workflow wf;
        wf.id        = "chain-timed";
        wf.executors = {node_desc("a"), node_desc("b"), node_desc("c")};
        wf.edges.push_back(Edge{"a", "b", edge_kind::chain, {}});
        wf.edges.push_back(Edge{"b", "c", edge_kind::chain, {}});
        wf.start = "a";
        wf.output_selection.push_back("c");
        wf.bound.max_rounds = 8;

        std::vector<ExecutorBody> bodies = {
            tracing_body("a", std::chrono::milliseconds(30)),
            tracing_body("b", std::chrono::milliseconds(30)),
            tracing_body("c", std::chrono::milliseconds(30)),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed, "C2 setup: the run completes");
        check(g_trace.size() == 3, "C2: three spans recorded, one per round");
        bool barrier_held = g_trace.size() == 3;
        for (std::size_t i = 1; i < g_trace.size(); ++i) {
            if (g_trace[i].entered < g_trace[i - 1].exited) barrier_held = false;
        }
        check(barrier_held,
              "C2 (014 Sec2): no round's executor ENTERED before the previous round's executor EXITED "
              "-- the superstep barrier holds, measured on timestamps rather than inferred from "
              "correct output");
    }

    // ---- C3: fan-out genuinely overlaps (mirrors the Quark test's own B2) ------------------------
    // Three nodes in ONE round, each sleeping 60ms. Serialized that is >= 180ms; overlapped, ~60ms.
    // Deliberately loose threshold -- asserting a design property, not measuring the dev box.
    {
        g_trace.clear();
        Workflow wf;
        wf.id        = "fanout";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3")};
        wf.edges.push_back(Edge{"src", "w1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w2", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src", "w3", edge_kind::fan_out, {}});
        wf.start = "src";
        wf.output_selection.push_back("w3");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "C3: the fan-out graph validates");

        std::vector<ExecutorBody> bodies = {
            tracing_body("src", std::chrono::milliseconds(0)),
            tracing_body("w1", std::chrono::milliseconds(60)),
            tracing_body("w2", std::chrono::milliseconds(60)),
            tracing_body("w3", std::chrono::milliseconds(60)),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        auto const t0 = std::chrono::steady_clock::now();
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        auto const elapsed = std::chrono::steady_clock::now() - t0;
        check(r.status == workflow_status::completed, "C3 setup: the run completes");
        check(r.rounds == 2, "C3: src's round, then w1/w2/w3's shared round");
        note("elapsed_ms",
             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        check(elapsed < std::chrono::milliseconds(150),
              "C3: three 60ms siblings in one round finish in well under their serialized sum "
              "(180ms) -- rt::ThreadPool is providing REAL concurrency, the one property this "
              "migration's whole fan-out redesign rests on");
    }

    // ---- C4: a throwing executor body is contained and retried --------------------------------
    // policy_for() reads the policy declared on the edge OUT OF the failing executor (see
    // workflow_supervisor.hpp's own comment) -- so "flaky" needs a real outgoing edge (to "sink") to
    // carry the retry policy; it cannot be the terminal node itself.
    {
        Workflow wf;
        wf.id        = "flaky";
        wf.executors = {node_desc("start"), node_desc("flaky"), node_desc("sink")};
        wf.edges.push_back(Edge{"start", "flaky", edge_kind::direct, {}});
        Edge flaky_edge{"flaky", "sink", edge_kind::direct, {}};
        flaky_edge.on_failure = EdgeFailurePolicy{edge_failure_policy::retry, 3, ""};
        wf.edges.push_back(flaky_edge);
        wf.start = "start";
        wf.output_selection.push_back("sink");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "C4: the retry graph validates");

        auto call_count = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
            [call_count](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                ++*call_count;
                if (*call_count < 2) throw std::runtime_error("flaky failure");
                return ExecutorOutcome{text_message("recovered")};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "C4: a throwing body that succeeds on retry converges rather than ending the workflow");
        check(*call_count == 2, "C4: the body was actually invoked twice -- once faulted, once retried");
        check(text_of(r.output) == "recovered",
              "C4: the workflow's output is the retried call's real result");
    }

    // ---- C5: a request-port suspend/resume round-trip ------------------------------------------
    {
        Workflow wf;
        wf.id        = "port";
        wf.executors = {node_desc("start"), Executor{"ask", executor_kind::request_port, "T", "T"},
                        node_desc("finish")};
        wf.edges.push_back(Edge{"start", "ask", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"ask", "finish", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("finish");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "C5: the request-port graph validates");

        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
            {},  // request_port: never invoked -- reaching it IS the event
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">finish")};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "C5: reaching the port suspends the run");
        check(r1.open_interactions.size() == 1, "C5: exactly one open Interaction for the one port");

        if (!r1.open_interactions.empty()) {
            ResumeWorkflow resume{};
            resume.interaction_id = r1.open_interactions.front().interaction_id;
            resume.response       = text_message("in>ask");
            WorkflowResult r2 = drive(sup.resume_workflow(resume));
            check(r2.status == workflow_status::completed, "C5: resolving the port lets the run finish");
            check(text_of(r2.output) == "in>ask>finish",
                  "C5: the port's response routed through to the terminal executor");
        }
    }

    // ---- P1: to_record()/restore_from_record() round-trips a completed run's position/output ----
    {
        Workflow wf;
        wf.id        = "chkpt";
        wf.executors = {node_desc("start"), node_desc("mid"), node_desc("end")};
        wf.edges.push_back(Edge{"start", "mid", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"mid", "end", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("end");
        wf.bound.max_rounds = 8;

        auto passthrough = [](Message const& in,
                              agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{in};
        };
        std::vector<ExecutorBody> bodies = {
            passthrough,
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">mid")};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">end")};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed, "P1 setup: the run completes");

        agentengine::rt::RunStateRecord rec = sup.to_record();
        check(rec.run_id == sup.run_id(), "P1: to_record() carries the real run_id");
        check(rec.rounds == 3, "P1: to_record() carries run position");
        check(text_of(rec.selected_output) == "in>mid>end",
              "P1: to_record() carries the real selected_output Message payload");

        WorkflowSupervisor restored;
        restored.initialize(wf, bodies);
        restored.restore_from_record(rec);
        check(restored.run_id() == rec.run_id, "P1: restore_from_record() restores run_id");
        check(restored.rounds_executed() == 3, "P1: restore_from_record() restores rounds");
    }

    // ---- P2: save_workflow_checkpoint()/load_workflow_checkpoint() round-trip through a real -----
    // ---- SessionStore conformer                                                                ----
    {
        Workflow wf;
        wf.id        = "chkpt-store";
        wf.executors = {node_desc("only")};
        wf.start = "only";
        wf.output_selection.push_back("only");
        wf.bound.max_rounds = 8;
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed, "P2 setup: the run completes");

        agentengine::rt::InMemorySessionStore store;
        auto saved = drive(agentengine::rt::save_workflow_checkpoint(sup, store));
        check(saved.has_value(), "P2: save_workflow_checkpoint() succeeds");

        auto loaded = agentengine::rt::load_workflow_checkpoint(store, sup.run_id());
        check(loaded.has_value() && loaded->has_value(), "P2: load_workflow_checkpoint() finds it");
        if (loaded.has_value() && loaded->has_value()) {
            check((*loaded)->run_id == sup.run_id(), "P2: the loaded record's run_id round-trips");
            check((*loaded)->rounds == r.rounds, "P2: the loaded record's rounds round-trips");
        }
    }

    // ---- P3: loading a run_id that was never saved returns std::nullopt, not an error ------------
    {
        agentengine::rt::InMemorySessionStore store;
        auto loaded = agentengine::rt::load_workflow_checkpoint(store, "never-existed");
        check(loaded.has_value() && !loaded->has_value(),
              "P3: an id that was never saved reports std::nullopt, not a failure");
    }

    // ---- P4: a RESTORED instance can actually resolve a real resume -- proof restore_from_record() --
    // ---- reconstructs genuinely resolvable state (including open ports), not just similar fields ---
    {
        Workflow wf;
        wf.id        = "chkpt-port";
        wf.executors = {node_desc("start"), Executor{"ask", executor_kind::request_port, "T", "T"},
                        node_desc("finish")};
        wf.edges.push_back(Edge{"start", "ask", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"ask", "finish", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("finish");
        wf.bound.max_rounds = 8;

        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
            {},
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">finish")};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "P4 setup: the run suspends at the port");

        agentengine::rt::RunStateRecord rec = sup.to_record();
        check(rec.ports.size() == 1, "P4: the record carries the one open port");

        // A fresh instance -- the graph is redeployed by the host from the same description, same
        // contract the Quark original's own checkpoint carried (see file banner).
        WorkflowSupervisor restored;
        restored.initialize(wf, bodies);
        restored.restore_from_record(rec);
        check(restored.open_interactions().size() == 1,
              "P4: the restored instance sees the same open port");

        if (!restored.open_interactions().empty()) {
            ResumeWorkflow resume{};
            resume.interaction_id = restored.open_interactions().front().interaction_id;
            resume.response       = text_message("in>ask");
            WorkflowResult r2 = drive(restored.resume_workflow(resume));
            check(r2.status == workflow_status::completed,
                  "P4: the RESTORED instance completes a real resume -- proof restore_from_record() "
                  "reconstructed genuinely resolvable state, not just cosmetically similar fields");
            check(text_of(r2.output) == "in>ask>finish", "P4: the restored run's output is correct");
        }
    }

    // ---- P5: the in-flight guard, proven with REAL threads (the first place in this migration ----
    // ---- a lock is held across genuine ThreadPool-driven multi-thread work, not just coroutine ----
    // ---- suspension -- see file banner for why AgentSession's own single-threaded proof         ----
    // ---- pattern does not apply here: execute() never suspends internally, it BLOCKS via         ----
    // ---- std::future::get(), so driving run_workflow() with one resume() call occupies its       ----
    // ---- calling OS thread for the run's whole duration.                                         ----
    {
        std::atomic<bool> round_started{false};
        Workflow wf;
        wf.id        = "chkpt-inflight";
        wf.executors = {node_desc("slow")};
        wf.start = "slow";
        wf.output_selection.push_back("slow");
        wf.bound.max_rounds = 8;

        std::vector<ExecutorBody> bodies = {
            [&round_started](Message const& in,
                             agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                round_started.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        agentengine::rt::task<WorkflowResult> run_task = sup.run_workflow(RunWorkflow{text_message("in")});
        std::thread driver([&run_task] { run_task.resume(); });

        while (!round_started.load(std::memory_order_acquire)) std::this_thread::yield();

        agentengine::rt::task<agentengine::rt::RunStateRecord> snap_task = sup.snapshot_record();
        snap_task.resume();
        check(!snap_task.done(),
              "P5: a concurrent snapshot_record() call queues behind an in-flight round -- run_mutex_ "
              "is genuinely held while the round's own ThreadPool job is still physically running on "
              "a separate worker thread, not just while the coroutine machinery is 'logically' busy");

        driver.join();
        check(run_task.done(), "P5: the driven run completed");
        check(snap_task.done(),
              "P5: the queued snapshot_record() was released and completed once the run finished and "
              "released run_mutex_");

        WorkflowResult run_result = run_task.take_value();
        check(run_result.status == workflow_status::completed, "P5: the run itself converged normally");
        agentengine::rt::RunStateRecord rec = snap_task.take_value();
        check(rec.rounds == 1,
              "P5: the snapshot observed POST-run state -- proof the guard prevented a torn read");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_supervisor: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_supervisor: %d failure(s)\n", g_failures);
    return 1;
}
