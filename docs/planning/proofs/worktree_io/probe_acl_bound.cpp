// PROVE-PHASE PROBE (A8/§34): the per-digest ACL root-count bound. Confirms, for real: (1) a
// digest's ACL can accumulate up to `kMaxAclRootsPerDigest` DISTINCT root principals; (2) the
// NEXT distinct root beyond that cap is REJECTED, failing closed (`ledger.acl_root_cap_exceeded`),
// not silently dropped or silently growing; (3) re-touching (put_blob_safe on identical content)
// from a root ALREADY in the ACL is a no-op success even when the set is already at the cap --
// the bound is on DISTINCT roots, not a blanket ceiling on how many times content can be reused;
// (4) every root that WAS successfully admitted before the cap can still read the content
// afterward -- hitting the cap must never silently revoke an existing, legitimate grant.

#include "worktree_ledger.hpp"

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

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Ledger<> ledger;

    std::string const content = "shared content many roots will touch";
    std::vector<std::byte> bytes(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);

    std::vector<Principal> roots;
    agentengine::Digest digest;
    for (std::size_t i = 0; i < Ledger<>::kMaxAclRootsPerDigest; ++i) {
        Principal root = authority.mint_root("acl-bound-root-" + std::to_string(i));
        auto d = ledger.put_blob_safe(bytes, root);
        CHECK(d.has_value());
        digest = *d;
        roots.push_back(root);
    }
    std::printf("[1] admitted exactly %zu distinct root principals into one digest's ACL "
                "(kMaxAclRootsPerDigest): all succeeded\n", roots.size());

    // (2) The NEXT distinct root must be rejected -- fails closed, not silently dropped.
    Principal overflow_root = authority.mint_root("acl-bound-overflow");
    auto overflow = ledger.put_blob_safe(bytes, overflow_root);
    CHECK(!overflow.has_value());
    CHECK(overflow.error().code == "ledger.acl_root_cap_exceeded");
    std::printf("[2] the %llu-th distinct root was REJECTED (%s), not silently admitted or "
                "silently dropped\n", static_cast<unsigned long long>(roots.size() + 1),
                overflow.error().code.c_str());

    // (3) Re-touching from an ALREADY-admitted root, with the set already at the cap, is a no-op
    // success -- the bound is on distinct roots, not a blanket "content is now frozen" ceiling.
    auto retouch = ledger.put_blob_safe(bytes, roots[0]);
    CHECK(retouch.has_value());
    CHECK(*retouch == digest);
    std::printf("[3] re-touching the SAME content from an ALREADY-admitted root (roots[0]), with "
                "the ACL already at the cap, still succeeds (same digest returned) -- PASS\n");

    // (4) Every root admitted BEFORE the cap was hit can still read the content afterward --
    // hitting the cap must never silently revoke an existing grant.
    bool all_can_still_read = true;
    for (auto const& root : roots) {
        auto read = ledger.get_blob_safe(digest, root);
        if (!read.has_value()) { all_can_still_read = false; break; }
    }
    CHECK(all_can_still_read);
    std::printf("[4] every one of the %zu originally-admitted roots can still read the content "
                "after the cap was hit -- the bound rejects NEW growth, it never revokes existing, "
                "already-legitimate access -- PASS\n", roots.size());

    // Sanity: the overflow root, correctly rejected, genuinely cannot read the content either.
    auto overflow_read = ledger.get_blob_safe(digest, overflow_root);
    CHECK(!overflow_read.has_value());
    std::printf("[5] the rejected overflow root correctly cannot read the content it was never "
                "granted access to -- PASS\n");

    std::printf("\nALL CHECKS PASSED -- the per-digest ACL root-count bound (A8/§29.6) is real, "
                "enforced, fails closed on overflow, never silently revokes an existing grant, and "
                "never blocks legitimate re-use of content by an already-admitted root.\n");
    return 0;
}
