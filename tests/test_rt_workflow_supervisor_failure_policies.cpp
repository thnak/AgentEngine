// Proof for ADR-037 Phase 3, Slice 1: agentengine::rt::WorkflowSupervisor's bound/failure-policy
// behaviour (include/agentengine/rt/workflow_supervisor.hpp), ported from the two retired
// Quark-actor-based test files whose own claims had no equivalent against the rt:: replacement yet --
// test_workflow_superstep.cpp (B3-B5) and test_workflow_failure.cpp (D1-D4). test_rt_workflow_supervisor
// .cpp already proves C1-C5 (linear convergence, the superstep barrier, real fan-out concurrency, a
// THROWING body retried and recovered, request-port suspend/resume) -- this file does not re-prove any
// of that.
//
//   B3 -- a cyclic graph is stopped by graph.bound.max_rounds, reporting bound_max_rounds specifically
//         (not bound_deadline, not silently running forever).
//   B4 -- an executor failure under the DEFAULT (fail) edge policy stops the run, reports
//         executor_failed, and the round count is correct (the failing round IS counted).
//   B5 -- an invalid/unexecutable graph is refused at run_workflow() time with `invalid`, with ZERO
//         executor invocations, proven with a real counter rather than inferred from the status alone.
//   D1 -- `fail` preserves partial results from BOTH an earlier round and same-round sibling work that
//         already succeeded.
//   D2 -- `retry` recovers a body that RETURNS an error (agentengine::result's std::unexpected path --
//         the channel test_rt_workflow_supervisor.cpp's own C4 does NOT cover, since C4 only proves the
//         THROWING channel), bounded by EdgeFailurePolicy::attempts.
//   D2b -- retry exhausted resolves to executor_failed, same as a bare `fail` policy would.
//   D2c -- `contract`/`policy`-classified failures are NEVER retried even with `retry` declared and
//          budget available -- is_retryable()'s own real behaviour (a retry of a denial is an I2
//          hazard the project takes seriously), proven by an invocation count of exactly 1.
//   D3 -- `propagate`: the run continues, and the target receives an attributable failure marker
//         Message (failure_marker()) naming the failed executor and its class.
//   D4 -- `fallback`: the NAMED recovery executor runs with the failure marker, while the failing
//         executor's own normal (non-fallback) target does NOT run.
//
// NOT ported here (see the task this file was written against for the full reasoning):
//   * B0 (spread_executor_keys) -- no shard placement exists in rt::.
//   * B6 (.passivate()/activation census) -- no actor activations exist in rt::.
//   * D0 (the graph validator's own rule table) -- unchanged, shared workflow/graph.hpp code, out of
//     scope here (test_workflow_graph_validation.cpp already covers it and is untouched).
//   * D5 (Quark actor-restart-budget specifics) / D7 (restart budget clamping the workflow retry
//     budget) -- rt:: has no restart-budget layer at all (workflow_supervisor.hpp's own file banner
//     explains why: ExecutorBody is a plain std::function with no per-executor actor state a restart
//     would be recovering from); the throwing-body path itself is C4's job, already proven.
//   * D6 (both failure channels reach the same declared policy) -- would be a byte-for-byte repeat of
//     D4 with a throwing body substituted for a returning one; C4 already proves the throwing channel
//     is classified `transient` and retried, and D4 below already proves `fallback` routing -- their
//     conjunction is not a new claim about THIS file's own code path.
//
// MACHINE SAFETY (CLAUDE.md): bounded round counts, no sleeps in this file.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
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

// Safe here for the same reason test_rt_workflow_supervisor.cpp's own drive<T>() is: every entry
// point's only suspension points are run_mutex_'s uncontended fast path and a nested co_await
// execute(), whose own body never co_awaits anything else -- concurrent fan-out happens through
// std::future::get(), an ordinary blocking call, so the whole chain resolves on one resume().
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Error;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::error;
using agentengine::failure_class;
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

// Renders EVERY content item: Text verbatim, Error as "!<message>". A propagate/fallback target
// receives a failure MARKER Message (failure_marker(), workflow_supervisor.hpp), so a downstream node
// that only read Text would never see it arrive -- this is what lets D3/D4 below check the marker's
// content rather than merely its absence of a crash.
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

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}

// A body that appends its own name to the rendered input (all_text_of, so an incoming failure marker
// is visible in the result too) and, if given a counter, increments it -- so a test can prove exactly
// how many times it ran rather than inferring invocation count from the final status alone.
[[nodiscard]] ExecutorBody appender(std::string name, std::shared_ptr<int> calls = nullptr) {
    return [name = std::move(name), calls](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (calls) ++*calls;
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

// A body that RETURNS an error (agentengine::result<...>'s std::unexpected path -- the channel C4 in
// test_rt_workflow_supervisor.cpp does NOT exercise, since C4 only proves the THROWING channel) for
// its first `fail_first_n` invocations, then succeeds. `calls` counts every invocation, failed or not.
[[nodiscard]] ExecutorBody failing(std::string name, failure_class klass, std::uint32_t fail_first_n,
                                    std::shared_ptr<int> calls) {
    auto seen = std::make_shared<std::uint32_t>(0);
    return [name = std::move(name), klass, fail_first_n, seen, calls](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (calls) ++*calls;
        if ((*seen)++ < fail_first_n) {
            return std::unexpected(error{klass, "injected failure in '" + name + "'", "test.injected"});
        }
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
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

// The partial-result entry for one executor, or nullptr.
[[nodiscard]] agentengine::rt::ExecutorOutput const* partial_for(WorkflowResult const& r, char const* id) {
    for (auto const& out : r.partial) {
        if (out.executor_id == id) return &out;
    }
    return nullptr;
}

}  // namespace

int main() {
    // ---- B3: a cyclic graph is stopped by max_rounds, reported as bound_max_rounds specifically ----
    // A two-node cycle has no terminal executor, so ONLY the bound can stop it -- without one this
    // test would hang rather than fail, which is why the graph is cyclic rather than merely long.
    {
        Workflow wf;
        wf.id        = "cycle";
        wf.executors = {node_desc("ping"), node_desc("pong")};
        wf.edges.push_back(Edge{"ping", "pong", edge_kind::direct, {}, {}});
        wf.edges.push_back(Edge{"pong", "ping", edge_kind::direct, {}, {}});
        wf.start             = "ping";
        wf.bound.max_rounds  = 5;
        check(validate_workflow(wf).has_value(),
              "B3: a cyclic graph with a bound validates (014 §9 Q2 allows cycles)");

        std::vector<ExecutorBody> bodies = {appender("ping"), appender("pong")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::bound_max_rounds,
              "B3 (014 §2): an endless cycle is stopped by max_rounds and reported as "
              "bound_max_rounds SPECIFICALLY -- not bound_deadline, not a result indistinguishable "
              "from a normal completion");
        check(r.rounds == 5,
              "B3: exactly max_rounds rounds ran -- the bound is checked BEFORE a round, so N means "
              "at most N executed, not N+1 with the last discarded after paying for it");
    }

    // ---- B4: an executor failure under the DEFAULT (fail) policy stops the run and is reported -----
    {
        Workflow wf;
        wf.id        = "failing-default";
        wf.executors = {node_desc("ok"), node_desc("boom")};
        wf.edges.push_back(Edge{"ok", "boom", edge_kind::direct, {}, {}});
        wf.start             = "ok";
        wf.output_selection.push_back("boom");
        wf.bound.max_rounds  = 8;
        check(validate_workflow(wf).has_value(), "B4: the graph validates");

        std::vector<ExecutorBody> bodies = {
            appender("ok"),
            [](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return std::unexpected(
                    error{failure_class::contract, "deliberate executor failure", "test.executor_failed"});
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed,
              "B4: a failure under the DEFAULT (fail) edge policy stops the run and is REPORTED, not "
              "swallowed into a `completed` result the caller would have no way to distinguish from "
              "success");
        check(r.failed_executor == "boom", "B4: the result NAMES the executor that ended the run (I4)");
        check(r.rounds == 2,
              "B4: the run stopped at the failing round, not before or after it -- the round that "
              "failed IS counted, no phantom extra round");
    }

    // ---- B5: an invalid/unexecutable graph is refused at run_workflow() time, with ZERO invocations -
    {
        Workflow wf;
        wf.id        = "unbounded";
        wf.executors = {node_desc("solo")};
        wf.start     = "solo";
        // Deliberately NO bound -- 014 §2's "an unbounded workflow does not run".
        check(!validate_workflow(wf).has_value(),
              "B5 setup: the graph fails validate_workflow() (no termination bound) -- the case "
              "run_workflow() itself must also refuse");

        auto calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {appender("solo", calls)};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::invalid,
              "B5 (014 §2): a supervisor holding an unbounded graph refuses to run it at all -- the "
              "Phase A validator is not bypassable by skipping the builder");
        check(r.rounds == 0, "B5: no round ran");
        check(*calls == 0,
              "B5: and no executor was invoked -- refused BEFORE any work, proven with a real "
              "invocation counter rather than merely inferred from the status");
    }

    // ---- D1: `fail` preserves partial results from BOTH an earlier round and a same-round sibling --
    {
        Workflow wf;
        wf.id        = "fail-preserves";
        wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("sink")};
        wf.edges     = {
            Edge{"src", "w1", edge_kind::fan_out, {}, {}},
            Edge{"src", "w2", edge_kind::fan_out, {}, {}},
            Edge{"w1", "sink", edge_kind::direct, {}, {}},
            Edge{"w2", "sink", edge_kind::direct, {}, {}},
        };
        wf.start             = "src";
        wf.bound.max_rounds  = 8;
        check(validate_workflow(wf).has_value(), "D1: the graph validates");

        auto w2_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            appender("src"),
            appender("w1"),
            failing("w2", failure_class::fatal, 999, w2_calls),
            appender("sink"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed,
              "D1: a `fail` policy (the default) stops the workflow on executor failure");
        check(r.failed_executor == "w2", "D1: the result names the executor that ended the run");

        auto const* p_src = partial_for(r, "src");
        auto const* p_w1  = partial_for(r, "w1");
        auto const* p_w2  = partial_for(r, "w2");
        check(p_src != nullptr && all_text_of(p_src->payload) == "in>src",
              "D1: an output completed in an EARLIER round survives the failure");
        check(p_w1 != nullptr && all_text_of(p_w1->payload) == "in>src>w1",
              "D1: an output completed in the SAME round as the failure survives it too -- 014 §6's "
              "'not discarded' covers sibling work, not just prior rounds");
        check(p_w2 == nullptr, "D1: the failed executor itself contributes no partial result");
        check(p_src != nullptr && p_src->round == 0 && p_w1 != nullptr && p_w1->round == 1,
              "D1: each partial result records the round it was actually produced in");
        note("partial results", std::to_string(r.partial.size()) + " entries");
    }

    // ---- D2: `retry` recovers a body that RETURNS an error, bounded by attempts ---------------------
    {
        Workflow wf;
        wf.id        = "retry-recovers";
        wf.executors = {node_desc("flaky"), node_desc("sink")};
        wf.edges     = {Edge{"flaky", "sink", edge_kind::direct, {}, retry_policy(3)}};
        wf.start             = "flaky";
        wf.output_selection  = {"sink"};
        wf.bound.max_rounds  = 8;
        check(validate_workflow(wf).has_value(), "D2: the retry graph validates");

        auto calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            failing("flaky", failure_class::transient, 2, calls),  // fails twice, then works
            appender("sink"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed && all_text_of(r.output) == "in>flaky>sink",
              "D2: a body that RETURNS an error (the std::unexpected path -- C4 only proves the "
              "THROWING path) recovers within its retry budget and the workflow completes");
        check(*calls == 3, "D2: the body was invoked exactly 3 times (1 + 2 retries)");
        check(r.rounds == 2,
              "D2: retries happen INSIDE the round -- 2 rounds ran, not one round per attempt");
    }

    // ---- D2b: retry EXHAUSTED resolves to executor_failed, same as a bare `fail` would -------------
    {
        Workflow wf;
        wf.id        = "retry-exhausted";
        wf.executors = {node_desc("flaky"), node_desc("sink")};
        wf.edges     = {Edge{"flaky", "sink", edge_kind::direct, {}, retry_policy(2)}};
        wf.start             = "flaky";
        wf.bound.max_rounds  = 8;

        auto flaky_calls = std::make_shared<int>(0);
        auto sink_calls  = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            failing("flaky", failure_class::transient, 999, flaky_calls),
            appender("sink", sink_calls),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed && r.failed_executor == "flaky",
              "D2b: an exhausted retry budget resolves to executor_failed -- the same terminal status "
              "a bare `fail` policy would report");
        check(*flaky_calls == 3, "D2b: the budget is a BOUND -- exactly 1 + 2 invocations, then it stops");
        check(*sink_calls == 0, "D2b: the failed node's normal edge never fired, so sink never ran");
    }

    // ---- D2c: `contract`/`policy` classified failures are NEVER retried, even with `retry` declared -
    // The positive control is D2 above, where the same policy retried a `transient` failure.
    {
        Workflow wf;
        wf.id        = "no-retry-on-contract";
        wf.executors = {node_desc("bad"), node_desc("sink")};
        wf.edges     = {Edge{"bad", "sink", edge_kind::direct, {}, retry_policy(3)}};
        wf.start             = "bad";
        wf.bound.max_rounds  = 8;

        auto calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            failing("bad", failure_class::contract, 999, calls),
            appender("sink"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed,
              "D2c: a `contract` failure under a retry policy still fails the workflow");
        check(*calls == 1,
              "D2c: is_retryable()'s own classification does real work -- a `contract` failure is "
              "invoked ONCE despite a budget of 3, never retried even with budget available (an "
              "unretried contract violation, not a retry that silently does nothing)");
    }
    {
        Workflow wf;
        wf.id        = "no-retry-on-policy";
        wf.executors = {node_desc("denied"), node_desc("sink")};
        wf.edges     = {Edge{"denied", "sink", edge_kind::direct, {}, retry_policy(5)}};
        wf.start             = "denied";
        wf.bound.max_rounds  = 8;

        auto calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            failing("denied", failure_class::policy, 999, calls),
            appender("sink"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed && *calls == 1,
              "D2c: a `policy` failure is a DENIAL and is never retried -- re-asking a denied request "
              "until the answer changes is an I2 hazard, not an ordinary retry (is_retryable()'s own "
              "comment in workflow_supervisor.hpp names exactly this)");
    }

    // ---- D3: `propagate` -- the run continues, and the target sees an attributable failure marker --
    {
        Workflow wf;
        wf.id        = "propagate";
        wf.executors = {node_desc("risky"), node_desc("handler")};
        wf.edges     = {Edge{"risky", "handler", edge_kind::direct, {}, propagate_policy()}};
        wf.start             = "risky";
        wf.output_selection  = {"handler"};
        wf.bound.max_rounds  = 8;
        check(validate_workflow(wf).has_value(), "D3: the propagate graph validates");

        auto handler_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return std::unexpected(
                    error{failure_class::fatal, "injected failure in 'risky'", "test.injected"});
            },
            appender("handler", handler_calls),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "D3: under `propagate` an executor failure does NOT take the workflow down -- the run "
              "keeps going and execution proceeds normally downstream");
        check(*handler_calls == 1, "D3: the propagate target ran exactly once");
        check(all_text_of(r.output).find("!executor 'risky' failed (fatal)") != std::string::npos,
              "D3: the target received an attributable failure marker Message (failure_marker(), an "
              "Error content item naming the failed executor and its class), not the failed node's "
              "absent output");
        note("propagated", all_text_of(r.output));
    }

    // ---- D4: `fallback` -- the NAMED recovery executor runs; the normal target does NOT ------------
    {
        Workflow wf;
        wf.id        = "fallback";
        wf.executors = {node_desc("risky"), node_desc("normal"), node_desc("recovery")};
        wf.edges     = {Edge{"risky", "normal", edge_kind::direct, {}, fallback_policy("recovery")}};
        wf.start             = "risky";
        wf.output_selection  = {"normal", "recovery"};
        wf.bound.max_rounds  = 8;
        check(validate_workflow(wf).has_value(), "D4: the fallback graph validates");

        auto normal_calls   = std::make_shared<int>(0);
        auto recovery_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            failing("risky", failure_class::resource, 999, std::make_shared<int>(0)),
            appender("normal", normal_calls),
            appender("recovery", recovery_calls),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "D4: under `fallback` the workflow completes through the named recovery branch");
        check(*recovery_calls == 1, "D4: the recovery executor ran");
        check(*normal_calls == 0,
              "D4: the failing executor's own NORMAL (non-fallback) target does NOT run -- a failed "
              "executor produces no output to carry along its ordinary edges");
        note("recovered", all_text_of(r.output));

        // Positive control on the SAME graph: no failure, no recovery.
        auto normal_calls2   = std::make_shared<int>(0);
        auto recovery_calls2 = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies2 = {
            appender("risky"),
            appender("normal", normal_calls2),
            appender("recovery", recovery_calls2),
        };
        WorkflowSupervisor sup2;
        sup2.initialize(wf, bodies2);
        WorkflowResult r2 = drive(sup2.run_workflow(RunWorkflow{text_message("in")}));
        check(r2.status == workflow_status::completed && *normal_calls2 == 1 && *recovery_calls2 == 0,
              "D4 positive control: with no failure the SAME graph runs the normal target and leaves "
              "the recovery branch cold");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_supervisor_failure_policies: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_supervisor_failure_policies: %d failure(s)\n", g_failures);
    return 1;
}
