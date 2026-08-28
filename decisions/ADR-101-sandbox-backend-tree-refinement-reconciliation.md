# ADR-101 — Does the real, production `SandboxBackend` concept need widening to carry tree-materialization verbs, or does a refined, additive concept — reconciled with `ExecutionSurface`/`SandboxRuntime` via a generic adapter — close the gap without touching the locked contract?

**Status:** Proposed — design and six independent red-team/verification rounds complete (2026-08-28),
all against real, compiled, live-Docker-run code, not reasoning alone. **No production code has been
written or merged from this design.** Every primitive lives under `docs/planning/proofs/
execution_surface/`, five new files, all untracked as of this writing. This ADR is a
**design-acceptance** record, matching this project's own established convention (ADR-099, ADR-100):
it does not authorize merging any of this work into `include/agentengine/` or `src/`.

**Relates to:** `decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md`
(`SandboxToolProvider`, the `CapabilitySet`-authorized precedent this design's own `cap::SandboxMount`
reuse is measured against), `decisions/ADR-098-default-sandbox-backend-registry-wiring.md`
(`build_default_sandbox_registry()`, the real registration pattern `register_hardware_isolation_
backend()` this design also uses), `decisions/ADR-099-identity-native-sandbox-worktree-capability-
model.md` (§7's A3 residual: "whether the three-verb `ExecutionSurface` shape generalizes past
Docker... unverified" — this ADR answers a version of that question, from the opposite direction),
`decisions/ADR-100-adr-096-098-sandbox-layering-reconciliation.md` (F4: "the registry's type erasure
is a deliberate boundary, not a gap to widen" — the precedent this design's own `SandboxBackend`
refinement, rather than widening, follows), `decisions/ADR-080-sandbox-backend-registry.md`
(`SandboxBackendRegistry` itself, unmodified by this ADR), `include/agentengine/sandbox/sandbox.hpp`
(`SandboxBackend`, `authorize_spec()` — both reused, neither modified), `docs/planning/proofs/
execution_surface/execution_surface.hpp`/`sandbox_runtime.hpp` (`ExecutionSurface`, `SandboxRuntime`
— both reused, neither modified), `docs/planning/proofs/execution_surface/docker_execution_surface.hpp`/
`containerd_execution_surface.hpp` (the two existing `ExecutionSurface` conformers this design
reconciles with, rather than replaces).

## 1. The question

**Stated so it has a wrong answer:** a project-owner conversation asked, concretely, whether Docker
support should be "gộp" (merged/unified) into the sandbox backend abstraction. Investigation found
`agentengine::SandboxBackend` (008 §2a — `create`/`exec`/`destroy`, handle-scoped, multi-instance) is
real, locked, production code with three real conformers (`NativeJailBackend`/`WasmBackend`/
`KataBackend`), while `docs/planning/proofs/execution_surface/`'s `ExecutionSurface` (`reset`/`run`/
`drain_to`, object-scoped, single-instance) is a separate, deliberately-fresh-built concept (ADR-099
§7 A3) with its own two real conformers (`DockerExecutionSurface`, `ContainerdExecutionSurface`) and
its own real consumer (`SandboxRuntime`, Ledger/`AsyncQuota`-integrated).

Does closing the "Docker isn't a `SandboxBackend`" gap mean **widening** the real, locked
`SandboxBackend` concept itself to also carry tree-materialization verbs (touching a contract three
shipped backends already depend on) — or does it mean a **refined**, additive concept that requires
`SandboxBackend` unchanged plus new verbs, and if the latter, does that just create a **third**,
uncoordinated "get tree content into/out of a sandbox" vocabulary alongside `SandboxBackend` and
`ExecutionSurface`, or can it be **reconciled** with the existing one?

## 2. The competing designs

### Design A (rejected, not attempted) — widen `SandboxBackend` itself

Add `reset`/`drain_to`-shaped requirements directly to the `SandboxBackend` concept (`sandbox.hpp`).

**Rejected because:** `SandboxBackend` is locked, production code with three real conformers
(`NativeJailBackend`/`WasmBackend`/`KataBackend`) that would all need new methods or the concept would
need to stay unsatisfied by them — either breaks existing, Judged-track code or fragments the concept
into "backends that support tree materialization" vs. "backends that don't," neither of which the
concept's own shape (008 §2a) was designed to express. Never seriously investigated past this framing;
the whole design line this project follows (ADR-100 F4: "a deliberate boundary, not a gap to widen")
already rejects this move for an analogous case (`create_python_worker()`/`exec_session()`).

### Design B (accepted) — `TreeCapableSandboxBackend`, additive refinement + generic reconciliation adapter

`docs/planning/proofs/execution_surface/sandbox_backend_tree_refinement.hpp`: a new concept
`TreeCapableSandboxBackend<T>` = `agentengine::SandboxBackend<T>` (real, unmodified) **+** three
additive, handle-scoped verbs — `reset(handle, host_dir, ctx)`, `drain_to(handle, host_dir, ctx)`,
`try_destroy(handle)` — each `EffectContext`-gated where relevant. One real conformer,
`DockerSandboxBackend`, wraps the already-proven `probe::DockerBackend`. Connected to the real,
production `SandboxBackendRegistry` (`probe_docker_sandbox_backend_registry.cpp`, via
`register_hardware_isolation_backend()`, `named_only`).

An architecture-fit red-team pass (§4 below) found this alone would create a **third**, uncoordinated
tree-materialization vocabulary alongside `SandboxBackend` (none) and `ExecutionSurface` (a different
shape). Closed by `docs/planning/proofs/execution_surface/tree_backend_execution_surface_adapter.hpp`:
a generic `TreeBackendExecutionSurface<Backend>` adapter that drives **any**
`TreeCapableSandboxBackend` conformer through the real, unmodified `ExecutionSurface` concept — by
lazily minting and re-minting exactly one `SandboxHandle` internally — making it, and therefore
`DockerSandboxBackend`, a real `ExecutionSurface` too, composable with `SandboxRuntime`'s real
`Ledger`/`AsyncQuota` integration with zero new code needed per future conformer.

**Steelman.** Touches nothing locked: `SandboxBackend`, `ExecutionSurface`, and `SandboxRuntime` are
all reused verbatim, none modified. Answers ADR-099 §7's own open question — whether `ExecutionSurface`
generalizes past Docker — from a new angle: not by adding a second bespoke conformer (already done,
`ContainerdExecutionSurface`), but by adding a **generic bridge** from a different concept family
entirely, proven to actually compose with the real consumer (`SandboxRuntime::run()`), not merely
type-check against the concept.

**Cost, stated honestly.** Two real, structural, disclosed trade-offs, neither smoothed over:
- **Exit-code fidelity loss.** `agentengine::ExecOutcome` (the real, locked type `SandboxBackend::
  exec()` must return) has no field for a raw process exit code; `probe::ExecOutcome` (what
  `ExecutionSurface::run()` must return) does. The adapter cannot losslessly bridge the two — for an
  ordinary (`klass==ok`) outcome it always reports `exit_code=0`; for a non-ok outcome (see C7 below)
  it reports the documented `exit_code=-1` sentinel with the real class folded into `stdout_text`. A
  caller needing real exit-code fidelity from a Docker-shaped surface must use `DockerExecutionSurface`
  directly, not this adapter — the two are not fully interchangeable.
- **`DockerExecutionSurface` and `DockerSandboxBackend` are not deduplicated.** Both independently wrap
  `probe::DockerBackend` with near-identical `reset()`/`drain_to()` bodies. This design unifies the
  CONSUMPTION path (both are now real `ExecutionSurface`s reachable through `SandboxRuntime`), not the
  two conformers' own implementations.

### Design C (not attempted) — a second, direct `ExecutionSurface` conformer for Docker via `SandboxBackend`-shaped code

Investigated only as a rejected alternative framing: writing a THIRD Docker wrapper conforming
directly to `ExecutionSurface`, ignoring `SandboxBackend` entirely, would answer the original
project-owner question ("should Docker be unified into the abstraction") more narrowly and would not
touch `SandboxBackend`/`SandboxBackendRegistry` at all — but it would not close the actual gap the
conversation surfaced (whether the real, production concept can be extended safely) and would produce
a *fourth* Docker-wrapping type in this tree, one more than Design B leaves. Not designed further.

## 3. Falsifiable claims (Design B)

- **C1 (refinement, not widening).** `TreeCapableSandboxBackend<T>` requires the real, unmodified
  `agentengine::SandboxBackend<T>` concept unchanged — no edit to `sandbox.hpp`'s concept definition.
  *Disproof: `sandbox.hpp`'s `SandboxBackend` concept changed as part of this design.* — **CORRECT**,
  `git diff` on `include/agentengine/sandbox/sandbox.hpp` across this whole design is empty.
- **C2 (tree-materialization verbs are capability-gated, not silently reachable).** `reset()`/
  `drain_to()` require a real `cap::SandboxMount` grant (via `authorize_tree_path()`, reusing
  `authorize_spec()`'s own `path_prefix_covers()`/`has_dot_or_dotdot_component()` logic) covering the
  requested host path, with correct read/write polarity, before touching the real filesystem/Docker.
  *Disproof: either verb succeeds against a real container with no covering grant, or a lexical `..`
  in the requested path defeats the prefix check.* — **CORRECT, after two real, found-and-fixed
  defects** (§4): the first version had NO capability gate at all (FATAL); the fix for that introduced
  a path-traversal bypass (a second FATAL), itself fixed and independently re-verified with 12
  adversarial path strings including deliberate false-positive checks (a directory literally named
  `foo..bar` must NOT be rejected).
- **C3 (`SandboxHandle` ownership is validated, not trusted from the caller).** Every verb
  (`exec`/`destroy`/`try_destroy`/`reset`/`drain_to`) fails closed on a `SandboxHandle` this
  `DockerSandboxBackend` instance never itself `create()`d. *Disproof: any verb succeeds against a
  forged or foreign handle.* — **CORRECT**, live-proven both calling `DockerSandboxBackend` directly
  and through the real `SandboxBackendRegistry`'s type-erased surface.
- **C4 (declared network policy is actually enforced, not merely authorized on paper).** `create()`
  rejects any `SandboxSpec::net` beyond `deny_all=true` with an empty allowlist, and the real container
  is started with `--network none`. *Disproof: a container created under this conformer has real
  outbound network access regardless of the requested policy.* — **CORRECT**, confirmed live: a `ping`
  from inside a `--network none` container reports "Network unreachable," and a positive control (a
  `SandboxSpec` requesting anything else) is rejected before any container exists.
- **C5 (`RegisteredSandboxBackend`'s type erasure does not carry tree-materialization).** A caller
  holding only what `SandboxBackendRegistry::resolve_named()`/`resolve_strict()` return
  (`RegisteredSandboxBackend const*`) has no `.reset()`/`.drain_to()` member — tree materialization is
  reachable only through a separately-held reference to the concrete conformer type. *Disproof: such a
  member exists or is reachable through the registry's own return type.* — **CORRECT**, proven as a
  compile-time fact (a concept-gated `static_assert`), matching ADR-100 F4's identical finding for
  `create_python_worker()`/`exec_session()`.
- **C6 (a `named_only`-registered conformer never wins `Strict` resolution, structurally).**
  `DockerSandboxBackend`, registered via `register_hardware_isolation_backend()`, is excluded from
  `resolve_strict()`'s candidate set regardless of its declared `strength` or what else is registered
  alongside it. *Disproof: a registry containing this entry (alone or with others) resolves `Strict`
  to it.* — **CORRECT**, both by direct trace of `SandboxBackendRegistry::resolve_strict()`'s real
  filtering (`strict_mode != eligible` is excluded before ranking, not merely outranked) and by a live
  test proving `resolve_strict()` fails closed (`no_strict_candidate`) against a registry holding only
  this one `named_only` entry.
- **C7 (a non-ok `exec()` outcome is charged, never refunded, through `SandboxRuntime`).** A
  `TreeCapableSandboxBackend` conformer's `exec()` returning a value with `klass != ok` (a genuine,
  resource-consuming attempt — timeout/oom/crash/policy_violation/escape_attempt/ask_pending, per this
  whole design line's own "a non-ok outcome is a normal result" convention) must not trigger
  `SandboxRuntime::run()`'s "nothing was attempted, refund `run_quota`" path. *Disproof: `run_quota` is
  refunded for a real, non-ok, genuinely-attempted outcome driven through the adapter.* — **CORRECT,
  after a real, found-and-fixed regression from this design's own first attempt at C8 below** (§4):
  the first fix for the klass-fidelity gap (C8) made `run()` return a `result<>` error for any non-ok
  klass, which `SandboxRuntime::run()` treats as "never attempted" and refunds — reopening the exact
  "run for free" bug class `RunCost` exists to prevent. Corrected to return a value instead (sentinel
  `exit_code=-1`), proven live with a synthetic `FakeCrashingBackend` conformer: `run_quota` confirmed
  consumed (97→96), not refunded, for a real `klass=crash` outcome.
- **C8 (a non-ok `klass` is never silently reported as fabricated success).** `TreeBackendExecutionSurface::
  run()` never returns `exit_code=0` for a `klass != ok` outcome. *Disproof: a real crash/timeout/oom/
  policy_violation/escape_attempt/ask_pending outcome is reported as `exit_code=0` through the
  adapter.* — **CORRECT, after a real, found-and-fixed defect**: the adapter's first version collapsed
  every `klass` into `exit_code=0` regardless of value — dormant only because `DockerSandboxBackend::
  exec()` today hardcodes `klass=ok` always; a future conformer that legitimately populates a non-ok
  `klass` would have hit this for real. Fixed (see C7 for the fix's own history — two attempts, the
  first itself defective).
- **C9 (a container is never orphaned on a transient cleanup failure).** `reset()`'s repeated
  destroy-then-create cycle (matching `DockerExecutionSurface::reset()`'s own "wipe fully every time"
  discipline) never loses the only reference to a possibly-still-running container. *Disproof: a
  transient `destroy()` failure causes the adapter to forget a handle it never confirmed cleaned up.*
  — **CORRECT, after a real, found-and-fixed defect**: `SandboxBackend::destroy()`'s real, locked
  signature returns `void` (008 §2), giving the original adapter no way to know whether cleanup
  succeeded — it cleared its handle unconditionally regardless, a real leak on every turn after the
  first. Fixed by adding `try_destroy()` (a checkable twin, additive to `TreeCapableSandboxBackend`,
  not changing `destroy()`'s own locked signature) and having the adapter keep its handle on confirmed
  failure, matching `DockerExecutionSurface`'s own established discipline exactly.

## 4. The red-team attack

Six independent rounds, 2026-08-28, all against real, compiled, live-Docker-run code (fresh agents
each round, not forks sharing context):

1. **Three parallel rounds on the initial `TreeCapableSandboxBackend`/`DockerSandboxBackend`**
   (security/I2-I3, C++ correctness, architecture-fit lenses), before the registry connection existed.
   **Security: verdict STRUCTURALLY WRONG** — FATAL: `reset()`/`drain_to()` had no capability input at
   all (C2's original gap); MUST-FIX: no handle-ownership validation (C3's original gap); MUST-FIX:
   `SandboxSpec::net` authorized on paper, never enforced against the real container (C4's original
   gap); SHOULD-FIX: `SandboxSpec::limits` never applied; SHOULD-FIX: `strength=0`'s "kept out of
   `resolve_strict()`" claim contradicted by the round's own registry test. **C++ correctness: verdict
   NEEDS FIXES** — MUST-FIX: throwing `std::filesystem` calls could leak an already-created container
   on an OS-level failure; SHOULD-FIX: `exec()`'s own comment misdescribed why exit-code fidelity was
   lost. **Architecture-fit: verdict NEEDS RECONCILIATION** — MUST-FIX: the design's "concrete answer"
   claim was validated against exactly one, copy-based conformer while ignoring
   `containerd_execution_surface.hpp` (a second, already-proven, live-bind-mount conformer in the same
   directory) that would have weakened it; SHOULD-FIX: created a third, uncoordinated
   tree-materialization vocabulary with no cross-reference to `ExecutionSurface`.
2. **Fix round 1** closes every finding above: `EffectContext`+`cap::SandboxMount` gate (C2),
   mutex-guarded `live_containers_` tracking (C3), `--network none` + net-policy rejection (C4),
   `--memory`/`--pids-limit` (partial I8 fix), `std::error_code` filesystem overloads, `named_only`
   registration (C6's mechanism), `exec()`'s `"cd /workspace && "` consistency fix, corrected comments.
3. **Verify round 1** on the fix, independent fresh agent. **Found a NEW FATAL**: the capability-gate
   fix (C2) itself had no lexical `.`/`..` guard — a grant for `/tmp/allowed` authorized a request for
   `/tmp/allowed/../../../etc/anywhere` (the literal string still matched the prefix). The original
   claim "no other bug found" for the other seven claimed fixes held on direct trace.
4. **Fix round 2** adds the `has_dot_or_dotdot_component()` guard (reusing `authorize_spec()`'s own
   helper, not inventing a new one) plus negative tests for read/write polarity and path traversal.
5. **Verify round 2**, independent fresh agent, including its own throwaway adversarial probe (12
   hostile/edge-case path strings, compiled and run for real): **CONVERGED** on the base refinement +
   registry connection. One SHOULD-FIX/NIT named (symlink-based escape is a lexical-check-only
   limitation, symmetric with `authorize_spec()`'s own identical, equally-undisclosed limitation) —
   disclosed, not fixed (out of scope for a lexical check).
6. **Reconciliation red-team round**, independent fresh agent, on the newly-built
   `tree_backend_execution_surface_adapter.hpp` (Design B's answer to round 1's architecture-fit
   finding). Found two real defects: **MUST-FIX** (C9's original gap — the void-returning `destroy()`
   made the adapter's `reset()` silently orphan a container on transient cleanup failure, not a
   one-off case since `SandboxRuntime::run()` calls `reset()` every turn); **MUST-FIX** (C8's original
   gap — `run()` collapsed every `klass` into fabricated `exit_code=0` success, dormant today only
   because `DockerSandboxBackend::exec()` hardcodes `klass=ok`).
7. **Fix round 3**: adds `try_destroy()` (checkable, required by `TreeCapableSandboxBackend`) with
   keep-on-failure discipline for C9; makes `run()` return a `result<>` error for non-ok `klass` for
   C8's FIRST attempted fix.
8. **Verify round 3**, independent fresh agent. C9's fix: **REAL and COMPLETE**, including confirming
   parity with `DockerExecutionSurface`'s own identical "stuck forever on a permanently-broken
   container" behavior in the worst case (not a regression). C8's fix: **REAL for its own stated goal,
   but opened a new, undisclosed regression** (C7's gap) — `SandboxRuntime::run()` refunds `run_quota`
   for any `surface.run()` error, so the "return an error for non-ok klass" fix silently reopened the
   exact quota-bypass "run for free" bug class `RunCost` was invented to prevent, for any future
   conformer that legitimately populates a non-ok `klass`.
9. **Fix round 4**: corrects the C8 fix to return a VALUE (`exit_code=-1` sentinel, real class folded
   into `stdout_text`) instead of an error, preserving quota-charging semantics. Adds a synthetic
   `FakeCrashingBackend` conformer (a minimal `TreeCapableSandboxBackend` whose `exec()` always returns
   `klass=crash`) as a real positive control, compiled and run against the real `SandboxRuntime::run()`:
   `run_quota` confirmed consumed (97→96), not refunded.
10. **Verify round 4**, independent fresh agent, tracing `SandboxRuntime::run()`'s real step sequence
    line by line to confirm the fix and its positive control are both sound (not merely "the numbers
    happened to match"). **Verdict: CONVERGED** on C7/C8/C9. One new MUST-FIX (a documentation-only
    defect: an earlier banner comment still described the FIRST, since-superseded fix for C8, directly
    contradicting the actual shipped code) plus two named, not-fixed residuals: `SandboxRuntime::run()`
    commits a real Ledger checkpoint even for a non-ok-`klass` outcome (a real, undecided design
    question belonging to `SandboxRuntime` itself, out of this design's scope); `exit_code=-1` already
    carries a different meaning in `DockerExecutionSurface`'s own existing code (`_popen` launch
    failure) — not currently exploitable (no fully-generic caller across both surfaces exists yet),
    named for whoever eventually writes one.
11. **Fix round 5** (documentation only): corrects the stale banner text to match the actually-shipped
    C7/C8 fix, updates a stale check-count reference, and writes down both named residuals explicitly.

**The pattern, stated as a residual, not smoothed over:** three separate times in this one design
(the path-traversal bypass, the quota-refund regression, the stale-documentation defect), the FIX for
one red-team-found defect itself introduced or left behind a new, real problem, caught only by a
SEPARATE, independent verification round — never by the same reasoning that produced the fix. This
mirrors `ADR-096` §8's own already-recorded standing caution for this general area ("self-directed,
'grep-confirmed' claims turn out wrong") and `ADR-100` §2's identical pattern, now observed a further
three times across this one ADR's own work.

## 5. Executed evidence

Every claim above is backed by a real MSVC compile and a real run against this session's live,
running Docker Desktop daemon (server 29.7.2) — not reasoning, not a mock. Cumulative check counts
across all five files' final state: `probe_sandbox_backend_tree_refinement.cpp` 29/29,
`probe_docker_sandbox_backend_registry.cpp` 14/14, `probe_tree_backend_execution_surface_adapter.cpp`
24/24 — 67 real, passing checks total, plus one independent verifier's own 12-case adversarial
path-traversal probe (compiled and run separately, not part of the checked-in tree). Build artifacts
(`.exe`/`.obj`/`.pdb`) were cleaned after every compile-and-run cycle; none are checked in. None of the
five files are wired into `CMakeLists.txt` — compiled by hand each round via
`cl /std:c++latest /EHsc /nologo /I <repo>/include /I ..`, matching this whole design line's own
"standalone probe, not a production build target" convention.

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — refinement, not widening | **CORRECT** | `sandbox.hpp`'s `SandboxBackend` concept unmodified across this whole design (§3). |
| C2 — tree verbs capability-gated | **CORRECT, after two real, found-and-fixed defects** | Round 1 (no gate at all), round 3 (path-traversal bypass in the fix) — both fixed, both independently re-verified live. |
| C3 — handle ownership validated | **CORRECT** | Round 1 finding, fixed round 2, live-proven both directly and through the registry. |
| C4 — network policy enforced | **CORRECT** | Round 1 finding, fixed round 2, confirmed live ("Network unreachable"). |
| C5 — registry type erasure excludes tree verbs | **CORRECT** | Compile-time `static_assert`, matching ADR-100 F4's precedent. |
| C6 — `named_only` structurally excluded from Strict | **CORRECT** | Traced against real `resolve_strict()` filtering; live test confirms fail-closed. |
| C7 — non-ok outcome charged, not refunded | **CORRECT, after a real regression from this design's own first fix of C8** | Round 6's finding, round 8's regression-of-the-fix finding, round 9's corrected fix, round 10's independent step-by-step trace + the `FakeCrashingBackend` positive control. |
| C8 — non-ok klass never reported as fake success | **CORRECT, after a real, found-and-fixed defect** | Round 6 finding, fixed (twice — see C7) round 9, verified round 10. |
| C9 — no container leak on transient cleanup failure | **CORRECT, after a real, found-and-fixed defect** | Round 6 finding, fixed round 7, verified round 8 (including worst-case parity with `DockerExecutionSurface`). |

## 7. The decision

**Design B is accepted as the answer to "should Docker (or any future technology) be unified into the
sandbox backend abstraction": refine `SandboxBackend` via an additive concept, never widen the locked
one, and reconcile the result with `ExecutionSurface`/`SandboxRuntime` via a generic adapter rather
than leaving a third, uncoordinated vocabulary.** This ADR **authorizes the design**, not an
implementation merge — no file under `include/agentengine/` or `src/` changes as a result of this ADR
by itself; `sandbox.hpp`, `sandbox_backend_registry.hpp`, `execution_surface.hpp`, and
`sandbox_runtime.hpp` are all reused exactly as they exist today.

**Binds, once a future implementation ADR actually wires this in:**
- `TreeCapableSandboxBackend<T>` = `SandboxBackend<T>` + `reset`/`drain_to`/`try_destroy`, the pattern
  any future technology (containerd, a hypothetical microVM-class backend under a NAMED_ONLY
  registration) should follow if it needs both real `SandboxBackend` conformance AND tree
  materialization — never by widening `SandboxBackend` itself.
- `authorize_tree_path()`'s reuse of `cap::SandboxMount`/`path_prefix_covers()`/
  `has_dot_or_dotdot_component()` as the authorization pattern for tree-materialization verbs — real,
  proven, not a new parallel scheme.
- `TreeBackendExecutionSurface<Backend>` as the generic bridge from any `TreeCapableSandboxBackend`
  conformer into `ExecutionSurface`/`SandboxRuntime` — reused, not re-derived, by any future conformer
  wanting both surfaces.
- The `exit_code=-1`-for-non-ok-klass / value-not-error convention (C7/C8) for any future
  `ExecutionSurface`-adapting code built on top of `SandboxBackend`-shaped outcomes.

**Explicitly out of scope — named, not silently dropped:**
- **No production wiring.** `SandboxBackendRegistry`'s real, production `build_default_sandbox_
  registry()` (`src/sandbox/default_sandbox_registry.cpp`, ADR-098) is untouched — `DockerSandboxBackend`
  is not registered there, and this ADR does not decide whether it ever should be.
- **`DockerExecutionSurface`/`DockerSandboxBackend` deduplication** — named as a real, disclosed cost
  (§2), not attempted here. A future pass could fold `DockerExecutionSurface` into a thin wrapper over
  `TreeBackendExecutionSurface<DockerSandboxBackend>`, or leave both — undecided.
- **`SandboxRuntime`'s unconditional-commit-on-non-ok-klass behavior** (§4 round 10's residual) — a
  real, undecided design question belonging to `SandboxRuntime` itself (whether a checkpoint should
  record the exec outcome/klass, whether `ask_pending` specifically should commit at all), out of this
  ADR's scope.
- **The `exit_code=-1` cross-conformer ambiguity** (`DockerExecutionSurface`'s "never launched" vs.
  this adapter's "genuinely ran, not representable") — named, not fixed; not currently exploitable
  (no fully-generic-over-`ExecutionSurface` caller exists yet).
- **A second `TreeCapableSandboxBackend` conformer** (e.g. containerd-shaped) — not attempted; C2/C7's
  own claims are proven against exactly one conformer, matching this whole design line's own
  "generic in interface, not yet empirically demonstrated to generalize" honesty standard
  (`sandbox_runtime.hpp`'s own disclosed limitation for `ExecutionSurface` itself, before
  `ContainerdExecutionSurface` existed).

## 8. Residual risks

- **This design has never been Judged**, and neither have `ADR-099`/`ADR-100`, the two most relevant
  already-shipped/already-Proposed pieces it builds on. Whoever judges this should weigh the pattern
  named in §4's own closing paragraph: three separate self-inflicted regressions across this one ADR's
  work, each caught only by a genuinely independent verification round, never by the same reasoning
  that produced the fix. The honest expectation, matching `ADR-099` §35.4's own closer for an
  analogous pattern, is that a further pass would likely find an eleventh-style issue somewhere in
  this tree too — implementation planning should budget for continued adversarial review as a standing
  cost, not a one-time gate already cleared.
- **`SandboxRuntime`'s unconditional-commit-on-non-ok-klass behavior** (§7) is unexamined by anything
  this design built — a real gap in the ALREADY-SHIPPED `SandboxRuntime` (predates this ADR), merely
  surfaced by this design's own `FakeCrashingBackend` positive control, not caused by it.
- **`Checkpoint`'s own shape has no field for the exec outcome/klass** — the only place a non-ok
  outcome's real classification survives is the transient `RunOutcome::exec` returned synchronously to
  the immediate `SandboxRuntime::run()` caller; nothing in the durable Ledger record distinguishes a
  checkpoint born from a real success vs. a real crash. Named, not fixed, not this ADR's own scope to
  fix (`SandboxRuntime`/`Ledger` are reused unmodified throughout).
- **The `strength=0`/`named_only` safety argument (C6) is proven against exactly the registries this
  design itself builds** (a registry holding only this one entry, alone or conceptually alongside
  others per the real `resolve_strict()` filtering logic) — it has not been build-verified against the
  REAL `build_default_sandbox_registry()` with `DockerSandboxBackend` actually registered into it,
  since this ADR explicitly does not do that (§7).
- **Windows-only verification.** Every real compile/run this ADR's evidence rests on happened on this
  session's Windows dev box against Docker Desktop. `DockerSandboxBackend`'s own `traits.platform_mask`
  claims Linux support too; that claim is untested by anything in this ADR.
