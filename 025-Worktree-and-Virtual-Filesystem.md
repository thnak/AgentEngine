# 025 — Worktree and Virtual Filesystem

**Status:** Draft · **Depends on:** 003, 005, 007, 008, 019 · **Gate:** §9

## Goal

Give every session a **worktree** — a virtual disk the engine owns — that multiple agents in that
session can share or branch, that survives sandbox destruction, process restart, and node
migration, and that looks to the agent like an ordinary filesystem.

## 1. Why files must not belong to the sandbox

A sandbox is a *boundary*, not a *disk*. If files live inside it, then every property the operator
cares about — persistence across turns, survival of a crash, portability across profiles,
shareability between agents, auditability, rewind — becomes a property of whichever isolation
technology happens to be selected. That is backwards.

**The worktree is durable engine state; the sandbox is a disposable execution boundary that mounts
it.** File semantics are then identical under `wasm`, `native-jail`, and `remote` (008), and a
destroyed sandbox loses nothing.

## 2. Model

A worktree is a **content-addressed object store plus a mutable tree**:

```
Blob   = immutable bytes, addressed by digest      (shared with 003 §3 BlobRef)
Tree   = { name → Blob | Tree }, addressed by digest
Ref    = a mutable name → Tree digest              (what "the worktree" currently is)
```

Deliberately the shape of a versioned-file object model, which is why "worktree" is the right word.
The consequences are the useful part:

- **Snapshotting a worktree is recording one digest** — cheap enough to do at every turn boundary.
- **Diffing two states is a tree comparison**, so "what did this agent change?" is answerable.
- **Deduplication is free** — the same file written by two agents is stored once.
- **Rewind is assignment** — restoring a checkpoint is pointing a ref at an earlier tree (014 §5).
- **Identity is the digest**, so artifacts (010 §4), blobs in messages (003 §3), and worktree files
  are the same objects with the same provenance.

Storage is Quark 012's `Store` seam. **No new storage engine.**

## 3. Layout and sub-worktrees

```
session:s-42                     ← root worktree, the session's disk
  /work                          shared working area
  /input                         read-only inputs mounted by the host
  /out                           artifacts collected into the conversation
  /agents/researcher             sub-worktree (branch or share)
  /agents/writer                 sub-worktree
  /skills/<name>                 loaded skill packages, read-only (009 §8)
```

**Sub-worktrees** exist because a session may run several agents (001 §4). Each is created with a
declared **sharing mode**:

| Mode | Semantics | Use |
|---|---|---|
| **`shared`** | The same mutable tree as the parent. Writes are immediately visible to siblings. | Collaborating agents working one artifact set — the common case for a single conversation |
| **`branch`** (default for concurrent siblings) | Copy-on-write from the parent's current tree; changes are private until merged | Parallel agents that must not corrupt each other |
| **`readonly`** | A pinned tree digest; writes rejected | Reviewers, critics, evaluators |
| **`scratch`** | Fresh empty tree, discarded on completion | Throwaway computation |

**The default is chosen by concurrency, not by taste:** agents running *sequentially* in a session
default to `shared` (the natural reading of "they work on the same disk"); agents running
*concurrently* default to `branch`, because concurrent blind writes to one tree is precisely how
multi-agent systems destroy each other's work.

## 4. Concurrency and merge

- **One writer per tree.** A worktree node is a Quark actor; writes to a given tree serialize
  through it (Quark's single-executor invariant). There is no file locking protocol and no lost
  update.
- **Merge on join.** A `branch` sub-worktree merges back when its agent completes. Three-way merge
  against the common ancestor:
  - disjoint changes → merged automatically;
  - identical content → trivially merged;
  - **conflict → the merge fails and is surfaced**, with both versions retained at
    `/conflicts/<path>.<agent>`, and the run's supervising agent or a human resolves it.
- **Conflicts are never resolved by guessing, and never by last-writer-wins.** Silently discarding
  an agent's work is worse than a visible conflict.
- Merges are audited and produce a diff summary usable as model context ("the writer changed 3
  files") without dumping content.

## 5. Mounts and capabilities

A worktree subtree becomes visible to a sandbox only through a capability (007 §3):

```
FsRead<mount>   → { worktree ref, subtree path, size cap }
FsWrite<mount>  → { worktree ref, subtree path, byte quota, file-count cap }
```

- Mount points are **canonical, ordinary-looking paths** inside the guest (`/work`, `/input`,
  `/out`) — never runtime-revealing paths like `/sandbox/vm42/mnt` (026 §2).
- **Path escape is a security bug, not a bug.** Resolution is canonicalized against the mount and
  rejects `..`, absolute redirects, symlinks/junctions/reparse points crossing the boundary, ADS,
  `\\?\` prefixes, unicode-normalization tricks, and TOCTOU re-resolution — tested per 021 §6 G3.
- **A sandbox never sees the object store.** It sees a filesystem view; digests, refs, and history
  are host-side concepts.
- **Quotas are enforced at write**, and exhaustion is an ordinary `No space left on device`-class
  error inside the guest (026 §3), not a protocol lecture.

## 6. Persistence, checkpoints, and lifecycle

- The worktree ref is part of the **session checkpoint** (019 §1); a turn's committed tree digest is
  recorded with the turn.
- **Turn-boundary commit**: at each turn boundary the current tree is committed and its digest
  recorded, making per-turn rewind possible at no meaningful cost.
- **Restore after restart or node migration** is fetching the tree by digest — worktrees are
  location-independent, which is what lets Quark place a session anywhere in a cluster.
- **Retention and GC**: unreachable objects are collected by policy; retained checkpoints pin their
  trees. Redaction and deletion (005 §6) must reach the object store, including unreferenced blobs.

## 7. What the agent sees

Ordinary files. `open()`, `pathlib`, `os.listdir`, `ls`, `cat` — all work, because the mount is a
real filesystem view inside the guest. The agent is **not told** it is a virtual disk, is not asked
to call a worktree API, and does not need to know that its writes are content-addressed and
committed per turn (026 §2). Files that appear are just files.

Two host-side behaviours make this pay off:

- **Artifacts** written under `/out` are collected, digested, and surfaced into the conversation
  automatically (010 §4) — the agent "saves a file", the user receives an artifact.
- **Inputs** are mounted, not pasted into the prompt: a 40 MB CSV costs a mount, not a context
  window.

## 8. Observability and audit

Per turn: files created/modified/deleted, bytes written, quota headroom, tree digest before and
after, merges and conflicts. Every write is attributable to `{run, agent, principal}` (I4), and the
digest chain means "who produced this byte sequence" is answerable after the fact — which is the
practical value of content addressing for an audited system.

## 9. Promotion gate

- **G1 (durability)** — a worktree survives sandbox destruction, process restart, and simulated node
  migration with byte-identical content; proven for every profile in 008.
- **G2 (isolation)** — the path-escape corpus (§5) fails to read or write outside a mount on
  Windows, Linux, and macOS.
- **G3 (concurrency)** — N concurrent `branch` agents produce a deterministic merge result; every
  genuine conflict is surfaced, never silently resolved; no lost update over 10⁴ randomized
  interleavings.
- **G4 (cost)** — turn-boundary commit p99 within the 023 budget; storage overhead for a 1 000-turn
  session with small edits stays within a declared bound (the deduplication claim, measured).
- **G5 (rewind)** — restoring an arbitrary retained turn digest reproduces that turn's tree exactly.
- **G6 (redaction)** — deleting content removes it from live trees, checkpoints, and unreferenced
  objects; a store scan finds no residue.

## 10. Open questions

- **Q1** — Should merge conflicts be presented to the *model* as a task (it can often resolve them)
  or escalated to a human by default? Current position is escalate; model-assisted resolution is an
  obvious extension and an obvious way to lose data.
- **Q2** — Whether `shared` mode should be permitted at all for concurrent agents given §4's
  single-writer serialization makes it *safe* but still makes it *confusing* (an agent's file
  changes under it between reads).
- **Q3** — Large-file strategy: chunked content addressing for multi-GB files versus whole-blob.
- **Q4** — Whether the worktree should be exposed to the *user* as a browsable, downloadable
  artifact of the conversation (it probably should) and what that means for retention policy.
- **Q5** — Cross-session worktree sharing (a project disk spanning conversations) is desirable and
  breaks the "one worktree, one principal, one session" simplicity that makes §5 sound.
