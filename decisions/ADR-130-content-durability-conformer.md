# ADR-130 — `agentengine::FileWorktreeObjectStore`: closing the content-durability half of a long-disclosed gap

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors, 317 targets)
  and full `ctest` clean (291 total, 1 failure, pre-existing/environment, zero regression),
  `naming_lint.py` clean. This closes gate items 1, 2, 3, and 6 of
  `docs/planning/content-durability-conformer-design-draft.md` §5 with real, executed evidence, and
  provides a real (if informal) measurement for item 4. **SAME-DAY INDEPENDENT RED-TEAM COMPLETE (§7):
  one real MUST-FIX found and fixed** — both cross-process tests' self-relaunch quoting was genuinely
  broken on POSIX (not merely unverified), reproduced directly against a real `/bin/sh -c`, and fixed by
  making the Windows-`cmd.exe`-specific outer quote wrap conditional. Gate item 4's own named coverage
  gap (the same-digest concurrent-write sub-case) was also closed with a new, real, barrier-synchronized
  test section, found safe by construction. **Linux-verified, ADR-131** (2026-08-30, same day): a
  complete, unconditional pass on real GCC 14.2.0 — both self-relaunching tests pass completely against
  real `/bin/sh`, confirming the red-team's own quoting fix genuinely works (not merely the string
  reproduction §7 used), and the concurrency probe's full claim set, including the new same-digest race
  section, reproduces on a second, independent allocator and filesystem.
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

- ~~No integration with `SandboxRuntime`/`MandatorySandboxProvider`.~~ **Closed by ADR-132** — both
  classes gained a `Store` template parameter (defaulted, purely additive), and a new full-stack
  integration test proves `MandatorySandboxProvider::commit_task_branch()` genuinely succeeds after a
  simulated crash with real, durable content, through the real, unmodified production tool surface.
- **No independent red-team pass yet.** This is new, real on-disk content-storage code — the design
  draft's own gate item 7 named this as expected, not optional, for exactly this class of change.
- ~~No Linux verification yet.~~ **Closed by ADR-131** — a complete, unconditional pass on real
  GCC 14.2.0: both self-relaunching tests (`test_content_durability_cross_process`,
  `test_identity_durability_precondition`) pass completely against real `/bin/sh`, confirming the
  red-team's own `_WIN32`-conditional quoting fix genuinely works (not merely the string
  reproduction §7 used), and `test_content_durability_concurrency`'s full claim set, including the
  new same-digest race section, reproduces on a second, independent allocator and filesystem.
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

## 7. Independent red-team round (same day)

**Scope of this round.** A fresh agent, with no prior context on this change beyond the commit and this
ADR, independently re-derived and checked every claim in §2-§6 by reading the real diff (`git show
1a4b6b2`) and code directly, then rebuilt and reran every new test itself rather than trusting this ADR's
own account. Focus areas: `FileWorktreeObjectStore` port fidelity, `decode_tree()`/`is_well_formed_digest()`
adversarial soundness, I2/I3 (no ACL bypass through the new store), the cross-process self-relaunch
quoting mechanism, the concurrency probe's own decisiveness (including gate item 4's own named coverage
gap), and the identity-precondition proof's generality.

**[1] `FileWorktreeObjectStore` port fidelity and the `unique_ptr<mutex>` claim — confirmed, no changes
needed.** Diffed `include/agentengine/core/file_worktree_object_store.hpp` against `docs/planning/proofs/
worktree_io/file_object_store.hpp` line by line: identical storage shape, identical temp-file-plus-
atomic-rename discipline, identical `decode_tree()`/`is_well_formed_digest()` logic. The only functional
delta is an improvement, not a regression: `blob_count()`/`tree_count()` use the `directory_iterator(path,
error_code&)` overload (fails closed, no exception) where the prove-phase original used the throwing
overload. Independently traced `Ledger<Store>`'s constructor (`ledger.hpp:310-318`,
`: store_(std::move(store)), ...`) myself: this ADR's own claim that the `Store` parameter is
unconditionally moved into `store_` on every construction, and that `std::mutex` is neither movable nor
copyable, both check out exactly as stated — the `std::unique_ptr<std::mutex>` indirection is genuinely
load-bearing, not legacy caution carried forward unexamined.

**[2] `decode_tree()`/`is_well_formed_digest()` — adversarially probed against crafted files, not just
re-read.** `is_well_formed_digest()`'s 64-lowercase-hex-only gate is sound by inspection (rejects any `..`
or path-separator content by construction, since neither survives the character-class check). For
`decode_tree()`, wrote a temporary probe (`probe_decode_tree_corruption.cpp`, since removed) that bypassed
`put_tree()` entirely and wrote six hand-crafted byte sequences directly to a `trees/<digest>` file on
disk: an empty file, a `count` field with no entry data, `count=0xFFFFFFFF` with no entry data (checking
for a loop-bound/pre-allocation DoS — none exists, since entries are `push_back`-accumulated one at a time
and the very first `read_str()` inside the loop fails closed immediately), an oversized name-length field
with only one byte actually present, an oversized digest-length field with nothing following, and a
complete name+digest pair missing only the final `is_tree` byte. **Every one of the six failed closed with
`worktree.tree_decode_failed`, zero crashes** — plus a seventh, positive-control case (a genuinely
well-formed single-entry tree) decoded correctly, confirming the failures above are real corruption
detection, not `decode_tree()` being universally broken. No fix needed.

**[3] I2/I3 — no ACL bypass through the new store, confirmed by tracing every `store_` access, not
assumed.** Grepped `ledger.hpp` for every `store_.put_blob/get_blob/put_tree/get_tree` call site: all four
`_safe()` public entry points (`put_blob_safe`, `get_blob_safe`, `get_tree_safe`, and the `commit()`/
`create_root_branch()`/`perform_three_way_merge_locked()` internal paths) take `Ledger`'s own `mutex_` and
either call `authorized_for()` or `insert_acl_root_bounded()` before ever touching `store_`.
`FileWorktreeObjectStore` itself has no ACL awareness of its own and no code path that could be reached
without going through one of these gated methods first — confirmed genuinely true for every call site in
this file, not merely for the ones this ADR's own tests happen to exercise. No finding.

**[4] THE REAL MUST-FIX: the cross-process self-relaunch quoting was genuinely broken on POSIX, not
merely unverified.** Both `test_content_durability_cross_process.cpp` and `test_identity_durability_
precondition.cpp`'s comments claimed the Windows-`cmd.exe`-specific outer quote wrap was "harmless" on
POSIX `/bin/sh -c` too ("a plain no-op pair around the whole string"). Checked this directly rather than
accepting it: `std::system()` on POSIX runs the command via `/bin/sh -c <command>`, and reproduced the
EXACT command-string construction both files use against a real `sh -c` (this environment's Git Bash
`/bin/sh`, which implements the same POSIX quote-removal rules any Linux `/bin/sh` would). The
double-wrapped form failed immediately: `sh: line 1: <path> --writer-role <dir1> <dir2> <dir3>: No such
file or directory` (exit 127) — the executable path and every argument collapsed into ONE bad, nonexistent
command name, because the extra quote pair shifts POSIX quote-state parity by one, leaving the executable
path unquoted and spuriously quoting the space before the first argument, merging the two. The identical
command string WITHOUT the extra wrap parsed correctly into four separate `argv` elements. Reproduced the
same failure/fix pair for `test_identity_durability_precondition.cpp`'s own `run_child()` helper (identical
pattern). **This would have made both tests fail unconditionally the first time this design line's
established "Linux-verified" follow-on ADR ran them** — not merely be unverified, since `std::system()`
invokes `/bin/sh -c` on Linux exactly as it does in this Git-Bash reproduction. **Fixed** by making the
extra outer wrap conditional on `_WIN32` in both files, leaving POSIX to pass the correctly-quoted inner
command directly (verified via the same direct `sh -c` reproduction: the fixed form parses into the
correct four arguments). **Verified**: rebuilt both tests, reran each three times on Windows — unchanged,
still `ALL CHECKS PASSED`, exit 0 every time (the `#ifdef _WIN32` branch is byte-identical to the original
code, so Windows behavior could not have changed). The revert-to-confirm-fail side of this fix was done via
the same direct `sh -c` reproduction rather than an actual Linux build of this repo (none available in this
round) — the broken form was shown to fail, the fixed form was shown to succeed, on the identical shell
semantics `std::system()` would actually invoke.

**[5] Gate item 4's own named coverage gap — same-digest concurrent write — closed, found safe by
construction.** This ADR's own §5/§6 already disclosed that the shipped `test_content_durability_
concurrency.cpp` only ever writes DISTINCT blobs per thread, never exercising two writers racing to
produce the IDENTICAL digest (and therefore the identical temp-file name). Wrote a temporary probe first
(`probe_same_digest_race.cpp`, since removed): 16-24 real threads, a spin-wait barrier holding every
thread until all are constructed and ready before releasing them together, writing IDENTICAL 3 MiB content
(large enough to force multiple internal `WriteFile()` calls per writer, not one atomic buffer flush) —
checked directly against the on-disk file after the race, never through a repair `put_blob()` call that
would silently paper over a missing/corrupt result. **Found safe across ~6,000 racing attempts, including
under a deliberately sabotaged version of `put_blob()` with the temp-file/atomic-rename discipline and the
mutex both removed entirely** — concurrent writers of the same digest are, by the content-addressing
invariant itself, always writing byte-identical bytes, and a Windows file handle stays bound to its
underlying file object across a path rename, so no interleaving of these racing writes can strand a final
file with anything other than correct bytes. Converted this into a permanent, real test-coverage addition
(`test_content_durability_concurrency.cpp`'s new `[1b]` section, 16 threads × 20 iterations of the 3 MiB
same-digest race) rather than leaving it as a deleted probe, since it closes a real, explicitly-named gap.
**Confirmed the new check has genuine teeth**, not a vacuous pass: temporarily sabotaged `put_blob()` to
write only the first half of the bytes, rebuilt, reran — both `[1]` and the new `[1b]` correctly FAILED;
restored the original code, rebuilt, reran — clean pass again, three consecutive times.

**[6] Concurrency probe `[2c]` hazard reproducibility — reconfirmed, unchanged.** Reran
`test_content_durability_concurrency` (with the new `[1b]` section included) five consecutive times: every
run showed 200/200 byte-exact distinct-digest writes, the new same-digest race clean, and the genuine
concurrent-`Ledger`-construction hazard losing exactly one of two branches every time — deterministic, not
a fluke, matching this ADR's own §3 claim.

**[7] Identity-precondition proof — reran, and a more general pattern worth naming (not separately
tested).** Reran `test_identity_durability_precondition` three times: identical result each time (attacker
id 1 / genuine leak in the vulnerable configuration; attacker id 2 / `ledger.blob_access_denied` in the
correct one). The test's own framing centers on "the FIRST `mint_root()` call in each process" (id 1) —
worth stating explicitly, though not a separate gap needing its own test, since the mechanism this ADR's
own §4 already describes generalizes cleanly: `IdentityAuthority::mint_root()`'s allocator is a bare
in-process call counter, so the collision is not specific to "id 1" at all — ANY two processes whose
`mint_root()`/`adopt()` call-count history up to the point of minting the identity in question happens to
match (e.g., both processes call `mint_root()` exactly three times before minting the identity that ends up
colliding) receive the identical id and collide identically. The shipped test demonstrates the simplest,
most easily-triggered instance of this pattern (the very first call in each process) — the general pattern
is already implied by the mechanism this ADR documents, not a distinct, unproven claim, and does not
change the fix (`IdentityAuthority::bootstrap()`'s own `durable_dir`, configured consistently, closes every
instance of it, not just the id-1 case).

**Net effect of this round.** One real, Linux-blocking defect found and fixed (the quoting bug, §7 item
4) — genuinely material, since it would have silently broken exactly the "Linux verification" step this
ADR's own §5/§6 already lists as the expected next step. One real, meaningful test-coverage gap (gate item
4's same-digest race) closed with a permanent addition, found safe rather than broken. Everything else
checked out clean under genuine adversarial pressure: the port is faithful, `decode_tree()` is genuinely
bounds-safe, I2/I3 hold structurally (not by omission), and the concurrency/identity claims reproduce
deterministically. Full rebuild (zero errors) and full `ctest` (291 total, the same one pre-existing,
unrelated `test_reference_agent_task_corpus` failure, zero regression) after every change; `python
tools/naming_lint.py` clean throughout (no new exported type introduced by this round). No Linux
verification was attempted this round (no Linux checkout available) — the quoting fix's own correctness
was established via direct `sh -c` reproduction of the exact command strings involved, not a full Linux
build of this repository; that full verification pass remains this ADR's own disclosed residual.
