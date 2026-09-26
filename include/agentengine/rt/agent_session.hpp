#pragma once
// (Also implements ADR-191's approved-lesson grant and ADR-192's unattended-mode opt-ins: see `set_approved_lessons`,
// `disable_system_channel_fence`, `set_unattended_approvals`, `apply_approved_lessons`, `effective_approval_decider`.)
// ADR-037 Phase 2, Slice 1: `agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>`,
// the Quark-free replacement for `agentengine::AgentSession` (core/agent_session.hpp). Lives under
// `agentengine::rt`, a NEW namespace, deliberately NOT wired into any live call site yet -- nothing
// in the current Quark-based build is touched by this file existing.
//
// SCOPE OF THIS SLICE, named explicitly (matching this codebase's own "residuals named, not silently
// assumed complete" convention): this migrates the core turn loop -- configuration, admission,
// run_model_call()/run_rounds()'s model-call + tool-call round loop, ADR-029's approval suspend/
// resume, and the pure bookkeeping helpers (fork_from/redact/clear_in_process_state/open_interaction/
// resolve_interaction). It does NOT yet migrate:
//   - TimerWake (the reminder-service wake ACKNOWLEDGEMENT path itself) -- depended on a live
//     quark::Engine's ReminderService, a real integration-test-only surface
//     (test_agent_session_timer_wake.cpp) this slice has no standalone replacement design for. Still
//     true, and Slice 4 (below) does NOT change it: nothing here resurrects a self-firing timer --
//     a literal "AgentSession owns a background std::jthread" design was considered and rejected for
//     schedule_wakeup too (docs/planning/schedule-wakeup-standing-effect-design-draft.md §4, ADR-053),
//     for the same reason ADR-037 removed Quark's own ambient background activity in the first place.
//     `standing_effects_`'s OTHER wake row, "Local background task completion" (BackgroundTaskDone),
//     IS migrated -- see Slice 3 below; the THIRD row, "Timer/schedule" (`schedule_wakeup`), is now
//     ALSO real, via a structurally different, host-polled mechanism -- see Slice 4.
//   - Event streaming (enable_event_stream/emit_run_event) uses core/stream.hpp's stream<T> --
//     RESOLVED: core/stream.hpp's own backend migration (an ADR-037 pass after this slice was first
//     written) swapped stream<T>'s internals from quark::ReplyStream to rt::channel<T>, and a LATER
//     ADR-037 pass finished the job: stream<T>::terminal()/fail_error() now return
//     agentengine::stream_terminal/agentengine::error (native, dependency-free types) instead of
//     quark::ReplyStreamTerminal/quark::error. No residual left here at all.
//
// A LARGER, MORE FUNDAMENTAL NAMED GAP, found while writing this file -- RESOLVED by a later ADR-037
// pass, kept here (updated, not deleted) since the reasoning is still the right way to understand why
// it mattered. Removing `quark::Actor<Self, Sequential>` and Quark's mailbox does NOT, by itself, make
// this type Quark-free: `ChatClientT`/`HistoryProviderT` are the EXISTING `agentengine::ChatClient`/
// `ContextProvider` conformers (`core/chat_client.hpp`/`core/context_provider.hpp`), and every real
// backend (`OpenAIChatClient`, `AnthropicChatClient`, `ModelCallGateway`) and every `HistoryProvider<...>`
// names its return type through the `agentengine::task<T>` alias. `core/task.hpp` USED TO define that
// alias as a single blanket `= quark::task<T>` for every T -- meaning every conformer was still
// quark::task<T>-typed even though this file's own `run_model_call()`/`run_rounds()` could already
// `co_await` them transparently (any `rt::task<T>` coroutine body can `co_await` any awaitable,
// including a `quark::task<T>`, since both independently implement the same C++20 awaiter protocol).
// UPDATE (superseded by a LATER ADR-037 pass, kept here since the history is still the right way to
// understand why it mattered): `core/task.hpp` briefly split the alias per-T -- `task<void>` (bare
// `task<>`) stayed `quark::task<void>` (Quark's ADR-007 dispatch-handler type, needed by every
// still-live `quark::Actor` at that point in the migration), while `task<T>` for `T != void` already
// resolved to `agentengine::rt::task<T>` directly. That split is now GONE: every `quark::Actor` type
// this project ever defined has since been deleted, so `task<T>` for every `T`, including `void`,
// resolves to `agentengine::rt::task<T>` through one blanket alias (`core/task.hpp`'s own current
// banner). Since every conformer names its return type through the alias rather than the concrete
// type, this closed the gap for all of them at once, with zero per-conformer source changes --
// verified (not just reasoned) at the time via a full rebuild and the full test suite passing clean
// across two consecutive runs. This AgentSession is Quark-free at BOTH the actor/mailbox/dispatch
// layer AND the coroutine-type layer.
//
// I1 ("one session, one executor"), Quark's actor mailbox's job before this migration, is now
// enforced by `rt::AsyncMutex session_mutex_` (async_mutex.hpp, itself proven in this same phase):
// every public async entry point (`start_run`, `resolve_interaction`) acquires it for the whole call.
// Unlike Quark's mailbox (which structurally makes a second concurrent call impossible), this is a
// runtime-checked guard -- ADR-037 §5's own red-team finding, named honestly, not silently upgraded
// to "just as safe": a NEW public entry point that forgets to acquire the guard would reintroduce the
// exact race the mailbox used to make unreachable by construction. Every entry point below is
// reviewed against this rule; a future one must be too.
//
// SLICE 2 ADDITION (snapshot/checkpoint, this file's own residual list above named this as not-yet-
// done): `to_record()`/`restore_from_record()`, `snapshot_record()`, and the free functions
// `save_agent_session_snapshot`/`load_agent_session_snapshot`/`checkpoint_if_due`/`delete_session`,
// all built against `rt::SessionStore` (session_store.hpp, already built/tested in Phase 1) instead
// of `quark::FenceToken`/`Activation`/`snapshot_sequential`. Two real design points, not a
// mechanical port:
//   - THE IN-FLIGHT GUARD (ADR-037 §5's own named red-team finding: "persistence's hardest problem
//     -- safe concurrent snapshot -- is currently solved by Quark's FenceToken... Phase 1 needs its
//     own design pass before it's trusted"). The Quark original relied on being called at the point
//     `quiesce(Drain)` reaches on a Sequential actor -- i.e., the actor mailbox itself guaranteed no
//     handler was concurrently mutating state. There is no mailbox anymore, so `snapshot_record()`
//     (below) acquires `session_mutex_` -- the SAME `rt::AsyncMutex` `start_run()`/
//     `resolve_interaction()` already use for I1 -- for the whole duration of reading state into an
//     `AgentSessionRecord`. A concurrent `start_run()` and a concurrent `snapshot_record()` queue on
//     the same FIFO lock; neither can observe the other's partial mutation. `delete_session()`
//     (below) uses the equivalent locked path (`clear_in_process_state_locked()`) for the same
//     reason on the write side.
//   - ENCODE/DECODE: the Quark original used `quark::Described`/`QUARK_SERIALIZE`, which this file
//     cannot depend on (a Quark type). Records are instead encoded as JSON via
//     `core/json_value.hpp` (already std-only, zero Quark dependency) -- `agent_session_record_to_
//     json()`/`_from_json()` plus a thin bytes<->text wrapper to satisfy `SessionStore`'s
//     opaque-bytes contract.
// A real, DELIBERATE narrowing versus the original `AgentSessionRecord`, named rather than silently
// dropped: `created_at_ns`/`updated_at_ns` are NOT carried by this slice's record, because Slice 1's
// own `AgentSession` never added `created_at_`/`updated_at_` members in the first place (001 §7
// already documents the original's own versions of these fields as unwired placeholders carrying no
// real wall-clock value project-wide -- this slice does not invent ones just to round-trip them).
// A smaller, PRE-EXISTING residual, not introduced by this slice: `core/interaction.hpp` (already
// included by Slice 1, for `Interaction`/`interaction_reason`) itself transitively includes
// `quark/core/describe.hpp` for `QUARK_SERIALIZE` -- this file's own encode/decode never calls that
// macro (it hand-writes JSON for `Interaction` instead), but the transitive include is still there.
// Named here as the same kind of residual the file banner's "LARGER, MORE FUNDAMENTAL NAMED GAP"
// paragraph already tracks (a coroutine-type-layer gap, not this seam), not overclaimed as fixed.
//
// SLICE 3 ADDITION (standing effects / background tasks): `start_background_task()`/
// `cancel_standing_effect()`/`list_standing_effects()`, backed by `agentengine::StandingEffect`
// (standing_effect.hpp, reused verbatim -- pure data, no actor coupling, same "reuse despite its own
// transitive quark/core/describe.hpp include" precedent Interaction already set, see Slice 2's own
// paragraph above). `tool_pipeline.hpp::background_task()` itself needed ZERO changes -- it already
// had no Quark dependency (steps 1-7 synchronous, step 8 onward a detached std::thread calling a
// caller-supplied std::function on_complete). The ONLY Quark-coupled piece was the ORIGINAL
// AgentSession wiring on_complete to `self.tell(BackgroundTaskDone{...})` -- a quark::ActorRef's
// thread-safe, mailbox-serialized delivery into the actor's own processing queue. Design (produced by
// an independent design/red-team pass before implementation, per this project's own governance for a
// genuinely hard concurrency question, not an ad-hoc guess):
//   - `on_complete` (running on the detached worker thread, no coroutine, cannot co_await
//     session_mutex_) instead pushes a `BackgroundTaskDone{handle_id, call_id, ok}` into
//     `background_completions_`, a `BackgroundCompletionQueue` (its own small, separate `std::mutex`
//     + `std::deque` -- deliberately NOT session_mutex_, which only a coroutine can acquire) held
//     behind a `std::shared_ptr` on `AgentSession`. The closure captures a `std::weak_ptr` to that
//     queue, not `this`/a reference to the session -- if the session (and its queue) has already been
//     destroyed by the time the worker finishes, `weak_ptr::lock()` returns null and the completion is
//     silently dropped (no UAF), the same "no delivery guarantee, best-effort" spirit the original's
//     `tell()`-into-a-possibly-gone-actor already implied without this file being able to inspect
//     Quark's own answer to that case.
//   - `drain_background_completions_locked()` (private, caller must already hold session_mutex_)
//     drains the queue and applies each entry to `standing_effects_` exactly like the original's
//     `handle(BackgroundTaskDone const&)` did (find by handle_id, capture owner_run_id BEFORE erasing,
//     emit ToolCallFinished attributed to that run -- never whatever run is current when the
//     completion happens to land). Called as the FIRST statement inside `start_run()`/
//     `resolve_interaction()`, right after acquiring the guard -- so a host never has to remember to
//     drain separately; `drain_background_completions()` (public, its own task<T>) exists for a host
//     that wants to force a drain between runs anyway.
//   - `cancel_standing_effect()`/`list_standing_effects()` stay PLAIN, UNLOCKED methods (no
//     session_mutex_ acquisition) -- this is not an oversight, it matches the ORIGINAL's own asymmetry
//     exactly: neither was ever part of Quark's `Messages` type list for this actor either (unlike
//     `BackgroundTaskDone`, which WAS), so they were already unserialized-by-the-mailbox in the Quark
//     version too, the same category `fork_from()`/`redact()`/`clear_in_process_state()` above already
//     fall into. NAMED, NOT SILENTLY ASSUMED SAFE: a host that calls these concurrently with a
//     `start_run()`/`resolve_interaction()` in flight on another "thread of control" races
//     `standing_effects_` directly -- a real, pre-existing-in-kind precondition (not a new one
//     introduced here), but easy to wrongly assume is now covered just because a completion queue
//     exists. `cancel_standing_effect()` racing an already-queued-but-not-yet-drained completion is
//     safe by construction: drain looks up by `handle_id`; if cancel already erased the entry, drain's
//     lookup misses and no-ops, exactly the original's own documented idempotent-no-op behavior for "a
//     late BackgroundTaskDone for a canceled handle."
//     ADR-061 §20.5/§24.2: `start_background_task()` is NO LONGER part of this "plain, unlocked"
//     group -- it now acquires `session_mutex_` like `start_run()`/`resolve_interaction()` do,
//     deliberately breaking the asymmetry described above for this one function. Confirmed (grep,
//     repo-wide, across three independent red-team rounds) to have zero real callers in product code,
//     so nothing shipped depended on the old unlocked behavior; I1 made staying unlocked a real hazard
//     once Tier 3 makes a session reachable from more than one concurrently-arriving caller. See that
//     function's own comment for the full rationale.
//   - `rt::channel<T>` (already built, Phase 1) was considered and rejected for this queue: its
//     producer side is move-only and auto-closes on destruction, so sharing it across many independent
//     detached-thread closures would need the exact same weak_ptr-to-a-shared-instance dance for zero
//     benefit -- draining here is synchronous/opportunistic (never suspends), and a
//     `{handle_id,call_id,bool}` payload needs no bounded-backpressure story `Background<max_concurrent>`
//     doesn't already provide at the authorize step. A hand-rolled mutex+deque is simpler and has no
//     close/terminal semantics to reason about for a queue that is never itself "done."
//
// SLICE 4 ADDITION (schedule_wakeup, ADR-053, closing 2026-08-10-full-codebase-adr-gap-audit.md gap
// #7): `schedule_wakeup()`/`due_standing_effects()`, the THIRD real `StandingEffect` producer (019 §2's
// "Timer/schedule" row, 006 §6b). Re-grounding this against current code (the design draft's own §1)
// found the underlying primitive itself gone, not merely unwrapped -- ADR-037 removed Quark's
// `ReminderService` entirely and `rt::` has never had ANY timer/delay primitive. The capability side
// turned out to be already fully built and simply unused: 007 §3's own table names `Schedule<max_
// horizon, max_active>` as a CAPABILITY (`cap::Schedule`/`cap::decl::Schedule<Seconds, MaxActive>`,
// `trust/capability.hpp`), not a new CRTP policy tag the way the design draft's own §3(b) speculated
// before this slice touched real code -- an agent that declares `Capabilities<cap::decl::Schedule<...>>`
// already gets it compiled into `AgentMetadata::capability_ceiling` and bound into its session's
// `CapabilitySet` through the EXISTING mechanism `Background<max_concurrent>` already exercises end to
// end; only `CapabilitySet::find_schedule()` (mirroring `find_background()`) needed adding. Design,
// matching `start_background_task()`'s own shape exactly:
//   - `schedule_wakeup(delay, label, now)` WAS plain, unlocked, same asymmetry as
//     `cancel_standing_effect()` above (never part of Quark's own Messages list either -- see Slice
//     3's own paragraph for why that's a pre-existing, named, not-new precondition). ADR-061 §20.5
//     found it was ALSO a directly-callable, unlocked entry point in its own right (not just its
//     internal reads) -- now split: `schedule_wakeup_impl()` (private, unlocked, the logic above)
//     stays exactly this shape, and a new public `schedule_wakeup()` wrapper locks and resolves
//     per-request authority before delegating to it, mirroring `start_background_task()`'s own
//     locking exactly. The internal offer-gate closure in `run_rounds()` calls `schedule_wakeup_impl()`
//     directly (already locked via its own caller; a non-reentrant `AsyncMutex` would self-deadlock on
//     a second `co_await lock()`). `now` is a REQUIRED caller-supplied parameter, never read from an
//     ambient clock --
//     the same discipline `CircuitBreaker::on_send(now_ns)`/`on_result(now_ns)` already establishes in
//     this codebase (I5: nondeterminism crosses a recorded seam), and the only way this stays free of a
//     new ambient-Clock-capability violation (007 §3's own separate, explicitly-granted `Clock` cap).
//   - Fails closed three ways, structurally rather than by runtime convention: no `cap::Schedule`
//     granted at all (`schedule_wakeup.not_granted`); `delay` exceeds the grant's own `max_horizon`
//     (`schedule_wakeup.horizon_exceeded` -- the audit's own "currently unbounded, a live I2 gap"
//     finding, closed by a value the type/capability system won't let an over-long request past, not a
//     check a caller could forget); the live count of already-armed `schedule_wakeup` effects meets the
//     grant's own `max_active` (`schedule_wakeup.capacity_exceeded`, mirroring G9's `Background<
//     max_concurrent>` precedent exactly).
//   - Registration emits `state_changed` (006 §6b: "Registering, resolving, or cancelling one is
//     visible on the run's event stream via StateChanged (013 §1)"), not `tool_call_started` --
//     deliberately different from `start_background_task()`'s own event, since there is no
//     `ToolCallRequest` backing a `schedule_wakeup` registration to attribute a tool-call-shaped event
//     to.
//   - `due_standing_effects(now)` is the design draft's own named "missing seam" (§3c): read-only,
//     mirrors `open_interactions()`'s existing shape, returns every `schedule_wakeup` effect whose
//     `fire_at <= now`. A HOST polls this -- deciding WHEN/HOW OFTEN is deliberately out of this
//     primitive's own scope (a cron-style poll loop, a `tools/cli_chat.cpp`-style REPL tick, a future
//     `EmbeddedHost` facade's own scheduler), matching this project's "host-injected, no ambient
//     authority" pattern (`SessionStore`/`ChatClient`/`SecretStore` are all host-supplied seams, never
//     an ambient engine service) rather than resurrecting a self-firing timer (file banner's residual
//     paragraph above, and the design draft's own §4 self-red-team). A due entry is NOT auto-cleared by
//     this call -- the real resumption call's own shape (a new `WakeupDue` request, or reuse of the
//     existing turn-start path) is separate, not-yet-designed work the draft named explicitly and this
//     slice does not attempt; a host that has acted on a due entry clears its bookkeeping via the
//     already-general `cancel_standing_effect()` above.
//
// `StartRun`/`ResolveInteraction` keep their EXISTING field shapes (matching core/agent_session.hpp's
// own types) for call-site compatibility, but are no longer Quark::Ask<> messages -- the 192-byte
// MessagePool::kMaxPayload constraint that shaped `SessionCaller` (a narrowed wire-sized identity
// type, deliberately smaller than the general `Principal`) no longer applies once there is no Quark
// mailbox to cross. `SessionCaller` is kept anyway, unchanged, rather than widened back to `Principal`
// in this slice -- the ADMISSION RULE it encodes (exact id/tenant match only, no delegation) is a
// real, deliberate 018 §2 design choice independent of the byte-budget that originally forced its
// shape, and widening it is out of this slice's scope (a future slice's call, not a side effect of
// this migration).
//
// ADR-199 (issue #115 E3): the non-template part of this class -- every member that does not name ChatClientT,
// StateT or HistoryProviderT -- lives in `AgentSessionCore` (rt/agent_session_core.hpp), compiled once in
// src/rt/agent_session_core.cpp instead of once per session type in every translation unit. This template adds
// the bound chat client, history provider and state, and the hooks the core's loop calls through.

#include "agentengine/rt/agent_session_core.hpp"

namespace agentengine::rt {

template <class ChatClientT, class StateT = NoSessionState,
          class HistoryProviderT = agentengine::HistoryProvider<agentengine::Window<0>>>
    requires (agentengine::ChatClient<ChatClientT> || agentengine::ModelCallGatewayLike<ChatClientT>) &&
             agentengine::ContextProvider<HistoryProviderT>
class AgentSession final : public AgentSessionCore {
public:
    template <class... Args>
    ChatClientT& emplace_chat_client(Args&&... args) {
        return chat_client_.emplace(std::forward<Args>(args)...);
    }

    // decisions/ADR-066-context-provider-attribution-provenance.md §7 residual / ADR-070 amendment:
    // this session calls `history_provider_.on_context()` DIRECTLY, never `assemble_context()` --
    // messages/tools this session assembles carry NO `attribution` (`Message::attribution`/
    // `ToolDescriptor::attribution` stay `nullopt` throughout), even for a genuinely multi-role
    // `HistoryProviderT`. A host that wants ADR-066's mandatory, non-bypassable provenance stamping
    // composes through `ComposedContextProvider<...>` as `HistoryProviderT` instead (that conformer
    // routes through `assemble_context()` internally, no change needed here) -- using this single-
    // provider slot as-is is a deliberate, acknowledged trade for the simpler API, not an
    // undiscovered gap.
    // ADR-116: stamps `history_provider_` with this session's own identity on every call, when
    // `HistoryProviderT` happens to be a type that tracks one (currently only `ComposedContextProvider
    // <Ms...>`'s own private `bind_owner()`, which this class is friended for -- see that method's
    // own comment). `if constexpr` keeps this a true no-op, not just harmless, for every OTHER
    // `HistoryProviderT` (e.g. the default `HistoryProvider<Window<0>>`), which never declares
    // `bind_owner()` at all -- the closing hazard this exists to prevent (cross-session aliasing of
    // LIVE, capability-bearing provider state) doesn't apply to a plain, copyable history buffer.
    //
    // ADR-116 follow-on: stamps `session_identity_` (a permanent, never-reused counter value), NOT
    // this session's raw `this` address -- an independent, same-day red-team pass empirically
    // confirmed the raw-address version had a real ABA hole (this object's own comment on
    // `session_identity_` has the full repro: destroy a session, heap-allocate an unrelated one that
    // happens to land at the same freed address, and a stale tagged `ComposedContextProvider` from the
    // FIRST session wrongly matches the SECOND).
    [[nodiscard]] HistoryProviderT& history_provider() noexcept {
        if constexpr (requires { history_provider_.bind_owner(session_identity_); }) {
            history_provider_.bind_owner(session_identity_);
        }
        return history_provider_;
    }

    [[nodiscard]] bool has_chat_client() const noexcept { return chat_client_.has_value(); }
    [[nodiscard]] StateT& state() noexcept { return state_; }
    [[nodiscard]] StateT const& state() const noexcept { return state_; }

    // Locked wrapper around clear_in_process_state() -- delete_session() (free function, below)
    // needs this so a concurrently in-flight start_run()/resolve_interaction() can never race a
    // deletion, the write-side counterpart to snapshot_record()'s read-side guard.
    task<void> clear_in_process_state_locked() {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();
        clear_in_process_state();
        co_return;
    }

    // ADR-102 Phase 5 fix, closing a real, twice-independently-found structural gap (ADR-102 Phase 3
    // §22's `SandboxRuntime::merge_into()`/`discard()` finding, and Phase 4 §29's own disclosed-not-
    // fixed residual naming this exact function): `fork_from()` used to run with NO serialization
    // against `source`'s own in-flight `start_run()`/`resolve_interaction()` at all -- unlike every
    // OTHER public entry point on this class, which all acquire `session_mutex_` (I1) for their whole
    // duration. Without it, `history_provider_ = source.history_provider_;` below (a plain copy-
    // assignment) can run CONCURRENTLY with a `source.start_run()` round already in flight on a
    // different thread: for a `MandatorySandboxProvider`-shaped `HistoryProviderT`, that
    // copy-assignment triggers a real `SandboxRuntime::spawn_child_branch()` call, which takes
    // `SandboxRuntime`'s own `exclusivity_` lock -- the SAME lock an in-flight `run()` call on
    // `source` may already hold. `rt/block_on.hpp` makes that specific race SURVIVABLE (no more
    // coroutine-frame use-after-free under contention), but survivable is not the same as CORRECT:
    // without locking `source.session_mutex_` here, `source`'s other fields (`history_`/`state_`/
    // `metadata_`) could also be read mid-mutation by a concurrent round. Fixed by acquiring
    // `source.session_mutex_` for the whole copy, matching every other public entry point's own I1
    // discipline -- driven synchronously via `agent_session_detail::acquire_session_mutex()` +
    // `block_on()` (see that helper's own comment) so `fork_from()` itself stays a plain, synchronous
    // function; no call site anywhere in this codebase needs to change.
    //
    // SCOPE, deliberately narrow: only `source`'s own mutex is acquired, not `*this`'s. Every real
    // call site in this codebase (and this design's own established usage) forks INTO a fresh,
    // not-yet-`start_run()`-able target -- forking into an already-live, concurrently-running session
    // is not a documented or supported operation this class offers anywhere else, so guarding against
    // it here would invent new semantics for a usage pattern nothing else in this codebase exercises,
    // rather than closing the specific, real, already-named hazard.
    //
    // REAL HAZARD this fix itself introduced, found by an independent red-team pass, disclosed, and NOW
    // CLOSED (ADR-123, same design line, later pass): `AsyncMutex` had no reentrancy check, so calling
    // `fork_from(source, ...)` from code ALREADY running on the same OS thread inside an in-flight
    // `start_run()`/`resolve_interaction()` round on `source` -- e.g. synchronously, from a tool
    // closure's own body, the exact shape `schedule_wakeup`'s own closure already has to route around
    // via an internal `_impl` bypass for this identical reason -- would genuinely, reproducibly
    // self-deadlock: `block_on()`'s own busy-wait spins forever, because the only thing that could ever
    // call `unlock()` is the very `start_run()`/`resolve_interaction()` Guard already parked one frame
    // up on the SAME stack, waiting for this call to return. Confirmed via a real, targeted repro (a
    // `ChatClient::chat()` that calls `fork_from()` on the in-flight session from inside a live round,
    // same thread) -- 100% reproducible hang before this fix. NOT reachable through any real call site
    // in this codebase today (every `fork_from()` caller is a top-level `main()`), but exactly the shape
    // a near-future `agent.spawn`-style tool wired to call `fork_from()` directly from its own closure
    // would hit. CLOSED, FOR THE SAME-OS-THREAD-THROUGHOUT CASE, via `AsyncMutex::
    // is_held_by_current_thread()` (`rt/async_mutex.hpp`, ADR-123) -- a small, additive owner-thread
    // query on the primitive itself (no new locking discipline, no behavior change for any existing
    // caller), checked below: if the calling thread already holds `source.session_mutex_`, I1 already
    // guarantees no other thread can be touching `source` concurrently, so the lock is safely skipped
    // rather than re-acquired.
    //
    // SCOPE CORRECTION (same-day independent red-team round, ADR-123 §7): `owner_` is written ONCE, at
    // the moment `source.session_mutex_` is acquired (`LockAwaiter::await_resume()`), to whichever OS
    // thread happens to be physically running at that instant -- it is NOT re-stamped as the round's
    // own execution proceeds. `agentengine::rt::block_on()`'s own file banner already documents, as a
    // normal and exercised case (not hypothetical -- `RunCommandTool`/`AsyncQuota` contention hits it
    // for real), that a coroutine's continuation can resume on a DIFFERENT OS thread than the one that
    // suspended it. If `source`'s own in-flight round suspends on some OTHER async primitive (e.g. a
    // real `ChatClient::chat()` awaiting network I/O) and its continuation resumes on a different OS
    // thread BEFORE a tool closure reentrantly calls `fork_from()`, `is_held_by_current_thread()`
    // returns a FALSE NEGATIVE on that new thread (`owner_` still names the original thread) --
    // `fork_from()` then tries to re-acquire `source.session_mutex_` and self-deadlocks again, the
    // exact failure mode this fix exists to close, just via a narrower trigger. Empirically confirmed
    // with a throwaway repro (forced thread hop before the reentrant call; not committed -- see
    // ADR-123 §7). NOT fixed in this pass: a general fix needs tracking the in-flight ROUND's own
    // identity (a coroutine/call-chain property) rather than OS-thread identity, which no thread-keyed
    // mechanism (this one included) can give by construction -- real, contained follow-on work, not a
    // same-pass mechanical tightening. Matches this hazard's own pre-ADR-123 status: not reachable
    // through any real call site in this codebase today.
    //
    // CLOSED BY decisions/ADR-175 -- and a worse failure the thread comparison also had: after a lock
    // hand-off, the thread that released `session_mutex_` (or resumed the new holder inline) could run
    // unrelated code for which the check was TRUE, and this function then skipped the lock while the real
    // holder was mid-round (an I1 violation, demonstrated). `is_held_by_current_thread()` now compares the
    // holder id of the logical task (`rt/resume_home.hpp`), recorded when the lock is granted, and a
    // round's holder id travels with it through `block_on()` -- including across the thread hop this
    // paragraph describes, since a homed round now resumes on its own `block_on()` thread.
    void fork_from(AgentSession const& source, std::string new_session_id,
                    std::optional<std::size_t> history_prefix_len = std::nullopt) {
        AsyncMutex::Guard source_guard;
        if (!source.session_mutex_.is_held_by_current_thread()) {
            source_guard = agentengine::rt::block_on(
                agent_session_detail::acquire_session_mutex(source.session_mutex_));
        }
        // ADR-199: everything but the template's own members is copied or reset by the core, under the same guard.
        fork_core_from(source, std::move(new_session_id), history_prefix_len);
        state_            = source.state_;
        history_provider_ = source.history_provider_;
    }

    // ADR-199: the core clears everything it owns (see clear_core_state()'s own comment for the contract); the state
    // and the history provider are this template's.
    void clear_in_process_state() {
        clear_core_state();
        state_            = StateT{};
        history_provider_ = HistoryProviderT{};
    }

private:

    // Same shape as core/agent_session.hpp's own run_model_call(), ported to rt::task<T>, with ONE
    // real consolidation (not a byte-for-byte port): the original had three branches (gateway /
    // buffered-chat() / buffered-drain-when-chat()-is-unavailable / live-streaming-when-opted-in) --
    // this collapses the last three into ONE shared drain loop, gated only on whether to emit
    // model_delta events (`stream_model_calls_`), since "buffer silently" and "stream live" differ
    // ONLY in that one respect once chat() isn't being used. `ChatClientT::chat_stream()` still
    // returns `agentengine::stream<ChatResponseUpdate>` (core/stream.hpp's type -- now rt::channel<T>-
    // backed internally, see file banner's UPDATED note on event streaming) -- unaffected by this
    // consolidation either way, just inherited from the same place it always was.
    // Fail-closed-on-missing-usage (004 §5's TokenBudget<N>) is preserved exactly, on both paths
    // through the shared loop.
    task<result<ChatResponse>> run_model_call(ChatRequest const& request, EffectContext& ctx) override {
        // Gap-audit finding 19, Phase 1: fail closed BEFORE any backend ever sees this request, when
        // it carries Media content the bound backend hasn't declared multimodal support for -- every
        // real backend's own outbound translation silently drops what it can't encode (chat_client.
        // hpp's own comment on `validate_outbound_media_capabilities`), so checking here is what
        // turns a silent, unattributable content loss into a real, attributable run failure instead.
        if (auto gate = validate_outbound_media_capabilities(request, chat_client_->capabilities());
            !gate) {
            // ADR-198 §5 (issue #112 B3): no run_failed here -- the caller (run_rounds()) emits the one run_failed
            // for any failed model call. This site and the two leak refusals below used to emit their own, so one
            // failure produced two (AG-UI: two RUN_ERRORs).
            co_return std::unexpected(gate.error());
        }

        result<ChatResponse> response = std::unexpected(
            error{failure_class::contract, "unreachable: neither call path executed", "run.internal"});
        last_stream_failure_.reset();  // ADR-177: only THIS call's drain may leave one behind
        detail::EmitFn const emit = [this](run_event_kind k, RunEventPayload p) {
            emit_run_event(k, std::move(p));
        };

        if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
            // unified-streaming-design-draft.md §3 (Piece A), Rev 7 (Finding 4-new, 5th red-team pass):
            // MUST be `if constexpr`, not a runtime `if` nested inside `if constexpr` -- a runtime `if`
            // does not prune the unreached branch from template instantiation, and
            // `MiddlewareModelCallGateway`/`ContentReplayGateway` (real, currently-used
            // `ModelCallGatewayLike` conformers, e.g. `tests/test_rt_agent_session_content_replay.cpp`)
            // have no `call_stream()` member at all -- a runtime-gated `chat_client_->call_stream(...)`
            // would be a hard compile error the instant either type is instantiated here, regardless of
            // `stream_model_calls_`'s value.
            if constexpr (agentengine::ModelCallGatewayStreamLike<ChatClientT>) {
                if (stream_model_calls_) {
                    response = detail::drain_streaming_response(chat_client_->call_stream(request, ctx),
                                                                  stream_model_calls_, emit, nullptr,
                                                                  ctx.cancellation);
                } else {
                    response = co_await chat_client_->call(request, ctx);
                }
            } else {
                // This concrete gateway type has no call_stream() -- unchanged from before Piece A,
                // regardless of stream_model_calls_. start_run()'s own warning names this narrower case.
                response = co_await chat_client_->call(request, ctx);
            }
        } else {
            if constexpr (requires(ChatClientT& c, ChatRequest const& r, EffectContext& e) {
                              { c.chat(r, e) } -> std::same_as<agentengine::task<result<ChatResponse>>>;
                          }) {
                if (!stream_model_calls_) {
                    response = co_await chat_client_->chat(request, ctx);
                    if (response.has_value() && scan_response_format_leaks_) {
                        response->message = apply_response_format_scan(std::move(response->message), request.tools);
                    } else if (response.has_value() && chat_client_->capabilities().tool_calling) {
                        // OQ-23 (design draft: docs/planning/oq23-undeclared-tool-call-leak-design-
                        // draft.md, Design D) -- reached only when the scan above did NOT run (its own
                        // `else if` makes the two mutually exclusive by construction, not by
                        // happenstance): a raw wire-format leak matching a live tool name, with no
                        // scan armed to recover it, refuses the response instead of silently returning
                        // it as an ordinary text reply.
                        if (auto leak = detect_undeclared_tool_call_leak(response->message, request.tools);
                            !leak) {
                            co_return std::unexpected(leak.error());  // ADR-198 §5: the caller reports it
                        }
                    }
                    co_return response;
                }
            }
            detail::StreamFailure failure;
            response = detail::drain_streaming_response(chat_client_->chat_stream(request, ctx),
                                                          stream_model_calls_, emit, &failure, ctx.cancellation);
            if (!response.has_value() && response.error().code == "run.stream_incomplete") {
                last_stream_failure_ = std::move(failure);
            }
        }

        if (response.has_value() && scan_response_format_leaks_) {
            response->message = apply_response_format_scan(std::move(response->message), request.tools);
        } else if (response.has_value() && chat_client_->capabilities().tool_calling) {
            // OQ-23 -- same check, same reasoning, as the non-streaming early-return branch above.
            if (auto leak = detect_undeclared_tool_call_leak(response->message, request.tools); !leak) {
                co_return std::unexpected(leak.error());  // ADR-198 §5: the caller reports it
            }
        }
        co_return response;
    }

    [[nodiscard]] static std::optional<ChatClientT> make_default_chat_client() {
        if constexpr (std::is_default_constructible_v<ChatClientT>) {
            return std::optional<ChatClientT>(std::in_place);
        } else {
            return std::optional<ChatClientT>{};
        }
    }

    // ---- ADR-199: the hooks AgentSessionCore's loop calls ------------------------------------------
    task<result<ContextContribution>> bound_on_context(SessionContext& session_ctx, EffectContext& ctx) override {
        return history_provider_.on_context(session_ctx, ctx);
    }
    task<std::monostate> bound_on_turn_end(TurnView const& turn, EffectContext& ctx) override {
        return history_provider_.on_turn_end(turn, ctx);
    }
    void bound_filter_cross_provider_reasoning(ContextContribution& contribution) override {
        // `if constexpr` on `HasProducerChatClientId`, never a runtime branch, so a client that names no identity
        // (every mock) is unaffected.
        if constexpr (agentengine::HasProducerChatClientId<ChatClientT>) {
            if (chat_client_) {
                detail::filter_cross_provider_reasoning(
                    contribution, chat_client_->producer_chat_client_id(),
                    [this](run_event_kind k, RunEventPayload p) { emit_run_event(k, std::move(p)); });
            }
        } else {
            (void)contribution;
        }
    }
    [[nodiscard]] bool bound_has_chat_client() const noexcept override { return chat_client_.has_value(); }
    [[nodiscard]] model_route bound_model_route() const noexcept override {
        // Nested exactly as run_model_call() and the former start_run() warning branched.
        if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
            if constexpr (agentengine::ModelCallGatewayStreamLike<ChatClientT>) {
                return model_route::gateway_streaming;
            } else {
                return model_route::gateway;
            }
        } else {
            return model_route::direct;
        }
    }

    std::optional<ChatClientT> chat_client_ = make_default_chat_client();
    StateT                     state_{};
    HistoryProviderT           history_provider_;
};

// Save `session`'s narrowed durable record under its own session_id() -- see file banner and
// snapshot_record()'s own comment for the in-flight guard this relies on. `session` is non-const
// (not const&, unlike the Quark original) because acquiring session_mutex_ mutates the mutex's own
// state even though this operation is logically a read of session data.
template <class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
[[nodiscard]] task<result<void>> save_agent_session_snapshot(
    AgentSession<ChatClientT, StateT, HistoryProviderT>& session, StoreT& store) {
    AgentSessionRecord rec = co_await session.snapshot_record();
    co_return store.save(rec.session_id, encode_agent_session_record(rec));
}

// Load the latest durable record for `session_id`, or std::nullopt if it was never snapshotted or
// was deleted (delete_session()'s tombstone) -- a caller of THIS function sees no distinction
// between "never existed" and "deleted", matching the Quark original's own read-path property.
// Synchronous (no task<T>) -- unlike save/delete, this never touches a live AgentSession instance,
// so there is no in-flight state to guard against.
template <SessionStore StoreT>
[[nodiscard]] result<std::optional<AgentSessionRecord>> load_agent_session_snapshot(
    StoreT const& store, std::string const& session_id) {
    if (!store.exists(session_id)) return std::optional<AgentSessionRecord>{};
    result<std::vector<std::byte>> bytes = store.load(session_id);
    if (!bytes) return std::unexpected(bytes.error());
    result<AgentSessionRecord> rec = decode_agent_session_record(*bytes);
    if (!rec) return std::unexpected(rec.error());
    if (rec->deleted) return std::optional<AgentSessionRecord>{};
    return std::optional<AgentSessionRecord>{std::move(*rec)};
}

// Gap-15 fix (2026-08-14, decisions/ADR-043-*.md): 005 §2's exact policy vocabulary
// ("acknowledged... only after its effects and history delta are durable, or the session declares
// an at_most_once_ack durability policy"). `at_most_once` is today's only real behavior (a bare
// start_run()/resolve_interaction() call, unchanged) -- `require_durable` is new, wired ONLY through
// the two *_with_ack_policy() free functions below, never inside AgentSession's own methods
// (checkpoint_if_due's own comment three declarations up: "AgentSession has no ambient Store access,
// I2" -- this policy switch honors that same boundary rather than giving AgentSession a
// self-referencing Store hook).
// ae-naming-lint: allow ack_policy — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class ack_policy : std::uint8_t { at_most_once, require_durable };

// 005 §2's "history delta" specifically -- the messages ONE turn added, not the whole conversation.
// AgentSessionRecord's own comment already names full-history serialization as a separate, larger,
// not-yet-built gap; this is deliberately narrower and, unlike that, tractable today: reuses
// rt/message_codec.hpp's already-proven Message<->JSON codec (built for WorkflowSupervisor's own
// checkpoint record, ADR-037 Phase 3 Slice 2) rather than inventing a second one.
// ae-naming-lint: allow TurnDeltaRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct TurnDeltaRecord {
    std::string session_id;
    std::uint64_t turn_index = 0;
    std::vector<Message> messages;
};

[[nodiscard]] inline json::Value turn_delta_record_to_json(TurnDeltaRecord const& rec) {
    std::vector<json::Value> messages;
    messages.reserve(rec.messages.size());
    // Explicitly qualified, not bare -- the message_codec.hpp file banner comment above (near
    // #include "agentengine/rt/message_codec.hpp") explains the ADL hazard: Message lives in
    // namespace agentengine directly, so an unqualified call is ambiguous in any TU that also
    // includes core/chat_recording.hpp's own, separately-maintained same-named function.
    for (Message const& m : rec.messages) messages.push_back(agentengine::rt::message_to_json(m));
    return json::Value::make_object({
        {"session_id", json::Value::make_string(rec.session_id)},
        {"turn_index", json::Value::make_number(static_cast<double>(rec.turn_index))},
        {"messages", json::Value::make_array(std::move(messages))},
    });
}

[[nodiscard]] inline result<TurnDeltaRecord> turn_delta_record_from_json(json::Value const& v) {
    json::Value const* session_id = v.find("session_id");
    json::Value const* turn_index = v.find("turn_index");
    json::Value const* messages   = v.find("messages");
    if (session_id == nullptr || !session_id->is_string() || turn_index == nullptr ||
        !turn_index->is_number() || messages == nullptr || !messages->is_array()) {
        return std::unexpected(error{failure_class::contract, "malformed TurnDeltaRecord",
                                      "rt.agent_session.turn_delta.malformed"});
    }
    TurnDeltaRecord rec;
    rec.session_id = session_id->as_string();
    rec.turn_index = static_cast<std::uint64_t>(turn_index->as_number());
    rec.messages.reserve(messages->as_array().size());
    for (json::Value const& item : messages->as_array()) {
        result<Message> parsed = message_from_json(item);
        if (!parsed) return std::unexpected(parsed.error());
        rec.messages.push_back(std::move(*parsed));
    }
    return rec;
}

[[nodiscard]] inline std::vector<std::byte> encode_turn_delta_record(TurnDeltaRecord const& rec) {
    std::string const text = json::dump(turn_delta_record_to_json(rec));
    auto const* const first = reinterpret_cast<std::byte const*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

[[nodiscard]] inline result<TurnDeltaRecord> decode_turn_delta_record(std::vector<std::byte> const& bytes) {
    std::string const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    result<json::Value> parsed = json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return turn_delta_record_from_json(*parsed);
}

// The key one turn's delta is stored under -- distinct from AgentSessionRecord's own key
// (session_id alone), namespaced per turn so a later restore can find exactly the delta a specific
// turn_index wrote, and so consecutive turns don't overwrite each other's durable record.
[[nodiscard]] inline std::string turn_delta_store_key(std::string const& session_id,
                                                        std::uint64_t turn_index) {
    return session_id + ":turn:" + std::to_string(turn_index);
}

// Durably writes `turn_index`'s own history delta to the SAME SessionStore instance
// `save_agent_session_snapshot()` already writes to, under a per-turn key. `delta` must be exactly
// the messages ONE turn added -- `start_run_with_ack_policy()`/`resolve_interaction_with_ack_policy()`
// below are the only real callers.
template <SessionStore StoreT>
[[nodiscard]] inline task<result<void>> save_turn_delta(StoreT& store, std::string const& session_id,
                                                          std::uint64_t turn_index,
                                                          std::span<Message const> delta) {
    TurnDeltaRecord rec{session_id, turn_index, std::vector<Message>(delta.begin(), delta.end())};
    co_return store.save(turn_delta_store_key(session_id, turn_index), encode_turn_delta_record(rec));
}

// Symmetric reader, for a future restore path to consume -- not itself wired into
// load_agent_session_snapshot() (rehydrating history_ on restart is a separate, larger question this
// ADR does not answer; see the ADR's own "what this does not claim").
template <SessionStore StoreT>
[[nodiscard]] inline result<std::optional<TurnDeltaRecord>> load_turn_delta(
    StoreT const& store, std::string const& session_id, std::uint64_t turn_index) {
    std::string const key = turn_delta_store_key(session_id, turn_index);
    if (!store.exists(key)) return std::optional<TurnDeltaRecord>{};
    result<std::vector<std::byte>> bytes = store.load(key);
    if (!bytes) return std::unexpected(bytes.error());
    result<TurnDeltaRecord> rec = decode_turn_delta_record(*bytes);
    if (!rec) return std::unexpected(rec.error());
    return std::optional<TurnDeltaRecord>{std::move(*rec)};
}

// 005 §2's ack-durability contract, honored WITHOUT giving AgentSession ambient Store access (I2,
// matching checkpoint_if_due's own comment above): the CALLER supplies both the session and the
// store, this function just sequences them correctly. For `at_most_once` (today's only real
// behavior, unchanged), behaves exactly like calling `session.start_run()` directly. For
// `require_durable`, the turn's own history delta AND the session's bookkeeping record must both
// durably write before the caller sees a successful response -- if either fails, the caller gets an
// error, never a false "acknowledged" success (005 §2's own named failure mode: "silently
// acknowledging before durability... loses a user's conversation on a crash").
//
// NAMED RESIDUAL, not fixed here: the delta capture below (history().size() before/after) is correct
// for the single-caller-at-a-time usage 005 §2's own G2 gate describes (sequential turn-taking on
// one session), but is NOT safe against a second, concurrently-overlapping start_run() call on the
// SAME session instance racing between this wrapper's own call returning and its subsequent
// history() read -- I1's FIFO session_mutex_ serializes each individual start_run()/
// resolve_interaction() call, but does not extend that critical section to this wrapper's own
// post-hoc read. Closing that fully would mean capturing the delta INSIDE run_rounds()'s own locked
// region -- a run_rounds() change this pass deliberately avoids, matching the audit's own "the
// proposed insertion point breaks a tested invariant" caution (that concern was about inserting a
// durability wait relative to run_finished's emission; this residual is a different, narrower one
// about the delta-capture window specifically). Real, scoped follow-up work, not silently accepted.
template <class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
[[nodiscard]] task<result<AgentResponse>> start_run_with_ack_policy(
    AgentSession<ChatClientT, StateT, HistoryProviderT>& session, StartRun request, ack_policy policy,
    StoreT& store) {
    std::size_t const history_before = session.history().size();
    result<AgentResponse> response = co_await session.start_run(std::move(request));
    if (!response) co_return response;  // a failed run was never a turn to ack in the first place
    if (policy == ack_policy::require_durable) {
        std::span<Message const> const delta(session.history().data() + history_before,
                                              session.history().size() - history_before);
        std::uint64_t const turn_index = session.to_record().turn_index;
        result<void> delta_saved = co_await save_turn_delta(store, session.session_id(), turn_index, delta);
        if (!delta_saved) {
            co_return std::unexpected(
                error{failure_class::resource,
                      "turn completed but the required durable history-delta write failed: " +
                          delta_saved.error().message,
                      "run.durable_ack_failed"});
        }
        result<void> snapshot_saved = co_await save_agent_session_snapshot(session, store);
        if (!snapshot_saved) {
            co_return std::unexpected(
                error{failure_class::resource,
                      "turn completed and its history delta is durable, but the session bookkeeping "
                      "write failed: " +
                          snapshot_saved.error().message,
                      "run.durable_ack_failed"});
        }
    }
    co_return response;
}

// Same contract as start_run_with_ack_policy() above, for the OTHER real caller-facing entry point
// that can complete a turn (a suspended interaction resuming after human approval, 001 §2) -- 005 §2's
// "acknowledged to the caller" applies equally to both; this is not a second, independently-reasoned
// mechanism, just the identical sequencing applied to resolve_interaction()'s own result shape.
template <class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
[[nodiscard]] task<result<AgentResponse>> resolve_interaction_with_ack_policy(
    AgentSession<ChatClientT, StateT, HistoryProviderT>& session, ResolveInteraction request,
    ack_policy policy, StoreT& store) {
    std::size_t const history_before = session.history().size();
    result<AgentResponse> response = co_await session.resolve_interaction(std::move(request));
    if (!response) co_return response;
    if (policy == ack_policy::require_durable) {
        std::span<Message const> const delta(session.history().data() + history_before,
                                              session.history().size() - history_before);
        std::uint64_t const turn_index = session.to_record().turn_index;
        result<void> delta_saved = co_await save_turn_delta(store, session.session_id(), turn_index, delta);
        if (!delta_saved) {
            co_return std::unexpected(
                error{failure_class::resource,
                      "turn completed but the required durable history-delta write failed: " +
                          delta_saved.error().message,
                      "run.durable_ack_failed"});
        }
        result<void> snapshot_saved = co_await save_agent_session_snapshot(session, store);
        if (!snapshot_saved) {
            co_return std::unexpected(
                error{failure_class::resource,
                      "turn completed and its history delta is durable, but the session bookkeeping "
                      "write failed: " +
                          snapshot_saved.error().message,
                      "run.durable_ack_failed"});
        }
    }
    co_return response;
}

// Same cadence-policy shape as the Quark original's CheckpointCadence<N> -- a pure function of how
// many turns have completed since the last checkpoint actually written, zero Quark dependency,
// ported unchanged.
template <std::uint32_t EveryNTurns>
    requires(EveryNTurns >= 1)
// ae-naming-lint: allow CheckpointCadence — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct CheckpointCadence {
    [[nodiscard]] static constexpr bool due(std::uint64_t turns_since_last_checkpoint) noexcept {
        return turns_since_last_checkpoint >= EveryNTurns;
    }
};

// The cadence-gated way a host calls save_agent_session_snapshot() at a turn boundary --
// `turns_since_last_checkpoint` is the CALLER's own count (this function does no bookkeeping of its
// own -- AgentSession has no ambient Store access, I2). Returns false (not an error) when the
// cadence skips a write; result<bool> still surfaces a real store failure on the turns that DO
// attempt one.
template <class CadenceT, class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
[[nodiscard]] task<result<bool>> checkpoint_if_due(
    AgentSession<ChatClientT, StateT, HistoryProviderT>& session, StoreT& store,
    std::uint64_t turns_since_last_checkpoint) {
    if (!CadenceT::due(turns_since_last_checkpoint)) co_return false;
    result<void> saved = co_await save_agent_session_snapshot(session, store);
    if (!saved) co_return std::unexpected(saved.error());
    co_return true;
}

// Same "hard removal, with a completion receipt" shape as the Quark original -- a receipt naming
// which of the two halves (durable tombstone, in-process clear) actually happened, since either can
// independently fail.
// ae-naming-lint: allow SessionDeletionReceipt — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct SessionDeletionReceipt {
    std::string session_id;
    bool        durable_record_removed = false;
    bool        in_process_state_cleared = false;
};

template <class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
[[nodiscard]] task<result<SessionDeletionReceipt>> delete_session(
    AgentSession<ChatClientT, StateT, HistoryProviderT>& session, StoreT& store) {
    SessionDeletionReceipt receipt{};
    receipt.session_id = session.session_id();

    // ADR-061 §24.1: goes through the named factory, not a direct field-by-field build -- this exact
    // site was the real, hand-built AgentSessionRecord construction §23a Finding 1 found silently
    // contradicting the struct's own "only two places" comment.
    AgentSessionRecord tombstone = make_tombstone_record(receipt.session_id);
    result<void> saved = store.save(receipt.session_id, encode_agent_session_record(tombstone));
    if (!saved) co_return std::unexpected(saved.error());
    receipt.durable_record_removed = true;

    co_await session.clear_in_process_state_locked();
    receipt.in_process_state_cleared = true;

    co_return receipt;
}

}  // namespace agentengine::rt
