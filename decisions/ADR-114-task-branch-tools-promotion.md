# ADR-114 — Task-branch tools promoted to production: `SandboxRuntime::merge_into()` gets its first real caller

- **Status:** Proposed — implemented, verified against a REAL Docker daemon (Windows/MSVC), full
  project rebuild (zero errors) and full `ctest` (251/252 — the one failure is the same pre-existing,
  unrelated matplotlib/pandas Python-worker gap this branch's own ADR-111/112 verification already
  named), `naming_lint.py` clean. Not yet independently red-teamed by a fresh agent; not yet
  Linux-verified.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (new tools, new methods, new
  opt-in binding call, no change to `bind_sandbox()`'s existing signature), `tests/test_task_branch_tools.cpp`
  (new), `tests/CMakeLists.txt` (new target), `027-Vocabulary-and-Naming.md` (12 new rows). No other
  file touched — every existing caller of `bind_sandbox()`/`MandatorySandboxProvider` compiles and
  runs unchanged.
- **Related specs:** `docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp` (ADR-099 §7's A10,
  the prove-phase original this promotes); `SandboxRuntime::merge_into()`
  (`sandbox/sandbox_runtime.hpp`, ADR-111's `MergeCost`-gated form — previously disclosed with "zero
  real callers anywhere in the tree," ADR-111 §7); `docs/research/2026-08-27-real-world-agent-use-case-coverage.md`
  #1 (every actively-developed coding agent surveyed ships branch/worktree isolation as a
  tool-callable primitive — the gap this closes).

## 1. The question

`SandboxRuntime::merge_into()` has been real, `AsyncQuota<MergeCost>`-gated, and independently
red-teamed since ADR-111 — but nothing in `include/agentengine/` has ever called it. A9's
`MandatorySandboxProvider` gives sessions `run_command`; A10's own prove-phase design
(`TaskBranchSandbox`) already worked out start/run/commit/discard, three independent red-team
rounds, and a real capability-gating decision — but it was never promoted. Is promoting it a
mostly-mechanical port (matching ADR-106/108's own precedent for promoting prove-phase code), or
does it need new design work first?

## 2. Findings

Mechanical divergence from the prove-phase original was smaller than expected: `probe::Principal` →
`IdentityHandle`, `probe::error` → `agentengine::error{failure_class,...}`, and the rest of the usual
substitutions are the only renames. The one REAL, non-cosmetic gap: `merge_into()`'s signature grew
a third parameter (`AsyncQuota<MergeCost>&`) after the prove-phase file was last touched (ADR-111,
2026-08-29) — every call site needed that argument threaded through, not just a rename.

The genuinely open question was **where the four verbs should live and how they should be gated**:

- **Placement.** The prove-phase design specified a separate `TaskBranchSandbox<Surface>` object
  taking `SandboxRuntime const& main` plus copies of every quota `MandatorySandboxProvider` already
  owns. Building it as a second, sibling object would have meant duplicating `runtime_`/
  `branch_quota_`/`run_quota_`/`storage_quota_` pointers a second time for no real reason —
  `MandatorySandboxProvider<Surface>` already owns everything `TaskBranchSandbox`'s own header
  comment required its "main" parameter to be. **Decision: add the four verbs directly onto
  `MandatorySandboxProvider<Surface>`,** not as a separate class.
- **Capability gating.** The prove-phase design's real answer (`task_branch_capability.hpp`) was a
  two-tag `CapabilitySet` membership gate (`cap::decl::TaskBranch`/`cap::decl::TaskBranchCommit`,
  confirmed with the project owner) — but that tag pair was never promoted into the real, closed
  `agentengine::Capability` variant (19 alternatives, exhaustive switches throughout
  `core/capability*.hpp`). Widening that variant is real, security-critical surgery this design line
  has consistently declined to do speculatively (mirrors `MergeCost`'s own "a dedicated tag, not
  reused" discipline, just one level up in the type hierarchy). **Decision: mirror `RunCommandTool`'s
  own existing precedent instead** — zero static `Capabilities<...>` ceiling, authorized entirely
  through this design's `Grant<T>`/`AsyncQuota<T>`/`is_bound()` model. This is not a weaker bar than
  what's already shipped: `RunCommandTool` already grants a bound session unconditional, ungated
  main-branch-write authority on every single call (no isolation step at all); `commit_task_branch`
  requires an explicit prior `start_task_branch` plus a separate, explicit commit call before
  anything reaches main, making it arguably LESS consequential per call, not more. Widening the
  `Capability` variant remains real, disclosed follow-on work (§6), not a gap silently introduced
  here.
- **Opt-in surface.** `bind_sandbox()`'s own signature was deliberately left untouched — every
  existing caller (`tools/cli_chat.cpp`, `tools/sandboxed_shell_chat.cpp`,
  `tools/containerd_shell_chat.cpp`, four test files) compiles and runs unmodified. A new, separate
  `bind_task_branch_tools(AsyncQuota<MergeCost>&)` call is the only way the four tools are ever
  contributed by `on_context()` — mirroring ADR-070's Delegated Decision Seam discipline (explicit
  host opt-in, fails closed/absent when unset) a second time, on top of `bind_sandbox()`'s own
  already-established "a host that never calls this gets no execution capability" precedent.

## 3. What was built

Four new `Tool<>` conformers (`StartTaskBranchTool`/`RunInTaskBranchTool`/`CommitTaskBranchTool`/
`DiscardTaskBranchTool`) with their own JSON-schema-typed Args/Reply structs, matching
`RunCommandTool`'s exact shape (unreachable `invoke()` sentinel; real dispatch through
`make_tool_descriptor_with_invoke()` closures in `on_context()`, driven via the already-proven
`agentengine::rt::block_on<T>()`). Four new public methods on `MandatorySandboxProvider<Surface>`
(`start_task_branch`/`run_in_task_branch`/`commit_task_branch`/`discard_task_branch`), each failing
closed with `mandatory_sandbox_provider.task_branch_not_enabled` unless BOTH `bind_sandbox()` and
`bind_task_branch_tools()` have been called. New private state: `merge_quota_` (null until opted
in), `task_branches_` (a `std::map<std::string, SandboxRuntime>`), and `task_branch_mutex_` (its own
`AsyncMutex`, distinct from each child `SandboxRuntime`'s own internal `exclusivity_` — guards the
WHOLE body of every method that touches `task_branches_`, mirroring the prove-phase original's own
finding-1 fix for the identical hazard class). `commit_task_branch()` ports the prove-phase design's
own A10 fix verbatim: a rejected merge is immediately reclaimed via `SandboxRuntime::
reclaim_orphaned_child()` and re-surfaced under the SAME `handle_id`, so a real conflict never
strands the caller at the lower-level Ledger orphan-reclaim API.

The copy-assignment operator (`fork_from()`'s own mechanism) was extended to handle the new state
correctly: `merge_quota_` is carried forward on a successful fork (a SHARED resource reference,
exactly like `branch_quota_`/`run_quota_`/`storage_quota_` already are), while `task_branches_`/
`task_branch_mutex_` are NOT — a forked child starts with zero active task branches of its own (I2:
a fork shares AUTHORITY, never another instance's ACTIVE, in-flight state) and its own fresh mutex.

## 4. Verification

`tests/test_task_branch_tools.cpp` (new, 252 total project tests now, up from 251) exercises, against
a REAL Docker daemon:

- **The opt-in gate itself**: `bind_sandbox()` alone contributes exactly `run_command`; only after
  `bind_task_branch_tools()` do all 5 tools appear.
- **Start → run → commit**: a real child branch is forked, a real command runs on it, and committing
  genuinely merges the result into the PARENT's own branch — the parent reads a file it never wrote
  itself, independently of the tool's own reported reply. `MergeCost` is proven consumed by exactly 1
  unit on success; committing an already-committed handle fails `task_branch_unknown_handle`.
- **Best-of-N / conflict-reclaim**: two children forked from the SAME unmoved head; the first commit
  is a clean fast-forward, the second (now stale) is a genuine, real rejection — and the SAME
  `handle_id` stays usable afterward (re-surfaced via reclaim), proven by a further real
  `run_in_task_branch` call succeeding on it, not returning `unknown_handle`.
- **Discard**: refunds the `BranchCost` unit `start_task_branch` spent, and genuinely invalidates the
  handle afterward.
- **Fail-closed paths**: every verb on a never-issued handle fails `task_branch_unknown_handle`; every
  verb on a bound-but-not-`bind_task_branch_tools()`-enabled provider fails `task_branch_not_enabled`.
- **Driven through the REAL, unmodified `session.start_run() → invoke_tool()` pipeline** (matching
  `test_mandatory_sandbox_provider.cpp`'s own established rigor bar for this design line — a
  direct-accessor-only proof was exactly what earlier phases of this design got red-teamed for
  skipping): a `ScriptedChatClient` issues `start_task_branch`, `run_in_task_branch`, and
  `commit_task_branch` as three real tool calls across three `start_run()` turns; the committed file
  is independently read back off the parent's own Ledger branch afterward.

**A real bug was caught and fixed while writing this test, not after**: the local
`ScriptedChatClient` fixture (copied from `test_mandatory_sandbox_provider.cpp`, which only ever
drives ONE `start_run()` per session) never reset its own `call_count` between successive
`set_script()` calls. Driving three sequential script phases on the SAME session — needed to prove
start/run/commit as three separate real tool calls — silently misaligned which scripted message each
phase's internal chat-loop round-trips consumed, causing the pipeline-driven commit to run before the
scripted `run_in_task_branch` command had actually executed (a clean merge of unchanged content,
which still "succeeds," masking the real problem). Caught because the specific check that the
CONTENT the child wrote arrived on the parent's branch failed, not because a `co_await` or exit code
looked wrong. Fixed by resetting `call_count` inside the test fixture's own `set_script()`.

Also confirmed (git-stash-scoped): the one pre-existing `test_reference_agent_task_corpus` failure in
the full `ctest` run reproduces identically with this ADR's entire diff stashed out — unrelated,
pre-existing, matplotlib/pandas-in-this-environment, not touched by anything here. The MSVC `C1128`
(`number of sections exceeded`) failure encountered while probing `agentengine_cli_chat`'s build was
independently confirmed pre-existing the same way (identical failure on the stashed, unmodified tree)
— a known large-translation-unit MSVC limit unrelated to this change, not investigated further here.
`agentengine_sandboxed_shell_chat` and `test_composed_sandbox_providers_live` (the `AGENTENGINE_WITH_HTTPS`
build) both compile and pass clean against a real Docker daemon, confirming
`ComposedContextProvider<SandboxToolProvider, MandatorySandboxProvider<Surface>>` is unaffected — it
still contributes exactly 2 tools (never opting into `bind_task_branch_tools()`), proving the opt-in
gate holds in the composed setting too, not just standalone.

## 5. What was NOT done

- **No independent red-team pass yet.** This design line's own history (ADR-108, ADR-109, ADR-111's
  own MUST-FIX) shows every independent pass finds something real; this should not be assumed clean
  merely because the underlying primitives (`Ledger::merge()`, `SandboxRuntime::merge_into()`) were
  already proven separately.
- **No Linux verification.** Windows/MSVC only this pass.
- **The `Capability`-variant widening question (§2) remains open**, named explicitly as follow-on
  work rather than resolved or silently skipped.
- **Concurrency across MULTIPLE task branches on the SAME `MandatorySandboxProvider` instance is
  exercised sequentially, not under genuine concurrent dispatch** — `task_branch_mutex_` guarding the
  whole body of every method is the same discipline `TaskBranchSandbox`'s own header comment (finding
  1) established and this promotion inherits verbatim, but this pass did not add a dedicated
  concurrent-dispatch stress test the way ADR-102 Phase 4's own `block_on()` fix did for the
  quota-sharing case.
- **`active_`'s table has no durability of its own** — a process crash mid-task-branch strands it
  from this tool surface's own verbs even in a durable-`Store` `Ledger` configuration, though the
  underlying branch itself survives and remains reclaimable via the lower-level orphan-reclaim API.
  Inherited, disclosed, unchanged from the prove-phase original's own finding 6.
- **Whether `commit_task_branch`/`discard_task_branch` should touch an `AgentSession`'s own
  conversation/turn history is unaddressed** — inherited unchanged from the prove-phase original's
  own finding 5 (industry precedent, per that file's own research citation, treats file-restore and
  conversation-restore as independently selectable, not coupled by default).

## 6. Residuals

- Everything named in §5.
- This is now the SECOND real production caller pattern layered onto `MandatorySandboxProvider`
  (after `RunCommandTool`) — a third tool wanting a genuinely different authorization model (a real
  `CapabilitySet` ceiling, say) would need to resolve the same tension §2 named, not invent a third
  approach.
