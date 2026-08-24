# ADR-094 — `ExecRequest` mediation contract consolidated; `cap::Exec` investigated, deferred

Status: Proposed (Finding 1 implemented and verified; Finding 2 investigated, decided real but not
implemented this pass, awaiting project-owner sign-off — mirrors ADR-090/ADR-092's own
"investigated and deferred" precedent)

## 1. The question

`kata_backend.hpp`'s residual list carried "`ExecRequest::source` is... not yet Runner-mediated" as
the last un-investigated KataBackend gap. This ADR is that investigation, run to a real conclusion —
and it split into two genuinely separate findings, not one.

Full investigation, options considered, and an independent adversarial red-team pass (`Agent` tool)
that caught and corrected a real error in the first draft's analysis: `docs/planning/sandbox-exec-
request-capability-mediation-design-draft.md`. This ADR is the short-form record; the draft has the
full reasoning and citations.

## 2. Finding 1 — the residual bullet describes correct-by-design behavior, duplicated three times

Tracing the real call path (`tools/cli_chat.cpp`'s `real_execute_code()`) confirms `execute_code`'s
model-supplied `code` never reaches any `SandboxBackend::exec()` today, for any backend — it routes
through `MediatedPythonRunner`, a separate IPC-driven jailed worker process. The contract each of the
three backends' `exec()` already implements — trusting `ExecRequest` as fully-mediated input,
mediation being the caller's job, not the backend's, per `008-Sandbox-and-Isolation.md` §1b layer 2 —
is correct, settled architecture, not an unfinished wiring task. The actual defect: this one invariant
was stated three times, independently worded, in `kata_backend.hpp`, `linux_native_jail_backend.hpp`,
and `native_jail_backend.hpp` — a drift risk, the same shape `decisions/ADR-087-sandbox-spec-
capability-enforcement.md` already named ("no per-backend divergence") for a different field.

**Fixed this pass:** `008-Sandbox-and-Isolation.md` §2 (the backend-agnostic `SandboxBackend` contract
section — corrected from an initial draft that proposed §1b, which a red-team pass caught as the wrong
home; §1b is scoped to native-jail's own composed mediation stack, not backend-agnostic contract
language) gained a new paragraph, "`ExecRequest` mediation is the caller's responsibility, not the
backend's," stating the contract once, canonically. All three backend headers now point to it instead
of re-describing it, keeping their own genuinely backend-specific framing (Kata's residual-list
cross-references, native-jail's M2-scope notes) intact.

Zero runtime code changed — comment/spec text only. Verified: WSL build of both Linux targets
(`agentengine_kata_backend`, `agentengine_linux_native_jail_backend`) clean; Windows full build clean;
full `ctest` suite 247/247.

## 3. Finding 2 — `cap::Exec` is a real, unenforced gap; NOT implemented this pass

Investigating Finding 1 surfaced a second, genuinely separate question: is *any* capability required
to call `SandboxBackend::create()`/`exec()` at all? An early draft misread `cap::Exec`
(`include/agentengine/trust/capability.hpp:120-130`) as scoped to *nested* exec (008 §4's "`Exec`
(nested)" table row, unconditionally denied everywhere by construction) and concluded there was no
real gap. **The red-team pass caught this as wrong**, independently verifying
`007-Capability-and-Trust-Model.md:59`: `Exec<profile>` | "Create a sandbox of a profile" | profile,
resource limits — unambiguously a top-level, host-side gate on `create()` itself, distinct from the
008 §4 nested-exec row. `authorize_spec()` (`sandbox.hpp:210`) — the function ADR-087 built
specifically to make `SandboxSpec::capabilities` load-bearing — never reads `cap::Exec` grants at all;
no call site anywhere in this codebase checks whether a caller holds `Exec<profile>` before `create()`
runs. This is real: the exact shape of gap ADR-087 already fixed once for `mounts`/`net`, reproduced
one layer earlier, untouched by that fix.

**Deliberately not implemented this pass.** Extending `authorize_spec()` to check `cap::Exec` (reusing
ADR-087's own proven empty-skips-check, opt-in mechanism, rather than inventing a parallel one — the
"fresh design over patch" approach the project owner asked for mid-investigation) hit a real, unresolved
design question: `sandbox_backend_registry.hpp:151`'s `register_hardware_isolation_backend(std::string
name, ...)` shows backend identity in this codebase is a host-chosen registration string, not a fixed
enum — no `sandbox_profile` enum type exists anywhere in the tree, contradicting `capability.hpp`'s own
comment that `cap::Exec::profile_name` matches "`sandbox_profile`'s enumerators... by convention."
Whether `cap::Exec::profile_name` should be checked against backend-*type* (`"kata"`, `"native_jail"`)
or backend-registration-*name* (the registry's own caller-chosen string) is genuinely open, and
shipping a check against a guessed answer would be exactly the "decorative, not real" containment this
project's own posture rejects (the same judgment `decisions/ADR-088-...md` already applied once this
session, rejecting a speculative OOM heuristic with no verifiable signal).

## 4. Decision

- **Finding 1: closed.** The contract is real, correct, and now stated once. `kata_backend.hpp`'s
  residual bullet is retired.
- **Finding 2: real, disclosed, deferred — not closed, not implemented.** `cap::Exec` is a genuine,
  currently-unenforced I2 gap across all three `SandboxBackend` implementations. Implementing it
  needs a resolved answer to the backend-identity-granularity question above (design draft §7, items
  4-5) before it can be done as real enforcement rather than a guess.

## 5. What changed in this pass

- `008-Sandbox-and-Isolation.md` — new §2 paragraph (contract-level, backend-agnostic).
- `src/backends/kata/kata_backend.hpp` — residual bullet replaced with a pointer to 008 §2 and this
  ADR; the Finding-2 follow-on named explicitly, not silently dropped.
- `src/backends/native_jail/linux_native_jail_backend.hpp` — same consolidation for its own bullet.
- `src/backends/native_jail/native_jail_backend.hpp` — same consolidation for its own scope note.
- `docs/planning/sandbox-exec-request-capability-mediation-design-draft.md` — the full investigation,
  corrected in place after red-team (kept, not deleted, so the mid-course correction is part of the
  record rather than erased).
- No runtime code changed anywhere in this pass.

## 6. Residuals, carried forward explicitly

- **`cap::Exec` remains unenforced** (Finding 2, §3-4) — the one open item from this investigation,
  with its real open question named (design draft §7 items 4-5), not silently dropped. A genuine
  next-candidate follow-on for whoever picks it up.
- The other two previously-named KataBackend gaps are unchanged and unaffected by this pass: GPU
  passthrough (deliberately out of scope), and everything already tracked in ADR-088/090/091/092/093.
- This investigation is source/spec-level analysis, verified by an independent adversarial review
  (§1) that materially corrected one of its central claims — recorded here as evidence the process
  worked, not smoothed over.
