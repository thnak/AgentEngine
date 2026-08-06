#pragma once
// Implements 007-Capability-and-Trust-Model.md and I4 — the mandatory attribution parameter
// carried into every effect. Never an ambient thread-local (CONVENTIONS.md "Security rules").

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

struct EffectContext {
    Principal                             principal;
    CapabilitySet const*                  capabilities = nullptr;  // borrowed; never owned here
    // 006 §3 step 7's per-call handles ("materialize capability handles for this call only") --
    // distinct from `capabilities` above (the run's whole held set, useful for read-only checks):
    // these are freshly minted for THIS invocation and revoked at step 10 (core/tool_pipeline.hpp),
    // so a tool that copies one out of this vector (BoundCapability is copyable, ADR-009) still
    // loses it the moment the call ends -- the shared-ticket revocation the handle carries doesn't
    // care how many copies exist. Borrowed; never owned here.
    std::vector<BoundCapability> const*   bound_capabilities = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    std::string                           trace_id;
    std::string                           span_id;
    // Milestone 4 Phase A3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
    // real 001 §1/§2 Run/Turn identity, threaded through here because 019 §3's per-effect
    // idempotency key is derived from exactly `{run_id, turn_index, call_index, argument_digest}`
    // -- the first two live on the EffectContext every effect already carries, rather than a
    // parallel identity parameter. Minted once per `Ask<StartRun, AgentResponse>`
    // (`core/agent_session.hpp`), deterministically from the session's own monotonic run counter
    // -- never wall-clock-derived (001 §7: an unrecorded wall-clock read here would be exactly the
    // kind of untracked nondeterminism I5 forbids).
    std::string  run_id;
    std::uint64_t turn_index = 0;
};

} // namespace agentengine
