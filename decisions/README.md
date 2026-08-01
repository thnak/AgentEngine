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
