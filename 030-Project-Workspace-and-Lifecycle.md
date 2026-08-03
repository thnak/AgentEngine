# 030 — Project: Workspace Grouping and Directed Lifecycle

**Status:** Draft · **Depends on:** 001, 005, 007, 014, 020, 025, Quark 012/ADR-034 · **Gate:** §7

## Goal

Give a host application a durable, addressable unit *above* a single session — a **Project** — that
groups a root session and every session it transitively owns, with directed pause/resume distinct
from Quark's idle-triggered passivation, so a host can present "the user's open workspaces" as a
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
  members: [
    { session_id, parent_session_id?, role, spawned_at, worktree_ref, status }
    ...
  ],
  status: Active | Paused | Archived,
  created_at, updated_at, last_active_at,
  title, host_metadata,    // opaque to the engine; a WinUI host's tab title/icon lives here
}
```

- **`members` is the missing index.** 001 §4 links a sub-agent's *run* to its parent via
  `parent_run_id`; nothing links the *sessions*. A Project's `members` list is that index, built
  incrementally as sub-agent sessions are created under it — not derived by walking every member's
  full run history at load time, which would mean scanning N session logs just to answer "what's in
  this project."
- **Each member session keeps its own independent worktree.** 025's "one worktree, one principal, one
  session" (025 §3) is unbroken by this RFC. This is the resolution to 025 §10 Q5 ("cross-session
  worktree sharing... breaks the simplicity that makes §5 sound"): don't share one mutable tree across
  sessions — index N independent worktree refs at the Project layer instead. A "what changed across
  the whole workspace" view is a diff over N tree digests (each already cheap per 025 §2), never a
  merge of N trees into one, so 025 §4's merge machinery is not reused or stressed by this RFC.

## 3. Persistence: a new record, the same seam

005 §2's rule holds without exception here: **AgentEngine adds no storage engine of its own.** A
Project's manifest is a new *record type*, stored through Quark 012's `Store` seam exactly like
`AgentSession` is (005 §2) — keyed by `project_id` instead of `session_id`. It needs none of
`AgentSession`'s two-mode complexity: a manifest changes rarely (a member added, a status flip, a
title edit) and stays small, so **it is always snapshot-mode**, never event-sourced — there is no
"Project history" to replay, only current membership and status. This is the "new file structure"
in the concrete sense: a new schema on the existing seam, not a new persistence engine.

## 4. Directed lifecycle: pause is not idle eviction

005 §1's passivation is automatic and idle-triggered — an actor kept busy (a session hit by recurring
reminders, say) never idles out, and there is today no caller-facing way to say "tear this down now"
at the AgentEngine layer. Quark already solved exactly this at the actor layer:
**`ActorRef<A>::passivate()` (Quark ADR-034, Accepted)** is a fire-and-forget, on-demand teardown that
reuses the identical deactivation path the idle wheel uses, flushes the actor's latest state through
the same `Store::save_snapshot` seam a `declare_lazy<A>(store,...)` actor already gets on automatic
eviction, and is proven race-free against a concurrent idle-triggered passivation (two triggers
converge on exactly one retirement — `engine_passivate_test`, ADR-034 evidence table).

`pause_project(project_id)` is built from this primitive directly, not reinvented:

- **Pause** — call `.passivate()` on every member session's `ActorRef<AgentSession>` (and on the
  Project's own supervising actor, last), then mark the manifest `Paused`. Each passivate's
  persistence flush **is** the save — there is no separate "save" step to design, because 001 §3
  already made every turn boundary a checkpoint and ADR-034's flush persists exactly that latest
  checkpoint. A paused Project holds zero session activations and zero sandboxes: 008 §6a already
  guarantees the latter per session, and pausing every member session inherits it project-wide.
- **Restore** — read the manifest back by `project_id`. This does **not** need to eagerly reactivate
  every member session: 005 §1's "activated on demand" already covers that — the moment a host issues
  a Run against any member, Quark's lazy activation (`declare_lazy<A>`) brings that one session back
  from its last flushed snapshot. Restore only repopulates the host's view (member list, titles,
  worktree refs) cheaply, without paying to reactivate N sessions the user isn't looking at yet.
- **Resume / continue** — the ordinary case: a host starts a new Run against a member session
  (usually root) exactly as if the project had never paused. The pause/restore cycle is invisible to
  the run itself, same as any idle-then-reactivated session today.
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
(principal-scoped manifest query) — each an ordinary `ask` against a small Project-registry actor, not
a new protocol surface. A WinUI tab closing calls `pause_project`; reopening calls `restore_project`
then drives Runs against the member sessions exactly as 020 §3a already describes.

## 7. Promotion gate

- **G1** — N concurrently active Projects (N ≥ 100, host-configured; no engine-side cap observed at
  any N tested): pausing one measurably drops its member sessions' activation count to zero and its
  sandbox count to zero, with zero observable effect (latency, event ordering) on the other N-1.
- **G2** — a Project paused, then restored after a full process restart, presents an identical member
  list and identical latest worktree refs to the pre-pause state; a Run issued against any member
  session after restore produces output identical to the same Run issued without ever having paused
  (extends 001 §9 G2's single-session claim to the whole member set).
- **G3** — `list_projects` for principal P never returns a Project belonging to another principal,
  under concurrent create/pause/restore from multiple principals (007's cross-principal denial,
  exercised at this layer).
- **G4** — the manifest snapshot write is a bounded, small operation (023 budget) independent of
  member-session count growth up to the tested N — adding a member session never triggers a full
  manifest rewrite proportional to history.

## 8. Open questions

- **Q1** — Should `members` be capped per Project, or does truly unbounded sub-agent fan-out (an
  agent that spawns hundreds of short-lived helper sessions over a project's lifetime) need the
  manifest format to handle a rolling/archived-members tail before it becomes a 023 budget problem?
- **Q2** — Whether `archive_project` should trigger worktree GC (025 §6) immediately or only once
  retention policy independently reclaims it — archiving is a lifecycle state change, and reclaiming
  storage eagerly on archive could surprise a host that expects "archived" to mean "hidden," not
  "shrunk."
- **Q3** — Whether a Project needs its own principal-scoped capability set distinct from the union of
  its member sessions'. 007's "never a superset" rule for sub-agent capability inheritance (001 §4)
  should already forbid a sub-agent session reaching a capability the root never had, but this RFC
  introduces a new record referencing multiple sessions and deserves an explicit check that it isn't
  a second, weaker path to the same authority — not assumed safe by analogy.
- **Q4** — Relationship to 014's workflow checkpoint/resume: when a workflow executor is itself an
  agent running inside a Project's root session, does pausing the Project need to reach into the
  workflow's own checkpoint store, or is "the session is paused" already sufficient because the
  workflow's supervising actor is itself session-scoped? Needs checking against 014 §5's actual actor
  placement, not assumed.
