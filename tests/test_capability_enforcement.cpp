// Design -> red-team -> prove -> judge for decisions/ADR-009-capability-set-enforcement-mechanism.md
// (trust/capability.hpp): the in-process `Capability`/`CapabilitySet` enforcement mechanism that
// upholds 007-Capability-and-Trust-Model.md §3's five properties. Miniature versions of 007 §9's
// G1 (no ambient authority), G3 (attenuation only), and G4 (revocation) — this milestone's surface,
// not the full fuzzed/randomized-workload gates a later pass owns. C4/R-C3's per-axis widening
// sweep and C5/R-C5's stashed-copy revocation test are the positive-control-bearing claims (022 §5:
// "a test that cannot fail proves nothing" — every negative-result claim below is paired with a
// case that DOES succeed, proving the check itself is live).

#include <iostream>
#include <string>
#include <type_traits>

#include "agentengine/trust/capability.hpp"

using namespace agentengine;

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

// Structural claim (part of C6): CapabilitySet is not an aggregate -- there is no brace-init
// shortcut around grant_root(), even setting private members aside. Fails the BUILD, not a test
// run, if this property is ever accidentally lost (e.g. by removing the user-declared default
// constructor).
static_assert(!std::is_aggregate_v<CapabilitySet>,
              "CapabilitySet must not be an aggregate -- grant_root() must be the only construction path");

}  // namespace

int main() {
    // ---- C1: empty by default ----------------------------------------------------------------
    {
        CapabilitySet empty;
        AE_CHECK(empty.size() == 0, "C1: default-constructed CapabilitySet is empty");
        AE_CHECK(!empty.contains(cap::FsRead{"workspace", "", std::nullopt}),
                  "C1: an empty set contains nothing");
        AE_CHECK(!empty.contains_kind(capability_kind::clock),
                  "C1: an empty set's kind-only check also finds nothing");
    }

    // ---- C2: attenuation -- narrowing succeeds -------------------------------------------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::FsRead{"workspace", "", std::nullopt}},
        });
        auto narrower = root.attenuate({Capability{cap::FsRead{"workspace", "/reports", 1024}}});
        AE_CHECK(narrower.has_value(), "C2: narrowing to a subtree+cap under the parent succeeds");
        if (narrower.has_value()) {
            AE_CHECK(narrower->contains(cap::FsRead{"workspace", "/reports", 512}),
                      "C2: the derived set actually reflects the narrower request");
            AE_CHECK(!narrower->contains(cap::FsRead{"workspace", "", std::nullopt}),
                      "C2: the derived set does NOT still carry the parent's wider grant");
        }
    }

    // ---- C3 / R-C3: attenuation -- every widening axis is rejected, not just one ---------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::FsRead{"workspace", "/reports", 1000}},
        });

        auto wider_mount   = root.attenuate({Capability{cap::FsRead{"secrets", "/reports", 1000}}});
        auto wider_prefix   = root.attenuate({Capability{cap::FsRead{"workspace", "", 1000}}});
        auto wider_segment  = root.attenuate({Capability{cap::FsRead{"workspace", "/repo", 1000}}}); // "/repo" is NOT a prefix-boundary subset of "/reports"
        auto wider_cap      = root.attenuate({Capability{cap::FsRead{"workspace", "/reports", 2000}}});
        auto uncapped       = root.attenuate({Capability{cap::FsRead{"workspace", "/reports", std::nullopt}}});
        auto kind_swap      = root.attenuate({Capability{cap::FsWrite{"workspace", "/reports", std::nullopt, std::nullopt}}});

        AE_CHECK(!wider_mount.has_value(),  "R-C3: attenuation to a different mount is rejected");
        AE_CHECK(!wider_prefix.has_value(), "R-C3: attenuation to a broader path prefix is rejected");
        AE_CHECK(!wider_segment.has_value(),"R-C3: a non-prefix sibling path is rejected");
        AE_CHECK(!wider_cap.has_value(),    "R-C3: a higher byte cap is rejected");
        AE_CHECK(!uncapped.has_value(),     "R-C3: claiming 'uncapped' against a capped parent is rejected");
        AE_CHECK(!kind_swap.has_value(),    "R-C3: swapping FsRead for FsWrite (same mount) is rejected");

        // Positive control: a request identical to the parent still succeeds -- proves the six
        // rejections above are the subsumption check actually firing, not "attenuate always fails".
        auto identical = root.attenuate({Capability{cap::FsRead{"workspace", "/reports", 1000}}});
        AE_CHECK(identical.has_value(), "R-C3 (positive control): an exact-match request is accepted");
    }

    // ---- C4: parameterized subsumption is real, not kind-only ----------------------------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::FsRead{"workspace", "", std::nullopt}},
        });
        AE_CHECK(root.contains_kind(capability_kind::fs_read),
                  "C4: kind-only check sees the fs_read grant (both are fs_read)");
        AE_CHECK(!root.contains(cap::FsRead{"secrets", "", std::nullopt}),
                  "C4: real check correctly denies a same-kind request against a different mount "
                  "('secrets' vs 'workspace') -- kind alone is not enough");

        // NetOut host-allowlist subsumption: same kind, different (non-subset) parameter -> denied.
        auto net_root = CapabilitySet::grant_root({
            Capability{cap::NetOut{{"api.search.example:443:https"}, std::nullopt, {}}},
        });
        AE_CHECK(net_root.contains(cap::NetOut{{"api.search.example:443:https"}, std::nullopt, {}}),
                  "C4: NetOut request for exactly the granted host is allowed");
        AE_CHECK(!net_root.contains(cap::NetOut{{"evil.example:443:https"}, std::nullopt, {}}),
                  "C4: NetOut request for a host NOT in the allowlist is denied");
    }

    // ---- C5 / R-C5: per-invocation bind + revoke, including a stashed copy ---------------------
    {
        auto root = CapabilitySet::grant_root({Capability{cap::ToolCall{"web_search"}}});
        auto bound = root.bind(cap::ToolCall{"web_search"});
        AE_CHECK(bound.has_value(), "C5: bind() succeeds for a held capability");

        auto denied = root.bind(cap::ToolCall{"other_tool"});
        AE_CHECK(!denied.has_value(), "C5: bind() fails closed for a capability not held");

        if (bound.has_value()) {
            BoundCapability stashed_copy = *bound;  // simulates a tool implementation squirreling
                                                     // away a copy of its handle in member state.
            AE_CHECK(bound->use().has_value(), "C5: the handle is usable before revocation");
            AE_CHECK(stashed_copy.use().has_value(), "C5: the stashed copy is ALSO usable before revocation");

            bound->revoke();  // 006 §3 step 10, called on the original.

            AE_CHECK(!bound->use().has_value(),
                      "R-C5: the original handle is unusable after revoke() (006 §8 G3)");
            AE_CHECK(!stashed_copy.use().has_value(),
                      "R-C5: the STASHED COPY is also unusable after revoke() -- a copy made before "
                      "revocation does not escape it (the actual point of the shared-ticket design)");

            bound->revoke();  // idempotence: a second revoke (e.g. from an error-handling path)
                               // must not resurrect or crash anything.
            AE_CHECK(!bound->use().has_value(), "C5: revoke() is idempotent");
        }
    }

    // ---- R-C5b: revoking one handle does not affect an unrelated handle ------------------------
    {
        auto root = CapabilitySet::grant_root({Capability{cap::ToolCall{"web_search"}}});
        auto handle_a = root.bind(cap::ToolCall{"web_search"});
        auto handle_b = root.bind(cap::ToolCall{"web_search"});
        AE_CHECK(handle_a.has_value() && handle_b.has_value(), "R-C5b setup: two independent binds succeed");
        if (handle_a.has_value() && handle_b.has_value()) {
            handle_a->revoke();
            AE_CHECK(!handle_a->use().has_value(), "R-C5b: handle_a is revoked");
            AE_CHECK(handle_b->use().has_value(),
                      "R-C5b: handle_b (a DIFFERENT invocation's ticket) is unaffected by handle_a's revoke");
        }
    }

    // ---- C6: no ambient/zero-argument "grant everything" shortcut exists -----------------------
    // Structural, not behavioral: `grant_root` is the only way to produce a non-empty set, and it
    // always requires an explicit, spelled-out capability list -- there is no `CapabilitySet::all()`
    // or similar. The static_assert above (not an aggregate) is the compile-time half of this claim.
    {
        CapabilitySet default_constructed;
        CapabilitySet also_default = CapabilitySet();
        AE_CHECK(default_constructed.size() == 0 && also_default.size() == 0,
                  "C6: every public zero-argument construction path yields an empty set, always");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All capability enforcement (ADR-009) checks passed.\n";
    return 0;
}
