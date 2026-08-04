# Research record — MAF orchestration patterns: Magentic vs. Group Chat

**Compiled:** 2026-08-04 · **Source revision:** Microsoft Agent Framework docs, `ms.date: 2026-05-27`,
last updated `2026-07-10` · **Status:** dated snapshot

Grounds 014 §3's pattern table and the resolution of 014 Q5 (whether Magentic needs its own row).
Primary source: [Microsoft Learn — Agent Framework Workflows Orchestrations: Magentic](https://learn.microsoft.com/en-us/agent-framework/workflows/orchestrations/magentic).

## 1. What Magentic actually is

Magentic orchestration is MAF's implementation of the **Magentic-One** design (originally an AutoGen
system). Microsoft's own docs state it plainly: *"The Magentic orchestration has the same architecture
as the Group Chat orchestration pattern, with a very powerful manager that uses planning to coordinate
agent collaboration. If your scenario requires simpler coordination without complex planning, consider
using the Group Chat pattern instead."*

So the **graph shape is identical** to Group Chat (a manager/moderator cycling among participants).
What differs is what the manager *does*:

- **Task ledger.** The manager produces an initial plan (`MagenticPlanCreatedEvent`) before any
  participant acts, tracked as a `FullTaskLedger`.
- **Progress ledger, updated every round** (`MagenticProgressLedgerUpdatedEvent`): tracks
  `IsRequestSatisfied`, `IsInLoop`, `IsProgressBeingMade`, and picks `NextSpeaker` +
  `InstructionOrQuestion` — the manager decides completion and routing, not a fixed script.
- **Stall detection → autonomous replan.** Consecutive non-progressing rounds increment a stall
  counter (`max_stall_count`/`WithMaxStalls`); exceeding it triggers an automatic reset and replan
  (`MagenticReplannedEvent`) — the manager corrects course on its own, mid-task, without a human
  re-prompting it.
- **Bounds exist but are safety valves, not the control mechanism.** `max_round_count`,
  `max_stall_count`, `max_reset_count` cap runaway execution; they do not define *when* the task is
  done — the manager's progress-ledger assessment does.
- **Optional human plan review** (`RequirePlanSignoff`/`enable_plan_review`) is opt-in in Python
  (default off) and opt-in-by-default in .NET — a human can approve or revise the plan before
  execution, but the design point is that this is optional, not load-bearing.

## 2. What this means for "send it a goal and it finishes"

The defining property is not the graph shape (Group Chat already has that) — it's that the manager
**owns its own definition of done and its own recovery from getting stuck**, autonomously, within
caller-set safety bounds. A caller hands Magentic a task string, not a fixed sequence of rounds or a
hand-authored routing table; the manager decides both the routing *and* when to stop. That is a
materially different authoring experience from "a moderator executor cycling among participants with a
bound," where the bound is typically the primary termination contract a caller reasons about.

## 3. Implication for 014

Nothing here requires a new graph primitive — cyclic graphs with a mandatory bound (014 §2) and a
request port for optional human review (014 §4) already cover every mechanism above; the ledger and
stall/replan logic are manager-executor business logic, consistent with §3's stated philosophy that
patterns are graph configurations, not separate subsystems. What the graph-shape argument alone misses
is that Magentic is the recognizable, MAF-native name for a *specific, reusable* configuration —
autonomous ledger-driven routing with self-triggered replanning — and 027 §1's naming rule ("adopt
MAF's name wherever the concept is the same... give a distinct name only where it genuinely differs")
argues for naming it, not folding it silently into Group chat/debate's row.
