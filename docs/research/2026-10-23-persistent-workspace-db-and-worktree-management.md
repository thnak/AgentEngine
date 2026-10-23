# A database for persistent workspaces with worktree management

Compiled: 2026-10-23. Question: which database (or database-shaped store) should back a durable,
branchable agent workspace? Not an ADR; it proposes no spec change. The recommendation below is a
shortlist to prove, not a decision.

## What the repo already has (so the question is narrower than "we have nothing")

Verified in `include/agentengine/core/`:

- `WorktreeObjectStore` (`worktree_types.hpp`) is a **concept of four async methods**: `put_blob`,
  `get_blob`, `put_tree`, `get_tree`, all `task<result<...>>`. Its comment says a store "backed by a
  database or an object service suspends on its I/O", so a DB conformer is anticipated.
  `WorktreeObjectStoreWithBlobSize` is an optional refinement for quota checks.
- `InMemoryWorktreeObjectStore` is the production default and is **not durable**.
- `FileWorktreeObjectStore` is a durable conformer: one file per digest (git's loose-object layout
  without fan-out), temp-file plus atomic `rename`, digest-shape gate against path traversal.
- `Ledger<Store>` with `durable_dir` makes **branch and ACL bookkeeping** durable via a whole
  snapshot (`persist_snapshot_locked()`, batched by a scope guard). It never touches `store_`.
- `worktree_ref_store.hpp` has `RefMoved` / `TurnCommit` for per-turn refs.

So the real gap is a DB-backed store with properties the file store does not give. The grep found
**no GC or prune** in `file_worktree_object_store.hpp` or `ledger.hpp`. I did not audit crash
consistency between the object store and the Ledger snapshot, nor multi-process behaviour beyond
what the file store's header describes; treat those as open questions, not findings.

## Candidate requirements (to confirm before choosing)

1. Content-addressed put/get of blobs and trees behind the existing four-method concept.
2. Atomic ref/branch update together with the objects it points at (today: two separate stores).
3. Cheap branch creation and diff proportional to the change, not the tree.
4. Reclaiming unreachable objects (GC).
5. Embeddable from C++23 with a permissive licence, on Windows and Linux. No server to run
   (the core is transport-agnostic and single-process by design).
6. Many concurrent branches and sessions (admission and quotas stay engine-side, I8).

## Candidates

| Candidate | What it is | Fit | Evidence quality |
|---|---|---|---|
| **SQLite as the object store** | Table `object(digest PRIMARY KEY, kind, bytes)` plus refs table in one WAL database | Meets 1, 2, 5 directly; atomic refs and objects in one transaction. 4 and 3 are ours to build (reachability walk, tree diff by digest compare, which the engine already does). | Primary: sqlite.org "faster than filesystem" |
| **DoltLite** | SQLite fork whose B-tree is a prolly tree; Git-like branch/merge on SQL (`github.com/dolthub/doltlite`, C, Apache-2.0) | Closest "branchable DB" that is embeddable. But it versions *tables*, not file trees, so file content would still be rows or blobs, and the engine's own tree/digest model would duplicate it. | Secondary only: AlternativeTo listing, 235 stars, listing dated 2026-09-01. Maturity, merge semantics and C++ embedding unverified. |
| **Dolt (server)** | MySQL-compatible versioned database | Wrong shape (server). Reported not to scale to 1,000 concurrent agent branches in one paper. | Single secondary account of a vendor-affiliated benchmark |
| **lakeFS** | Git-like branches over object storage, Go server, S3 API | A server and HTTP API; no embedded mode found. Good reference design (a branch is a mounted path per sandbox run), poor fit as a dependency. | Vendor pages; Wikipedia lists release 1.87.0 (2026-09-22) |
| **Fossil / git (libgit2)** | VCS repositories | Fossil is one SQLite file with content-hash dedup; git is a key-value store of objects. Both already solve GC and diff, but bring VCS semantics (commits, authorship) the engine's `Ledger` already models. Not evaluated beyond the Fossil-vs-git page. | Fossil's own comparison page; no benchmark found |
| **Filesystem CoW (ZFS, reflink, overlayfs)** | Snapshot the workspace directory, not an object DB | Linux-only and host-specific, so cannot be the common path (Windows is a target). Strong as an optimisation under a Linux backend. | Crab paper (below) |
| **Prolly-tree libraries (`prolly-map`, Rust)** | Content-addressed ordered KV with pluggable store | Interesting algorithm reference (diff cost scales with the change). Rust, so not directly usable. | docs.rs design notes only |

### What the sources actually say

- **SQLite vs files** (sqlite.org/fasterthanfs, measured week of 2017-06-05, updated 2025-11-13):
  about 35% faster reads for ~10 KB blobs, about 20% less space on a 1 GB / 100k-blob set. Per-file
  `fsync` made direct writes about 10x or more slower than SQLite; antivirus slowed direct writes
  by about an order of magnitude with little effect on SQLite (relevant to Windows).
  Caveats from the page: results depend on hardware, larger blobs favour the filesystem, cold
  caches hurt SQLite, one transaction per blob makes it uncompetitive. 2017 hardware, small
  blobs; **re-measure on our file sizes** before trusting it.
- **Git4Data** (arXiv:2609.02106, via a Pebblous write-up, not the paper itself): 100 GB branch
  clone in 0.20 s; 1,000 concurrent agents finished in 400 s while DoltDB did not finish in two
  hours; the bottleneck moves to "scheduling, admission control, and per-branch quotas". The
  authors work for the vendor of the system they compare, so the speed-ups are advocacy until
  reproduced. The qualitative point is the useful one: once branching is cheap, governance of
  thousands of branches is the hard part, which matches I8.
- **Crab** (arXiv:2604.28138): per-turn checkpoints on ZFS snapshot plus CRIU. Filesystem-only
  checkpoints 20-100 ms; ZFS snapshot overhead within 22 ms; needed on about 5-25% of turns.
  Its "Chat+FS" baseline (snapshot workspace only) reached 100% on SWE-bench but only 28% and 42%
  on Terminal-Bench because long-lived process state is lost. Implication for us: a workspace DB
  restores **files, not the persistent shell**; D4's shared `ExecState` is a separate concern.

## Recommendation

1. **Prove a `SqliteWorktreeObjectStore`** conformer (and a refs table in the same database) as the
   first DB option. It needs no new dependency class, gives atomic object-plus-ref commits, and
   fits the existing concept. Gate it with: the same conformance suite `FileWorktreeObjectStore`
   passes, a crash-injection test (kill mid-commit, reopen, no dangling ref), and a benchmark on
   our real blob sizes against the file store, on both Windows and Linux.
2. **Treat GC as its own design item**, regardless of store. Reachability from refs and pinned
   snapshots, with a cap so a hostile branch cannot pin unbounded storage (Git4Data lists
   "storage growth from pinned snapshots" as open).
3. **Do not adopt DoltLite or Dolt now.** Re-check DoltLite once its maturity can be read from the
   primary repo; its table-level versioning does not replace our tree model.
4. **Linux CoW (ZFS/reflink) is an optional backend optimisation**, never the portable path.

Per the repo's own rule, if this is built it goes through `design -> red-team -> prove -> judge`
and an ADR (numbered from 245 or the next free number; check `main` first, parallel sessions
allocate numbers).

## Open questions

- Is the Ledger's whole-snapshot rewrite on every mutation a measured problem at many branches?
  Not measured here.
- What happens today if the process dies between `put_tree` and the Ledger snapshot write?
- Do we need multi-process access to one workspace (the file store's prove phase did show two
  processes sharing a directory), or is one writer per workspace enough? SQLite WAL fits the
  latter and needs care for the former.
- Is DoltLite actually embeddable from C++ and does it preserve SQLite's WAL and locking model?
- Retention: how long are per-turn commits (`TurnCommit`) kept, and who pays for them?

## Sources

- [SQLite: 35% faster than the filesystem](https://www.sqlite.org/fasterthanfs.html) (read)
- [Pebblous: SQL database branching for AI agents (Git4Data)](https://blog.pebblous.ai/blog/database-branching-sql-for-agents/en/) (read; the arXiv paper itself not read)
- [Crab: checkpoint/restore runtime for agent sandboxes, arXiv:2604.28138](https://arxiv.org/html/2604.28138v1) (read first 100k of ~115k characters)
- [DoltLite listing (AlternativeTo)](https://alternativeto.net/software/doltlite/about/) (read; repo not read)
- [Dolt storage engine docs](https://docs.dolthub.com/architecture/storage-engine) (search snippet only)
- [lakeFS: AI agent versioned filesystem with E2B](https://lakefs.io/blog/ai-agent-a-versioned-filesystem-with-e2b-and-lakefs/) and [lakeFS on Wikipedia](https://en.wikipedia.org/wiki/LakeFS) (snippets only)
- [Fossil vs Git](https://fossil-scm.org/home/doc/16ea738a8e14924a/www/fossil-v-git.wiki) (snippet only)
- [DeltaBox, arXiv:2605.22781](https://arxiv.org/html/2605.22781v1) and [arXiv:2608.22928 on safe checkpoint/fork/merge](https://arxiv.org/pdf/2608.22928) (titles and snippets only, not read)
- Internal: `include/agentengine/core/worktree_types.hpp`, `file_worktree_object_store.hpp`, `ledger.hpp`, `worktree_ref_store.hpp`
