// PROVE-PHASE POSITIVE PROBE for identity_authority.hpp.
// Must compile AND run to completion with exit code 0. Each check aborts (via std::abort through
// the CHECK macro) with a distinct message on failure, so a real failure is immediately locatable.

#include "identity_authority.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    // 1. bootstrap() is a genuine singleton -- callable any number of times, same instance.
    IdentityAuthority& a1 = IdentityAuthority::bootstrap();
    IdentityAuthority& a2 = IdentityAuthority::bootstrap();
    CHECK(&a1 == &a2);
    std::printf("[1] bootstrap() singularity: PASS (repeated calls return the same instance)\n");

    // 2. mint_root() produces a real Principal, known to the authority.
    Principal root = a1.mint_root("root");
    CHECK(a1.is_known(root.id()));
    std::printf("[2] mint_root(): PASS (id=%llu known=%d)\n",
                (unsigned long long)root.id(), (int)a1.is_known(root.id()));

    // 3. Direct child: derive_child() records real ancestry.
    Principal child = a1.derive_child(root, "child");
    CHECK(a1.is_ancestor_of(root.id(), child.id()));
    CHECK(!a1.is_ancestor_of(child.id(), root.id()));  // one-directional, not symmetric
    std::printf("[3] direct child ancestry: PASS\n");

    // 4. MULTI-HOP: grandchild via child -- round 1's Finding 3 (single-hop parent_id couldn't
    // answer this). This is the specific property the original Revision-1 Principal type could NOT
    // provide (it stored only one hop). Real check, not asserted.
    Principal grandchild = a1.derive_child(child, "grandchild");
    CHECK(a1.is_ancestor_of(root.id(), grandchild.id()));       // root -> child -> grandchild
    CHECK(a1.is_ancestor_of(child.id(), grandchild.id()));      // child -> grandchild
    CHECK(!a1.is_ancestor_of(grandchild.id(), root.id()));      // reverse direction still false
    std::printf("[4] MULTI-HOP grandchild ancestry: PASS (this is the property Revision 1's "
                "single-parent_id Principal could NOT provide)\n");

    // 5. Three hops (great-grandchild) -- confirm the walk isn't hardcoded to 1-2 levels.
    Principal ggc = a1.derive_child(grandchild, "great-grandchild");
    CHECK(a1.is_ancestor_of(root.id(), ggc.id()));
    CHECK(a1.is_ancestor_of(grandchild.id(), ggc.id()));
    CHECK(!a1.is_ancestor_of(ggc.id(), grandchild.id()));
    std::printf("[5] three-hop ancestry: PASS\n");

    // 6. A sibling is NOT an ancestor of another sibling (no false widening across branches).
    Principal sibling = a1.derive_child(root, "sibling");
    CHECK(!a1.is_ancestor_of(sibling.id(), grandchild.id()));
    CHECK(!a1.is_ancestor_of(grandchild.id(), sibling.id()));
    std::printf("[6] sibling non-ancestry: PASS\n");

    // 7. is_known() is false for an id that was never minted.
    CHECK(!a1.is_known(999999));
    std::printf("[7] is_known() rejects an unminted id: PASS\n");

    // 8. mint_grant()/authorized() -- the real end-to-end path. `sibling` (a direct child of root,
    // check #6/#7's namesake) is deliberately NOT used as the negative case here: it IS a real
    // descendant of `root` and correctly WOULD be authorized -- an earlier version of this probe
    // asserted the opposite and the real compiler+runtime run caught that as a genuine test-authoring
    // mistake, not an implementation defect. The real negative case needs a principal with NO
    // ancestry relationship to `root` at all -- a second, independent root.
    Principal stranger = a1.mint_root("stranger");
    CHECK(!a1.is_ancestor_of(root.id(), stranger.id()));  // sanity: genuinely unrelated

    struct RollbackAuthority { std::uint32_t max_turns_back; };
    Grant<RollbackAuthority> grant = a1.mint_grant(RollbackAuthority{5}, /*issued_to=*/root,
                                                     /*issued_by=*/root);
    CHECK(authorized(grant, root, RollbackAuthority{5}));         // issued_to itself: authorized
    CHECK(authorized(grant, child, RollbackAuthority{5}));        // descendant: authorized
    CHECK(authorized(grant, grandchild, RollbackAuthority{5}));   // 2-hop descendant: authorized
    CHECK(authorized(grant, ggc, RollbackAuthority{5}));          // 3-hop descendant: authorized
    CHECK(authorized(grant, sibling, RollbackAuthority{5}));      // ALSO a real child of root:
                                                                    // correctly authorized (fixed
                                                                    // expectation)
    CHECK(!authorized(grant, stranger, RollbackAuthority{5}));    // genuinely unrelated: NOT authorized
    std::printf("[8] authorized() end-to-end (incl. multi-hop descendant inheritance, and correctly "
                "rejecting a genuinely unrelated principal): PASS\n");

    // 9. Grant<T> read accessors work without needing friendship at the call site (17.1's fix).
    CHECK(grant.issued_to_id() == root.id());
    CHECK(grant.issued_by_id() == root.id());
    CHECK(grant.payload().max_turns_back == 5);
    std::printf("[9] Grant<T> public read accessors: PASS\n");

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
