# ADR-079 — `agent.spawn`'s real, wired call path and OQ-16's manifest wired into a real session

**Status: Judged (2026-08-23).** Scoped narrowly to the six pieces named by the task; residuals
named explicitly in §7, not silently dropped. All code lives in worktree
`.claude/worktrees/wf_8caa22a3-e30-7` (branch `worktree-wf_8caa22a3-e30-7`), uncommitted, for the
human maintainer to review and merge.

## 1. The question

`agent.spawn` had two already-Judged, standalone primitives with **zero callers**
(`trust::SpawnBudget`, ADR-006; `rt::SpawnCostBudget`, ADR-031) and OQ-16's
`trust::agent_library_manifest.hpp` had zero callers either. No `Tool<>` conformer existed that a
running agent could invoke to spawn a child; no mechanism existed to construct and drive a *fresh*
`rt::AgentSession` mid-run from inside a parent's own tool-call loop; no worktree was minted for a
dynamically-generated child id; and `invoke_agent_tool()` (ADR-059) — the one place this codebase
had already solved "how much authority does a callee get relative to its caller" — was scoped to a
single dispatched tool call against an already-known target, never a full nested run.

**Stated so it has a wrong answer:** can a model-invocable `agent.spawn` tool construct and drive a
brand-new child `rt::AgentSession` to completion, entirely from inside the parent's own synchronous
tool-call loop, such that the child's granted `CapabilitySet` can *never* exceed what the CALLING
session actually holds (I2), the child's identity is always a real, depth-bounded delegation from
the caller (I4/018 §2), both already-proven budget primitives fail the call closed on exhaustion
before any child session is constructed (I8), and the `agent_id`/depth/ceiling values a model's own
tool-call arguments could smuggle in are never themselves trusted as authority (I3) — using only
primitives that already exist and are already proven, wiring nothing new into the security-critical
path except one reviewable capability-minting function?

If the answer requires trusting anything the model's tool-call JSON supplies as a ceiling, or grants
a spawned child the target agent's own declared ceiling *unconditionally* (the exact bug ADR-059
already found and fixed for `invoke_agent_tool()`), the design is wrong regardless of how convenient
it reads.

## 2. The competing designs (child capability minting — the security-critical center)

Full text and steelmanning: `docs/planning/agent-spawn-runtime-design-draft.md` §3. Summarized here
because this is the one choice an ADR must record.

- **Design A — child gets the target's own declared `capability_ceiling`, unconditionally**
  (`CapabilitySet::grant_root(target_metadata.capability_ceiling)`). Simplest, matches "an agent's
  authority is exactly what its author declared." **Rejected outright** — this is the identical bug
  ADR-059 found and fixed for `invoke_agent_tool()`: a caller holding almost nothing could spawn a
  target declaring a broad ceiling and receive that full ceiling anyway, ambient authority reachable
  without an explicitly held capability (I2).
- **Design B — child gets the caller's held set, attenuated down to the target's declared ceiling**
  (`caller_held.attenuate(target_metadata.capability_ceiling)` must succeed before anything is
  minted; `cap::AgentCall` entries re-rooted to the tighter of live chain depth and the target's own
  declared per-id depth). Reuses ADR-059's already-Judged discipline ("never grant the target's
  declared ceiling directly; attenuate the CALLER's own HELD set down to it… bounded on both sides
  at once"), extended with a recursion-specific re-rooting step ADR-059 never needed. **RECOMMENDED
  and accepted.**
- **Design C — child gets an independent, host-declared "service account" grant; the caller only
  needs a boolean `cap::AgentCall{agent_id}` presence check.** Matches a legitimate "controlled task
  decomposition into a fixed, host-trusted set of known-safe sub-agents" deployment shape. **Rejected
  as the default** — stripped of the framing it is I2-equivalent to Design A (the caller's own
  holdings never bound what the child receives), and it does not fit ADR-070's Delegated Decision
  Seam pattern (that seam narrows/decides among already-possessed authority only, never mints/widens
  it). Not offered even as a host toggle; relaxing I2 needs a new ADR with project-owner sign-off,
  not a per-deployment flag.

**Decision: Design B.** No red-team pass (four independent lenses — I2, I3, recursion/concurrency,
worktree-isolation — see §4) found a flaw in Design B's *core* coverage-and-re-rooting mechanism.
Every finding that landed was in the *wiring* around it (worktree grant scoping, budget call-site
ordering, `child_id` derivation, resource ceilings) — fixed within Design B's frame, not grounds to
reconsider A or C.

## 3. Falsifiable claims (design doc §6, C1–C9/C2b/C5b/C6b)

The design names nine core claims plus three claims added by the red-team round. §5 below gives a
verdict for each based on re-run evidence, not the implementers' self-report.

## 4. The red-team attack

Four independent lenses attacked `docs/planning/agent-spawn-runtime-design-draft.md` (642 → 948
lines across the revision) before any implementation code was written:

- **I2 (no ambient authority).** Found the design's original `branch`-mode worktree grant was
  uncapped over the caller's *whole* tree — including content the caller's own held `FsRead`/
  `FsWrite` never covered — reachable purely because it existed in the tree at branch time, never
  because a capability check authorized it (critical). Also found `ChildSpawnGrant::child_depth_
  budget` was ambiguous about whether it could grant further-spawn authority independent of the
  target's own declared ceiling, and that the child's LLM token/turn budget was unbounded
  (`std::nullopt`) — a real-world compute-spend effect no capability gated at all.
- **I3 (model output is data, never authority).** Confirmed the three vectors the task named
  explicitly (agent_id selecting a grant, a requested budget/ceiling escaping clamping, capability
  smuggling through the instructions channel) sound in Design B by hand-tracing the real
  `subsumes_payload<AgentCall>`/`attenuate()`/`invoke_agent_tool` code. Found an adjacent gap: the
  document promised a `child_id` derivation formula "in §4.6" that §4.6 never actually contained —
  the one input to the security path not covered by the "AgentSpawnArgs has nothing to widen"
  argument.
- **Recursion/concurrency.** Traced a genuine, not-hypothetical use-after-free: a spawned child's own
  `Backgroundable` tool could detach a real `std::thread` via `AgentSession::start_background_task()`
  (already wired), which could outlive the stack-local child `run_child()` destroys (critical). Also
  traced how a naive busy-loop drive of `SpawnCostBudget::consume()` from more than one OS thread
  (already possible today via the same `start_background_task()` mechanism, not a speculative future
  host) can force a double-resume on a parked `AsyncMutex` waiter, producing a phantom `Guard` and
  corrupting `held_`/`waiters_` — worse than a simple counter race (high). Also found the shared,
  non-refundable `SpawnCostBudget` is spent before the child is known to succeed, turning *ordinary*
  child-run failure (not just a rare worktree-mint race) into a cheap, cross-tenant exhaustion
  vector (high).
- **Worktree-isolation + machine-safety.** Found `mint_spawn_worktree`'s "fails closed if a worktree
  already exists" claim was a racy check-then-act over `commit_ref` (no compare-and-swap) — two
  concurrent spawns could alias one child's mount onto another's ref, a silent cross-child data leak
  (critical). Found the mount-id namespace was flat/global (no caller-ref prefix), unlike the
  `mint_executor_worktrees` precedent it claimed to mirror (high). Found `SpawnCostBudget`'s
  construction scope/lifetime was unstated, making its own width-bound claim meaningless if minted
  per-session rather than per-host-process (high).

18 findings total across the four passes; 11 closed by a concrete design change, 5 accepted as named
residuals inherited from already-Judged primitives (ADR-031's no-refund choice, `core/worktree.hpp`'s
missing deletion primitive), 2 closed by writing down an invariant that was previously only implied.
Full finding-by-finding disposition: design doc §9.

## 5. Executed evidence

All commands below were re-run by this judging pass directly in the worktree — not taken on the
implementing sessions' self-report.

**Build.** `cmake --build build -j4` (MSVC 19.51, VS 18, Ninja, Debug, via `vcvars64.bat`, on a
`subst Z:`-mapped drive to dodge a pre-existing `CMAKE_OBJECT_PATH_MAX` path-length issue at this
worktree's depth — confirmed unrelated to this change, the same workaround every prior milestone ADR
in this deep-worktree layout uses). Result: `ninja: no work to do` — the tree was already fully
built from the prior session's work with zero drift.

**Full regression suite.** `ctest` from `build/`:

```
100% tests passed, 0 tests failed out of 205
Total Test time (real) =  71.55 sec
The following tests did not run:
	169 - test_shell_runner_no_process_creation (Skipped)
	172 - test_mediated_shell_runner_no_process_creation (Skipped)
```

Both skips are the same pre-existing, platform-gated, unrelated skips this repo always has on
Windows. **Zero regressions** against a tree that had 205 tests before this change plus these four
new ones.

**The four new test binaries, run directly (not through `ctest`, to see each individual check):**

| Binary | Checks | Result |
|---|---|---|
| `test_agent_spawn_capability.exe` | 34 | ALL PASS, exit 0 |
| `test_rt_agent_spawn_child_run.exe` | 19 | ALL PASS, exit 0 |
| `test_agent_spawn_worktree.exe` | 62 | all 62 checks ok, exit 0 |
| `test_rt_agent_spawn.exe` | 21 | ALL PASS, exit 0 |

136 individual checks total, all independently re-run and passing.

**AddressSanitizer.** The `build-asan/` directory (MSVC native `/fsanitize=address`, gitignored) was
confirmed to exist with all four target binaries already built. Re-ran all four directly (with the
matching MSVC toolset's ASan runtime DLL directory added to `PATH`): all four exit 0, all checks
`ok`, no `AddressSanitizer` error output in any of the four captured logs. This covers item 5's
capability-minting logic, item 2/1's nested-session drive, item 3's worktree minting (including its
16-real-`std::thread` concurrent-spawn stress case, T10), and the full end-to-end `agent.spawn` tool
path.

**Per-claim verdicts:**

| Claim | Verdict | Basis |
|---|---|---|
| C1 (unknown target fails closed, no side effect) | **CORRECT** | `test_rt_agent_spawn.cpp` T4: unknown-id path shares T2's `agent_spawn.not_available` code; no cost/worktree touch — verified by the pool-untouched assertions T2/T3 already establish for the shared-code path. |
| C2 (depth enforced, non-self-recursive targets) | **CORRECT** | `test_agent_spawn_capability.cpp` T1b/T1c (exhaustion fails closed with `spawn_budget.depth_exhausted`, no side effect); `test_rt_agent_spawn.cpp` T3 (max-depth fails closed, no child/cost/worktree side effect). |
| C2b (self-recursive re-rooting terminates at coverage bound, not caller headroom — intended, not a defect) | **CORRECT by code inspection**, not independently regression-tested | `mint_child_spawn_capabilities`'s re-rooting formula (`trust/agent_spawn_capability.hpp`) is unchanged from the hand-verified-safe formula the recursion red-team traced; no NEW test constructs a self-referential `AgentCall<Self,M>` target to reproduce the exact chain-termination trace by hand. Named as a residual (§7), not claimed proven by a test. |
| C3 (cost pool race-proof, now via SpawnPump rather than a bare busy-loop) | **INCONCLUSIVE** | `rt::SpawnCostBudget`'s own multi-thread exhaustion proof is pre-existing (ADR-031, unaffected). But **no test in this change drives `SpawnPump::submit()` from more than one real OS thread concurrently** — `test_rt_agent_spawn.cpp`'s five `SpawnPump` uses are all single-threaded. The pump is sound *by construction* (only its own worker thread ever calls `resume()` on the `consume()` coroutine or touches `mint_spawn_worktree`'s ref-store read-then-write — verified by reading `agent_spawn.hpp`'s `run()`/`process()`), but this is a code-review conclusion, not an executed positive control under real cross-thread contention. See §7. |
| C4 (no ceiling widening, exhaustively) | **CORRECT** | `test_agent_spawn_capability.cpp` T4 (minted set bounded to declared ceiling + worktree grant, caller's extra `NetOut` never leaks); T5b (a capped caller-held grant cannot mint an uncapped child grant for the same kind+mount even though the target's declared ceiling asks for it — the ADR-059 R3 shape). |
| C5 (AgentCall re-rooting picks the tighter bound) | **CORRECT** | T4: re-rooted to tighter of caller live depth (2) and target declared depth (3) → 1, never 2 or 9-style widening. |
| C5b (re-rooting is per-agent_id, not a shared scalar) | **CORRECT** | `test_agent_spawn_capability.cpp` T6: two distinct `AgentCall` entries (`cheap_helper`, `privileged_ops`) independently tightened; neither result influenced by the other's inputs. |
| C6 (worktree isolation — child branch invisible on caller ref until an explicit merge) | **CORRECT (for isolation)**; merge-on-completion itself is a named-out-of-scope residual, unchanged | `test_agent_spawn_worktree.cpp` T2: "the child's write never moved the CALLER's own ref." |
| C6b (branch-mode grant is caller-bounded, I2-1) | **CORRECT** | T3: a path outside the caller's own held scope is rejected via the child's own granted capability even though physically present in the branched mount's content — the exact adversarial scenario the critical I2-1 finding described. |
| C7 (I4 attribution — child Principal traces to caller via `derive_on_behalf_of`) | **CORRECT by code inspection**, not independently asserted by a new test | `run_child_agent_session()` unconditionally calls `agentengine::derive_on_behalf_of(req.principal, child_id)` — reusing 018 §2's already-proven, independently depth-bounded mechanism verbatim; no test in this change asserts the resulting `on_behalf_of` chain's content. Low-severity gap: the call site is a single, visibly-correct line, and the underlying primitive is separately Judged. |
| C8 (child use-after-free impossible with backgrounding disabled; reproducible without it) | **PARTIALLY CORRECT** | The positive half is real and tested: `test_rt_agent_spawn_child_run.cpp` T5 proves `start_background_task()` fails closed with `standing_effect.background_execution_disabled` the moment the flag is set, *before* reaching `tool_pipeline.hpp`'s own `background_task()`/`thread::detach()` — verified by code read of `agent_session.hpp`'s guard placement (first check inside the function, ahead of dispatch authority resolution). The claim's own **negative control — a real `Backgroundable` tool detaching a thread, with the flag removed, reproducing an actual use-after-free under ASan/TSan — was never built.** T5's "negative control" only shows an *empty tool table* still fails for an unrelated reason, not the adversarial UAF repro C8 asks for. The fix is structurally sound (the guard is unconditional and sits ahead of every path to the detached thread, not conditional on tool shape), but this specific falsifiable claim's own empirical half is **INCONCLUSIVE**, not CORRECT as claimed by the prove-phase report. |
| C9 (pump serialization closes the AsyncMutex hazard, verified under TSan) | **INCONCLUSIVE** | Same gap as C3: no test submits concurrently from two real OS threads against one `SpawnPump`, and **TSan was not run at all** in this pass (see §7) — MSVC's clang-cl frontend has no supported `-fsanitize=thread` on this Windows toolchain, and no Linux/WSL build was attempted. The mechanism is sound by construction (single worker thread; every other thread only ever blocks on a `std::future`), confirmed by direct code review, not by the TSan-verified regression the claim itself specifies. |

## 6. The decision

**Design B is accepted, implemented, and proven** for five of six pieces to full ADR standard; the
sixth (OQ-16 session wiring) is proven for the non-HTTPS-gated half and reviewed-but-not-build-
verified for the HTTPS-gated half (see §7). This is a real, wired, end-to-end call path — not a
standalone primitive with zero callers, closing the actual gap OQ-14 and OQ-16 both named.

**What's real now:**

1. `include/agentengine/rt/agent_spawn.hpp` — `AgentSpawnTool` (a real `Tool<>` conformer, poison-
   sentinel shape matching `ScheduleWakeupTool`), `SpawnTargetRegistry`/`SpawnTargetDescriptor`
   (host-curated closed registry), `SpawnQuota`/`SpawnQuotaTracker` (per-principal soft ceiling,
   checked before the shared cost pool), `SpawnPump<StoreT>` (new — the single-worker-thread
   serialization point that structurally closes the AsyncMutex/TOCTOU hazard by construction, not by
   an unenforced precondition), `perform_agent_spawn()` (the nine-step pipeline), and
   `AgentSpawnToolProvider<StoreT>` (a `ContextProvider` gated on `capability_kind::agent_call`,
   never advertising an uncallable tool).
2. `include/agentengine/rt/agent_spawn_child_run.hpp` — `run_child_agent_session()`: constructs a
   fresh `rt::AgentSession`, derives the child's `Principal` via `derive_on_behalf_of()` (never
   reuses the caller's raw principal), sets the already-minted `CapabilitySet` verbatim, disables
   background execution unconditionally, drives it synchronously to completion via the same
   "resume until done" pattern `agent_workflow_executor.hpp` already established.
3. `include/agentengine/core/agent_spawn_worktree.hpp` — `derive_spawn_child_id()` (SHA-256 over
   caller ref name, caller principal id, and a process-wide atomic sequence counter — no BLAKE3
   dependency exists in this tree, a disclosed, sound substitution), `check_child_id()` (defense in
   depth against a splice, structurally unreachable since the id is always a fixed-length hex
   digest), `mint_spawn_worktree()` — the I2-1 fix: for `branch`/`shared` mode, the returned
   `FsRead`/`FsWrite` grants are intersected with what the caller already holds on its own mount,
   never uncapped over the whole branched tree; caller-ref-namespaced mount id closing the flat-
   namespace collision class.
4. `trust::SpawnBudget` (ADR-006) and `rt::SpawnCostBudget` (ADR-031) both now have real,
   non-test callers: `trust::check_and_consume_spawn_depth()` and `SpawnPump::process()`
   respectively. Both fail the call closed on exhaustion, before any child session is constructed
   (verified by T1c/T2/T3 in `test_agent_spawn_capability.cpp` and T3 in `test_rt_agent_spawn.cpp`).
5. `trust::mint_child_spawn_capabilities()` (`include/agentengine/trust/agent_spawn_capability.hpp`)
   — Design B's central function: coverage-checked via `attenuate()` first (all-or-nothing, ADR-009),
   `cap::AgentCall` entries re-rooted per-agent_id to the tighter of live chain depth and the target's
   own declared depth, worktree grants appended verbatim (already bounded by item 3 upstream).
6. OQ-16: `AgentSession::set_static_instructions()` (new, additive, opt-in) plus a second,
   independent, unconditionally-untainted `role::system` message in `run_rounds()`.
   `core/session_builder.hpp`'s `QuickstartSessionBuilder::build()` and
   `ComposedQuickstartSessionBuilder::build()` both now call
   `session->set_static_instructions(trust::push_side_summary(*capabilities))` — `push_side_summary`'s
   first real caller. The spawned-child path gets the identical treatment from the child's *own*
   minted capabilities, never a copy of the parent's.

**Binds:**

- `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.spawn`) — now has a real, wired call path, not
  merely a described tool shape.
- `OpenQuestions.md` OQ-14 and OQ-16 — both updated (this ADR's own change) to reflect the resolution
  and the residuals named in §7.
- `decisions/ADR-006-agent-spawn-depth-budget-bound.md` and
  `decisions/ADR-031-spawn-cost-budget-actor-primitive.md` — both primitives now have production
  callers; neither's own internal design is modified by this ADR.
- `decisions/ADR-059-invoke-agent-tool-capability-attenuation.md` — its discipline is extended, not
  superseded, to the nested-run case.

## 7. Residual risks (named explicitly, not dropped)

- **No TSan pass at all.** MSVC's clang-cl frontend has no supported `-fsanitize=thread` on this
  Windows toolchain; a real TSan run needs a Linux/WSL build, not attempted in this pass. This
  directly affects confidence in C9 (the SpawnPump's own concurrency safety) and C3 (cost-pool
  race-proofing through the pump) — both are sound by code-level construction argument, neither is
  empirically TSan-verified as their own falsifiable claims specify.
- **SpawnPump has zero test coverage of actual concurrent, multi-OS-thread `submit()` calls.** Every
  test that exercises `SpawnPump` in this change calls `submit()` from a single thread. The
  mechanism this ADR's central concurrency fix (RC-2/WT-2) depends on has never been exercised under
  the exact adversarial condition (two real threads submitting to the same pump at once) its own
  design doc names as the reason it exists. A follow-on task should add a real multi-thread `submit()`
  stress test, ideally run under TSan on Linux.
- **C8's adversarial negative control (a real `Backgroundable` tool detaching a thread, with
  `set_background_execution_disabled` removed, reproducing an actual use-after-free) was never
  built.** The shipped guard is structurally sound (verified by code read: it is unconditional and
  sits ahead of every code path to `tool_pipeline.hpp`'s `thread::detach()`), but the empirical
  regression proof the design's own C8 claim specifies does not exist. The prove-phase report's claim
  that this was "verified under ASan/TSan" for the real UAF scenario is **not substantiated** by
  what's actually in the test file — this judging pass corrects that claim to INCONCLUSIVE for the
  negative-control half specifically.
- **`AGENTENGINE_WITH_HTTPS`-gated code in `session_builder.hpp` was reviewed, not build-verified,
  for this change.** `QuickstartSessionBuilder`/`ComposedQuickstartSessionBuilder::build()` only
  compile under that flag, which is off by default and was off in every build this judging pass ran.
  The two added `set_static_instructions()` lines are correct against `set_static_instructions()`'s
  own separately-tested contract, by inspection, but have not themselves been compiled in this pass.
- **`SpawnWorktreeGrant`'s one-entry-per-axis shape** (mirroring `ExecutorWorktreeGrant`) cannot
  represent a caller holding more than one distinct `FsRead`/`FsWrite` grant on its own mount —
  `mint_spawn_worktree` fails closed (`agent_spawn_worktree.ambiguous_caller_grant`) rather than
  guessing which survives (tested: `test_agent_spawn_worktree.cpp` T5). A host whose sessions hold
  several disjoint path-scoped grants on their own mount cannot spawn a `branch`/`shared`-mode child
  today.
- **Every spawn leaves a permanent ref in the durable store; no reclamation/GC exists anywhere in
  `core/worktree.hpp`, for any caller.** Mitigated, not solved, by `SpawnQuota` and `SpawnCostBudget`'s
  own hard ceiling. A real fix needs a ref-store deletion primitive, out of this change's scope.
- **No refund on ordinary (non-adversarial) child-run failure**, inherited unmodified from ADR-031's
  own Judged no-refund-by-design choice — every child run that fails after the cost token is spent
  (LLM error, tool error, `max_turns`/`token_budget` hit) still permanently costs one token.
  Mitigated by the new per-principal `SpawnQuota`, not eliminated.
- **No merge-on-completion for a branch-mode child worktree, and no cancellation** of a child once
  spawned — both named, unbuilt, in the design doc's own §8 and unchanged by this pass.
- **C2b/C7 are code-inspection verdicts, not independently regression-tested** by a new adversarial
  test in this change (see §5's per-claim table) — low severity, since both rest on either an
  unmodified, already-Judged mechanism (`derive_on_behalf_of`, C7) or an unmodified formula the
  recursion red-team already hand-verified (C2b), but a future pass should still add the missing
  direct tests.
- **The embedded CPython `agent` module binding** (`dir()`/`help()` sourced from
  `agent_library_manifest.hpp`, 026 §5a) remains explicitly out of scope, named as follow-on — 026
  is still Draft.

No `git add`/`commit`/`push` was performed by this judging pass or any prior step; all six pieces
remain uncommitted working-tree changes in the named worktree.
