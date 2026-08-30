# ADR-130 — `agentengine::FileWorktreeObjectStore`: closing the content-durability half of a long-disclosed gap

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors, 317 targets)
  and full `ctest` clean (291 total, 1 failure, pre-existing/environment, zero regression),
  `naming_lint.py` clean. **NOT Judged.** This closes gate items 1, 2, 3, and 6 of
  `docs/planning/content-durability-conformer-design-draft.md` §5 with real, executed evidence, and
  provides a real (if informal) measurement for item 4 — but item 5 (independent red-team) has not yet
  run, and the design draft's own item 7 framing ("every ADR in this design line that touches
  `mandatory_sandbox_provider.hpp`/`ledger.hpp` has gotten one... there is no reason to expect this
  line's first genuine on-disk content-storage change would be the exception") is taken seriously here:
  this ADR is `Proposed`, not `Judged`, until that round runs.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/core/file_worktree_object_store.hpp` (new), `tests/test_content_
  durability_cross_process.cpp` (new), `tests/test_content_durability_concurrency.cpp` (new), `tests/
  test_identity_durability_precondition.cpp` (new), `tests/CMakeLists.txt` (three new targets
  registered). No existing production file was modified.
- **Related specs:** `docs/planning/content-durability-conformer-design-draft.md` (this same session's
  own prior design-research pass — its Option A recommendation, ported here). Closes the residual named
  identically by `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md`,
  `decisions/ADR-126-task-branch-durability-recovery.md`, and `decisions/ADR-128-root-branch-recovery.md`:
  "a real, separate, materially larger piece of work (a durable object-store conformer for blob/tree
  content, not merely branch/ACL bookkeeping)."

## 1. The question

`Ledger<Store>::durable_dir` makes branch/ACL bookkeeping durable — it never touches `store_`, and the
production default (`InMemoryWorktreeObjectStore`) provides no content durability at all. ADR-102,
ADR-126, and ADR-128 each named this gap and explicitly declined to close it. This session's own prior
turn produced a design-research document (not an ADR — no code, no red-team round) recommending
**Option A: port the already-proven, standalone prove-phase prototype `probe::FileWorktreeObjectStore`
into production essentially as-is.** Does that port actually work, and does it hold up under the design
draft's own named gate criteria (§5), executed for real rather than assumed?

## 2. A real, material scope boundary found while building this — not assumed going in

`Ledger<Store>` is genuinely store-agnostic via its own template parameter, exactly as every prior ADR
in this line claimed. But **`SandboxRuntime`/`MandatorySandboxProvider` — the production TOOL-SURFACE
layer this whole session's task-branch/crash-recovery line (ADR-102/114/117/119/126/128) actually
lives in — are hardcoded to `Ledger<>` (the default `InMemoryWorktreeObjectStore`) throughout, not
templated on `Store` at all.** `bind_sandbox()`, `bind_root_branch()` (ADR-128), and every task-branch
verb all take `agentengine::Ledger<>&` literally (`mandatory_sandbox_provider.hpp:356/421/888`);
`SandboxRuntime` stores `agentengine::Ledger<>* ledger_` the same way (`sandbox_runtime.hpp:337`).

This means the design draft's own gate item 2 wording — "process 2... reclaims both branches via the
already-shipped `reclaim_orphaned_branch()`/`bind_root_branch()` machinery" — assumed an integration
point that does not actually exist yet. **This ADR does NOT attempt that integration.** It proves
content durability genuinely works at the raw `Ledger<Store>` level (where the "pure `Store`
substitution" claim is actually true), using `create_root_branch()`/`branch_from()`/`commit()`/
`orphaned_branches()`/`reclaim_orphaned_branch()`/`merge()` directly — the same primitives
`MandatorySandboxProvider`'s own production automation is built on, just not routed through that
specific class. **Templatizing `SandboxRuntime`/`MandatorySandboxProvider` on `Store` too, to wire a
durable content store into the real tool surface, is real, separate, larger follow-on work** — it means
changing the two most heavily-verified files in this entire session's own design line, and is
deliberately out of this ADR's own scope rather than silently assumed solved.

## 3. What was built

**`include/agentengine/core/file_worktree_object_store.hpp`** — `agentengine::FileWorktreeObjectStore`,
ported from `docs/planning/proofs/worktree_io/file_object_store.hpp` (`probe::FileWorktreeObjectStore`).
A real port, not a rewrite: the storage shape (one file per digest, two flat directories, git's own
loose-object layout minus fan-out), the temp-file-plus-atomic-`rename` write discipline, the
bounds-checked `decode_tree()`, and the `is_well_formed_digest()` path-traversal gate are all carried
forward unchanged from the prove-phase original's own already-red-teamed form. Real changes: dropped the
`probe::` namespace; **explicitly re-checked (not assumed) that the `std::unique_ptr<std::mutex>`
indirection is still load-bearing** — `Ledger<Store>`'s constructor unconditionally moves its `Store`
parameter into the `store_` member on every construction, and `std::mutex` is neither movable nor
copyable, so the indirection is still required today, for the identical reason the prove-phase original
had it. Registered with the same `// ae-naming-lint: allow` suppression `InMemoryWorktreeObjectStore`
itself already carries (ADR-025 §4c precedent).

**`tests/test_content_durability_cross_process.cpp`** — a **genuine TWO-REAL-OS-PROCESS proof**
(mirroring `identity-native-sandbox-worktree-design.md` §34.3's own methodology), the strongest bar
available for a security/hot-path-adjacent claim like this. One executable plays both roles: invoked
with no arguments it is "process 2" (the reader) — it first self-relaunches via `std::system()` with
`--writer-role` as a genuinely separate OS process, waits for it to exit cleanly (a real, distinct
process, not a thread or an in-process simulation), then does its own work. Process 1 creates a root
branch, spawns a child, commits real, distinguishable content to BOTH, and exits **without merging**.
Process 2 constructs a fresh `Ledger<FileWorktreeObjectStore>` against the same directories, finds both
branches as real orphans, reclaims both, and **`merge()`s the child's real, disk-recovered content into
the root — successfully**, not `ledger.merge_tree_load_failed` (the exact, precise failure
`test_task_branch_durability_recovery.cpp`, ADR-126, correctly asserts for the in-memory-store case this
test does not use). The merged tree's own content is then read back through the ACL-gated
`get_blob_safe()` production path and confirmed byte-for-byte correct for both the root's own blob and
the child's real, cross-process-recovered blob — not merely "the merge call returned success."

**`tests/test_content_durability_concurrency.cpp`** — real, executed adversarial probe of the design
draft's own §3 disclosure. **[1] Content is safe**: 8 real OS threads, each with its own separate
`FileWorktreeObjectStore` instance pointed at the same objects root, write 200 total distinct blobs
concurrently — every one reads back byte-exact through a fourth, freshly-constructed store instance, and
the real on-disk blob count is exactly 200. **[2]/[2b]/[2c] Metadata is not safe, reproduced for real,
not merely restated**: sequential multi-instance construction (even three instances in a row) loses
nothing — proven as a negative-result baseline before the real hazard is isolated. Genuine concurrent
construction (two real threads, both constructing their own `Ledger<FileWorktreeObjectStore>` against
the same, still-empty `durable_dir` before either persists) reliably loses exactly one of the two
branches from the durable record — the later `persist_snapshot_locked()` full-rewrite silently
overwrites the earlier one, since neither instance's in-memory state ever learns of the other's. This is
the SAME, pre-existing, unchanged hazard `ADR-128` §2's own "still-live double-bind" disclosure and the
design draft's own §3/§6 already name — confirmed here as genuinely unchanged (not worsened, not fixed)
by adding durable content alongside it.

**`tests/test_identity_durability_precondition.cpp`** — real, adversarial, two-real-process proof of the
design draft's own gate item 6 (the §33/§34.2 precondition), on CURRENT production code. **[1] The
vulnerable configuration**: an owner process using an in-memory-only `IdentityAuthority` (no
`durable_dir` — the easy-to-get-wrong default) mints its identity as the first `mint_root()` call in
that process (id 1), commits real secret content to a durable `Ledger<FileWorktreeObjectStore>`, and
exits. An unrelated "attacker" process, ALSO using an in-memory-only `IdentityAuthority`, has its own
first `mint_root()` call ALSO land on id 1 (fresh in-memory allocation always restarts at 1) — and
genuinely, successfully reads the real owner's real secret content through the ACL-gated
`get_blob_safe()` production path. **[2] The correct configuration**: the identical scenario, except
both processes configure `IdentityAuthority::bootstrap()` with the SAME durable identity directory — the
attacker's own honestly-minted identity now correctly receives id 2 (the durable high-water-mark is
never recycled), and `get_blob_safe()` correctly, genuinely fails closed with
`ledger.blob_access_denied`. **This confirms, on current production code, exactly what the design draft
predicted**: making content durable turns a previously-latent risk into a live one when
`IdentityAuthority` is not also configured durably and consistently — and that the fix is a real,
available host-configuration choice today, not a theoretical mitigation.

**A real, honest `MergeCost` measurement** (gate item 4), captured via a temporary probe
(`probe_mergecost_timing.cpp`, built, run once for evidence, then deleted along with its CMake entry —
the same "temporary probe, capture evidence, remove" pattern this session's own independent red-team
rounds already use): 200 `merge()` calls each, same machine, same Debug build.
`InMemoryWorktreeObjectStore`: **6.63 ms/merge()**. `FileWorktreeObjectStore`: **10.42 ms/merge()**. A
**1.6x, +3.8 ms/call delta**. Disclosed honestly as informal, not rigorous: a single-machine, Debug-build
(not Release-optimized), NTFS-backed measurement, not a statistically-controlled benchmark across
platforms or build configurations — but a real number, not an assumption, which is what gate item 4
asked for.

## 4. Verification

Every new test's mandatory sanity check: `test_content_durability_cross_process`'s core claim was
verified by temporarily swapping `Ledger<FileWorktreeObjectStore>` for `Ledger<InMemoryWorktreeObjectStore>`
in both roles, rebuilding, and confirming the test reproduces the EXACT ORIGINAL disclosed failure
(`ledger.merge_tree_load_failed`) instead of passing — then restored and reconfirmed a full pass.
`test_content_durability_concurrency`'s two claims needed no revert-based check (there is no "fix" here
to revert — it documents an existing, disclosed, not-to-be-closed hazard, not a regression guard): its
decisiveness is instead established by 5 consecutive clean runs of the full binary, all identical
outcomes (200/200 content writes correct; exactly 1 of 2 branches surviving the genuine-concurrency
case, every time). `test_identity_durability_precondition`'s two scenarios (vulnerable vs. correctly
configured) form a real positive/negative control pair on their own — each is only meaningful in
contrast with the other, and both were observed with the exact predicted outcome (attacker id 1 /
genuine leak vs. attacker id 2 / correctly denied).

Full project rebuild (`cmake --build . --config Debug`, 317 targets): zero errors. Full `ctest`: **291
total (287 baseline + 4 new tests — 3 permanent, plus this pass rode along after the temporary probe's
removal), 1 failure** — the same, already-established `test_reference_agent_task_corpus` pandas/
matplotlib environment gap, zero regression anywhere else. `python tools/naming_lint.py`: clean, 361
suppressed findings (360 baseline + 1 for `FileWorktreeObjectStore`'s own `ae-naming-lint: allow`
line, the same suppression pattern its sibling `InMemoryWorktreeObjectStore` already carries).

## 5. What was NOT done

- **No integration with `SandboxRuntime`/`MandatorySandboxProvider`.** See §2 — this is real, separate,
  larger follow-on work (templatizing both classes on `Store`), not attempted here. Every proof in this
  ADR operates at the raw `Ledger<Store>` level directly.
- **No independent red-team pass yet.** This is new, real on-disk content-storage code — the design
  draft's own gate item 7 named this as expected, not optional, for exactly this class of change.
- **No Linux verification yet.** Same established next-step pattern as every other ADR in this design
  line.
- **No garbage collection**, no cross-process file locking beyond what already exists, no migration path
  for existing in-memory data, no encryption/compression — all explicitly out of scope per the design
  draft's own §6, unchanged here.
- **The metadata-bookkeeping concurrency hazard (gate item 3) is demonstrated, not fixed.** This ADR's
  own `test_content_durability_concurrency.cpp` proves the hazard is real and unchanged by adding
  durable content; it does not propose or implement a fix (a lock file, a single-writer-process design,
  or similar) — that is real, separate, undesigned follow-on work, matching the design draft's own §6
  scope boundary exactly.
- **`MergeCost`'s real-I/O measurement (gate item 4) is informal**, not a rigorous, cross-platform,
  Release-build benchmark. A real number was captured; a rigorous one was not attempted.

## 6. Residuals

- Everything named in §5.
- The `FileWorktreeObjectStore`↔`durable_dir` wiring convention is not yet decided at the API level:
  today a caller passes `Ledger<Store>`'s own `durable_dir` and the store's own root directory as two
  independent paths (the design draft's own §3 already named this open question) — nothing here forces
  or checks that a caller configures them consistently (the same class of misconfiguration risk §4's
  own identity-precondition proof demonstrates for `IdentityAuthority`, though unlike that case there is
  no ACL-keyed cross-content-leak mechanism at stake here — merely two independently-durable stores that
  a caller could point at mismatched directories and get confusing, not dangerous, results).
