# ADR-053 — `schedule_wakeup`, the third real `StandingEffect` producer

**Status:** Proposed (2026-08-14). Designed (inherited from
`docs/planning/schedule-wakeup-standing-effect-design-draft.md`'s own already-red-teamed sketch, this
ADR's §2 corrects one part of it against real code), implemented, and proven (real code + tests, §4);
awaiting the project owner's explicit "Judged" sign-off per this project's governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11 — only the project owner marks an ADR "Judged").

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #7 (the finding this ADR
closes). `docs/planning/schedule-wakeup-standing-effect-design-draft.md` (the design this implements,
with one correction — see §2). `006-Tool-and-Function-Plane.md` §6b / `019-Durability-and-Long-Running-
Agents.md` §2 (`schedule_wakeup`'s own normative definition — the "Timer/schedule" wake-condition
table row). `007-Capability-and-Trust-Model.md` §3 (the `Schedule<max_horizon, max_active>` capability
row this ADR wires up, rather than inventing a new mechanism). `include/agentengine/core/
standing_effect.hpp`, `include/agentengine/rt/agent_session.hpp` (its "SLICE 4 ADDITION" banner
paragraph carries the same design writeup as this ADR, in the file the reader is actually looking at).

## 1. The question

**Stated so it has a wrong answer:** does `rt::AgentSession` — the real, current runtime every other
gap-audit closure this project has landed targets — have a working `StandingEffect` producer for 019
§2's "Timer/schedule" row, the way it already does for "Local background task completion"?

**Before this ADR: no.** `standing_effect_kind::schedule_wakeup` existed as an enumerator (`standing_
effect.hpp`) with zero producer anywhere. The design draft's own re-grounding (2026-08-14) found this
gap bigger than the audit's own one-line framing assumed: ADR-037 removed Quark's `ReminderService`
entirely, and `rt::` has never had ANY timer/delay/scheduled-callback primitive — not "wire an existing
mechanism into the StandingEffect handle shape," but "there is no mechanism to wire."

## 2. A correction to the design draft, found while implementing it

The design draft's own §3(b) proposed a **new CRTP policy tag**, `Schedule<max_horizon_ms>`, "matching
`MaxTurns<N>`/`TokenBudget<N>`'s existing idiom" — written before this ADR's implementation pass touched
real code. Re-grounding that specific proposal against `007-Capability-and-Trust-Model.md` §3's own
table found it already wrong: **`Schedule<max_horizon, max_active>` is already a normatively-named
CAPABILITY** (007 §3's own row, same table `Background<max_concurrent>` — the mechanism `start_
background_task()` already enforces — comes from), not a bare agent-level policy tag. Checking the code
directly confirmed the ENTIRE capability-side mechanism already exists and was simply never wired to a
real producer:

- `cap::Schedule{max_horizon (seconds), max_active}` — the runtime capability payload
  (`trust/capability.hpp:131-134`).
- `cap::decl::Schedule<MaxHorizonSeconds, MaxActive>` — the compile-time declaration tag an agent's
  `Capabilities<...>` policy already accepts (`trust/capability.hpp:315-316`).
- `to_capability(cap::decl::Schedule<...>)` — the conversion `capability_ceiling_of<Policies...>()`
  already calls for every declared capability, including this one, with zero changes needed
  (`trust/capability.hpp:375-378`).
- `capability_detail::subsumes_payload(cap::Schedule const&, cap::Schedule const&)` — attenuation
  already correctly bounds both `max_horizon` and `max_active` (`trust/capability.hpp:492-494`).

**What was actually missing was one lookup method** — `CapabilitySet::find_schedule()`, mirroring the
already-real `find_background()` exactly (same "pure lookup, not `subsumes()`-based" reason: `schedule_
wakeup()` needs the grant's OWN `max_horizon`/`max_active` ceiling to compare a requested delay and a
live count against, which `contains()`'s object-shaped API cannot answer for a capped grant). This
closes the design draft's own §3(b) horizon-bounding requirement — "not a runtime convention a caller
could forget to check, but a value the... system won't let an over-long request past" — through the
EXISTING capability-enforcement mechanism, not a new one. No new CRTP policy tag was added; an agent
declares `Capabilities<cap::decl::Schedule<MaxHorizonSeconds, MaxActive>>` exactly the way it already
would for any other capability.

A second, smaller correction: the draft hedged between emitting `tool_call_started` or `state_changed`
on registration ("mirroring `start_background_task()`'s exact existing shape... emit `run_event_kind::
tool_call_started`/`state_changed`"). `006-Tool-and-Function-Plane.md` §6b is unambiguous: *"Registering,
resolving, or cancelling one is visible on the run's event stream via `StateChanged`."* `schedule_
wakeup()` emits `state_changed`, never `tool_call_started` — there is no `ToolCallRequest` behind a
`schedule_wakeup` call to attribute a tool-call-shaped event to, unlike `start_background_task()`.

## 3. The design (as implemented)

Full writeup lives in `agent_session.hpp`'s own "SLICE 4 ADDITION" banner paragraph (the file a reader
debugging this is actually looking at); summarized here:

- **`StandingEffect` gains `std::optional<std::chrono::steady_clock::time_point> fire_at`**
  (`standing_effect.hpp`), populated only for `kind == schedule_wakeup`. Same in-memory-only durability
  caveat as the rest of `StandingEffect` (§5 below) — not a new regression.
- **`AgentSession::schedule_wakeup(delay, label, now)`** — PLAIN, UNLOCKED (same asymmetry as `start_
  background_task()`/`cancel_standing_effect()`, never part of Quark's own Messages list either, a
  pre-existing precondition named in Slice 3's own banner paragraph, not new here). `now` is a REQUIRED
  caller-supplied parameter, never read from an ambient clock (I5; matches `CircuitBreaker::on_send
  (now_ns)`'s own established discipline; keeps this free of a new ambient-`Clock`-capability violation,
  007 §3's own separately-granted `Clock` cap). Fails closed three ways: no `cap::Schedule` granted at
  all (`schedule_wakeup.not_granted`); `delay` exceeds the grant's own `max_horizon`
  (`schedule_wakeup.horizon_exceeded`); the live count of already-armed `schedule_wakeup` effects meets
  the grant's own `max_active` (`schedule_wakeup.capacity_exceeded`, the `Background<max_concurrent>`/G9
  precedent applied to `Schedule<max_active>`).
- **`AgentSession::due_standing_effects(now)`** — the design draft's own named "missing seam" (§3c):
  read-only, mirrors `open_interactions()`'s existing shape, returns every `schedule_wakeup` effect
  whose `fire_at <= now`. A HOST polls this; deciding WHEN/HOW OFTEN is deliberately out of this
  primitive's own scope (matching `SessionStore`/`ChatClient`/`SecretStore`'s own host-injected, no-
  ambient-authority pattern). Does NOT auto-clear a due entry — the real resumption call's own shape (a
  new `WakeupDue` request, or reuse of the existing turn-start path) is separate, not-yet-designed work
  the draft named explicitly and this ADR does not attempt; a host that has acted on a due entry clears
  it via the already-general `cancel_standing_effect()`.

**The design draft's own self-red-team finding (§4 there) stands unchanged**: a literal "self-firing
timer" (`AgentSession` owning a background `std::jthread`) was considered and rejected — it would
reintroduce exactly the ambient, engine-owned background activity shape ADR-037 removed, reopen a
use-after-free hazard class ADR-035's own middleware-`chat_stream()` red-team finding already caught
once, and silently reintroduce ambient authority (I2) over who gets background CPU time. This
implementation builds nothing that fires on its own; `due_standing_effects()` is passive introspection
only.

## 4. Evidence

`tests/test_rt_agent_session_schedule_wakeup.cpp` (S1-S6, new):
- **S1** — no `cap::Schedule` granted at all: fails closed, nothing registered.
- **S2** — a delay exceeding the grant's own `max_horizon` fails closed; a delay within the same
  horizon then succeeds, proving S2 rejected for the stated reason, not because the grant is unusable.
- **S3** — a successful call produces a real `StandingEffect{kind=schedule_wakeup, fire_at=now+delay}`,
  visible via `list_standing_effects()`, attributed to the right principal; a real `state_changed` event
  lands on the run's event stream, and `tool_call_started` does NOT (the §2 correction, proven, not just
  asserted in prose).
- **S4** — `max_active` enforced against a live count at registration, never silently queued
  (`Schedule<..,1>`'s second call rejected with `schedule_wakeup.capacity_exceeded`).
- **S5** — `due_standing_effects(now)` returns exactly the effects whose `fire_at <= now`, is a pure
  read (repeat calls see the same result), and never mutates `standing_effects_` itself.
- **S6** — `cancel_standing_effect()` (the pre-existing, general mechanism) works on a `schedule_wakeup`
  effect exactly like it already does for `background_task`: denies a different principal, succeeds for
  the owner, and the canceled entry then no longer appears in `due_standing_effects()` while an
  unrelated, not-yet-due entry stays untouched.

Full suite: green (this pass), zero regressions — confirmed by direct build + `ctest` run against the
edited headers plus the new test binary.

## 5. What this ADR does not claim

- **Does not resurrect a self-firing timer.** Named explicitly in §3 and in the design draft's own §4;
  `due_standing_effects()` is polled by a host, never self-invoked.
- **Does not design the host-side poller itself** — `020-Configuration-and-Hosting.md`'s own
  `EmbeddedHost` facade, or an interim `tools/cli_chat.cpp`-level poll loop, is real, separate follow-up
  work, matching the design draft's own scoping.
- **Does not design the resumption call's shape** — what a host does with a due entry beyond observing
  it and eventually canceling the bookkeeping (a new `WakeupDue` request vs. reusing the existing
  turn-start path) is separate, not-yet-designed work, named honestly rather than assumed away.
- **Does not solve `StandingEffect` durability generally** — `fire_at` is exactly as in-memory-only as
  the rest of `StandingEffect` (`standing_effect.hpp`'s own top comment); a `schedule_wakeup` armed just
  before a process restart is lost, the same pre-existing limitation `background_task` already has, not
  a new regression this ADR introduces.
- **Does not close `watch_resource`** (019 §2's "External event" row) — needs 012 (A2A), per `standing_
  effect.hpp`'s own existing, correct scoping note; unrelated to this ADR.
- **Does not expose `schedule_wakeup`/`due_standing_effects` as a model-callable declared tool.** This
  ADR builds the direct `AgentSession` method surface (matching `start_background_task()`'s own shape,
  which is likewise a direct method, not a `ToolTable` entry) — a `schedule_wakeup` `ToolDescriptor`
  wired through the full ten-step tool pipeline (006 §3) so a MODEL can call it directly, rather than
  only a host-side caller, is real, separate follow-up work this ADR does not attempt.
