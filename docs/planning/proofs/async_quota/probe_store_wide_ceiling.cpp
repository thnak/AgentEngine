// PROVE-PHASE PROBE (A5/§34): closes §11's "cross-session/store-wide quota interaction" open
// question. DECISION: a real deployment mints exactly ONE root `AsyncQuota<StorageBytes>` per
// STORE (not per session) -- every session's own quota is obtained via the ALREADY-PROVEN (§21)
// `allocate_child_share()`, not a second independent `mint_root()` call. This needed NO new
// mechanism -- §5 already specifies AsyncQuota<T> as "scoped per Principal subtree," and the store-
// wide ceiling is simply the ROOT of that same subtree, with every session's owning principal
// `derive_child()`'d (or `adopt()`'d with real delegation) from one common store-owning principal.
// What was missing was the DECISION plus a real, concrete demonstration that this actually produces
// a store-wide ceiling, not just per-session independence -- which per-session `mint_root()` calls,
// the historical design's own (rejected) alternative, structurally cannot provide.

#include "async_quota.hpp"

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
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal store_owner = authority.mint_root("store-owner");

    // The ONE store-wide root quota -- 1000 bytes total, for the WHOLE deployment, not per session.
    auto root_quota = AsyncQuota<StorageBytes>::mint_root(authority, store_owner, 1000);
    CHECK(root_quota.has_value());

    Principal session1 = authority.derive_child(store_owner, "session-1");
    Principal session2 = authority.derive_child(store_owner, "session-2");
    Principal session3 = authority.derive_child(store_owner, "session-3");

    auto s1_quota = run(root_quota->allocate_child_share(session1, 400));
    CHECK(s1_quota.has_value());
    auto s2_quota = run(root_quota->allocate_child_share(session2, 400));
    CHECK(s2_quota.has_value());
    std::printf("[1] two sessions each granted a 400-byte child share off the ONE store-wide root "
                "(1000 total): root remaining=%llu (expect 200)\n",
                static_cast<unsigned long long>(root_quota->remaining()));
    CHECK(root_quota->remaining() == 200);

    // THE PROPERTY per-session-independent quotas cannot provide: session3 asking for 300 bytes --
    // well within what ANY single session's own reasonable individual limit might be -- is REJECTED
    // because the STORE-WIDE ceiling (not session3's own limit) is what's actually exhausted.
    auto s3_overreach = run(root_quota->allocate_child_share(session3, 300));
    CHECK(!s3_overreach.has_value());
    std::printf("[2] a THIRD session requesting 300 bytes is REJECTED (%s) even though 300 is a "
                "perfectly reasonable per-session limit on its own -- the STORE-WIDE ceiling, not "
                "any individual session's own limit, is what's actually exhausted. A per-session-"
                "independent mint_root() model (the historical design's own rejected approach) "
                "could never produce this rejection at all.\n", s3_overreach.error().message.c_str());

    // session3 asking for exactly what's left succeeds -- the root is a real, live, shared ledger,
    // not a static per-session cap.
    auto s3_quota = run(root_quota->allocate_child_share(session3, 200));
    CHECK(s3_quota.has_value());
    CHECK(root_quota->remaining() == 0);
    std::printf("[3] session-3 requesting exactly the remaining 200 bytes succeeds; root remaining "
                "is now 0 -- the store-wide ceiling is fully, correctly accounted for across THREE "
                "independent sessions sharing it.\n");

    // Already-granted child shares remain genuinely independent, already-proven (§21) budgets --
    // exhausting the ROOT does not retroactively freeze sessions that already hold a live share.
    auto s1_spend = run(s1_quota->try_consume(400, session1));
    CHECK(s1_spend.has_value());
    auto s2_spend = run(s2_quota->try_consume(400, session2));
    CHECK(s2_spend.has_value());
    auto s3_spend = run(s3_quota->try_consume(200, session3));
    CHECK(s3_spend.has_value());
    std::printf("[4] all three sessions can still fully spend their own already-granted child "
                "shares even though the store-wide root itself is now fully allocated -- a granted "
                "share is a real, independent budget, not re-checked against the root on every use "
                "(§21's own already-proven AsyncQuota semantics, unchanged here) -- PASS\n");

    std::printf("\nALL CHECKS PASSED -- one root AsyncQuota<StorageBytes> per store, sessions "
                "obtaining child shares via the already-proven allocate_child_share(), is a real, "
                "working store-wide ceiling above every session's own subtree -- closing §11's own "
                "\"cross-session/store-wide quota interaction\" open question with the mechanism "
                "this design already had, not a new one.\n");
    return 0;
}
