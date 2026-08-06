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

- **F1.** `/work`, `/input`, `/out` wired as real `FsRead`/`FsWrite`-gated mounts (Phase C) into
  `PythonRunner`/`ShellRunner`'s sandbox spec — files created under `/out` are collected, digested
  (A1), and surfaced as `Content` items (003), closing 025 §7's "the agent saves a file, the user
  receives an artifact" claim end to end. **M**
- **F2.** `call_tool` bridge (010 §6) — bridged tool calls traverse the full 006 §3 pipeline (real
  since M2) at the sandbox's own trust tier and capability set, never the agent's; bundled approval
  at `execute_code` time over the pre-registered bridged set (010 §6, not per-call — 010 §10 Q2's
  resolution); results re-enter as tainted data (003 §2). Proven with the same capability-
  handle-reuse-denial discipline M2's B4 used. **L**
- **F3.** Output discipline (010 §3 item 4/5) — `stdout`/`stderr`/`result_repr` size-capped with
  explicit truncation markers, `result_repr` specifically covering "a value never `print()`-ed"
  (010 §3's own named gap). **S**

### Phase G — The `agent.*` library / CodeAct (026 §4, §5, §5a)

- **G1.** `agent.tools` as ordinary Python callables with real signatures/docstrings/`.pyi` stub,
  generated from the same tool metadata B1's JSON-schema derivation (006 §1, real since M2) already
  produces — 026 §4's "generated from the same tool metadata as everything else" claim, made literal
  rather than re-derived. **L**
- **G2.** The remaining eight `agent.*` modules (026 §5's table) over F1/F2/A1: `files`, `data`,
  `output`, `progress`, `ask`, `spawn` at minimum (`memory`/`notes` land with 029, out of scope for
  M3, named not silently dropped — 026's own table already ties them to 029 §4/§5). Each individually
  capability-gated, each passing 026 §5a's four-part curation rubric already applied in the RFC text
  itself (no new admission work needed, just building what already passed the test on paper). **XL**
- **G3.** `dir(agent)`/`help(agent)` wired to `trust/agent_library_manifest.hpp`'s already-real
  `granted_modules()` (026 §5a, §9 G7) — this is the one task in Phase G that is mostly integration,
  not new design, since the registry and its tests already exist. **M**
- **G4.** Error mapping (026 §3) — the closed table (`PermissionError`, `OSError`, `ConnectionError`,
  `TimeoutError`, `MemoryError`, "command not found") sourced from real occurrences of each exception
  class (026 §10 Q2's resolution — not hand-authored per site), never a host stack trace or
  architecture term (026 §8 G3/G6). **M**

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
