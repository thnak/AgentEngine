#pragma once
// Implements 007-Capability-and-Trust-Model.md §2 and 018-Identity-Authorization-and-Secrets.md
// §1/§2 — the authenticated identity a run executes on behalf of, plus (Milestone 5 Phase H) the
// two inbound-identity mechanisms decision 1 scopes as buildable now ("Embedded / in-process" and
// "Local CLI" — the other three 018 §1 rows need 011/012/013, all M7), delegation (007 §2's
// "on_behalf_of", never elevated claims, depth-bounded), and the session-ownership admission
// predicate 018 §2 requires at the actor boundary. Propagated into every effect and every outbound
// protocol call (I4).

#include <cstdint>
#include <string>
#include <utility>

#include "agentengine/core/error.hpp"

namespace agentengine {

// 007 §2's full shape is `Principal = {id, kind, claims[], issuer, expiry}`. `claims`/`issuer`/
// `expiry` are token-bearing-surface concepts (HTTP bearer/OIDC, 018 §1) that need 013 (M7) to have
// an inbound surface at all — named deferred, not silently dropped, matching decision 1's own
// narrowing. `kind` is kept here because it is meaningful even for the two surfaces this milestone
// builds: Embedded is always `service`, Local CLI is always `human`, a delegated call is always
// `agent`, and 018 §1's "Anonymous is a principal, not a bypass" rule needs a real value to hold.
enum class principal_kind : std::uint8_t { human, service, agent, anonymous };

struct Principal {
    std::string id;         // stable identity, opaque to the core
    std::string tenant_id;  // multi-tenancy scope (018 §6); empty for single-tenant deployments

    principal_kind kind = principal_kind::anonymous;

    // 007 §2: "A sub-agent or delegated call runs as a derived principal carrying on_behalf_of,
    // never as the host and never with elevated claims." Empty = not a delegated principal. Names
    // only the immediate parent's `id`, not the full chain — reconstructing the full chain needs
    // walking an audit surface this milestone doesn't build (018 §7 G4, named out of scope,
    // decision 9); `delegation_depth` below is what bounds chain length, independent of whether the
    // full chain is reconstructable after the fact.
    std::string   on_behalf_of;
    std::uint32_t delegation_depth = 0;

    // Fields added after `id`/`tenant_id` with defaults, deliberately — every pre-existing
    // `Principal{id, tenant_id}` two-argument aggregate-init call site across `tests/` (and
    // `AgentSessionRecord::restore_from_record`, which does not persist these three fields yet,
    // same narrowing category as its own already-named gaps) keeps compiling unchanged.
    friend bool operator==(Principal const&, Principal const&) = default;
};

// 018 §1's "Embedded / in-process" row: "Host-supplied principal; the host is trusted (007 §1)."
[[nodiscard]] inline Principal make_embedded_principal(std::string id, std::string tenant_id = {}) {
    Principal p{};
    p.id        = std::move(id);
    p.tenant_id = std::move(tenant_id);
    p.kind      = principal_kind::service;
    return p;
}

// 018 §1's "Local CLI" row: "Local user identity."
[[nodiscard]] inline Principal make_local_cli_principal(std::string user_id, std::string tenant_id = {}) {
    Principal p{};
    p.id        = std::move(user_id);
    p.tenant_id = std::move(tenant_id);
    p.kind      = principal_kind::human;
    return p;
}

// 018 §1: "Anonymous is a principal, with its own (usually minimal) capability set — not a
// bypass." A stable, shared id rather than a caller-chosen one, so a genuinely anonymous caller is
// trivially distinguishable from a deliberately-forged "anonymous" identity elsewhere in the id
// space.
[[nodiscard]] inline Principal make_anonymous_principal(std::string tenant_id = {}) {
    Principal p{};
    p.id        = "anonymous";
    p.tenant_id = std::move(tenant_id);
    p.kind      = principal_kind::anonymous;
    return p;
}

// 018 §2: "Delegation chains are recorded and depth-bounded." A fixed, small bound — this
// milestone's own real use (H4, one outbound-call hop per delegated sub-agent invocation) never
// approaches it; it exists to make `derive_on_behalf_of` fail closed rather than mint an unbounded
// chain if something ever loops.
inline constexpr std::uint32_t kMaxDelegationDepth = 8;

// 007 §2 / 018 §1's "delegation only via on_behalf_of, never token passthrough" rule, and the
// attenuation discipline 007 G3 already enforces for capabilities, applied here to identity:
// `tenant_id` is copied from `parent` UNCHANGED (a delegated call can never cross a tenant boundary
// its parent didn't already occupy, 018 §6) and `kind` is always `agent` (a derived principal is
// never re-labeled as the more-trusted `human`/`service`.) Fails closed past `kMaxDelegationDepth`
// rather than minting an unbounded chain.
[[nodiscard]] inline result<Principal> derive_on_behalf_of(Principal const& parent, std::string derived_id) {
    if (parent.delegation_depth >= kMaxDelegationDepth) {
        return std::unexpected(error{failure_class::policy,
                                      "delegation chain exceeds the maximum depth",
                                      "principal.delegation_depth_exceeded"});
    }
    Principal derived{};
    derived.id              = std::move(derived_id);
    derived.tenant_id       = parent.tenant_id;
    derived.kind            = principal_kind::agent;
    derived.on_behalf_of    = parent.id;
    derived.delegation_depth = parent.delegation_depth + 1;
    return derived;
}

// 018 §2's admission rule ("may this principal start this run on this session/agent at all?"),
// factored as a free predicate rather than inlined only into `AgentSession::handle()` (Phase H2) so
// any other session-adjacent surface (memory, sandbox workspace — Phase I's I1) can reuse the exact
// same ownership rule instead of re-deriving it. Admitted if `caller` IS the owning principal, or
// is a principal derived `on_behalf_of` the owner — single-hop only, matching `on_behalf_of`'s own
// "immediate parent, not full chain" scope above — and never across a tenant boundary (018 §6),
// even when the id matches (a cross-tenant id collision is not ownership).
[[nodiscard]] inline bool principal_admitted_for(Principal const& caller, Principal const& owner) {
    if (caller.tenant_id != owner.tenant_id) return false;
    if (caller.id == owner.id) return true;
    return !caller.on_behalf_of.empty() && caller.on_behalf_of == owner.id;
}

} // namespace agentengine
