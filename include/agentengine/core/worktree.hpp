#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §2 — a content-addressed object store plus a
// mutable tree. Storage is Quark's Store seam (012); no new storage engine (025 §2).

#include <string>
#include <unordered_map>
#include <variant>

namespace agentengine {

struct Blob {
    std::string digest;  // content address
};

struct Tree; // fwd — recursive: name -> Blob | Tree, addressed by digest

using TreeEntry = std::variant<Blob, Tree*>;

struct Tree {
    std::string                              digest;
    std::unordered_map<std::string, TreeEntry> entries;
};

// A mutable name -> Tree digest (025 §2). What "the worktree" currently is, for one session or one
// principal (029 §2 reuses this exact shape one level up, scoped to a principal instead).
struct Ref {
    std::string name;      // e.g. "session:s-42" or "principal:p-7"
    std::string tree_digest;
};

enum class sharing_mode { shared, branch, readonly, scratch };

} // namespace agentengine
