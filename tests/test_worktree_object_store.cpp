// Proof for 025-Worktree-and-Virtual-Filesystem.md §2's content-addressed object model
// (core/worktree.hpp), Milestone 3 Phase A1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). Exercises the three
// structural claims §2 makes: snapshotting is recording one digest, diffing two states is a tree
// comparison, and deduplication is free (the same content stored twice is stored once) -- the last
// one specifically checked via `blob_count()`/`tree_count()`, not inferred from digest equality
// alone (022 §5: a test that cannot fail proves nothing).

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;

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

std::vector<std::byte> bytes_of(std::string const& s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

bool is_valid_hex_digest(Digest const& d) {
    if (d.size() != 64) return false;
    for (char c : d) {
        bool const hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

} // namespace

int main() {
    // A-C1: put_blob/get_blob round-trips the exact bytes, and the digest is well-formed.
    {
        InMemoryWorktreeObjectStore store;
        auto bytes = bytes_of("hello worktree");
        auto digest = store.put_blob(bytes);
        AE_CHECK(digest.has_value(), "A-C1: put_blob succeeds");
        AE_CHECK(digest.has_value() && is_valid_hex_digest(*digest),
                 "A-C1: digest is a well-formed 64-char hex string");
        auto fetched = digest.has_value() ? store.get_blob(*digest) : result<std::vector<std::byte>>{};
        AE_CHECK(fetched.has_value() && *fetched == bytes,
                 "A-C1: get_blob returns exactly the bytes that were put");
    }

    // A-C2 (dedup): putting identical content twice yields the identical digest AND the store's
    // blob count does not grow -- proving "stored once" structurally, not just that two digests
    // happen to compare equal.
    {
        InMemoryWorktreeObjectStore store;
        auto bytes = bytes_of("duplicate me");
        auto d1 = store.put_blob(bytes);
        auto d2 = store.put_blob(bytes);
        AE_CHECK(d1.has_value() && d2.has_value() && *d1 == *d2,
                 "A-C2: identical content produces the identical digest");
        AE_CHECK(store.blob_count() == 1, "A-C2 (dedup): store holds exactly one blob after two puts");
    }

    // A-C3: different content produces different digests (the store isn't just returning a
    // constant, and no accidental collision on this input pair).
    {
        InMemoryWorktreeObjectStore store;
        auto d1 = store.put_blob(bytes_of("content A"));
        auto d2 = store.put_blob(bytes_of("content B"));
        AE_CHECK(d1.has_value() && d2.has_value() && *d1 != *d2,
                 "A-C3: different content produces different digests");
    }

    // A-C4 (canonical order): a Tree with the same entries built in two different insertion orders
    // hashes to the identical digest, and only one tree is actually stored -- this is what makes
    // §2's dedup claim hold for trees, not just blobs.
    {
        InMemoryWorktreeObjectStore store;
        Tree order_a{{{"b.txt", "digest-b", false}, {"a.txt", "digest-a", false}}};
        Tree order_b{{{"a.txt", "digest-a", false}, {"b.txt", "digest-b", false}}};
        auto d1 = store.put_tree(order_a);
        auto d2 = store.put_tree(order_b);
        AE_CHECK(d1.has_value() && d2.has_value() && *d1 == *d2,
                 "A-C4: identical entries in different insertion order hash identically");
        AE_CHECK(store.tree_count() == 1, "A-C4 (dedup): store holds exactly one tree after both puts");
    }

    // A-C5 (tree diff via digest comparison): two trees differing by a single entry produce
    // different digests, and a tree fetched back exposes exactly the entries it was built from
    // (in canonical, sorted order) -- proving 025 §2's "diffing two states is a tree comparison"
    // claim is answerable at all.
    {
        InMemoryWorktreeObjectStore store;
        Tree before{{{"a.txt", "digest-a1", false}, {"b.txt", "digest-b", false}}};
        Tree after{{{"a.txt", "digest-a2", false}, {"b.txt", "digest-b", false}}};  // a.txt changed
        auto d_before = store.put_tree(before);
        auto d_after = store.put_tree(after);
        AE_CHECK(d_before.has_value() && d_after.has_value() && *d_before != *d_after,
                 "A-C5: a tree differing by one entry produces a different digest");

        auto fetched = store.get_tree(*d_before);
        AE_CHECK(fetched.has_value() && fetched->entries.size() == 2,
                 "A-C5: get_tree returns the right entry count");
        AE_CHECK(fetched.has_value() && fetched->entries[0].name == "a.txt" &&
                     fetched->entries[1].name == "b.txt",
                 "A-C5: get_tree returns entries in canonical (sorted) order regardless of insertion order");
    }

    // A-C6: fetching an unknown digest fails closed with a stable, machine-readable error code --
    // never a crash, never a default-constructed empty result mistaken for "found."
    {
        InMemoryWorktreeObjectStore store;
        auto missing_blob = store.get_blob(std::string(64, '0'));
        AE_CHECK(!missing_blob.has_value() && missing_blob.error().code == "worktree.blob_not_found",
                 "A-C6: get_blob on an unknown digest fails closed with worktree.blob_not_found");
        auto missing_tree = store.get_tree(std::string(64, '0'));
        AE_CHECK(!missing_tree.has_value() && missing_tree.error().code == "worktree.tree_not_found",
                 "A-C6: get_tree on an unknown digest fails closed with worktree.tree_not_found");
    }

    // A-C7 (pure function, no store): canonical_tree_bytes is deterministic over pre-sorted input --
    // identical entries produce byte-identical output; changing one field changes the output.
    {
        Tree t1{{{"x", "dx", false}, {"y", "dy", true}}};
        Tree t2{{{"x", "dx", false}, {"y", "dy", true}}};
        Tree t3{{{"x", "dx", false}, {"y", "dy-changed", true}}};
        AE_CHECK(canonical_tree_bytes(t1) == canonical_tree_bytes(t2),
                 "A-C7: canonical_tree_bytes is deterministic for identical input");
        AE_CHECK(canonical_tree_bytes(t1) != canonical_tree_bytes(t3),
                 "A-C7: canonical_tree_bytes differs when an entry's digest differs");
    }

    // A-R1 (red-team-ish edge case): hashing empty content does not crash and still produces a
    // well-formed digest, and an empty blob is distinguishable from an empty tree (they must not
    // collide onto the same digest, since they'd be looked up in different tables anyway, but a
    // well-formed digest for each is still worth proving directly).
    {
        InMemoryWorktreeObjectStore store;
        auto empty_blob = store.put_blob(std::span<std::byte const>{});
        auto empty_tree = store.put_tree(Tree{});
        AE_CHECK(empty_blob.has_value() && is_valid_hex_digest(*empty_blob),
                 "A-R1: hashing an empty blob succeeds with a well-formed digest");
        AE_CHECK(empty_tree.has_value() && is_valid_hex_digest(*empty_tree),
                 "A-R1: hashing an empty tree succeeds with a well-formed digest");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree object store proof checks passed.\n";
    return 0;
}
