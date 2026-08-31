#pragma once
// ADR-152 (issue #29): WorkflowSupervisor's fine-grained, genuinely-live event stream --
// docs/planning/workflow-event-stream-design-draft.md has the full design and the red-team pass
// that shaped it. Distinct from `WorkflowLiveEvent` (workflow/live_view.hpp), which stays exactly
// as-is: that type is a coarse, once-per-superstep summary; this one carries every individual
// structural decision AND every per-node streaming delta, as it happens.
//
// TWO BUCKETS, two different delivery mechanisms, merged into one consumer-facing type
// (WorkflowEventStream, below):
//   - STRUCTURAL (routing/lifecycle/checkpoint/merge decisions) -- produced ONLY by
//     WorkflowSupervisor::execute()'s own single coroutine/thread, the same safe single-writer
//     shape enable_live_view() already uses. Rides an ordinary channel-backed
//     agentengine::stream<WorkflowEvent>.
//   - MULTIPLEXED per-node (agent_turn_event/moderator_stream_delta) -- produced concurrently,
//     from whichever ThreadPool worker thread is running that node's call. Rides
//     workflow/multiplex_sink.hpp's multiplex_sink<T> instead -- see that header's own banner for
//     why this bucket deliberately does NOT ride the same channel the structural bucket does.
//
// Every event here is OBSERVATION-ONLY (I2/I3): nothing in this vocabulary is ever fed back into
// a routing, policy, or authorization decision by any engine code. `route_selected`'s payload, for
// instance, is a passive mirror of a decision execute()'s own route_from() already made through
// ExecuteReply::routes -- never a second, independent source of truth for it. A consumer of this
// stream must observe the same discipline: never feed a WorkflowEvent back into an authorization
// decision.
//
// FAN-OUT/FAN-IN: `fan_out_dispatched` fires once per execute()-internal route_from() call that
// has at least one firing `edge_kind::fan_out` edge, carrying every target that fired. `fan_in_
// aggregated` fires once per round per target that received at least one `edge_kind::fan_in`
// delivery, carrying every contributing source -- aggregated across the WHOLE round (a fan-in
// target can receive contributions from several different route_from() calls, one per upstream
// source, within the same round), not per-edge.
//
// ATTEMPT DISCRIMINATOR: execute()'s own EdgeFailurePolicy::retry loop can dispatch the SAME
// executor_id/round more than once. `AgentTurn`/`ModeratorDelta` carry `attempt` so a live
// consumer can tell a retried delivery's own fresh stream apart from a failed prior attempt's
// partial output -- key any per-node rendering state on {executor_id, round, attempt}, never just
// {executor_id, round}.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/run_event.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/multiplex_sink.hpp"

namespace agentengine::workflow {

enum class workflow_event_kind {  // ae-naming-lint: allow workflow_event_kind — mirrors run_event_kind's own project-normative naming (core/run_event.hpp)
    workflow_run_started, workflow_run_suspended, workflow_run_resumed,
    workflow_run_completed, workflow_run_failed,
    superstep_started, superstep_completed,
    executor_dispatched, executor_completed,
    message_routed, fan_out_dispatched, fan_in_aggregated, route_selected,
    request_port_opened, request_port_resolved,
    checkpoint_saved, merge_completed, merge_conflict,
    agent_turn_event, moderator_stream_delta,
};

namespace workflow_event_payload {

struct Empty {};

// `status_tag` is a plain string ("routing_failed", "bound_max_rounds", ...), not
// rt::workflow_status itself -- workflow/ does not depend on rt/ (WorkflowSupervisor lives in
// rt/ and already depends on workflow/; the reverse would be circular). WorkflowSupervisor
// converts its own workflow_status to this tag at the one call site that constructs this payload.
struct RunFailed {
    std::string status_tag;
};

struct ExecutorRef {
    std::string executor_id;
};

struct ExecutorResult {
    std::string executor_id;
    bool        ok = true;
};

struct MessageRouted {
    std::string                       from_executor_id;
    std::string                       to_executor_id;
    agentengine::workflow::edge_kind  kind;
    std::string                       case_label;  // only meaningful for switch_case/multi_selection
};

struct FanOut {
    std::string              from_executor_id;
    std::vector<std::string> to_executor_ids;
};

struct FanIn {
    std::string              to_executor_id;
    std::vector<std::string> from_executor_ids;
};

struct RouteSelected {
    std::string              executor_id;
    std::vector<std::string> chosen_cases;
    std::vector<std::string> available_cases;
};

struct PortRef {
    std::string executor_id;
    std::string interaction_id;
};

struct CheckpointSaved {
    std::uint32_t round = 0;
};

struct MergeRef {
    std::string executor_id;
};

struct SuperstepBounds {
    std::vector<std::string> executor_ids;
};

// The multiplexed bucket -- see file banner's ATTEMPT DISCRIMINATOR note.
//
// `path` (issue #42 item 3, docs/planning/nested-workflow-event-forwarding-design-draft.md):
// the lineage of sub_workflow executor_ids from the outer WorkflowSupervisor down to (but NOT
// including) `executor_id` itself. Empty for anything dispatched directly by the top-level
// supervisor -- a non-nested run's events are byte-for-byte unchanged from before this field
// existed. Deliberately a separate vector<string>, not glued onto `executor_id` with a delimiter:
// a real node id could itself contain any delimiter chosen, and a consumer should never have to
// unescape a composite string to recover the real local id. A consumer distinguishing forwarded
// events from different nesting levels/attempts should key on {path, executor_id, round, attempt},
// not {executor_id, round, attempt} alone, once nesting is in play -- `round` stays whichever
// level's OWN internal round counter was live when THAT level pushed the event, never the outer's.
struct AgentTurn {
    std::string              executor_id;
    std::uint32_t            attempt = 0;
    agentengine::RunEvent    inner;  // wrapped UNCHANGED -- see design draft's I3/taint-preservation note
    std::vector<std::string> path;
};

struct ModeratorDelta {
    std::string              executor_id;
    std::uint32_t            attempt = 0;
    std::string              text_delta;
    std::vector<std::string> path;
};

}  // namespace workflow_event_payload

// ae-naming-lint: allow WorkflowEventPayload — mirrors RunEventPayload's own project-normative naming (core/run_event.hpp)
using WorkflowEventPayload = std::variant<
    workflow_event_payload::Empty, workflow_event_payload::RunFailed,
    workflow_event_payload::ExecutorRef, workflow_event_payload::ExecutorResult,
    workflow_event_payload::MessageRouted, workflow_event_payload::FanOut,
    workflow_event_payload::FanIn, workflow_event_payload::RouteSelected,
    workflow_event_payload::PortRef, workflow_event_payload::CheckpointSaved,
    workflow_event_payload::MergeRef, workflow_event_payload::SuperstepBounds,
    workflow_event_payload::AgentTurn, workflow_event_payload::ModeratorDelta>;

struct WorkflowEvent {  // ae-naming-lint: allow WorkflowEvent — mirrors RunEvent's own project-normative naming (core/run_event.hpp)
    workflow_event_kind  kind  = workflow_event_kind::workflow_run_started;
    std::uint32_t         round = 0;
    WorkflowEventPayload   payload = workflow_event_payload::Empty{};
};

// The consumer-facing handle WorkflowSupervisor::enable_event_stream() returns -- merges the
// structural bucket's ordinary channel-backed stream<WorkflowEvent> with the multiplexed bucket's
// non-blocking multiplex_sink<WorkflowEvent>, presenting one poll-only next()/done() surface,
// matching agentengine::stream<T>'s own contract (core/stream.hpp) so callers already familiar
// with that idiom need nothing new.
//
// DRAIN ORDER: next() checks the multiplexed bucket first. Multiplexed events are the
// high-frequency, latency-sensitive ones (per-token deltas); draining them ahead of the
// comparatively rare structural bucket (at most a handful of events per round) keeps a
// fast-polling consumer from starving on them.
//
// TERMINATION: done()/terminal()/fail_error() all reflect the STRUCTURAL stream's own state only.
// WorkflowSupervisor never explicitly closes either producer on run completion (matching
// enable_live_view()'s own pre-existing behavior) -- a consumer that wants "the run is over" reads
// the workflow_run_completed/_suspended/_failed EVENT, not stream termination. A race is possible,
// and accepted, at the exact moment a run ends: a multiplexed event pushed in the same instant
// could still be sitting in the multiplex_sink after the structural stream reports its last event
// -- an observability-only, already-precedented cost (see multiplex_sink.hpp's own banner),
// exactly like any other drop/loss this design already accepts under load.
class WorkflowEventStream {  // ae-naming-lint: allow WorkflowEventStream — this file's own introspection-stream family, mirrors ae::stream<T>'s naming
public:
    WorkflowEventStream() = default;
    WorkflowEventStream(agentengine::stream<WorkflowEvent> structural,
                        std::shared_ptr<multiplex_sink<WorkflowEvent>> multiplexed)
        : structural_(std::move(structural)), multiplexed_(std::move(multiplexed)) {}

    [[nodiscard]] std::optional<WorkflowEvent> next() {
        if (multiplexed_) {
            if (std::optional<WorkflowEvent> ev = multiplexed_->try_pop()) return ev;
        }
        return structural_.next();
    }

    [[nodiscard]] bool valid() const noexcept { return structural_.valid(); }
    [[nodiscard]] bool done() const noexcept { return structural_.done(); }
    [[nodiscard]] agentengine::stream_terminal terminal() const noexcept { return structural_.terminal(); }
    [[nodiscard]] agentengine::error fail_error() const noexcept { return structural_.fail_error(); }

    // Diagnostics only -- see multiplex_sink.hpp's own comment on why a caller is never required
    // to check this to stay correct.
    [[nodiscard]] std::size_t multiplexed_dropped_count() const noexcept {
        return multiplexed_ ? multiplexed_->dropped_count() : 0;
    }

    void cancel() noexcept { structural_.cancel(); }

private:
    agentengine::stream<WorkflowEvent>              structural_;
    std::shared_ptr<multiplex_sink<WorkflowEvent>>  multiplexed_;
};

}  // namespace agentengine::workflow
