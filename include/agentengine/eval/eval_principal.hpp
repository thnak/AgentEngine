#pragma once
// Implements ADR-187 §3.9 (round-4 fix, E25's second positive control): trial principals are minted
// under a reserved `eval:` tenant prefix so a harness-held grant can never satisfy a check against a
// production mount (`memory_mount_id`/`memory_ref_name`, memory.hpp:82-84,133-135, compared as bare
// strings at memory.hpp:395 and worktree_mount.hpp:267,362).
//
// Round 4 found the prefix alone provable-wrong: those two functions join `tenant_id` and `id` with
// a bare, UNESCAPED `:`, so `(tenant "eval:acme", id "run1")` and `(tenant "eval", id "acme:run1")`
// produce the identical string `memory:eval:acme:run1` — a reserved PREFIX proves nothing if the
// delimiter it is anchored to is not itself reserved. This file is the fix: it refuses to mint a
// trial principal whose tenant suffix or id contains `:`, from either direction, so that specific
// collision cannot be constructed through this function. It does not, and cannot, stop a caller from
// building a `Principal{}` aggregate directly and bypassing this check entirely (§3.9 already
// discloses that harness confinement is a property of discipline plus a lint that can fail, not the
// type system) — this file is the discipline half of that pair for principal minting specifically.

#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::eval {

inline constexpr std::string_view kEvalTenantPrefix = "eval:";

// Refuses ANY `:` in either component, not only the specific collision round 4 executed — a
// production-side caller can reuse this same predicate to refuse a `tenant_id` that is literally
// `"eval"` or that itself contains `:`, closing the collision from both directions at once.
[[nodiscard]] inline bool contains_colon(std::string_view s) {
    return s.find(':') != std::string_view::npos;
}

// Mints a trial `Principal` under the reserved `eval:` tenant prefix. `tenant_suffix` and `id` are
// host-supplied (a suite/run identifier), never model output (I3).
[[nodiscard]] inline result<Principal> mint_eval_trial_principal(std::string tenant_suffix, std::string id) {
    if (contains_colon(tenant_suffix)) {
        return std::unexpected(error{failure_class::contract,
                                      "eval trial tenant suffix must not contain ':'",
                                      "eval.tenant_colon"});
    }
    if (contains_colon(id)) {
        return std::unexpected(error{failure_class::contract,
                                      "eval trial id must not contain ':'",
                                      "eval.id_colon"});
    }
    if (tenant_suffix.empty() || id.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "eval trial tenant suffix and id must be non-empty",
                                      "eval.identity_empty"});
    }

    Principal p{};
    p.id        = std::move(id);
    p.tenant_id = std::string(kEvalTenantPrefix) + std::move(tenant_suffix);
    p.kind      = principal_kind::service;
    return p;
}

// A production-side guard (ADR-187 §3.9): a principal-construction path outside this file's own
// minting function can call this to refuse a `tenant_id` that collides with the reserved namespace —
// literally `"eval"`, starting with the reserved prefix, or containing `:` at all (the general form
// of the specific collision round 4 found; refusing any colon closes every reordering of it, not
// just the one that was executed).
[[nodiscard]] inline bool is_eval_reserved_tenant(std::string_view tenant_id) {
    return tenant_id == "eval" || tenant_id.starts_with(kEvalTenantPrefix) || contains_colon(tenant_id);
}

}  // namespace agentengine::eval
