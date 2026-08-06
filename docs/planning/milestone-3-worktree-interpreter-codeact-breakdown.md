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
- **A3.** Compile-fail/positive-control proof that `Blob`/`Tree` objects are immutable once written —
  no public mutator exists on a fetched object, matching the M1/M2 `try_compile()` gate pattern
  (`tests/compile_fail/`). **S**

### Phase B — Sub-worktrees, sharing modes, concurrency and merge (025 §3-4)

- **B1.** Sub-worktree creation with a declared `sharing_mode` (`shared`/`branch`/`readonly`/
  `scratch`, 025 §3) over A1/A2 — `branch` as copy-on-write (a new `Ref` pointing at the parent's
  current tree digest, diverging on write), `readonly` as a pinned digest rejecting writes,
  `scratch` as a fresh empty tree discarded on completion. Default-by-concurrency (025 §3: sequential
  siblings default `shared`, concurrent siblings default `branch`) is a caller-supplied flag at this
  layer, not inferred here — inferring "concurrent" needs 001's run/turn scheduling context this
  header doesn't have. **M**
- **B2.** Three-way merge on `branch` join (025 §4) — disjoint changes merge automatically, identical
  content merges trivially, a genuine conflict fails closed and retains both versions at
  `/conflicts/<path>.<agent>` (never last-writer-wins, matching 025 §4's explicit rule and I3's "model
  output is data" — a model-drafted resolution is a proposal, never auto-applied, 025 §10 Q1's
  resolution). **L**
- **B3.** `shared`-mode staleness note (025 §3/§10 Q2's resolution) — a short diff-summary computed
  when a `shared` sub-worktree has moved since an agent's last read, reusing B2's own diff mechanism
  proactively rather than only at merge time. **S**
- **B4.** Concurrency proof (025 §9 G3, scoped per decision 7) — N concurrent `branch` agents produce
  a deterministic merge result over a machine-safe bounded randomized-interleaving count; every
  genuine conflict is surfaced, never silently resolved; a positive control (a deliberately miswired
  auto-apply path) is caught, proving B2's "never last-writer-wins" claim isn't vacuous. **L**

### Phase C — Mounts, capabilities, path-escape hardening (025 §5) — ADR-track, see decision 6

- **C1.** `FsRead<mount>`/`FsWrite<mount>` capability-gated mount resolution over A1/B1 — a worktree
  subtree becomes a guest-visible filesystem view only through a granted capability (007 §3), mount
  points are canonical/ordinary-looking paths (026 §2), never a runtime-revealing path. **M**
- **C2. ADR-track (decision 6).** Path-escape corpus: `..`, absolute redirects, symlink/junction/
  reparse-point boundary crossing, ADS, `\\?\` prefixes, unicode-normalization tricks, TOCTOU
  re-resolution — contained on Windows first (matching 021 §2's own platform-priority ordering and
  M2's own Windows-first sequencing for `native-jail`), with positive controls per attack class
  proving containment isn't incidental. Goes through design→red-team→prove→judge and produces
  `decisions/ADR-0NN-worktree-mount-path-canonicalization.md` before landing, per decision 6 — not
  an ordinary task, sized and process-gated as such. **XL**
- **C3.** Write quotas enforced at write time (025 §5, byte + file-count caps), surfacing as an
  ordinary `No space left on device`-class error inside the guest (026 §3) rather than a policy
  message — proven against C1's mount layer directly. **S**
- **C4.** Linux path-escape parity pass, once C2's Windows corpus and its ADR are settled — same
  sequencing precedent M2 used for `native-jail` (Windows first, Linux parity as its own dated task,
  C4 in that milestone's own numbering). Named here rather than silently assumed bundled into C2. **L**

### Phase D — Persistence, checkpoints, lifecycle (025 §6) — partial, real gaps deferred to M4

- **D1.** Turn-boundary commit — at each turn boundary, the current tree is committed (A1) and its
  digest recorded against the session's `Ref` (A2). Provable now without needing 019's full
  checkpoint/durability machinery, since it only needs A1/A2 to exist. **M**
- **D2.** Rewind as ref reassignment (025 §9 G5) — restoring an arbitrary retained turn digest points
  the `Ref` back at it and reproduces that tree exactly, proven via A1's content-addressing (fetching
  by digest is deterministic by construction). **S**
- **D3. Deferred to M4 (019, Durability and Long-Running Agents), named not silently dropped:** full
  session-checkpoint integration (025 §9 G1's "survives sandbox destruction, process restart, and
  simulated node migration"), retention/GC policy (025 §6), and redaction reaching the object store
  (025 §9 G6, 005 §6's redaction contract) — all three need 019's suspension/recovery machinery this
  milestone does not build, the same way M1 deferred 001 §9 G1's 10⁴-session gate and M2 deferred
  019-dependent items generally.

### Phase E — `ExecState`, `PythonRunner`, `ShellRunner` (010 §1a, §2, §3a) — see decisions 3-5

- **E1.** `ExecState{cwd, env}` made real and shared by reference across every `Runner` call for a
  session (010 §3a) — the concrete carrier `sandbox/runner.hpp`'s comment already names but doesn't
  yet build. **S**
- **E2.** `PythonRunner` satisfying `Runner`, under `native-jail`, embedding CPython per
  `AGENTENGINE_BUILD_PYTHON_RUNNER` — real per-call capability-freshness derivation from
  `EffectContext`/`CapabilitySet` (closing python_runner.hpp's own stated gap), `sys.meta_path`
  import allowlist per ADR-002/003's Judged findings, `open`/`socket`/`subprocess` mediation
  wrappers per 010 §9 G7's two claims (import allowlist, mediated calls) — written fresh per
  decision 4. **XL**
- **E3.** `ShellRunner` satisfying `Runner` — recursive-descent parser → pmr AST → tree-walking
  evaluator against `CommandRegistry`'s closed three-way lookup (builtin / registered `Runner` /
  registered `Tool`, 010 §2), per ADR-001's Judged Design A, written fresh per decision 4.
  `RunnerCall<python>` composition (010 §1a — `ShellRunner` invoking `PythonRunner` under an explicit
  capability, never exec'ing a real shell) proven directly, including the negative control (denied
  without the capability). **L**
- **E4.** Containment/mediation proof (010 §9 G2, G7) — the hostile corpus (`os.system`, `ctypes`,
  `/proc`/registry probing, symlink escape, egress to `169.254.169.254`, fork bomb, memory bomb,
  output flood, `sys.settrace`) plus the `ShellRunner`-specific classes (`PATH` hijacking, command
  substitution reaching an unregistered binary, builtin shadowing) — each proven to **never reach a
  reachable code path**, not merely be contained, per G2's stronger bar for the shell specifically.
  Reuses 008 §7's abuse-case corpus pattern (`tests/helpers/abuse_case_corpus.hpp`) where the same
  probe shape already applies. **XL**
- **E5. ADR-track (decision 5).** `ShellRunner` grammar-parser fuzzing (010 §9 G8) —
  libFuzzer-class, corpus-driven, under ASan/UBSan, gated in CI, with a deliberately reintroduced
  known-bug class as the positive control. Goes through design→red-team→prove→judge and produces its
  own ADR before landing, per CLAUDE.md and decision 5's reasoning. **L**

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
