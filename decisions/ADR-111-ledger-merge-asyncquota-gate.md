# ADR-111 — `Ledger::merge()` gets its own `AsyncQuota<MergeCost>` gate, closing a real I8 gap named across four prior phases

- **Status:** Proposed — implemented, verified against the real `InMemoryWorktreeObjectStore` (Windows/
  MSVC), full project rebuild and `ctest` clean outside 5 pre-existing, environment-caused failures (see
  §5) that share zero code with this change. An independent red-team pass on this exact fix found and
  this session fixed **one real, MUST-FIX data-loss bug in the fix's own first version** — see §4. A
  second, separate review pass caught a real CI-breaking gap (`MergeCost` had no
  `027-Vocabulary-and-Naming.md` row, which `tools/naming_lint.py`'s CI gate enforces — this exact
  branch's own recent history shows two prior "Fix naming_lint gap" commits for the identical class of
  miss) and a minor unqualified-vs-qualified style inconsistency; both fixed same day (`027-Vocabulary-
  and-Naming.md` row added, `sandbox_runtime.hpp`'s `agentengine::MergeCost` unqualified to match its
  sibling `BranchCost` usage three lines away) — `python3 tools/naming_lint.py` now reports clean.
  While fixing it, also closed two PRE-EXISTING, unrelated vocabulary-doc gaps from ADR-106
  (`ContainerdCliBackend`/`ContainerdExecutionSurface` had shipped with no vocabulary rows at all) that
  `naming_lint.py` surfaced in the same run — not this ADR's own defect, but cheap and directly
  actionable once found, matching this lineage's own "fix what an independent pass surfaces" posture.
  Not yet re-verified on Linux.
- **Date:** 2026-08-29.
- **Scope:** `include/agentengine/core/ledger.hpp` (`MergeCost` Kind tag, `Ledger<Store>::merge()`
  signature and body), `027-Vocabulary-and-Naming.md` (rows for `MergeCost`, and the two pre-existing
  `ContainerdCliBackend`/`ContainerdExecutionSurface` gaps found alongside it),
  `include/agentengine/sandbox/sandbox_runtime.hpp`
  (`SandboxRuntime::merge_into()` signature and body, threading the new quota through), and
  `tests/test_ledger.cpp` (two existing merge() call sites updated to pass a quota plus new
  consumption/refund proofs, and a new case [12] proving the exhausted-quota path). No other file
  changed — `SandboxRuntime::merge_into()` has zero production or test callers anywhere in this tree
  today (grep-confirmed; only a prove-phase file under `docs/planning/proofs/` calls a same-named
  method on its own, separate, standalone `SandboxRuntime` copy), so this ADR's blast radius is smaller
  than `merge()`'s own signature change might suggest.
- **Related specs:** `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` §15/§22/§29/
  §36 (Phase 2 first named this gap; Phases 3, 4, and 5 each re-confirmed it "remains unchanged and
  unaddressed by this phase" without fixing it) · `decisions/ADR-099-identity-native-sandbox-worktree-
  capability-model.md` §42 (the precedent this ADR follows: `SandboxRuntime::reset_to_turn()` closing
  the identical "call it in a tight loop for free" shape via a dedicated `AsyncQuota<ResetCost>`) ·
  `include/agentengine/core/ledger.hpp`'s own `commit()`/`branch_from()` (the lock-before-`co_await`
  restructure this ADR reuses verbatim, not reinvents).

## 1. The question

`Ledger::merge()` performs real object-store I/O on every call — three tree loads, one `put_tree`, up
to two ACL mutations, one snapshot persist — yet, unlike every other mutating verb on the class
(`commit()`→`AsyncQuota<StorageBytes>`, `branch_from()`→`AsyncQuota<BranchCost>`), it took no quota
parameter at all. First disclosed by an independent red-team pass during ADR-102 Phase 2's port (§15),
and re-confirmed as unaddressed by Phases 3 (§22), 4 (§29), and 5 (§36) without ever being fixed — a
real I8 (budgets enforced) gap that survived four separate phases of otherwise-thorough red-teaming.
Is the fix actually as simple as the disclosure's own "just mirror `commit()`/`branch_from()`'s
restructure" framing suggested, or does `merge()`'s own eight distinct early-return paths make it a
real trap for a mechanical port?

## 2. The fix

Added `struct MergeCost {}` (`ledger.hpp`, alongside the existing `BranchCost`/`StorageBytes` tags) —
a **dedicated** Kind, not a reuse of either sibling tag. `merge()`'s own cost shape is a fixed,
roughly-constant per-call expense, unlike `StorageBytes`' byte-proportional basis (`commit()` charges
`approx_bytes`, `merge()` always does the same bounded amount of work regardless of tree size beyond
what `merge_trees()` itself already bounds), and it is a conceptually distinct budget from "how many
branches may this identity create" even though both `BranchCost` and `MergeCost` happen to consume a
fixed `1` per call — matching this class's own established one-tag-per-verb convention (mirrored by
`sandbox_runtime.hpp`'s own `RunCost`/`ResetCost` tags for its own two gated verbs).

`merge()`'s signature gained `agentengine::rt::AsyncQuota<MergeCost>& quota`. Its body was restructured
to the exact shape `commit()`/`branch_from()` already established: `co_await quota.try_consume(1,
requested_by)` runs FIRST, before `mutex_` is ever taken; the entire existing body (all eight
early-return paths, every comment, every finding disclosure, unchanged) now runs inside a
non-coroutine, immediately-invoked lambda (`co_return` → `return` throughout) so the lock is never held
across a `co_await`; and every failure path — the lambda returning `!has_value()` — refunds exactly the
`1` unit consumed. This part of the restructure touches control flow only; no early-return path's own
logic, error code, or side effect (the `orphaned_from_restart_` registration, `child.resolved_`
handling) inside the lambda changed.

The quota-exhaustion path itself (`try_consume` failing, before the lambda is even entered) is NOT a
pure mechanical copy of `branch_from()`'s own shape, and needed its own real logic — see §4: `merge()`
takes `child` BY VALUE (unlike `branch_from()`'s `parent` parameter, taken by const reference), so a
quota-refused merge must explicitly register `child` as a reclaimable orphan (mirroring every other
rejection path inside the lambda) rather than silently falling through to `BranchHandle`'s own
destructor-triggered abandon path.

`SandboxRuntime::merge_into()` — the one place in the real tree that calls `Ledger::merge()` — gained
the matching `AsyncQuota<MergeCost>& merge_quota` parameter and threads it straight through.

## 3. Verification

`tests/test_ledger.cpp` updated and extended, not merely made-to-compile:

- Case [4] (clean merge, requester `child_identity`): now asserts `child_merge_quota.remaining()`
  drops by exactly 1 across the call — proving real consumption, not a parameter the method silently
  ignores.
- Case [5] (rejected conflict merge, requester `owner`): now asserts `owner_merge_quota.remaining()`
  is **unchanged** across the call — proving the refund path actually runs on a real failure, not just
  on paper.
- **New case [12]**: mints a fresh identity with a real `BranchCost`/`StorageBytes` quota (enough to
  legitimately create a branch and commit a clean, non-conflicting change) but an `AsyncQuota<MergeCost>`
  minted at `0`. The subsequent `merge()` call is asserted to fail with `async_quota.exhausted`, and the
  child branch is asserted to become a real, reclaimable orphan — `orphaned_branches()` contains it, and
  `reclaim_orphaned_branch()` genuinely succeeds for its own authorized owner, matching the same
  contract every other merge() rejection path already honors (case [5] proves the conflict-rejection
  shape of this identical contract). This case's own first version asserted the OPPOSITE (branch left
  "untouched", NOT orphaned) — see §4 for why that was wrong and how the case itself was corrected
  alongside the fix.

Full local verification (Windows/MSVC): `test_ledger` alone passes standalone; a full project rebuild
(`cmake --build . --config Debug`) completes with zero new errors; the full `ctest` suite reports
246/251 passing (98%), with the 5 failures independently confirmed to be pre-existing and
environment-caused, not caused by this change: `test_sandbox_runtime`, `test_mandatory_sandbox_provider`,
`test_docker_orphan_reap`, and `test_composed_sandbox_providers_live` all explicitly require a live
Docker daemon (each file's own header states this precondition; `docker ps` on this machine confirms
Docker Desktop is not currently running) and none of them exercise `merge()`/`merge_into()` at all
(grep-confirmed against `test_sandbox_runtime.cpp`, the one file among the four that even links
`sandbox_runtime.hpp`); `test_reference_agent_task_corpus` fails on an unrelated, pre-existing Python
worker/matplotlib environment gap. 2 tests are skipped, matching this environment's own existing,
unrelated posture.

## 4. Red-team round

A genuinely independent, fresh-agent adversarial pass (not this session's own self-review) found **one
real, MUST-FIX issue in this fix's own first version**, holding this whole design lineage's own
established pattern: every independent pass finds something real.

**MUST-FIX — the quota-exhaustion early return left `child` unresolved, silently scheduling the
branch's real ERASURE instead of leaving it reclaimable.** The first version of this fix wrote:

```cpp
auto consumed = co_await quota.try_consume(1, requested_by);
if (!consumed.has_value()) co_return std::unexpected(consumed.error());
```

— a direct, unmodified copy of `branch_from()`'s own quota-check shape. But `branch_from()`'s `parent`
parameter is taken by **const reference**; nothing is consumed or lost if it fails. `merge()`'s `child`
parameter is taken **by value**, and every one of its OTHER seven early-return paths (inside the
lambda) explicitly does `orphaned_from_restart_.insert(child.name()); child.resolved_ = true;` before
returning — because `BranchHandle::~BranchHandle()` calls `maybe_queue_abandon()`, which for any
still-`resolved_ == false` handle calls `owner_->queue_pending_abandon(name_)`. That is not a no-op:
the NEXT unrelated call to `reap_pending_abandons()` anywhere in the process would find this pending
name and genuinely call `abandon()` on it, **erasing the branch from `branches_` for real** — silently
destroying the caller's own real, committed work (the clean, non-conflicting commit the test's own
case [12] makes just before attempting the merge), not merely leaving it "untouched" as this ADR's own
first draft claimed, and not merely "orphaned but recoverable" as every other rejection path
guarantees. `tests/test_ledger.cpp`'s own first version of case [12] did not catch this: it checked
`orphaned_branches()`/`head_tree_digest()` immediately after the failed call and never invoked
`reap_pending_abandons()`, so the assertions it wrote (branch untouched, not orphaned) happened to be
true at the instant it checked, while the branch was already scheduled for destruction the moment
anything else in the process reaped pending abandons.

**Fixed same day**: the quota-exhaustion path now takes `mutex_` briefly (synchronously, no `co_await`
inside this scope, so the lock-across-suspension hazard this method's own header comment names does
not reopen), checks whether `child` still genuinely exists, registers it into
`orphaned_from_restart_` if so, and marks it `resolved_ = true` — the exact same contract every other
rejection path already honors. `test_ledger.cpp`'s case [12] was corrected to assert the CORRECT
behavior (the branch becomes a real, reclaimable orphan, verified by both `orphaned_branches()`
containing it AND a real `reclaim_orphaned_branch()` call succeeding) rather than the wrong one its
first version asserted. Re-verified: `test_ledger` still passes standalone, full project rebuild still
clean, full `ctest` still 246/251 with the same 5 pre-existing, unrelated failures named in §3.

**Checked and found clean by the red-team round:** the refund logic inside the lambda (every one of
the 8 internal early-return paths still refunds exactly what was consumed, unchanged by this fix); the
`commit()`/`branch_from()`-style lock-before-`co_await` restructure itself (no suspension point exists
between the lambda's `std::lock_guard` acquisition and release); `SandboxRuntime::merge_into()`'s own
signature threading (no equivalent by-value-parameter hazard there — it consumes `this` by rvalue, a
pre-existing, separately-disclosed concern per §6, not something this ADR's own change worsens).

## 5. What was NOT done

- **No Linux verification.** This change touches two Linux-and-Windows-shared headers
  (`ledger.hpp`/`sandbox_runtime.hpp`); only Windows/MSVC was exercised this pass.
- **`SandboxRuntime::merge_into()` remains uncalled by any real production code or test.** This ADR
  keeps it compiling and correctly threaded, but does not give it its own first real caller — that
  remains separate, pre-existing, unaddressed scope (ADR-102's own Phase 5 §47 already named the whole
  `ContainerdExecutionSurface`/parity backlog as "out of scope, named not dropped"; this is adjacent to
  but not identical to that item).

## 6. Residuals

- **Awaiting Linux re-verification**, per §5.
- **The blob-content-level ACL gap post-merge** (ADR-102 Phase 2 §15, reconfirmed unaddressed through
  Phase 5) is unrelated to and unchanged by this ADR — still real, still open.
- **`merge_into()`'s own pre-existing, disclosed concurrency hazard** (ADR-102 Phase 3 §22: `Ledger::
  merge()` takes `child` by value, so `std::move(branch_)` mutates `SandboxRuntime`'s own member before
  `exclusivity_` is locked in some callers' orderings) is untouched by this ADR — the quota parameter
  addition does not change `merge_into()`'s own lock-acquisition order or fix that hazard.
- **The Docker-daemon-dependent test failures named in §3 remain unresolved** in this environment (not
  this ADR's own scope to fix — a local Docker Desktop startup, not a code change, would close them).
