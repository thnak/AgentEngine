# ADR-113 — `SandboxRuntime::run()`'s unconditional-commit-on-non-ok-klass gap is real but dormant; recorded as a promotion gate, not fixed

- **Status:** Proposed — analysis only, no production code changed. This ADR exists to convert a
  disclosed-but-unexamined residual into a properly scoped finding with an explicit promotion gate,
  not to ship a fix.
- **Date:** 2026-08-30.
- **Scope:** Read-only investigation of `include/agentengine/sandbox/sandbox_runtime.hpp`,
  `include/agentengine/sandbox/execution_surface.hpp`,
  `include/agentengine/sandbox/docker_execution_surface.hpp`,
  `include/agentengine/sandbox/containerd_execution_surface.hpp`, and
  `docs/planning/proofs/execution_surface/tree_backend_execution_surface_adapter.hpp`. No file
  touched.
- **Related specs:** ADR-101 (`ADR-101-sandbox-backend-tree-refinement-reconciliation.md`), whose own
  "what was NOT done" section named this exact behavior — "`SandboxRuntime`'s own
  unconditional-commit-on-non-ok-klass behavior (an already-shipped, unexamined gap this design's
  own positive control surfaced, not caused)" — and scoped it out. This ADR is the examination ADR-101
  deferred, not a new discovery.

## 1. The question

Does `SandboxRuntime::run()` (`sandbox_runtime.hpp:102-177`) genuinely commit a checkpoint regardless
of whether the underlying command's outcome was a policy violation, a timeout, an OOM kill, a sandbox
escape attempt, or a pending-approval state — and if so, is that reachable by anything shipped in
`include/agentengine/` today, or only by prove-phase code?

## 2. Findings

**The commit path itself has no gate.** `run()`'s step 4 treats any `result<SurfaceRunOutcome>`
*value* — regardless of what it contains — as "the command was genuinely attempted," and proceeds
unconditionally through step 5 (`drain_to`), step 6 (scan into a `Tree`), and step 7
(`ledger_->commit(...)`, `sandbox_runtime.hpp:167`). There is no branch anywhere in `run()` that
inspects the outcome's *kind* before deciding whether to persist what it produced. `Checkpoint`
itself (`core/ledger.hpp:202-209`) carries only `self_digest`/`tree`/`parent`/`authored_by_id`/
`turn_index` — no field of any kind records what the exec outcome actually was. Once committed, a
checkpoint produced by a crashed, timed-out, or escape-attempted run is indistinguishable in the
ledger from one produced by ordinary success; the only surviving trace is the caller's own
in-memory `SandboxRunOutcome::exec` copy, never persisted alongside the checkpoint it describes.

**But `SurfaceRunOutcome` — what production actually returns — cannot express a non-ok outcome at
all.** `SurfaceRunOutcome` (`execution_surface.hpp:38-41`) is `{int exit_code; std::string
stdout_text;}` — no klass field, no enum, nothing beyond a raw process exit code. Both real,
production `ExecutionSurface` conformers confirm this is exactly what they produce:
`DockerExecutionSurface::run()` (`docker_execution_surface.hpp:844-851`) returns
`docker_.exec(*instance_, ...)` directly — a raw `docker exec` exit code — and
`ContainerdExecutionSurface::run()` (`containerd_execution_surface.hpp:687`) does the equivalent
through `ctr tasks exec`. Neither backend has any concept of `timeout`/`oom`/`crash`/
`policy_violation`/`escape_attempt`/`ask_pending` in this vocabulary at all. Those klass values
belong to `agentengine::exec_outcome_class` — the separate, deliberately un-unified vocabulary
`SandboxBackend`/`ExecOutcome` use (`sandbox/sandbox.hpp`, 008 §2a) — which `ExecutionSurface`
explicitly does not share (`execution_surface.hpp`'s own file-top comment: a genuinely different
concept, kept distinctly named per 027 §1's discipline, "bridge explicitly if a bridge is ever
needed").

**The only place a non-ok klass could ever reach `SandboxRuntime::run()` is a bridge that is not
shipped.** `docs/planning/proofs/execution_surface/tree_backend_execution_surface_adapter.hpp`
(prove-phase only, never promoted into `include/agentengine/`) is that bridge: it wraps a
`TreeCapableSandboxBackend` (ADR-101, itself still Proposed/unjudged) as an `ExecutionSurface`
conformer, and its own `run()` (lines 184-221) documents exactly this problem by name — a first fix
attempt (return a `result<>` error for non-ok klass) was found, by ADR-101's own red-team, to
reopen the "run for free" quota-refund bug (`sandbox_runtime.hpp:150-153` treats any `result<>` error
from `surface.run()` as "nothing was attempted" and refunds `run_quota`). The adapter's corrected
fix collapses a non-ok klass into a *value* — `exit_code=-1`, the klass name folded as text into
`stdout_text` (lines 209-215) — specifically so `SandboxRuntime::run()` keeps the `RunCost` charge.
That correction closes the quota bug but does nothing about the commit path: once collapsed into a
value, that outcome flows through `run()`'s steps 5-7 identically to ordinary success, and gets
committed. The adapter's own comment calls this fidelity loss out for the exit-code case but does
not address the commit question at all — which is the gap this ADR examines.

**Net: real, but currently dormant.** No path in shipped `include/agentengine/` code can produce a
non-ok klass and hand it to `SandboxRuntime::run()` today — `TreeBackendExecutionSurface` is the only
thing that can, and it has never been promoted past `docs/planning/proofs/`. The gap cannot be
exercised by any live caller, any test, or any model-influenced input in the current tree.

## 3. Why this is not fixed here

Widening `SurfaceRunOutcome` with a klass field, or adding a commit-gating branch to
`SandboxRuntime::run()`, right now would be designing against a caller that does not exist —
`TreeBackendExecutionSurface` is not merely unwired, it is not even Judged (ADR-101 is Proposed).
Per this repo's own working conventions (CLAUDE.md: "Don't design for hypothetical future
requirements... Don't add features... beyond what the task requires"), speculative generality added
now would be exactly that: a field and a branch with no real conformer to validate against, decided
without the design→red-team→prove cycle CLAUDE.md requires for anything touching I4
(attributability) — what a checkpoint is allowed to mean is squarely that class of question.

This is also not a case where leaving it undone creates live exposure: the invariant that actually
matters today — "a `SandboxRuntime` checkpoint always reflects genuine successful command output" —
still holds, because no shipped conformer can produce anything else. The residual is a promotion
prerequisite, not an active defect.

## 4. Disposition: a promotion gate, not a deferred TODO

Recording this as a bare "not yet fixed" residual (as ADR-101 did) risks it being silently
rediscovered, or silently ignored, whenever `TreeBackendExecutionSurface` or any future
klass-carrying `ExecutionSurface` conformer is actually promoted. Instead:

**Any future promotion of `TreeBackendExecutionSurface` (or any other `ExecutionSurface` conformer
capable of producing a non-ok `exec_outcome_class`) into `include/agentengine/` MUST resolve this
gap as part of that same promotion ADR, not after it.** At minimum, that ADR must decide, with its
own red-team round:

- Whether `SurfaceRunOutcome` gains a real klass field (widening the concept) or whether the
  decision is made entirely inside the conformer/adapter without touching the shared vocabulary.
- Whether a non-ok-klass outcome should commit at all, commit with some marker, or bypass commit
  while still keeping the already-consumed `RunCost` charge (the adapter's existing "genuinely
  attempted, keep the charge" reasoning would need to be reconciled with "but don't silently persist
  it as ledger state" — these are not the same decision).
- Whether `Checkpoint` needs any field to make an exec-outcome-derived checkpoint distinguishable
  from an ordinary one after the fact (I4: a checkpoint that cannot be told apart from a normal one,
  once persisted, is not fully attributable to what actually produced it).

## 5. Residuals

- No code changed by this ADR. The gap ADR-101 named remains open, now with an explicit trigger
  condition (§4) instead of an implicit one.
- This analysis did not examine whether `ask_pending` specifically (a genuinely different case from
  `timeout`/`crash`/`policy_violation`/`escape_attempt` — it means "not yet resolved," not "resolved
  badly") should be modeled as a distinct sub-case of the same gate, or as its own separate design
  question; left for whichever future ADR actually triggers §4's gate.
- `SandboxRuntime::merge_into()` still has zero real production callers (ADR-111 §7) — a separate,
  already-disclosed residual, unrelated to this one beyond both living in the same file.
