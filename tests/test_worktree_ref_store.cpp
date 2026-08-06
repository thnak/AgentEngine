// Proof for 025-Worktree-and-Virtual-Filesystem.md §2's Ref ("a mutable name -> Tree digest"),
// Milestone 3 Phase A2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `Ref` persistence going through Quark's `Store` seam directly (`core/worktree.hpp`'s
// `commit_ref`/`read_ref`), proving a Ref update is durable and fenced the same way any other
// Quark-actor state is (022 §5: the fencing claim specifically is proven by making a stale writer
// fail, not merely by a happy-path round trip succeeding).

#include <iostream>
#include <string>

#include "agentengine/core/worktree.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"

using namespace agentengine;
using quark::EventLog;
using quark::InMemoryStore;

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
        InMemoryStore store;
        auto committed = commit_ref(store, "session:s-1", "digest-a");
        AE_CHECK(committed.has_value() && committed->name == "session:s-1" &&
                     committed->tree_digest == "digest-a",
                 "A2-C1: commit_ref on a fresh name succeeds and returns {name, digest}");
    }

    // A2-C2: reading a name that has never been committed returns nullopt, not an error and not a
    // default-constructed Ref mistaken for "found."
    {
        InMemoryStore store;
        auto ref = read_ref(store, "session:never-committed");
        AE_CHECK(ref.has_value() && !ref->has_value(),
                 "A2-C2: read_ref on an uncommitted name returns nullopt");
    }

    // A2-C3: a committed Ref round-trips through read_ref exactly.
    {
        InMemoryStore store;
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
        InMemoryStore store;
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

    // A2-C5 (fencing): a superseded (stale-token) writer's commit is rejected -- proving a Ref
    // update goes through the identical fencing gate every other Quark-actor state does (012),
    // not something this header's API quietly bypasses. Drives the store directly (below
    // commit_ref's own always-fresh-fence API) to construct the stale-writer scenario at all.
    {
        InMemoryStore store;
        std::string const name = "session:fence-test";
        auto const id = ref_actor_id(name);
        auto const stale_fence = store.acquire_fence(id);   // writer 1 acquires...
        (void)store.acquire_fence(id);                       // ...then writer 2 supersedes it.

        EventLog<RefMoved, InMemoryStore> stale_log(store, id, stale_fence, store.last_seq(id) + 1);
        stale_log.stage(RefMoved{"attempted-by-stale-writer"});
        auto rc = stale_log.commit();
        AE_CHECK(!rc.has_value(),
                 "A2-C5 (fencing): a stale-fenced writer's commit is rejected, not silently applied");

        // Positive control: the CURRENT (non-stale) fence, used the same way, succeeds -- proving
        // the rejection above is really about the fence being stale, not e.g. a broken store.
        auto const current_fence = store.acquire_fence(id);
        EventLog<RefMoved, InMemoryStore> current_log(store, id, current_fence, store.last_seq(id) + 1);
        current_log.stage(RefMoved{"written-by-current-writer"});
        auto rc2 = current_log.commit();
        AE_CHECK(rc2.has_value(),
                 "A2-C5 (positive control): the current (non-stale) fence's commit succeeds");
    }

    // A2-R1: two independently named Refs never interfere with each other (isolation).
    {
        InMemoryStore store;
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
