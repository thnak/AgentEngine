# `Backgroundable`/`StandingEffect` (006 §6b, 019 §2) — residual status

**Status:** Pre-milestone scoping note, not a stage-4 work breakdown. [The review-signoff
workflow](v1-review-signoff-workflow.md) §4 gates a work-breakdown-and-kickoff doc on "an RFC/cluster
reaches Reviewed for the milestone about to start" — this pairing has no owning milestone yet (see
below), so writing that doc now would be premature per the project's own process. This note exists so
the gap is tracked in one place instead of scattered across three milestones' breakdown docs, ready to
seed a real stage-4 breakdown once a milestone claims it.

**RFCs:** 006 (Tool and Function Plane) §6b, 019 (Durability and Long-Running Agents) §2. Both
**Reviewed** (2026-08-05) — the design is not in question; only the build order is unresolved.

## Where this was deferred

- **M2** (`milestone-2-tools-capabilities-sandbox-breakdown.md`) — built 006, but narrower than the
  RFC's full text; §6b was out of scope.
- **M4** (`milestone-4-sessions-durability-memory-breakdown.md`, decision 4) — 019 §2's six-row
  wake-condition table was broken down; only two rows ("Timer/schedule", "Human/caller input") were
  buildable inside M4's own RFC scope. Verbatim: *"'Local background task completion' needs
  `Backgroundable`/`StandingEffect` (006 §6b), which — confirmed by direct inspection — was never
  built even though 006 itself has been real since M2... Named as a real, not hypothetical, gap;
  deferred to whichever milestone builds its owning RFC."*
- **M5** (`milestone-5-providers-identity-secrets-breakdown.md`) — restates the same "confirmed never
  built" finding as a residual blocking 004 §8 Q1's batch-API resolution (below); doesn't claim it
  either.
- **M6** (`milestone-6-multi-agent-orchestration-breakdown.md`, in progress) — owns 014/030, not
  006/019; does not mention this gap.

## What's spec'd vs. what's built

The RFC text is fully resolved (both sections dated 2026-08-04 as closed design questions), and it's
detailed enough to implement directly — nothing here is an open design question:

- **006 §6b.** A tool declares `Backgroundable` (§5's sibling to `Parallelizable`) — "safe to detach
  from the turn that called it." Invoked via `background_task(tool, args)`, gated by
  `Background<max_concurrent>` (007 §3). Still runs the full 10-step tool pipeline (§3); only step 8
  (invoke) stops blocking the turn on completion. Completion delivers as `ToolCallFinished` on the
  run's event stream (013 §1) plus a new 019 §2 wake condition. One handle shape for three producers —
  `schedule_wakeup`, `watch_resource`, and `background_task` all return a `StandingEffect` handle
  (unforgeable, like a capability handle, 007 §3.4), with one introspection/kill surface:
  `list_standing_effects()` / `cancel_standing_effect(handle)`. This also resolves 010's open question
  about whether backgrounding is permitted at all (gated by `Background`, accounted under the same
  usage/duration/bytes tracking as any tool call, §3 step 10).
- **019 §2.** The wake-condition table has six rows; two are wired (Timer/schedule via Quark's durable
  reminders; Human/caller input via `Interaction`/`InputRequired`, both built in M4). "Local background
  task completion" is the row this gap blocks. ("External event" and "Remote task completion" are a
  separate, also-unbuilt residual — they need 012/A2A, M7's RFC, not this one.)
- **Proof obligations already named**, not yet attempted: **G6** (a run that calls `schedule_wakeup`/
  `watch_resource` is fully `Suspended` — no activation, sandbox, connection, or thread — until its
  wake condition fires; measured, not asserted), **G7** (`background_task` doesn't block the calling
  turn; `ToolCallFinished` survives a suspend/resume of the run in between), **G8** (cross-run/
  cross-principal `cancel_standing_effect` fails, proven both same- and cross-principal), **G9** (a
  session already at its `Background<max_concurrent>` cap has its next `background_task` rejected at
  pipeline step 4/authorize, never silently queued or throttled elsewhere).

Code: verified by direct grep of `include/` — **zero implementation.** No `Backgroundable`,
`StandingEffect`, `background_task`, `list_standing_effects`, or `cancel_standing_effect` symbol exists
anywhere. The only adjacent artifact is `TimerWake` (`include/agentengine/core/agent_session.hpp:137`),
which proves a Quark durable reminder can reach a session actor's real `session_actor_id()` end to end
— its own comment is explicit that it stops there: *"what a real 'resume the paused run this timer was
arming' would DO needs 006 §6b's `schedule_wakeup`/`Backgroundable`, confirmed absent from this
codebase... a different, un-built vertical this task does not invent standing in for."*

## Downstream work already blocked on this

- **004 §8 Q1** (batch API): resolved to ride this exact mechanism — a vendor batch call (submit now,
  complete later, poll or webhook) is treated as `Backgroundable`, completed via 019 §2's wake table,
  not a bespoke batch-tracking structure. `ChatClientCapabilities::batch`
  (`include/agentengine/core/chat_client.hpp:52`) exists as a declared capability bit for exactly this,
  currently `false` everywhere because nothing sets it.
- **019 §8 Q2**: MCP's tasks extension (a single long-running tool call, distinct from a whole-run
  pause) maps onto `Backgroundable`/`StandingEffect` — `tasks/get` polling is meant to be served from
  the same durable handle `list_standing_effects` exposes, not a second tracking structure. Blocked on
  the same gap.

## Owning milestone

**Assigned to Milestone 7** (`v1-implementation-roadmap.md`, 2026-08-08). Rationale: 019 §8 Q2 already
resolved that MCP's tasks extension maps onto this exact mechanism, and A2A's task lifecycle is the
real-world exerciser for 019 §2's two remaining un-wired wake-condition rows ("External event",
"Remote task completion" — both need 012, M7's RFC). Building 011/012 (M7's own RFCs) against a stub
`Suspended` state would make their own promotion gates dishonest — an A2A MUST-level task-completion
case or an MCP `tasks/get` poll can't pass against a mechanism that isn't real. M7 closes 006 §6b first,
then builds 011/012 against it, finishing 019 §2's six-row wake-condition table completely (the other
two rows — Timer/schedule, Human/caller input — shipped in M4).

This resolves the open question above; a real stage-4 work-breakdown-and-kickoff doc for M7 is still
owed just-in-time, per `v1-review-signoff-workflow.md` §4, immediately before M7 starts (M6 is still in
progress) — this note remains the scoping input for that doc, not a substitute for it.
