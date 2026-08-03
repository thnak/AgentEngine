# Decisions — the ADR record

An **ADR** here is not a memo. It is the durable record of a `design → red-team → prove → judge`
loop: competing designs implemented in real C++23, attacked adversarially, compiled under multiple
compilers, run under sanitizers, and measured — before a judge picks a winner on evidence.

This is the process Quark uses (36 ADRs and counting), and it exists because the alternative —
settling a hot-path or security-critical question by argument — produces designs that read well and
fail under load or attack.

## When an ADR is required

- Any **security-critical** choice: capability representation, isolation boundary, taint mechanism,
  approval binding, secret handling.
- Any **hot-path** choice measured by a 023 budget.
- Any choice where **credible designs disagree** and the disagreement is not resolvable by reading.
- Any **promotion** of an RFC from Draft to Proven — the gate execution *is* the ADR.

Ordinary implementation choices do not need one. An ADR for everything is an ADR for nothing.

## What an ADR must contain

1. **The question**, stated so that it has a wrong answer.
2. **The competing designs**, each steelmanned.
3. **Falsifiable claims** per design — specifically the fast/safe/correct claims, each paired with
   an experiment that would *disprove* it.
4. **The red-team attack** on each design, hardest where it claims to be fast or safe.
5. **The executed evidence**: commands, compilers, sanitizers, measured numbers (percentiles, not
   means), with **positive controls** — a test that cannot fail proves nothing, and for security
   claims this is mandatory (022 §5).
6. **Per-claim verdicts**: CORRECT / WRONG / INCONCLUSIVE, decided by observed output, not argument.
   `INCONCLUSIVE` is an honest verdict and must not be laundered into either of the others.
7. **The decision**, the specs it binds, and the residual risks or deferred gates.

## Naming

`ADR-NNN-<kebab-case-question>.md`, numbered sequentially and permanently. A superseded ADR is
marked Superseded and points forward; it is never deleted, because the reasoning that was wrong is
part of the record.

## Index

| ADR | Question | Outcome |
|---|---|---|
| [001](ADR-001-shellrunner-grammar-and-dispatch.md) | How does `ShellRunner` parse and dispatch without a name ever resolving to an arbitrary process, and with the parser itself safe against adversarial input? | **Judged.** Design A (recursive-descent → AST → tree-walking eval) accepted, scoped to Windows/MSVC+clang; Design B never implemented (not defeated on evidence). Partial evidence toward 010 §9 G2/G3/G4; G6 (cross-platform) and others remain open. |
| [002](ADR-002-pythonrunner-embedding-and-mediation.md) | How does embedded CPython enforce a closed import allowlist and mediate `open`/`socket`/`subprocess`, robustly rather than as a degrading blocklist? | **Judged.** Meta-path finder mechanism accepted; "closed by construction" narrowed after prove phase found granting numpy+pandas requires ~130 allowlisted names including `ctypes`/`winreg`/`subprocess` (module-name granularity can't distinguish trusted internals from guest code). See OpenQuestions.md for the caller-aware-gating follow-up. |
| [003](ADR-003-caller-aware-import-gating.md) | How does the import gate distinguish a granted package's own internal use of a sensitive module from guest code importing the same module directly (OQ-15)? | **Judged.** Host-side dual-registry + C-level frame-stack-walk mechanism accepted, narrowing ADR-002's scope-limitation for `ctypes`/`_ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess`. Not airtight (gadget-chaining and C-reentrancy questions remain open); one real entry-point gap (`importlib.import_module` on a cache miss) was found and fixed during independent re-verification, underscoring that this class of design has a demonstrated history of missed entry points. Resolves OQ-15 for the measured target. |
| [004](ADR-004-appcontainer-native-jail-windows-backend.md) | Does AppContainer + Job Object satisfy `native-jail`'s Windows contract across pure-Python, native-extension, and CodeAct-authored guest code? | **Spiked, not Judged.** AppContainer (zero capabilities + `CHILD_PROCESS_RESTRICTED`) confirmed correct for process/network denial and all three CodeAct library tiers against a vendored CPython 3.14. Headline finding: AppContainer's ACL model is **not** a sufficient filesystem boundary — `win.ini`/`hosts` remain readable via Windows' own inherited `ALL (RESTRICTED) APPLICATION PACKAGES` grants regardless of configuration, confirming 008 §1b's choice to make interpreter-level `open()` mediation primary. **§10 addendum (built and measured, same day):** `JobObjectLimits` (`src/backends/native_jail/job_object_limits.{hpp,cpp}`) — memory and active-process limits are precise and reliable; `cpu_ms`/`JOB_OBJECT_LIMIT_JOB_TIME` fired in only 3/11 measured runs (1.38x-8.22x overrun when it did) and must be treated as best-effort, with the host-side `wall_ms` watcher (confirmed reliable) as the real enforcement point. No independent red-team pass yet. |
| [005](ADR-005-capability-bearer-tokens-cross-process.md) | Should a capability that must cross a process boundary be a self-verifying macaroon-style bearer token, or an opaque reference checked against a host-side registry? | **Judged, narrowly.** Resolves OQ-3. Bearer token (`trust/capability_token.hpp`) accepted for `ExpiresAt`/`PathPrefix` caveats — proven attenuation-only and forge-resistant under red-team (bit-flip, field tamper, caveat-strip, reorder, fabricated-parent derivation all rejected; zero ASan findings, UBSan not attempted — no clang toolchain on this machine), no round-trip required by construction. Host-side registry (`trust/capability_registry.hpp`) not rejected — it is the answer for capabilities needing immediate revocation, a gap the token structurally has. Performance claim left **INCONCLUSIVE**: measured local cost favored the registry, confounded by an unoptimized (uncached) BCrypt handle in this pass and by the registry benchmark omitting the real cross-process round-trip Design A avoids by construction. |
| [006](ADR-006-agent-spawn-depth-budget-bound.md) | Are depth and budget bounds sufficient to contain `agent.spawn`'s recursion hazard, and how is the bound represented so guest code cannot widen it? | **Judged (partial — depth only).** Partially resolves 026 §9 Q1 / OQ-14. `SpawnBudget` (`trust/spawn_budget.hpp`) — a private-construction, strictly-decrementing in-process value type, not a cryptographic token (agent.spawn never crosses an OS process boundary) — proven exhaustive over 51 starting depths plus two compile-time `static_assert`s blocking out-of-class construction; zero ASan findings. Depth bounds confirmed sufficient against unbounded recursion, **conditional on** the effect-mediation boundary (006 §9 G4) holding, which this ADR assumes rather than re-proves. Cost bounds (wall-clock/token spend per spawn) remain open, tracked against 023. |
