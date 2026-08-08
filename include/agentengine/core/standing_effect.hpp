#pragma once
// Implements 006-Tool-and-Function-Plane.md §6b and 019-Durability-and-Long-Running-Agents.md §2 --
// "One handle shape for three producers -- schedule_wakeup, watch_resource, and background_task all
// return a StandingEffect handle (unforgeable, like a capability handle, 007 §3.4), with one
// introspection/kill surface: list_standing_effects()/cancel_standing_effect(handle)." Milestone 7
// Phase B (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Milestone 7 Phase B scope: `background_task` is the one real producer (agent_session.hpp's
// `AgentSession::start_background_task()`) -- it is what 019 §2's un-shipped "Local background task
// completion" wake row needs, and what 004 §8 Q1's batch-API resolution is blocked on. `schedule_wakeup`
// (019 §2's "Timer/schedule" row) already ships for real via `TimerWake`/Quark's reminder service
// (M4 Phase E3) WITHOUT going through this handle shape -- retrofitting it to mint a StandingEffect
// too is a real, named follow-up, not done here (it would touch reminder-arming call sites this phase
// does not otherwise need to touch). `watch_resource` has no real producer anywhere in this codebase
// yet; its wake condition is 019 §2's "External event" row, which needs 012 (A2A) -- this milestone's
// own Phase D, not this one. Both are named here, not silently claimed built.
//
// All-scalar/string fields -- no variant, no serialization gap -- so `StandingEffect` is `Described`
// (QUARK_SERIALIZE-able) from the start, the same precedent `Interaction` (interaction.hpp, M4 Phase
// E1) already set for exactly this reason: a session's own durable record can embed it directly
// (`AgentSessionRecord.open_standing_effects`, agent_session.hpp) without inventing a projection.

#include <cstdint>
#include <string>

#include "quark/core/describe.hpp"

namespace agentengine {

// 006 §6b: "one handle shape for three producers." `background_task` is the only real producer this
// phase builds; the other two enumerators exist so the vocabulary is total for whichever phase wires
// them for real (see file-top comment).
enum class standing_effect_kind { schedule_wakeup, watch_resource, background_task };  // ae-naming-lint: allow standing_effect_kind — 006 §6b names this vocabulary normatively; 027 has not been updated to list it

// The unforgeable handle 006 §6b's own text describes ("unforgeable, like a capability handle, 007
// §3.4"). Unforgeability here means the same thing `BoundCapability` gets from being mint-only via
// `CapabilitySet::bind()` (trust/capability.hpp): nothing outside `AgentSession::start_background_task()`
// constructs one with a real, session-registry-recognized `handle_id` -- a caller-fabricated
// `StandingEffect{}` with a guessed id simply fails to match any entry `cancel_standing_effect()`
// looks up (007 §3.4's "unforgeable" property is "the registry is the source of truth", not
// cryptographic unguessability, matching `BoundCapability`'s own ticket-based revocation shape one
// layer down).
struct StandingEffect {  // ae-naming-lint: allow StandingEffect — 006 §6b names this concept normatively; 027 has not been updated to list it
    std::string          handle_id;
    std::string          session_id;
    std::string          principal_id;    // 019 §2 G8: cross-principal cancel_standing_effect() denial
    std::string          run_id;          // the run that registered this effect -- 013 §1's
                                           // ToolCallFinished (or a future StateChanged) needs to be
                                           // attributed to the run that ASKED for the background work,
                                           // not whatever run happens to be current when it resolves.
    standing_effect_kind kind = standing_effect_kind::background_task;
    std::string          label;           // background_task: the tool_name; introspection-only.

    friend bool operator==(StandingEffect const&, StandingEffect const&) = default;
};
QUARK_SERIALIZE(StandingEffect, (1, handle_id), (2, session_id), (3, principal_id), (4, run_id),
                 (5, kind), (6, label))

}  // namespace agentengine
