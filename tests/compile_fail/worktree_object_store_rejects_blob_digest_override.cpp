// This file MUST NOT compile (025-Worktree-and-Virtual-Filesystem.md §2, Milestone 3 Phase A3) --
// see tests/CMakeLists.txt's try_compile() gate. `InMemoryWorktreeObjectStore::put_blob` (core/
// worktree.hpp) takes ONLY the content -- `put_blob(bytes) -> digest` -- and the digest is always
// DERIVED from that content by `compute_digest`, never accepted as a caller-supplied parameter.
// This is what makes 025 §2's "Blob = immutable bytes, addressed by digest" true by construction,
// not by convention: there is no entry point through which a caller could bind arbitrary content
// to a digest of their own choosing, which is exactly what a two-argument
// `put_blob(digest, bytes)` overload would reopen. Calling it with an explicit digest must
// therefore fail to compile, not silently accept a shape the store's real API never declared.

#include <cstddef>
#include <span>
#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;

int main() {
    InMemoryWorktreeObjectStore store;
    std::string const forced_digest(64, '0');
    std::span<std::byte const> const bytes{};
    auto put = store.put_blob(forced_digest, bytes);  // must not compile: no such overload exists
    return put.has_value() ? 0 : 1;
}
