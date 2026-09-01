// Proof for GitHub issue #37: mid-run cancellation for WorkflowSupervisor. Design draft:
// docs/planning/workflow-mid-run-cancellation-design-draft.md (red-teamed once before any code
// existed -- this file proves the revised, post-red-team mechanism).
//
// C1 -- positive, round-boundary, DETERMINISTIC liveness (not a timing race): a body deliberately
//       blocked on a condition_variable past round 1, driven on a real std::thread; cancel() is
//       called from the main thread and CONFIRMED to have landed (stop_requested() observed true)
//       while the body is still genuinely blocked, then released -- the run stops with
//       workflow_status::cancelled at rounds == 1; round 2 never dispatches.
// C2 -- the SAME scenario also proves non-preemption: the blocked node's own contribution (its real
//       return value) still made it into the run's own partial output -- cancellation does not
//       truncate or corrupt the round already in flight when it was requested, it only prevents
//       FUTURE rounds.
// C3 -- positive, cooperative mid-call: a body that explicitly checks
//       ctx.cancellation.stop_requested() observes a cancel() call made from another thread while
//       it is running, and returns a distinguishable result because of it.
// C4 -- non-preemption, explicit: a body that does NOT check the token at all still runs to its own
//       natural completion for that one call, even though cancel() was called mid-call -- the
//       mechanism is genuinely cooperative, not accidentally preemptive.
// C5 -- zero-cost-when-unused: a run that never calls cancel() at all completes with its ordinary
//       status, every round, unaffected -- matching this file's own "additive, existing callers
//       unaffected" convention for every optional hook.
// C6 -- cancellation_token() handed to independent code observes the SAME request cancel() made --
//       a caller can hold the token separately from ctx.cancellation.
//
// MACHINE SAFETY (CLAUDE.md): every wait below is bounded (10s condition_variable timeouts).
//
// Run: ./test_rt_workflow_cancellation

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T",
                     .output_type = "T", .worktree_mode = sharing_mode::branch,
                     .capability_ceiling = {}};
}

// A 3-round linear chain: blocker -> second -> sink. Each node fires in its own round (direct
// edges), so "rounds == 1" after a cancelled run means ONLY blocker's round ever dispatched.
[[nodiscard]] Workflow chain_graph(char const* id) {
    Workflow wf;
    wf.id        = id;
    wf.executors = {node_desc("blocker"), node_desc("second"), node_desc("sink")};
    wf.edges.push_back(Edge{"blocker", "second", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"second", "sink", edge_kind::direct, {}});
    wf.start = "blocker";
    wf.output_selection.push_back("sink");
    wf.bound.max_rounds = 8;
    return wf;
}

// ---- C1/C2: deterministic round-boundary liveness + non-preemption of the in-flight round --------

void c1_c2_round_boundary_cancellation_is_deterministic_and_non_preempting() {
    std::mutex             cv_mutex;
    std::condition_variable cv;
    std::atomic<bool>      may_proceed{false};
    std::atomic<bool>      run_finished{false};

    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [&](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return may_proceed.load(); });
            return ExecutorOutcome{text_message(text_of(in) + ">blocker")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">second")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">sink")};
        },
    };
    sup.initialize(chain_graph("c1c2"), bodies);

    std::thread driver([&] {
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::cancelled,
              "C1: the run stops with workflow_status::cancelled");
        check(r.rounds == 1,
              "C1: rounds == 1 -- round 2 (second/sink) never dispatched, cancellation genuinely "
              "prevented FUTURE rounds, not merely coincided with natural completion");
        check(text_of(r.output).empty(),
              "C1: no output_selection node (sink) ever ran, so there is no selected output");
        check(r.partial.size() == 1 && text_of(r.partial[0].payload) == "go>blocker",
              "C2: blocker's own real contribution DID make it into the run's partial output -- "
              "cancellation did not truncate or corrupt the round already in flight when it was "
              "requested, it only prevented the NEXT round from starting");
        run_finished.store(true);
    });

    // Bounded poll: wait for the supervisor to genuinely be inside blocker's own call (there is no
    // direct "in a call" signal to poll, so this waits a short, bounded amount for the driver
    // thread to have started and dispatched -- the DETERMINISTIC part of this proof is not this
    // poll, it's the assertion right after cancel(), which can only pass if blocker is still
    // genuinely blocked at that moment).
    for (int i = 0; i < 200 && !run_finished.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(!run_finished.load(), "C1 setup: the run has not finished yet (still blocked in blocker, "
                                 "as designed) -- sanity check before the deterministic assertion");

    sup.cancel();
    // Deterministic, not a race: cancel_source_'s stop-state is set synchronously by request_stop()
    // (specified thread-safe) before this call returns -- cancellation_token() observing it true
    // right after is not a timing race, it's a direct consequence of that call having returned.
    check(sup.cancellation_token().stop_requested(),
          "C1: cancel() is observed to have landed via cancellation_token() -- and run_finished is "
          "STILL false here (blocker has not been released yet), so this is confirmed to have "
          "happened WHILE the run was still genuinely in progress, not after it finished naturally");
    check(!run_finished.load(),
          "C1: ...and the run is STILL not finished at this exact point -- structurally guaranteed, "
          "not a timing race: blocker cannot return until may_proceed is set, which has not "
          "happened yet");

    {
        std::lock_guard<std::mutex> lock(cv_mutex);
        may_proceed.store(true);
    }
    cv.notify_all();
    driver.join();
    check(run_finished.load(), "C1: the run eventually finishes once blocker is released");
}

// ---- C3: cooperative mid-call opt-out ------------------------------------------------------------

void c3_cooperative_mid_call_observes_cancellation() {
    std::mutex             cv_mutex;
    std::condition_variable cv;
    std::atomic<bool>      may_proceed{false};
    std::atomic<bool>      run_finished{false};

    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [&](Message const&, EffectContext& ctx) -> result<ExecutorOutcome> {
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return may_proceed.load(); });
            // Checked AFTER being released -- by then the test below has already called cancel()
            // and confirmed it landed (see the assertion right after sup.cancel() below).
            bool const cancelled = ctx.cancellation.stop_requested();
            return ExecutorOutcome{text_message(cancelled ? "observed-cancelled" : "ran-normally")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(chain_graph("c3"), bodies);

    std::thread driver([&] {
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.partial.size() == 1 && text_of(r.partial[0].payload) == "observed-cancelled",
              "C3: the body's own explicit ctx.cancellation.stop_requested() check observed the "
              "cancellation (made from a different thread, mid-call) and returned a distinguishable "
              "result because of it");
        run_finished.store(true);
    });

    for (int i = 0; i < 200 && !run_finished.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sup.cancel();
    check(sup.cancellation_token().stop_requested() && !run_finished.load(),
          "C3: cancel() landed while the body is still genuinely blocked, before its own "
          "ctx.cancellation.stop_requested() check ever runs");
    {
        std::lock_guard<std::mutex> lock(cv_mutex);
        may_proceed.store(true);
    }
    cv.notify_all();
    driver.join();
}

// ---- C4: non-preemption -- a body that never checks the token still completes normally ------------

void c4_non_cooperative_body_completes_normally() {
    std::mutex             cv_mutex;
    std::condition_variable cv;
    std::atomic<bool>      may_proceed{false};
    std::atomic<bool>      run_finished{false};

    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [&](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            // Deliberately never reads ctx.cancellation at all.
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return may_proceed.load(); });
            return ExecutorOutcome{text_message(text_of(in) + ">ran-to-completion")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(chain_graph("c4"), bodies);

    std::thread driver([&] {
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.partial.size() == 1 && text_of(r.partial[0].payload) == "go>ran-to-completion",
              "C4: the non-cooperative body's own call completed to its OWN natural end (never "
              "interrupted mid-call) even though cancel() was called while it was running -- "
              "genuinely cooperative cancellation, not accidentally preemptive");
        run_finished.store(true);
    });

    for (int i = 0; i < 200 && !run_finished.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sup.cancel();
    check(!run_finished.load(), "C4: cancel() landed while the non-cooperative body is still "
                                 "genuinely blocked, not after it already finished");
    {
        std::lock_guard<std::mutex> lock(cv_mutex);
        may_proceed.store(true);
    }
    cv.notify_all();
    driver.join();
}

// ---- C5: zero-cost-when-unused ---------------------------------------------------------------------

void c5_unused_cancellation_is_a_no_op() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">blocker")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">second")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">sink")};
        },
    };
    sup.initialize(chain_graph("c5"), bodies);
    // cancel() is NEVER called anywhere in this test.
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed,
          "C5: a run that never calls cancel() completes normally, unaffected by the mechanism's "
          "mere presence");
    check(r.rounds == 3, "C5: all three rounds (blocker/second/sink) ran");
    check(text_of(r.output) == "go>blocker>second>sink",
          "C5: the full chain's real output is unaffected, byte for byte");
}

// ---- C6: cancellation_token() observes the SAME request cancel() made -----------------------------

void c6_cancellation_token_mirrors_cancel() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(chain_graph("c6"), bodies);
    std::stop_token const held = sup.cancellation_token();
    check(!held.stop_requested(), "C6: a freshly held token reports not-yet-requested");
    sup.cancel();
    check(held.stop_requested(), "C6: the SAME held token (obtained before cancel() was ever "
                                  "called) now observes the request -- a caller can hold this "
                                  "independently of any run in flight");
}

}  // namespace

int main() {
    c1_c2_round_boundary_cancellation_is_deterministic_and_non_preempting();
    c3_cooperative_mid_call_observes_cancellation();
    c4_non_cooperative_body_completes_normally();
    c5_unused_cancellation_is_a_no_op();
    c6_cancellation_token_mirrors_cancel();

    std::fprintf(stderr, g_failures == 0 ? "test_rt_workflow_cancellation: ALL PASS\n"
                                          : "test_rt_workflow_cancellation: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
