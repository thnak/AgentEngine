# ADR-132 — `SandboxRuntime`/`MandatorySandboxProvider` become `Store`-generic, closing the content-durability integration gap

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors, 318 targets)
  and full `ctest` clean (292 total, 1 failure, pre-existing/environment, zero regression),
  `naming_lint.py` clean. SAME-DAY INDEPENDENT RED-TEAM (§7): clean bill of health, no defect found.
  **Linux-verified, ADR-133**: a complete rebuild (46 build steps, zero errors, zero warnings) and
  test pass on real GCC 14.2.0 — the whole tree compiled clean, confirming GCC agrees with MSVC on
  the injected-class-name/CTAD/method-template-deduction reasoning this templatization depends on,
  and `test_task_branch_content_durability_integration` passes completely on Linux too. Still NOT
  Judged.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/sandbox/sandbox_runtime.hpp`, `include/agentengine/sandbox/real_io_
  filesystem.hpp`, `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (all three widened, no
  method body logic changed), `tests/test_task_branch_content_durability_integration.cpp` (new),
  `tests/CMakeLists.txt` (one new target registered). No call site anywhere else in the repository needed
  any change.
- **Related specs:** Closes `decisions/ADR-130-content-durability-conformer.md` §2's own explicitly-named
  scope boundary: `SandboxRuntime`/`MandatorySandboxProvider` were hardcoded to `Ledger<>` (the default
  `InMemoryWorktreeObjectStore`), not `Store`-generic, so ADR-130's real, durable
  `FileWorktreeObjectStore` conformer could only be proven at the raw `Ledger<Store>` level, never through
  the production task-branch tool surface. This ADR is that follow-on.

## 1. The question

ADR-130 built and proved `agentengine::FileWorktreeObjectStore` — a real, durable `WorktreeObjectStore`
conformer — but found, while building it, that the production tool-surface layer this whole session's
task-branch/crash-recovery line (ADR-102/114/117/119/126/128) actually lives in could not use it at all:
`SandboxRuntime` and `MandatorySandboxProvider<Surface>` both name `agentengine::Ledger<>`/
`agentengine::BranchHandle<>` literally, not generically, even though `Ledger<Store>` has been a template
since ADR-102 Phase 2. Can this be closed — letting `MandatorySandboxProvider::commit_task_branch()`
genuinely succeed on a recovered handle with real, durable content — without disturbing any of the
already-verified behavior for every existing caller that only ever used the default, in-memory store?

## 2. The design: an additive second template parameter, not a breaking change

`SandboxRuntime` gained a template parameter, `Store = agentengine::InMemoryWorktreeObjectStore`.
`MandatorySandboxProvider<Surface>` gained a second, `Store = agentengine::InMemoryWorktreeObjectStore`.
`RealIoFileSystem`'s three `Ledger`-touching methods (`drain_into_tree()`, `scan_and_drain_into_tree()`,
`materialize()`) each became method templates over `Store` — `RealIoFileSystem` itself stays a
**non-template class** (its only `Store`-dependent surface is those three method parameters, and
`Store` is deduced from the `Ledger<Store>&` argument at each call site, needing no explicit
specification anywhere).

Every method BODY across all three files is completely unchanged — this is a pure widening of what
`Store` each class can be bound to, not a logic change. The design choice that made this genuinely
additive rather than a repo-wide breaking refactor: because `Store` DEFAULTS to
`InMemoryWorktreeObjectStore` everywhere, and because C++ lets a template be instantiated with fewer
explicit arguments than parameters when the trailing ones have defaults, **every existing caller that
only ever wrote `MandatorySandboxProvider<Surface>` (one argument) continues to compile and behave
identically, with zero call-site changes.**

## 3. What was actually touched, and why the blast radius was smaller than feared

A repo-wide search before starting found 20 files mentioning `SandboxRuntime`/`MandatorySandboxProvider`
by name. Checking each individually (not assumed) found **19 of them are comment-only** — doc references,
not actual type usages — and the ONE real type-usage site outside the three files this ADR touches
(`tests/test_sandbox_runtime.cpp:93`: `SandboxRuntime runtime(ledger, std::move(*root_r), staging);`)
needed **zero changes**, because it is a local-variable declaration with a matching-constructor
initializer — exactly the shape C++17 class template argument deduction (CTAD) handles automatically,
deducing `Store` from the `ledger` argument's own type with no deduction guide required. Confirmed, not
assumed: this file compiled and its own dedicated real-Docker test passed unchanged on the first build
attempt after the templatization.

Inside the three touched files, C++'s injected-class-name rule did the rest of the work for free: every
place `SandboxRuntime` referred to itself from WITHIN its own class-template body (return types like
`agentengine::result<SandboxRuntime>`, the `merge_into(SandboxRuntime const& parent, ...)` parameter,
constructor calls like `SandboxRuntime(*ledger_, ...)`) needed no change at all — the injected name
already denotes the current specialization. The only sites that genuinely needed editing were: the class
declaration itself, the constructor's own parameter types, the two private members (`Ledger<Store>*
ledger_`, `BranchHandle<Store> branch_`), and — in `MandatorySandboxProvider` — the `runtime()` accessor's
return type and the `std::map<std::string, SandboxRuntime<Store>> task_branches_`/`std::optional<
SandboxRuntime<Store>> runtime_` members, since `SandboxRuntime` is a *different* class template there,
not the injected name.

## 4. What was built and verified

**The full-stack integration proof** (`tests/test_task_branch_content_durability_integration.cpp`, new)
— the definitive test this whole session's task-branch crash-recovery line has been building toward:

- **[1]** Real content is committed to a durable `Ledger<FileWorktreeObjectStore>`'s root branch via the
  raw `Ledger` API, before `MandatorySandboxProvider` ever touches it.
- **[2]** `MandatorySandboxProvider<FakeSurface, FileWorktreeObjectStore>` binds to that SAME root
  (`bind_sandbox()`) and starts a real task branch (`start_task_branch()`) through the real tool surface.
- **[3]** Everything goes out of scope without merging — the simulated crash, the same "destroy +
  reconstruct" methodology this session's own `test_task_branch_durability_recovery.cpp` (ADR-126)
  established.
- **[4]** A fresh `Ledger<FileWorktreeObjectStore>` is reconstructed. `MandatorySandboxProvider::
  bind_root_branch()` (ADR-128) reclaims the root by identity alone — and its own internal call into
  `bind_sandbox()`'s `recover_orphaned_task_branches()` (ADR-126) automatically rehydrates the orphaned
  child task branch too — **all three of this session's own crash-recovery mechanisms composing
  together for the first time**, through the real, unmodified production API, with no manual
  `Ledger`-level call anywhere in the test.
- **[5] THE CORE CLAIM**: `commit_task_branch()` on the ORIGINAL `handle_id` **succeeds** — a real
  three-way merge, genuinely reloading the root's own real content from real disk — not `ledger.
  merge_tree_load_failed`, the exact, precise failure `test_task_branch_durability_recovery.cpp`
  correctly asserts for its own, deliberately-in-memory-store case.
- **[6]** The real content committed in [1] is confirmed still readable, byte-exact, through the
  ACL-gated production `get_blob_safe()` path after the full crash-recovery-and-merge cycle.

**Mandatory sanity check**: temporarily swapped `FileWorktreeObjectStore` for `InMemoryWorktreeObjectStore`
throughout the test (both the raw-`Ledger` setup and the `Provider` alias), rebuilt, and confirmed checks
[5]/[6] genuinely FAIL with the exact original disclosed error (`ledger.merge_tree_load_failed`) rather
than silently passing — then restored and reconfirmed a full pass.

**Full project verification**: rebuild (`cmake --build . --config Debug`, 318 targets): zero errors, zero
new warnings on any of the three touched files. Full `ctest`: **292 total (287 baseline-before-this-
session's-content-work + 5 new tests across ADR-130/132), 1 failure** — the same, already-established
`test_reference_agent_task_corpus` pandas/matplotlib environment gap, zero regression anywhere else,
including every other real-Docker test that constructs a `SandboxRuntime`/`MandatorySandboxProvider`
(`test_sandbox_runtime`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`,
`test_composed_sandbox_providers_live`, `test_mandatory_sandbox_provider_composed`, and every ADR-119/
124/126/128 test in this design line) — all pass unchanged, confirming the widening genuinely changed
nothing for the default, already-verified case. `python tools/naming_lint.py`: clean, 361 suppressed
findings (unchanged — no new exported type, `Store` is a template parameter, not a type registered in
027's own vocabulary tables).

## 5. What was NOT done

- **No independent red-team pass yet.** This touches the two most heavily-verified files in this entire
  session's design line (`sandbox_runtime.hpp`/`mandatory_sandbox_provider.hpp`) — a same-day adversarial
  round is the expected next step, matching this design line's own unbroken track record (ADR-102/114/
  117/119/126/128 each got one).
- ~~No Linux verification yet.~~ **Closed by ADR-133** — a complete rebuild and test pass on real
  GCC 14.2.0: the whole tree compiled clean (zero errors, zero warnings) confirming GCC agrees with
  MSVC on the injected-class-name/CTAD/method-template-deduction reasoning this templatization
  depends on, and `test_task_branch_content_durability_integration` (along with every other
  Docker-independent test in this design line) passes completely on Linux too.
- ~~`RunCommandTool`/task-branch tools are still only wired for the default `Store`.~~ **Closed by
  ADR-134** — `tools/durable_sandboxed_shell_chat.cpp` is a real, user-reachable host that explicitly
  instantiates `MandatorySandboxProvider<DockerExecutionSurface, FileWorktreeObjectStore>` and verified,
  across two genuinely separate real process invocations, that `bind_root_branch()` reattaches to durable
  state correctly. `tools/cli_chat.cpp`/`tools/sandboxed_shell_chat.cpp`/`tools/containerd_shell_chat.cpp`
  remain unchanged, deliberately ephemeral tools — this closes the residual by adding a new host, not by
  changing those three.
- **The `FileWorktreeObjectStore`↔`durable_dir` configuration-consistency residual named in ADR-130 §6
  remains unchanged** — nothing here forces or checks that a host configures `Ledger`'s own `durable_dir`
  and the store's own root directory consistently.

## 6. Residuals

- Everything named in §5.
- `RealIoFileSystem` staying a non-template class while its three `Ledger`-touching methods became
  method templates is a deliberate asymmetry, not an oversight — it minimizes the change's own surface
  area (no new class-level template parameter propagating to every OTHER method/member on that class,
  none of which touch `Ledger` at all) while still achieving full `Store`-genericity for the methods that
  need it. Worth naming explicitly so a future reader does not assume it was missed.
- ~~No Linux verification yet (unchanged by §7 — that round was Windows/MSVC only, same as this ADR's
  own original pass).~~ **Closed by ADR-133** — see §5.

## 7. Independent red-team round (same day)

An independent adversarial pass, briefed with zero prior context beyond this ADR and told explicitly to
treat this as the highest-stakes review in the session's design line (the two touched files are, by
this ADR's own account, the two most heavily-verified files in the entire line). Read the full diff
(`git show 28fc845`) directly rather than trusting this ADR's own account, then went after five specific
risk claims:

- **Backward compatibility, not just "it compiled."** Re-ran, from a real, just-rebuilt binary (not
  reasoned about), the four real-Docker tests that construct `SandboxRuntime`/`MandatorySandboxProvider`
  against a genuine Docker daemon: `test_sandbox_runtime`, `test_mandatory_sandbox_provider`,
  `test_task_branch_tools`, `test_composed_sandbox_providers_live`. All four pass, unchanged output,
  confirming zero behavioral difference for the default, 1-template-argument case this ADR claims is
  untouched.
- **The injected-class-name reasoning (§3), verified as correct C++, not a compiler-specific accident.**
  Wrote a genuine negative-compile probe (`SandboxRuntime<InMemoryWorktreeObjectStore>::merge_into()`
  called with a `SandboxRuntime<FileWorktreeObjectStore> const&` parent) and built it through the real
  project build system (a temporary CMake target, removed after use). MSVC correctly rejected it with
  C2664 — `merge_into()`'s unqualified `SandboxRuntime` parameter resolves, via the injected class name,
  to the SAME specialization as `this`, exactly as §3 claims, and this is standard C++ behavior (not an
  MSVC-specific accident) since the injected class name inside a class template's own body always denotes
  the current instantiation, per the standard's own class-template-declaration rules — nothing here relies
  on implementation-defined behavior.
- **The `RealIoFileSystem` method-template design.** Traced `SandboxRuntime<Store>::run()`'s calls into
  `io_fs_.materialize()`/`io_fs_.scan_and_drain_into_tree()` (both take `Ledger<Store>&`, deduced from
  `*ledger_`, itself `Ledger<Store>*` where `Store` is the ENCLOSING class template's own parameter) — no
  ambiguity, no cross-`Store` mixing possible: the method template's own `Store` parameter name shadows
  the class template's `Store` only lexically, deduction still binds it to whatever concrete type
  `*ledger_`'s type actually is. Confirmed by the full rebuild succeeding with zero errors and the probe
  above failing exactly where a mismatch was deliberately introduced (proving deduction isn't silently
  papering over a real type error).
- **I2/I3 — no widened authority.** Line-by-line diffed `bind_sandbox()`/`bind_root_branch()`/
  `commit_task_branch()`/`start_task_branch()`/`discard_task_branch()` bodies against the pre-ADR-132
  version (`git show 28fc845`): every change is a parameter/member TYPE (`Ledger<>` → `Ledger<Store>`,
  `SandboxRuntime` → `SandboxRuntime<Store>`), zero logic/control-flow lines touched, zero new code paths
  that could bind a caller to content it does not already own via the SAME `Store` instance it was
  constructed against.
- **The new integration test's own claim.** Re-ran `test_task_branch_content_durability_integration`
  (ALL CHECKS PASSED, real Docker-independent `FakeSurface`, real disk). Independently reproduced the
  ADR's own documented sanity check — not merely trusted its account — by editing a scratch copy back to
  `InMemoryWorktreeObjectStore` throughout (type alias, both `Ledger<>` template arguments, both
  constructor calls corrected to `InMemoryWorktreeObjectStore{}`'s own actual default-constructible
  signature), rebuilding, and confirming checks [5]/[6] genuinely FAIL with the exact original disclosed
  error (`ledger.merge_tree_load_failed`, `merge could not load base/ours/theirs from the object store`)
  rather than silently passing — then restored the original file, rebuilt, and reconfirmed a full pass.

**Full re-verification after the round** (no production code changed — this round found nothing to fix):
full rebuild (`cmake --build . --config Debug`, zero errors), full `ctest` — 292 total, 1 failure
(`test_reference_agent_task_corpus`, the same pre-existing, disclosed pandas/matplotlib environment gap),
zero regression anywhere else — and `python tools/naming_lint.py` clean (361 suppressed findings,
unchanged). The temporary negative-compile probe target and the temporary sanity-check edit to the
integration test were both fully reverted; the working tree carries no artifact from this round.

**Verdict: clean bill of health.** Every one of this ADR's own central claims (purely additive,
zero-call-site-change widening; the injected-class-name reasoning; the method-template deduction
correctness; the new integration test's own genuine proof) held up under independent, executed
verification — including the two claims (injected-class-name correctness, the sanity-check reproduction)
this round specifically declined to take on the ADR's word alone. No real, fixable defect found — this
section documents a genuine attempt, not a rubber stamp.
