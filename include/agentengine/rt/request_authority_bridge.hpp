#pragma once
// ADR-061 §31.2 (decisions/ADR-061-host-provided-inbound-transport.md, as corrected in place through
// §33/§34/§35): the recommended -- not construction-enforced, `RequestAuthority` stays a plain
// aggregate matching `Principal`'s own existing posture (ADR-039 §3c) -- path from a verified bearer
// credential to the `RequestAuthority` the Tier-3 mechanism in `agent_session.hpp` (§20-§30, proven)
// already gates real tool authorization on.
//
// First real `rt::` code to reference an `agentengine::trust`-namespace-qualified symbol
// (`trust::BearerTokenClaims`, `trust::principal_from_bearer_claims`) -- `rt::` already depends on a
// header living under the `trust/` directory (`agent_session.hpp` already includes
// `trust/principal.hpp`), so this is not a new directory-level dependency, only the first
// namespace-qualified one. Placed in `rt::`, not `trust::`, on its own merits: `trust::` is a
// lower-level identity/credential-verification layer with no knowledge of sessions or capabilities-
// as-consumed-by-a-run; `rt::` already assembles primitives into session-level concepts
// (`RequestAuthority` is defined in `agent_session.hpp`). Putting this bridge in `trust::` would
// require `trust/` to include the large `rt/agent_session.hpp` orchestration header to reference one
// struct -- the actual inversion.

#include <chrono>
#include <memory>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/bearer_token.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/steady_deadline.hpp"

namespace agentengine::rt {

// The recommended path from a verified bearer credential to a RequestAuthority -- not a
// construction-enforced one: RequestAuthority (agent_session.hpp:278-290) is a plain, fully-public
// aggregate, matching Principal's own existing posture (ADR-039 §3c), so nothing stops a host from
// hand-constructing one directly. This function mirrors `trust::principal_from_bearer_claims()`'s own
// "one shared primitive" role (ADR-039 §3c) so a future red-team pass has exactly one recommended
// place to re-check when this bridge changes -- a convention, named as a convention, not asserted as
// the only possible path.
//
// `capabilities` is REQUIRED and caller-supplied -- never derived inside this function. Not because
// every other capability entry point in this codebase refuses a default (AgentSession itself defaults
// an unset session-level grant to empty, agent_session.hpp:883-887 et al. -- a real, different,
// considered answer to a different question: "what does an ungranted SESSION mean"), but because this
// specific function has no legitimate way to decide what a default should mean for a caller who forgot
// to wire capabilities at all, and 007 §5's policy engine (the only thing that COULD correctly derive
// one) does not exist (ADR-061 §13.9). Guessing empty here would look safe (deny-by-default) while
// actually hiding that omission behind a silently-succeeding, silently-inert authority -- the ambient-
// authority shape I2 forbids this function specifically from introducing.
//
// `wall_now`/`steady_now` MUST be sampled together, by the caller, at the actual admission event --
// not read internally here, and not read at two separate points. This function performs a
// system_clock -> steady_clock conversion (`claims.exp` is necessarily system_clock, since it is a
// wire-transmitted wall-clock claim; `RequestAuthority::expiry` is steady_clock, per ADR-061 §13.4/S8's
// already-decided "no settable-clock authority extension" discipline) that is only correct if both
// samples describe the same instant. Passing them in is this function's own I5 compliance -- the
// nondeterministic read crosses a recorded seam at the caller's admission boundary, consistent in
// spirit with (but, since there is no safe implicit default for a credential-verification instant,
// deliberately stricter than) `start_run()`/`resolve_interaction()`'s own defaulted-`now` convention.
[[nodiscard]] inline result<RequestAuthority> request_authority_from_bearer_claims(
        trust::BearerTokenClaims const& claims,
        std::shared_ptr<agentengine::CapabilitySet const> capabilities,
        std::chrono::system_clock::time_point wall_now,
        std::chrono::steady_clock::time_point steady_now,
        agentengine::principal_kind kind = agentengine::principal_kind::service) {
    // ADR-061 §42/§43: the clock conversion itself is `trust::steady_deadline_from()` (does NOT
    // re-check claims.exp against wall_now as a pass/fail gate -- verify_bearer_token() already did
    // that; the saturating subtraction it performs guards a narrower case: an already-expired-but-
    // otherwise-valid claims object converts to an ALREADY-DEAD deadline, never a wraparound-derived
    // far-future one). Re-wrapped into THIS function's own, already-tested error code -- the shared
    // primitive's generic `steady_deadline.horizon_exceeded` is never observed by a caller of this
    // function; `tests/test_request_authority_bridge.cpp` asserts the exact string below.
    auto deadline =
        trust::steady_deadline_from(claims.exp, wall_now, steady_now, trust::kMaxAuthorityHorizon);
    if (!deadline) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "bearer credential exp exceeds the maximum authority horizon",
                                          "request_authority.exp_horizon_exceeded"});
    }
    return RequestAuthority{
        trust::principal_from_bearer_claims(claims, kind),
        std::move(capabilities),
        *deadline,
    };
}

}  // namespace agentengine::rt
