# 030 — Project: Workspace Grouping and Directed Lifecycle

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 005, 007, 014, 020, 025 (historical: originally also Quark 012/ADR-034 — ADR-037 removed Quark as a dependency; `decisions/ADR-038-rt-project-passivation-semantics.md` re-derives this RFC's own passivation mechanism against `agentengine::rt::`) · **Gate:** §7

## Goal

Give a host application a durable, addressable unit *above* a single session — a **Project** — that
groups a root session and every session it transitively owns, with directed pause/resume distinct
from idle-triggered passivation (historical: originally Quark's idle-triggered passivation; ADR-037
removed Quark, and `agentengine::rt::` has no host-managed idle eviction at all — see
`decisions/ADR-038-rt-project-passivation-semantics.md`), so a host can present "the user's open
workspaces" as a
first-class, unboundedly-many, independently resumable list. 020 §3a's embedded case (WinUI tabs) is
the motivating one, but the concept is host-shape-agnostic.

**This is new ground, not a port.** Checked directly against MAF (`agent_framework_devui/
_conversations.py` persists conversations per-thread; `_workflows/_checkpoint.py` checkpoints a
workflow per-run) and corroborated by direct operating experience: MAF persists chat history and
per-workflow checkpoints, and nothing above a single thread. There is no MAF vocabulary to adopt here
— 027 §1's naming rule ("adopt MAF's name wherever the concept is the same") has nothing to adopt
from, so `Project` is coined here rather than reused with different semantics from something else.

## 1. What a Project is, and isn't

- **A Project is an index, not a new execution unit.** It owns no turn loop, no history of its own,
  no model calls. Its only job is to durably answer "which sessions make up this workspace, and what
  state are they in" and to mediate directed lifecycle verbs across them.
- **It is not a session.** A session (005) is a conversation with one principal and one history. A
  Project may contain many — a root session plus every sub-agent session spawned under it (001 §4's
  `parent_run_id` chains) over the workspace's whole lifetime.
- **It is not a workflow (014).** A workflow is an executor graph *inside* one run, checkpointed at
  superstep boundaries. A Project is a container *above* many runs and potentially many workflows,
  over days or weeks, not one turn loop.

## 2. Model

```
Project = {
  project_id,             // stable handle; independent of any session_id, survives session churn
  principal,               // 007; a project belongs to exactly one, like a session does
  root_session_id,
  active_members: [
    { session_id, parent_session_id?, role, spawned_at, worktree_ref, status }
    ...
  ],
  archived_members: [
    { session_id, parent_session_id?, role, spawned_at, worktree_ref, status }
    ...
  ],
  status: Active | Paused | Archived,
  created_at, updated_at, last_active_at,
  title, host_metadata,    // opaque to the engine; a WinUI host's tab title/icon lives here
}
```

- **`active_members` / `archived_members` is the missing index, split in two.** 001 §4 links a
  sub-agent's *run* to its parent via `parent_run_id`; nothing links the *sessions*. A Project's
  member list is that index, built incrementally as sub-agent sessions are created under it — not
  derived by walking every member's full run history at load time, which would mean scanning N
  session logs just to answer "what's in this project." A member starts in `active_members` and moves
  to `archived_members` once its role completes (its parent's run finishes, or ordinary session
  retention, 005 §1/025 §6) — the split §8 Q1 resolves, carried into the normative model rather than
  left implicit.
- **Each member session keeps its own independent worktree.** 025's "one worktree, one principal, one
  session" (025 §3) is unbroken by this RFC. This is the resolution to 025 §10 Q5 ("cross-session
  worktree sharing... breaks the simplicity that makes §5 sound"): don't share one mutable tree across
  sessions — index N independent worktree refs at the Project layer instead. A "what changed across
  the whole workspace" view is a diff over N tree digests (each already cheap per 025 §2), never a
  merge of N trees into one, so 025 §4's merge machinery is not reused or stressed by this RFC.

## 3. Persistence: a new record, the same seam

005 §2's rule holds without exception here: **AgentEngine adds no storage engine of its own.** A
Project's manifest is a new *record type*, stored through `agentengine::rt::SessionStore` exactly
like `AgentSession` is (005 §2) — keyed by `project_id` instead of `session_id` (historical:
originally Quark 012's `Store` seam; ADR-037 replaced the backend, the contract is unchanged). It
needs none of
`AgentSession`'s two-mode complexity: a manifest changes rarely (a member added, a status flip, a
title edit) and its active-member list stays small, so **it is always snapshot-mode**, never
event-sourced — there is no "Project history" to replay, only current membership (active and
archived) and status. The archived tail can grow unboundedly over a Project's long lifetime (§8 Q1);
§7 G4 is the check that this growth never costs a write proportional to history. This is the "new
file structure" in the concrete sense: a new schema on the existing seam, not a new persistence
engine.

## 4. Directed lifecycle: pause is not idle eviction

**Historical note (ADR-037/ADR-038):** this section originally described directed pause as built
directly on Quark's own actor-layer passivation primitive. ADR-037 removed Quark as a dependency
entirely, and `decisions/ADR-038-rt-project-passivation-semantics.md` re-derived what survives: a
plain `rt::AgentSession` has no runtime managing its residency, so eviction-from-memory (the idle
wheel, `ActorRef<A>::passivate()`'s reuse of that path) is gone completely — there is no host-managed
passivation/reactivation across process restarts or node loss in `rt::` land at all. What survives is
the OTHER half passivation always did — flush durable state first — as
`rt::ProjectSupervisor::checkpoint_members_and_workflows()`; `rt::pause_project()`/`archive_project()`
flip the manifest's status ONLY if every checkpoint succeeds, a real, deliberate fail-closed choice
the Quark original's bare-`bool` `passivate()` had no shape to make. The original text below is kept
for its still-relevant framing of the PROBLEM (005 §1's passivation is automatic/idle-triggered, with
no caller-facing directed teardown), even though the described SOLUTION mechanism is superseded.

005 §1's passivation is automatic and idle-triggered — a session kept busy (hit by recurring
reminders, say) never idles out, and there is today no caller-facing way to say "tear this down now"
at the AgentEngine layer. (Historical: Quark used to solve exactly this at the actor layer via
`ActorRef<A>::passivate()`, Quark ADR-034 — a fire-and-forget, on-demand teardown reusing the
identical deactivation path the idle wheel used, flushing the actor's latest state through the same
`Store::save_snapshot` seam a `declare_lazy<A>(store,...)` actor already got on automatic eviction,
proven race-free against a concurrent idle-triggered passivation. ADR-037/ADR-038 replaced this: see
the note above.)

`pause_project(project_id)` is built from this primitive directly, not reinvented:

- **Pause** — `rt::pause_project()` (ADR-038) calls
  `rt::ProjectSupervisor::checkpoint_members_and_workflows()` on every member session (and the
  Project's own workflow supervisors), then flips the manifest to `Paused` ONLY if every checkpoint
  succeeds — a real, deliberate fail-closed choice (historical: originally `.passivate()` on every
  member session's `ActorRef<AgentSession>`, Quark ADR-034, whose bare-`bool` return had no shape to
  make this fail-closed distinction; ADR-037 removed the actor-eviction half of passivation entirely
  — a plain `rt::AgentSession` has no runtime managing its residency — leaving only the durable-flush
  half, which is what `checkpoint_members_and_workflows()` reproduces). A paused Project's sessions
  hold zero sandboxes (008 §6a); there is no "session activation" left to hold zero of, since there
  is no actor runtime managing activations at all post-ADR-037.
- **Pause and workflow-supervising code.** §8 Q4's extension applies to `WorkflowSupervisor` (014)
  instances hosted under a member session the same way, via the same `checkpoint_members_and_
  workflows()` call (historical: originally `.passivate()` on a workflow-supervising actor).
- **Restore** — read the manifest back by `project_id`. This does **not** need to eagerly reconstruct
  every member session: 005 §1's "activated on demand" intent is retained as host-side "construct
  the `AgentSession` object lazily, from its last flushed checkpoint, only when a Run is actually
  issued against it" (historical: this used to be Quark's automatic `declare_lazy<A>` lazy
  activation; `rt::` has no equivalent runtime-managed laziness, so the host must implement this
  construct-on-demand behavior itself now — a named, permanent narrowing from a structural to a
  host-implemented property). Restore only repopulates the host's view (member list, titles,
  worktree refs) cheaply, without paying to reconstruct N sessions the user isn't looking at yet.
- **Resume / continue** — the ordinary case: a host starts a new Run against a member session
  (usually root) exactly as if the project had never paused. The pause/restore cycle is invisible to
  the run itself, same as any lazily-reconstructed session today.
- **Archive** — pause, then apply 005 §6 / 025 §6 retention policy; distinct from **delete** (005 §6),
  which is a data-subject-request-grade hard removal, not a lifecycle state.

## 5. Unbounded concurrency is inherited, not new

Nothing above introduces a cap. 001 §4's "across sessions — fully parallel" and 020 §7 G5's
N-concurrent-runs gate already establish that concurrency is a capacity question (023 budgets), never
an artificial limit — a Project is just an index over sessions that already had this property. **The
only new claim this RFC makes is that pausing one Project must not perturb any other**: pausing
project A's N member sessions touches only those N `ActorRef`s; a concurrently running project B is
untouched by construction, because `.passivate()` (ADR-034) is scoped to one `ActorRef` at a time with
no shared lock.

## 6. Host surface (020 §3a)

For the embedded-host case that motivated this: `EmbeddedHost` gains four calls — `create_project`,
`pause_project`, `restore_project` (→ manifest only, no activation), `list_projects`
(principal-scoped manifest query). `create_project` and `list_projects` are ordinary `ask`s against a
small Project-registry actor — the shared index a new Project needs to register into, and a
principal-scoped list needs to query. `pause_project` and `restore_project` do **not** route through
that registry actor: each acts directly on the named Project's own supervising actor (§4), addressed
by `project_id`, exactly as §4's `.passivate()` sequence already assumes. Pausing a Project is
therefore dispatch to that one actor, not a call serialized through an actor shared by every Project —
which is what makes §7 G1's "zero observable effect on the other N-1" hold. None of this is a new
protocol surface. A WinUI tab closing calls `pause_project`; reopening calls `restore_project` then
drives Runs against the member sessions exactly as 020 §3a already describes.

## 7. Promotion gate

- **G1** — N concurrently active Projects (N ≥ 100, host-configured; no engine-side cap observed at
  any N tested): pausing one measurably drops its member sessions' activation count to zero and its
  sandbox count to zero, and any workflow-supervising actor(s) hosted under those sessions also drop
  to zero activations, with zero observable effect (latency, event ordering) on the other N-1.
- **G2** — a Project paused, then restored after a full process restart, presents an identical member
  list and identical latest worktree refs to the pre-pause state; a Run issued against any member
  session after restore produces output identical to the same Run issued without ever having paused
  (extends 001 §9 G2's single-session claim to the whole member set).
- **G3** — `list_projects` for principal P never returns a Project belonging to another principal,
  under concurrent create/pause/restore from multiple principals (007's cross-principal denial,
  exercised at this layer).
- **G4** — the manifest snapshot write is a bounded, small operation (023 budget) tested against two
  independent growth axes, each up to the tested N: active-member-count growth and archived-tail size
  growth (§2, §8 Q1). Adding a member session never triggers a full manifest rewrite proportional to
  active-member count, and a member session moving to the archived tail never triggers a full
  manifest rewrite proportional to archived-tail size.

## 8. Open questions

- ~~**Q1** — Should `members` be capped per Project, or does truly unbounded sub-agent fan-out need
  the manifest format to handle a rolling/archived-members tail before it becomes a 023 budget
  problem?~~ **Resolved, No cap, but a rolling/archived tail is needed (2026-08-04):** a hard cap
  would be an artificial limit, contradicting §5's own "concurrency is a capacity question, never an
  artificial limit" principle extended to member count. But §3's "always snapshot-mode... stays
  small" assumption genuinely breaks for a project with hundreds of short-lived helper sessions over
  its lifetime — §7 G4's own gate ("up to the tested N") already implicitly flags this without
  stating the fix. Fix: `members` splits into an active list (what §3's manifest already assumes,
  kept small) and an archived tail a short-lived helper session moves to once its role completes (its
  parent's run finishes, or ordinary session retention, 005 §1/025 §6) — the same distinction 019 §5
  already draws between different retention lifetimes for different record kinds, applied here to one
  record's internal structure. G4 is extended to test that manifest write cost stays bounded as the
  archived tail grows, not just active-member count.
- ~~**Q2** — Whether `archive_project` should trigger worktree GC immediately or only once retention
  policy independently reclaims it.~~ **Resolved, No, retention policy reclaims independently, never
  eagerly on archive (2026-08-04):** consistent with every other retention/GC mechanism in this
  project — 025 §6's GC runs "by policy," decoupled from any particular state transition; 005 §6's
  redaction/deletion is explicit and separate, never an automatic side-effect of an unrelated
  lifecycle change. Making archive trigger immediate reclaim would be an exception to that pattern
  for no stated benefit, and would violate the expectation the question itself correctly names as
  right: "archived" means "hidden," not "shrunk." §4's existing wording already implied this; stated
  explicitly now so it isn't re-litigated as ambiguous.
- ~~**Q3** — Whether a Project needs its own principal-scoped capability set distinct from the union
  of its member sessions'.~~ **Resolved, No — forced by §1, not assumed by analogy (2026-08-04):**
  §1 states plainly a Project "owns no turn loop, no history of its own, no model calls" — it never
  itself produces an effect, so there is nothing for a capability set to gate at the Project level
  beyond the ordinary principal-scoping §2's struct already has. Project-level verbs (pause/restore/
  archive/create/list) are ordinary effects authorized against the calling principal's existing
  capability set through the same pipeline (007 §5, 018 §2) every other effect goes through — there
  is no second authority path, because `members` is just data (session ids, worktree refs); reaching
  what a member session can *do* still requires going through that session's own capability set
  independently, governed by 007's attenuation-only rule unchanged. §7 G3 (already specified) *is*
  the explicit check this question asked for, not an assumption by analogy — it proves the no-bypass
  property rather than assuming it.
- ~~**Q4** — Relationship to 014's workflow checkpoint/resume: does pausing the Project need to reach
  into the workflow's own checkpoint store, or is "the session is paused" already sufficient?~~
  **Resolved, Yes, pause must explicitly reach a workflow's own supervising code too — extending
  §4's same primitive, not assuming session-pause covers it (2026-08-04):** 001 §1's runtime-mapping
  table lists "Workflow" and "AgentSession" as distinct rows (a `WorkflowSupervisor` owning a graph
  of executors versus `AgentSession`'s own plain instance) — a workflow running inside a session is,
  per that table, its own distinct object, not the session's own instance wearing a different hat.
  The conservative answer, given the question's own genuine uncertainty: `pause_project` (§4) is
  extended to call `checkpoint_members_and_workflows()` against any `WorkflowSupervisor` instance(s)
  hosted under a member session, using the identical mechanism already built on, not a second one
  (historical: originally "its own Quark actor," extended via `.passivate()` and the identical
  ADR-034 primitive, before ADR-037/ADR-038 replaced the underlying mechanism — the "extend the same
  primitive to workflows too" decision itself is unaffected). Assuming session-pause transitively
  covers a separately-addressed workflow risks leaving it — and transitively its own checkpoint
  store — live after a Project reports itself `Paused`, contradicting §4's own claim that a paused
  Project holds zero sandboxes. §7 G1's gate is what makes this claim actually hold, provided pause's
  iteration covers workflow supervisors explicitly rather than by assumption.
