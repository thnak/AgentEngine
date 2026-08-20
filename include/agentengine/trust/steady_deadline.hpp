#pragma once
// ADR-061 §42/§43 (decisions/ADR-061-host-provided-inbound-transport.md): converts a wall-clock
// deadline to a steady-clock one, sampled together by the caller at the same admission event (I5: the
// nondeterministic read crosses a recorded seam at the caller's boundary, never read internally here).
//
// Extracted from ADR-061 §31.2's own bearer-credential-to-RequestAuthority bridge, which needed this
// exact conversion first and got it right only after two design rounds and three red-team passes
// (§31-§35) -- shared here rather than re-derived a second time by the sibling MCP-side bridge
// (protocol/mcp/capability_grant_bridge.hpp), matching this project's own "one shared primitive, not N
// independently-written copies" discipline (trust/hmac.hpp's extraction,
// trust::principal_from_bearer_claims()'s own stated role).
//
// Deliberately generic -- NOT tied to bearer tokens specifically (the underlying problem has nothing
// to do with them), so this file has no bearer-token-specific dependency: a future caller with a
// different system_clock-based expiry source is not forced to pull in HMAC/replay-guard machinery it
// has no use for. Returns a generic `steady_deadline.horizon_exceeded` error on rejection -- CALLERS
// re-wrap this into their own established, already-tested error vocabulary; this primitive does not
// invent a caller-specific code.

#include <chrono>

#include "agentengine/core/error.hpp"

namespace agentengine::trust {

// Bounded: `exp` more than `max_horizon` past `wall_now` is rejected outright rather than reaching an
// unchecked duration_cast. Saturating: an already-past `exp` converts to `steady_now` itself (dead on
// arrival), never a wraparound-derived far-future deadline.
[[nodiscard]] inline result<std::chrono::steady_clock::time_point> steady_deadline_from(
        std::chrono::system_clock::time_point exp, std::chrono::system_clock::time_point wall_now,
        std::chrono::steady_clock::time_point steady_now,
        std::chrono::system_clock::duration max_horizon) {
    if (exp > wall_now + max_horizon) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "deadline exceeds the maximum authority horizon",
                                          "steady_deadline.horizon_exceeded"});
    }
    auto const remaining = exp > wall_now
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(exp - wall_now)
        : std::chrono::steady_clock::duration::zero();
    return steady_now + remaining;
}

}  // namespace agentengine::trust
