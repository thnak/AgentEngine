// Positive control for the Milestone 3 Phase A3 compile-fail proof (025-Worktree-and-Virtual-
// Filesystem.md §2) -- this file MUST compile (see tests/CMakeLists.txt). Without this, the
// companion worktree_object_store_rejects_{blob,tree}_digest_override.cpp files failing to compile
// would be meaningless: it could be failing because core/worktree.hpp doesn't compile at all, not
// because the specific explicit-digest overload each one names is correctly absent. Exercises the
// real, one-argument `put_blob`/`put_tree` signatures that DO exist.

#include <cstddef>
#include <span>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;

int main() {
    InMemoryWorktreeObjectStore store;
    std::span<std::byte const> const bytes{};
    auto blob_digest = store.put_blob(bytes);  // ok: the real, digest-derived signature
    Tree const tree{};
    auto tree_digest = store.put_tree(tree);   // ok: the real, digest-derived signature
    return (blob_digest.has_value() && tree_digest.has_value()) ? 0 : 1;
}
