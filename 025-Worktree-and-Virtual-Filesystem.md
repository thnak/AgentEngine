# 025 — Worktree and Virtual Filesystem

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 005, 007, 008, 010, 019 · **Gate:** §9

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
  /knowledge/<corpus>            operator-populated document corpus, read-only, shared (029 §5a)
```

**As-built note on `/skills/<name>`:** this diagram is the target shape. Today, `SkillsProvider`
materializes each skill from its own private, per-instance object/ref store (ADR-024 §4), not as a
subtree of this session `Ref` — the sandbox filesystem ends up looking like the diagram, but the
storage behind `/skills/<name>` and the storage behind `/work`/`/input`/`/out`/`/agents/*` are two
separate content-addressed universes materialized side-by-side, not one shared tree. See
`docs/architecture/worktree-sharing-skills-and-subagents.md` §2 for the full trace.

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

**`shared` for concurrent siblings requires an explicit opt-in (resolves OQ-13 Q2).** Single-writer
serialization makes `shared` free of data races and lost updates even between concurrent siblings,
but a small concurrency prove (`OpenQuestions.md` OQ-13) confirmed it does not make cross-file reads
consistent: a sibling can deterministically observe one file of a related pair already updated and
the other not yet, on every run, not occasionally. An operator who wants concurrent siblings on
`shared` anyway (e.g. a genuinely single-artifact collaboration) sets it explicitly, acknowledging
the read-skew hazard; it is never reached by a plain default.

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
  an agent's work is worse than a visible conflict. A model may be offered a surfaced conflict as a
  **drafting** task — both versions plus the common ancestor, produce a proposed merge — but a
  proposal is never auto-committed; writing it back as the accepted resolution is a write effect
  gated the same as any other (007, 006 §4), confirmed by the supervising agent or a human (OQ-13).
- Merges are audited and produce a diff summary usable as model context ("the writer changed 3
  files") without dumping content. The same mechanism runs proactively for **`shared`** sub-
  worktrees: if the tree has moved since an agent's last read, its next turn opens with the same
  kind of short diff-summary note, so cross-visibility stays ordinary-computer-like (026 §1) without
  the staleness being silent (OQ-13).

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
  migration with byte-identical content; proven for every profile in 008. Passivation and a change
  of sandbox profile (010 §4) are covered by this same gate without separate test scenarios: both
  restore through the identical digest-fetch mechanism (§6) G1 already exercises, so a worktree that
  survives destruction, restart, and migration by that mechanism survives passivation and profile
  changes by construction.
- **G2 (isolation)** — the path-escape corpus (§5) fails to read or write outside a mount on every
  platform in the current target set (021 §2 — Windows now, Linux next).
- **G3 (concurrency)** — N concurrent `branch` agents produce a deterministic merge result; every
  genuine conflict is surfaced, never silently resolved; no lost update over 10⁴ randomized
  interleavings.
- **G4 (cost)** — turn-boundary commit p99 within the 023 budget; storage overhead for a 1 000-turn
  session with small edits stays within a declared bound (the deduplication claim, measured).
- **G5 (rewind)** — restoring an arbitrary retained turn digest reproduces that turn's tree exactly.
- **G6 (redaction)** — deleting content removes it from live trees, checkpoints, and unreferenced
  objects; a store scan finds no residue.
- **G7 (conflict drafting and shared staleness, OQ-13)** — a model-drafted merge proposal never
  reaches the accepted tree without a separate confirming write; a positive control (a draft
  auto-committed by a deliberately miswired path) is caught. A `shared` sub-worktree that changed
  since an agent's last read surfaces the staleness note on its next turn, measured, not asserted.

## 10. Open questions

- ~~**Q1** — Should merge conflicts be presented to the *model* as a task (it can often resolve them)
  or escalated to a human by default?~~ **Resolved 2026-08-03 (see OpenQuestions.md OQ-13):**
  escalate by default, model-assisted resolution allowed only as a **policy-gated proposal, never an
  auto-apply**. The model may draft a merged file from the two conflicting versions retained at
  `/conflicts/<path>.<agent>` — that draft is `Tainted` output like any other model output (003 §2)
  and is data, not authority (I3, 007 §4): it is presented for the same argument-hash-bound approval
  006 §4 already requires for other irreversible effects, never applied because "the model resolved
  it," which 007 §4 already names as a concretely forbidden derivation in a different guise (deriving
  a data-loss decision from model-supplied content). Default is escalate-to-human with no model
  drafting; enabling model-assisted drafting is an explicit per-session/operator policy opt-in, not a
  standing behavior.
- ~~**Q2** — Whether `shared` mode should be permitted at all for concurrent agents given §4's
  single-writer serialization makes it *safe* but still makes it *confusing* (an agent's file
  changes under it between reads).~~ **Resolved, permitted as an explicit override, not banned
  (OQ-13, 2026-08-04):** banning it would make the worktree *more* restrictive than an ordinary
  computer for a case ordinary computers handle constantly — two processes sharing a directory — and
  026 §1's whole design commits to the environment reading as ordinary. Real collaborative patterns
  (a concurrent producer/consumer pair, live co-editing) genuinely want immediate cross-visibility,
  which forcing `branch`+merge would tax for no benefit. What §4 already flags as "confusing" is a
  real, distinct hazard, though — silent staleness with no retained-both-versions safety net, unlike
  a `branch` conflict — so it gets the same treatment §4 already gives merges: a short diff-summary-
  as-context note ("the shared worktree changed since your last turn: 3 files modified by `writer`")
  surfaced at the start of a turn when the tree has moved since the agent's last read. This is an
  extension of §4's existing merge-audit mechanism applied proactively to `shared` staleness, not a
  new one — it closes the silent half of the hazard without banning the mode. `branch` stays the
  default for concurrent siblings (§3, unchanged); `shared` remains available as an explicit,
  non-default choice.
- ~~**Q3** — Large-file strategy: chunked content addressing for multi-GB files versus whole-blob.~~
  **Resolved, No, stay whole-blob for v1 (2026-08-04):** whole-blob dedup already delivers the
  property that matters most (§2: identical files stored once); content-defined chunking would trade
  §2's simple `Blob → Tree → Ref` model for a genuinely more complex chunk-tree object model, for a
  benefit — efficient storage of similar-but-not-identical large files with small deltas — that no
  workload in this spec currently demonstrates needing. CONVENTIONS' "don't design for hypothetical
  future requirements" applies directly. Whole-blob's actual cost at multi-GB scale (a one-byte
  change re-storing the whole file) is bounded by §6's GC reclaiming the old blob once unreferenced —
  a temporary storage spike, not unbounded growth, survivable rather than design-blocking. If a real
  workload later demonstrates this cost is prohibitive, that's evidence-driven design work for a
  dedicated ADR at that time, matching how other genuinely-hard, evidence-needing questions in this
  project (021 §7 Q2) are left open rather than solved speculatively.
- ~~**Q4** — Whether the worktree should be exposed to the *user* as a browsable, downloadable
  artifact of the conversation (it probably should) and what that means for retention policy.~~
  **Resolved, Yes, and it needs no new mechanism (2026-08-04):** it's an ordinary `FsRead<mount>`
  grant (007 §3) to the end-user principal, surfaced through 020's server surfaces, not a bypass of
  the mount model. §3's existing subtree structure gives natural scoping — an operator grants the
  user-facing principal read access to whichever subtrees should be browsable (typically `/work` and
  `/out`, not `/skills/*` or another agent's `/agents/<name>` working directory) — the identical
  capability-gated, principal-scoped model every other mount already goes through, including 018 §6's
  cross-principal-access-denied rule unchanged. **Retention**: browsability doesn't add a new
  category — it's a read view over the same content-addressed store, still subject to §6's existing
  GC and redaction/deletion rules. What it does add is that "downloadable" makes the worktree a real,
  human-visible surface a data-subject deletion request (017 §5) must actually reach — which §6's
  existing "redaction and deletion must reach the object store" language already requires, not a new
  rule to write.
- ~~**Q5** — Cross-session worktree sharing (a project disk spanning conversations) is desirable and
  breaks the "one worktree, one principal, one session" simplicity that makes §5 sound.~~
  **Resolved by 030**: don't share one worktree across sessions — index N independent worktree refs
  at a new Project layer above sessions instead. §5's per-session mount/capability model is
  unmodified; 030 §2 has the detail.
