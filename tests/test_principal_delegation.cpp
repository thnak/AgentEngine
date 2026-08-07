// Milestone 5 Phase H1 (018-Identity-Authorization-and-Secrets.md §1's inbound-identity table,
// decision 1's own narrowing: only "Embedded / in-process" and "Local CLI" are buildable now
// without 011/012/013, M7). Before this task, every test hand-built a `Principal{id, tenant_id}`
// aggregate ad hoc, with no `kind`, no delegation concept, and no shared admission rule anywhere in
// the tree. This file proves `trust/principal.hpp`'s new mechanism standalone, independent of
// `AgentSession`: the two inbound-identity factories tag the right `principal_kind`; `derive_on_behalf_of`
// (007 §2's "never as the host and never with elevated claims") preserves tenant, forces `kind=agent`,
// and depth-bounds the chain (018 §2's "delegation chains are recorded and depth-bounded"); and
// `principal_admitted_for` (the shared ownership predicate Phase H2/Phase I both reuse) accepts an
// exact match or a single-hop on_behalf_of match, and rejects everything else, including a
// same-id-different-tenant collision and a forged on_behalf_of claim.

#include <iostream>
#include <string>

#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

} // namespace

int main() {
    // --- Factories tag the right kind (018 §1's table) ------------------------------------------
    {
        ae::Principal const embedded = ae::make_embedded_principal("svc-1", "tenant-a");
        AE_CHECK(embedded.kind == ae::principal_kind::service,
                 "H1-R1: an embedded/in-process principal is kind=service (018 §1: 'the host is trusted')");
        AE_CHECK(embedded.id == "svc-1" && embedded.tenant_id == "tenant-a",
                 "H1-R2: make_embedded_principal preserves the supplied id/tenant verbatim");

        ae::Principal const cli = ae::make_local_cli_principal("alice");
        AE_CHECK(cli.kind == ae::principal_kind::human,
                 "H1-R3: a local CLI principal is kind=human (018 §1: 'local user identity')");

        ae::Principal const anon1 = ae::make_anonymous_principal("tenant-a");
        ae::Principal const anon2 = ae::make_anonymous_principal("tenant-b");
        AE_CHECK(anon1.kind == ae::principal_kind::anonymous,
                 "H1-R4: anonymous_principal is kind=anonymous, a real principal, not a bypass (018 §1)");
        AE_CHECK(anon1.id == anon2.id,
                 "H1-R5: anonymous principals share a stable id across tenants -- the id space they "
                 "occupy is fixed and distinguishable from a caller-forged 'anonymous' identity");
    }

    // --- derive_on_behalf_of: never elevated, tenant preserved, depth tracked -------------------
    {
        ae::Principal const parent = ae::make_local_cli_principal("alice", "tenant-a");
        auto derived = ae::derive_on_behalf_of(parent, "sub-agent-1");
        AE_CHECK(derived.has_value(), "H1-R6: deriving a first-hop delegated principal succeeds");
        AE_CHECK(derived->id == "sub-agent-1", "H1-R7: the derived principal gets its OWN id, not the parent's");
        AE_CHECK(derived->on_behalf_of == "alice",
                 "H1-R8: on_behalf_of names exactly the parent -- 007 §2's real delegation expression");
        AE_CHECK(derived->tenant_id == "tenant-a",
                 "H1-R9: tenant_id is copied unchanged -- a delegated call can never cross a tenant "
                 "boundary its parent didn't already occupy (018 §6)");
        AE_CHECK(derived->kind == ae::principal_kind::agent,
                 "H1-R10: a derived principal is always kind=agent, even though its parent was "
                 "kind=human -- never re-labeled as the more-trusted parent kind (never elevated claims)");
        AE_CHECK(derived->delegation_depth == 1, "H1-R11: depth is exactly one hop from a root parent");

        // Chain a second hop to prove depth accumulates, not resets.
        auto grandchild = ae::derive_on_behalf_of(*derived, "sub-agent-2");
        AE_CHECK(grandchild.has_value(), "H1-R12: a second delegation hop succeeds");
        AE_CHECK(grandchild->delegation_depth == 2, "H1-R13: depth accumulates across chained delegation");
        AE_CHECK(grandchild->on_behalf_of == "sub-agent-1",
                 "H1-R14: on_behalf_of names the IMMEDIATE parent at each hop, not the chain root");
    }

    // --- derive_on_behalf_of fails closed past the depth bound -----------------------------------
    {
        ae::Principal chain = ae::make_embedded_principal("root", "tenant-a");
        for (std::uint32_t i = 0; i < ae::kMaxDelegationDepth; ++i) {
            auto next = ae::derive_on_behalf_of(chain, "hop-" + std::to_string(i));
            AE_CHECK(next.has_value(), "H1-R15: hop " + std::to_string(i) + " within the bound succeeds");
            chain = *next;
        }
        AE_CHECK(chain.delegation_depth == ae::kMaxDelegationDepth,
                 "H1-R16: the chain reached exactly the configured maximum depth");

        auto over_bound = ae::derive_on_behalf_of(chain, "one-hop-too-many");
        AE_CHECK(!over_bound.has_value(),
                 "H1-R17: deriving past kMaxDelegationDepth fails closed (018 §2: 'depth-bounded')");
        AE_CHECK(over_bound.error().klass == ae::failure_class::policy,
                 "H1-R18: the depth-exceeded failure is classified as a policy denial (007's own "
                 "attenuation-failure family), not a contract/fatal error");
    }

    // --- principal_admitted_for: the shared ownership rule (Phase H2/Phase I reuse this) --------
    {
        ae::Principal const owner = ae::make_embedded_principal("p-owner", "tenant-a");

        AE_CHECK(ae::principal_admitted_for(owner, owner),
                 "H1-R19: a principal is admitted for itself (exact-match ownership)");

        ae::Principal const stranger = ae::make_embedded_principal("p-stranger", "tenant-a");
        AE_CHECK(!ae::principal_admitted_for(stranger, owner),
                 "H1-R20: an unrelated principal (same tenant, different id, no delegation) is not admitted");

        ae::Principal const same_id_other_tenant = ae::make_embedded_principal("p-owner", "tenant-b");
        AE_CHECK(!ae::principal_admitted_for(same_id_other_tenant, owner),
                 "H1-R21: an id match across a DIFFERENT tenant is not ownership (018 §6) -- checked "
                 "even before the id comparison runs");

        auto delegated = ae::derive_on_behalf_of(owner, "sub-agent-1");
        AE_CHECK(delegated.has_value() && ae::principal_admitted_for(*delegated, owner),
                 "H1-R22: a principal properly derived on_behalf_of the owner IS admitted");

        auto forged = ae::derive_on_behalf_of(stranger, "sub-agent-2");
        AE_CHECK(forged.has_value() && !ae::principal_admitted_for(*forged, owner),
                 "H1-R23: on_behalf_of naming someone OTHER than the owner is still denied -- "
                 "on_behalf_of is not itself a bypass");

        auto two_hop = ae::derive_on_behalf_of(*delegated, "sub-agent-3");
        AE_CHECK(two_hop.has_value() && !ae::principal_admitted_for(*two_hop, owner),
                 "H1-R24: a SECOND-hop delegated principal (on_behalf_of the first hop, not the "
                 "owner directly) is NOT admitted -- principal_admitted_for is single-hop only, "
                 "matching on_behalf_of's own 'immediate parent, not full chain' scope");
    }

    if (g_failures == 0) {
        std::cout << "OK: all principal delegation/admission checks passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
