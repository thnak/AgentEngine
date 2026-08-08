#pragma once
// Implements 014-Workflow-and-Orchestration.md §7's last bullet: "a running workflow exposes a
// live view of executor states, in-flight messages, and round number" -- over `ae::stream<T>`
// (004 §1's adapter over Quark's credit-controlled ring), per §7's own words. Milestone 6 Phase G
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// One event per superstep boundary, at the SAME point `WorkflowSupervisor::checkpoint_hook_`
// fires from (supervisor.hpp) -- that is the one place `execute()`'s round loop actually knows
// "a superstep just finished, here is what happened in it". `WorkflowLiveEvent` is deliberately
// NOT `RunStateRecord` (checkpoint.hpp): a checkpoint is a durability record (the full state
// needed to resume exactly); a live event is a UI-shaped summary (WHICH executors ran/failed/
// opened a port THIS round, and how many messages are now in flight) -- the two answer different
// questions and conflating them would make the live event carry Message payloads a dashboard has
// no use for and a checkpoint not need duplicated.
//
// A round that ends the run via a `fail`-policy failure (the same round `checkpoint_hook_` also
// skips, supervisor.hpp's own `broke` branch) produces no live event either -- there is no
// completed superstep to describe, and `WorkflowResult` already carries full attribution for that
// case. Named here rather than silently assumed covered.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agentengine::workflow {

enum class executor_live_state { pending, ran_ok, ran_failed, port_open };  // ae-naming-lint: allow executor_live_state — 014 §7 names "executor states" normatively; 027 has not been updated to list this vocabulary

struct ExecutorLiveState {  // ae-naming-lint: allow ExecutorLiveState — this file's own introspection-record family, mirrors AgentSessionRecord's naming
    std::string          executor_id;
    executor_live_state  state;

    friend bool operator==(ExecutorLiveState const&, ExecutorLiveState const&) = default;
};

struct WorkflowLiveEvent {  // ae-naming-lint: allow WorkflowLiveEvent — this file's own introspection-record family, mirrors AgentSessionRecord's naming
    std::uint32_t round = 0;
    // Every executor that did something THIS round: ran (ok/failed) or opened as a port. An
    // executor with nothing to report this round (nothing delivered to it) is simply absent, not
    // listed as `pending` -- `pending` exists in the enum for a future finer-grained event (e.g. "in
    // flight, not yet invoked this round") this file does not build a producer for yet.
    std::vector<ExecutorLiveState> executor_states;
    // `state_.pending.size()` at this same boundary -- messages routed but not yet delivered to
    // their next round's executor.
    std::size_t in_flight_message_count = 0;

    friend bool operator==(WorkflowLiveEvent const&, WorkflowLiveEvent const&) = default;
};

}  // namespace agentengine::workflow
