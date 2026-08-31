# A durable `WorktreeObjectStore` conformer for `Ledger<Store>` — closing the blob/tree content half of a long-disclosed gap

Design research only, prompted by the open residual `ADR-126`/`ADR-128` each declined to touch and
`tests/test_ledger.cpp` has disclosed since before either of them landed. **This document is not an
ADR** — no `design → red-team → prove → judge` loop has been executed against it, no code exists, and
nothing here is Judged or Proposed. It is what I would carry into a real round of that process. No file
under `include/`, `src/`, or `tests/` was touched to produce it.

## 0. The problem, restated precisely

`Ledger<Store>` (`include/agentengine/core/ledger.hpp`) has two genuinely different pieces of state,
made durable on two different schedules, and only one of them is actually durable today:

- **`branches_`/`blob_acl_`/`tree_acl_`/`branch_seq_`** — branch names, head-tree/head-self digests,
  turn indices, per-branch ownership, and the digest-keyed ACL maps. Durable whenever `durable_dir` is
  supplied to `Ledger`'s constructor: `persist_snapshot_locked()` rewrites a full snapshot (temp file +
  atomic `rename`) after every mutation, and `load_durable_state()` restores it — including
  re-populating `orphaned_from_restart_` — at construction time (`ledger.hpp:1188-1283`). This is the
  half `ADR-126` and `ADR-128` just closed the *recovery* story for: a crashed process's branches and
  their ACL bookkeeping come back, reattachable via `orphaned_branches()`/`reclaim_orphaned_branch()`.
- **The actual blob/tree bytes those digests name** — held only in `Store store_`, which for every real
  production caller today is `agentengine::InMemoryWorktreeObjectStore` (`worktree_types.hpp`): two
  `std::unordered_map`s, gone the instant the process exits. `durable_dir` never touches `store_` at
  all — `Ledger`'s constructor comment says so directly ("atop whatever durability `Store` itself
  already provides for blob/tree CONTENT", `ledger.hpp:1184`), and `InMemoryWorktreeObjectStore`
  provides none.

The consequence is exact and already has a real error code: `perform_three_way_merge_locked()`
(`ledger.hpp:1031-1057`) calls `store_.get_tree(base_digest)`/`get_tree(ours_digest)`/
`get_tree(theirs_digest)` to actually load content for a three-way merge, and on a real crash-recovery
path — a recovered `BranchHandle` whose branch metadata survived but whose store is a fresh, empty
`InMemoryWorktreeObjectStore` — those loads fail, and `merge()` returns `ledger.merge_tree_load_failed`
(`ledger.hpp:1037-1043`). `MandatorySandboxProvider::commit_task_branch()` hits this directly through
`merge_into()`. `tests/test_task_branch_durability_recovery.cpp` (built by `ADR-126`) asserts this
*exact* code as a precise regression test, not merely tolerates it: recovery gets you far enough to
`discard_task_branch()` a recovered handle (a pure branch-table erase, no content needed) but not far
enough to `commit_task_branch()` it (real content load, which fails). `get_blob_safe()`/`get_tree_safe()`
fail the same way outside the merge path too (`ledger.get_blob_failed`/`ledger.get_tree_failed`,
`ledger.hpp:373-393`) — a recovered branch's own content is simply gone, not merely conflict-prone.

`ADR-102`, `ADR-126`, and `ADR-128` each name this the same way: "a real, separate, materially larger
piece of work (a durable object-store conformer for blob/tree content, not merely branch/ACL
bookkeeping)," explicitly out of scope. This document is the first real attempt to scope that work.

## 1. What already exists — this is not a green-field design

Before proposing anything, it matters that a real, standalone, cross-process-proven prototype for
exactly this already exists in this repo and was deliberately *not* ported when `Ledger` moved to
production:

- `docs/planning/proofs/worktree_io/file_object_store.hpp` — `probe::FileWorktreeObjectStore`, a real
  `WorktreeObjectStore` conformer (`static_assert(agentengine::WorktreeObjectStore<FileWorktreeObjectStore>)`
  holds) storing one file per blob under `<root>/blobs/<digest>` and one per tree under
  `<root>/trees/<digest>`, git's own loose-object layout minus the two-character fan-out directory.
  Temp-file-plus-atomic-`rename` writes, bounds-checked tree decoding, and (added after a real
  code-review pass) a `is_well_formed_digest()` gate rejecting anything that isn't exactly 64 lowercase
  hex characters before it ever reaches a filesystem path — closing a real path-traversal opening a
  caller-supplied (as opposed to self-computed) digest string would otherwise have.
- `docs/planning/proofs/worktree_io/probe_durability.cpp` — proves dedup, durability, and the
  path-traversal rejection, all against real files on real disk, object-destroyed-and-reconstructed in
  the same process.
- `docs/planning/proofs/worktree_io/durable_ledger_write.cpp` / `durable_ledger_read.cpp` and
  `crash_reclaim_write.cpp` / `crash_reclaim_read.cpp` — `identity-native-sandbox-worktree-design.md`
  §34.3/§34.6 compiled and ran `Ledger<FileWorktreeObjectStore>` (the exact template the production type
  already supports — `Store` is a template parameter today specifically so this substitution needs no
  change to `Ledger` itself) across **two genuinely separate OS processes**, not an in-process
  simulation, and it worked: branch, ACL, *and* content all survived a real process exit and were read
  back correctly by a freshly-launched process that shared nothing but the directory.

So the honest framing is not "does a durable object store exist for this shape" — it does, and it has
already been proven to compose with the real `Ledger<Store>` template, in real cross-process runs. The
open questions are: (a) is loose-object-per-file still the right shape given what has changed since
§34 (ADR-111's `AsyncQuota<StorageBytes>`/`MergeCost` gating, ADR-112's per-entry ACL grants, the sheer
number of `put_tree`/`get_tree` calls `merge()`'s four-step decomposition now makes per call), (b) what
would it take to actually port it rather than leave it as a standalone probe, and (c) is there a
genuinely better-fitting alternative given how this design has evolved. That is what follows.

## 2. The question, stated so it has a wrong answer

Does closing this gap mean **porting `FileWorktreeObjectStore` essentially as-is** (one file per digest
under `durable_dir`, the loose-object model, already proven against the real `Ledger<Store>` template
in a genuine two-process run) — or does something about how `Ledger` has grown since §34 (a fixed
per-`merge()` cost of three `get_tree` loads plus one `put_tree`, now `AsyncQuota`-gated; per-entry ACL
grants touching many more digests per merge than the prove-phase original ever exercised) make a
different storage shape the actually-better fit, worth the cost of *not* reusing an already-proven
artifact?

## 3. Design options

### Option A — port `FileWorktreeObjectStore` (loose objects, git's own model)

One regular file per digest, two flat directories (`durable_dir/blobs/`, `durable_dir/trees/`),
content-addressed by construction — the file's own name *is* its integrity check (a caller can always
re-hash what it read and compare). This is Option A rather than "the recommendation" only because I
have not yet run it through a real red-team round against the current `Ledger`; see §5.

**How it satisfies `WorktreeObjectStore`.** Directly — `put_blob`/`get_blob`/`put_tree`/`get_tree`,
unchanged from the concept in `worktree_types.hpp:99-105`. `FileWorktreeObjectStore` already
`static_assert`s conformance.

**Changes to `Ledger<Store>` itself.** None structurally required — `Store` is already a template
parameter, and every `store_.put_tree()`/`get_tree()`/`put_blob()`/`get_blob()` call site in
`ledger.hpp` is already generic. The one real integration question is at the *construction* call site,
not inside `Ledger`: something has to decide `durable_dir / "objects"` (or a sibling directory) is
where `FileWorktreeObjectStore`'s own root goes, and `MandatorySandboxProvider`/whatever host code
constructs a durable `Ledger` needs to pass a `Ledger<FileWorktreeObjectStore>` rather than
`Ledger<InMemoryWorktreeObjectStore>` (today's implicit default) when it wants both halves durable
together. `Ledger`'s own `durable_dir` and the store's own root directory are two independently-passed
paths today (the constructor takes `Store store_` and `durable_dir` as two separate parameters) — a
caller that supplies `durable_dir` for the branch/ACL half but constructs a plain, non-durable
`InMemoryWorktreeObjectStore` for `store_` gets exactly today's split-durability behavior, silently.
Worth a real API-level decision (not attempted here): should `Ledger`'s constructor derive the object
store's root from `durable_dir` itself when `Store == FileWorktreeObjectStore`, closing that
mismatch-by-construction, or is that overreach for a class that is otherwise deliberately store-shape-
agnostic?

**Concurrency.** `Ledger`'s own `mutex_` already serializes every `store_` access — `create_root_branch()`'s
comment states this explicitly (`ledger.hpp:428-430`: "InMemoryWorktreeObjectStore has no internal
synchronization of its own"), and every mutating method takes `mutex_` before touching `store_`.
`FileWorktreeObjectStore` as prototyped adds its *own* `std::mutex` on top (`file_object_store.hpp:200`)
— today, under a single `Ledger`, that second lock is redundant (never contended, since `mutex_` already
excludes concurrent access), not wrong. It stops being redundant the moment any *other* caller — a
second `Ledger` instance pointed at the same `durable_dir`, or a completely separate tool reading the
same object files directly — touches the same directory without going through that `Ledger`'s `mutex_`.
That is a real, live scenario this design has to name honestly: nothing today prevents two processes
(or two `Ledger` instances in one process) from opening the *same* `durable_dir` concurrently. Content
writes are the safer half of this — `put_blob`/`put_tree`'s temp-file-plus-rename means a concurrent
writer of the *same* digest either does the identical rename twice (harmless, since content-addressing
guarantees identical bytes) or the two `.tmp` filenames never collide (each writer computes its own
digest first) — but the *bookkeeping* half is not: two `Ledger` instances would each run their own
`persist_snapshot_locked()` full-rewrite against the same `ledger_state.snapshot` path with no
coordination at all, and the later rename wins, silently discarding whatever the other process wrote in
between. This is not a new hazard this option introduces — it is already true of today's
metadata-only durability, `ADR-126`/`ADR-128` never claimed to close it, and `bind_root_branch()`'s own
§2 explicitly names the structurally identical "still-live double-bind" hazard for the same reason
(`ADR-128` §2/§5) — but making content durable too raises the stakes of an already-disclosed gap rather
than introducing a new one, and a real design pass should say that plainly rather than let it hide
behind "concurrency is already handled."

**Sketch.**
```cpp
class FileWorktreeObjectStore {
public:
    explicit FileWorktreeObjectStore(std::filesystem::path root);
    result<Digest> put_blob(std::span<std::byte const> bytes);
    result<std::vector<std::byte>> get_blob(Digest const& digest) const;
    result<Digest> put_tree(Tree tree);
    result<Tree> get_tree(Digest const& digest) const;
    std::size_t blob_count() const;
private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
};
static_assert(WorktreeObjectStore<FileWorktreeObjectStore>);
```
Essentially the prove-phase file verbatim, minus its `probe::` namespace and `std::unique_ptr<std::mutex>`
indirection (a `BranchHandle`-style move-only wrapper reason that may no longer apply once this is a
real, not-moved-around production type — worth checking, not assumed).

**Real risks/tradeoffs.**
- **Partial-write/crash-mid-write.** Already addressed by the temp-file-plus-atomic-`rename` discipline
  the prove-phase file already uses — this is the same pattern `persist_snapshot_locked()` and
  `identity_authority.hpp`'s `persist_high_water_mark()` already use elsewhere in this codebase, not a
  new idiom. `std::filesystem::rename` is atomic *within the same filesystem/volume* on both platforms
  this repo targets; it is not atomic across volumes, and this design does not attempt to detect or
  reject `durable_dir` pointing across a mount/volume boundary from its own temp-file location — a real,
  disclosed gap, not a solved one. The design's own stated bar (§34.3: "safe across a clean exit or
  crash, not across a genuine power loss mid-write") should be restated identically here, not
  re-litigated.
- **Disk space growth with no GC.** Every distinct blob/tree digest this `Ledger` (or any branch that
  ever existed against it) has ever written stays on disk forever — `abandon()`/`reap_pending_abandons()`
  only ever remove `branches_` entries, never touch `store_`, and this is true today for
  `InMemoryWorktreeObjectStore` too (it just resets on process exit, silently hiding the growth). Making
  the store durable turns "unbounded in-memory growth within one process lifetime" into "unbounded
  on-disk growth across the store's *entire* lifetime" — a materially different failure mode (an
  operator has to notice and intervene; the old one self-resolved on restart). No GC is proposed here —
  see §5.
- **Cross-platform filename concerns.** SHA-256 hex digests are lowercase ASCII `[0-9a-f]{64}` —
  already the only alphabet `is_well_formed_digest()` accepts — which is a legal filename on both NTFS
  and every POSIX filesystem this repo targets, with no case-folding risk (`Ledger::
  check_case_folding_collision()`'s own concern is about tree *entry names*, a completely different
  string, not digests). The one real cross-platform difference worth naming: Windows path length limits
  (`MAX_PATH` without long-path opt-in) are not a concern here either, since `durable_dir/blobs/<64 hex
  chars>` is well under any practical limit — but a real implementation should still confirm this
  against whatever `durable_dir` value a real host actually passes (nothing today bounds how deep a
  caller-chosen `durable_dir` itself might already be).
- **Two directories, one per-file `stat`/`open`/`close` per object.** A branch's tree can reference many
  entries; `merge()` alone loads three full trees plus commits one more per call, `AsyncQuota<MergeCost>`-
  gated specifically because this cost is "a fixed, roughly-constant per-call expense" (`ledger.hpp:186-189`)
  — that framing assumed in-memory map lookups, not real filesystem I/O. This is a real, currently
  unmeasured cost delta this option introduces that the in-memory default never had, and `MergeCost`'s
  own "roughly-constant" framing should be re-examined (not assumed still accurate) once real I/O is in
  the loop — see §5's gate.

### Option B — a single append-only log ("pack") file with an in-memory (or small durable) index

Instead of one file per digest, every `put_blob`/`put_tree` appends a length-prefixed record (digest,
kind, bytes) to one growing `durable_dir/objects.log` file, and an index (`Digest -> {offset, length}`)
resolves reads. This is closer in shape to what `Ledger`'s own metadata durability already does
(`persist_snapshot_locked()`'s full-rewrite) or to a git packfile, and closer to the
`rt::AppendLogStore` pattern this codebase already uses for `Ref` history (`AgentEngineSpecification.md`
D4/§7: "Worktree objects and refs... `rt::AppendLogStore`-backed `Ref` history").

**How it satisfies `WorktreeObjectStore`.** Same four methods; `get_blob`/`get_tree` seek to the
indexed offset and read the record. Deduplication (`put_blob`'s "don't rewrite an existing digest",
`FileWorktreeObjectStore`'s own `!std::filesystem::exists(path)` check) becomes an index lookup instead
of a filesystem existence check — functionally identical, one less `stat` call per write.

**Changes to `Ledger<Store>` itself.** Same as Option A — none structurally, same construction-site
question about wiring `durable_dir` to the store's own root/log path.

**Concurrency.** Strictly *worse* than Option A for the same "two writers, one directory" scenario named
above: Option A's per-digest files mean two concurrent writers of *different* content never contend on
the same file at all; a single shared log file means every `put_blob`/`put_tree` call is a real
serialization point on the file itself (an append needs the current end-of-file offset, which is a
genuine race between two writers unless externally serialized — exactly the same shape `Ledger`'s own
`mutex_` already provides for a single `Ledger` instance, but does nothing for two). The append-only
design also makes crash-safety *harder* to reason about, not easier: a crash mid-append can leave a
truncated trailing record, and unlike Option A's atomic-rename-per-object (where an incomplete write
never becomes visible under its final name), a truncated tail here requires the reader to detect and
skip it explicitly — real, additional logic Option A does not need.

**Sketch.** Same public surface as Option A; internally, `struct IndexEntry { std::uint64_t offset,
length; bool is_tree; }`, `std::unordered_map<Digest, IndexEntry> index_`, rebuilt by a full scan of
`objects.log` at construction (mirroring `load_durable_state()`'s own "rebuild in-memory state by
reading the durable file at startup" pattern) unless a durable index is also maintained, which is its
own extra durability problem layered on top.

**Real risks/tradeoffs.** Everything Option A has (GC, cross-volume rename atomicity, no encryption/
compression attempted), *plus*: an unbounded single file that never shrinks even more visibly than many
small files (no filesystem-level "delete this one object" primitive — removing one object means
rewriting the whole log, the exact `persist_snapshot_locked()`-shaped full-rewrite cost `Ledger`
already accepts for its comparatively tiny metadata, now applied to potentially-large content); a
single point of corruption (one bad byte in the log can desync every offset after it, versus Option A
where a single corrupted object file affects only that one digest); and it reuses none of the
already-proven `FileWorktreeObjectStore`/`probe_durability.cpp`/`durable_ledger_write.cpp` evidence —
everything would need to be re-proven from scratch. I do not think this option earns its added
complexity for this specific problem (see §4), but it is the genuinely different shape the prompt asked
for, and it is the shape `rt::AppendLogStore` precedent in this same codebase would make a reviewer ask
about first.

### Option C (considered, not detailed) — an embedded SQLite-backed store

A single `durable_dir/objects.db` file, one table keyed by digest, using SQLite's own WAL mode for
crash-safety and its transaction support instead of hand-rolled temp-file-plus-rename logic. Steelman:
SQLite's crash-safety guarantees are far more battle-tested than a bespoke atomic-rename scheme, and a
single file is operationally simpler to back up/inspect than a directory tree of thousands of loose
objects. Rejected from detailed treatment here, not because it's a bad idea, but because it fails this
codebase's own established precedent-reuse standard the same way `oci-execution-surface-design-draft.md`
§2 rejected Podman for the OCI conformer: SQLite is not a dependency anywhere in this project today
(`session_store.hpp`'s own comment names it only as a hypothetical a future host might add on its own,
never as something this codebase carries) — pulling it in would be a real, new third-party dependency
for a design line (ADR-037) that just spent real effort *removing* one (Quark). Worth a real design
pass of its own if a future round decides the operational-simplicity argument outweighs that cost; not
pursued further here.

## 4. Recommendation

I would take **Option A — porting `FileWorktreeObjectStore` largely as-is** into a real
`design → red-team → prove → judge` round, for reasons that match this repo's own stated bias
(`CLAUDE.md`: "Default to enabling, not blocking") without pretending that bias settles the security
questions §5 still needs answered:

- It is not a new design — it is a real, already-proven artifact (§1) that closes the exact gap this
  document restates in §0, already shown to compose with the real `Ledger<Store>` template across a
  genuine two-process run. Reusing it costs less than either alternative and starts from a stronger
  evidence base than a fresh design would.
- It satisfies `WorktreeObjectStore` with no changes to the concept or to `Ledger<Store>` itself —
  the template parameter this design already carries is precisely the seam this option needs and
  nothing more.
- Option B's real advantage over Option A (fewer files) is not a property this problem actually needs —
  `durable_dir`-backed `Ledger`s are, by this design's own existing scope, per-session/per-owner
  artifacts (root branches are named `root-<owner_id>`), not expected to accumulate the way a single
  shared git repository's object count does — and it is strictly worse on every concurrency and
  crash-recovery axis named in §3.
- Option C's advantage is real but orthogonal to correctness — this is an "add a dependency to make
  ops easier" argument, not an "Option A cannot be made safe" argument, and this codebase's own
  disclosed precedent (ADR-037) is to keep the dependency surface small.

**What I am not recommending**: treating this as settled. `CLAUDE.md` is explicit that this class of
design ("security/hot-path-adjacent," which digest-keyed content storage feeding an ACL-gated read path
plainly is) needs a real `design → red-team → prove → judge` round with genuine adversarial pressure
before anything ships — not merely this document's own reasoning about it. In particular, §34's own
prove-phase evidence for `FileWorktreeObjectStore` predates `ADR-111`/`ADR-112` (the `AsyncQuota<MergeCost>`
gating and the per-entry ACL grant `merge()` now performs) and predates `ADR-126`/`ADR-128`'s own
crash-recovery automation — all real changes to `Ledger`'s call pattern against `store_` that the
original prove-phase probes never exercised. A real round needs to re-run (not merely re-read) that
evidence against the *current* `Ledger`, not assume 2026-08-28's proof still describes 2026-08-30's code.

## 5. Proposed gate / promotion criteria

Matching this repo's own stated policy ("A design without a falsifiable gate does not get written down
as settled"), before any of this could be marked Judged I would want, at minimum:

1. **The exact regression `ADR-126` already wrote, now passing for the right reason.**
   `tests/test_task_branch_durability_recovery.cpp`'s Phase B currently *asserts*
   `commit_task_branch()` fails with `ledger.merge_tree_load_failed` on a recovered handle — that
   assertion is the precise, disclosed signature of this gap. A real implementation must make that
   `commit_task_branch()` call **succeed** instead, and the test updated accordingly, with the old
   assertion's removal itself reviewed (not silently deleted) so the historical record of what this
   closed is not lost.
2. **A genuine three-process (not merely three-call) proof, mirroring `identity-native-sandbox-worktree-design.md`
   §34's own methodology**, but exercising a real `merge()` (not just `get_blob`/`get_tree`) across a
   simulated crash — process 1 creates a root branch, spawns a child, commits real content to the
   child, and exits without merging; process 2 (fresh `Ledger<FileWorktreeObjectStore>`, same
   `durable_dir`) reclaims both branches via the already-shipped `reclaim_orphaned_branch()`/
   `bind_root_branch()` machinery and successfully `merge()`s the child's real, disk-recovered content
   into the parent. This is the actual end-to-end claim `ADR-126`/`ADR-128` left open, not merely
   "the store round-trips bytes."
3. **A real, adversarial pass on the §3 concurrency disclosure**, not merely restating it: what actually
   happens under two `Ledger` instances (same process or two real processes) pointed at the same
   `durable_dir` concurrently — probed for real, the way `probe_concurrent_ledger.cpp`/
   `probe_concurrent_acl.cpp` already did for the in-memory store in the prove phase, extended to cover
   the durable case those probes never touched.
4. **A real measurement of `MergeCost`'s "roughly-constant per-call" claim against real disk I/O**, not
   an in-memory map — `AsyncQuota<MergeCost>`'s whole justification (`ledger.hpp:180-192`) rests on that
   claim; this design changes what "per-call cost" actually means underneath it, and that needs a real
   number, not an assumption it still holds.
5. **`tools/naming_lint.py` clean** for whatever new type this introduces (`FileWorktreeObjectStore`
   itself, ported verbatim or renamed, needs registration the way every other Phase 2/3/4 vocabulary
   addition in `ADR-102` needed it — this repo's own history shows that step catching real gaps, not
   being a formality).
6. **An explicit statement of the `IdentityAuthority`-durability precondition**, verified against the
   *production* code, not just the prove-phase probe: `identity-native-sandbox-worktree-design.md` §33
   found a real, twice-reproduced cross-principal leak when durable ACL bookkeeping outlives a
   non-durable identity allocator, fixed in §34.2 by making `IdentityAuthority::bootstrap(durable_dir)`
   durable too (confirmed present in production, `include/agentengine/trust/identity_authority.hpp:103-105`).
   Production `Ledger` already persists `blob_acl_`/`tree_acl_` keyed by principal id today, with or
   without this change — but today, a recovered ACL entry pointing at a recycled id can never actually
   leak *content*, because the content itself is unrecoverable (`ledger.get_blob_failed`/
   `ledger.get_tree_failed`). **Making content durable is exactly the change that would convert this
   currently-latent, fails-closed risk into a live one** if a host ever configures `Ledger`'s
   `durable_dir` without *also* consistently configuring `IdentityAuthority::bootstrap()`'s own
   `durable_dir` (or points the two at directories that silently fall out of sync — a fresh identity
   store paired with an old, still-populated Ledger). A real round should reproduce §33's own two-process
   probe against the *current* production `IdentityAuthority` + a durable `Ledger<FileWorktreeObjectStore>`
   together, not trust that §34.2's fix (proven against the prove-phase probe, on a different day) still
   holds against what's shipped since.
7. **Independent red-team round**, full stop — every ADR in this design line that touches
   `mandatory_sandbox_provider.hpp`/`ledger.hpp` has gotten one (`ADR-102`, `ADR-114`, `ADR-117`,
   `ADR-119`, `ADR-126`), and each found something real. There is no reason to expect this line's first
   genuine on-disk content-storage change would be the exception.

## 6. Explicitly out of scope for an initial cut

- **Garbage collection of unreferenced content.** Nothing here proposes reference-counting or mark-and-
  sweep across `branches_`/`checkpoints` to reclaim blobs/trees no live branch still references. This is
  a real, disclosed gap (§3), not a solved one — a v1 should ship with unbounded on-disk growth
  explicitly named as a residual, the same honest posture `ADR-102`/`ADR-126` already model for other
  gaps, rather than block on solving it first.
- **Cross-process file locking beyond what `Ledger::mutex_` already provides.** The "two `Ledger`
  instances, same `durable_dir`" hazard named in §3/§5 item 3 is investigated and disclosed, not
  necessarily closed, by a first cut — the existing `persist_snapshot_locked()` metadata path already
  carries an equivalent unaddressed risk today (`ADR-128` §2's own "still-live double-bind" disclosure),
  and a content-durability cut should not be blocked on solving a class of problem this design line has
  already shipped without solving elsewhere, though a real round should at minimum confirm content
  writes don't make that existing risk *worse* in a new way (§3's per-digest-file argument suggests they
  don't, but "suggests" is not "proven").
- **Migrating already-existing `InMemoryWorktreeObjectStore` data.** There is no proposed mechanism for
  taking a live, in-memory-only `Ledger`'s current content and writing it into a newly-durable store —
  a host that wants durability configures it from the start (or from the next restart), the same way
  `durable_dir` already works for the metadata half today. A running process switching stores live is
  not attempted.
- **Encryption at rest, compression, or any format beyond the prove-phase original's plain
  length-prefixed framing.** Not because they're unimportant, but because they're orthogonal to closing
  the specific, precisely-scoped gap this document addresses, and adding them now would be exactly the
  kind of scope creep this repo's own ADR process (`decisions/README.md`: "An ADR for everything is an
  ADR for nothing") warns against bundling into one change.
- **Any change to `WorktreeObjectStore`'s own concept shape**, `Ledger<Store>`'s public API, or the
  `merge()`/`commit()` call sequence. Every option in §3 is designed to be a pure `Store` substitution —
  if a real round finds that assumption wrong (e.g. the concept genuinely needs a new method for GC or
  for exposing storage-usage introspection), that is itself a finding worth its own document, not
  something to fold in here.
