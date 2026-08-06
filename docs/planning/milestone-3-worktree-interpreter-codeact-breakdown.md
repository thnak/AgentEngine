# Milestone 3 — Code interpreter, CodeAct, worktree — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 3 exit criterion: *"CodeAct runs a
multi-step Python program against real tools and the worktree; 026 §8 G4 (transparency is not
security — full hostile suite re-run against an agent explicitly told it's sandboxed, results
identical) passes, which is this RFC's central, falsifiable claim."*

**RFCs:** 025 (Worktree and Virtual Filesystem, gate §9), 010 (Python Code Interpreter and Shell,
gate §9), 026 (Agent-Facing Runtime Surface, gate §8). All three are Reviewed (2026-08-05). The
roadmap's own build order — worktree first ("it's what makes the interpreter's state feel
persistent"), then the interpreter/shell sharing one `ExecState`, then the `agent.*` library on
top — is followed here.

**A live caveat, not a blocker but worth stating up front:** `025-Worktree-and-Virtual-
Filesystem.md` has uncommitted working-tree edits from another concurrent session as of this
writing (2026-08-06) — confirmed via `git status`, content read directly from the working tree
below. This breakdown is written against that current text; if it moves further before Phase A
lands, the affected section gets a fast re-check, not a silent staleness. `009-Plugin-and-
Extension-System.md` and `029-Memory-System.md` are also mid-edit elsewhere and are out of scope
for M3 regardless.

## Current state (verified 2026-08-06, after M2)

| Item | State |
|---|---|
| `Blob`/`Tree`/`Ref`/`sharing_mode` (`core/worktree.hpp`) | M0 vocabulary only — `Blob{digest}`, a recursive `Tree` keyed by raw pointer (not a real content-addressed store), `Ref{name, tree_digest}` as plain strings, `sharing_mode` as a bare enum. No hashing, no store, no mutation API, no sub-worktree/mount machinery at all |
| `ExecState`, `Runner` concept (`sandbox/runner.hpp`) | M0 vocabulary only — `{cwd, env}` and a `run(request, state, ctx) -> result<ExecOutcome>` concept. No concrete `Runner`, no `PythonRunner`/`ShellRunner` satisfying it |
| `SandboxBackend`, `MountSpec`, `ExecRequest`/`ExecOutcome` (`sandbox/sandbox.hpp`) | Real (M2), including `MountSpec::source` as `std::variant<std::string /*host path*/, BlobRef>` — already anticipates a worktree-backed mount source, unused by any caller yet |
| `BlobRef` (`core/content.hpp`) | Real (M0/M2) — digest + media type + size, the one digest vocabulary 025 §2 says the worktree's `Blob` should share rather than reinvent. Currently a hand-rolled digest holder, not wired to any actual hash function or store |
| `agent_library_registry()`/`granted_modules()`/`push_side_summary()` (`trust/agent_library_manifest.hpp`) | Real, already proven (`tests/test_agent_library_manifest.cpp`) — 026 §5a's discoverability *metadata* mirrors §5's nine-module table and gates on `capability_kind`. This is the registry only: no embedded CPython `agent` module exists yet to bind `dir()`/`help()` against, and it gates on bare `capability_kind` rather than a real parameterized `Capability` (documented limitation in the header itself, left for a follow-up, not blocking) |
| `src/backends/native_jail/{shell_grammar,shell_parser,shell_dispatch,shell_runner,command_registry}.*` | ADR-001's (**Judged**) prove-phase spike for the `ShellRunner` grammar/dispatch design — ADR-001 accepted "Design A" (recursive-descent parser → pmr AST → tree-walking evaluator against injected `FileSystemAdapter`/`CommandRegistry`) on Windows only; no adversarial red-team or fuzzing pass has run against the parser (010 §9 G8 is new work, not yet started) |
| `src/backends/native_jail/{python_lockdown,python_runner}.*` | ADR-002's (**Judged, with a stated caveat** — "the finder mechanism is accepted; the 'closed by construction' claim is [narrower than first written]") prove-phase spike for CPython embedding + `sys.meta_path` import mediation. `python_runner.hpp`'s own header states plainly what's *not* built: `__import__`/`importlib.import_module` defense-in-depth wrappers, `open`/`socket`/`subprocess` mediation wrappers, and per-call capability-freshness derivation from `EffectContext` — it uses a fixed allowlist baked in at construction, not real 007 `CapabilitySet` enforcement |
| ADR-003 (caller-aware dual-registry import gating) | **Judged**, but per the M2 breakdown's own note, "has never been built as real C++ (only a standalone Python reproduction used for red-team/prove)" — this gap is explicitly M3's to close, not M2's |
| `AGENTENGINE_BUILD_PYTHON_RUNNER` (root `CMakeLists.txt`) | Exists as a CMake option (default off, matches the tier-2-dependency posture `AGENTENGINE_WITH_WASM`/`AGENTENGINE_WITH_HTTPS` also use) — embeds CPython, currently only exercised by the native_jail spike code above, not by anything under `include/`/`src/sandbox/` |
| Quark's `Store` seam (`third_party/quark/include/quark/core/persistence.hpp`) | Real, but its shape is **per-`ActorId` event log + snapshot**, not content-addressed key-value — see decision 1 below, since 025 §2 literally says "Storage is Quark 012's `Store` seam. No new storage engine," and that claim needs reconciling against what `Store` actually is, not assumed to fit as written |
| `agent.tools` bridge, `call_tool` (010 §6, 026 §4) | Not started — depends on both a real `Runner` and the tool pipeline (006, real since M2) being wired together, which is this milestone's Phase F |

## Design decisions made while breaking this down

1. **025 §2's "Storage is Quark 012's `Store` seam. No new storage engine" needs one refinement,
   not a rewrite: `Store` and content-addressed blob storage are two different shapes, and the
   worktree needs both, doing different jobs.** Quark's `Store` concept
   (`persistence.hpp:132-147`) is keyed by `ActorId` and models one actor's mutable, event-sourced
   state — `acquire_fence`/`append`/`save_snapshot`/`read_log`. A **`Ref`** (025 §2: "a mutable name
   → Tree digest — what 'the worktree' currently is") is exactly that shape: one small, frequently-
   updated pointer per session/principal, wanting durability, fencing (rejecting a stale writer —
   relevant once multi-node placement is real), and a natural history (the event log doubles as
   "which tree did this ref point to, when," useful for 025 §9 G5's rewind proof). A **`Blob`**/
   **`Tree`** is the opposite shape: immutable once written, addressed by its own digest rather than
   by an owning id, and never mutated or fenced — the closer analogy is git's loose-object store,
   not an actor's durable state. Forcing blobs through `Store`'s `ActorId`-keyed, fencing,
   sequence-numbered API (e.g. treating each digest as a synthetic single-write `ActorId`) would be
   a genuine misfit, not an elegant reuse — paying for machinery (fencing, ordered replay) that a
   write-once object store doesn't need and that would have to be defeated rather than used.
   **Resolution:** `Ref` persistence goes through `Store` directly (a worktree ref *is* a small piece
   of actor-durable state — literally what 025 §6 already says: "the worktree ref is part of the
   session checkpoint"). The `Blob`/`Tree` object store is a **new, small, std-only seam** — content
   digest → bytes, immutable, get-or-put only — built the same way `FileStore` was (`pal::file_io`'s
   durable-write primitives directly, `012-Persistence.md`'s own reference adapter pattern applied to
   a different shape), not a new storage *engine* in the sense CONVENTIONS' dependency-tier language
   warns against (no new third-party dependency, no new durability protocol — same PAL primitives,
   same crash-safety discipline, different key shape). This is an ordinary implementation decision
   (a storage-shape clarification, not an isolation boundary, hot path, or a case of genuinely
   competing designs), so it's recorded here rather than escalated to an ADR — flagged instead as a
   candidate 025 §2 wording fix once M3 lands, so a future reader isn't left reconciling the RFC
   prose against the code from scratch.
2. **The digest function is SHA-256, taken directly from 003 §3's `BlobRef` precedent rather than
   chosen fresh for the worktree.** 025 §2 says worktree `Blob`s are "shared with 003 §3 `BlobRef`,"
   and `BlobRef` is already the project's one digest+store+media-type vocabulary (008 §2's
   `MountSpec` comment says so explicitly) — introducing a second hash choice for `Tree`/`Ref`
   digests would fork that vocabulary for no stated reason. `Tree` and `Ref` digests are computed
   over a canonical serialization of their own contents (sorted entry list by name, so two trees
   with the same entries in different insertion order hash identically) — matching git's own
   "identical content, identical hash regardless of how you got there" property, which is what makes
   025 §2's deduplication claim ("the same file written by two agents is stored once") actually true
   rather than aspirational.
3. **`native-jail` is M3's only real sandbox target for the interpreter/shell, matching 010 §2's own
   table** (`native-jail` = default/every tier, `remote` = out of scope, deferred to M9 same as M2's
   `remote` deferral). This mirrors the M2 breakdown's decision 3 (native-jail-only for M2's sandbox
   proof) one milestone later — 010 §2 itself forecloses a `wasm` Python backend permanently (the
   locked decision in CLAUDE.md), so there is no third profile to build against here at all, unlike
   M2 where `wasm` was in scope for the *plugin* ABI specifically.
4. **Confirmed by the project owner (2026-08-06). `PythonRunner`/`ShellRunner` are written fresh
   against 008/010 as currently Reviewed, carrying forward the *findings* of ADR-001/002/003 (all
   Judged), not extending the spike code under `src/backends/native_jail/`** — the identical
   treatment the M2 breakdown gave `NativeJailBackend` relative to ADR-004's (Spiked, not Judged)
   findings, one notch stronger here since ADR-001/002/003 are actually Judged rather than merely
   Spiked. The spike files stay in place as those ADRs' cited evidence; this milestone's real
   `PythonRunner`/`ShellRunner` (satisfying `sandbox/runner.hpp`'s `Runner` concept, taking a shared
   `ExecState&`, wired to real 007 `CapabilitySet` mediation) are new translation units — no line-by-
   line audit of the spike's prove-phase scope limitations (fixed allowlist, missing `open`/`socket`/
   `subprocess` wrappers) is needed to confirm nothing prove-phase-only survives into production,
   because nothing of the spike's implementation is reused, only its Judged design findings.
5. **`ShellRunner`'s grammar-parser fuzzing gate (010 §9 G8) is its own ADR-track item**, per the
   roadmap's explicit call-out ("new fuzzing infrastructure, build it here rather than deferring
   hostile-input hardening") and `decisions/README.md`'s "any choice where credible designs disagree"
   /hot-path-adjacent-security-critical bar — a parser processing tainted input (010 §10 Q7's own
   resolution) that gets to decide what runs is exactly the shape prior ADRs in this project have
   taken through design→red-team→prove→judge. Scoped separately below (Phase E, task E5) rather than
   folded into an ordinary task, matching how M2 separated its egress-proxy and policy-reachability
   ADR items into their own Phase F.
6. **Confirmed by the project owner (2026-08-06). 025 §5's mount-path-escape corpus goes through the
   full design→red-team→prove→judge cycle and produces its own ADR before landing** —
   `decisions/README.md` names "isolation boundary" as an unconditional ADR trigger, 025 §5 states
   the property in exactly those terms ("path escape is a security bug, not a bug"), and unlike
   `NativeJailBackend` (which carried forward ADR-004's already-spiked design as an ordinary task per
   M2's own precedent) there is no prior spike or ADR for worktree mount canonicalization to carry
   forward — this is the *first* design pass on it, held to the same evidentiary bar 008/009's own
   isolation-boundary choices (the WASM sandbox, native-jail, the egress proxy, TLS) were all held
   to. C2 below (path-escape corpus) is therefore explicitly an ADR-deliverable task, not an ordinary
   one — its own competing-canonicalization designs, red-team attacks, and executed evidence land in
   `decisions/ADR-0NN-worktree-mount-path-canonicalization.md` (number assigned when Phase C starts,
   after checking the current tip of the ADR sequence — ADR-013 is the latest as of this writing, but
   another concurrent session may have taken 014 in the meantime given the staged-file signal already
   noted in this doc's caveat above).
7. **025 §9's full gates are, like every milestone before this one, narrower here than the RFC's own
   promotion bar** — the same discipline M1/M2 both used. Specifically deferred past M3, named rather
   than silently dropped: G3's "10⁴ randomized interleavings" (scaled down to a machine-safe bounded
   count, same treatment C6 gave 008 §9 G4's 10⁵ teardown cycles); G4's real p99 cost measurement
   against 023 (023 stays `TBD-baselined` project-wide until M8, same status quo M2's own "what's
   deferred" section already established); the `remote`-profile half of anything 008/010 mention it
   for (M9, unchanged). 010 §9 G1's "identical chart artifact across the target platform set" and 026
   §8 G1's "declared threshold over a task corpus" both need a *reference agent* and a small task
   corpus that don't exist yet in this repo — building the smallest corpus that makes both gates
   real is in scope (Phase F/G below), but a large, curated corpus is explicitly not — same "narrower
   than the full gate, gap named not hidden" pattern.

## Tasks, in the roadmap's own build order

### Phase A — Worktree core object model (025 §2)

- **A1 (done, in-memory half only — see note below).** Real content-addressed `Blob`/`Tree` object
  model in `core/worktree.hpp`: `TreeEntry{name, digest, is_tree}`, `Tree{entries}` (canonical
  serialization via `canonical_tree_bytes`, sorted-by-name so identical content hashes identically
  regardless of insertion order), and the `WorktreeObjectStore` concept (`put_blob`/`get_blob`/
  `put_tree`/`get_tree`, `result<T>`-returning per M2 decision 2's synchronous-stand-in convention).
  `compute_digest` is real SHA-256 via Windows CNG/BCrypt (`src/core/worktree_digest.cpp`, new
  `agentengine::worktree_store` target, `WIN32`-gated) — the same "system API, not a third-party
  dependency" posture ADR-005's `capability_token.cpp` already established for HMAC-SHA256, a Linux
  provider named as the same kind of tracked gap ADR-005 itself left open, not silently assumed.
  `InMemoryWorktreeObjectStore` is the reference adapter, proven in
  `tests/test_worktree_object_store.cpp` (13 checks): dedup structurally verified via `blob_count()`/
  `tree_count()` staying at 1 after a duplicate put (022 §5 — digest equality alone doesn't prove
  "stored once"), canonical ordering (two insertion orders of the same entries hash identically),
  tree-diff-via-digest (a one-entry change produces a different digest, `get_tree` returns entries in
  canonical order), fail-closed lookup on an unknown digest, and `canonical_tree_bytes` proven
  directly as a pure function. `tests/smoke_vocabulary.cpp`'s stale `ae::Blob{"digest-1"}`
  construction (the old M0 stub's shape) was updated to the new vocabulary in the same pass — it
  would not otherwise have compiled. Full regression run after landing: Windows `ctest -j4` 35/36 (the
  one failure is the already-tracked, pre-existing `test_native_jail_backend_windows` `-j4` timing
  flake noted in this session's own memory, reconfirmed not a regression by its own prior history, not
  re-verified standalone this pass since it's already a known quantity). **Note — narrower than
  originally scoped**: decision 1's `pal::file_io`-backed durable adapter (the `Blob`/`Tree`
  equivalent of `FileStore`) is NOT built in this pass — only the in-memory reference adapter is,
  matching the size Quark's own `InMemoryStore` shipped before `FileStore` followed it. Tracked as
  **A1b** (open): a durable `WorktreeObjectStore` adapter over `pal::file_io`'s durable-write
  primitives, needed before Phase D's persistence work can be more than in-process. **M** (executed
  scope), **A1b unsized, not started**.
- **A2 (done).** `Ref` as real, `Store`-backed mutable state (decision 1), built on Quark's
  **EventSourced** model rather than snapshot-only: `RefMoved{tree_digest}` is the durable event,
  `RefState{tree_digest}` the folded state, `ref_actor_id(name)` bridges a human-readable Ref name
  to Quark's `uint64`-keyed `ActorId` (a hash of the name, tagged with `RefState`'s own 016
  fingerprint). `commit_ref`/`read_ref` (`core/worktree.hpp`) wrap `EventLog::stage`/`commit` and
  `recover_event_sourced` directly — no new persistence mechanism, matching decision 1's own framing.
  Chose EventSourced over plain snapshot-only specifically because it gives a Ref its own history for
  free (each committed digest is a retained, replayable log entry), which Phase D's rewind task (D2)
  needs and a snapshot-only Ref would not have provided without revisiting this task later. A small
  `quark::error` → `agentengine::error` conversion (`detail::from_quark_error`) was needed — the two
  error vocabularies don't share a type, a boundary this is the first worktree.hpp task to cross.
  Proven in `tests/test_worktree_ref_store.cpp` (14 checks): fresh commit, uncommitted-name read
  returns `nullopt`, round-trip, a Ref moved across three commits reflects the latest (not the
  first), and — the fencing claim specifically, not just a happy-path round trip (022 §5) — a
  stale-fenced writer's commit is rejected with a positive control proving the current fence's
  commit succeeds the same way. Full regression: Windows `ctest -j4` 36/37 (same pre-existing
  `test_native_jail_backend_windows` flake as A1, not a regression; +1 net test vs. A1's 35/36). **S**
- **A3 (done).** Compile-fail/positive-control proof, matching the M1/M2 `try_compile()` gate
  pattern. Sharpened from the original framing during execution: the actual property that makes
  `Blob`/`Tree` immutable is that `put_blob`/`put_tree` accept ONLY content, never a caller-supplied
  digest — the digest is always derived, so there is no entry point through which a caller could
  rebind an existing digest to different content (a local mutation of a fetched `Tree`/`vector<byte>`
  value is ordinary C++ and neither preventable nor meaningful to gate). Two negative controls
  (`tests/compile_fail/worktree_object_store_rejects_{blob,tree}_digest_override.cpp`, each proven
  independently rather than assumed to follow from the other) plus one positive control
  (`worktree_object_store_positive_control.cpp`, the real one-argument signatures). WIN32-gated,
  matching A1's own platform scope. Two real CMake mechanics surfaced and fixed while building this:
  `try_compile()`'s isolated mini-project cannot see the outer build's `agentengine::worktree_store`
  ALIAS target, so all three checks instead compile+link `src/core/worktree_digest.cpp` directly
  against the real `bcrypt` system library (a plain linker name, not a CMake target); and `try_compile`
  results are cached (`INTERNAL` cache variables), so the standard "temporarily break it, confirm the
  gate fires, revert" verification this project uses for its most rigorous checks needed the specific
  cache variables cleared (`cmake -U...`) between runs, not just a plain reconfigure — a plain
  reconfigure silently reused the prior (stale) result and would have given a false "still passing"
  read. Verified load-bearing this way: temporarily added the forbidden `put_blob(Digest const&,
  span)` overload, confirmed `FATAL_ERROR` correctly fired, reverted, reconfirmed all three checks
  pass clean again. Deliberately linked EVEN the negative controls against the real digest
  implementation (not left unlinked) — otherwise a negative control failing to build for the wrong
  reason (a permanently missing link dependency) could stay "green" forever regardless of whether the
  forbidden overload actually existed, exactly the vacuous-test risk this verification step exists to
  rule out. Full regression: Windows `ctest -j4` 36/37, same pre-existing flake, unchanged. **S**

### Phase B — Sub-worktrees, sharing modes, concurrency and merge (025 §3-4)

- **B1 (done).** Sub-worktree creation with a declared `sharing_mode` (`shared`/`branch`/`readonly`/
  `scratch`, 025 §3) over A1/A2. Modeled as a `SubWorktree{name, backing_ref_name, mode,
  pinned_digest}` value naming which `Ref` (if any) reads/writes actually go through, rather than a
  separate name→name alias registry: `shared`'s `backing_ref_name` IS the parent's own `Ref` name (no
  new `Ref` created at all), so immediate cross-visibility falls out of `commit_ref`/`read_ref`
  unchanged rather than needing special-case logic; `branch` is copy-on-write (a new, independent
  `Ref` seeded at the parent's current digest); `scratch` seeds at a universal, deterministic
  `empty_tree_digest()` instead of the parent's; `readonly` has no `Ref` at all (`backing_ref_name`
  empty), so `write_sub_worktree` fails closed on the mode itself, before the store is ever
  consulted. Default-by-concurrency (025 §3) is, as scoped, a caller-supplied flag — not inferred
  here, since that needs 001's run/turn scheduling context this header doesn't have. Proven in
  `tests/test_worktree_sub_worktree.cpp` (22 checks): `shared`'s claim proven as TRUE immediate
  cross-visibility (a write through the sub-worktree is read back through the parent's own name
  directly), not merely an identical starting value; `branch`/`scratch` proven to diverge
  independently in BOTH directions (a write to the child doesn't move the parent, AND a later move
  of the parent doesn't move the already-created child — ruling out an accidental live alias);
  `readonly` proven to stay pinned even after the parent later moves, and its write-rejection proven
  specifically (a positive control shows the identical write succeeding against a `branch` of the
  same parent). WIN32-gated (like A1) since `scratch` calls `empty_tree_digest()` ->
  `compute_digest()`. Full regression: Windows `ctest -j4` 37/38, same pre-existing flake, unchanged.
  **M**
- **B2 (done).** Three-way merge on `branch` join (025 §4) — disjoint changes merge automatically,
  identical content merges trivially, a genuine conflict fails closed and retains both versions
  (never last-writer-wins, matching 025 §4's explicit rule). Two layers over A1/B1:
  - `merge_trees`/`detail::merge_subtrees` — a pure algorithm over `WorktreeObjectStore` only (no
    `Ref`, no commit). At each tree level: `ours == theirs` → trivial; `ours == base` → theirs wins
    wholesale (only theirs changed); `theirs == base` → ours wins wholesale; otherwise recurse per
    entry name over the union of base/ours/theirs names. Per name: identical on both sides (including
    both absent — delete/delete) → trivial; unchanged from base on one side → the other side's value
    wins (covers adds, edits, and deletes each propagating automatically when only one side touched
    them); otherwise a genuine two-sided divergence — recurse one level if both sides still agree it's
    a subtree (so a change two levels deep in one branch and an unrelated change two levels deep in
    the parent never conflict at a shared ancestor directory, matching §4's "disjoint changes merge
    automatically" literally, not just at the top level), else record a `MergeConflict{path, base,
    ours, theirs}` (covers content forks, add/add divergence, edit/delete forks, and blob-vs-tree type
    forks) and keep scanning the rest of the tree rather than stopping at the first collision, so every
    conflict is reported in one pass. A tree with any conflict commits nothing — `MergeResult::
    merged_tree_digest` is only valid when `conflicts.empty()`.
  - `merge_branch_into_parent` — the orchestration layer over `merge_trees` plus `Ref`: reads the
    branch's own current tree, merges it against `SubWorktree::base_digest` (the parent's digest at
    the moment the branch was created — a field B1 didn't need but B2 does, added to `SubWorktree` and
    populated in `create_sub_worktree`'s `branch` case) and the caller-supplied `expected_parent`. On
    a clean merge, re-reads the parent's `Ref` live and refuses to commit if it no longer matches
    `expected_parent` (`worktree.merge_stale_parent`) — a caller who observed the parent, then lost a
    race to someone else's commit, is told to re-read and retry rather than silently overwriting that
    other commit. This narrows, but by the code's own admission does not eliminate, the TOCTOU window
    between that recheck and `commit_ref`'s own append — full elimination needs the read-merge-commit
    sequence inside one Quark-actor turn (025 §4's "one writer per tree"), which a seam-level function
    called directly (not yet wrapped in an actor) can't itself guarantee; **B4 is where this gets
    stress-tested under real concurrent load**, and if the residual window proves reachable there, it
    drives a real fix (e.g. a compare-and-set primitive on `Store`) rather than living with the
    best-effort recheck indefinitely.
  - `/conflicts/<path>.<agent>` materialization (surfacing conflicts as actual guest-visible files) is
    NOT part of B2 — that needs the mount/filesystem layer (Phase C/F), which doesn't exist yet. B2
    produces the conflict *data* (`MergeConflict`, in memory); where it gets written for a human or
    supervising agent to see is a later integration concern, not invented here ahead of its own
    dependency.
  - Model-drafted conflict resolution (025 §10 Q1: a proposal, never auto-applied, I3's "model output
    is data") has no code here either — B2 is the deterministic algorithmic merge only; a drafting
    workflow needs the tool-invocation and approval-gate machinery (007, 006 §4) this header doesn't
    own.
  Proven in `tests/test_worktree_merge.cpp` (37 checks): disjoint top-level additions (B2-C1), identical
  edits merging trivially (B2-C2), a genuine divergent edit surfacing as a conflict with all three
  versions retained (B2-C3), recursion actually happening — disjoint changes inside a commonly-touched
  subdirectory merge clean (B2-C4) while a real conflict nested inside that same subdirectory is still
  caught at its full path rather than masked by the sibling change (B2-C5), delete/delete merging
  trivially vs. an edit/delete fork surfacing correctly with the deleted side shown as absent (B2-C6/
  C6b), add/add identical vs. divergent (B2-C7), a blob-vs-tree type fork (B2-C8); `merge_branch_into_
  parent`'s happy path actually committing and being independently re-readable (B2-R1), a failed merge
  leaving the parent `Ref` provably UNCHANGED — not just returning an error value (B2-R2), and the
  stale-parent race check catching a real interleaving (another writer's commit lands between the
  caller's observation and the merge call) with the other writer's commit surviving untouched (B2-R3).
  WIN32-gated (builds real Blob/Tree fixtures via `put_blob`/`put_tree`, same real-crypto dependency as
  A1/B1). Full regression: Windows `ctest -j4` 38/39, same pre-existing `test_native_jail_backend_
  windows` flake, unchanged. **L**
- **B3 (done).** `shared`-mode staleness note (025 §3/§10 Q2's resolution) — a short diff-summary
  computed when a `shared` sub-worktree has moved since an agent's last read, surfaced proactively at
  the start of a turn rather than only at merge time. Two pieces, standalone rather than layered on
  B2's three-way `merge_trees` (a two-way old-vs-new diff has nothing to *decide*, so it's the smaller
  primitive B2's algorithm could in principle be built from, not the reverse — written that way rather
  than forced through the three-way shape for reuse's sake):
  - `diff_trees`/`detail::diff_subtrees` — recursive two-way file-level diff between an old and a new
    tree digest. Per name: unchanged (same digest+kind) → silent; present on only one side → every
    leaf under it walked and recorded (`detail::collect_leaves_as`) as wholly added/removed, so a
    whole added/removed subdirectory reports one entry per actual file, not one entry for the
    directory name; both present but changed → recurse if both sides still agree it's a directory
    (an unrelated addition next to an unrelated addition inside a commonly-touched directory doesn't
    collapse into one misleading "dir changed" line), a single `modified` entry if both are blobs, or
    a full removed+added split if one side is a blob and the other a tree (a file becoming a directory
    reported per-file, not as one confusing "modified").
  - `summarize_diff` — a short, **count-only** line ("3 files changed (1 modified, 1 added, 1
    removed)") matching 025 §4's own phrasing exactly: what changed, not what it changed to or which
    paths — `TreeDiff::changes` still carries full paths for a caller that wants them (an audit log),
    only the model-facing summary stays path- and content-free. **Named gap, not silently assumed**:
    the RFC's own example text also names the writer ("changed by `writer`"); `Ref`/`RefMoved` (Phase
    A2) record only a tree digest, no committer identity, so attribution isn't in this summary — a
    candidate for whichever later phase threads an agent/run identity through a commit (Phase D's
    turn-boundary work is the likely home), not invented here ahead of that seam existing.
  - `check_shared_staleness`/`SharedStalenessNote` — the orchestration: compares a `shared` sub-
    worktree's live digest against a caller-supplied `last_read_digest`, returning the diff only when
    they differ (an unchanged tree never touches `diff_trees` at all). Fails closed with a distinct
    code on any non-`shared` mode — `branch`/`scratch` are private until an explicit merge and
    `readonly` is pinned at creation, so none of the three can ever be "stale" in this sense, and
    silently returning "not stale" for them would be worse than refusing to answer the question at
    all.
  Proven in `tests/test_worktree_staleness.cpp` (27 checks): identical trees diff to nothing (B3-C1);
  single added/modified/removed top-level files reported with the right kind (B3-C2/C3/C4); a whole
  added subdirectory correctly counted per-file rather than per-directory (B3-C5); a disjoint addition
  inside a commonly-touched directory doesn't drag in its unchanged sibling (B3-C6); a blob↔tree type
  change reported as a legible removed+added split (B3-C7); `summarize_diff`'s breakdown text checked
  exactly for a mixed one-of-each-kind diff (B3-C8); `check_shared_staleness` proven not stale
  immediately after a read (B3-R1, no false positive), genuinely stale end-to-end when a SIBLING's
  shared sub-worktree (same backing `Ref`, different `SubWorktree` value) writes between the agent's
  reads — with a positive control that re-reading clears the staleness rather than it staying sticky
  (B3-R2) — and rejecting a `branch`-mode sub-worktree with a stable error code (B3-R3). WIN32-gated
  (same real-crypto dependency as A1/B1/B2). Full regression: Windows `ctest -j4` 38/40 (the known
  `test_native_jail_backend_windows` flake plus, this run, `test_native_jail_parity_windows` failing
  the identical OOM-vs-timeout assertion under `-j4` contention — confirmed NOT a regression by
  re-running both single-threaded: parity passes clean alone, backend fails on the same assertion even
  alone, matching the already-documented flake exactly; neither test touches worktree code). **S**
- **B4 (done).** Concurrency proof (025 §9 G3, scoped per decision 7) — N concurrent `branch` agents
  produce a deterministic merge result over a machine-safe bounded randomized-interleaving count;
  every genuine conflict is surfaced, never silently resolved; a positive control (a deliberately
  miswired auto-apply path) is caught, proving B2's "never last-writer-wins" claim isn't vacuous.
  **Concurrency here is a single-threaded, discrete-event simulation, not real OS threads** — an
  explicit design choice, not an implicit shortcut: real threads calling into
  `InMemoryStore`/`InMemoryWorktreeObjectStore` (plain `unordered_map`s, no internal locking) would be
  a data race in the *test harness itself*, unrelated to what this is trying to prove, and CLAUDE.md's
  Machine Safety section rules out spawning `hardware_concurrency()` threads regardless. Instead: N
  agents all "observe" the identical parent snapshot (the worst-case, maximally contentious starting
  point — everyone began before anyone committed), then their merge attempts are dispatched one at a
  time in a randomized order across many trials, with the actual TOCTOU race B2 built defenses for
  genuinely exercised (not merely simulated by assertion) every time a non-first agent's stale
  snapshot is rejected.
  - **`retry_merge_branch_into_parent`** (new production code, not test-only) — the response B2's own
    `worktree.merge_stale_parent` error message implies but B2 didn't build: tries the caller's
    already-observed parent first (the ordinary case, no extra read), and only re-reads live on a
    subsequent attempt, specifically because the prior one was rejected as stale. A genuine conflict
    is never retried — it's a real, terminal result returned immediately, exactly like a clean merge.
  - `tests/test_worktree_branch_concurrency.cpp` (5 checks, but each an aggregate over hundreds of
    trials — 025 §9 G3's claim is a property of the whole run, not any one trial in isolation; a
    per-trial failure is still reported immediately by number, not averaged away): **B4-C1** — 500
    trials, 5 agents each making a disjoint edit, dispatched in a random order every trial; every
    trial ends with the parent containing all 5 additions (no lost update), and the stale-parent path
    is proven to fire *exactly* `trials × (agents − 1)` times — not zero (which would mean the test
    never actually raced anyone) and not more (which would mean something is retrying when it
    shouldn't need to). **B4-C2** — 500 trials, two agents editing the SAME file differently; whichever
    wins the race merges cleanly, the other's retry surfaces a real conflict every single trial,
    regardless of dispatch order — never silently resolved, never last-writer-wins. **B4-C3** (positive
    control) — the identical B4-C1 scenario run through a deliberately miswired
    `naive_last_writer_wins_merge` (blind overwrite, no merge check at all) instead of the real path:
    it loses updates on all 50 control trials, proving B4-C1's "no lost update" assertion is a real
    gate, not one that would pass regardless of what the merge logic did (022 §5). Runtime measured at
    ~120ms for the full 1 050-scenario run (well inside the 60s CTest timeout set on this test per
    CLAUDE.md's Machine Safety section) — cheap enough that 500 was chosen as a meaningfully large,
    still-bounded count (decision 7), not the smallest number that would pass; unlike `native-jail`'s
    300-cycle reduction from 008 §9 G4's 10⁵ (bounded by real per-cycle AppContainer/Job-object cost),
    this is pure in-memory CPU work, so the reduction from the RFC's own 10⁴ is a deliberate scope
    decision, not a cost-forced one. WIN32-gated (same real-crypto dependency as A1/B1/B2/B3). Full
    regression: Windows `ctest -j4` 40/41, same pre-existing `test_native_jail_backend_windows` flake,
    unchanged. **L**

### Phase C — Mounts, capabilities, path-escape hardening (025 §5) — ADR-track, see decision 6

- **C1 (done).** `FsRead<mount>`/`FsWrite<mount>` capability-gated mount resolution over A1/B1 — a
  worktree subtree becomes a guest-visible filesystem view only through a granted capability (007
  §3), mount points are canonical/ordinary-looking paths (026 §2), never a runtime-revealing path.
  Consumes `trust/capability.hpp`'s EXISTING `cap::FsRead`/`cap::FsWrite` (ADR-009) directly — this
  phase does not invent a second capability shape, and reuses that header's own
  `capability_detail::path_prefix_covers` for scope checks rather than a second path-matching
  routine. **Explicitly NOT 025 §5's OS-level path-escape corpus** (that's C2, ADR-track per decision
  6): a `Tree` is a plain `name -> digest` map with no parent pointers, no symlinks, no ADS — `..` has
  no "walk up" to perform, so `split_mount_path` rejects it outright as malformed input rather than
  defending against it via canonicalize-then-check (the fragile pattern C2's whole corpus exists
  because that pattern is where real path-escape bugs hide). C2 hardens the DIFFERENT, later
  mechanism that materializes a mount onto a real OS filesystem for a sandboxed process to see, which
  doesn't exist yet — this phase does not get ahead of it.
  - `Mount{mount_id, ref_name, subtree_path}` — a host-side binding from a guest-visible `mount_id`
    to a concrete worktree location, never derived from guest input (I2), the same posture every
    `cap::*` payload already has.
  - `mount_read`/`mount_write` — both check `granted.mount_id == mount.mount_id` and
    `path_prefix_covers(granted.path_prefix, guest_path)` BEFORE any store access at all (007 §3 —
    the grant is the authority, checked before the effect, not after). `mount_write` walks
    `detail::set_entry_at_path` down `mount.subtree_path` + the guest path as one combined segment
    list (not two separate descend-then-reascend phases), creating missing intermediate directories
    via `detail::ensure_empty_tree` (a REAL, `put_tree`'d empty tree — unlike bare
    `empty_tree_digest()`, which only computes what the digest would be) and propagating the change
    to a new ROOT digest in one recursive unwind, so a write through a nested mount correctly leaves
    sibling subtrees at the ref's actual root untouched. `granted.size_cap_bytes` (read) is enforced
    after the fetch (only knowable once the blob is in hand); **`granted.quota_bytes`/
    `file_count_cap` (write) are deliberately NOT enforced here** — that numeric check is C3's own
    task, proven against this mount layer directly, not duplicated ahead of it (a named gap, not a
    silent omission).
  Proven in `tests/test_worktree_mount.cpp` (34 checks): `split_mount_path`'s own contract in
  isolation (a leading `/`, a trailing `/`, `//`, `.`/`..` all rejected, an ordinary path splits
  correctly) (C1-C1); `mount_read` happy path, a capability for a different `mount_id` rejected, a
  path-prefix-scoped capability rejected outside its scope with a positive control proving the SAME
  capability reads fine inside it, a `size_cap_bytes` rejection with a positive control under a larger
  cap, reading a directory as a file rejected, reading a nonexistent path rejected (C1-C2);
  `mount_write` creating a new file, creating missing intermediate directories, overwriting an
  existing file WITHOUT disturbing an unrelated sibling file (C1-C3); the same capability-mismatch/
  out-of-scope/positive-control pattern for `mount_write`, plus writing through an existing FILE as
  if it were a directory rejected, and writing to the mount root itself (empty path) rejected (C1-C4);
  a `subtree_path`-rooted mount (`/work` → ref's `work/` subtree) writing correctly AND leaving
  sibling `/input`/`/skills` mounts of the SAME underlying ref completely untouched — the real test
  that root-propagation targets only the written branch (C1-C5); both `mount_read`/`mount_write`
  failing closed (not silently treating it as an empty tree) against a mount whose ref was never
  committed (C1-C6). WIN32-gated (same real-crypto dependency as A1/B1/B2/B3/B4). Full regression:
  Windows `ctest -j4` 41/42, same pre-existing `test_native_jail_backend_windows` flake, unchanged.
  **M**
- **C2 (done). ADR-track (decision 6) — `decisions/ADR-014-worktree-mount-path-canonicalization.md`,
  Judged.** New `core/worktree_mount_fs.hpp`/`.cpp` (linked into the existing
  `agentengine::worktree_store` target): `open_within_mount_root` opens a guest-relative path
  against a REAL OS directory (the not-yet-built materialization Phase C1's own header comment
  deferred) with a single handle-based `CreateFileW`, then verifies containment from the resolved
  HANDLE (`GetFinalPathNameByHandleW`) rather than from a re-parsed string — the design decision
  this ADR's whole prove phase turns on. The competing, rejected design (lexical canonicalize →
  string-prefix-check → reopen the checked string later) was built too, kept as a permanent
  `redteam::naive_check_within_root`/`naive_open_checked_path` regression control (same precedent as
  B4's `naive_last_writer_wins_merge`), and proven vulnerable to TOCTOU **deterministically**: a
  real scratch directory is checked, then swapped by hand for a junction pointing outside the mount
  (the exact state a real racing attacker needs, made reproducible instead of timing-dependent —
  B4's discrete-event-simulation precedent applied to a different property), then the naive design's
  reopen reads the swapped-in OUTSIDE content despite the check having validated an INSIDE path. The
  accepted design proven immune to the identical interleaving two ways: a handle opened BEFORE the
  swap keeps reading its original content afterward (Windows handles reference the file object, not
  the path), and a FRESH request made AFTER the swap re-resolves current reality and is correctly
  rejected — no cached, staleable "validated" answer either way. A real production bug was found and
  fixed during this proof: the target handle initially lacked `FILE_SHARE_DELETE`, which would let a
  guest's still-open handle block a host-side delete/replace; caught by the TOCTOU test's own setup,
  not a separate review pass. Full corpus (`tests/test_worktree_mount_fs_escape_corpus.cpp`, 22
  checks, real Win32 I/O against a scratch temp directory, junctions created via `cmd.exe /c mklink
  /J` matching ADR-004's own shell-out-for-setup precedent): lexical `..`/absolute-redirect rejected
  pre-syscall; a junction crossing the mount boundary rejected with a positive control proving an
  identical in-mount junction is followed, not blanket-denied; ADS and `\\?\` rejected structurally
  by a small forbidden-character check (backslash/colon/NUL) layered on top of `split_mount_path`'s
  existing segment grammar, before any syscall; a real on-disk fullwidth-dot (`U+FF0E` x2) directory
  name proven treated as an ordinary opaque name, never conflated with literal `..` in either
  direction; case-insensitive resolution confirmed still contained. **Named, untested residuals**
  (not silently assumed complete): 8.3 short-filename aliasing (reasoned-covered by the same
  `GetFinalPathNameByHandleW` mechanism, not executed — 8.3 name generation is commonly disabled by
  default and this machine's volume config wasn't controlled for); real symbolic links as distinct
  from junctions (junctions need no special privilege and were used throughout; symlinks share the
  identical OS resolution mechanism, expected but not tested to generalize, matching ADR-004 §6.4's
  own honesty about its untested LPAC case); and, named as the largest residual by the ADR itself —
  nothing yet forces Phase E's `PythonRunner`/`ShellRunner` to actually route every guest-reachable
  filesystem operation through this primitive once built (ADR-003's own missed-entry-point history is
  the cited precedent for why this is flagged rather than assumed). Full regression: Windows
  `ctest -j4` 42/43, same pre-existing `test_native_jail_backend_windows` flake, reconfirmed via a
  standalone single-threaded re-run to fail identically alone (not a side effect of this work).
  **XL**
- **C3 (done).** `mount_write` (core/worktree.hpp) now enforces `granted.quota_bytes`/
  `file_count_cap` for real — the gap C1's own header comment named and deferred. Two new `detail`
  helpers: `resolve_subtree_digest` (finds the Digest of `mount.subtree_path` within a candidate new
  root — quota is scoped to THIS mount's subtree, never the whole ref, since one ref can host several
  independently-quota'd mounts at different `subtree_path`s) and `subtree_usage` (recursively sums
  every reachable Blob's size and counts every reachable Blob leaf under a subtree). The candidate
  tree is built first, usage is recomputed against IT, and `commit_ref` only runs if both caps still
  hold — a rejected write leaves the Ref completely unchanged, never committed-then-flagged. Usage is
  recomputed from the tree on every write rather than tracked as running state (the content-addressed
  store has no side state to keep in sync by construction), a named O(subtree size) cost accepted
  under 025 §9 G4's project-wide deferral of real p99 measurement past M3. Error framing matches
  026 §3's mapping table verbatim: `failure_class::resource`, message `"No space left on device"` —
  an ordinary OS-shaped message a future guest-facing translator (Phase E) can raise as-is, not a
  policy identifier to reword. Proven in `tests/test_worktree_mount_quota.cpp` (26 checks, same
  in-memory content-addressed store as C1 — no real filesystem needed at this layer): a write under
  both caps succeeds (C3-C1); byte-quota and file-count rejections each confirmed
  `failure_class::resource` with the exact 026 §3 message, and the Ref proven byte-for-byte unchanged
  by re-reading its tree digest before/after (C3-C2/C3-C3); the file-count cap's boundary proven
  inclusive (exactly AT the cap succeeds) (C3-C3); quota proven cumulative across separate writes,
  with the first write's content surviving the second write's rejection — only the offending write is
  rejected, not a rollback of history (C3-C4); the identical oversized write from C3-C2 proven to
  succeed under an uncapped `FsWrite` — `std::nullopt` means uncapped, never "0" (C3-C5); two sibling
  mounts on the SAME ref at different `subtree_path`s proven to have fully independent budgets, one
  mount's usage never inflating the other's (C3-C6); overwriting a file with smaller content proven
  to reduce recomputed usage, with a write C3-C7 rejects against the old (larger) size later
  succeeding once the old content shrinks — proving recomputation from final tree state, not a stale
  running delta (C3-C7). Full regression: Windows `ctest -j4` 43/44, same pre-existing
  `test_native_jail_backend_windows` flake, unchanged. **S**
- **C4 (done).** Linux path-escape parity pass over C2/ADR-014's Windows corpus — an ordinary
  follow-on task, no second ADR (decision 6: ADR-014 already settled the design question; nothing
  here is a first design pass), same sequencing precedent M2 used for `native-jail` (Windows first,
  Linux parity as its own dated task). New `core/worktree_mount_fs_posix.hpp`/`.cpp`
  (`SafeFileHandlePosix`, `open_within_mount_root`, `redteam::naive_check_within_root`/
  `naive_open_checked_path`) ports Design B's structural property exactly: `open()` resolves
  symlinks transparently the same way `CreateFileW` does, and `readlink("/proc/self/fd/N")` reads
  the kernel's own record of what the resulting descriptor actually refers to — the same "verify
  from the object opened, not a re-parsed string" property, using the mechanism ADR-014 §8.3 itself
  anticipated ("`/proc/self/fd`-based re-verification" where `openat2(RESOLVE_BENEATH)` kernel
  support can't be assumed as a floor) rather than a fresh design pass. Deliberate signature
  divergence from the Windows header, not an oversight: POSIX `open()` folds Win32's split
  `desired_access`/`creation_disposition` into one `flags` bitmask — matching each OS's own
  idiomatic shape is what "isolation parity is a gate, not identical shape" (CONVENTIONS.md) asks
  for. `agentengine::worktree_store`'s CMake target gained a `NOT WIN32` branch (root
  `CMakeLists.txt`) linking only `worktree_mount_fs_posix.cpp` — no digest provider, since nothing
  here needs `compute_digest`/the content-addressed store, so this is not blocked on decision 2's
  still-open Linux SHA-256 gap. Proven in
  `tests/test_worktree_mount_fs_escape_corpus_linux.cpp` (21 checks, real unprivileged Linux
  filesystem I/O — `symlink()` needs no special privilege at all, unlike Windows junctions, so this
  corpus is if anything less environment-dependent than the Windows one): the identical TOCTOU
  interleaving reproduced deterministically, with an even more direct proof-of-immunity than Windows
  needed (a Linux fd keeps referencing its original inode after the directory entry pointing to it
  is removed and replaced, with no `FILE_SHARE_DELETE`-equivalent flag required at open time); a
  crossing symlink rejected with an in-mount symlink followed as the paired positive control; an
  embedded-NUL segment rejected structurally. **Named platform divergences, not silently assumed to
  generalize**: ADS and `\\?\` have no Linux analog (N/A, not a gap); 8.3 aliasing is Windows-only
  (N/A); case handling runs the *opposite* direction (ext4 case-sensitive by default vs. NTFS
  case-insensitive) — tested explicitly (C4-8) as a documented behavioral difference, not a security
  property. ADR-014 amended with a dated addendum (§9) closing its own Linux-parity reopening
  trigger; `decisions/README.md`'s ADR-014 entry updated to match. Windows regression unaffected (the
  `NOT WIN32` CMake branch is inert there): `ctest -j4` 44/45, same pre-existing
  `test_native_jail_backend_windows` flake, unchanged. Linux regression (WSL2 Ubuntu, kernel
  6.6.87.2, gcc 15.2.0): full `ctest -j4` 25/25 (one unrelated, by-design skip —
  `test_shell_runner_no_process_creation`). **L**

### Phase D — Persistence, checkpoints, lifecycle (025 §6) — partial, real gaps deferred to M4

- **D1 (done).** Turn-boundary commit (`core/worktree.hpp`) — `commit_turn` wraps the same fence+
  append machinery `commit_ref` already uses (factored into a shared `detail::commit_ref_impl`, so
  `commit_ref`'s own behavior and every existing caller — `mount_write`, `merge_branch_into_parent`,
  `create_sub_worktree` — are untouched) and additionally surfaces the commit's own `SeqNo` as
  `TurnCommit::turn`: 025 §6's "a turn's committed tree digest is recorded with the turn," made real
  by reusing A2's own retained, replayable log entry as the turn's identity rather than inventing a
  parallel ledger — exactly what A2's own header comment already earmarked this task to do. **S**
  (narrower than the original **M** estimate: the whole task turned out to be "stop discarding a
  value `EventLog::commit()` already returns," not new machinery).
- **D2 (done).** Rewind as ref reassignment (025 §9 G5, `core/worktree.hpp`) — `turn_digest_at` reads
  a ref's own log tail from exactly the requested `SeqNo` and returns that turn's digest only on an
  EXACT seq match (fails closed on a compacted-away or never-committed turn, never silently hands
  back a neighboring commit); `rewind_to_turn` fetches that digest and re-commits it via
  `commit_turn`, so `read_ref`/`mount_read` see the restored tree immediately. Deliberately generalized
  beyond `commit_turn`-originated points: `turn_digest_at` resolves ANY retained `SeqNo` on a ref
  (a mount write, a merge, a sub-worktree branch commit), matching G5's own "an *arbitrary* retained
  turn digest" wording rather than a narrower "only turns `commit_turn` itself minted." Rewind is
  proven non-destructive, not merely asserted: a SECOND `rewind_to_turn` recovers the exact state
  that existed just before the first one, since a rewind is itself a new, ordinary retained log entry
  rather than a history edit. Proven in `tests/test_worktree_turn_commit.cpp` (30 checks, real
  Blob/Tree fixtures, same real-crypto dependency as A1/B1-B4/C1): strictly increasing turn numbers
  on repeated commits with per-name isolation (D1-C1/C2, D1-R1); a 3-turn history rewound to turn 2
  with the fetched tree's content verified directly, not just digest equality (D2-C1); the live
  `read_ref` head proven to move backward, not merely the return value (D2-C2); the rewind's own
  non-destructiveness proven via a second rewind recovering the pre-rewind turn exactly (D2-C3); a
  turn-0/past-the-end/never-committed-name negative trio each failing closed with the identical
  `worktree.turn_not_found` code, paired with a positive control at the adjacent valid turn (D2-C4);
  and an integration check through `mount_read` itself — a guest-facing read sees old content after a
  rewind through the ordinary read path, not just via direct `Digest`/`Tree` inspection (D2-C5). Full
  regression: Windows `ctest -j4` 44/45, same pre-existing `test_native_jail_backend_windows` flake,
  unchanged. **S**
- **D3. Deferred to M4 (019, Durability and Long-Running Agents), named not silently dropped:** full
  session-checkpoint integration (025 §9 G1's "survives sandbox destruction, process restart, and
  simulated node migration"), retention/GC policy (025 §6), and redaction reaching the object store
  (025 §9 G6, 005 §6's redaction contract) — all three need 019's suspension/recovery machinery this
  milestone does not build, the same way M1 deferred 001 §9 G1's 10⁴-session gate and M2 deferred
  019-dependent items generally.

### Phase E — `ExecState`, `PythonRunner`, `ShellRunner` (010 §1a, §2, §3a) — see decisions 3-5

- **E1 (done).** `SessionExecStateRegistry` (`sandbox/runner.hpp`) — the "held by the sandbox" half
  of 010 §3a that `ExecState{cwd, env}` alone didn't provide: `get_or_create(session_id)` hands back
  the SAME `ExecState&` for a given session on every call (an `unordered_map` keyed by session id —
  reference/pointer stability across further insertions is a guaranteed container property, which is
  exactly what lets this keep handing out the identical object), and `destroy(session_id)` is a real
  teardown (a later `get_or_create` for the same id constructs a genuinely fresh object, never
  resurrects the old one). `PythonRunner`/`ShellRunner` (E2/E3) don't exist yet, so §3a's own two
  worked examples ("a `cd` in a shell command mutates the same ExecState a subsequent `execute_code`
  call reads", and the reverse direction via `os.chdir()`) are reproduced using two minimal,
  `static_assert`-proven `Runner`-concept-conforming mock runners (test-only) rather than waiting on
  the real ones. Proven in `tests/test_exec_state.cpp` (15 checks): default-constructed on first
  access; a second `get_or_create` for the same id observes an earlier mutation through a genuinely
  identical object (`&first == &second`, not just equal content); both §3a directions reproduced
  literally via the mock runners; two sessions proven never to observe each other's mutation;
  destroy-then-recreate proven to yield a fresh object, not a resurrected one. Header-only, no
  platform dependency — full regression clean on both platforms: Windows `ctest -j4` 45/46 (same
  pre-existing `test_native_jail_backend_windows` flake), Linux (WSL2 Ubuntu, gcc 15.2.0) `ctest -j4`
  26/26 (one unrelated, by-design skip). **S**
- **E2 (done, one residual named — Stage C).** `MediatedPythonRunner`
  (`src/backends/native_jail/mediated_python_runner.{hpp,cpp}`) — a genuinely new translation unit
  per decision 4's literal reading (confirmed with the project owner mid-task once direct code
  inspection showed `python_runner.hpp`/`python_lockdown.{hpp,cpp}` are already real, tested, non-
  stub code, not the "prove-phase spike" this doc's own "Current state" table had assumed —
  decision 4 was re-confirmed as still the intended path regardless). `python_runner.hpp`/
  `python_lockdown.{hpp,cpp}` stay completely untouched (verified: all their own existing tests
  still pass, unmodified — `test_python_embed_smoke`, `_layer0_sweep`, `_meta_path_finder`,
  `_numpy_pandas_import`, `_audit_hook`, `_caller_gated_import`, `_caller_gated_benchmark`,
  `_trusted_loader_proxy_heap_type_negative_control`, 19/19 alongside the worktree suite). The new
  class lives in `agentengine::native_jail::MediatedPythonRunner` (not `agentengine::PythonRunner`)
  specifically to avoid colliding with the untouched spike's own class name in the same namespace.
  Built, as new code, citing which ADR finding each piece carries forward:
  - **Stage A (embedding + `ExecState`)**: `Py_InitializeFromConfig` with `isolated=1`/
    `site_import=0` (ADR-002's embedding finding), a Layer-0 `sys.modules` keep-set (ADR-002 §3.0's
    measured baseline, reused as DATA not code) — extended with a NEW finding this design needed
    that ADR-002 never did: `os`/`socket`/`subprocess` are pinned into the keep-set (plus whatever
    they transitively pull in, captured by snapshotting `sys.modules` right after the mediation
    bootstrap rather than hand-enumerating a closure — avoiding ADR-002 §10.1's own cautionary
    ~130-name-guess history) so a guest re-`import`-ing one always gets the SAME, already-mediated
    module object, never a freshly re-executed, unpatched one. `ExecState.cwd`/`.env` sync happens
    at call boundaries (real `SetCurrentDirectoryW`/environment-block sync in before `run()`, read
    back out after) rather than continuous interception — correct because this process hosts exactly
    one session (ADR-002 §5.5.6's scope, unchanged), proven bidirectionally in
    `tests/test_mediated_python_runner_smoke.cpp` (E2-C3/C4: a variable AND a script's own
    `os.chdir()` both survive into the next call on the same Runner, matching 010 §9 G3 literally).
  - **Stage B (import allowlist)**: a fresh custom `PyTypeObject` meta-path finder (ADR-002 §3.1's
    "gate by module name before any loader runs" finding, reimplemented) delegating allowed names to
    the captured original finders. The effective allow-set is session-config-derived
    (`package_policy_allowlist`, 010 §5's not-yet-wired package policy) rather than literally
    re-read from `EffectContext` per call — a scope clarification, not a shortfall: there is no
    `cap::*` kind for "which packages may be imported" (`capability_kind`'s table has no
    import/package entry), so "per-call capability freshness" is real where a capability genuinely
    exists (Stage D below), not forced through a nonexistent one for imports specifically.
  - **Stage C (ADR-003's caller-aware gating tier) — NOT built this pass, a named residual, not
    silently dropped.** `caller_gated_modules` is accepted in `MediatedPythonConfig` but inert: a
    name placed there is denied to everyone (fail-closed, never a silent widening relative to not
    having the field at all). Scoped out because (a) it only matters once a package policy grants a
    heavy transitive-dependency package like numpy/pandas — no caller in this codebase populates
    that pipeline yet — and (b) it is ADR-003's single riskiest mechanism by its own account: that
    ADR's text documents THREE independent rounds (design, red-team, an initially-clean prove pass)
    each missing a real skip-anchor gap before a fourth, careful re-read found it. Reproducing it
    faithfully deserves its own dedicated pass, tracked here for Phase E4 or a follow-up task, not
    appended to an already-large one.
  - **Stage D (`open`/`socket`/`subprocess` mediation, 010 §9 G7's second claim — entirely unbuilt
    in the spike)**: a small, private (never `sys.modules`-registered) `_ae_internal` C module
    exposes `open(path, mode)`/`check_net(host, port)`; a bootstrap Python script (run once, in a
    throwaway namespace guest code can never reach) rebinds `builtins.open`/`io.open`,
    `socket.socket.connect`, and denies `subprocess.Popen.__init__`/`os.system`/`os.popen`/the
    `os.exec*`/`os.spawn*` family outright (composition via `RunnerCall<shell>` is E3's job, not yet
    wired — named, not silently gapped). `open()`'s mediation reuses `core/worktree_mount_fs.hpp`'s
    `open_within_mount_root` directly — that primitive's own header names itself as exactly what a
    future `FileSystemAdapter`/caller should call, so reusing it is the documented intent, not a
    decision-4 violation (only `python_lockdown.cpp`/`python_runner.hpp` are off-limits to reuse).
    The verified `HANDLE` is bridged into a real Python file object via `_open_osfhandle` + `io.
    FileIO`/`BufferedReader`/`BufferedWriter`/`TextIOWrapper` — never a re-derived path, preserving
    ADR-014's TOCTOU-safe guarantee end to end. Capability checks go through
    `CapabilitySet::contains()` with a freshly-constructed `cap::FsRead`/`cap::FsWrite`/`cap::NetOut`
    request per call — real per-call freshness from `EffectContext`, closing the old code's
    "fixed at construction" gap for exactly the surface that has a real capability to check. Covers
    r/rb/w/wb/a/ab modes only this pass (a named, narrower scope, not a silent wrong guess for other
    mode strings).
  Proven in `tests/test_mediated_python_runner_smoke.cpp` (21 checks, two sequential interpreter
  lifetimes in one process — `Py_Finalize` then a fresh `Py_InitializeFromConfig`, proving that
  cycle itself works): real execution + stdout capture; variable AND `cwd` persistence across calls;
  import denial (fail-closed default) paired with its positive control (a second runner with the
  identical module granted via `package_policy_allowlist`); `subprocess.Popen`/`os.system` denied
  with `PermissionError`; `open()` for write denied without a capability (with the denied write
  proven to never reach disk) paired with its positive control (granted `FsWrite`, real content
  written and read back byte-for-byte through `open_within_mount_root`) and a read-side positive
  control (`FsRead`). Windows only this pass (`AGENTENGINE_BUILD_PYTHON_RUNNER`, matching ADR-002's
  own POSIX gap — unchanged, not attempted here). Full regression: `build-py`'s worktree+python
  test set 19/19 (existing spike tests unmodified and still passing, confirming decision 4's "spike
  stays in place" held); default-config `build` (AGENTENGINE_BUILD_PYTHON_RUNNER off, everything
  this milestone has built so far) unaffected — the new CMake wiring lives entirely inside the
  already-existing `if(AGENTENGINE_BUILD_PYTHON_RUNNER)` guard. **XL**
- **E3 (done, one residual named).** `MediatedShellRunner` (`src/backends/native_jail/
  mediated_shell_{grammar,parser,dispatch}.{hpp,cpp}`, `mediated_command_registry.hpp`,
  `mediated_filesystem_adapter.{hpp,cpp}`, `mediated_shell_runner.hpp`) — a genuinely new set of
  translation units per decision 4's literal reading (same as E2). `shell_grammar.hpp`/
  `shell_parser.{hpp,cpp}`/`shell_dispatch.{hpp,cpp}`/`command_registry.hpp`/
  `real_filesystem_adapter.{hpp,cpp}`/`shell_runner.hpp` stay completely untouched (verified: their
  own tests — `test_shell_runner_proof`, `test_shell_parser_adversarial`,
  `test_shell_runner_no_process_creation` — still pass unmodified, alongside all of E2's own tests
  and the worktree suite, 24/24). Named `MediatedShellRunner`, not `ShellRunner`, to avoid colliding
  with the untouched spike's own class name in the same namespace (matching `MediatedPythonRunner`'s
  precedent). Carries forward ADR-001's exact Judged grammar (the EBNF verbatim: `command_line`,
  `and_or`, `pipeline`, `simple_command`, `assignment`, `redirect`, `word`, `if_stmt`, `for_stmt`,
  `script`), the pmr-arena/shared-recursion-depth-counter safety knobs, and the closed three-way
  `CommandRegistry` lookup (builtin / registered `Runner` / registered `Tool`, reserved-name
  rejection at registration time) as design, not code. Two real upgrades over the untouched spike,
  both because `trust/capability.hpp` gained real scope fields (`mount_id`/`path_prefix` on
  `FsRead`/`FsWrite`) after ADR-001 was written: (1) every builtin does a REAL, path-scoped
  `CapabilitySet::contains()` check (a genuine `cap::FsRead{mount_id, path_prefix, ...}`/
  `cap::FsWrite{...}` request per call), not the kind-only `contains_kind` shortcut ADR-001 §2.5.4's
  now-superseded downgrade used; (2) `MediatedFileSystemAdapter` is built directly on
  `core/worktree_mount_fs.hpp`'s `open_within_mount_root` (ADR-014) rather than reimplementing its
  own canonicalize-then-check — that primitive's own header names itself as exactly what a future
  `FileSystemAdapter` is expected to call, so reusing it is the documented intent (only
  `real_filesystem_adapter.{hpp,cpp}` was off-limits). `remove`/`list_directory` use handle-anchored
  Win32 APIs (`SetFileInformationByHandle`/`FileDispositionInfo`, `GetFileInformationByHandleEx`/
  `FileFullDirectoryInfo`) so the already-verified handle, not a re-resolved path, is what acts.
  **Two real bugs found and fixed during this pass, not among any prior red-team's findings**: (a)
  `CreateFileW` cannot actually create a directory (`FILE_FLAG_BACKUP_SEMANTICS` only OPENS an
  existing one) — an initial `make_directory` silently created a zero-byte FILE named after the
  requested directory instead, caught only because a later `write_file` into it failed with
  `ERROR_PATH_NOT_FOUND`, not by the (bug-blind) `std::filesystem::exists` check the test itself
  originally used; fixed by using the real `CreateDirectoryW` API against a handle-verified parent
  path (a narrower TOCTOU guarantee than `open_within_mount_root`'s own open-then-verify pattern,
  named as a residual below, not silently assumed equivalent). (b) the parser's word-collection loop
  had no notion of a reserved keyword, so `if echo x then ...` silently swallowed `then` as an
  ordinary argument to `echo` instead of ending the command there — breaking essentially all `if`/
  `for` parsing; fixed by making an unquoted word matching `then`/`else`/`fi`/`do`/`done` always end
  word-collection (a quoted `"then"` still round-trips as a literal argument, since the check
  inspects the atom's KIND, not just its text). `rename` is implemented as copy-then-delete rather
  than `SetFileInformationByHandle(FileRenameInfo)` (which did not behave as documented against this
  handle-relative usage) — this reuses, not contradicts, ADR-001's own finding 8 ("mv/cp cross-device
  fallback... non-atomic, no rollback... still open"), just as the primary path rather than a
  fallback. `evaluate_pipeline` distinguishes hard, script-stopping errors (capability denial,
  command-not-found, malformed dispatch) from ordinary command-level failures (a real filesystem
  operation that simply didn't succeed) — only the latter become an inspectable, non-`ok`
  `ExecOutcome` `&&`/`||` can branch on; a design clarification the researched ADR-001 text didn't
  make explicit, needed to make `&&`/`||` mean anything at all.
  `RunnerCall<python>` composition (010 §1a/§9 G4) proven directly against the REAL
  `MediatedPythonRunner` from E2 (not a fake), including the negative control the breakdown doc's
  own task text asked for, and the "cannot exceed" half (PythonRunner's own `FsWrite` denial still
  fires when invoked through ShellRunner with only `RunnerCall{"python"}` granted — the identical
  `EffectContext&` is passed through, never attenuated-then-silently-widened). A real, load-bearing
  test-design finding surfaced while proving this: passing nontrivial Python source (spaces, string
  quotes, multi-line control flow) through this shell grammar's own word-splitting/quote-stripping
  mangles it unless the WHOLE source is wrapped in one shell-level double-quoted word — a genuine,
  named limitation of composing through a grammar with no heredoc support (ADR-001 §3's own v1
  scope), not a `ShellRunner` bug, and worth remembering for Phase F/G's own `agent.*` library design.
  Proven in `tests/test_mediated_shell_runner_smoke.cpp` (37 checks: parser bounds, registration-time
  shadowing rejection, all 10 builtins with denied/granted capability pairs — `mv`/`cp` proving BOTH
  a granted `FsRead` source and `FsWrite` destination are independently required — pipelines,
  `&&`/`||`, `if`/`for`, command-not-found fail-closed across a hostile-name corpus, the fake-Runner
  gate + negative control) and `tests/test_mediated_shell_runner_python_composition.cpp` (6 checks,
  only built when `AGENTENGINE_BUILD_PYTHON_RUNNER` is ON). **Named residual, not silently closed**:
  `make_directory`'s `CreateDirectoryW`-against-a-verified-parent-path pattern is a narrower TOCTOU
  guarantee than `open_within_mount_root`'s own primitive (a real, if smaller, race window remains
  between verifying the parent and the path-string-based `CreateDirectoryW` call) — a candidate for
  E4 or a follow-up to close via a stronger primitive, not attempted this pass. `argv`->registered-
  `Tool`-typed-`Args` mapping stays undesigned (ADR-001 §11 item 3, unchanged); command substitution
  and background processes stay out of scope (unchanged). Windows only this pass (matching ADR-001's
  own scope). Full regression: `build-py`'s worktree+python+shell test set 24/24 (existing spike
  tests unmodified and still passing); default-config `build` 46/47, same pre-existing
  `test_native_jail_backend_windows` flake. **L**
- **E4 (done, G7's literal syscall-trace claim named as an open residual).** Containment/mediation
  proof (010 §9 G2, G7) against the REAL `MediatedPythonRunner` (E2) and `MediatedShellRunner` (E3).
  `tests/test_mediated_python_runner_hostile_corpus.cpp` (21 checks): `ctypes`/`winreg` (Windows'
  `/proc` analogue)/`array` (a real stdlib native extension, not a hypothetical one) denied by the
  import allowlist; `os.popen`/the exec/spawn family denied or genuinely absent (measured, not
  assumed); `os.fork()` absent on Windows CPython entirely; egress to `169.254.169.254` denied
  without a matching `NetOut` grant, paired with a same-mount positive control; a real Windows
  junction (`mklink /J`) crossing the mount boundary denied through the identical
  `open_within_mount_root` (ADR-014) path `open()` mediation always uses, paired with an inside-the-
  mount positive control; an object-graph-introspection probe generalizing "sys.settrace
  shenanigans" to the real underlying risk (recovering a pre-mediation reference via ordinary
  attribute access, no trace hooks needed). `tests/test_mediated_shell_runner_hostile_corpus.cpp`
  (20 checks) + `tests/test_mediated_shell_runner_no_process_creation.cpp` (the STATIC half, same
  llvm-nm-against-the-built-artifact methodology as the untouched spike's own
  `test_shell_runner_no_process_creation.cpp`, applied to `agentengine_mediated_shell_runner`):
  PATH hijacking has no code path to hijack (`resolve()` never reads `ExecState.env` at all, proven
  behaviorally with a decoy binary at the hijacked path); command substitution `$(...)` is not in
  the grammar, so it lexes as inert literal text, never a nested dispatch; `alias`/`function`
  shadowing attempts resolve as ordinary command-not-found (no such grammar production exists);
  registration-time reserved-name rejection against all 5 file-touching builtins; a hostile-name
  corpus (absolute paths, `\\?\` prefixes, traversal, well-known binaries) all command-not-found.
  **Two real bugs found and fixed by this corpus, neither hypothetical:** (a) the Layer-0 keep-set
  computation unioned in the ENTIRE post-bootstrap `sys.modules` snapshot rather than just the names
  `os`/`socket`/`subprocess` themselves newly pulled in — measured finding: a bare, isolated,
  no-site CPython startup on this target already has `winreg` (security-relevant — one of G2's own
  named hostile classes) resident in `sys.modules` before any of this design's code runs at all, so
  the wholesale snapshot silently granted guest code `import winreg` despite it never being on
  ADR-002's own hand-curated baseline; fixed by diffing `sys.modules` immediately before vs. after
  the bootstrap's own `import os, socket, subprocess`, keeping only the genuinely new names. (b) the
  bootstrap captured the real `socket.socket.connect` as a Python-level global
  (`_ae_real_connect = socket.socket.connect`) inside its own throwaway namespace — but CPython sets
  a function's `__globals__` to its DEFINING dict, not a copy, and that dict stays alive for as long
  as the wrapper function (now bound as `socket.socket.connect`) exists; guest code could recover it
  with nothing more than `socket.socket.connect.__globals__['_ae_real_connect']` and call it
  directly, bypassing `NetOut` capability mediation entirely (undetected SSRF-class egress). Fixed
  by moving the real reference into a C++ TU-static (`g_real_socket_connect`, mirroring
  `g_real_meta_path`'s existing precedent for the import finder) that no Python-reachable name ever
  points to, exposed only through a C-implemented `_ae_internal.do_connect(sock, address)` that
  performs the capability check and the real connect in one call whose RESULT is observable but
  whose underlying callable is not. **Named, not silently closed, residual**: G7's own text asks for
  the mediated-call denial to be "proven by a syscall-level trace (strace/ETW) showing zero
  attempts, not merely a caught exception at the Python level" — this pass proves the behavioral
  half (exception raised before any Python-level side effect, positive/negative controls) but NOT
  the literal kernel-trace half; a real Windows ETW kernel-logger session needs an elevated process,
  and this dev environment was not running elevated. ADR-002's own prove phase hit this identical
  wall twice (A1's canary/ETW half, all of C2/C3) and named it "NOT ATTEMPTED" rather than
  approximate it — this pass does the same, honestly, pending an elevated relaunch to pick up the
  ETW instrumentation. Memory-bomb/output-flood containment for `MediatedPythonRunner` standalone is
  also named out of scope here (it executes in-process, not as a native_jail-sandboxed child; that
  containment is Job Object-based and applies once Phase F composes this Runner's process under
  `NativeJailBackend`, not before — `test_native_jail_abuse_corpus_windows.cpp` already proves that
  class for the process-boundary case). Full regression: `build-py`'s worktree+python+shell test set
  56/57 (same pre-existing `test_native_jail_backend_windows` `-j4` timing flake tracked all
  session); default-config `build` 48/49, identical flake. **XL**
- **E5 (done — ADR-track, decision 5).** `ShellRunner` grammar-parser fuzzing (010 §9 G8):
  `decisions/ADR-015-shellrunner-grammar-parser-fuzzing.md` (Judged). A real libFuzzer harness
  (`tests/fuzz/mediated_shell_parser_fuzz.cpp`) against `mediated_shell_parser::parse()`'s pure
  `bytes -> result<ScriptNode>` entry point — no fakes needed, matching that function's own
  by-construction isolation from `FileSystemAdapter`/`CommandRegistry`/`ExecState`/`EffectContext`.
  Clang-only (`AGENTENGINE_BUILD_SHELL_FUZZER`, default off, matching `AGENTENGINE_WITH_WASM`/
  `AGENTENGINE_BUILD_PYTHON_RUNNER`'s own opt-in posture — libFuzzer has no MSVC-native equivalent),
  seeded from a checked-in corpus (`tests/fuzz/corpus/mediated_shell_parser/`), gated in a new
  `windows-shell-fuzz` CI job (`.github/workflows/ci.yml`) running a declared 120-second corpus-
  driven pass on every push/PR. Two real Windows-specific libFuzzer ABI mismatches found and fixed
  during setup (neither a design flaw): the vendored `clang_rt.fuzzer-x86_64.lib` needs the static
  CRT (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`) against this project's default dynamic-CRT
  posture; and MSVC STL's ASan container-annotation feature needed explicitly disabling
  (`-D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION`) to match the runtime lib's own build.
  **The harness's teeth were proven directly, not assumed**: ADR-001 finding 12's nesting-depth
  guard (`kMaxNestingDepth` check in `parse_if`) was deliberately disabled, rebuilt, and both a
  direct single-input replay and a real corpus-driven fuzzing run reproduced a genuine native
  stack-overflow crash (`STATUS_ACCESS_VIOLATION`, and separately libFuzzer's own `ERROR: libFuzzer:
  deadly signal` classification) — then the guard was restored (confirmed via `git diff` showing
  zero net change) and a clean 120-second run reconfirmed (23,517 executions, 0 crashes/findings).
  **Named residuals, not silently closed**: the CI job was written and reviewed line-by-line against
  the working local sequence but never executed on an actual GitHub Actions runner (this local repo
  has no configured remote to push to); the harness has no semantic oracle, so it can only ever catch
  crashes/hangs/UB, never a silent wrong-output bug (the exact shape of both of E3's own real
  historical bugs); no corpus-growth persistence across CI runs (each run restarts from the same
  checked-in seed corpus); Linux and the full `dispatch`/`evaluate` execution path are both out of
  this ADR's scope (matching `agentengine_mediated_shell_runner`'s own Windows-only, parser-only
  framing). **L**

  **Milestone 3 Phase E is now complete** (E1-E5, tasks #48-52). Phase E's own remaining known
  residuals, carried forward rather than re-litigated here: ADR-003's caller-aware import-gating
  tier (E2's Stage C, scoped out); G7's literal syscall-level (ETW) trace proof for denied
  `open`/`socket`/`subprocess` calls (E4, needs an elevated relaunch this session never got); and
  the several narrower TOCTOU/argv-mapping/redirect-scope residuals named in E2/E3/E4's own entries
  above.

### Phase F — Worktree integration, artifacts, tool bridge (025 §7, 010 §4, §6)

- **F1 (done).** `materialize_mount`/`harvest_mount`
  (`src/backends/native_jail/worktree_mount_sync.hpp`) bridge a worktree `Mount`'s content-addressed
  Tree with the real host directory `MediatedPythonConfig::mount_roots`/`MediatedShellRunner` already
  point at (Phase C/E) — `materialize_mount` primes a real directory from the mount's current tree
  through `mount_read` per file (the same `cap::FsRead` check a guest `open()` gets); `harvest_mount`
  walks a real directory back into the tree through `mount_write` per file (same `cap::FsWrite` +
  quota check a guest `open(..., "w")` gets) and returns one `ContentItem` per file harvested
  (digested via A1's `compute_digest`, `BlobRef{digest, media_type, size, store="worktree"}") —
  closing 025 §7's "the agent saves a file, the user receives an artifact" claim end to end, proven
  in `tests/test_worktree_mount_sync.cpp` against the REAL `MediatedFileSystemAdapter` (ADR-014), not
  a test double: real files on disk, real nested directories, real Tree round-trips via `mount_read`
  after harvest, and positive-control-paired capability denials (022 §5) for both directions. Reuses
  `mount_read`/`mount_write` entirely rather than touching the object/ref store directly, so the
  SAME capability/quota enforcement guest code gets applies to host-orchestrated priming/harvesting
  too — no second authority path invented. `ExecOutcome` (`sandbox/sandbox.hpp`) gained a real
  `artifacts: vector<ContentItem>` field (previously "elided pending BlobRef-backed artifact
  vocabulary" — that vocabulary is now real), populated by whichever caller does the harvesting, not
  by a backend/runner itself (008 §2/010 §3a keep `SandboxBackend::exec`/`Runner::run` mount-agnostic).
  **A real, previously-undiscovered bug found and fixed along the way**: `MediatedFileSystemAdapter::
  list_directory("")` (listing a mount's own root — e.g. `ls` with no argument at the freshly-
  constructed `ExecState.cwd`, which starts `""`) unconditionally failed with
  `worktree.mount_path_is_root`, because `open_within_mount_root` structurally rejects an empty guest
  path (correct for `read_file`/`write_file`, where the root is never a valid FILE target, but wrong
  for a directory listing, where the root is an ordinary target) — a pre-existing gap in
  `MediatedShellRunner`'s own `ls` builtin, never caught because no existing test exercised a bare
  `ls` before any `cd`. Fixed by special-casing an empty path in `list_directory` to open `root_`
  directly (`CreateFileW`), mirroring the identical root special-case `create_one_directory` already
  had for the same reason. Full regression after the fix: default `build` 49/50, `build-py` 57/58,
  both with the same pre-existing `-j4` `test_native_jail_backend_windows` OOM-reporting flake (E4's
  note called this a timing flake; this run's manifestation was the OOM-class assertion specifically
  — consistent with resource contention under parallel test execution, not a new regression). Not
  built this pass, named as residuals rather than silently assumed: no caller yet actually invokes
  `materialize_mount`/`harvest_mount` around a real `PythonRunner`/`ShellRunner` turn (that caller is
  the not-yet-built session/turn-loop layer, same gap `core/worktree.hpp`'s own `TurnCommit` section
  already names); `materialize_mount`'s documented precondition (`granted.path_prefix` must cover the
  whole subtree or the walk fails closed) is not itself relaxed to a partial-materialize mode. **M**
- **F2 (done).** `call_tool` bridge (010 §6) — `ToolBridgeConfig{bridged_tools, capabilities,
  approved}`/`bridge_tool_call()` (`src/backends/native_jail/tool_bridge.hpp`) call the full,
  real-since-M2 `invoke_tool()` 006 §3 pipeline at a `CapabilitySet` built fresh from the bridge
  config alone — structurally, not just conventionally, never the agent's ceiling: there is no
  parameter through which an agent-level `CapabilitySet` could reach `bridge_tool_call`, unlike
  `core/agent_registry.hpp`'s `invoke_agent_tool`, which does thread the agent's own
  `capability_ceiling` through and was deliberately not reused as a template here. Bundled approval
  (010 §10 Q2) is one `ApprovalDecider` closure capturing `config.approved`, gating the whole
  pre-registered set at `execute_code` time, never per-call. Results re-enter tainted unconditionally
  (pipeline step 9, 003 §2). Proven in `tests/test_tool_bridge.cpp` (13 checks): capability scoping
  and bundled approval each with a positive control (022 §5), result tainting, and the SAME
  capability-handle-reuse-denial discipline `test_tool_pipeline_capability_reuse.cpp` (M2 B4) proved
  for `invoke_tool()` directly, reproduced end to end through the bridge.
  Wired into `MediatedPythonRunner` (Stage D idiom, alongside `_ae_open`/`_ae_connect`): a new
  `_ae_internal.call_tool(name, args_json) -> reply_json` C function and a bootstrap-installed
  `call_tool(name, args_json='{}')` builtin, mapping `invoke_tool`'s error codes to Python exceptions
  per 026 §3's table (`tool.capability_not_held`/`tool.approval_denied` → `PermissionError`,
  `tool.unknown_name` → `ValueError`, `tool.deadline_exceeded` → `TimeoutError`). `nullopt`
  (`MediatedPythonConfig::tool_bridge`'s default) means no tools are bridged this session —
  `call_tool(...)` fails closed with a `PermissionError`, matching every other host-configured
  surface in this file (`mount_roots` empty means no mounts).
  **Deliberate scope narrowing, found only by testing, not by design review**: the first version
  passed arguments as a Python dict via `json.dumps`/`json.loads`, requiring `import json` in the
  bootstrap script. Full regression caught two real problems this created, not just broke a test
  assertion: (1) `import json` at bootstrap time permanently added `json` to the Layer-0 keep-set
  the pre/post-bootstrap `sys.modules` diff computes, making `json` importable by every guest
  session regardless of `package_policy_allowlist` — an actual, unintended widening of the
  fail-closed import default (`test_mediated_python_runner_smoke.cpp` E2-C5 caught this); (2) the
  new `call_tool` function, defined in the bootstrap's shared globals dict alongside `_ae_connect`
  et al., became reachable via `socket.socket.connect.__globals__` walk
  (`test_mediated_python_runner_hostile_corpus.cpp` E4-PY9's leak check). Fix: `call_tool` takes and
  returns raw JSON text (`call_tool(name, args_json='{}')`), `import json` removed from the
  bootstrap entirely, and `call_tool` added to E4-PY9's `wrappers` allowlist alongside `_ae_open`/
  `_ae_connect`/`_ae_denied` — it's exactly as legitimate an intentional, capability-checked wrapper
  as those three. Ergonomic dict-based `agent.tools.<name>(...)` callables built from real tool
  schemas are Phase G1's job, not this raw bridge's — named as a residual the same way ADR-001 §11
  item 3 already names ShellRunner's own argv-to-typed-Args mapping as undesigned. That ShellRunner
  wiring (`mediated_command_registry.hpp`'s `RegisteredTool`) is likewise not connected to the
  bridge this pass, for the identical reason: the argv-mapping design question ADR-001 already named
  is a prerequisite this task doesn't relitigate. Full regression after both fixes: default `build`
  50/51, `build-py` 58/59, both with only the same pre-existing `test_native_jail_backend_windows`
  OOM-reporting flake F1's own regression run hit — not a new failure. **L**
- **F3 (done).** Output discipline (010 §3 items 4/5). Two genuinely separate mechanisms, not one:
  `ExecOutcome::result_repr` (new field, `sandbox.hpp`) closes 010 §3's own named gap — a value never
  `print()`-ed, `data = open(huge_file).read(); data` as the last expression, say — which running the
  whole script as an ordinary module (`Py_file_input`, values computed then discarded, the same as any
  `.py` file) always threw away. `MediatedPythonRunner`'s `split_trailing_expression()`
  (`mediated_python_runner.cpp`) finds a plausible EXEC/EVAL split point using only the C compiler's
  own success/failure as ground truth (`Py_CompileStringExFlags`, trial-compiled and discarded) —
  never Python's `ast` module, for the identical reason F2 already avoided a new stdlib import:
  `compute_effective_keep_set`'s sys.modules diff has no per-call granularity, so any new import here
  would permanently widen what every guest session can import regardless of `package_policy_
  allowlist`. Every candidate split (whole-last-line-as-eval first, then each top-level-looking `;`
  split from rightmost to leftmost — 010 §3's own example is exactly a `;`-separated last line) is
  re-validated by compiling both halves before being accepted, so a wrong guess (a `;` that turns out
  to be inside a string literal) simply fails to compile and falls back to running the whole source as
  an ordinary exec, identical to pre-F3 behavior — never a new failure mode, since `result_repr` is
  display formatting of an already-executed, already-captured value, not an authority boundary (no
  I2/I3 property rides on this). Proven in `test_mediated_python_runner_smoke.cpp`'s new Scenario 3
  (F3-R1..R4): a multi-line trailing bare name, the RFC's own literal `;`-separated example, a trailing
  `print()` (returns `None` — stays unrepr'd, and runs exactly once via the eval branch, not
  re-executed), and a script with no trailing expression at all (unaffected, matching pre-F3 behavior
  exactly). Named residual, not silently unhandled: a multi-line trailing expression (e.g. a
  parenthesized expression spanning several lines) isn't split out — a physical-line-based split can't
  express it without a real parser, which the "no new stdlib import" constraint forecloses this pass.

  The size-cap-with-truncation-marker half is `src/backends/native_jail/output_discipline.hpp`'s
  `cap_output()` — a small, shared, pure-C++ utility applied uniformly to `stdout_text`/`stderr_text`/
  `result_repr` at both `MediatedPythonRunner::run()`'s and `MediatedShellRunner::run()`'s return
  boundary (010 §1's "not Python-only by design" made concrete for this one cross-cutting concern;
  Shell has no last-expression semantics, so its `result_repr` is simply never set — a legitimate
  empty per that field's own comment, not a gap). Explicitly a DIFFERENT layer than
  `ResourceLimits::output_bytes` (008 §2, already real via `drain_pipe_bounded()` in
  `native_jail_backend.cpp`/`linux_native_jail_backend.cpp`): that's a host-safety ceiling on raw bytes
  a spawned child can make the backend buffer, enforced before this file ever sees the data; this
  cap is the separate, always-applied MODEL-VISIBLE discipline 010 §3 names, so a value stays legible
  even when it comfortably fits under the host-safety ceiling. **Named, not silently assumed**: 006 §7
  says this threshold should be "derived from the run's effective per-turn token budget... scaled to a
  declared fraction, never a global byte constant" — that plumbing doesn't exist anywhere in this
  codebase (023 stays TBD-baselined project-wide until M8, the same status quo every other
  budget-derived gate here already shares). `kDefaultOutputCapBytes` (64 KiB) is a provisional stand-in
  a caller can override per session via `MediatedPythonConfig::output_cap_bytes`/
  `MediatedShellRunner`'s new (defaulted, so no existing call site breaks) constructor parameter —
  exactly the override point a real per-turn value would need once 023 lands. Proven directly in
  `tests/test_output_discipline.cpp` (`cap_output()`'s own byte accounting, marker presence, and a
  UTF-8-codepoint-boundary case: a cut that would land mid-multi-byte-sequence backs off rather than
  emitting a truncated codepoint) and through both Runners with a small explicit cap (F3-T1 in the
  Python smoke test, F3-S1 in the Shell smoke test). Full regression: default `build` 51/52,
  `build-py` 59/60, both with only the same pre-existing `test_native_jail_backend_windows`
  OOM-reporting flake F1/F2's own regression runs already hit. **S**

### Phase G — The `agent.*` library / CodeAct (026 §4, §5, §5a)

- **G1 (done).** `agent.tools` as ordinary Python callables (`from agent import tools;
  tools.web_search(query=..., max_results=5)`), generated from the SAME `ToolDescriptor` metadata
  (`name`/`description`/`args_schema_json`) `006 §1`'s JSON-Schema derivation already produces for
  every other tool-pipeline caller — 026 §4's "generated from the same tool metadata as everything
  else... so they cannot drift" claim, made literal rather than re-derived. All schema-to-Python-
  source generation lives in a new, pure-C++, Python-free header,
  `src/backends/native_jail/agent_tools_codegen.hpp` (parses `args_schema_json` via the existing
  `core/json_value.hpp` parser, not by having the GENERATED Python re-introspect its own schema at
  runtime), specifically so the generation logic is unit-testable without an embedded interpreter
  (`tests/test_agent_tools_codegen.cpp`) — the actual `PyRun_String` execution wiring
  (`run_agent_tools_bootstrap`) stays in `mediated_python_runner.cpp` alongside the rest of that
  file's CPython-C-API code.

  **Real signatures**: every generated function is keyword-only (`def web_search(*, query: str,
  max_results: int = None):`) — sidesteps Python's "no default parameter before a non-default one"
  ordering rule entirely (a struct's field declaration order has no reason to already be
  required-first) and matches 026 §4's own calling example verbatim, which already passes every
  argument by keyword. A zero-argument tool correctly gets a bare `def name():`, not a syntactically
  invalid trailing `*` with nothing after it (json_schema.hpp's own "required" bit derived
  structurally, `std::optional<T>` vs not, is what drives which fields get a `= None` default).
  **Docstrings**: the tool's real `description`, quote/backslash-escaped so an embedded `"` can never
  prematurely close the generated `"""..."""` literal. **Typed results**: replies decode into a
  shared, attribute-accessible `_AeReply` wrapper (`self.__dict__.update(data)`), not a raw dict —
  named as a deliberately narrower claim than 026 §4's literal "dataclass-shaped" language: one
  per-tool NOMINAL class would need `exec()`-generated class bodies for no behavioral gain over the
  shared wrapper, since neither approach can validate a reply's shape any more precisely without
  re-deriving `reply_schema_json` a second time at the attribute level. **A `.pyi` stub**
  (`generate_agent_tools_pyi_stub`) mirrors the real signatures exactly, generated as text only — no
  established consumer (LSP/static-analysis integration) exists in this codebase yet to write it to,
  named as a residual rather than an invented delivery mechanism nothing asked for;
  `dir()`/`help()`-at-runtime (026 §4's OTHER introspection claim) is already fully satisfied by the
  real function/module objects themselves, needing the stub for neither.

  **A Python-keyword-named argument field (`from`, `in`, `is`, ...) — a perfectly ordinary C++
  identifier and a reserved Python keyword at the same time — disqualifies the WHOLE tool's
  generation, loudly** (a `result` error naming the exact field), never a silently broken or silently
  dropped parameter; same treatment for a tool name that isn't a usable Python identifier.

  **Wiring** (`mediated_python_runner.cpp`): `run_agent_tools_bootstrap` runs the generated module
  source in its own private, throwaway globals dict — the identical shape `run_mediation_bootstrap`
  already uses for `_ae_open`/`_ae_connect`/`call_tool` (never `__main__`'s dict) — creating REAL
  `agent`/`agent.tools` module objects from `type(sys)` (no `import types` needed, `sys` is already
  Layer-0-permanent) and registering both directly into `sys.modules`, so ordinary `import agent`/
  `from agent import tools` resolve straight from that cache, never touching the meta-path finder at
  all (the same "never re-resolved fresh" property `os`/`socket`/`subprocess` already rely on). Runs
  ONLY when `config_.tool_bridge.has_value()`, and runs BEFORE `compute_effective_keep_set`'s
  pre/post-bootstrap `sys.modules` diff — so `json`, `agent`, and `agent.tools` are all captured by
  the SAME diff mechanism that already covers os/socket/subprocess's own transitive closure, and
  therefore survive `sweep_to_keep_set()` instead of being deleted right after creation.

  **Deliberately DOES `import json`, unlike F2's raw bridge** — and this is safe, not a repeat of
  F2's own already-documented regression: F2 avoided `import json` because `kMediationBootstrapSource`
  runs UNCONDITIONALLY for every session regardless of whether any tool is ever bridged, so importing
  it there would have widened every session's keep-set even when `agent.tools` never exists at all.
  This generator's output only ever runs for a session that is ALREADY getting a real `agent.tools`
  module built from real, host-configured tools, and each session is its own process (ADR-002
  §5.5.6) — `json` becoming importable exactly when `agent.tools` exists is the intended shape, not a
  leak into a session that never asked for it.

  Proven in `tests/test_agent_tools_codegen.cpp` (portable, no CPython dependency — 9 checks: real
  signature shape, docstring escaping, the zero-argument boundary, the two loud-failure cases, full
  module source shape, `.pyi` stub parity with the real signature) and
  `tests/test_mediated_python_runner_agent_tools.cpp` (Python-gated, 10 checks against a real
  embedded interpreter): an ordinary `from agent import tools; tools.echo_tool(message=...)` round
  trip; `dir()`/`help()` discoverability; special characters (embedded quote, backslash, newline)
  surviving the real `json.dumps`/`json.loads` wire encoding exactly, proving real JSON handling
  rather than a hand-rolled approximation; a negative control pairing (022 §5) proving a call without
  the bridge's own required capability still raises `PermissionError` through the REAL 006 §3
  pipeline, never a shortcut that bypasses enforcement; and a second negative control proving that
  with no `tool_bridge` configured at all, `import agent` fails `ModuleNotFoundError` — an ungranted
  module is simply absent (026 §5a), not present-but-empty. Full regression: default `build` 52/53
  (`test_agent_tools_codegen` is portable, runs in both trees), `build-py` 61/62, both with only the
  same pre-existing `test_native_jail_backend_windows` OOM-reporting flake every prior Phase F task's
  own regression run already hit. **L**
- **G2 (done, narrowed from its original XL scope — see below).** `agent.files`/`agent.data`, built
  for real over F1/Stage D's existing worktree/filesystem mediation. **`output`/`progress`/`ask`/
  `spawn` are NOT built this pass** — a research finding, not an oversight (see "Scope finding"
  below). `memory`/`notes` remain 029's job as this doc's original text already said.

  **Scope finding, surfaced before any code was written.** This task's original one-liner assumed all
  eight remaining modules had "already passed the [curation] test on paper" and needed only wiring.
  A research pass (citing `EffectContext` at `include/agentengine/core/effect_context.hpp:15-28`,
  `OutputSchema` at `core/agent.hpp:112`, `agent_registry.hpp:423-428`'s `invoke_agent_tool`) found
  this true only for `files`/`data`: both have real, existing host machinery to be a convenience layer
  OVER (F1's `materialize_mount`/`harvest_mount`, Stage D's `open()` mediation). The other four do
  not — `output` needs 003 §4's structured-output-schema enforcement (an empty placeholder struct
  today, not implemented); `progress` needs 013's run event stream (RFC-only, zero C++ hits for
  `EventStream`/`StateChanged`/`report_progress` anywhere in `include/`/`src/`, and `EffectContext`
  has no reverse channel to push events through even if one existed); `ask` needs 001 §2's
  `InputRequired`/`Interaction` suspend-resume mechanism (the `cap::Elicit` capability tag is real and
  gateable, but nothing in this codebase can actually pause a run and later deliver an answer back);
  `spawn` needs a real nested-agent-run invocation path (`cap::AgentCall`/`SpawnBudget` are real, but
  the only existing "invoke another agent" function, `invoke_agent_tool`, is tool-dispatch-only — ONE
  call through the target's tool table, never a recursive run — and uses the target agent's own FULL,
  unattenuated capability ceiling, directly contradicting 026 §5's own explicit "`agent.spawn`
  inherits an attenuated capability set" design constraint). Building working versions of these four
  would mean inventing new cross-cutting host infrastructure that several OTHER RFCs (013, 001, 003)
  own, not writing a Python wrapper over something that already exists — a materially different, much
  larger task than "wire up `agent.*` over existing metadata," and one this project's own CLAUDE.md
  names as needing `design → red-team → prove → judge` treatment before an ad-hoc implementation,
  given how directly `spawn` in particular touches I2 (no ambient authority: the wrong-ceiling bug
  above is exactly an ambient-authority shape). Presented to the project owner as an explicit
  three-way choice (build files/data only and name the rest blocked / build the missing host
  machinery first / ship files+data plus `NotImplementedError` stubs for the other four); the first
  was chosen, matching this project's own established residual-naming discipline (008/025's own
  "named gap, not silently worked around" precedent) over either silently shipping fake modules that
  would violate 026 §1a's "we do not lie" principle (a `progress.report()` that silently no-ops, an
  `ask()` that cannot really suspend) or silently absorbing four other RFCs' own unbuilt design work
  into this task.

  **What was built.** `src/backends/native_jail/agent_files_data_codegen.hpp` (new, pure C++,
  Python-free, mirroring `agent_tools_codegen.hpp`'s own separation) — unlike G1, there is no
  per-session-variable schema to derive source from (the function set is fixed by 026 §5's table), so
  this header is a single static Python source string covering both modules:
  `agent.files.input(name)`/`.artifact(name, data)`/`.list(path)` (reading/writing the RFC's own
  canonical `/input`/`/out` mount framing, 026 §2), `agent.data.read_json(path)` (a small-file
  convenience) and `.read_json_lines(path)`/`.read_csv_rows(path, delimiter=',')` — both real Python
  GENERATORS (`yield`, not `return [...]`), which is what makes 026 §5's own "without loading them
  wholly into memory" claim concretely true: iterating a real file object already streams line-by-line
  without materializing the whole file. `read_csv_rows` is deliberately a plain `str.split`, NOT
  RFC4180-compliant (no quoted-field/embedded-delimiter support) — named as a narrower scope than a
  real CSV parser, the same "fails safe over silently wrong" tradeoff F3's own trailing-expression
  split already makes for an analogous reason.

  **A new low-level primitive**: `agent.files.list` needed directory listing, which nothing in this
  codebase provided — `core/worktree_mount_fs.hpp`'s existing `open_within_mount_root` only opens
  files. Added `list_within_mount_root` (same file) reusing the IDENTICAL containment mechanism
  (one `CreateFileW` with `FILE_FLAG_BACKUP_SEMANTICS`, then `GetFinalPathNameByHandleW`-verified
  against the root) applied to a directory HANDLE for the first time, then enumerates via
  `FindFirstFileW`/`FindNextFileW` against the ALREADY-VERIFIED canonical path, never the raw guest
  string — "the object verified is the object used" preserved for listing, not just for opening.
  Proven in `tests/test_worktree_mount_fs_listdir.cpp` (6 checks: empty-root listing, real
  file/subdir entries with correct name/is_dir/size, a `..`-escape negative control, and a junction-
  escape negative control paired with a junction-stays-inside positive control, 022 §5).

  **Wiring** (`mediated_python_runner.cpp`): a new `_ae_internal.listdir(path) -> str` (JSON array of
  `{"name","is_dir","size"}`), capability-checked against `cap::FsRead` BEFORE any Win32 call — the
  same shape `Internal_open` already uses for `cap::FsRead`/`cap::FsWrite` — encoded via the existing
  `json::Value`/`json::dump` serializer (never hand-concatenated strings around a guest-influenced
  file name). `run_agent_files_data_bootstrap()` mirrors G1's own `run_agent_tools_bootstrap` shape
  exactly (private throwaway globals dict, its own fresh `_ae_internal`), gated on a NEW, DEDICATED
  `MediatedPythonConfig::expose_agent_files_data` opt-in — deliberately NOT derived from
  `!mount_roots.empty()`, a real bug this phase found via its own regression suite: `mount_roots`
  predates G2 (Stage D) and several pre-existing tests configure a mount purely for `open()`/`os`
  mediation while still asserting plain `import json` is denied by default
  (`test_mediated_python_runner_smoke.cpp`'s own E2-C5). Gating on `!mount_roots.empty()` alone
  silently widened every one of those sessions' importable set the moment this bootstrap's `import
  json` ran. Fixed by adding the dedicated flag (defaulting `false`, changing nothing for any caller
  that predates G2) and adding a permanent regression control for the exact shape of this bug
  (`tests/test_mediated_python_runner_agent_files_data.cpp`'s G2-N3, independent of relying on the
  older test file alone to keep catching it).

  **`agent.tools`/`agent.files`/`agent.data` coexistence**: G1's own `generate_agent_tools_module_
  source` unconditionally created a FRESH `agent` module object
  (`_agent_module = _ModuleType('agent')`), which would have silently detached whichever submodule the
  OTHER bootstrap (G1's or this phase's) already attached, if both ran in the same session — a latent
  ordering bug G1 could not have hit alone (it was the only bootstrap that existed) but G2's own
  arrival triggers. Fixed in `agent_tools_codegen.hpp` (`_agent_module = _sys.modules.get('agent') or
  a fresh one`) and mirrored in this phase's own generator, making the two bootstraps
  order-independent. Proven under a real interpreter with BOTH a tool bridge and mount_roots
  configured at once (`test_mediated_python_runner_agent_files_data.cpp`'s Scenario 2): `agent.tools`,
  `agent.files`, and `agent.data` all resolve off the SAME `agent` object regardless of which
  bootstrap ran first.

  Proven in `tests/test_agent_files_data_codegen.cpp` (portable, no CPython dependency — 15 checks:
  every function defined/attached, the canonical `/input`/`/out` mount framing, the `agent`-reuse
  fix, both generators actually using `yield`) and
  `tests/test_mediated_python_runner_agent_files_data.cpp` (Python-gated, 22 checks across three
  scenarios): a real `input`/`artifact`/`list` round trip including a real file landing on disk at
  `/out`; `read_json_lines` proven to be a real generator streaming NDJSON (skipping a blank line,
  and correctly NOT raising on a file `json.load` would reject outright); `read_csv_rows`; a
  capability-denial negative control through the real per-call pipeline (never a bypass); the
  `agent.tools`/`agent.files`/`agent.data` coexistence proof above; and the `expose_agent_files_data`
  regression control (G2-N3) for the gating bug this phase found and fixed. Full regression: default
  `build` 54/55 (`test_agent_files_data_codegen` and `test_worktree_mount_fs_listdir` both portable/
  Windows-only-but-no-Python, running in both trees), `build-py` 64/65, both with only the same
  pre-existing `test_native_jail_backend_windows` OOM-reporting flake every prior Phase task's own
  regression run already hits. **L** (narrowed from **XL** — the four residual modules below account
  for the rest of the original estimate)

  **Residuals, named explicitly rather than silently dropped** (blocked on infrastructure this task
  does not own): `agent.output` blocked on 003 §4's real structured-output-schema enforcement;
  `agent.progress` blocked on 013's run event stream, including giving `EffectContext` a reverse
  channel to report through; `agent.ask` blocked on 001 §2's `InputRequired`/`Interaction`
  suspend-resume mechanism; `agent.spawn` blocked on a real nested-agent-run invocation path with a
  genuinely attenuated capability set (`invoke_agent_tool`'s current full-ceiling behavior must not be
  reused for this — see the scope finding above). None of these four should be implemented as a
  Python-level shim without the underlying host mechanism landing first — 026 §1a's own "we do not
  lie" principle rules that out.
- **G3 (done).** `dir(agent)`/`help(agent)` wired to `trust/agent_library_manifest.hpp`'s already-real
  registry (026 §5a, §9 G7).

  **Scope, made precise against what actually exists this milestone.** `dir(agent)`/`dir(agent.tools)`/
  `dir(agent.files)`/`dir(agent.data)` already reflect exactly what's granted — real Python module/
  function objects, established since G1/G2, cannot lie about their own membership by construction.
  What was genuinely missing: `__doc__` text. `agent.tools`'s own individual FUNCTIONS already had
  real docstrings (G1); `agent.files`/`agent.data`'s functions already had them too (G2, written
  alongside each function) — but no `agent.*` MODULE object, nor the top-level `agent` namespace
  itself, had one, so `help(agent)`/`help(agent.files)` showed nothing. `granted_modules()`/
  `push_side_summary()` (the CapabilitySet-driven pull/push functions `agent_library_manifest.hpp`
  already had) are NOT wired in directly this pass: `MediatedPythonConfig` has no `CapabilitySet` field
  at `initialize()` time (capabilities are a strictly per-`run()`-call concept here, via
  `EffectContext`/`g_current_ctx`) — which module OBJECTS exist is, and remains, a session-scoped host-
  config decision (`tool_bridge.has_value()`, `expose_agent_files_data`), matching every other module
  gate in this file, not a literal `CapabilitySet` argument. Threading one through for this alone would
  be new architecture, not integration — exactly the kind of scope creep G2's own scope finding just
  turned down for other modules. What's genuinely reused, making this "mostly integration" true: the
  registry's one-liner TEXT itself. Added `trust::module_one_line(name)` (`agent_library_manifest.hpp`)
  as the one lookup both `agent_tools_codegen.hpp` and `agent_files_data_codegen.hpp` now call to set
  `agent.tools.__doc__`/`agent.files.__doc__`/`agent.data.__doc__`, and append their own
  `"agent.<name>: <one-liner>\n"` line into the top-level `agent.__doc__` — the SAME text 026 §5's
  table and the registry already state, with exactly one place it's written, never a second
  hand-duplicated string that could drift.

  **A new shared helper**: `python_string_literal` (`agent_tools_codegen.hpp`) — a full, escaped,
  single-quoted Python string literal (unlike the pre-existing `escape_for_docstring`, which only
  escapes for embedding inside an already-open `"""..."""`) — needed since `__doc__` assignments are
  complete expressions, not text destined for a literal some other line already opened.
  `agent_files_data_codegen.hpp` includes `agent_tools_codegen.hpp` to reuse it rather than
  duplicating the escaping logic.

  **Composability**: since either bootstrap may run first (or alone), `agent.__doc__` is APPENDED to
  (`(getattr(_agent_module, '__doc__', None) or '') + ...`), never overwritten — mirroring this
  phase's own G2 fix for `_agent_module` reuse. Proven under a real interpreter with all three
  bootstraps active at once (`test_mediated_python_runner_agent_files_data.cpp`'s G3-coexist check):
  `agent.__doc__` carries all three lines regardless of ordering.

  Proven in `tests/test_mediated_python_runner_agent_tools.cpp` (+2 checks: `agent.tools.__doc__`
  matches the registry exactly; `agent.__doc__` mentions it) and
  `tests/test_mediated_python_runner_agent_files_data.cpp` (+4 checks: `agent.files.__doc__`/
  `agent.data.__doc__` each match the registry; `agent.__doc__` mentions both; the three-way coexist
  check above) — every check compares against `trust::module_one_line(...)` directly rather than a
  second hand-typed copy of the expected string, so a future edit to the registry's wording cannot
  silently desync the test's own expectation. Full regression unchanged from G2 (no new test binaries,
  only existing ones extended): default `build` 54/55, `build-py` 64/65, same pre-existing flake. **S**
  (smaller than the **M** estimate — the registry/lookup/escaping machinery already existed or was a
  small, targeted addition; wiring the actual CapabilitySet through, deliberately not done this pass,
  was the estimate's larger assumption)
- **G4.** Error mapping (026 §3) — the closed table (`PermissionError`, `OSError`, `ConnectionError`,
  `TimeoutError`, `MemoryError`, "command not found") sourced from real occurrences of each exception
  class (026 §9 Q2's resolution — not hand-authored per site), never a host stack trace or
  architecture term (026 §8 G3/G6). **(done, narrowed from its original M scope)**

  **Scope finding, surfaced before any code was written**: a survey of the current codebase against
  026 §3's table row by row found that most rows had a REAL GAP, not just a wiring job — several were
  raising the wrong exception type, one row's spec-exact message existed but was on a dead code path,
  and two rows had zero in-process infrastructure at all:
  - *Path outside a mount* — wired, but leaking a HOST DIAGNOSTIC
    (`"CreateFileW(target) failed: GetLastError=2"`, `worktree_mount_fs.cpp`'s `win_error`) instead
    of real CPython-sourced `OSError`/`FileNotFoundError` text — a direct violation of both this row
    and 026 §9 Q2's "never hand-authored" rule.
  - *Quota exhausted* — the exact spec-sourced message already existed
    (`core/worktree.hpp`'s `mount_write`, "No space left on device"), but on the CAS/Ref-based
    store's batched-write path, which `Internal_open`'s LIVE filesystem write path never calls — no
    real-time enforcement existed at all.
  - *Host not permitted* — raised `PermissionError` on both the raw-socket
    (`Internal_do_connect`) and bridged-tool (`net_egress_proxy`/ADR-011) denial paths, not
    `gaierror`/`ConnectionError` as the table specifies.
  - *Wall-clock exceeded* / *Memory exceeded* — the only enforcement mechanism that exists
    (`job_object_limits.hpp`'s Windows Job Objects) is architecturally OUT-OF-PROCESS
    (`TerminateJobObject`-style kill) and isn't even wired to the embedded-interpreter runner this
    phase targets (it applies to `native_jail_backend.cpp`'s separate spawned-child-process exec
    model). Raising `TimeoutError`/`MemoryError` *inside* a live interpreter needs a genuinely new
    subsystem — a watchdog thread with `PyErr_SetInterrupt`/`Py_AddPendingCall`, and a custom
    `PyMemAllocatorEx` hook, respectively — not a wiring pass.
  - *Tool denied by policy* — already correctly wired and tested
    (`test_mediated_python_runner_agent_tools.cpp`'s G1-N1: a real 006 §3 pipeline denial, not a
    shortcut). Best-covered row; untouched this phase.
  - *Command not found* — the WRONG SHAPE entirely: a hard-stop script failure
    (`"shell.command_not_found"` in `mediated_shell_dispatch.cpp`'s `kHardStopCodes`), not "nonzero
    exit + stderr line" as the table specifies.

  Presented to the project owner as a scoping choice (matching G2's own precedent): build the rows
  with a real fix reachable this pass, or also take on live quota enforcement, while naming wall-
  clock/memory as blocked. **Chosen: fix path/mount, host-not-permitted, and command-not-found; also
  build live quota enforcement; leave wall-clock and memory named, explicitly, as residuals** — both
  are exactly the kind of hot-path, security-adjacent new subsystem CLAUDE.md's own process
  ("contested, hot-path, or security-critical designs go through design → red-team → prove → judge,"
  producing an ADR) exists for, not a same-pass wiring job a work-breakdown phase should absorb
  silently.

  **What was built:**
  - **Path outside a mount / quota / listdir failures** now raise the REAL, correctly-typed,
    correctly-worded CPython exception. `core/error.hpp`'s `error` struct grew one field,
    `native_code` (a win32/errno passthrough, 0 = none); `worktree_mount_fs.cpp`'s `win_error` sets
    it from the real `GetLastError()`, and the one mount-escape policy error sets it to
    `ERROR_ACCESS_DENIED` (there is no real OS code behind a policy denial, so this is the closest
    real occurrence of "you may not reach this"). `mediated_python_runner.cpp`'s new `raise_os_error`
    helper calls `PyErr_SetFromWindowsErr` when a code is present — CPython's OWN errno-mapping table
    picks the right subclass (`FileNotFoundError`, `PermissionError`, ...) with CPython's own wording,
    never this file's approximation. Falls back to the pre-G4 policy/generic-`OSError` split,
    unchanged, for the contract-class errors that have no OS code behind them at all (invalid path
    encoding, forbidden character). Replaces `Internal_open` and `Internal_listdir`'s previous
    `PyErr_SetString(policy ? PermissionError : OSError, hand_authored_message)`.
  - **Live quota enforcement** — `core/worktree_mount_fs.hpp`'s new `mount_root_usage(mount_root)`
    walks a real, host-owned mount directory recursively (an explicit stack, not recursion, since
    guest-driven depth isn't bounded; reparse points are never followed, so a junction a prior write
    planted inside the mount can't turn a usage scan into an unbounded/cyclic walk — an availability
    precaution for a usage counter, not a second ADR-014). `Internal_open`'s write-mode branch checks
    live usage against the granted `cap::FsWrite`'s `quota_bytes`/`file_count_cap` BEFORE granting a
    new write-mode `open()`, raising `OSError("No space left on device")` — the exact literal
    `mount_write` already uses, not re-authored — when either axis is already exceeded. Named,
    narrower scope, stated plainly in both the header comment and the test: checked at the open()
    boundary, not intercepted per-byte on an already-open handle, so a single `open()`+`.write()`
    call's own volume can still push usage past the cap between checks — the same boundary
    granularity `mount_write`'s own batched CAS-based check already has.
  - **A real bug found and fixed while building the quota check, before it could even be tested**:
    `Internal_open`'s existing write-mode capability gate built a "requested" `cap::FsWrite` with
    `quota_bytes`/`file_count_cap` left `std::nullopt`, then called `CapabilitySet::contains()`. Per
    `capability.hpp`'s own documented `cap_covers` rule ("a capped parent and an uncapped request is a
    WIDENING attempt, never an implicitly-fine omission" — correct for `attenuate()`/`bind()`, where a
    request really is asking to mint a new, independently-reusable capability), this meant a granted
    `FsWrite` with any real `quota_bytes` cap was UNUSABLE for the embedded Python runner at all —
    every write denied as "no capability grants write access," regardless of actual usage, before this
    phase's quota logic could ever run. No existing test caught it because none had configured a
    quota-capped grant. Fixed with a new, narrowly-scoped, ADDITIVE `CapabilitySet::find_fs_write`
    (structural mount_id/path_prefix lookup, deliberately NOT `subsumes()`-based) used in place of
    `contains()` for exactly this one call site — `subsumes()`/`contains()`/`attenuate()`/`bind()`
    themselves are untouched, so no other caller's behavior changes.
  - **Host not permitted** — `Internal_do_connect`'s capability-denial branch and
    `raise_mapped_tool_error`'s new `"net.address_blocked"`/`"net.host_unresolvable"` cases both raise
    `ConnectionRefusedError` (a real `ConnectionError` subclass, one of the table's two sanctioned
    choices) via a new `raise_connection_error` helper — `PyErr_SetExcFromWindowsErr` against
    `WSAECONNREFUSED`, the same real, win32-code-sourced text a genuinely refused connection would
    produce, so a policy-blocked host stays indistinguishable from an unreachable one (026 §1a). The
    tool-call path works because `tool_pipeline.hpp` step 9 passes a tool's own `error` through
    verbatim, `.code` included — a bridged tool whose `invoke()` propagates a `net_egress_proxy`
    failure unchanged reaches `raise_mapped_tool_error` with `"net.address_blocked"` intact, proven
    with a purpose-built `BlockedNetTool` test double, not assumed.
  - **Command not found** — moved out of `mediated_shell_dispatch.cpp`'s `kHardStopCodes` into the
    same "ordinary command-level failure" path a failed `cat` on a missing file already uses
    (`ExecOutcome{klass: policy_violation, stderr_text: ...}`) — no new field needed on `ExecOutcome`;
    reusing `klass != ok` as the "nonzero exit" signal an already-established pattern in this same
    function already provides. The message itself changed from `"command not found: " + name` to
    `name + ": command not found"` — real bash phrasing for exactly this condition, not a bespoke
    wording.
  - **Left untouched, on purpose**: the analogous `cap::FsRead::size_cap_bytes`/`cap::NetOut::byte_cap`
    instances of the SAME `contains()`-vs-capped-grant bug (Internal_open's read branch,
    Internal_listdir, `Internal_do_connect`'s own allowlist check) are not exercised by anything this
    phase built or tests, and fixing them isn't this row's job — named, not silently carried forward
    as if they'd been checked.

  **Residuals, named explicitly**: **wall-clock enforcement** and **memory-cap enforcement** *inside*
  the embedded interpreter remain unbuilt. Both need a genuinely new subsystem (a watchdog thread
  interrupting the interpreter mid-execution; a custom Python allocator hook translating a budget hit
  into `PyErr_NoMemory()`) that CLAUDE.md's own process reserves for `design → red-team → prove →
  judge` and an ADR, not a work-breakdown phase. `tool.deadline_exceeded`'s existing `TimeoutError`
  mapping (a single TOOL CALL's own deadline, already correct, G1-era) is unaffected and unchanged.

  Proven in a new, portable-to-build test (`tests/test_mediated_python_runner_error_mapping.cpp`,
  10 checks: G4-R1 for path/mount, G4-R2-Q1 through Q3 and C1 through C3 for both quota axes, G4-R3-1
  and R3-2 for host-not-permitted on both the raw-socket and tool-call paths) plus fixes to two
  PRE-EXISTING tests whose assertions encoded the old, now-superseded behavior:
  `test_mediated_shell_runner_smoke.cpp`'s E3-N1 and `test_mediated_shell_runner_hostile_corpus.cpp`'s
  E4-SH3/SH4/SH6 (hard-stop → inspectable non-ok outcome), and
  `test_mediated_python_runner_hostile_corpus.cpp`'s E4-PY7 (`PermissionError` →
  `ConnectionRefusedError`) — the underlying security property each of these hostile-corpus checks
  exists for (never dispatched; the real `connect()` never reached) is unchanged, only the shape of
  "denied" is, and each fix says so in its own comment. Full regression clean on both trees except the
  one pre-existing, previously-established flake (`test_native_jail_backend_windows`): default `build`
  55/55 (55 total: G4 added no new default-tree binary, only fixed two existing shell test files), and
  `build-py` 66/66 (one new binary, `test_mediated_python_runner_error_mapping`, plus the two hostile-
  corpus fixes). **L** (larger than the **M** estimate — the row-by-row survey revealed several
  independent real gaps rather than one wiring pass, and fixing the `find_fs_write` capability bug was
  a precondition for the quota row to be testable at all)

### Phase H — The central falsifiable claim (026 §1a, §8 G4)

- **H1.** A small reference-agent task corpus (026 §8 G1/026 §10 Q5, 010 §9 G1) — the smallest set
  that makes both gates real rather than assumed: enough tasks to measure first-attempt execution
  success under the §7 token budget, and one NumPy+pandas task producing a chart artifact identically
  across the target platform set from the pinned `preinstalled` image (010 §5, §10 Q1). Explicitly
  NOT a large curated corpus (decision 7) — sized to make the gate executable, named as narrower than
  the RFC's own eventual bar. **L**
- **H2. The milestone's central claim.** Re-run the full 008/010 hostile suites (Phase C/E's own
  corpora) against an agent explicitly told it is sandboxed, with accurate architecture detail
  injected into its prompt (026 §1a, gate G4). Containment results must be byte-for-byte identical
  to the baseline (transparent-by-default) runs. If they are not, 026 is wrong and gets rewritten —
  stated as plainly here as the RFC states it, not softened for a work-breakdown doc. **M**

## What's explicitly deferred past M3

- **019-dependent persistence** (Phase D3): full checkpoint/restart/node-migration survival,
  retention/GC, redaction reaching the object store. M4's job.
- **029-dependent modules**: `agent.memory`, `agent.notes` (026 §5's table ties both to 029 §4/§5
  directly). M4's job, alongside 029 itself.
- **`remote` sandbox profile** for the interpreter/shell (010 §2's table already scopes it out) and
  for the worktree's node-migration story (025 §9 G1's `remote` half). M9's job, unchanged from every
  earlier milestone's own deferral of `remote`.
- **023-baselined budgets** for every gate that names one (025 §9 G4, 010 §9 G5) — 023 stays
  `TBD-baselined` project-wide until M8, the same status quo M1/M2 already established.
- **Linux path-escape parity's own full corpus** beyond C4's initial pass, and Linux `ShellRunner`/
  `PythonRunner` mediation parity generally, if C2/E2-E4's Windows-first sequencing (matching 021 §2's
  and M2's own precedent) leaves a documented gap the way M2's fs-escape/`/proc` gaps were tracked as
  GitHub issue #5 rather than silently closed.
- **A large, curated reference-agent task corpus** for 026 §8 G1's real statistical claim (decision 7,
  H1) — H1 builds the smallest corpus that makes the gate executable, not the eventual production
  corpus.
- **JS/TS `execute_code` language option** (010 §2, 026 §10 Q4) — both RFCs already defer this
  explicitly as lower-priority than Python; nothing in M3 changes that.

## Handover note (review-signoff workflow §5)

Started 2026-08-06, immediately following M2's close (three post-M2 residuals — ADR-012, the
`ResourceLimits::net_bytes` reconciliation, ADR-013 — all committed locally, not yet pushed).
Decisions 1-7 are now all confirmed, including the two (4, 6) that started as recommendations
pending explicit project-owner sign-off — both confirmed 2026-08-06, matching this doc's
recommended option in each case. One live caveat remains open, not a decision but a fact to
re-check: 025 has uncommitted edits from a concurrent session, noted above; re-check before Phase A
begins in earnest, since this whole doc is written against the working-tree text as it stood when
this breakdown was authored.
