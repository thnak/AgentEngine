// PROVE-PHASE PROBE (A8 fix, 2026-08-27, design doc §40): the real, previously-uncovered gap
// §29.6/§34's own "generous, documented-not-tuned default" framing understated -- the per-digest
// ACL cap (`kMaxAclRootsPerDigest`) is not merely untuned, it is a PERMANENT, non-evictable ceiling
// with no escape hatch: once 64 distinct, non-descendant principals have ever touched one digest
// (the realistic driver: many independent sessions forking from an identical, differently-owned
// SHARED base, e.g. a common onboarding template -- `branch_from()`/`merge()`'s own ACL insertions),
// the 65th legitimate session is denied FOREVER, with no tuning knob and no way for a content owner
// to declare "this is meant to be read by anyone." This probe proves the two-part fix: (1) the cap
// is now a real, per-instance constructor parameter, not a compile-time-only constant; (2) an
// already-authorized principal can call `mark_digest_shared()` to exempt a specific digest from the
// cap entirely -- both readability AND future ACL growth -- gated so an UNRELATED principal cannot
// mark someone else's content shared, and confirmed to be a genuine, permanent EXEMPTION (not just
// "allowed to exceed the cap once"), matching this whole ACL mechanism's own "no eviction, no silent
// revocation" posture with a one-way ratchet instead.

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

namespace {
std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> bytes(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) bytes[i] = static_cast<std::byte>(s[i]);
    return bytes;
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();

    // ---- Part 1: the cap is a real, per-instance constructor parameter -----------------------
    {
        Ledger<> small_cap_ledger(agentengine::InMemoryWorktreeObjectStore{}, std::nullopt,
                                    /*max_acl_roots_per_digest=*/2);
        auto content = to_bytes("small-cap content");
        Principal r0 = authority.mint_root("small-cap-root-0");
        Principal r1 = authority.mint_root("small-cap-root-1");
        Principal r2 = authority.mint_root("small-cap-root-2");

        auto d0 = small_cap_ledger.put_blob_safe(content, r0);
        CHECK(d0.has_value());
        auto d1 = small_cap_ledger.put_blob_safe(content, r1);
        CHECK(d1.has_value());
        auto d2 = small_cap_ledger.put_blob_safe(content, r2);
        CHECK(!d2.has_value());
        CHECK(d2.error().code == "ledger.acl_root_cap_exceeded");
        std::printf("[1] a Ledger constructed with max_acl_roots_per_digest=2 admits exactly 2 "
                    "distinct roots and rejects the 3rd (%s) -- the cap is a REAL runtime "
                    "constructor parameter, not just a compile-time constant only a recompile "
                    "could change -- PASS\n", d2.error().code.c_str());
    }

    // ---- Part 2: mark_digest_shared() requires the caller be ALREADY authorized --------------
    Ledger<> ledger;
    Principal owner = authority.mint_root("share-owner");
    Principal stranger = authority.mint_root("share-stranger");
    auto shared_content = to_bytes("this content will be marked publicly shared");
    auto digest = ledger.put_blob_safe(shared_content, owner);
    CHECK(digest.has_value());

    auto unauthorized_mark = ledger.mark_digest_shared(*digest, /*is_tree=*/false, stranger);
    CHECK(!unauthorized_mark.has_value());
    CHECK(unauthorized_mark.error().code == "ledger.mark_shared_unauthorized");
    auto stranger_read_before = ledger.get_blob_safe(*digest, stranger);
    CHECK(!stranger_read_before.has_value());
    std::printf("[2] an UNRELATED principal ('stranger') cannot mark someone else's content shared "
                "(%s), and still cannot read it -- marking shared is gated by the SAME "
                "authorized_for() check every read already uses, never a free-for-all -- PASS\n",
                unauthorized_mark.error().code.c_str());

    // ---- Part 3: the legitimate owner CAN mark it shared, and a stranger can then read it -----
    auto mark = ledger.mark_digest_shared(*digest, /*is_tree=*/false, owner);
    CHECK(mark.has_value());
    auto stranger_read_after = ledger.get_blob_safe(*digest, stranger);
    CHECK(stranger_read_after.has_value());
    std::string const readback(reinterpret_cast<char const*>(stranger_read_after->data()),
                                  stranger_read_after->size());
    CHECK(readback == "this content will be marked publicly shared");
    std::printf("[3] the legitimate owner marked the digest publicly shared; a completely "
                "UNRELATED principal ('stranger', not a descendant of 'owner') can now read the "
                "REAL content back byte-for-byte -- PASS\n");

    // ---- Part 4: the fix for the GROWTH vector, not just the denial -- a publicly-shared digest --
    // is a genuine, permanent EXEMPTION from the cap: many MORE distinct principals can read it,
    // past what the cap would otherwise ever allow, with zero further ACL-set growth. Uses a
    // small-cap Ledger (cap=1) to make this checkable without minting 65+ principals.
    {
        Ledger<> tiny_cap_ledger(agentengine::InMemoryWorktreeObjectStore{}, std::nullopt,
                                   /*max_acl_roots_per_digest=*/1);
        Principal tiny_owner = authority.mint_root("tiny-cap-owner");
        auto tiny_content = to_bytes("tiny-cap shared content");
        auto tiny_digest = tiny_cap_ledger.put_blob_safe(tiny_content, tiny_owner);
        CHECK(tiny_digest.has_value());

        // Cap is already exhausted (1/1) -- a second, different root is normally rejected.
        Principal second = authority.mint_root("tiny-cap-second-before-share");
        auto rejected_before_share = tiny_cap_ledger.put_blob_safe(tiny_content, second);
        CHECK(!rejected_before_share.has_value());
        CHECK(rejected_before_share.error().code == "ledger.acl_root_cap_exceeded");

        auto tiny_mark = tiny_cap_ledger.mark_digest_shared(*tiny_digest, /*is_tree=*/false, tiny_owner);
        CHECK(tiny_mark.has_value());

        // Now, past an ALREADY-exhausted cap, 5 completely new, unrelated principals can all read
        // it -- real proof this is an EXEMPTION (no more growth attempted at all), not merely "the
        // owner's one grant now also lets everyone in via some separate check that still tries and
        // fails to grow the set."
        bool all_new_principals_can_read = true;
        for (int i = 0; i < 5; ++i) {
            Principal newcomer = authority.mint_root("tiny-cap-newcomer-" + std::to_string(i));
            auto read = tiny_cap_ledger.get_blob_safe(*tiny_digest, newcomer);
            if (!read.has_value()) { all_new_principals_can_read = false; break; }
        }
        CHECK(all_new_principals_can_read);

        // And a WRITE from a new principal referencing this same digest (put_blob_safe on
        // identical content) -- the real ACL-insertion path -- is ALSO a genuine no-op success,
        // never re-attempting growth and never hitting the (already-exhausted, cap=1) bound again.
        Principal late_writer = authority.mint_root("tiny-cap-late-writer");
        auto late_write = tiny_cap_ledger.put_blob_safe(tiny_content, late_writer);
        CHECK(late_write.has_value());
        CHECK(*late_write == *tiny_digest);

        std::printf("[4] REAL EXEMPTION CONFIRMED: with max_acl_roots_per_digest=1 already "
                    "exhausted, marking the digest publicly shared lets 5 completely NEW, unrelated "
                    "principals read it AND a 6th successfully write-reference the same digest -- "
                    "the cap is genuinely bypassed for a shared digest, not merely allowed to be "
                    "exceeded once -- PASS\n");
    }

    std::printf("\nALL CHECKS PASSED -- the per-digest ACL cap (A8) is now a real, per-instance "
                "constructor parameter, and an already-authorized principal has a real, "
                "adversarially-gated escape hatch to exempt genuinely shared content from the cap "
                "entirely, closing the real permanent-denial-for-legitimate-use gap this pass "
                "found, not just disclosing it.\n");
    return 0;
}
