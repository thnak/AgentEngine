# ADR-132 — `SandboxRuntime`/`MandatorySandboxProvider` become `Store`-generic, closing the content-durability integration gap

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors, 318 targets)
  and full `ctest` clean (292 total, 1 failure, pre-existing/environment, zero regression),
  `naming_lint.py` clean. NOT Judged — no independent red-team pass yet, no Linux verification yet.
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
- **No Linux verification yet.** Same established next-step pattern as every other ADR in this line.
- **`RunCommandTool`/task-branch tools are still only wired for the default `Store`.** Nothing in this ADR
  changes `tools/cli_chat.cpp`/`tools/sandboxed_shell_chat.cpp`/`tools/containerd_shell_chat.cpp` or any
  other real production caller to actually USE a non-default `Store` — this ADR proves the CAPABILITY
  exists and works correctly (§4's integration test), it does not wire any real host to opt into it. A
  real host wanting durable content still has to explicitly instantiate
  `MandatorySandboxProvider<Surface, FileWorktreeObjectStore>` itself and pass a
  `Ledger<FileWorktreeObjectStore>` to `bind_sandbox()`/`bind_root_branch()` — no new convenience wiring
  was added for that choice.
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
