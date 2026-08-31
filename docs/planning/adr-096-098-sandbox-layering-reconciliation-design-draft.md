# Reconciling ADR-096 (`SandboxToolProvider`) and ADR-098 (`build_default_sandbox_registry()`)

Prompted by explicit project-owner direction: reconcile these two before any further implementation
in this space, including the identity-native design's own A3/A9 (`ADR-099`). ADR-099 §8 named this
exact gap as a residual risk ("two independent, unjudged, security-critical sandbox-lifecycle
designs... no ADR yet reconciling them") but scoped it out of its own pass.

**Revision note (this is Revision 2, after a fatal correction).** Revision 1 of this document
claimed Shell's lack of OS-level containment was explained by "Python routes through the fix
(`NativeJailBackend::create_python_worker()`) that Shell hasn't gotten yet, via the generic shape
`SandboxBackendRegistry` provides" — and proposed, as future work, making `SandboxToolProvider` "the
real registry consumer ADR-098 has been missing." **Three independent red-team passes checked this
against the real code and against `ADR-080`'s own text; the architecture-fit pass found the central
claim false**: `MediatedPythonRunner` reaches `NativeJailBackend::create_python_worker()`/
`exec_session()` via a direct, constructor-injected `NativeJailBackend&` (`mediated_python_runner.hpp:116,140`,
`.cpp:20,72,90`) — **zero references to `SandboxBackendRegistry`/`resolve_strict`/`resolve_named`
anywhere in that file.** Python never touches the registry either. Worse: `decisions/ADR-080-
sandbox-backend-registry.md` §4/§7 already states this, twice, as "Finding O" from `docs/planning/
2026-08-22-component-role-audit-tracker.md`, written before ADR-096/098 existed: *"real Python/Shell
execution already bypasses `SandboxBackend::create/exec/destroy` entirely via interpreter-level
mediation."* Revision 1 re-derived a narrower, wrong version of a question ADR-080 had already
correctly answered. This revision corrects that, rather than softening it.

## 0. Method

Read the real, current code for every type either ADR names: `SandboxToolProvider`
(`sandbox_tool_provider.hpp`), `SessionShellSandbox`/`MediatedShellRunner` (`session_shell_wiring.hpp`,
`mediated_shell_runner.hpp`, `mediated_shell_dispatch.cpp`, `mediated_command_registry.hpp`,
`mediated_shell_grammar.hpp`), `SandboxBackendRegistry`/`RegisteredSandboxBackend`
(`sandbox_backend_registry.hpp`), `build_default_sandbox_registry()`
(`src/sandbox/default_sandbox_registry.{hpp,cpp}`), `NativeJailBackend`
(`native_jail_backend.{hpp,cpp}`), `MediatedPythonRunner` (`mediated_python_runner.{hpp,cpp}`), and
the shared `Runner`/`ExecState` vocabulary (`sandbox/runner.hpp`) — plus `decisions/ADR-080-sandbox-
backend-registry.md` and `decisions/ADR-030-session-scoped-codeact-wiring.md` in full, not just this
document's own paraphrase of them. Three independent, adversarial red-team rounds (security/I2-I3,
C++ correctness/scope, architecture-fit) checked Revision 1 against all of this before any of it
became an ADR.

## 1. What the real code actually shows (corrected)

**Neither Python nor Shell routes through `SandboxBackendRegistry`, and this was never in
question — `ADR-080` settled it before either ADR-096 or ADR-098 existed.** `ADR-080` §4/§7,
verbatim: real Python/Shell execution bypasses `SandboxBackend::create/exec/destroy` entirely via
interpreter-level mediation (Finding O). `ADR-096`'s own README row already states this correctly
("a real connection from `SandboxBackendRegistry`'s resolved backend to `SandboxToolProvider`'s
construction... building one is not recommended speculatively") and `ADR-098`'s own §2/§6 states it
again ("no production caller... `SandboxBackendRegistry`'s resolved backend does not connect to
`SandboxToolProvider`'s construction"). **There is no unreconciled gap here to bridge — this was
already reconciled, correctly, by ADR-080, and both ADR-096 and ADR-098 already cite it.** Treating
"the registry has no consumer" as news, or as something `SandboxToolProvider` should fix by calling
`resolve_strict()`, would not close anything real — it would re-litigate an already-settled, already-
disclosed, already-correctly-scoped decision.

**The real, previously-imprecise finding is narrower: Shell has zero OS-level containment,
Python has one — reached by direct dependency, not by the registry.** `MediatedPythonRunner` holds
a constructor-injected `NativeJailBackend&` and calls `create_python_worker()`/`exec_session()`
directly. `MediatedShellRunner` (what `SandboxToolProvider` actually wires up) never does anything
of the kind: `CommandRegistry::resolve()` is a provably closed lookup with no branch that can reach
`fork`/`exec`/`CreateProcess` (the type's own header comment states this directly, carried forward
from `ADR-001` §2.2) — Shell runs a parsed command tree purely in-process against a
`MediatedFileSystemAdapter`, with no AppContainer, no Job Object, no resource limit, no wall-clock
kill. This is the real asymmetry: not "Python is registry-routed and Shell isn't" (false — neither
is), but **"Python has an OS-level jail via a direct concrete dependency, and Shell has none at
all."**

**A live, present-day, unmitigated consequence of that asymmetry, found independently by the
security-lens red-team round, not merely a future concern:** `mediated_shell_grammar.hpp` bounds
source size (1 MiB), token count (50k), and nesting depth (32), but has **no wall-clock or iteration
cap on the `for_stmt` construct's evaluation itself**. A model-supplied `RunShellArgs.source`
(`run_shell`'s argument is model-influenced tool-call content, I3-relevant) containing a loop over a
large word-list, or repeated reads over mount-legal-but-large paths, runs synchronously with no kill
mechanism — Python's own jailed worker gets a real watchdog (`idle_cpu_budget_ms`, `init_timeout_ms`);
Shell gets none. `RunShellTool`'s `cap::decl::FsRead<"work">`/`FsWrite<"work">` capabilities bound
path scope only — they say nothing about, and cannot bound, CPU/time consumption, so their presence
does not offset this gap.

**`RegisteredSandboxBackend`'s type erasure cannot carry `create_python_worker`/`exec_session`
regardless — and this is a deliberate scope boundary, not an oversight to fix.**
`native_jail_backend.hpp`'s own comment states `exec_session` is "intentionally absent from [the
`SandboxBackend` concept's] `requires` clause, matching the final spec §4's 'additive non-concept
method' statement." `register_backend<B>()` only ever type-erases `create`/`exec`/`destroy` at its
template-instantiation boundary — confirmed by grep: no `std::any`/`std::variant`/`dynamic_cast`/
second registration path exists anywhere in the tree. Session/worker-persistent execution surfaces
were designed to be reached by concrete type, exactly as `MediatedPythonRunner` already does — never
through the registry's type-erased interface. **A future OS-jailed Shell worker's correct
integration point, matching the one real precedent this codebase has, is a direct
`NativeJailBackend&` dependency on `SandboxToolProvider` (or its replacement) — never
`SandboxBackendRegistry`.**

**`ExecState`'s shared vocabulary type is real but not a ready-made seam — its `cwd` field means
two different things in the two consumers.** `mediated_shell_dispatch.cpp`: `state.cwd` holds a
mediated, virtual path relative to the sandbox mount. `native_jail_backend.cpp`: `inst.cwd` is a real
host-absolute path passed to Win32 `CreateProcess` as `lpCurrentDirectory`. A future shell-worker
design would need a real virtual-to-host-path translation layer that does not leak the real host
root — itself I2/I3-relevant, not just an engineering inconvenience — named here as an open question,
not asserted as a solved seam.

## 2. The actual question, stated so it has a wrong answer

Given that ADR-080 already, correctly decided "neither Python nor Shell routes through the registry,
and that's fine" — is there anything left for ADR-096 and ADR-098 to reconcile at all, or does this
whole exercise risk producing a well-written document that changes zero real constraints on future
work, for a pair of ADRs that were never actually in tension?

## 3. The decision

**There is nothing to bridge between ADR-096 and ADR-098 themselves — they already correctly cite
ADR-080 and correctly decline to build a connection ADR-080 already named as out of scope.**
Formally recorded here, closing `ADR-099` §8's framing of these two specifically as "uncoordinated":
they are coordinated, by `ADR-080`'s own already-settled Finding O, which both already cite. No code
changes as a result of this section.

**What this reconciliation pass is actually worth is not a bridge between 096 and 098 — it's the
sharper, corrected finding in §1: Shell has zero OS-level containment, has a live unmitigated
wall-clock/iteration DoS gap in its mediated-interpreter loop, and the one real architectural
precedent for fixing it (`MediatedPythonRunner`'s direct `NativeJailBackend&` dependency) points away
from the registry, not toward it.** Two concrete, differently-scoped follow-ons:

- **Immediate, cheap, in-process mitigation — built and proven same day (2026-08-27).**
  `evaluate_statement()` (`mediated_shell_dispatch.{hpp,cpp}`) now checks a real
  `std::chrono::steady_clock::time_point deadline` once per statement — the one funnel every
  statement, top-level or nested-loop-body, passes through — closing the DoS gap named in §1.
  `MediatedShellRunner` gained a `wall_clock_budget` constructor parameter (default
  `kDefaultShellWallClockBudget` = 10s, provisional, mirroring `output_discipline.hpp`'s own
  disclosed-not-final posture); on expiry the internal fail-fast error is translated into a real
  `ExecOutcome{klass: exec_outcome_class::timeout}`, matching how `NativeJailBackend`'s own watchdog
  classifies a wall-clock kill. Proven by `tests/test_mediated_shell_runner_wall_clock_timeout.cpp`: a
  positive control (an ordinary bounded loop still completes normally) plus a real 20-level-nested,
  3-items-per-level script (3^20 ≈ 3.49 billion body executions, well under every parser bound) given
  a 20ms/50ms budget — both return a real `timeout` outcome within a measured, bounded real wall-clock
  time (asserted under 2s). Full regression: 11 pre-existing shell/sandbox tests 100% green.
- **Larger, still deferred, its own future design→red-team→prove→judge cycle:** a
  `create_shell_worker()`/`exec_shell_session()` additive pair on `NativeJailBackend`, mirroring
  `create_python_worker()`/`exec_session()`'s already-proven shape, with `SandboxToolProvider` (or its
  replacement) taking a direct `NativeJailBackend&` — the real precedent, not the registry. Must
  resolve the `ExecState.cwd` virtual-vs-host-path translation (§1) as part of that design, not assume
  it away. This is what would close Shell's remaining OS-level-containment gap (no AppContainer/Job
  Object at all) — the wall-clock fix above closes only the specific unbounded-loop DoS shape.

## 4. What this reconciliation binds, once accepted

- `SandboxToolProvider` (ADR-096) and `SandboxBackendRegistry`/`build_default_sandbox_registry()`
  (ADR-098) remain exactly as scoped in their own text — this document changes neither.
- **Falsifiable claim, so this decision has a checkable test, not just prose**: any future design
  routing Shell's execution through `SandboxBackendRegistry`/`SandboxHandle` (rather than a direct
  concrete-backend dependency) must first show a real, non-test call site that constructs a
  `SandboxHandle` from a registry-resolved backend and uses it for persistent, `ExecState`-shaped
  session state — since no such call site exists anywhere in this tree today (verified by grep,
  `RegisteredSandboxBackend`'s own type-erasure shape, §1). *Disproof: such a call site is found or
  built, showing the registry's type-erased `create`/`exec`/`destroy` trio is in fact sufficient for
  session-persistent execution.*
- `ADR-099`'s own A9 residual (§8, "three real, already-shipped, uncoordinated mechanisms") is
  narrowed by exactly one relationship: `SandboxToolProvider` (096) and `SandboxBackendRegistry`
  (080/098) were never actually uncoordinated — `ADR-080` already coordinated them, correctly, before
  ADR-099 was written. The other two named relationships in that residual (`CodeActRunnerBinding` vs.
  `SandboxToolProvider`; either of these vs. `Grant<T>`/`Ledger`'s own eventual A9 integration) are
  untouched by this document.
- The concrete next piece of engineering work this space actually needs, if pursued, is the two
  items in §3 — not a registry-consumption patch.

## 5. What this does NOT establish

- Does not build `create_shell_worker()`/`exec_shell_session()` — named future work only (§3). The
  wall-clock mitigation IS built (§3).
- Does not resolve `ExecState.cwd`'s virtual-vs-host-path semantic gap (§1) — a real open question
  for whoever picks up the deferred future work, not decided here.
- Does not touch `CodeActRunnerBinding` (ADR-030) or the Python/Shell combined-provider question
  ADR-096 §7 already named as separate future work.
- Does not re-verify any of ADR-096's, ADR-098's, or ADR-080's own per-claim verdicts — all three
  stand as originally verified; this document corrects a new claim Revision 1 introduced, not any
  claim in the three existing ADRs.
- The built wall-clock mitigation is not risk-free or complete — it closes specifically the
  unbounded-loop DoS shape named in §1, not Shell's OS-level containment gap as a whole (no
  AppContainer/Job Object), which only the deferred worker design would close.
  `kDefaultShellWallClockBudget` (10s) is an arbitrary, provisional default, not a tuned production
  value.
