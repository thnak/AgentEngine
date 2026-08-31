// Proof for ADR-152 (issue #29): WorkflowSupervisor's fine-grained, genuinely-live event stream --
// docs/planning/workflow-event-stream-design-draft.md has the full design and the red-team pass
// that shaped it (workflow_event.hpp/multiplex_sink.hpp are new; effect_context.hpp/
// agent_session.hpp/agent_workflow_executor.hpp/workflow_supervisor.hpp were extended).
//
// W1 -- structural events (workflow_run_started/superstep_started/executor_dispatched/
//       executor_completed/message_routed/superstep_completed/workflow_run_completed) fire, in a
//       plausible order, for a plain linear function graph.
// W2 -- no enable_event_stream() call: a run proceeds exactly as before, same output/status, no
//       crash -- the pre-existing zero-cost-when-unattached guarantee extended, not broken.
// W3 -- request_port_opened/request_port_resolved fire around a real suspend/resume cycle.
// W4 -- checkpoint_saved fires once per round when a checkpoint hook is set.
// W5 -- fan_out_dispatched/fan_in_aggregated fire correctly on a real fan-out/fan-in graph,
//       aggregated across the whole round (fan_in_aggregated lists EVERY contributing source).
// W6 -- route_selected carries the real chosen/available case labels on a switch_case graph.
// W7 -- the attempt discriminator: a retried delivery's moderator_stream_delta events carry
//       attempt=0 then attempt=1 for the SAME executor_id/round, never conflated.
// W8 -- the agent-kind bridge: AgentSession's OWN direct enable_event_stream() and the workflow-
//       level agent_turn_event bridge, wired SIMULTANEOUSLY onto the SAME session, both observe
//       the identical real RunEvent sequence -- proving both the live forwarding chain
//       (WorkflowSupervisor -> EffectContext.agent_turn_sink -> AgentSession.set_run_event_tap ->
//       emit_run_event_for) and the red-teamed fix for the single-consumer collision the first
//       design draft would have had.
// W9 -- the central property this whole redesign exists to guarantee: a fan-out round with
//       several concurrently-dispatched bodies, each pushing far more multiplexed events than the
//       sink's capacity, into a stream NO ONE DRAINS -- the run still completes in bounded time
//       (worker threads never block), and multiplexed_dropped_count() confirms drops happened
//       rather than a silent hang.
// W10-W13 -- issue #42 item 3 (ADR-157 §4's last named residual): forwarding a nested
//       WorkflowSupervisor run's own multiplexed events outward through the OUTER's stream, wired
//       transiently at dispatch time (ScopedForwardedEventSink, workflow_supervisor.hpp) rather
//       than at bind/enable time -- docs/planning/nested-workflow-event-forwarding-design-draft.md.
//   W10 -- single-level forwarding: the inner node's own moderator_stream_delta is observed on the
//          OUTER's stream, tagged with path=[the outer's own sub_workflow node id].
//   W11 -- 3-level forwarding: the innermost node's own event carries the FULL path (both
//          intermediate sub_workflow node ids, in order), not just the immediate parent.
//   W12 -- the central liveness property: a nested node's own event is observed on the outer's
//          stream WHILE the inner run is STILL genuinely in progress (a deliberately-blocked inner
//          node, released only after the polling thread already confirmed the event arrived) --
//          not merely present once the whole nested run already finished, ruling out the
//          "batch after drive() returns" hazard the design draft's Approach 3 was rejected for.
//   W13 -- bind/enable construction order (bind-then-enable vs enable-then-bind) produces
//          identical forwarding -- nothing is precomputed at bind/enable time, so order genuinely
//          does not matter.
//
// Run: ./test_rt_workflow_event_stream

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::AgentSession;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::StartRun;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::agent_session_as_executor_body;
using agentengine::rt::workflow_status;
using agentengine::workflow::WorkflowEvent;
using agentengine::workflow::workflow_event_kind;
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

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] std::vector<WorkflowEvent> drain_all(WorkflowEventStream& s) {
    std::vector<WorkflowEvent> out;
    while (std::optional<WorkflowEvent> ev = s.next()) out.push_back(std::move(*ev));
    return out;
}

[[nodiscard]] bool contains_kind(std::vector<WorkflowEvent> const& evs, workflow_event_kind k) {
    for (auto const& e : evs) {
        if (e.kind == k) return true;
    }
    return false;
}

[[nodiscard]] std::size_t index_of_first(std::vector<WorkflowEvent> const& evs, workflow_event_kind k) {
    for (std::size_t i = 0; i < evs.size(); ++i) {
        if (evs[i].kind == k) return i;
    }
    return evs.size();
}

// ---- W1/W2: a plain 2-node linear graph -----------------------------------------------------------

[[nodiscard]] Workflow linear_graph() {
    Workflow wf;
    wf.id        = "event-stream-linear";
    wf.executors = {node_desc("a"), node_desc("b")};
    wf.edges.push_back(Edge{"a", "b", edge_kind::direct, {}});
    wf.start = "a";
    wf.output_selection.push_back("b");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> linear_bodies() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">a")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">b")};
        },
    };
}

void w1_structural_events() {
    WorkflowSupervisor sup;
    sup.initialize(linear_graph(), linear_bodies());
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W1: the run completes");
    check(text_of(r.output) == "go>a>b", "W1: the real output is unaffected by event streaming");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    check(contains_kind(evs, workflow_event_kind::workflow_run_started), "W1: workflow_run_started fired");
    check(contains_kind(evs, workflow_event_kind::superstep_started), "W1: superstep_started fired");
    check(contains_kind(evs, workflow_event_kind::executor_dispatched), "W1: executor_dispatched fired");
    check(contains_kind(evs, workflow_event_kind::executor_completed), "W1: executor_completed fired");
    check(contains_kind(evs, workflow_event_kind::message_routed), "W1: message_routed fired");
    check(contains_kind(evs, workflow_event_kind::superstep_completed), "W1: superstep_completed fired");
    check(contains_kind(evs, workflow_event_kind::workflow_run_completed), "W1: workflow_run_completed fired");
    check(index_of_first(evs, workflow_event_kind::workflow_run_started) <
              index_of_first(evs, workflow_event_kind::executor_dispatched),
          "W1: workflow_run_started precedes the first executor_dispatched");
    check(index_of_first(evs, workflow_event_kind::executor_dispatched) <
              index_of_first(evs, workflow_event_kind::workflow_run_completed),
          "W1: executor_dispatched precedes the terminal workflow_run_completed");

    bool found_message_routed_ab = false;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::message_routed) continue;
        if (auto const* p = std::get_if<payload::MessageRouted>(&e.payload)) {
            if (p->from_executor_id == "a" && p->to_executor_id == "b" && p->kind == edge_kind::direct) {
                found_message_routed_ab = true;
            }
        }
    }
    check(found_message_routed_ab, "W1: message_routed carries the real a->b direct edge");
}

void w2_no_wiring_is_a_no_op() {
    WorkflowSupervisor sup;
    sup.initialize(linear_graph(), linear_bodies());
    // Deliberately never call enable_event_stream().
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W2: unwired run still completes");
    check(text_of(r.output) == "go>a>b", "W2: unwired run's output matches the wired run's exactly");
}

// ---- W3: suspend/resume around a request_port ------------------------------------------------------

[[nodiscard]] Workflow port_graph() {
    Workflow wf;
    wf.id        = "event-stream-port";
    wf.executors = {node_desc("planner"), node_desc("review", executor_kind::request_port)};
    wf.edges.push_back(Edge{"planner", "review", edge_kind::direct, {}});
    wf.start = "planner";
    wf.output_selection.push_back("review");
    wf.bound.max_rounds = 8;
    return wf;
}

void w3_request_port_events() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">planned")};
        },
        {},
    };
    sup.initialize(port_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("draft")}));
    check(r1.status == workflow_status::suspended, "W3: the run suspends at the review port");
    std::vector<WorkflowEvent> evs1 = drain_all(stream);
    check(contains_kind(evs1, workflow_event_kind::request_port_opened), "W3: request_port_opened fired");
    check(!contains_kind(evs1, workflow_event_kind::request_port_resolved),
          "W3: request_port_resolved has NOT fired yet -- nothing has been resumed");

    WorkflowResult r2 = drive(sup.resume_workflow(
        ResumeWorkflow{r1.open_interactions.at(0).interaction_id, text_message("approved"), {}}));
    check(r2.status == workflow_status::completed, "W3: resuming completes the run");
    std::vector<WorkflowEvent> evs2 = drain_all(stream);
    check(contains_kind(evs2, workflow_event_kind::request_port_resolved),
          "W3: request_port_resolved fires once the port is actually resumed");
}

// ---- W4: checkpoint_saved -----------------------------------------------------------------------

void w4_checkpoint_saved() {
    WorkflowSupervisor sup;
    sup.initialize(linear_graph(), linear_bodies());
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());
    int checkpoint_calls = 0;
    sup.set_checkpoint_hook([&checkpoint_calls](std::uint32_t, agentengine::rt::RunStateRecord const&) {
        ++checkpoint_calls;
    });

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W4: the run completes");
    check(checkpoint_calls > 0, "W4: the checkpoint hook actually ran");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    std::size_t checkpoint_events = 0;
    for (auto const& e : evs) {
        if (e.kind == workflow_event_kind::checkpoint_saved) ++checkpoint_events;
    }
    check(checkpoint_events == static_cast<std::size_t>(checkpoint_calls),
          "W4: checkpoint_saved fires exactly once per real checkpoint_hook_ call");
}

// ---- W5: fan_out/fan_in ------------------------------------------------------------------------

[[nodiscard]] Workflow fan_graph() {
    Workflow wf;
    wf.id        = "event-stream-fan";
    wf.executors = {node_desc("root"), node_desc("a"), node_desc("b"), node_desc("join")};
    wf.edges.push_back(Edge{"root", "a", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "b", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"a", "join", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"b", "join", edge_kind::fan_in, {}});
    wf.start = "root";
    wf.output_selection.push_back("join");
    wf.bound.max_rounds = 8;
    return wf;
}

void w5_fan_out_fan_in_events() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">a")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">b")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(fan_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W5: the fan-out/fan-in run completes");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    bool found_fan_out = false;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::fan_out_dispatched) continue;
        if (auto const* p = std::get_if<payload::FanOut>(&e.payload)) {
            if (p->from_executor_id == "root" && p->to_executor_ids.size() == 2) found_fan_out = true;
        }
    }
    check(found_fan_out, "W5: fan_out_dispatched carries BOTH real targets (a and b)");

    bool found_fan_in = false;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::fan_in_aggregated) continue;
        if (auto const* p = std::get_if<payload::FanIn>(&e.payload)) {
            if (p->to_executor_id == "join" && p->from_executor_ids.size() == 2) found_fan_in = true;
        }
    }
    check(found_fan_in, "W5: fan_in_aggregated is ONE event listing BOTH real contributing sources "
                         "(a and b), aggregated across the whole round, not one event per edge");
}

// ---- W6: route_selected --------------------------------------------------------------------------

[[nodiscard]] Workflow switch_graph() {
    Workflow wf;
    wf.id        = "event-stream-switch";
    wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech")};
    wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
    wf.start = "triage";
    wf.output_selection.push_back("billing");
    wf.output_selection.push_back("tech");
    wf.bound.max_rounds = 8;
    return wf;
}

void w6_route_selected_events() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{in, {"billing"}};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(switch_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W6: the switch_case run completes");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    bool found = false;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::route_selected) continue;
        if (auto const* p = std::get_if<payload::RouteSelected>(&e.payload)) {
            if (p->executor_id != "triage") continue;
            bool const chosen_billing_only =
                p->chosen_cases.size() == 1 && p->chosen_cases.at(0) == "billing";
            bool const both_available = p->available_cases.size() == 2;
            if (chosen_billing_only && both_available) found = true;
        }
    }
    check(found, "W6: route_selected reports the REAL decision (chosen=billing) against BOTH "
                 "available cases (billing, tech)");
}

// ---- W7: retry-attempt discriminator -----------------------------------------------------------

[[nodiscard]] Workflow retry_graph() {
    Workflow wf;
    wf.id        = "event-stream-retry";
    wf.executors = {node_desc("start"), node_desc("flaky"), node_desc("sink")};
    wf.edges.push_back(Edge{"start", "flaky", edge_kind::direct, {}});
    Edge flaky_edge{"flaky", "sink", edge_kind::direct, {}};
    flaky_edge.on_failure = EdgeFailurePolicy{edge_failure_policy::retry, 3, {}};
    wf.edges.push_back(flaky_edge);
    wf.start = "start";
    wf.output_selection.push_back("sink");
    wf.bound.max_rounds = 8;
    return wf;
}

void w7_retry_attempt_discriminator() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        [](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
            ctx.moderator_delta_sink("hello from this attempt");
            static bool failed_once = false;
            if (!failed_once) {
                failed_once = true;
                return std::unexpected(
                    error{failure_class::transient, "scripted transient failure", "test.flaky"});
            }
            return ExecutorOutcome{in};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(retry_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W7: the retried run eventually completes");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    std::vector<std::uint32_t> attempts_seen;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::moderator_stream_delta) continue;
        if (auto const* p = std::get_if<payload::ModeratorDelta>(&e.payload)) {
            attempts_seen.push_back(p->attempt);
        }
    }
    check(attempts_seen.size() == 2, "W7: exactly two moderator_stream_delta events fired (one per attempt)");
    check(attempts_seen.size() == 2 && attempts_seen[0] == 0 && attempts_seen[1] == 1,
          "W7: the failed attempt is tagged attempt=0 and the retried attempt is tagged attempt=1 -- "
          "a live consumer can tell them apart instead of conflating a failed attempt's output with "
          "the retry's");
}

// ---- W8: the agent-kind bridge composes with AgentSession's own enable_event_stream() --------------

class ScriptedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    agentengine::rt::task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"scripted reply"};
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

class NoToolsHistoryProvider {
public:
    agentengine::rt::task<result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    agentengine::rt::task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<NoToolsHistoryProvider>);

[[nodiscard]] Workflow agent_graph() {
    Workflow wf;
    wf.id        = "event-stream-agent";
    wf.executors = {node_desc("agent1", executor_kind::agent)};
    wf.start = "agent1";
    wf.output_selection.push_back("agent1");
    wf.bound.max_rounds = 8;
    return wf;
}

void w8_agent_bridge_composes_with_direct_tap() {
    AgentSession<ScriptedChatClient, NoSessionState, NoToolsHistoryProvider> session;
    std::vector<ExecutorBody> bodies = {agent_session_as_executor_body(session)};

    WorkflowSupervisor sup;
    sup.initialize(agent_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    // Wired SIMULTANEOUSLY with the workflow-level bridge above -- the red-teamed fix under test:
    // this must NOT be silently evicted by whatever the workflow bridge does internally.
    agentengine::stream<RunEvent> direct = session.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
    check(r.status == workflow_status::completed, "W8: the agent-kind run completes");

    std::vector<WorkflowEvent> workflow_evs = drain_all(stream);
    std::vector<run_event_kind> via_workflow_bridge;
    for (auto const& e : workflow_evs) {
        if (e.kind != workflow_event_kind::agent_turn_event) continue;
        if (auto const* p = std::get_if<payload::AgentTurn>(&e.payload)) {
            via_workflow_bridge.push_back(p->inner.kind);
        }
    }

    std::vector<run_event_kind> via_direct_tap;
    while (std::optional<RunEvent> ev = direct.next()) via_direct_tap.push_back(ev->kind);

    check(!via_workflow_bridge.empty(), "W8: the workflow-level agent_turn_event bridge saw real events");
    check(!via_direct_tap.empty(),
          "W8: AgentSession's OWN direct enable_event_stream() ALSO saw real events -- neither "
          "consumer evicted the other");
    check(via_workflow_bridge == via_direct_tap,
          "W8: both consumers observed the IDENTICAL real RunEvent kind sequence from the same run");
    check(contains_kind(workflow_evs, workflow_event_kind::agent_turn_event) &&
              !via_workflow_bridge.empty() && via_workflow_bridge.front() == run_event_kind::run_started,
          "W8: the sequence genuinely starts with run_started -- this is the session's real event "
          "stream, not a stub");
}

// ---- W9: the central liveness/backpressure property --------------------------------------------

[[nodiscard]] Workflow chatty_fan_graph() {
    Workflow wf;
    wf.id        = "event-stream-chatty-fan";
    wf.executors = {node_desc("root"), node_desc("chatty1"), node_desc("chatty2"), node_desc("chatty3")};
    wf.edges.push_back(Edge{"root", "chatty1", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "chatty2", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "chatty3", edge_kind::fan_out, {}});
    wf.start = "root";
    wf.output_selection.push_back("chatty1");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] ExecutorBody chatty_body(int n) {
    return [n](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
        // Far more pushes than multiplex_sink's default capacity (1024) -- with 3 concurrently
        // dispatched nodes each pushing this many, a blocking implementation would stall the
        // ThreadPool; a non-blocking one just drops the overflow and keeps going.
        for (int i = 0; i < 5000; ++i) {
            ctx.moderator_delta_sink("chatty node " + std::to_string(n) + " delta " + std::to_string(i));
        }
        return ExecutorOutcome{in};
    };
}

void w9_backpressure_never_stalls_compute() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        chatty_body(1), chatty_body(2), chatty_body(3),
    };
    sup.initialize(chatty_fan_graph(), bodies);
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());
    // Deliberately NEVER drained during the run -- this is the whole point: a consumer that is
    // completely absent must not be able to stall workflow compute.

    auto const started = std::chrono::steady_clock::now();
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    check(r.status == workflow_status::completed, "W9: the run completes despite an undrained, "
                                                    "massively-overflowed multiplexed event stream");
    check(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10,
          "W9: the run completes in bounded time (no worker thread blocked in push()) -- "
          "15000 pushes against a 1024-capacity non-blocking sink, zero drain, finished fast");
    check(stream.multiplexed_dropped_count() > 0,
          "W9: the sink actually dropped the overflow (proving pushes were non-blocking, not just "
          "coincidentally fast) rather than silently expanding without bound");
}

// ---- W10-W13: issue #42 item 3 -- forwarding a nested run's own multiplexed events outward -------
// docs/planning/nested-workflow-event-forwarding-design-draft.md (red-teamed once; the accepted
// mechanism wires an inner's multiplex_sink_/event_path_prefix_ transiently, at DISPATCH time, via
// ScopedForwardedEventSink -- never at bind/enable time, closing both a wrong-path-under-bottom-up-
// construction bug and a genuine cross-thread reassignment race an earlier bind-time design had).

[[nodiscard]] Workflow w42_leaf_graph(char const* id, char const* node_id) {
    Workflow wf;
    wf.id        = id;
    wf.executors = {node_desc(node_id)};
    wf.start     = node_id;
    wf.output_selection.push_back(node_id);
    wf.bound.max_rounds = 4;
    return wf;
}

[[nodiscard]] Workflow w42_wrap_graph(char const* id, char const* sub_node_id) {
    Workflow wf;
    wf.id        = id;
    wf.executors = {node_desc(sub_node_id, executor_kind::sub_workflow)};
    wf.start     = sub_node_id;
    wf.output_selection.push_back(sub_node_id);
    wf.bound.max_rounds = 4;
    return wf;
}

[[nodiscard]] Workflow w42_outer_graph(char const* id) {
    Workflow wf;
    wf.id        = id;
    wf.executors = {node_desc("start"), node_desc("sub", executor_kind::sub_workflow), node_desc("sink")};
    wf.edges.push_back(Edge{"start", "sub", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"sub", "sink", edge_kind::direct, {}});
    wf.start = "start";
    wf.output_selection.push_back("sink");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> w42_outer_bodies() {
    return {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
            {},
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; }};
}

[[nodiscard]] std::optional<payload::ModeratorDelta> find_moderator_delta(
    std::vector<WorkflowEvent> const& evs) {
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::moderator_stream_delta) continue;
        if (auto const* p = std::get_if<payload::ModeratorDelta>(&e.payload)) return *p;
    }
    return std::nullopt;
}

void w10_single_level_forwarding_and_path_tagging() {
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(w42_leaf_graph("w10-inner", "leaf"),
                       {[](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
                           ctx.moderator_delta_sink("nested delta");
                           return ExecutorOutcome{in};
                       }});

    WorkflowSupervisor outer;
    outer.initialize(w42_outer_graph("w10-outer"), w42_outer_bodies());
    outer.bind_sub_workflow("sub", inner);
    WorkflowEventStream stream = outer.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W10: the nested run completes");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    std::optional<payload::ModeratorDelta> found = find_moderator_delta(evs);
    check(found.has_value(), "W10: the INNER node's own moderator_stream_delta is observed on the "
                              "OUTER's stream -- forwarding actually happens, not just structural "
                              "request_port_opened/_resolved");
    check(found.has_value() && found->executor_id == "leaf",
          "W10: executor_id keeps its EXISTING meaning -- the real originating node's own local id, "
          "unaffected by nesting");
    check(found.has_value() && found->path.size() == 1 && found->path[0] == "sub",
          "W10: path carries exactly the outer's own sub_workflow node id -- the lineage from outer "
          "to (not including) the originating node");
    check(found.has_value() && found->text_delta == "nested delta",
          "W10: the real payload content survives the forwarding hop unchanged");
}

void w11_three_level_forwarding_carries_the_full_path() {
    auto leaf = std::make_shared<WorkflowSupervisor>();
    leaf->initialize(w42_leaf_graph("w11-leaf", "leaf"),
                      {[](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
                          ctx.moderator_delta_sink("deep delta");
                          return ExecutorOutcome{in};
                      }});

    auto mid = std::make_shared<WorkflowSupervisor>();
    mid->initialize(w42_wrap_graph("w11-mid", "wrap"), {{}});
    mid->bind_sub_workflow("wrap", leaf);

    WorkflowSupervisor outer;
    outer.initialize(w42_outer_graph("w11-outer"), w42_outer_bodies());
    outer.bind_sub_workflow("sub", mid);
    WorkflowEventStream stream = outer.enable_event_stream(std::pmr::get_default_resource());

    WorkflowResult r = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed, "W11: the 3-level nested run completes");

    std::vector<WorkflowEvent> evs = drain_all(stream);
    std::optional<payload::ModeratorDelta> found = find_moderator_delta(evs);
    check(found.has_value() && found->path.size() == 2 && found->path[0] == "sub" &&
              found->path[1] == "wrap",
          "W11: path carries the FULL lineage (both intermediate sub_workflow node ids, in order), "
          "not just the immediate parent -- the innermost level's own event is unambiguous even at "
          "3 levels deep");
}

void w12_nested_events_are_observed_while_the_run_is_still_in_progress() {
    // MACHINE SAFETY (CLAUDE.md): the inner node's own wait is bounded (10s), never unbounded, even
    // though the test always signals it well before that in the success path.
    std::atomic<bool>      may_proceed{false};
    std::atomic<bool>      run_finished{false};
    std::mutex             cv_mutex;
    std::condition_variable cv;

    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(
        w42_leaf_graph("w12-inner", "blocker"),
        {[&](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
            ctx.moderator_delta_sink("nested live delta");
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return may_proceed.load(); });
            return ExecutorOutcome{in};
        }});

    WorkflowSupervisor outer;
    outer.initialize(w42_outer_graph("w12-outer"), w42_outer_bodies());
    outer.bind_sub_workflow("sub", inner);
    WorkflowEventStream stream = outer.enable_event_stream(std::pmr::get_default_resource());

    // Driven on a SEPARATE thread: the inner node's own body genuinely blocks the worker thread
    // running it (inner->pool_'s own worker, joined-via-.get() by the outer's own worker, joined by
    // this driver thread's own drive() call) until this test explicitly releases it below -- the
    // main thread must stay free to poll `stream` concurrently while that block is in effect, which
    // is the entire point of this proof (ruling out the "batch after drive() returns" hazard the
    // design draft's Approach 3 was rejected for).
    std::thread driver([&] {
        WorkflowResult r = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::completed, "W12: the run eventually completes once unblocked");
        run_finished.store(true);
    });

    bool observed = false;
    payload::ModeratorDelta observed_payload{};
    for (int i = 0; i < 500 && !observed; ++i) {
        std::optional<WorkflowEvent> ev = stream.next();
        if (ev && ev->kind == workflow_event_kind::moderator_stream_delta) {
            if (auto const* p = std::get_if<payload::ModeratorDelta>(&ev->payload)) {
                observed_payload = *p;
                observed         = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(observed, "W12: the nested node's own moderator_stream_delta IS observed on the outer's "
                     "stream (within a bounded 5s poll budget)");
    // Deterministic, not a race: run_finished can only ever become true AFTER may_proceed is set,
    // which this test has not done yet at this point -- the inner node is still genuinely parked in
    // cv.wait_for() right now, so this assertion cannot pass by luck or timing.
    check(!run_finished.load(),
          "W12: ...and it was observed WHILE the run was still genuinely in progress (the blocked "
          "inner node has not yet been released) -- not merely present once the whole nested run "
          "already finished, which is the actual property this whole mechanism exists to provide");
    check(observed_payload.path.size() == 1 && observed_payload.path[0] == "sub",
          "W12: the live-observed event still carries the correct path");

    {
        std::lock_guard<std::mutex> lock(cv_mutex);
        may_proceed.store(true);
    }
    cv.notify_all();
    driver.join();
}

void w13_bind_enable_order_independence() {
    // Order A: bind, then enable.
    {
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(w42_leaf_graph("w13a-inner", "leaf"),
                           {[](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
                               ctx.moderator_delta_sink("order-a delta");
                               return ExecutorOutcome{in};
                           }});
        WorkflowSupervisor outer;
        outer.initialize(w42_outer_graph("w13a-outer"), w42_outer_bodies());
        outer.bind_sub_workflow("sub", inner);  // bind FIRST
        WorkflowEventStream stream = outer.enable_event_stream(std::pmr::get_default_resource());  // enable SECOND

        WorkflowResult r = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::completed, "W13a: bind-then-enable run completes");
        std::optional<payload::ModeratorDelta> found = find_moderator_delta(drain_all(stream));
        check(found.has_value() && found->path.size() == 1 && found->path[0] == "sub",
              "W13a: bind-then-enable forwards correctly with the right path");
    }
    // Order B: enable, then bind.
    {
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(w42_leaf_graph("w13b-inner", "leaf"),
                           {[](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
                               ctx.moderator_delta_sink("order-b delta");
                               return ExecutorOutcome{in};
                           }});
        WorkflowSupervisor outer;
        outer.initialize(w42_outer_graph("w13b-outer"), w42_outer_bodies());
        WorkflowEventStream stream = outer.enable_event_stream(std::pmr::get_default_resource());  // enable FIRST
        outer.bind_sub_workflow("sub", inner);  // bind SECOND

        WorkflowResult r = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::completed, "W13b: enable-then-bind run completes");
        std::optional<payload::ModeratorDelta> found = find_moderator_delta(drain_all(stream));
        check(found.has_value() && found->path.size() == 1 && found->path[0] == "sub",
              "W13b: enable-then-bind forwards identically -- construction order genuinely does not "
              "matter, since nothing is precomputed at bind/enable time at all");
    }
}

}  // namespace

int main() {
    w1_structural_events();
    w2_no_wiring_is_a_no_op();
    w3_request_port_events();
    w4_checkpoint_saved();
    w5_fan_out_fan_in_events();
    w6_route_selected_events();
    w7_retry_attempt_discriminator();
    w8_agent_bridge_composes_with_direct_tap();
    w9_backpressure_never_stalls_compute();
    w10_single_level_forwarding_and_path_tagging();
    w11_three_level_forwarding_carries_the_full_path();
    w12_nested_events_are_observed_while_the_run_is_still_in_progress();
    w13_bind_enable_order_independence();

    std::fprintf(stderr, g_failures == 0 ? "test_rt_workflow_event_stream: ALL PASS\n"
                                          : "test_rt_workflow_event_stream: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
