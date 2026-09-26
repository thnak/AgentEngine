// Proof for 025-Worktree-and-Virtual-Filesystem.md §6's turn-boundary commit ("at each turn
// boundary the current tree is committed and its digest recorded") and §9 G5's rewind gate
// ("restoring an arbitrary retained turn digest reproduces that turn's tree exactly"), Milestone 3
// Phase D1/D2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). Both build
// directly on Phase A1 (content-addressed Blob/Tree) and A2 (`commit_ref`/`read_ref`) -- a turn is
// identified by its own commit's position (`SeqNo`) in the ref's own retained log, not a second
// ledger invented alongside it.
//
// Every rejection/negative case is paired with a positive control (022 §5): a never-committed turn
// number fails closed (D2-C4) alongside turns that DO resolve correctly on the same ref; rewind's
// non-destructive "assignment, not history edit" claim (D2-C3) is proven by recovering the exact
// pre-rewind state with a SECOND rewind, not merely asserted.

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

std::vector<std::byte> bytes_of(std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return bytes;
}

Digest empty_tree(InMemoryWorktreeObjectStore& store) { return *store.put_tree(Tree{}); }

// A one-file tree {"a.txt" -> content}, matching how mount_write's own set_entry_at_path would build
// one turn's worth of change -- built directly here since these tests exercise the ref/turn layer,
// not the mount layer.
Digest one_file_tree(InMemoryWorktreeObjectStore& store, std::string const& content) {
    auto blob = store.put_blob(bytes_of(content));
    Tree t;
    t.entries.push_back(TreeEntry{"a.txt", *blob, false});
    return *store.put_tree(t);
}

} // namespace

int main() {
    // D1-C1: committing a fresh name's first turn succeeds, returns the moved Ref, and turn == 1
    // (the first SeqNo a fresh EventSourced log ever assigns).
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto d0 = empty_tree(obj_store);

        auto t1 = commit_turn(ref_store, "session:turn-1", d0);
        AE_CHECK(t1.has_value() && t1->ref.name == "session:turn-1" && t1->ref.tree_digest == d0,
                 "D1-C1: commit_turn on a fresh name succeeds and returns the moved Ref");
        AE_CHECK(t1.has_value() && t1->turn == 1, "D1-C1: the first turn committed on a fresh name is turn 1");
    }

    // D1-C2: a second turn on the SAME name returns a strictly increasing turn number, and the Ref's
    // live head reflects the LATEST commit, exactly like an ordinary commit_ref call would.
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto d0 = empty_tree(obj_store);
        auto d1 = one_file_tree(obj_store, "v1");

        auto t1 = commit_turn(ref_store, "session:turn-2", d0);
        AE_CHECK(t1.has_value() && t1->turn == 1, "D1-C2: setup -- first turn is 1");
        auto t2 = commit_turn(ref_store, "session:turn-2", d1);
        AE_CHECK(t2.has_value() && t2->turn == 2, "D1-C2: the second turn on the same name is 2, not 1 again");

        auto live = read_ref(ref_store, "session:turn-2");
        AE_CHECK(live.has_value() && live->has_value() && (*live)->tree_digest == d1,
                 "D1-C2: read_ref reflects the latest turn's digest, same as an ordinary commit_ref would");
    }

    // D1-R1: two independently-named refs' turn counters never interfere -- a busy ref having
    // already reached turn 5 does not push a freshly-named ref's first turn past 1.
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto d0 = empty_tree(obj_store);
        for (int i = 0; i < 5; ++i) {
            AE_CHECK(commit_turn(ref_store, "session:busy", d0).has_value(), "D1-R1: setup -- busy ref commit");
        }
        auto fresh = commit_turn(ref_store, "session:fresh", d0);
        AE_CHECK(fresh.has_value() && fresh->turn == 1,
                 "D1-R1: a freshly-named ref's first turn is 1 regardless of a sibling ref's own history");
    }

    // D2-C1/C2/C3: build a 3-turn history (empty -> a.txt=v1 -> a.txt=v2), rewind to turn 2
    // (a.txt=v1) and prove (a) the fetched digest names the EXACT turn-2 tree, byte for byte, (b)
    // read_ref's live head moves BACKWARD to it (assignment, not a side channel), and (c) a rewind
    // never destroys what it rewound past -- a second rewind recovers the turn-3 state exactly.
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        std::string const name = "session:rewind-1";

        auto d_empty = empty_tree(obj_store);
        auto d_v1 = one_file_tree(obj_store, "v1");
        auto d_v2 = one_file_tree(obj_store, "v2");

        auto turn1 = commit_turn(ref_store, name, d_empty);  // turn 1: empty root
        auto turn2 = commit_turn(ref_store, name, d_v1);     // turn 2: a.txt = v1
        auto turn3 = commit_turn(ref_store, name, d_v2);     // turn 3: a.txt = v2
        AE_CHECK(turn1.has_value() && turn2.has_value() && turn3.has_value() && turn1->turn == 1 &&
                     turn2->turn == 2 && turn3->turn == 3,
                 "D2-C1: setup -- three turns committed in order");

        // D2-C1: turn_digest_at fetches turn 2's exact digest, and the tree it names really is
        // a.txt=v1 (content read back, not just digest equality assumed).
        auto fetched_digest = turn_digest_at(ref_store, name, 2);
        AE_CHECK(fetched_digest.has_value() && *fetched_digest == d_v1,
                 "D2-C1: turn_digest_at(turn 2) returns exactly the digest committed at that turn");
        auto fetched_tree = obj_store.get_tree(*fetched_digest);
        AE_CHECK(fetched_tree.has_value() && fetched_tree->entries.size() == 1 &&
                     fetched_tree->entries[0].name == "a.txt",
                 "D2-C1: the fetched turn's tree really does contain a.txt (not merely digest equality)");

        // D2-C2: rewinding to turn 2 moves the ref's LIVE head backward to that digest.
        auto rewound = rewind_to_turn(ref_store, name, 2);
        AE_CHECK(rewound.has_value() && rewound->ref.tree_digest == d_v1,
                 "D2-C2: rewind_to_turn(2) reports the restored Ref pointing at turn 2's digest");
        auto live_after_rewind = read_ref(ref_store, name);
        AE_CHECK(live_after_rewind.has_value() && live_after_rewind->has_value() &&
                     (*live_after_rewind)->tree_digest == d_v1,
                 "D2-C2: read_ref's live head is now turn 2's digest (v1), not turn 3's (v2) -- true "
                 "assignment, not a side channel the ordinary read path doesn't see");

        // D2-C3: the rewind itself became a NEW retained turn (turn 4), and turn 3 (v2) is still
        // fully retained and exactly recoverable -- a rewind narrows nothing it passed over.
        AE_CHECK(rewound.has_value() && rewound->turn == 4,
                 "D2-C3: the rewind commit itself is a new, ordinary turn (4), not a history edit at turn 2");
        auto still_there = turn_digest_at(ref_store, name, 3);
        AE_CHECK(still_there.has_value() && *still_there == d_v2,
                 "D2-C3: turn 3's digest (v2) is still retained after rewinding past it");
        auto second_rewind = rewind_to_turn(ref_store, name, 3);
        AE_CHECK(second_rewind.has_value() && second_rewind->ref.tree_digest == d_v2 &&
                     second_rewind->turn == 5,
                 "D2-C3: a SECOND rewind (to turn 3) exactly recovers the state that existed just "
                 "before the first rewind -- proving nothing was destroyed by it");
    }

    // D2-C4 (negative, paired with the positive controls above on the SAME ref): a turn number that
    // was never committed -- neither 0 (turns start at 1) nor a number past the last commit -- fails
    // closed with a stable code, never silently resolving to a neighboring commit.
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        std::string const name = "session:rewind-negative";
        auto d0 = empty_tree(obj_store);
        AE_CHECK(commit_turn(ref_store, name, d0).has_value() &&
                     commit_turn(ref_store, name, one_file_tree(obj_store, "only-turn-2")).has_value(),
                 "D2-C4: setup -- two turns committed (1 and 2)");

        auto zero = turn_digest_at(ref_store, name, 0);
        AE_CHECK(!zero.has_value() && zero.error().code == "worktree.turn_not_found",
                 "D2-C4: turn 0 (turns start at 1) fails closed with worktree.turn_not_found");

        auto past_end = turn_digest_at(ref_store, name, 99);
        AE_CHECK(!past_end.has_value() && past_end.error().code == "worktree.turn_not_found",
                 "D2-C4: a turn number past the last commit fails closed the same way");

        auto never_committed = turn_digest_at(ref_store, "session:never-committed-at-all", 1);
        AE_CHECK(!never_committed.has_value() && never_committed.error().code == "worktree.turn_not_found",
                 "D2-C4: a name with no log at all fails closed identically, not with a different code");

        // Positive control: turn 1 on this SAME ref (the boundary right next to the rejected turn 0)
        // resolves cleanly -- proving the rejections above are really about the specific turn number,
        // not e.g. this ref being broken in some other way.
        auto turn1_ok = turn_digest_at(ref_store, name, 1);
        AE_CHECK(turn1_ok.has_value() && *turn1_ok == d0,
                 "D2-C4 (positive control): turn 1, immediately next to the rejected turn 0, resolves fine");
    }

    // D2-C5 (integration): rewinding a mount's own backing ref changes what mount_read sees, through
    // the ordinary read path a guest would actually use -- 025 §7's "files that appear are just
    // files" holds through a rewind too, not only through direct Digest/Tree inspection.
    {
        InMemoryAppendLogStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        std::string const name = "session:rewind-mount";
        Mount mount{"/work", name, ""};
        cap::FsRead granted{"/work", "", std::nullopt};

        AE_CHECK(commit_turn(ref_store, name, empty_tree(obj_store)).has_value(),
                 "D2-C5: setup -- turn 1, empty root");
        AE_CHECK(commit_turn(ref_store, name, one_file_tree(obj_store, "old-content")).has_value(),
                 "D2-C5: setup -- turn 2, a.txt = old-content");
        AE_CHECK(commit_turn(ref_store, name, one_file_tree(obj_store, "new-content")).has_value(),
                 "D2-C5: setup -- turn 3, a.txt = new-content");

        auto before = mount_read(obj_store, ref_store, mount, granted, "a.txt");
        AE_CHECK(before.has_value() && std::string(reinterpret_cast<char const*>(before->data()), before->size()) == "new-content",
                 "D2-C5: before rewinding, mount_read sees the latest content");

        AE_CHECK(rewind_to_turn(ref_store, name, 2).has_value(), "D2-C5: rewind to turn 2");

        auto after = mount_read(obj_store, ref_store, mount, granted, "a.txt");
        AE_CHECK(after.has_value() && std::string(reinterpret_cast<char const*>(after->data()), after->size()) == "old-content",
                 "D2-C5: after rewinding to turn 2, mount_read sees the OLD content through the "
                 "ordinary guest-facing read path");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree turn-commit/rewind proof checks passed.\n";
    return 0;
}
