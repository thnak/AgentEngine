#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §3's sub-worktree sharing-mode vocabulary.
//
// Split out of core/worktree.hpp (ADR-032) so a header that only needs the four-way MODE VALUE --
// not the object store, the Ref/Tree machinery, or any Quark actor/persistence dependency --
// doesn't have to pull all of that in transitively. `workflow/graph.hpp` is the first such
// consumer: it is deliberately "THE GRAPH AS DATA, and nothing else" (its own header comment), and
// `worktree.hpp` pulls in `quark/core/event_log.hpp`/`persistence.hpp`/`snapshot.hpp`/
// `trust/capability.hpp` -- real actor/persistence/capability machinery a graph renderer, YAML
// validator, or CI policy-reachability tool has no business depending on just to read one enum.
//
// `worktree.hpp` includes this header and keeps using the unqualified `sharing_mode` name --
// nothing that already spells `agentengine::sharing_mode` needs to change.

namespace agentengine {

// ae-naming-lint: allow sharing_mode — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class sharing_mode { shared, branch, readonly, scratch };

}  // namespace agentengine
