# ADR-030 — Session-scoped CodeAct wiring

**Status:** Proposed (2026-08-11). Designed, red-teamed, implemented, and proven (real code +
deterministic tests, §5); awaiting the project owner's explicit "Judged" sign-off per this
project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project
owner is the ADR judge).

**Relates to:** `decisions/ADR-028-session-scoped-stateful-tools.md` §6 ("Real CodeAct wiring...
is separate, later work — this ADR proves the general mechanism only"), the residual this ADR
closes; `decisions/ADR-002-pythonrunner-embedding-and-mediation.md` §5.5.6 Finding 7.8, whose "one
OS process per session" rule this ADR enforces in code for the first time rather than leaving it as
prose.

## 1. The question

`tools/cli_chat.cpp` wired `execute_code`/`mount_skill` (CodeAct) through FIVE independent
function-local `static` variables in an anonymous namespace — not session members at all: a
`MediatedPythonRunner`, an `ExecState`, a `MountedSkillsState`, a pending-mount-roots vector, and a
THIRD independent `SkillsProvider<>` instance. This works today only because this CLI happens to run
exactly one session per process — nothing in the code enforces that, and if it ever ran two, they
would silently share all five, including genuinely per-conversation state (mounted skills, shell-like
cwd/env).

**Stated so it has a wrong answer:** can ADR-028's `make_tool_descriptor_with_invoke` mechanism give
CodeAct real per-session state, and if so, exactly which of the five pieces of state can safely
become per-session, and which cannot?

## 2. The design considered and rejected

**First draft:** give each `AgentSession`'s provider its own, separately-owned
`MediatedPythonRunner` instance — the literal reading of "port CodeAct onto ADR-028's per-instance
ownership model."

**Rejected by red-team, fatally, on two independent grounds** (an agent dispatch that read
`src/backends/native_jail/mediated_python_runner.{hpp,cpp}` and
`decisions/ADR-002-pythonrunner-embedding-and-mediation.md` directly, not from a summary):

- **Finding 1 (fatal):** `MediatedPythonRunner::run()`/`refresh_agent_tools()` route through
  unsynchronized, process-wide file-scope globals in `mediated_python_runner.cpp` —
  `g_current_ctx` (read by every mediated `open()`/`socket()` wrapper), `config_.tool_bridge`
  (mutated in place by `refresh_agent_tools()`), `g_effective_keep_set`. Nothing serializes calls
  into these globals across TWO DIFFERENT `AgentSession` actor instances (`quark::Sequential` only
  serializes handlers *within* one actor). Two sessions each owning their own runner and calling it
  from their own actor's worker thread is a real cross-session confused-deputy hazard, not just a
  crash: session B's `refresh_agent_tools()` overwriting `config_.tool_bridge` while session A's
  Python code is still running mid-call could route A's next `agent.tools.foo(...)` call against
  B's bridged tools and B's `EffectContext` (B's capabilities).
- **Finding 2 (fatal to the specific claim, not the mechanism):** cwd/env/the work-mount directory
  are real OS-process-global resources (`SetCurrentDirectoryW`/`SetEnvironmentVariableW`,
  `mediated_python_runner.cpp`'s `sync_state_into_process`), and the work mount is a single fixed
  physical directory. A genuinely per-session `ExecState` cannot isolate two sessions through the
  SAME interpreter no matter how it's owned — CPython's classic embedding API only supports one
  `Py_InitializeFromConfig` per process, ever (confirmed directly, not assumed).

Also found and folded into the accepted design: (3) a testability mismatch between "concrete runner
pointer" and "swap in a fake for tests" (resolved by making the CLAIM mechanism generic, not the
whole provider); (4) a `main()`-local runner would be a real dangling-pointer risk against Quark's
actor-drain lifetime, versus the pre-existing static's process-lifetime safety; (5)/(6)/(7)
confirmed clean (no capability-check bypass; the `Backgroundable`/`captures_session_state` guard is
inert for these two tools since neither declares `Backgroundable`; consolidating the file's THREE
independent `SkillsProvider<>` instances down to one is behaviorally a no-op — all three already
resolved the identical `demo_skill_sources()`); (8) reconfiguration after
`AgentSession::clear_in_process_state()` is a non-issue on inspection — that method is the in-process
half of session DELETE (005 §6), not a mid-life reconfiguration point, so a deleted session's
provider ending up unconfigured matches deletion semantics rather than being a real gap; (9) `fork_from()`
copies `MountedSkillsState`/`ExecState` in-memory bookkeeping but not the underlying on-disk "work"
directory — named, not fixed (matching ADR-028 §6's own precedent for this exact class of residual).

## 3. The accepted design

**Split by what's actually safe to make per-session, not by what ADR-028's mechanism makes
*possible*:**

- `MediatedPythonRunner` stays a genuinely process-wide singleton (a lazy function-local static,
  unchanged lifetime from before this ADR) — reached through a NEW primitive,
  `CodeActRunnerBinding<RunnerT>` (`include/agentengine/core/codeact_runner_binding.hpp`), which
  wraps a non-owning `RunnerT&` plus a claim slot: the FIRST session to call `bind(session_id)`
  succeeds (idempotently, for repeat calls with the same id); any DIFFERENT session_id's `bind()`
  fails closed, mutating nothing. This makes ADR-002 §5.5.6's "one interpreter per process" rule
  literally impossible to violate — not merely commented — closing red-team finding #1 structurally:
  since at most one session is EVER bound, `g_current_ctx`/`config_.tool_bridge` can never see a
  second session's writes.
- `MountedSkillsState` and `ExecState` become real, ordinary per-instance members of
  `ToolDeclaringHistoryProvider` (`tools/cli_chat.cpp`) — genuinely safe now, precisely BECAUSE the
  claim mechanism above guarantees at most one session is ever live against the shared runner at a
  time (finding #2's concern about OS-global cwd/env doesn't apply when there is provably nothing to
  interleave against).
- `ExecuteCodeTool`/`MountSkillTool` are built via `make_tool_descriptor_with_invoke`, their closures
  capturing `this` — reaching the provider's own `mounted_skills_`/`exec_state_`/`skills_` and the
  claimed `runner_binding_` instead of the five process-global statics. Their own static `invoke()`
  bodies are now unreachable poison sentinels (matching ADR-028's own test-file precedent), the real
  logic moved to `ToolDeclaringHistoryProvider::real_execute_code()`/`real_mount_skill()`.
- A new `AgentSession::history_provider()` accessor (mutable, host-callable, same category as
  `set_capabilities()`/`emplace_chat_client()`) lets `main()` call the provider's own
  `configure(session_id, runner_binding, mount_roots)` once, after skill materialization and before
  the first `StartRun` — the general reconfiguration seam ADR-028's own §6 named as unbuilt.
- `CodeActRunnerBinding<RunnerT>` is deliberately generic over `RunnerT` (not hardcoded to
  `native_jail::MediatedPythonRunner`), closing red-team finding #3: its own claim/fail-closed logic
  is independently, deterministically testable (`test_codeact_runner_binding.cpp`) with a trivial
  stand-in type, with zero CPython dependency, while `ToolDeclaringHistoryProvider` itself stays
  concretely bound to the real runner type (matching this file's own existing level of abstraction).

## 4. Falsifiable claims and verdicts

`test_codeact_runner_binding.cpp` (the claim primitive, isolated) and
`test_codeact_session_isolation.cpp` (the full stack — two real `AgentSession`s, a synthetic
provider built the same shape as `ToolDeclaringHistoryProvider`, a real internal tool-call round
loop) are both deterministic and offline.

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| B1-B5 | `CodeActRunnerBinding`'s claim is first-caller-wins, idempotent for the same session, and fails closed for a different one, without ever losing the caller's own reference to the wrapped runner. | `test_codeact_runner_binding.cpp`: first bind succeeds; a same-id re-bind succeeds; a different-id bind fails with `codeact.runner_bound_to_other_session` AND leaves the original binding untouched; `runner()` returns the exact same object, not a copy. | **CORRECT** |
| CI1 | Two sessions' `MountedSkillsState` never leak into each other — the actual payoff this ADR exists to prove. | `test_codeact_session_isolation.cpp`: session A mounts "demo"; A's own state reflects it; session B's state is completely unaware of it. | **CORRECT** |
| CI2/CI3 | The claim mechanism, wired through real `AgentSession`s exactly as production code wires it, still enforces "first session wins, second is rejected." | CI2: session A's `configure()` against the shared binding succeeds. CI3: session B's `configure()` against the SAME already-bound binding fails closed. | **CORRECT** |
| CI4/CI5 | The claim is enforced at the point of USE, not merely at configure() time — an unconfigured session can never reach the shared runner through its own tool closures. | CI4: session A (configured) genuinely reaches the shared runner (`use_runner` returns its real tag, proving no denial). CI5: session B (rejected at configure time) is denied on every attempt — `runner_binding_` stayed null. | **CORRECT** |

Real build verification: `tools/cli_chat.cpp` itself (the actual production consumer) compiles and
links cleanly against the corrected design (`agentengine_cli_chat.exe`), not just the standalone
test doubles.

## 5. What this ADR does not claim

- **Multiple concurrent CodeAct sessions in one process still cannot both execute Python.** This ADR
  makes that fact *safe* (the second session is refused, not silently corrupted) — it does not make
  it *possible*. Real multi-tenant CodeAct still needs either ADR-002 §6 item 5's deferred
  subinterpreter work, or the "one OS process per session" deployment shape ADR-002 §5.5.6 already
  names as the answer today.
- **`fork_from()`'s interaction with a bound `runner_binding_` is a named, unexercised residual, not
  a solved one.** `AgentSession::fork_from()` copies `history_provider_` by value (ADR-028's own
  addendum), which for this provider copies the raw `runner_binding_` pointer — the forked session
  inherits a pointer to a binding still bound to the SOURCE session's id, not its own new one, and
  neither `real_execute_code()` nor the binding itself re-validates that against the fork's actual
  identity (a `HistoryProviderT` has no generic way to learn its owning `AgentSession`'s
  post-fork session_id today — a gap broader than CodeAct specifically). `tools/cli_chat.cpp` never
  calls `fork_from()` (a single, non-forking REPL session), so this is unexercised in production
  today; named here rather than silently assumed safe for a future caller that does fork a
  CodeAct-wielding session.
- **The underlying "work" mount directory is still one shared physical location** — `fork_from()`
  copies `MountedSkillsState`/`ExecState`'s in-memory bookkeeping, never the files a session's
  `execute_code` calls actually wrote to disk (red-team finding #9, matching ADR-028 §6's own
  precedent for this class of residual).
- **`Interaction`/approval-suspend interplay is untouched** — this ADR and ADR-029 are independent;
  `execute_code`/`mount_skill` declare no `Approval<M>` policy today, so ADR-029's suspend-for-
  approval path never engages for them.

## 6. Files changed

- `include/agentengine/core/codeact_runner_binding.hpp` (new) — `CodeActRunnerBinding<RunnerT>`.
- `include/agentengine/core/agent_session.hpp` — new `history_provider()` mutable accessor.
- `tools/cli_chat.cpp` — `ToolDeclaringHistoryProvider` gains `configure()`, real per-instance
  `mounted_skills_`/`exec_state_`/`mount_roots_`/`runner_binding_` members, and
  `real_execute_code()`/`real_mount_skill()`; `ExecuteCodeTool`/`MountSkillTool`'s static `invoke()`
  bodies become unreachable poison; the five process-global statics reduce to two (the runner itself
  and its binding, both still process-wide by necessity); `main()` reordered so session construction
  precedes the skills banner, and calls `configure()` once.
- `tests/test_codeact_runner_binding.cpp` (new), `tests/test_codeact_session_isolation.cpp` (new) —
  this ADR's §4 evidence.
- `tests/CMakeLists.txt` — registers both new test targets.

Full regression suite: no new failures introduced by this ADR (verified against the pre-existing
baseline ADR-027/028/029 left — the one known failure, `test_mediated_python_runner_hostile_corpus`,
is the same pre-existing, unrelated flake, untouched by any file this ADR changes); `cli_chat.cpp`
itself rebuilds and links cleanly against the corrected design.
