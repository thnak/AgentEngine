# ADR-100 — Does ADR-096 (`SandboxToolProvider`) actually conflict with ADR-098
(`build_default_sandbox_registry()`), and if not, what real gap does reconciling them surface?

**Status:** Proposed — design complete, three independent red-team rounds complete (2026-08-27); F3's
immediate mitigation is real, built, and proven the same day (`mediated_shell_dispatch.{hpp,cpp}`,
`mediated_shell_runner.hpp`, `tests/test_mediated_shell_runner_wall_clock_timeout.cpp`, 11/11 shell/
sandbox tests green, zero regressions), still awaiting `Judged` sign-off. Per explicit project-owner
direction: reconcile ADR-096 and ADR-098 before any further implementation in this space, including
`ADR-099`'s own A3/A9 follow-on work, which named this exact gap as a residual it deliberately left
open.

**Relates to:** `decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md`,
`decisions/ADR-098-default-sandbox-backend-registry-wiring.md`, `decisions/ADR-080-sandbox-backend-
registry.md` (§4/§7's "Finding O" — the fact this ADR turns out to hinge on), `decisions/ADR-081-
jailed-python-worker-process-slice-1.md` (`NativeJailBackend::create_python_worker()`/`exec_session()`,
the real precedent this ADR's finding is measured against), `decisions/ADR-030-session-scoped-codeact-
wiring.md` (`CodeActRunnerBinding`, explicitly untouched by this ADR), `decisions/ADR-099-identity-
native-sandbox-worktree-capability-model.md` §8 (named this reconciliation as an open residual). Full
record: `docs/planning/adr-096-098-sandbox-layering-reconciliation-design-draft.md` (two revisions —
the first contained a fatal factual error, corrected in the second after red-team; both kept, not
silently replaced, since the correction itself is the load-bearing finding).

## 1. The question

**Stated so it has a wrong answer:** `ADR-099` §8 lists `SandboxToolProvider` (ADR-096) and
`SandboxBackendRegistry` (ADR-080/098) as two of "three real, already-shipped, uncoordinated
mechanisms" occupying the same conceptual space, with no ADR reconciling them. Is that framing
accurate — do these two designs actually need a bridge built between them — or does reconciling them
mean discovering (and it does) that `ADR-080` already, correctly, decided no such bridge belongs
there, and that the real, un-surfaced gap is something else entirely?

## 2. What actually happened (told honestly, including the failure)

The first attempt at this reconciliation got the causal story backwards: it claimed Python already
uses `SandboxBackendRegistry` to reach `NativeJailBackend::create_python_worker()`'s real OS-level
jail, and that Shell should be given the same treatment by making `SandboxToolProvider` a registry
consumer. **Three independent, adversarial red-team rounds** (security/I2-I3, C++ correctness/scope,
architecture-fit) checked this against the real code before it became an ADR. The architecture-fit
round found it false, with exact citations: `MediatedPythonRunner` reaches `NativeJailBackend` via a
plain constructor-injected `NativeJailBackend&` (`mediated_python_runner.hpp:116,140`), never through
the registry — and `decisions/ADR-080-sandbox-backend-registry.md` §4/§7 already states, twice, as
"Finding O" (from `docs/planning/2026-08-22-component-role-audit-tracker.md`, written before
ADR-096/098 existed): *"real Python/Shell execution already bypasses `SandboxBackend::create/exec/
destroy` entirely via interpreter-level mediation."* The first draft re-derived a narrower, wrong
version of a question `ADR-080` had already correctly answered. This ADR is the corrected result, not
a polished version of the original claim — recording the failure here rather than smoothing it over,
matching this project's own established practice (`ADR-096` §4 records an identical pattern: three of
its own five red-team rounds found its self-directed reasoning wrong in exactly this "grep-confirmed
claim turns out false" shape).

## 3. Findings

- **F1 (ADR-080 already reconciled these; there is no bridge to build).** Neither Python
  (`MediatedPythonRunner`) nor Shell (`MediatedShellRunner`, what `SandboxToolProvider` wires up)
  routes through `SandboxBackendRegistry`. `ADR-080` §4/§7, `ADR-096`'s own README row, and `ADR-098`
  §2/§6 all already say this. Treating it as an open gap `SandboxToolProvider` should close by calling
  `resolve_strict()` would not close anything real — `RegisteredSandboxBackend` only type-erases
  `create`/`exec`/`destroy`, so the result would be a lookup whose value is unusable for
  session-persistent Shell state, the exact "selection without consumption" shape both ADR-080 and
  ADR-098 already name as a disclosed residual.
- **F2 (the real, corrected finding): Shell has zero OS-level containment; Python has one, reached
  by direct dependency, not by the registry.** `CommandRegistry::resolve()` is a provably closed
  lookup (no branch reaches `fork`/`exec`/`CreateProcess`) — Shell runs entirely in-process against a
  `MediatedFileSystemAdapter`, with no AppContainer, no Job Object, no resource limit. Python's own
  `NativeJailBackend::create_python_worker()`/`exec_session()` gives it a real AppContainer+Job-Object
  jail — via a direct `NativeJailBackend&`, the one real precedent in this codebase for how such a
  thing should be wired.
- **F3 (a live, present-day DoS gap, found independently by the security-lens round).**
  `mediated_shell_grammar.hpp` bounds source size/token count/nesting depth but has **no wall-clock or
  iteration cap on loop evaluation**. A model-supplied `run_shell` script with an unbounded loop runs
  synchronously with no kill mechanism — `RunShellTool`'s `cap::decl::FsRead`/`FsWrite` capabilities
  bound path scope only and do not offset this. Real, exploitable today via ordinary tool-call
  content, not a future concern.
- **F4 (the registry's type erasure is a deliberate boundary, not a gap to widen).**
  `create_python_worker`/`exec_session` are additive, non-concept methods by design (`native_jail_
  backend.hpp`'s own comment). No escape hatch (`std::any`/`variant`/`dynamic_cast`) exists anywhere
  in `RegisteredSandboxBackend`. A future OS-jailed Shell worker's correct integration point is a
  direct `NativeJailBackend&`, matching Python's real precedent — never the registry.
- **F5 (`ExecState.cwd` is shared vocabulary, not a ready-made seam).** The same field means a
  mediated virtual path in `MediatedShellRunner` and a real host-absolute path in
  `NativeJailBackend::exec_session()`. A future shell-worker design needs a real translation layer
  that does not leak the host root (I2/I3-relevant) — named as an open question, not solved here.

## 4. The decision

**ADR-096 and ADR-098 are not in conflict and need no bridge built between them.** `ADR-099` §8's
"uncoordinated mechanisms" framing is corrected, for this one relationship: `SandboxToolProvider`
(096) and `SandboxBackendRegistry` (080/098) were already coordinated by `ADR-080`'s own Finding O,
which both already cite. Neither ADR's own text, decisions, or code changes as a result of this ADR.

**Falsifiable claim, so this decision has a checkable test:** any future design routing Shell's
execution through `SandboxBackendRegistry`/`SandboxHandle` must first exhibit a real, non-test call
site anywhere in this tree that constructs a `SandboxHandle` from a registry-resolved backend and
uses it for persistent, `ExecState`-shaped session state. *Disproof: such a call site is found or
built.* None exists today (verified by grep and by `RegisteredSandboxBackend`'s own type-erasure
shape, F1/F4).

**What this pass is actually worth is not the (empty) bridge — it's F2/F3/F4/F5.** F3's immediate
mitigation is now built and proven (2026-08-27, same day as this ADR):

- **Built and proven: F3's live DoS gap is closed.** `evaluate_statement()`
  (`mediated_shell_dispatch.{hpp,cpp}`) now takes a `std::chrono::steady_clock::time_point deadline`,
  checked once per statement — the single funnel every statement passes through, top-level or
  nested-loop-body alike, so one check bounds the N^32-body-executions shape F3 names, not just a
  straight-line sequence. `MediatedShellRunner` gained a `wall_clock_budget` constructor parameter
  (default `kDefaultShellWallClockBudget` = 10s, the same "named, provisional stand-in" posture
  `output_discipline.hpp`'s `kDefaultOutputCapBytes` already documents), computing one real deadline
  per `run()` call. On expiry, `evaluate()`'s internal fail-fast propagation is intercepted at
  `MediatedShellRunner::run()` and translated into a real `ExecOutcome{klass:
  exec_outcome_class::timeout}` — a legitimate outcome, matching exactly how
  `NativeJailBackend`'s own watchdog classifies a real wall-clock kill, never a propagated error the
  caller has to specially handle. Proven by `tests/test_mediated_shell_runner_wall_clock_timeout.cpp`:
  a positive control (an ordinary bounded loop still completes normally, no false positive) and a
  real 20-level-nested, 3-items-per-level exponential-blowup script (3^20 ≈ 3.49 billion body
  executions, well under every existing parser bound) given a 20ms/50ms budget — both return a real
  `timeout` outcome within a bounded, measured real wall-clock time (asserted under 2s), never running
  anywhere close to completion. Full regression: the pre-existing shell/sandbox suite (11 tests
  spanning `test_shell_runner_proof`, `test_mediated_shell_runner_smoke`,
  `test_mediated_shell_runner_python_composition`, `test_sandbox_tool_provider`,
  `test_worktree_mount_sync`, and others) is 100% green, zero regressions.
- **Larger, still deferred, genuinely its own future design→red-team→prove→judge cycle:** a
  `create_shell_worker()`/`exec_shell_session()` additive pair on `NativeJailBackend`, mirroring
  `create_python_worker()`/`exec_session()`'s already-proven shape exactly, with `SandboxToolProvider`
  (or its replacement) taking a direct `NativeJailBackend&` — never the registry (F4) — and resolving
  `ExecState.cwd`'s virtual-vs-host-path translation (F5) as part of that design, not assuming it away.
  This closes Shell's remaining OS-level-containment gap (no AppContainer/Job Object at all); the
  wall-clock fix above closes only the specific unbounded-loop DoS shape, not that broader gap.

## 5. Explicitly out of scope

- Building the deferred `create_shell_worker()`/`exec_shell_session()` follow-on — named precisely
  (§4), not implemented; F3's immediate wall-clock mitigation IS built (§4).
- `CodeActRunnerBinding` (ADR-030) and the Python/Shell combined-provider question (ADR-096 §7) —
  untouched.
- `ADR-099`'s full three-way A9 residual — this ADR narrows it by exactly the 096↔098 relationship;
  the `CodeActRunnerBinding` and `Grant<T>`/`Ledger` relationships in that same residual are untouched.
- Re-verifying ADR-080's, ADR-096's, or ADR-098's own per-claim verdicts — all stand unchanged; this
  ADR corrects a new claim its own first draft introduced, not any claim in those three ADRs.

## 6. Residual risks

- **F3's specific unbounded-loop DoS gap is fixed and proven (§4); Shell's broader OS-level
  containment gap (no AppContainer/Job Object) is not.** A model-supplied `run_shell` script can no
  longer hang the calling thread indefinitely via nested-loop iteration, but it still runs with no
  memory/process/CPU-time OS-level ceiling — only the deferred `create_shell_worker()` closes that.
- **`kDefaultShellWallClockBudget` (10s) is an arbitrary, provisional default, not a tuned production
  value** — the same disclosed-not-final posture `output_discipline.hpp`'s `kDefaultOutputCapBytes`
  already carries for an identical reason (023's real per-turn budget doesn't exist yet).
- **This ADR's own first-draft failure (§2) is itself evidence worth weighing when this reaches
  `Judged`**: a plausible-sounding, specific technical claim ("Python already uses the registry") was
  wrong, survived one author's own reasoning, and was only caught by independent, adversarial
  red-team against the real code — the same pattern `ADR-096` §8 already names as this general area's
  standing track record, now repeated a sixth time across the two related design lines.
- **The two named follow-ons (§4) are not scheduled or committed to** — this ADR records what the
  next real engineering step would be if the project owner chooses to pursue it, not a roadmap
  commitment.
