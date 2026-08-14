# `schedule_wakeup` as a real `StandingEffect` producer — design draft

**Status: designed, not implemented.** Matches this session's own gap-4/gap-10 precedent: a large,
genuinely open-ended primitive gets a real design + self-red-team pass here, landing as document-only
work rather than rushed code, per this project's `design → red-team → prove → judge` discipline
(CLAUDE.md) applied honestly to something this size.

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #7. `006-Tool-and-
Function-Plane.md` §6b / `019-Durability-and-Long-Running-Agents.md` §2 (`schedule_wakeup`'s own
normative definition — the "Timer/schedule" row of 019 §2's wake-condition table).
`core/standing_effect.hpp` (the existing, real `StandingEffect` handle shape `background_task`
already produces for real). `rt/agent_session.hpp`'s own top comment (the exact, current, honest
statement of what's missing — quoted in §1).

## 1. Re-grounding: this is a bigger gap than the audit's own framing assumed

The audit's own recommended approach: "give `schedule_wakeup` a real `StandingEffect` producer — but
must enforce `Schedule<max_horizon>` at arm time (currently unbounded, a live I2 gap) and name the
missing `ReminderService`-access seam." This framing assumes a WORKING underlying mechanism exists
(Quark's `ReminderService`/`TimerWake`) and only the `StandingEffect` wrapping + horizon-bounding is
missing.

**Re-verified directly against current code: the underlying mechanism itself is gone, not merely
unwrapped.** `rt/agent_session.hpp`'s own top comment (lines 12-15, present before this draft, not
added by it) states plainly: `TimerWake` "depended on a live `quark::Engine`'s `ReminderService`...
this slice has no standalone replacement design for it yet." ADR-037 removed Quark as a dependency
entirely; `rt::AgentSession` — the REAL, current runtime every new gap-audit closure this session has
targeted — has **zero** timer/delay/scheduled-callback primitive anywhere. A targeted survey of every
file under `include/agentengine/rt/` confirms: `thread_pool.hpp`/`task.hpp` have no chrono usage at
all; the only chrono reads in `rt::` are a one-shot elapsed-time CHECK inside `WorkflowSupervisor`'s
executor loop (`workflow_supervisor.hpp`, comparing `steady_clock::now()` against a deadline every
round — not a scheduler) and `CircuitBreaker`'s own caller-supplied-`now_ns` state machine (no
internal clock read, no sleep). Nothing anywhere fires a callback, wakes a coroutine, or re-invokes
anything after a delay.

**Consequence:** this is not "wire `schedule_wakeup` into the already-real `StandingEffect` shape,"
it is "design the first real timer/reminder primitive `rt::` has ever had, then wire `schedule_wakeup`
into it." A real implementation attempt in this same pass, without a dedicated design/red-team cycle
of its own, would risk exactly the failure pattern this whole audit's Executive Summary names as its
own headline finding: converting a safe, visible absence ("this doesn't work yet, honestly named")
into an unsafe, silent one (a `schedule_wakeup` call that appears to succeed but never actually wakes
anything, because the seam it needs doesn't exist).

## 2. Why `rt::AgentSession`'s own architecture makes this genuinely hard, not just unbuilt

Quark's `ReminderService` worked because a Quark actor has a persistent, engine-owned event loop —
arming a reminder meant "the engine will deliver a message to this actor's mailbox at time T,"
independent of whether anything is currently calling into the actor. **`rt::AgentSession` has no such
loop.** Per ADR-037's own accepted design, it is "a plain templated class instance" — I1 ("one
session, one executor") is enforced by an in-flight guard (`AsyncMutex`), not mailbox exclusivity, and
nothing owns a background thread that could fire a callback into a session that isn't currently being
driven by some caller. This means `schedule_wakeup`, in the CURRENT architecture, cannot be a
self-firing async timer the way it conceptually was under Quark — it has to become a **durable intent
record plus a host-polled resumption seam**, matching this project's own consistent "host-injected,
no ambient authority" pattern (`SessionStore`, `ChatClient`, `SecretStore` are all seams the HOST
supplies, never an ambient engine service) rather than reintroducing an ambient scheduler.

## 3. The design

**(a) `StandingEffect` gains a `fire_at` field** (`std::chrono::steady_clock::time_point`, populated
only for `kind == schedule_wakeup`), and `AgentSession` gains `schedule_wakeup(EffectContext& ctx,
std::chrono::milliseconds delay, std::string label, std::chrono::steady_clock::time_point now)` —
mirroring `start_background_task()`'s exact existing shape (mint `handle_id`, build a `StandingEffect`,
push to `standing_effects_`, emit `run_event_kind::tool_call_started`/`state_changed`). `now` is a
REQUIRED caller-supplied parameter, never read from an ambient clock — the same discipline
`CircuitBreaker`'s own `on_send(now_ns)`/`on_result(now_ns)` already establishes in this codebase, and
the only way to keep this deterministic/replayable (I5) and free of a new ambient-Clock-capability
violation (001 §7: "Clock is not a wired capability").

**(b) `Schedule<max_horizon_ms>`, a new compile-time CRTP policy tag** (matching `MaxTurns<N>`/
`TokenBudget<N>`'s existing idiom), applied the same way those are — checked at `register_agent<A>()`
time so an agent that never declares it gets a conservative built-in default, never an unbounded one.
`schedule_wakeup()` itself fails closed (`schedule_wakeup.horizon_exceeded`) when `delay` exceeds the
declared (or default) horizon — this is what structurally closes the audit's own "currently unbounded,
a live I2 gap" finding: not a runtime convention a caller could forget to check, but a value the type
system won't let an over-long request past.

**(c) The missing seam, named explicitly, not hidden**: a new `due_standing_effects(now) ->
std::vector<StandingEffect>` introspection method on `AgentSession` (read-only, mirroring `open_
interactions()`'s existing shape) that a HOST polls — the host, not `AgentSession` itself, owns
deciding WHEN and HOW OFTEN to check (a cron-style poll loop, a `tools/cli_chat.cpp`-style REPL tick,
a future `020-Configuration-and-Hosting.md`-owned `EmbeddedHost` facade's own scheduler — all
deliberately out of THIS primitive's scope). A due entry the host observes is resolved by the host
calling back into the session (the exact shape of that resumption call — a new `WakeupDue` request
analogous to `ResolveInteraction`, or reuse of the existing turn-start path — is real, separate design
work this draft does not attempt, named honestly rather than assumed away).

## 4. Self-red-team findings

**A literal "self-firing timer" design was considered and rejected as a false economy.** A tempting
first instinct: give `AgentSession` its own background `std::jthread` that sleeps until the earliest
`fire_at` and then calls back in. Rejected — this reintroduces exactly the "ambient, engine-owned
background activity" shape ADR-037 deliberately removed (no mailbox, no supervision DSL, "no
distributed anything"), makes `AgentSession` non-trivially-destructible again (a live background
thread with a callback into a possibly-destroyed session is a real use-after-free hazard, the same
class ADR-035's own middleware-`chat_stream()` red-team finding already caught once in this codebase),
and silently reintroduces ambient authority (WHO decided this session gets background CPU time,
unaccountable to any capability grant) — a real I2 violation, not just an implementation-quality
concern.

**Storing `fire_at` as a wall-clock-independent `steady_clock::time_point` on an in-memory-only
`StandingEffect` has a real, named limitation**: `StandingEffect` is explicitly, deliberately
in-memory-only (`standing_effect.hpp`'s own comment: "NOT one of `AgentSessionRecord`'s own
`QUARK_SERIALIZE`'d fields"). A `schedule_wakeup` armed just before a process restart is lost — the
SAME limitation `background_task` already has today, not a new regression this draft introduces, but
worth naming plainly since "Timer/schedule" (019 §2's own row name) has a much stronger intuitive
expectation of durability than "a task currently running." Closing this needs `StandingEffect`
persistence generally — a separate, larger, already-named gap (`decisions/README.md`'s own ADR-038
entry notes passivation's own durable-state story), not something this draft's own scope can absorb.

## 5. What this draft does not claim

- **No code.** Every symbol named above (`fire_at`, `Schedule<max_horizon_ms>`, `due_standing_
  effects()`) is a design proposal, not a real declaration anywhere in the tree.
- **Does not design the host-side poller itself** — `020`'s own `EmbeddedHost` facade, or an interim
  `tools/cli_chat.cpp`-level poll loop, is real, separate follow-up work.
- **Does not close `watch_resource`** (019 §2's "External event" row) — that needs 012 (A2A), per
  `standing_effect.hpp`'s own existing, correct scoping note; unrelated to this draft.
- **Does not solve `StandingEffect` durability generally** — named as a real, pre-existing limitation
  (§4) this draft's own scope does not attempt to close.
