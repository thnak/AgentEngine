#pragma once
// ADR-061 §42/§43 (decisions/ADR-061-host-provided-inbound-transport.md): the recommended path from a
// verified bearer credential to the `CapabilityGrant` `McpServer::dispatch()` (server.hpp) already
// consults for per-request capability authorization. Mirrors `rt::request_authority_from_bearer_
// claims()`'s own role and structure exactly (§31.2), built on the same shared `trust::
// steady_deadline_from()` primitive (trust/steady_deadline.hpp) rather than a second, independently-
// derived clock conversion.
//
// Placed in this SEPARATE header, not inside `server.hpp` itself, mirroring the rt-side bridge's own
// placement decision: `server.hpp` stays free of a `trust/bearer_token.hpp` dependency for callers who
// never verify bearer credentials at all (an embedded, single-tenant host per 018 §1's common case).
// Depends on `server.hpp` one-way, for `CapabilityGrant`/`CapabilitySet` (this function's own return
// type) -- never the reverse.

#include <chrono>
#include <memory>

#include "agentengine/protocol/mcp/server.hpp"
#include "agentengine/trust/bearer_token.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/steady_deadline.hpp"

namespace agentengine::mcp {

// `capabilities` stays REQUIRED and caller-supplied, for the identical reason the rt-side bridge
// already states: 007 §5's policy engine does not exist, and this function has no legitimate way to
// decide what a default should mean for a caller who forgot to wire capabilities at all.
[[nodiscard]] inline result<CapabilityGrant> capability_grant_from_bearer_claims(
        trust::BearerTokenClaims const& claims,
        std::shared_ptr<CapabilitySet const> capabilities,
        std::chrono::system_clock::time_point wall_now,
        std::chrono::steady_clock::time_point steady_now,
        principal_kind kind = principal_kind::service) {
    auto deadline =
        trust::steady_deadline_from(claims.exp, wall_now, steady_now, trust::kMaxAuthorityHorizon);
    if (!deadline) {
        // Re-wrap into this bridge's OWN namespaced code -- symmetric with the rt-side bridge, never
        // the shared primitive's generic code passed straight through.
        return std::unexpected(ae::error{failure_class::contract,
                                          "bearer credential exp exceeds the maximum authority horizon",
                                          "capability_grant.exp_horizon_exceeded"});
    }
    return CapabilityGrant{
        trust::principal_from_bearer_claims(claims, kind),
        std::move(capabilities),
        *deadline,
    };
}

}  // namespace agentengine::mcp
