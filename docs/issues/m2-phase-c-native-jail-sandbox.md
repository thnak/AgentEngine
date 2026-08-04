M2 Phase C — `native-jail` sandbox (008), Windows + Linux

## Context

Milestone 2 exit criterion (`docs/planning/v1-implementation-roadmap.md`): *"...`native-jail`
sandbox parity (008 §9 G1) holds on Windows and Linux..."* Phases A (capability enforcement,
ADR-009) and B (the 006 §3 tool pipeline) are done — see
[`docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`](../planning/milestone-2-tools-capabilities-sandbox-breakdown.md)
for their full "(done)" writeups. Phase C is next in the roadmap's own build order (capability
enforcement → tool pipeline → `native-jail` → WASM plugin host).

**M2's `native-jail` proof target is the raw `SandboxBackend` contract via minimal built-in probe
programs, not the Python interpreter** (breakdown decision 3): 010 (Python Code Interpreter and
Shell) isn't scheduled until M3, and ADR-002/003's import-gating mechanism is specifically about
mediating CPython's `import` — mixing that into M2 would pull a whole RFC's scope forward early.
`PythonRunner`/`ShellRunner` become real `Runner`s plugging into this same `SandboxBackend` in M3,
unchanged.

**A pre-existing, unrelated finding surfaced during Phase B's Linux verification pass**:
`tests/test_real_filesystem_adapter.cpp`'s case-fold-consistency check (ADR-001-era, M1) assumes a
case-insensitive filesystem and a Windows junction (`cmd /c mklink /J`) and fails on Linux — not a
regression from Phase A/B, but apparently never previously run against a real Linux filesystem.
Worth resolving as part of (or alongside) this phase's cross-platform parity work.

## Tasks

- **C1.** `SandboxBackend` contract completed — `ProfileTraits`, `MountSpec`'s source field — still
  synchronous (`ae::task<T>` stays deferred for M2, no gate item this milestone needs real coroutine
  concurrency to prove). **Size: S**
- **C2.** `native-jail` backend written fresh (not extending the ADR-001/002/004 prove-phase spike):
  `create`/`exec`/`destroy`, `ResourceLimits` enforcement, carrying forward ADR-004's *findings*
  (wall_ms as the dependable bound, `cpu_ms` best-effort only, documented as such — not the code) on
  Windows; namespaces + seccomp-BPF + cgroups v2 on Linux. **Size: XL**
- **C3.** Minimal probe binaries proving the 008 §7 abuse-case subset that needs no interpreter
  (fork bomb, OOM, infinite loop → `wall_ms` kill, fs-escape attempt, unbounded output) — 008 §9 G2
  scoped to what's buildable without 010. **Size: L**
- **C4.** Cross-platform parity proof (008 §9 G1) — the same probe corpus on Windows and Linux
  (Docker, the established M0/M1/M2 verification pattern), same outcome classification. Named
  directly in the roadmap's exit criterion, not optional. **Size: L**
- **C5.** No-ambient-authority probe (008 §9 G3) specifically against `native-jail`. **Size: M**
- **C6.** Teardown-cycle proof (008 §9 G4) — scoped down from the RFC's full 10⁵ cycles to a
  machine-safe bounded count (CLAUDE.md's build/test resource caps apply), rationale documented,
  same pattern as M1 deferring 001's 10⁴-session gate. **Size: M**

## What's explicitly deferred past this phase (see breakdown doc's full list)

- 008's full G4 (10⁵ cycles — C6 uses a bounded count instead), G5 (cold-start vs. 023 budgets —
  023 itself stays `TBD-baselined` until M8), G6 (downgrade visibility), G8 (snapshot fidelity,
  `wasm`-only, needs Phase D built out further than M2's minimal host).
- ADR-003's residual, still-open risks (gadget-chaining variant, fail-closed C-reentrancy,
  `sys.path`-shadowing precondition) — inherited into M3 when `PythonRunner` is actually built.
- The `remote` sandbox profile entirely (M9).

## Exit criteria

- `SandboxBackend` contract implemented and exercised (not just declared) on both platforms.
- Probe corpus proves the abuse-case subset in scope, with matching outcome classification on
  Windows and Linux (Docker) — the roadmap's own named exit-criterion sentence.
- No-ambient-authority and bounded teardown-cycle claims proven, not asserted.
- Full test suite (existing + new) green on Windows and Linux/gcc-14, following this project's
  established build+test+Docker-verification-per-phase discipline.
