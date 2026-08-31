// Proof for ADR-037 Phase 3: 014-Workflow-and-Orchestration.md §7's live-view bullet ("a running
// workflow exposes a live view of executor states, in-flight messages, and round number") against
// `agentengine::rt::WorkflowSupervisor::enable_live_view()` -- the Quark-actor-free replacement for
// `agentengine::workflow::WorkflowSupervisor`'s own live view. SUPERSEDES the old, Quark-actor-based
// test_workflow_live_view.cpp (LV1/LV2, both reused verbatim here, same graphs/claims), matching the
// precedent test_rt_workflow_supervisor_request_port.cpp already set for superseding
// test_workflow_request_port(.cpp)/test_workflow_request_port_independent_qa.cpp. Ported onto the
// `rt::` single-threaded harness that file established (`drive<T>()`).
//
// UNLIKE the old Quark-actor test: no bounded-deadline drain loop is needed here. The original's
// producer ran on a separate worker thread (a real Quark actor), so draining had to tolerate genuine
// scheduling jitter. `rt::WorkflowSupervisor::execute()` pushes live-view events synchronously, on the
// SAME calling thread `drive()` spins `.resume()` on -- by the time `run_workflow()`/`resume_workflow()`
// returns, every event for that call is already buffered. A single drain-to-empty pass is correct, not
// a formality against timing.
//
//   LV1 -- draft -> ready -> [approve] -> publish. Three superstep boundaries before suspending, each
//          producing exactly one live event; resuming produces a fourth for publish's own round -- the
//          SAME producer keeps working across a suspend/resume boundary.
//   LV2 -- a round that ends the run via a `fail`-policy failure produces NO live event for that round
//          (the same `broke` branch the checkpoint hook already skips -- live_view.hpp's own header
//          comment names this explicitly).

#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::workflow::WorkflowLiveEvent;
using agentengine::workflow::executor_live_state;

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

// Same rationale as test_rt_workflow_supervisor_request_port.cpp's own drive<T>().
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
using agentengine::sharing_mode;

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
            if (!out.empty()) out += ">";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] ExecutorBody failer(std::string message) {
    return [message = std::move(message)](
               Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::fatal, message, "test.injected_failure"});
    };
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::request_port, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

// Drain-to-empty -- see file banner for why no bounded-deadline loop is needed here.
[[nodiscard]] std::vector<WorkflowLiveEvent> drain_all(agentengine::stream<WorkflowLiveEvent>& s) {
    std::vector<WorkflowLiveEvent> out;
    while (auto ev = s.next()) out.push_back(std::move(*ev));
    return out;
}

}  // namespace

int main() {
    // =========================================================================================
    // LV1 -- draft -> ready -> [approve] -> publish. Three superstep boundaries; the live view
    // should report exactly what test_rt_workflow_supervisor_request_port.cpp's own E1-shaped
    // claims already establish each one looks like, from the executor-state angle.
    // =========================================================================================
    {
        Workflow wf;
        wf.id        = "lv-hitl";
        wf.executors = {node_desc("draft"), node_desc("ready"), port_desc("approve"), node_desc("publish")};
        wf.edges     = {Edge{"draft", "ready", edge_kind::direct, {}},
                        Edge{"ready", "approve", edge_kind::direct, {}},
                        Edge{"approve", "publish", edge_kind::direct, {}}};
        wf.start             = "draft";
        wf.output_selection  = {"publish"};
        wf.bound.max_rounds  = 8;

        std::vector<ExecutorBody> bodies = {appender("draft"), appender("ready"), {}, appender("publish")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        auto viewer = sup.enable_live_view(std::pmr::get_default_resource());

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "LV1 setup: the run suspends (sanity)");

        auto events = drain_all(viewer);
        check(events.size() == 3, "LV1: exactly one live event per superstep boundary");

        if (events.size() == 3) {
            check(events[0].round == 1 && events[0].executor_states.size() == 1 &&
                      events[0].executor_states[0].executor_id == "draft" &&
                      events[0].executor_states[0].state == executor_live_state::ran_ok &&
                      events[0].in_flight_message_count == 1,
                  "LV1: event 1 -- draft ran ok, one message now in flight (to 'ready')");
            check(events[1].round == 2 && events[1].executor_states.size() == 1 &&
                      events[1].executor_states[0].executor_id == "ready" &&
                      events[1].executor_states[0].state == executor_live_state::ran_ok &&
                      events[1].in_flight_message_count == 1,
                  "LV1: event 2 -- ready ran ok, one message in flight (to 'approve', not yet "
                  "classified as a port this round)");
            check(events[2].round == 3 && events[2].executor_states.size() == 1 &&
                      events[2].executor_states[0].executor_id == "approve" &&
                      events[2].executor_states[0].state == executor_live_state::port_open &&
                      events[2].in_flight_message_count == 0,
                  "LV1: event 3 -- 'approve' opened as a port, nothing else in flight");
        }

        // Resuming for real should produce a FOURTH event (publish's own round) -- the live view
        // keeps working across a suspend/resume boundary, not just for the first run.
        auto const open = sup.open_interactions();
        check(open.size() == 1, "LV1: one open port to resume (sanity)");
        std::string const interaction_id = open.empty() ? std::string{} : open.front().interaction_id;
        WorkflowResult r2 =
            drive(sup.resume_workflow(ResumeWorkflow{interaction_id, text_message("approved"), {}}));
        check(r2.status == workflow_status::completed, "LV1 resume: the run completes (sanity)");

        auto more = drain_all(viewer);
        check(more.size() == 1 && more[0].round == 4 && more[0].executor_states.size() == 1 &&
                  more[0].executor_states[0].executor_id == "publish" &&
                  more[0].executor_states[0].state == executor_live_state::ran_ok,
              "LV1: resuming produces a further live event for publish's own round -- the SAME "
              "producer keeps working across a suspend/resume boundary");
    }

    // =========================================================================================
    // LV2 -- a round that ends the run via a `fail`-policy failure produces NO live event for
    // that round (the same `broke` branch that skips the checkpoint hook too).
    // =========================================================================================
    {
        Workflow wf;
        wf.id                = "lv-fail";
        wf.executors         = {node_desc("ok_node"), node_desc("bad_node")};
        wf.edges             = {Edge{"ok_node", "bad_node", edge_kind::direct, {}}};
        wf.start             = "ok_node";
        wf.bound.max_rounds  = 4;

        std::vector<ExecutorBody> bodies = {appender("ok"), failer("boom")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        auto viewer = sup.enable_live_view(std::pmr::get_default_resource());

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed,
              "LV2 setup: the second round fails the whole run (sanity)");

        // Exactly ONE event: round 1 (ok_node succeeding). Round 2 (bad_node failing, ending the
        // run) produces none.
        auto events = drain_all(viewer);
        check(events.size() == 1 && events[0].round == 1 &&
                  events[0].executor_states[0].state == executor_live_state::ran_ok,
              "LV2: the successful first round IS reported, and the failing round produced NO live "
              "event -- draining to empty (not a bounded wait) proves nothing is buffered after it");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_live_view: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_live_view: %d failure(s)\n", g_failures);
    return 1;
}
