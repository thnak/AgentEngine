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
// Issue #156 (decisions/ADR-214):
// C7  -- a cancel while a REAL AgentSession step waits on its model ends the run `cancelled` (the step's
//        own `run.canceled` used to make it `executor_failed` under the default `fail` policy).
// C8  -- a cancel while SUSPENDED closes the open interaction at once and reports the run ending
//        (`workflow_run_failed{cancelled}`); a later resume is refused (`request_port_rejected`, reason
//        cancelled) and continue_workflow() runs nothing.
// C9  -- cancel is per run: the next run_workflow() on the same supervisor runs normally, and a cancel
//        arriving after a run completed does not poison the next one.
// C10 -- a cancel in the round that reaches a port ends `cancelled` with the port in unopened_ports
//        (it used to end `suspended`).
// C11 -- a function step that fails BECAUSE it observed the cancel ends the run `cancelled`.
// C12 -- cancelling an outer run suspended inside a nested sub-workflow closes the inner run too.
// C13 -- RunWorkflow::cancellation: a linked token stopped before run_workflow() cancels that run.
// C14 -- an outer cancel reaches a nested sub-workflow run while it is in flight.
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

#include <memory>
#include <memory_resource>
#include <optional>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::AgentSession;
using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::agent_session_as_executor_body;
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

// ==== Issue #156 (ADR-214) ==========================================================================

// A gate a body (or a chat client) parks on until the test opens it -- bounded (10s), never forever.
struct Gate {
    std::mutex              m;
    std::condition_variable cv;
    bool                    entered = false;
    bool                    open    = false;

    void park() {
        std::unique_lock<std::mutex> lock(m);
        entered = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::seconds(10), [this] { return open; });
    }
    [[nodiscard]] bool wait_entered() {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::seconds(10), [this] { return entered; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m);
            open = true;
        }
        cv.notify_all();
    }
};

// A chat client whose one call parks on a Gate, so the test can cancel while an agent step is waiting on
// its model -- the exact moment issue #156 reported (it then ended executor_failed).
class GatedChatClient {
public:
    GatedChatClient() : gate_(std::make_shared<Gate>()) {}
    [[nodiscard]] std::shared_ptr<Gate> gate() const { return gate_; }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    rt::task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        gate_->park();
        Message m;
        m.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"draft text"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<Gate> gate_;
};
static_assert(agentengine::ChatClient<GatedChatClient>);

[[nodiscard]] Executor kind_desc(char const* id, executor_kind kind) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] std::vector<WorkflowEvent> drain(WorkflowEventStream& s) {
    std::vector<WorkflowEvent> out;
    while (std::optional<WorkflowEvent> ev = s.next()) out.push_back(std::move(*ev));
    return out;
}

[[nodiscard]] bool has_run_failed(std::vector<WorkflowEvent> const& evs, char const* tag) {
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::workflow_run_failed) continue;
        if (auto const* p = std::get_if<workflow_event_payload::RunFailed>(&e.payload)) {
            if (p->status_tag == tag) return true;
        }
    }
    return false;
}

// ---- C7: cancelling while an AGENT step waits on its model ends `cancelled`, not executor_failed ----
//
// A REAL AgentSession (not a stand-in body): the session sees `ctx.cancellation` through the adapter's
// bridge, ends its own run `run.canceled` (class fatal) once the model call returns, and the workflow's
// default `fail` edge policy used to turn that into `executor_failed`.
void c7_cancel_during_agent_step_ends_cancelled() {
    AgentSession<GatedChatClient> session;
    session.initialize("c7-draft", Principal{"owner", ""});
    std::shared_ptr<Gate> const gate = session.emplace_chat_client().gate();

    Workflow wf;
    wf.id        = "c7";
    wf.executors = {kind_desc("draft", executor_kind::agent), kind_desc("after", executor_kind::function)};
    wf.edges.push_back(Edge{"draft", "after", edge_kind::direct, {}});
    wf.start            = "draft";
    wf.output_selection = {"after"};
    wf.bound.max_rounds = 4;

    auto after_calls = std::make_shared<std::atomic<int>>(0);
    WorkflowSupervisor sup;
    sup.initialize(wf, {agent_session_as_executor_body(session),
                        [after_calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                            after_calls->fetch_add(1);
                            return ExecutorOutcome{in};
                        }});

    WorkflowResult r;
    std::thread driver([&] { r = drive(sup.run_workflow(RunWorkflow{text_message("write")})); });
    check(gate->wait_entered(), "C7 setup: the agent step is waiting on its model");
    sup.cancel();
    gate->release();
    driver.join();

    check(r.status == workflow_status::cancelled,
          "C7 -- CORE CLAIM (#156): a cancel while an agent step waits on its model ends the run "
          "`cancelled` -- the step's own run.canceled is the workflow's cancel, not a step failure");
    check(r.failed_executor.empty(), "C7: no executor is named as failed");
    check(r.rounds == 1 && after_calls->load() == 0, "C7: the next step never ran");
    check(session.last_run_id().empty() == false, "C7: the agent's run really started (a real session ran)");
}

// ---- C11: the function-body analogue -- a step that FAILS because it observed the cancel ----------------
void c11_cancel_observing_failure_is_not_executor_failed() {
    auto gate = std::make_shared<Gate>();
    WorkflowSupervisor sup;
    sup.initialize(chain_graph("c11"),
                   {[gate](Message const&, EffectContext& ctx) -> result<ExecutorOutcome> {
                        gate->park();
                        if (ctx.cancellation.stop_requested()) {
                            return std::unexpected(error{failure_class::fatal, "the run was canceled", "run.canceled"});
                        }
                        return ExecutorOutcome{text_message("not cancelled")};
                    },
                    [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                    [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }});
    WorkflowResult r;
    std::thread driver([&] { r = drive(sup.run_workflow(RunWorkflow{text_message("go")})); });
    check(gate->wait_entered(), "C11 setup: the step is running");
    sup.cancel();
    gate->release();
    driver.join();
    check(r.status == workflow_status::cancelled && r.failed_executor.empty(),
          "C11 (#156): a step failing BECAUSE of the cancel ends the run cancelled, not executor_failed");
}

// A port graph: start -> port -> sink.
[[nodiscard]] Workflow port_graph(char const* id) {
    Workflow wf;
    wf.id        = id;
    wf.executors = {node_desc("start"), kind_desc("port", executor_kind::request_port), node_desc("sink")};
    wf.edges.push_back(Edge{"start", "port", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"port", "sink", edge_kind::direct, {}});
    wf.start            = "start";
    wf.output_selection = {"sink"};
    wf.bound.max_rounds = 6;
    return wf;
}
[[nodiscard]] std::vector<ExecutorBody> port_bodies() {
    return {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">start")};
            },
            {},
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">sink")};
            }};
}

// ---- C8: cancel while SUSPENDED ends the run cancelled and closes its interactions -- at once --------
// ---- C9: ... and cancel is per run: the next run_workflow() on the same supervisor runs normally ----
void c8_c9_cancel_while_suspended_and_per_run() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph("c8"), port_bodies());
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;
    check(r1.status == workflow_status::suspended && !id.empty(), "C8 setup: the run is suspended at the port");
    (void)drain(stream);

    sup.cancel();
    check(sup.open_interactions().empty(),
          "C8 -- CORE CLAIM (#156): a cancel while suspended closes the open interaction immediately (it used "
          "to change nothing)");
    check(has_run_failed(drain(stream), "cancelled"),
          "C8: the event stream reports the run ending, workflow_run_failed tagged 'cancelled'");

    WorkflowResult late = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}}));
    std::vector<WorkflowEvent> const late_evs = drain(stream);
    bool rejected = false;
    for (auto const& e : late_evs) {
        if (auto const* p = std::get_if<workflow_event_payload::PortRejected>(&e.payload)) {
            rejected = rejected || (e.kind == workflow_event_kind::request_port_rejected && p->reason == "cancelled" &&
                                    p->interaction_id == id);
        }
    }
    check(late.status == workflow_status::cancelled && rejected && !has_run_failed(late_evs, "cancelled"),
          "C8: answering the closed interaction returns cancelled, pushes request_port_rejected (reason "
          "cancelled) and does not report the run ending a second time");
    WorkflowResult cont = drive(sup.continue_workflow(ContinueWorkflow{}));
    check(cont.status == workflow_status::cancelled && cont.rounds == r1.rounds,
          "C8: continue_workflow() on the cancelled run returns cancelled and runs nothing");

    {
        auto decoded = agentengine::rt::decode_run_state_record(agentengine::rt::encode_run_state_record(sup.to_record()));
        WorkflowSupervisor restored;
        restored.initialize(port_graph("c8"), port_bodies());
        if (decoded) restored.restore_from_record(*decoded);
        WorkflowResult rc = drive(restored.continue_workflow(ContinueWorkflow{}));
        check(decoded.has_value() && decoded->cancelled && rc.status == workflow_status::cancelled,
              "C8: a checkpoint of the cancelled run, restored and continued, is still cancelled (not completed)");
    }

    WorkflowResult r2 = drive(sup.run_workflow(RunWorkflow{text_message("again")}));
    check(r2.status == workflow_status::suspended && r2.open_interactions.size() == 1,
          "C9 -- CORE CLAIM (#156): cancel() is per run -- a NEW run on the same supervisor runs normally "
          "(it used to end cancelled forever)");
    std::string const id2 = r2.open_interactions.front().interaction_id;
    WorkflowResult r3 = drive(sup.resume_workflow(ResumeWorkflow{id2, text_message("yes"), {}}));
    check(r3.status == workflow_status::completed && text_of(r3.output) == "yes>sink",
          "C9: ... and completes");

    sup.cancel();  // late: the run has already completed
    WorkflowResult r4 = drive(sup.run_workflow(RunWorkflow{text_message("third")}));
    check(r4.status == workflow_status::suspended,
          "C9: a cancel that arrives after a run completed does not poison the next run");
}

// ---- C10: a cancel in the round that REACHES a port -- the port never opens -----------------------
void c10_cancel_in_round_that_reaches_a_port() {
    auto gate = std::make_shared<Gate>();
    Workflow wf;
    wf.id        = "c10";
    wf.executors = {node_desc("start"), kind_desc("port", executor_kind::request_port), node_desc("slow")};
    wf.edges.push_back(Edge{"start", "port", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"start", "slow", edge_kind::fan_out, {}});
    wf.start            = "start";
    wf.bound.max_rounds = 6;
    WorkflowSupervisor sup;
    sup.initialize(wf, {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                        {},
                        [gate](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                            gate->park();
                            return ExecutorOutcome{in};
                        }});
    WorkflowResult r;
    std::thread driver([&] { r = drive(sup.run_workflow(RunWorkflow{text_message("go")})); });
    check(gate->wait_entered(), "C10 setup: the round that reaches the port is running");
    sup.cancel();
    gate->release();
    driver.join();
    check(r.status == workflow_status::cancelled && r.open_interactions.empty() && sup.open_interactions().empty(),
          "C10 (#156): a cancel in the round that reaches a port ends cancelled with no open interaction (it "
          "used to end suspended)");
    check(r.unopened_ports.size() == 1 && r.unopened_ports[0] == "port",
          "C10: the port reached that round is named in unopened_ports, not silently dropped");
}

// ---- C12: a cancel while suspended inside a NESTED sub-workflow closes the inner run too -----------
void c12_cancel_closes_nested_sub_workflow() {
    Workflow outer_wf;
    outer_wf.id        = "c12-outer";
    outer_wf.executors = {node_desc("start"), kind_desc("sub", executor_kind::sub_workflow), node_desc("sink")};
    outer_wf.edges.push_back(Edge{"start", "sub", edge_kind::direct, {}});
    outer_wf.edges.push_back(Edge{"sub", "sink", edge_kind::direct, {}});
    outer_wf.start            = "start";
    outer_wf.output_selection = {"sink"};
    outer_wf.bound.max_rounds = 6;

    WorkflowSupervisor outer;
    outer.initialize(outer_wf, {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                                {},
                                [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }});
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(port_graph("c12-inner"), port_bodies());
    outer.bind_sub_workflow("sub", inner);

    WorkflowResult r1 = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
    check(r1.status == workflow_status::suspended && inner->open_interactions().size() == 1,
          "C12 setup: the outer run is suspended on the inner run's port");
    outer.cancel();
    check(outer.open_interactions().empty() && inner->open_interactions().empty(),
          "C12 (#156): cancelling the outer closes the nested interaction AND the inner run's own port");
    WorkflowResult again = drive(inner->continue_workflow(ContinueWorkflow{}));
    check(again.status == workflow_status::cancelled, "C12: the inner run is cancelled, not merely orphaned");
}

// ---- C13: RunWorkflow::cancellation -- an outside token linked into the run, honoured even if it ------
// ---- stopped BEFORE the call (the per-run source would otherwise drop such a cancel).               ----
void c13_linked_token_reaches_the_run() {
    WorkflowSupervisor sup;
    sup.initialize(chain_graph("c13"),
                   {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                    [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                    [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }});
    std::stop_source outside;
    outside.request_stop();
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go"), std::nullopt, outside.get_token()}));
    check(r.status == workflow_status::cancelled && r.rounds == 0,
          "C13 (#156): a linked token already stopped before run_workflow() cancels that run before round 1");
    WorkflowResult r2 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r2.status == workflow_status::completed, "C13: the next, unlinked run is unaffected");
}

// ---- C14: an outer cancel reaches a NESTED sub-workflow while it is running ------------------------
void c14_cancel_reaches_nested_run_in_flight() {
    auto gate        = std::make_shared<Gate>();
    auto second_runs = std::make_shared<std::atomic<int>>(0);
    auto inner       = std::make_shared<WorkflowSupervisor>();
    inner->initialize(chain_graph("c14-inner"),
                      {[gate](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                           gate->park();
                           return ExecutorOutcome{in};
                       },
                       [second_runs](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                           second_runs->fetch_add(1);
                           return ExecutorOutcome{in};
                       },
                       [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }});

    Workflow wf;
    wf.id        = "c14-outer";
    wf.executors = {kind_desc("sub", executor_kind::sub_workflow), node_desc("after")};
    wf.edges.push_back(Edge{"sub", "after", edge_kind::direct, {}});
    wf.start            = "sub";
    wf.bound.max_rounds = 4;
    WorkflowSupervisor outer;
    outer.initialize(wf, {{}, [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }});
    outer.bind_sub_workflow("sub", inner);

    WorkflowResult r;
    std::thread driver([&] { r = drive(outer.run_workflow(RunWorkflow{text_message("go")})); });
    check(gate->wait_entered(), "C14 setup: the nested run is in flight");
    outer.cancel();
    gate->release();
    driver.join();
    check(r.status == workflow_status::cancelled && r.failed_executor.empty(),
          "C14 (#156): the outer run ends cancelled (the nested run's cancellation is not a step failure)");
    check(second_runs->load() == 0,
          "C14 (#156): the nested run stopped at its own next round -- it did not run to completion first");
}

}  // namespace

int main() {
    c1_c2_round_boundary_cancellation_is_deterministic_and_non_preempting();
    c3_cooperative_mid_call_observes_cancellation();
    c4_non_cooperative_body_completes_normally();
    c5_unused_cancellation_is_a_no_op();
    c6_cancellation_token_mirrors_cancel();
    c7_cancel_during_agent_step_ends_cancelled();
    c8_c9_cancel_while_suspended_and_per_run();
    c10_cancel_in_round_that_reaches_a_port();
    c11_cancel_observing_failure_is_not_executor_failed();
    c12_cancel_closes_nested_sub_workflow();
    c13_linked_token_reaches_the_run();
    c14_cancel_reaches_nested_run_in_flight();

    std::fprintf(stderr, g_failures == 0 ? "test_rt_workflow_cancellation: ALL PASS\n"
                                          : "test_rt_workflow_cancellation: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
