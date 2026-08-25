# Design draft — every session is bound to a worktree Ref; child sessions branch for merge/reset

**Status: Revision 2 — §3's scope question resolved by project-owner decision (2026-08-25, this
turn); not yet red-teamed.** This supersedes, not patches, part of `ADR-096`'s
`SandboxToolProvider` construction (`src/backends/native_jail/sandbox_tool_provider.hpp`) — per
explicit project-owner instruction (2026-08-25): *"mọi session phải đi kèm với worktree đó là bắt
buộc. các session con phải tạo nhánh từ đây để có thể merge/reset. đây là một thiếu sót trong
thiết kế cần được thiết kế lại, không được phép patch."* (Every session must come with a worktree
— that is mandatory. Child sessions must branch from it so merge/reset are possible. This is a
design gap that needs a redesign, not a patch.)

## 1. The gap, confirmed against real code (not assumed)

- **`AgentSession` (`include/agentengine/rt/agent_session.hpp`) has zero worktree/Ref state.**
  Grepped for `Ref`, `WorktreeObjectStore`, `TurnCommit`, `commit_turn` — no match anywhere in the
  file. `fork_from()` (line 1161) copies history/state/metadata/`history_provider_`/capabilities-
  adjacent flags — nothing filesystem- or worktree-shaped at all. A session today has no notion of
  "my own Ref," "my parent's Ref," or "branch/merge/reset."
- **`SandboxToolProvider`/`SessionShellSandbox` (ADR-096, shipped 2026-08-25) does not touch the
  worktree layer either.** It builds its sandbox from a bare host directory
  (`std::filesystem::create_directories(host_root / digest(session_id))` +
  `MediatedFileSystemAdapter::create(...)`) — no `Ref`, no `Mount`, no
  `materialize_mount()`/`harvest_mount()` call anywhere in that path. This mirrors
  `tools/cli_chat.cpp`'s pre-existing Python wiring (`shared_python_runner()`), which has the
  identical gap — so this was inherited, not newly introduced, but ADR-096 never named it as a
  residual either. That is this ADR's own miss, corrected here rather than smoothed over.
- **The real bridge between a worktree `Mount` (Tree/Ref content) and a live host directory
  already exists, tested, shipped**: `src/backends/native_jail/worktree_mount_sync.hpp`'s
  `materialize_mount()` (Tree → host dir, gated by an already-bound `cap::FsRead`) and
  `harvest_mount()` (host dir → Tree, gated by `cap::FsWrite`, emits one `ContentItem` per file —
  025 §7's "the agent saves a file, the user receives an artifact" claim, made literal). Its only
  two real callers today are `skill_mount_materializer.hpp` (skills) and
  `src/backends/native_process/native_worktree_bridge.hpp` (ADR-071's unsandboxed providers).
  Neither `execute_code` (cli_chat.cpp) nor `run_shell` (`SandboxToolProvider`) uses it.
- **Branch-for-merge/reset already exists too, but scoped BELOW the session, not AT it**:
  - `core/worktree_sub.hpp::create_sub_worktree()` — the real COW-branch primitive (`sharing_mode::
    branch` seeds a new Ref at the parent's current tree digest, captures `base_digest` for a later
    three-way merge via `worktree_merge.hpp`).
  - `core/agent_spawn_worktree.hpp::mint_spawn_worktree()` — a dynamically-`agent.spawn`'d CHILD
    AGENT (within a running session, via a tool call) gets a sub-worktree branched off the
    caller's own `Ref`, with I2-1's capability-intersection fix so the branch never inherits more
    than the caller explicitly held.
  - `workflow/worktree_scoping.hpp::mint_executor_worktrees()` — the same idea for a static
    `Workflow` graph's executors, minted up front from the graph definition.
  - **Neither is triggered by SESSION creation.** Both operate one level down: an agent or a
    workflow executor gets a sub-worktree; nothing mints one when an `AgentSession` object itself
    is constructed, and nothing connects either mechanism to `SandboxToolProvider`.
- **`commit_turn()`/`turn_digest_at()` (`core/worktree_ref_store.hpp`)** — real, tested, turn-level
  checkpointing of a Ref's tree digest (`TurnCommit{ref, turn}`) — is a pure store-level function.
  Nothing in `AgentSession::run_rounds()`'s own turn loop calls it. A session today produces no
  `TurnCommit` at all, so "reset to turn N" has nothing to reset TO.

**Net effect of the gap**: a session's shell/Python output is not a worktree artifact — it cannot
be branched, merged, or reset to a prior turn, and a spawned/forked "child" session shares nothing
with its parent's worktree state because neither has any worktree state to share in the first
place. `worktree_merge.hpp`'s real three-way merge machinery, `agent_spawn_worktree.hpp`'s real
capability-safe branching, and `worktree_mount_sync.hpp`'s real materialize/harvest bridge are all
already-Judged, already-tested primitives sitting unused by the actual session lifecycle.

## 2. The mandatory invariant (as stated, restated precisely)

1. **Every `AgentSession` is bound to exactly one worktree `Ref` from the moment it exists** — not
   lazily created on first sandbox use the way `SandboxToolProvider` currently defers its own
   sandbox construction. A session with no Ref is not a valid session under this invariant.
2. **A session created as a child of another session's Ref branches from it**
   (`create_sub_worktree(..., sharing_mode::branch)`), not from an empty tree — so the child starts
   with the parent's current content and diverges only on its own next write, matching
   `agent_spawn_worktree.hpp`'s already-Judged COW semantics.
3. **Branching must be undoable in both directions**: *merge* (the child's changes flow back into
   the parent, via `worktree_merge.hpp`'s three-way merge against `SubWorktree.base_digest`) and
   *reset* (the child — or the parent — can discard its own divergence and return to a known-good
   tree digest, presumably via `turn_digest_at()`/a direct `commit_ref` back to a prior digest).
   Both operations already exist as primitives; neither is currently reachable from a session.
4. **`SandboxToolProvider`'s own sandbox construction must change**: instead of a bare host
   directory, it materializes from the session's bound Ref (`materialize_mount()`) and harvests
   back into it (`harvest_mount()`) — making shell-written files real, versioned, content-addressed
   worktree content, not files sitting in an OS directory outside the worktree system entirely.

## 3. What "child session" means here — resolved (project-owner decision, 2026-08-25)

Of the three candidates §1's investigation found, **(a) `fork_from()` and (b) `agent.spawn` are
both in scope; (c) (a brand-new `AgentSession` object explicitly declared as another session's
child) is explicitly OUT of scope** — no such construction path exists or is being added.

The project owner's own framing of the two in-scope cases, restated precisely because it changes
what each one needs:

- **`fork_from()` models a background agent that inherits the FULL context of the main session.**
  Not a narrow, single-request child — a genuine continuation that needs the parent's entire
  worktree content available to it, branched (not shared live), so it can read everything the
  parent could and diverge independently from that point. This is the COW `sharing_mode::branch`
  case, at full session scope, not sub-agent scope.
- **`agent.spawn` handles exactly one small request.** Already matches `agent_spawn_worktree.hpp`'s
  existing shape closely (`sharing_mode::branch`/`scratch`/`readonly`, I2-1's capability
  intersection) — the real gap here (per §1) is narrower than it looked: `mint_spawn_worktree()`
  already exists and is already correct; what's missing is that nothing today hands it the
  SESSION's own bound root `Ref` as `caller_ref` — it has no real root to branch FROM yet, only a
  Ref some test or ad hoc caller happens to pass in. Once §2 item 1 (every session has a root Ref)
  is real, `agent.spawn`'s existing mechanism gets a real, session-scoped caller_ref "for free" —
  the same pattern ADR-096 itself found repeatedly (reusing already-Judged mechanisms closes most
  of a claim before any new code is written).

### The fork_from() consequence this decision forces — C2 is exploited, not loosened

`fork_from()` (`agent_session.hpp:1161`) currently does a PLAIN FIELD COPY: `history_provider_ =
source.history_provider_;`. For a session composed with `SandboxToolProvider` (ADR-096 C2), this
is a hard compile error today, by design — copying would alias the parent's live
`MediatedFileSystemAdapter`/`SessionShellSandbox` state onto the child, a real I1/I4-adjacent
session-isolation hazard.

With this redesign, that compile error is **not something to relax** — it is the exact signal that
tells an implementer "you must not copy this, you must build it fresh." The correct fix, forced by
the compiler rather than merely documented:

1. `fork_from()` must stop doing `history_provider_ = source.history_provider_` unconditionally for
   a worktree-bound provider pack. Instead, for the worktree-owning member specifically, it must
   call `create_sub_worktree(store, source's Ref, new_session_id, sharing_mode::branch)` to mint a
   REAL, independent branch Ref (COW, seeded at the parent's current tree digest, `base_digest`
   captured for a later merge — exactly `agent_spawn_worktree.hpp`'s own semantics, at session
   scope instead of sub-agent scope).
2. It then constructs a FRESH provider instance bound to that NEW branch Ref (never the parent's
   live one) and calls `.engage(...)` on the target's `ComposedContextProvider<Ms...>` — the exact
   "real recovery path" `test_composed_context_provider.cpp`'s own P3b case already proves works
   for a moved-from/reset instance (ADR-096 C5 leaned on the same `engage()` guarantee for
   `clear_in_process_state()`).
3. Net result: **C2's "must not compile" property survives completely intact** — a plain copy still
   fails to compile, on purpose. What changes is that `fork_from()` grows real, explicit branch-
   and-engage logic instead of a copy, for exactly the members that need it. This is the same shape
   Option A/B below both have to implement either way; it does not by itself decide between them.

## 4. Design sketch — narrowed by §3's resolution

§3 changes the calculus between the two remaining live options (Option C is dropped — see its own
§5 cost note, unrelated to this decision, still a bad fit): `fork_from()` is an ordinary,
**synchronous** `AgentSession` member function, not itself part of the async `on_context()`
protocol. It needs direct, synchronous access to "this session's Ref" and "the worktree store" to
call `create_sub_worktree()` — reaching that state through a `ContextProvider` that only surfaces
itself inside an `on_context()` coroutine call is an awkward fit for a plain synchronous function,
even with an accessor escape hatch.

**This makes Option A (`AgentSession` owns the Ref binding directly) the better fit for what §3
actually requires**, not merely a stylistic preference — `fork_from()`'s own branch-and-engage
logic (above) needs to be written IN `fork_from()` itself, synchronously, and that is far more
natural against a plain `Ref session_ref_` member than against state buried inside a composed
`ContextProvider`. Option B is not eliminated outright (a hybrid — `AgentSession` owns the Ref,
`SandboxToolProvider`/a new `WorktreeSessionProvider` still reads it via `EffectContext` each round
— may be the actual shape that survives red-team), but Option A is now the starting point rather
than a coin flip among three.

**This is still not a final decision** — it is the informed starting point for a real red-team
pass, per this project's own design→red-team→prove→judge discipline. The original Option A/B/C
sketches below (§5, renumbered from Revision 1's §4) are kept verbatim for the record.

## 5. Design sketch, Revision 1 (kept verbatim; §4 above narrows this, does not replace it)

### Option A — `AgentSession` itself owns the Ref binding (new required state)

Add a mandatory (non-optional after `initialize()`) `Ref session_ref_` (or equivalent) member.
`initialize()` either creates a fresh root Ref (name derived from `session_id`, mirroring ADR-096
C8's own digest-based naming discipline) or, given a parent `Ref` + `sharing_mode`, calls
`create_sub_worktree()`. `SandboxToolProvider` is rewritten to read the bound Ref off
`SessionContext`/a new `EffectContext` field instead of constructing its own bare directory, and
calls `materialize_mount()`/`harvest_mount()` around each `run_shell` call (or lazily, on first
use, then incrementally). **Cost**: touches `AgentSession` itself — the single largest, most
heavily red-teamed file in this codebase (11+ rounds per `session_shell_wiring.hpp`'s own
disclosed history) — a materially bigger, riskier surface than ADR-096's `ContextProvider`-only
approach.

### Option B — a `WorktreeSessionProvider` `ContextProvider`, composed like `SandboxToolProvider`

Mirrors ADR-096's own Design B shape: a new `ContextProvider` conformer owns the Ref binding,
exposes it through a new `EffectContext` field (e.g. `EffectContext::session_worktree_ref`, same
"borrowed, never owned, no capability check of its own" discipline `sandbox_fs` already
establishes), and `SandboxToolProvider` is rewritten to depend on that field instead of taking a
bare `host_scratch_root` constructor argument directly. Composed via the same
`Ms.../ComposedContextProvider<Ms...>` mechanism, in front of `SandboxToolProvider` in declared
order (`ContextContribution`'s drop-order-determinism rule already governs this). **Cost**: two
providers now have an ordering DEPENDENCY (`SandboxToolProvider` reads a field
`WorktreeSessionProvider` must have already written this round) — ADR-096's own C4 claim ("no real
`ContextProvider` in the tree reads a field another provider wrote via `EffectContext` in the same
round") would become FALSE the moment this ships, and would need to be explicitly re-opened, not
silently left stale in ADR-096's own text.

### Option C — push it to `session_builder.hpp` / a new builder step

`session_shell_wiring.hpp`'s own file-header already recorded that `session_builder.hpp` was
deliberately kept free of any tool-table concept (judged out of scope during Tier-1). This option
revisits that decision explicitly: a `.with_session_worktree(parent_ref?, mode)` builder step
that both binds the Ref and constructs `SandboxToolProvider` against it in one place. **Cost**:
reopens a decision the Tier-1 pass explicitly declined to make, on an 11+-round-red-teamed file —
should not be done as a side effect of this redesign without its own scoping.

None of these was selected in Revision 1. §4 above narrows it to Option A as the working starting
point (Option C dropped); Option A vs. a hybrid A/B still needs a real red-team pass before either
is finally picked.

## 6. What this draft does NOT attempt yet

- Does not modify `SandboxToolProvider`, `AgentSession`, or `session_builder.hpp`.
- Does not make Option A vs. hybrid A/B final — that is what the next red-team pass is for.
- Does not design `agent.spawn`'s own follow-on wiring in detail (§3: once a session has a root
  `Ref`, `mint_spawn_worktree()` needs to be CALLED with it somewhere real — presumably wherever
  `agent.spawn`'s tool-call handler runs today — not yet traced to that call site or designed).
- Does not address Python (`cli_chat.cpp`'s `execute_code`) — same gap, explicitly out of scope of
  ADR-096 already, inherited here as a known-adjacent, not-yet-scoped problem.
- Does not address `clear_in_process_state()`'s interaction with a bound Ref (does clearing
  in-process state reset the Ref binding too, or only the `ContextProvider`/history state it
  already resets? Not analyzed yet).
- Does not design the merge/reset API surface itself (who calls `worktree_merge.hpp`'s three-way
  merge when a `fork_from()`'d background agent's work is ready to fold back — a new `AgentSession`
  method? A host-driven call? Not decided).

## 7. Round 1 red-team findings (2026-08-25) — Revision 2's central story did NOT survive intact

Three independent agents (security/I2-I3, C++/architecture correctness, fabrication-hunt), each
reading the real code directly rather than trusting this draft's own citations. Findings recorded
plainly, not silently fixed — most are **BLOCKING**. §3's "C2 is exploited, not loosened" framing
does not hold up as written; the "fresh provider + `engage()`" mechanism it proposed is broken for
the realistic case, not merely underspecified.

**BLOCKING — the `engage()` mechanism §3 proposes cannot work for a real, multi-provider session
(found independently by TWO agents, security Finding 1 and correctness Finding 4).**
`ComposedContextProvider<Ms...>::engage()` (`composed_context_provider.hpp:129`) takes the **whole**
`std::tuple<Ms...>` at once, and can be called **exactly once** per instance (fails closed with
`already_engaged` on a second call). Worse: once engaged, the original typed `Ms` instances are
**unrecoverable** — they are type-erased into `shared_ptr<Ms>`-capturing closures
(`context_assembly.hpp:154-165`). So "construct a fresh provider bound to the new branch Ref and
`engage()` it" can only mean rebuilding and re-engaging **every** composed provider, not just the
worktree-bound one. For a session realistically composed of `SkillsProvider`/`MemoryProvider`/
`HistoryProvider<...>` alongside the worktree member — exactly what this draft's own §1 first
bullet invokes ("a background agent that inherits the FULL context of the main session") — this
means every OTHER provider's live accumulated state (memory write-back caches, skill usage
counters) is discarded and rebuilt from scratch on fork, directly contradicting the "inherits the
FULL context" premise the whole `fork_from()` scoping decision was based on. The
`test_composed_context_provider.cpp` P3b citation only proves reset-then-reengage-empty works, not
carry-forward-everything-but-one-member — it does not establish what this design needs.

**BLOCKING, I2 — `fork_from()`'s worktree branch has no capability intersection at all (security
Finding 2).** `agent_spawn_worktree.hpp`'s I2-1 fix exists specifically because an unconditional
branch-mode grant over the caller's whole tree is ambient authority (§52-58 of that file). §3's
`fork_from()` design does a bare `create_sub_worktree(...)` with no `CapabilitySet` parameter, no
intersection, no `cap::FsRead`/`cap::FsWrite` mention anywhere. Given `fork_from()` already does
NOT copy `capabilities_` (`agent_session.hpp:1168-1171`, an existing, deliberate gap a caller must
close with `set_capabilities()`), the likely real-world host pattern — handing the forked session
the parent's own `CapabilitySet` — combined with an un-intersected full-tree branch, could leave a
"handles one small request" mental model (this draft's own framing for `agent.spawn`) inverted: the
"full context" `fork_from()` case ends up with MORE exposure than the narrower `agent.spawn` case
that already has the safety fix.

**BLOCKING — no per-session worktree store exists anywhere; the draft never named where one comes
from (security Finding 3, sharpened by correctness Finding 3).** `create_sub_worktree()` needs an
`rt::AppendLogStore&`; `materialize_mount()`/`harvest_mount()` additionally need a
`WorktreeObjectStore&` AND a real `Mount{mount_id, ref_name, subtree_path}` plus an already-bound
capability grant (correctness Finding 2 — none of this is demonstrated buildable for a bare session
Ref). `AgentSession`'s real template signature (`agent_session.hpp:552-556`,
`<ChatClientT, StateT, HistoryProviderT>`) has no room for either store today. Option A's true cost
is therefore two new store dependencies, not one plain `Ref` member — and `grep -l "AgentSession<"`
returns **104 files**, a materially larger instantiation blast radius than §5's "touches
`AgentSession` itself" gestured at without quantifying.

**BLOCKING — materialize/harvest timing is genuinely irreconcilable with `SessionShellSandbox`'s
real design (correctness Finding 1).** `SessionShellSandbox` must never move after construction
(`MediatedShellRunner` holds a live reference into its own `fs_` member); `ExecState` persists
cwd/env across every `run_shell` call by explicit contract (the tool's own description promises
this). §2 item 4's "materialize/harvest around each call, or lazily on first use, then
incrementally" offers two options and checks neither against this constraint — re-pointing `fs_`
at a different materialized snapshot per call means destroying and rebuilding the whole sandbox,
which wipes `ExecState` and breaks the tool's own documented behavior.

**Real-but-smaller — an empirical claim in §1 was wrong (fabrication-hunt).** "Its only two real
callers today are `skill_mount_materializer.hpp` and `native_worktree_bridge.hpp`" is false:
`materialize_mount()` has exactly **one** real production caller (`skill_mount_materializer.hpp:77`);
`harvest_mount()` has **zero** real production callers anywhere. `native_worktree_bridge.hpp` only
*mentions* both functions in comments — its own header explicitly disclaims implementing
materialization, deferring it to "the provider layer," which doesn't exist yet either. §1's
directional point (this machinery sits unused by the session lifecycle) still holds; the specific
attribution did not.

**New fact, not previously known, refines §3's `agent.spawn` characterization (fabrication-hunt).**
`include/agentengine/rt/agent_spawn.hpp`'s `AgentSpawnToolProvider` already takes a
`caller_worktree_ref` as an explicit constructor parameter — real, production-shaped plumbing — but
is constructed **only** in `tests/test_rt_agent_spawn.cpp`, never in production. This makes §3's
"gets a real, session-scoped caller_ref for free" claim slightly more accurate than stated: the
wiring point already exists at the tool-provider level; what's missing is a real production
constructor call passing a session-bound Ref, not new plumbing.

**Escalated, not just "not analyzed" — `clear_in_process_state()`'s half-reset (correctness
Finding 5).** `clear_in_process_state()` (`agent_session.hpp:1210-1250`) resets
`history_provider_ = HistoryProviderT{}` unconditionally with zero mention of any Ref. Under Option
A, a plain `Ref session_ref_` member would silently survive this call while everything else resets
— the SAME SHAPE bug this codebase already found and fixed once before
(`fork_from()`'s own `principal_`-carried/`require_authority_`-not-carried gap, ADR-061 §21a
Finding 1, documented in `fork_from()`'s own comment at `agent_session.hpp:1165-1171`). This is a
known-recurring hazard class in this exact file, not a novel risk.

**Minor nit (fabrication-hunt).** §1's "grepped for `Ref`... no match anywhere in the file" is
loose as literally written — a bare substring grep for `Ref` DOES match unrelated identifiers
(`InteractionRef`, a `quark::ActorRef` comment). The intended, correct claim (no standalone `Ref`
*type* usage) holds under a word-boundary grep; only the phrasing overstated it.

**Verdict**: Revision 2 is not ready to red-team a second time as-is — §2 item 4 and §3's core
mechanism need a real design pass first, not just a "prove it compiles" pass, per correctness
Finding's own bottom line. Next step (not yet started): Revision 3, addressing at minimum (a) what
`fork_from()` actually does for a multi-provider session — likely NOT "engage() everything fresh,"
since that contradicts the "full context" premise outright; (b) a real capability-intersection step
for the worktree branch, matching `agent_spawn_worktree.hpp`'s own I2-1 precedent; (c) naming where
the worktree store(s) actually live and how `AgentSession` reaches them; (d) a materialize/harvest
timing model that doesn't fight `SessionShellSandbox`'s move/reference constraints.
