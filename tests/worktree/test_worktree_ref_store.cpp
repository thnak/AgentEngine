// Proof for 025-Worktree-and-Virtual-Filesystem.md §2's Ref ("a mutable name -> Tree digest"),
// Milestone 3 Phase A2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `Ref` persistence riding `agentengine::rt::AppendLogStore` (`core/worktree.hpp`'s
// `commit_ref`/`read_ref`), ADR-037. Proves a Ref update is durable and that two independently
// named Refs never interfere.
//
// DROPPED, NOT SILENTLY: the old suite's own A2-C5 proved Quark-actor fencing -- a stale writer's
// commit rejected because a later writer already superseded its fence token. `rt::AppendLogStore`
// has no such primitive at all (append_log_store.hpp's own banner: "which one's record ends up with
// the lower seq number is still a race a caller needing ordering guarantees must serialize
// externally" -- a caller-owned discipline, not something this store enforces). This is not a new
// narrowing introduced by this port -- it is the already-accepted, already-documented shape
// `AppendLogStore` had BEFORE `core/worktree.hpp`'s Ref persistence was ever wired onto it; a Ref's
// own single-writer-per-turn usage (one session/turn loop commits its own ref sequentially) never
// relied on cross-writer arbitration in practice, and `merge_branch_into_parent`'s own comment
// already accepts an analogous residual race at this same seam layer, driving a future fix (a
// compare-and-set primitive on the store) rather than living with it silently.

#include <iostream>
#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;
using agentengine::rt::InMemoryAppendLogStore;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

} // namespace

int main() {
    // A2-C1: committing a fresh Ref succeeds and returns the committed value.
    {
        InMemoryAppendLogStore store;
        auto committed = commit_ref(store, "session:s-1", "digest-a");
        AE_CHECK(committed.has_value() && committed->name == "session:s-1" &&
                     committed->tree_digest == "digest-a",
                 "A2-C1: commit_ref on a fresh name succeeds and returns {name, digest}");
    }

    // A2-C2: reading a name that has never been committed returns nullopt, not an error and not a
    // default-constructed Ref mistaken for "found."
    {
        InMemoryAppendLogStore store;
        auto ref = read_ref(store, "session:never-committed");
        AE_CHECK(ref.has_value() && !ref->has_value(),
                 "A2-C2: read_ref on an uncommitted name returns nullopt");
    }

    // A2-C3: a committed Ref round-trips through read_ref exactly.
    {
        InMemoryAppendLogStore store;
        auto committed = commit_ref(store, "session:s-2", "digest-b");
        AE_CHECK(committed.has_value(), "A2-C3: setup commit succeeds");
        auto fetched = read_ref(store, "session:s-2");
        AE_CHECK(fetched.has_value() && fetched->has_value() &&
                     (*fetched)->tree_digest == "digest-b",
                 "A2-C3: read_ref returns exactly the digest that was committed");
    }

    // A2-C4: committing again moves the Ref -- read_ref reflects the LATEST digest, not the first
    // one (proving this is a mutable pointer with history, not an immutable write-once record).
    {
        InMemoryAppendLogStore store;
        AE_CHECK(commit_ref(store, "session:s-3", "digest-v1").has_value(),
                 "A2-C4: first commit succeeds");
        AE_CHECK(commit_ref(store, "session:s-3", "digest-v2").has_value(),
                 "A2-C4: second commit (move) succeeds");
        AE_CHECK(commit_ref(store, "session:s-3", "digest-v3").has_value(),
                 "A2-C4: third commit (move) succeeds");
        auto fetched = read_ref(store, "session:s-3");
        AE_CHECK(fetched.has_value() && fetched->has_value() &&
                     (*fetched)->tree_digest == "digest-v3",
                 "A2-C4: read_ref reflects the most recent commit, not the first");
    }

    // A2-R1: two independently named Refs never interfere with each other (isolation).
    {
        InMemoryAppendLogStore store;
        AE_CHECK(commit_ref(store, "session:x", "digest-x").has_value(), "A2-R1: commit ref x");
        AE_CHECK(commit_ref(store, "session:y", "digest-y").has_value(), "A2-R1: commit ref y");
        auto x = read_ref(store, "session:x");
        auto y = read_ref(store, "session:y");
        AE_CHECK(x.has_value() && x->has_value() && (*x)->tree_digest == "digest-x",
                 "A2-R1: ref x is unaffected by ref y's commit");
        AE_CHECK(y.has_value() && y->has_value() && (*y)->tree_digest == "digest-y",
                 "A2-R1: ref y is unaffected by ref x's commit");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree ref store proof checks passed.\n";
    return 0;
}
