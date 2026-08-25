#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §2 -- a content-addressed object store plus a
// mutable tree: Blob = immutable bytes addressed by digest, Tree = {name -> Blob|Tree} addressed by
// digest, Ref = a mutable name -> Tree digest. This header is the umbrella entry point: it pulls in
// the full worktree vocabulary (object store, Ref persistence, sub-worktrees, three-way merge,
// tree diffing, and mounts) split across the `worktree_*.hpp` files below, so existing callers that
// `#include "agentengine/core/worktree.hpp"` see the identical symbol set as before the split -- no
// call-site changes required. A caller that only needs one slice (e.g. just the object store) can
// include the matching `worktree_*.hpp` directly instead of pulling in the whole vocabulary.
//
// Digest algorithm: SHA-256, hex-encoded (64 chars) -- the same digest+store vocabulary
// core/content.hpp's BlobRef already names (003 §3), not a second choice invented here.
// `compute_digest` is declared in worktree_types.hpp and implemented per-platform in
// src/core/worktree_digest.cpp (Windows CNG/BCrypt) / src/core/worktree_digest_posix.cpp (Linux --
// links against `agentengine::worktree_store`.
//
// `Ref` persistence is deliberately NOT part of the object store: a Ref mutates often and wants
// durability/history, unlike Blob/Tree (immutable, digest-addressed, write-once) -- see
// worktree_ref_store.hpp's own file-top comment and the milestone-3 breakdown's decision 1 for why
// forcing both shapes through one seam would be a genuine misfit, not an elegant reuse. ADR-037:
// Ref persistence rides `agentengine::rt::AppendLogStore` (rt/append_log_store.hpp) -- each
// `RefMoved` commit is one appended, JSON-encoded entry under a log id derived from the Ref's own
// `name` (`ref_log_id`), and `read_ref` reconstructs current state by reading the tail and taking
// the last entry -- the same "replay to reach current state" property 025 §9 G1 asks for, just
// without a separate typed EventLog/ActorId bridge: `rt::LogId` is already the plain string a Ref's
// `name` naturally is.
//
// File map:
//   worktree_types.hpp      -- Digest, TreeEntry, Tree, Ref, WorktreeObjectStore concept,
//                               InMemoryWorktreeObjectStore
//   worktree_ref_store.hpp  -- RefMoved, commit_ref, read_ref, turn-boundary commit/rewind
//   worktree_sub.hpp        -- SubWorktree, create/read/write_sub_worktree
//   worktree_merge.hpp      -- three-way merge (MergeConflict, MergeResult, merge_trees,
//                               BranchMergeOutcome, merge_branch_into_parent)
//   worktree_diff.hpp       -- two-way tree diff (TreeDiff, diff_trees, summarize_diff) and the
//                               `shared`-mode staleness note
//   worktree_mount.hpp      -- Mount, mount_read/mount_write, merge-conflict evidence
//                               materialization, the `/conflicts` mount

#include "agentengine/core/worktree_diff.hpp"
#include "agentengine/core/worktree_merge.hpp"
#include "agentengine/core/worktree_mount.hpp"
#include "agentengine/core/worktree_ref_store.hpp"
#include "agentengine/core/worktree_sub.hpp"
#include "agentengine/core/worktree_types.hpp"
