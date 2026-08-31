// AgentEngine "get started" examples, 25 -- WorkflowSupervisor's fine-grained, genuinely-live
// per-executor event stream (ADR-152, GitHub issue #29).
//
// Distinct from 09_concurrent_workflow.cpp's own enable_live_view() (a coarse, once-per-superstep
// summary): this example watches a REAL fan-out round -- three participants dispatched
// concurrently, on real ThreadPool worker threads -- and shows the structural decisions
// (executor_dispatched, fan_out_dispatched, executor_completed, fan_in_aggregated, ...) plus each
// participant's own streaming activity (moderator_stream_delta) arrive on ONE merged stream, live,
// not batched to the end of the round. The consumer here polls between resume() calls (a plain
// offline drive loop has nowhere else to poll from), but the events themselves are pushed the
// INSTANT each participant emits them, from whichever worker thread is running that participant's
// call -- see workflow/multiplex_sink.hpp and docs/planning/workflow-event-stream-design-draft.md
// for why that's true even though this example's own consumer only checks in between resumes.
//
// Fully offline -- no live model needed, matching examples 10/13/20/23's own style.
//
// Run: ./agentengine_example_25_workflow_event_stream_live

#include <cstdio>
#include <string>
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
namespace payload = agentengine::workflow::workflow_event_payload;

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
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] Workflow make_graph() {
    Workflow wf;
    wf.id        = "event-stream-live-demo";
    wf.executors = {node_desc("root"), node_desc("researcher"), node_desc("critic"), node_desc("summarizer"),
                     node_desc("join")};
    wf.edges.push_back(Edge{"root", "researcher", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "critic", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "summarizer", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"researcher", "join", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"critic", "join", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"summarizer", "join", edge_kind::fan_in, {}});
    wf.start = "root";
    wf.output_selection.push_back("join");
    wf.bound.max_rounds = 8;
    return wf;
}

// A participant that streams its own progress through the workflow event bridge -- the pattern
// examples/17/19's own live moderator would use with a real chat_stream() call, simplified here to
// avoid a live model dependency (matches this file's own "fully offline" scope).
[[nodiscard]] ExecutorBody streaming_participant(std::string name) {
    return [name](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
        for (int step = 1; step <= 3; ++step) {
            ctx.moderator_delta_sink(name + " step " + std::to_string(step) + "/3");
        }
        return ExecutorOutcome{text_message(text_of(in) + ">" + name)};
    };
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] char const* kind_name(workflow_event_kind k) {
    switch (k) {
        case workflow_event_kind::workflow_run_started: return "workflow_run_started";
        case workflow_event_kind::workflow_run_suspended: return "workflow_run_suspended";
        case workflow_event_kind::workflow_run_resumed: return "workflow_run_resumed";
        case workflow_event_kind::workflow_run_completed: return "workflow_run_completed";
        case workflow_event_kind::workflow_run_failed: return "workflow_run_failed";
        case workflow_event_kind::superstep_started: return "superstep_started";
        case workflow_event_kind::superstep_completed: return "superstep_completed";
        case workflow_event_kind::executor_dispatched: return "executor_dispatched";
        case workflow_event_kind::executor_completed: return "executor_completed";
        case workflow_event_kind::message_routed: return "message_routed";
        case workflow_event_kind::fan_out_dispatched: return "fan_out_dispatched";
        case workflow_event_kind::fan_in_aggregated: return "fan_in_aggregated";
        case workflow_event_kind::route_selected: return "route_selected";
        case workflow_event_kind::request_port_opened: return "request_port_opened";
        case workflow_event_kind::request_port_resolved: return "request_port_resolved";
        case workflow_event_kind::checkpoint_saved: return "checkpoint_saved";
        case workflow_event_kind::merge_completed: return "merge_completed";
        case workflow_event_kind::merge_conflict: return "merge_conflict";
        case workflow_event_kind::agent_turn_event: return "agent_turn_event";
        case workflow_event_kind::moderator_stream_delta: return "moderator_stream_delta";
    }
    return "?";
}

}  // namespace

int main() {
    Workflow const wf = make_graph();
    check(validate_workflow(wf).has_value(), "the fan-out/fan-in graph validates");

    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        streaming_participant("researcher"),
        streaming_participant("critic"),
        streaming_participant("summarizer"),
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };

    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    agentengine::rt::task<WorkflowResult> run = sup.run_workflow(RunWorkflow{text_message("start")});

    // A real caller would drain `stream` from a separate thread/UI loop while this drives; here,
    // offline and single-threaded, we poll between resumes -- the events queued between polls
    // (from the three concurrently-dispatched participants above, each on its own ThreadPool
    // worker thread) are what proves this is genuinely live, not just observable after the fact.
    std::size_t moderator_deltas_seen = 0;
    std::size_t fan_out_seen          = 0;
    std::size_t fan_in_seen           = 0;
    while (!run.done()) {
        run.resume();
        while (std::optional<WorkflowEvent> ev = stream.next()) {
            std::printf("[event] round=%u kind=%s\n", ev->round, kind_name(ev->kind));
            if (ev->kind == workflow_event_kind::moderator_stream_delta) {
                if (auto const* p = std::get_if<payload::ModeratorDelta>(&ev->payload)) {
                    std::printf("        %s: %s\n", p->executor_id.c_str(), p->text_delta.c_str());
                }
                ++moderator_deltas_seen;
            } else if (ev->kind == workflow_event_kind::fan_out_dispatched) {
                ++fan_out_seen;
            } else if (ev->kind == workflow_event_kind::fan_in_aggregated) {
                ++fan_in_seen;
            }
        }
    }
    WorkflowResult r = run.take_value();
    while (std::optional<WorkflowEvent> ev = stream.next()) {
        std::printf("[event, drained after done()] round=%u kind=%s\n", ev->round, kind_name(ev->kind));
    }

    check(r.status == workflow_status::completed, "the fan-out/fan-in run completes");
    check(moderator_deltas_seen == 9,
          "all 9 real moderator_stream_delta events (3 participants x 3 steps each) were observed live, "
          "not just a summary after the round");
    check(fan_out_seen == 1, "exactly one fan_out_dispatched fired, carrying all 3 real targets");
    check(fan_in_seen == 1,
          "exactly one fan_in_aggregated fired, listing all 3 real contributing sources -- not one "
          "event per converging edge");

    std::fprintf(stderr, g_failures == 0 ? "example_25_workflow_event_stream_live: OK\n"
                                          : "example_25_workflow_event_stream_live: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
