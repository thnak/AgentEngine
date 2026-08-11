# Worktree sharing — skills and sub-agents, as-built

**Compiled:** 2026-08-11 · **Status:** as-built trace, not a spec — cites the RFC/file each claim
comes from; when this disagrees with the code, the code (and the RFC it cites) wins, fix this doc.

Grew out of a direct question: today, how do skills and sub-agents actually share the worktree
environment? Short answer up front: **they don't share one, yet — not because it's broken, but
because most of the wiring that would make them share one doesn't exist.** Skills materialize from
an isolated, per-`SkillsProvider` store that never touches a session's own worktree Ref; sub-agent
worktree sharing (`agent.spawn` → sub-worktree) is fully designed and unit-tested at the primitive
level, but has no real caller. Details below, organized the same way
`tool-and-skill-context-and-invocation.md` organizes its own "declared vs. actually wired" trace.

---

## 1. The worktree primitive (025 §2-§3)

A worktree is a content-addressed object store (`Blob`/`Tree` by digest) plus a mutable `Ref`
(`name → tree digest`), not one monolithic type (`include/agentengine/core/worktree.hpp:47-86`).
`sharing_mode` (`shared | branch | readonly | scratch`, `worktree.hpp:86`) governs how a
`SubWorktree` relates to its parent `Ref` — `shared` reuses the parent's `backing_ref_name`
directly (immediate cross-visibility), `branch` copies the parent's digest and diverges
independently in both directions until merge-on-join (025 §4), `readonly` pins a digest with an
empty `backing_ref_name`, `scratch` starts empty. All four modes are proven at the `Ref`/`Tree` API
level (`tests/test_worktree_sub_worktree.cpp`, `test_worktree_branch_concurrency.cpp`,
`test_worktree_merge.cpp`) — using bare string ref names (`"session:s-1/agents/a"`) as stand-ins for
what a real agent would use, never a real `AgentSession`/spawned-actor instance.

`Mount` (`worktree.hpp:941-945`) is a separate, host-side concept: `{mount_id, ref_name,
subtree_path}` — a binding from a guest-visible name (matched against `cap::FsRead`/`cap::FsWrite`)
to a concrete `(ref, subtree)` location, never guest-supplied (I2). Two mounts can point at the same
ref with different subtrees, or at entirely different refs.

**025 §3's canonical layout** describes the *target* shape — one session worktree with sub-worktrees
and mounts as children:

```
session:s-42
  /work /input /out
  /agents/researcher   sub-worktree
  /agents/writer       sub-worktree
  /skills/<name>        loaded skill packages, read-only (009 §8)
```

§2 below is where this diagram and the actual code diverge.

## 2. Skills — isolated per-provider store, not a subtree of the session's own Ref

`SkillsProvider<ObjectStoreT>` (`include/agentengine/core/skill_provider.hpp:104-106`) is, by its
own header comment, *"a complete, self-contained `ContextProvider` conformer"* that **owns its own
object/ref store**:

```cpp
ObjectStoreT object_store_;        // skill_provider.hpp:264 — private to THIS provider instance
quark::InMemoryStore ref_store_;   // skill_provider.hpp:265 — private to THIS provider instance
```

For each resolved skill, it assembles a `Tree`, commits it under `"skill:" + name` as its own `Ref`
in that private `ref_store_`, and records `Mount{name, committed_ref->name, ""}` — bare skill name
as `mount_id` (`skill_provider.hpp:238-243`; the historical `/skills/<name>`-as-mount_id bug and its
fix are ADR-024 §4). **This confirms the layout diagram in §1 is aspirational as drawn**: `/skills/<name>`
does not live as a subtree of the *same* `Ref` the session's `/work`/`/input`/`/out` mounts point
at — it lives in a completely separate content-addressed store that a `SkillsProvider` instance owns
privately, materialized into the real sandbox filesystem side-by-side with the session's own mounts
(`src/backends/native_jail/skill_mount_materializer.hpp::materialize_skill_mounts`, one real host
subdirectory per skill, reusing the same generic `materialize_mount` primitive the session's own
`/work`/`/input`/`/out` mounts use — ADR-024 §4). The end result at the sandbox filesystem is
indistinguishable from the diagram; the storage layer behind it is two separate universes that
happen to get materialized next to each other, not one shared tree.

**A second, narrower isolation exists inside a single conceptual session, found while tracing
`tools/cli_chat.cpp`**: the CLI constructs three independent `SkillsProvider<>` instances in one
process (`main()`'s `startup_skills`, `CliSession::skills_`, `shared_codeact_skills()`), each with
its own private object/ref store, all resolving the same source list and asserted to reach
byte-identical results *by construction* (`cli_chat.cpp:236-262`) — never by actually sharing
storage. So even skill-to-skill sharing within one running session is, today, three separate stores
that happen to agree, not one store multiple consumers read from.

**What this is not**: not a bug, and not contradicted by any ADR-024 claim — ADR-024 fixed
"materialize a skill's private store into a real sandbox at all" (previously not wired anywhere
outside the provider instance); it never claimed to unify the skill store with the session's own
worktree Ref. §7 residuals already name adjacent gaps (scoping only covers `native_jail`'s
`MediatedPythonRunner`, no type-level guarantee ties declared/invocable tool tables) but not this
one specifically — recorded here as a previously-undocumented as-built fact, not a new finding of
brokenness.

## 3. Sub-agents — designed, unit-tested at the primitive level, not wired

`026-Agent-Facing-Runtime-Surface.md` §5 (line 185-186) states the target design: *"`agent.spawn`
inherits an attenuated capability set and a sub-worktree (025 §3) — a spawned agent can never exceed
its parent."* 026 is itself marked Draft (per ADR-006's own residual, quoted below) — this is
normal RFC style in this repo (present-tense target design, implementation status tracked
separately), not a claim of completeness.

Tracing what actually exists:

- `include/agentengine/core/agent_registry.hpp` (`register_agent<A>()`, the compile-time
  agent-metadata compiler) has **zero** worktree references. `check_handoff_cycle()` is a stub,
  explicitly commented *"needs 014's handoff/workflow graph, explicitly out of scope for M2."*
- `decisions/ADR-006-agent-spawn-depth-budget-bound.md` proves only the recursion-depth counter
  (`SpawnBudget::mint_root`/`attenuate_for_spawn`) in complete isolation from any real spawn path.
  Its own §9: *"No test exercises this against a real `agent.spawn` call path, because none exists
  yet (026 is Draft)."*
- `decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md` §7 independently confirms:
  *"`AgentSession` still owns no sandbox and no tool-call loop in production... Building that
  ownership is a separate, larger architectural question this ADR does not attempt."*
- **Updated 2026-08-11 — no longer true as stated.** `014-Workflow-and-Orchestration.md` §1 now
  states the resolved half of this gap directly, and `workflow/worktree_scoping.hpp`
  (`decisions/ADR-032-workflow-executor-worktree-scoping.md`) is real code: `Executor::worktree_mode`
  declares a per-node `sharing_mode`, and `mint_executor_worktrees`/`resume_executor_worktrees` turn
  it into a real `SubWorktree` + capability-gated `Mount`, reusing `create_sub_worktree` unmodified.
  **What is still true, narrower than before**: this closes the POLICY+MINTING half only — no code
  path wires a minted grant into a running `FunctionExecutor`'s `EffectContext`/`ExecutorBody` (no
  production host builds a `FunctionExecutor` fleet at all, so there is no real caller to design that
  wiring against yet), `agent`/`sub_workflow`-kind executors still don't exist to scope, and
  merge-on-join (WHEN a branch folds back into its parent) is unbuilt. The `agent.spawn`
  half below (sub-agent worktree inheritance via a real spawn call path) is entirely untouched by
  ADR-032 — that is a different mechanism (026 §5's `AgentCall`) from a workflow executor's own
  worktree, and remains exactly as described in the rest of this section.

**Net**: `create_sub_worktree`/the four sharing modes are a real, tested primitive
(`test_worktree_sub_worktree.cpp` and friends), now joined by a real, tested policy layer for
workflow executors specifically (ADR-032) — but no code path calls any of it from an actual
`agent.spawn` or handoff. The mechanism exists for both worktrees-in-general and
workflow-executor-worktrees-specifically; nothing currently uses either for a real multi-agent run
with a spawned agent in it.

## 4. Test coverage — what's proven vs. not

**Proven:**
- Sub-worktree sharing-mode semantics (`shared`/`branch`/`readonly`/`scratch`) at the `Ref`/`Tree`
  API level, including concurrent-branch merge and conflict surfacing.
- Multiple mounts at different subtree paths of the same ref not disturbing each other on write
  (`test_worktree_mount.cpp` C1-C5).
- Two skills from a single `SkillsProvider` instance both materializing to distinct real host
  directories and both being independently readable (`test_skill_mount_materializer.cpp` R1,
  `test_skill_provider_mount.cpp` R1/R2); a name collision across sources failing the whole load
  closed.
- A real `MediatedPythonRunner` reading a materialized skill's file through the mediated filesystem,
  including one live-model transcript (ADR-024 §5-§6).
- **Added 2026-08-11 (ADR-032)**: workflow executor worktree scoping at the `SubWorktree`/`Mount`
  level — two `branch` siblings isolated from each other and from the parent; two `shared` executors
  cross-visible; a capability minted for one executor's mount rejected against another's; a `readonly`
  executor correctly gets no guest-facing `Mount`; a resumed `branch`/`shared` grant continues the
  same worktree rather than silently re-branching (`tests/test_workflow_worktree_scoping.cpp`, 47
  checks).

**Not proven — no test found** (grepped `tests/` for "worktree" combined with "agent_session",
"multi_agent", or "spawn"):
- No test exercises two `SkillsProvider` instances, or two agents, reading/writing the *same*
  underlying object store instance.
- No test exercises a real spawned sub-agent actor receiving a `SubWorktree`.
- No test exercises a minted `ExecutorWorktreeGrant` actually wired into a running
  `FunctionExecutor`'s `EffectContext`/`ExecutorBody` — ADR-032 proves the grants directly against
  `mount_read`/`mount_write`, not through a live executor actor, because no production host builds a
  `FunctionExecutor` fleet to wire one into yet (§3 above).

## 5. Relevant already-resolved questions (not reopened here)

- **OQ-13** (worktree merge policy for concurrent agents) — resolved: model-assisted conflict
  resolution is escalate-by-default/draft-only-never-auto-apply; `shared` mode is permitted for
  concurrent siblings but not default, because a deterministic prove
  (`shared_mode_readskew.cpp`) showed single-writer serialization prevents data races but not torn
  cross-file reads between concurrent siblings — a real, measured hazard, mitigated by a staleness
  note next turn, not eliminated.
- **OQ-14** (in-sandbox library surface area) — resolves the curation rubric and `agent.spawn`'s
  depth-bound half via ADR-006; explicitly leaves the *cost* half open (tracked against 023), and
  does not address worktree isolation for spawned agents at all.
- **025 §10 Q5** (cross-session sharing) — resolved by not sharing: the worktree stays
  session-scoped; 030 (Project) indexes N independent worktree refs above sessions instead of
  broadening one worktree's scope.

None of the above is reopened by this doc — recorded here only so a reader doesn't have to
reassemble the full current picture from five separate files to answer "do skills and sub-agents
share a worktree today."
