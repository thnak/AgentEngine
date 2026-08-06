// This file MUST NOT compile (025-Worktree-and-Virtual-Filesystem.md §2, Milestone 3 Phase A3) --
// see tests/CMakeLists.txt's try_compile() gate. Same claim as the companion
// worktree_object_store_rejects_blob_digest_override.cpp, proven independently for `put_tree`
// rather than assumed to follow from `put_blob`'s own proof: `InMemoryWorktreeObjectStore::
// put_tree` (core/worktree.hpp) takes ONLY the `Tree` value -- `put_tree(tree) -> digest` -- and
// the digest is always derived from the tree's own canonical serialization, never accepted as a
// caller-supplied parameter. A two-argument `put_tree(digest, tree)` overload would let a caller
// rebind an existing digest to different entries, defeating 025 §2's immutability claim for trees
// the same way a `put_blob(digest, bytes)` overload would for blobs -- and must fail to compile.

#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;

int main() {
    InMemoryWorktreeObjectStore store;
    std::string const forced_digest(64, '0');
    Tree const tree{};
    auto put = store.put_tree(forced_digest, tree);  // must not compile: no such overload exists
    return put.has_value() ? 0 : 1;
}
