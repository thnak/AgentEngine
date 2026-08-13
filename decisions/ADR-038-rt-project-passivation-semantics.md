# ADR-038 — `rt::` Project-level passivation: what survives when there is no actor to evict

**Status:** Proposed (2026-08-13). Designed, implemented, and proven (real code + deterministic
tests, §4); awaiting the project owner's explicit "Judged" sign-off per this project's governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project owner is the ADR
judge).

**Relates to:** `decisions/ADR-037-remove-quark-as-core-runtime.md` (the umbrella "remove Quark"
initiative this ADR is one subsystem decision within — ADR-037 §3 names project-level passivation as
part of the coupling audit but does not itself design a replacement); `030-Project-Workspace-and-
Lifecycle.md` §4 (directed lifecycle) and §7 G1 (the scale/isolation promotion gate this ADR
reproves).

## 1. The question

`030-Project-Workspace-and-Lifecycle.md` §4 says "call `.passivate()` on every member session's
`ActorRef<AgentSession>` (and on the Project's own supervising actor, last), then mark the manifest
Paused." That sentence describes a mechanism — Quark actor eviction from memory, lazily reactivated
on the next ask — that has no counterpart once Quark is removed: `agentengine::rt::AgentSession` is a
plain, host-held C++ object with no runtime managing its residency.

**Stated so it has a wrong answer:** is there anything left worth building and calling "Pause" once
the literal eviction mechanism §4 describes is gone, or does removing Quark reduce 030 §4 to a bare
status-flip with no real behavior behind it?

## 2. The design

**Accepted answer: yes, something real survives, but it is honestly a narrower thing than the Quark
original, and this ADR keeps the two conceptually distinct rather than pretending one is a drop-in
replacement for the other.** Passivation always did two things at once: (a) evict the object from
memory, freeing the resource, and (b) flush its durable state first, so the eviction doesn't lose
work. Removing the actor engine removes (a) completely — there is no mechanism left in `rt::` land to
evict a plain C++ object a host still holds a reference to. What remains is (b), and it remains real:
`rt::ProjectSupervisor::checkpoint_members_and_workflows()` (`rt/project_supervisor.hpp`) calls
`save_agent_session_snapshot()`/`save_workflow_checkpoint()` on every registered member/workflow and
stops there. Whether a host then actually drops the in-memory object is the HOST's own decision, made
outside this type — the same "framework proposes, host disposes" split `rt::SessionStore`'s own
banner already establishes for storage generally.

`rt::pause_project()` (`rt/project_manifest.hpp`) layers 030 §4's status flip on top of that
checkpoint call, with one deliberate, real design choice the Quark original did not have the shape to
make: **the manifest's status only flips to `paused` if EVERY checkpoint succeeded**
(`CheckpointReport::all_ok()`). The Quark original's `pause_project` always flipped status
unconditionally — it had no error-reporting shape at all (`ActorRef<A>::passivate()` returns a bare
`bool`, "accepted" vs. "never resolved," not a per-member success/failure report). Marking a Project
"paused" while some member's durable state failed to flush would let an operator believe a resume
later restores from durable state that was never actually written — the same fail-closed discipline
this codebase already applies elsewhere (e.g. `delete_session`'s own two-outcome receipt).
`archive_project()` composes `pause_project()` and inherits the same rule.

`restore_project()` needs no I/O at all, in `rt::` land exactly as in the Quark original — a pure
status flip. The Quark original's own text already said "this does NOT need to eagerly reactivate
every member session"; in `rt::` land there is no lazy activation to rely on either, but the same
observation holds for a different reason: nothing in this codebase's `rt::` model auto-reactivates
anything on a schedule this function could hook into, so a status flip alone is honest either way.

`register_member`/`register_workflow` capture the live session/workflow and its store **by
reference** inside a `std::function<task<result<void>>()>` closure (`CheckpointHook`) — the type-
erasure mechanism that replaces `PassivatableHandle`'s `ActorRef<A>`-closure trick (030 §2 allows a
Project's members to differ in `ChatClientT`/`StateT`/`HistoryProviderT`, so `ProjectSupervisor`
cannot hold one concrete session type without contradicting that model). This is a **real, named
hazard the Quark original did not have**: `PassivatableHandle` held `ActorRef<A>` BY VALUE — a cheap,
location-independent handle safe to hold regardless of the actor's own lifecycle, because Quark's
activation table tracked the object's lifetime. `CheckpointHook` captures by reference because
`rt::` land has no such indirection; a `ProjectSupervisor` must never outlive the sessions/stores it
holds hooks to. This is the ordinary "don't outlive what you reference" C++ rule any raw-reference-
capturing closure already carries, not a new invariant — but it is worth stating explicitly because
the Quark original made this hazard structurally impossible, so a reader who assumes `rt::` is a
drop-in replacement would miss that the safety net changed shape.

## 3. 030 §7 G1's own promotion gate, reproven with a reframed claim

G1's original text bundles two claims: pausing one of N≥100 concurrently active Projects (a) has zero
effect on the other N-1's own correctness, and (b) shows no latency regression on a timed sample of
them — the latter specifically to catch an accidental global lock or scheduler-serialization point
creeping into `pause_project`'s implementation (the hazard ADR-034's own ActorId-scoped locking
existed to rule out).

Claim (a) still means something in `rt::` land and is reproven directly
(`tests/test_rt_project_scale_isolation.cpp`), strengthened from "nothing broke" to "provably nothing
was touched": each of the 100 Projects gets its own `InMemorySessionStore` instance, and pausing
Project #50 is checked to have called `save()` against ONLY store #50 — none of the other 99. All 99
then complete a real second `Run`, each continuing its own run-id sequence.

**Claim (b) is deliberately NOT reproven, and this is the one place this ADR overrides rather than
mechanically ports the original test.** `rt::AgentSession`/`rt::WorkflowSupervisor`/
`rt::ProjectSupervisor` are plain, host-held C++ objects with no actor engine, no shared scheduler,
and no shared lock of any kind reachable through this API — there is structurally nothing left for
`pause_project` to accidentally serialize on. A timing measurement here could never fail (no
mechanism exists that could make it fail), and 022 §5's own "a check that can't fail proves nothing"
rule says including one anyway would be theater, not proof. Claim (a)'s structural isolation check is
what actually stands in for claim (b) in `rt::` land: if `pause_project(#50)` genuinely never touches
store #7's `save()`, there is no path left by which it could have contended with Project #7 in the
first place — the hazard claim (b) was built to catch cannot arise by construction, not merely
"wasn't observed this run."

## 4. Falsifiable claims and verdicts

`tests/test_rt_project_supervisor.cpp` (Q1-Q5, checkpoint-orchestration primitive itself),
`tests/test_rt_project_manifest.cpp` (M1-M2 manifest round-trip, P1-P5 directed lifecycle + I4),
`tests/test_rt_project_scale_isolation.cpp` (G1, reframed per §3). Deterministic, offline, single-
threaded throughout — no sleeps, no real concurrency needed once there is no scheduler to race
against.

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| Q1 | `register_member()` type-erases across genuinely different `AgentSession<ChatClientT,StateT,HistoryProviderT>` instantiations into ONE `ProjectSupervisor` — the actual problem `PassivatableHandle` existed to solve, re-proven with no Quark actor involved. | `test_rt_project_supervisor.cpp` Q1: two members with different `HistoryProviderT` registered on one supervisor. | **CORRECT** |
| Q3 | A failing member's checkpoint is reported in `CheckpointReport` by index/kind WITHOUT aborting the remaining checkpoints. | `test_rt_project_supervisor.cpp` Q3: a middle member's store always fails; the members before and after it still checkpoint. | **CORRECT** |
| Q4 | A member session AND a workflow supervisor register and checkpoint together on the same `ProjectSupervisor` — 030 §8 Q4's own case. | `test_rt_project_supervisor.cpp` Q4. | **CORRECT** |
| Q5 | All member hooks run before any workflow hook (030 §4/§8 Q4's own ordering). | `test_rt_project_supervisor.cpp` Q5. | **CORRECT** |
| M1/M2 | `ProjectRecord` manifest round-trips byte-for-byte through save/load; a missing project_id loads `nullopt`, not an error; a second save overwrites (single-slot). | `test_rt_project_manifest.cpp` M1/M2. | **CORRECT** |
| P1/P4 | `pause_project()`/`archive_project()` checkpoint a healthy member and flip status ONLY then. | `test_rt_project_manifest.cpp` P1/P4. | **CORRECT** |
| P2/P5 | FAIL CLOSED: a failing member's checkpoint leaves status UNCHANGED (never falsely `paused`/`archived`). | `test_rt_project_manifest.cpp` P2/P5, using a store whose `save()` always fails. | **CORRECT** |
| P3 | `restore_project()` is a pure status flip — every other field byte-identical. | `test_rt_project_manifest.cpp` P3. | **CORRECT** |
| I4 | 030 §4's "the pause/restore cycle is invisible to the run" — reframed per §2: a checkpoint is read-only, so a real second `Run` issued right after `pause_project()` continues the SAME run-id sequence uninterrupted. | `test_rt_project_manifest.cpp` P1's own added check: `member.start_run()` after `pause_project()` continues `run:1 → run:2`. | **CORRECT** |
| G1(a) | Pausing one of 100 concurrently-registered Projects touches ONLY its own store — never any of the other 99's. | `test_rt_project_scale_isolation.cpp`: 100 independent `InMemorySessionStore` instances; after pausing #50, `exists()` is checked false on all 99 others. | **CORRECT** |
| G1(b) | All 99 other Projects complete a real second `Run`, each continuing its own run-id sequence uninterrupted by #50's pause. | `test_rt_project_scale_isolation.cpp`: `run:1 → run:2` verified for all 99. | **CORRECT** |

## 5. What this ADR does not claim

- **No eviction, no reactivation.** The Quark original's own "activation drops to Dormant, by
  census" claim (030 §7 G1, `test_project_lifecycle.cpp`'s own I2) has NO `rt::` equivalent and is
  not reproven here — there is no actor engine left to evict from or lazily reactivate. Named
  explicitly, not silently dropped: this is the real, structural narrowing §2 describes, not an
  oversight.
- **No latency/contention measurement for G1's claim (b)** — see §3 for why a check that cannot fail
  would be theater, not why the claim doesn't matter.
- **No sandbox-count claim.** Matching the Quark original's own scope limitation (`test_project_
  scale_isolation.cpp`'s own header note): no real sandbox allocation is wired into
  `AgentSession`/`WorkflowSupervisor` in this codebase yet, so "sandbox count drops to zero" would be
  vacuously true rather than a real measurement, in `rt::` land exactly as it was before.
- **No retention/GC mechanism for `archive_project()`** — matching the Quark original's own honest
  scope ("archived means hidden, not shrunk," 030 §8 Q2). This ADR's `archive_project()` is exactly
  the status flip, after an ordinary pause, inheriting pause's fail-closed rule.
- **The reference-lifetime hazard named in §2 is not type-system-enforced.** A `ProjectSupervisor`
  that outlives a registered session/store is a use-after-free a debug build would not catch
  structurally — named with the same prominence this codebase gives its other unenforced-but-
  documented invariants (matching ADR-024's own "enforced by comment, not the type system"
  precedent), not silently assumed safe.

## 6. Files changed (this pass; the underlying primitives were built across four earlier ADR-037
commits — `rt::ProjectRegistry`, `rt::ProjectSupervisor`, `rt::AppendLogStore`-backed archive,
`rt::ProjectRecord`/directed lifecycle — this ADR is the first to write up that already-implemented
design formally and close the remaining test gap)

- `tests/test_rt_project_manifest.cpp` — added the I4 check (a real second `Run` right after
  `pause_project()`) to `test_p1_pause_healthy()`.
- `tests/test_rt_project_scale_isolation.cpp` (new) — 030 §7 G1, reframed per §3.
- `tests/CMakeLists.txt` — registers the new test target; removes the two now-fully-superseded old
  Quark-actor targets below.
- `include/agentengine/project/lifecycle.hpp`, `include/agentengine/project/project.hpp` (deleted) —
  zero remaining real consumers once `tests/test_project_lifecycle.cpp`/`tests/test_project_scale_
  isolation.cpp` (deleted, fully superseded) were their only callers; confirmed by a repo-wide grep
  for real `#include` directives (not just comment mentions) before deletion.

Full regression suite: 202/202 (203 minus the two deleted Quark-actor tests, plus the one new `rt::`
test — a net -1 target count, full claim coverage preserved or honestly narrowed per §5).
