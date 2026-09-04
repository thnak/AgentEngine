#pragma once
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/standing_effect.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_call_hook.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/bounded_call_fanout.hpp"
#include "agentengine/core/turn_middleware.hpp"
#include "agentengine/rt/agent_session_trust.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/interaction_codec.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/standing_effect_registry.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/principal.hpp"

// ADDENDUM: docs/planning/agent-spawn-runtime-design-draft.md §4.2/§9 RC-1 (OpenQuestions.md OQ-14,
// `agent.spawn`'s nested-run mechanism) adds ONE new, additive-only, opt-in member to this class --
// `set_background_execution_disabled()`/`background_execution_disabled()` -- and one new guard at
// the top of `start_background_task()` below. See that setter's own comment for the full rationale;
// every existing session is byte-for-byte unaffected until a caller opts in.

namespace agentengine::rt {

// Reused, unchanged shape (matching agentengine::NoSessionState). A distinct type from the
// core/agent_session.hpp one, deliberately -- Slice 1 does not depend on that header at all, keeping
// this file's own Quark-free claim easy to verify by inspection (no transitive include of anything
// that pulls quark/*).
// ae-naming-lint: allow NoSessionState — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct NoSessionState {};

// Same narrowed, wire-shape-motivated-but-still-correct admission identity as
// agentengine::SessionCaller -- see file banner for why this slice keeps the shape even though the
// byte-budget that originally forced it no longer applies.
// ae-naming-lint: allow SessionCaller — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct SessionCaller {
    std::string id;
    std::string tenant_id;
};

// ADR-061 §20.1: the per-request identity+grant bundle a Tier-3 (host-fronted HTTP) dispatcher
// supplies, in place of the coarser `SessionCaller`. Deliberately ONE bundle carrying identity and
// capability grant TOGETHER, not two fields that must be kept in agreement -- §20.1's own rationale:
// keeping `caller` (identity) and a separate capabilities field as two things that must agree is
// exactly the two-sources-of-truth shape this ADR found buggy repeatedly elsewhere. A session with
// `require_authority_ == true` (`set_require_authority()`, below) consults ONLY `authority`, never
// `caller`, on a `StartRun`/`ResolveInteraction` that carries both (ADR-061 §20.4) -- there is no
// agreement check because there is no code path where both are read.
// ae-naming-lint: allow RequestAuthority — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RequestAuthority {
    agentengine::Principal principal;  // per-request identity, distinct from the session's own principal_
    // Owned, not borrowed: the CapabilitySet a per-request bearer credential resolves to must outlive
    // the coroutine frame that constructed this RequestAuthority (ADR-061 §20.1/§20.3) -- a raw
    // pointer/reference into that frame would dangle the moment start_run()/resolve_interaction()
    // returns, while EffectContext::capabilities (core/effect_context.hpp) is read again by a LATER,
    // unrelated call.
    std::shared_ptr<agentengine::CapabilitySet const> capabilities;
    std::chrono::steady_clock::time_point expiry{};
    [[nodiscard]] bool live(std::chrono::steady_clock::time_point now) const noexcept {
        return now < expiry;
    }
};

// ae-naming-lint: allow StartRun — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct StartRun {
    Message input;
    std::optional<SessionCaller> caller = std::nullopt;
    // ADR-061 §20.1: additive, defaulted -- every existing `StartRun{input}`/`StartRun{input, caller}`
    // call site is unaffected. Consulted only by a `require_authority_ == true` session (§20.4); a
    // non-Tier-3 session's admission check is byte-for-byte unchanged from before this field existed.
    std::optional<RequestAuthority> authority = std::nullopt;
};

// ae-naming-lint: allow ResolveInteraction — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ResolveInteraction {
    std::string interaction_id;
    bool        approved = false;
    std::optional<SessionCaller> caller = std::nullopt;
    // ADR-057 §9: additive -- interpreted ONLY when the named interaction's own `reason ==
    // interaction_reason::codeact_ask`; `approved` above stays exactly as-is, interpreted only for
    // `reason == interaction_reason::approval`. Appended last (this project's own established
    // field-ordering convention -- every existing positional `ResolveInteraction{a,b,c}` call site
    // is unaffected, the 4th field simply defaults to `std::nullopt`).
    std::optional<std::string> answer = std::nullopt;
    // ADR-061 §20.1: same additive, defaulted shape as StartRun::authority above.
    std::optional<RequestAuthority> authority = std::nullopt;
    // OQ-21: additive, appended last (this struct's own established field-ordering convention).
    // Interpreted ONLY when the named interaction's own `reason == interaction_reason::
    // hook_decision`; `approved`/`answer` above stay exactly as-is, interpreted only for their own
    // reasons. A vector, not a single field -- core/tool_call_hook.hpp's `HookDispatchAnswer` own
    // comment explains why (a round may have multiple calls pending external dispatch at once).
    std::optional<std::vector<agentengine::HookDispatchAnswer>> hook_dispatch_answers = std::nullopt;
};

struct AgentResponse {
    Message message;
    Usage   usage;
    // ADR-058 §8 (Design B) -- additive, appended last (this project's own established field-
    // ordering discipline; ADR-058 §4 B4 confirmed exactly one positional-aggregate
    // `AgentResponse{...}` construction site exists in the whole tree, so this keeps it compiling
    // unchanged). Populated only when a session has `set_output_schema()` configured AND the
    // converged response's text content validated successfully against it -- raw, still-erased JSON
    // text; the caller who owns the real T parses it a second time via
    // `schema::from_json_value<T>`/`schema::from_json<T>` (AgentSession itself never needs to know
    // T, matching ADR-058 §4 B2). `nullopt` means either no OutputSchema<T> was declared for this
    // session, or the run never reached a converged response.
    std::optional<std::string> structured_output_json;
};

// Slice 3's `BackgroundTaskDone`/`BackgroundCompletionQueue` now live in
// rt/standing_effect_registry.hpp (docs/planning/agent-session-decomposition-design-draft.md §2a)
// -- included above, same `agentengine::rt` namespace, so every existing reference to either name
// in this file keeps compiling unchanged.

// ADR-053 §5's own named follow-up, closed here: `schedule_wakeup` exposed as a real, MODEL-callable
// declared tool (006 §6b: "declared tools gated by a new capability... `Schedule<max_horizon,
// max_active>`"; 019 §2's own "Agent-callable, not just host-triggered" paragraph: "`schedule_wakeup`
// ... let[s] the model itself arm a Timer/schedule ... and then end its turn ... This adds a caller,
// not a new state machine"). Args/Reply are intentionally minimal, matching the already-Judged
// `AgentSession::schedule_wakeup(delay, label, now)` C++ API shape exactly. `now` is deliberately NOT
// a model-suppliable argument -- I3 (model output is data, never authority) means the model does not
// get to assert what time it currently is; the glue below (`run_rounds()`) reads real wall-clock time
// once, at the actual moment of invocation, the same "recorded seam" any other host-triggered call in
// this codebase reads real time at -- the model supplies only WHAT it wants (`delay_ms`, `label`).
// ae-naming-lint: allow ScheduleWakeupArgs — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ScheduleWakeupArgs {
    std::uint64_t delay_ms = 0;
    std::string   label;
};
AE_JSON_SCHEMA(ScheduleWakeupArgs, delay_ms, label)

// ae-naming-lint: allow ScheduleWakeupReply — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ScheduleWakeupReply {
    std::string handle_id;
};
AE_JSON_SCHEMA(ScheduleWakeupReply, handle_id)

// `invoke()` is an unreachable poison sentinel, matching ADR-028's own `CounterTool` precedent
// (test_rt_agent_session_tooling_and_delegation.cpp) -- real dispatch never reaches this static
// method. It exists only so `ScheduleWakeupTool` satisfies `Tool<Derived,...>`'s CRTP contract (a
// real Args/Reply/name/description/schema surface for `make_tool_descriptor_with_invoke<
// ScheduleWakeupTool>()`, below, to extract at construction time). The actual dispatch happens
// through the closure `run_rounds()` supplies, which reaches back into the owning `AgentSession`
// directly by capturing `this` -- the one place in this codebase that CAN do that, since
// `EffectContext` carries no seam back to the session (ADR-028 §1's own exhaustive check, confirmed
// unchanged) and no `ContextProvider` owns a back-reference to its `AgentSession` either. No
// `Capabilities<...>` policy tag is declared here deliberately: the REAL enforcement (does the
// session hold a `cap::Schedule` grant at all; does this delay fit its `max_horizon`; is its
// `max_active` already at capacity) is a LIVE, per-call check against a runtime count -- the exact
// same reason `Background<max_concurrent>`'s own enforcement lives inside `background_task()`'s body
// rather than a static `ToolDescriptor::capability_ceiling` entry, not this tool's own compile-time
// declared ceiling (which a generic `invoke_tool()` step-4/7 bind could only check for bare
// existence, never the live count `schedule_wakeup_impl()` (ADR-061 §20.5) itself already checks
// correctly).
// ae-naming-lint: allow ScheduleWakeupTool — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ScheduleWakeupTool : agentengine::Tool<ScheduleWakeupTool> {
    static constexpr std::string_view name = "schedule_wakeup";
    static constexpr std::string_view description =
        "Arms a durable wake condition that fires after the given delay (019 §2's Timer/schedule wake "
        "row), then the run stays suspended until the host observes the wake is due and resumes it. "
        "Requires a granted Schedule<max_horizon, max_active> capability.";
    using Args  = ScheduleWakeupArgs;
    using Reply = ScheduleWakeupReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "schedule_wakeup invoked without its session-bound dispatch closure -- this static "
            "method must never actually run",
            "schedule_wakeup.unreachable_static_invoke"});
    }
};

// ADR-057 §9 (Design B: abort-and-replay for `agent.ask()`, 026 §5): the host-side replay record a
// suspended `codeact_ask` `Interaction` needs to resume -- keyed by `interaction_id` in
// `AgentSession::pending_codeact_asks_` (below), NOT carried in the `Interaction` record itself
// (which stays the same small, uniform shape every reason uses). `source`/`language` are the
// ORIGINAL model-issued call's own arguments, captured once when the ask first suspends the round --
// re-invoking `execute_code` on resolve replays against these, never anything the model supplies
// again, which is exactly what makes this host-driven replay rather than a new model-issued call.
// `answers_so_far` grows by one element per `resolve_interaction()` call against this
// `interaction_id` (ADR-057 §9: "chaining through as many questions as one script asks without
// minting a new interaction_id per question"). Deliberately NOT durably checkpointed (no codec, no
// field in `AgentSessionRecord`) -- the same "not yet solved" scope `Interaction::
// expires_at_ns` already carries project-wide (ADR-029 §6), not a new gap this ADR introduces.
// ae-naming-lint: allow PendingCodeActAsk — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct PendingCodeActAsk {
    std::string              source;
    std::string              language;
    std::vector<std::string> answers_so_far;
    std::string               tool_call_id;
    // The most recently raised prompt -- what a caller re-reading `open_interactions()` after a
    // second/third ask-pending would want to show; not itself load-bearing for the replay mechanism
    // (the STORED source/language/answers are what actually drive the re-run).
    std::string               prompt;
};

// Slice 2's narrowed durable record -- see file banner for exactly what is and isn't carried
// (notably: no created_at_ns/updated_at_ns, a deliberate narrowing vs. the Quark original's own
// AgentSessionRecord; history/state/metadata are likewise not carried, same "no Message/ContentItem
// serialization yet" gap the original named). `to_record()`/`restore_from_record()` (AgentSession
// member functions, below) and `make_tombstone_record()` (free function, below) are the only three
// places that cross between the in-process type and this shape -- ADR-061 §24.1 found
// `delete_session()` had been hand-building one directly, silently contradicting this comment's own
// former "only two places" claim; `make_tombstone_record()` closes that gap rather than leaving a
// fourth hand-built site for the next one to find.
// ae-naming-lint: allow AgentSessionRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct AgentSessionRecord {
    std::string session_id;
    std::string principal_id;
    std::string principal_tenant_id;
    bool deleted = false;
    std::uint64_t run_counter = 0;
    std::uint64_t turn_index = 0;
    std::vector<Interaction> open_interactions;
    // ADR-061 §22.1: whether this session required per-request authority (Tier 3). Carried through
    // fork_from()/restore_from_record() (below) so a fork or a restart of a Tier-3 session cannot
    // silently downgrade to the caller-only admission path -- fail-closed direction (§21a Finding 1).
    bool require_authority = false;
};

// ADR-061 §24.1: the ONE other sanctioned way to construct an AgentSessionRecord outside
// to_record() -- replacing delete_session()'s previous direct field-by-field build. The
// `require_authority` value chosen here is inert either way (load_agent_session_snapshot(), below,
// returns nullopt for any deleted==true record before require_authority is ever read back), but it
// is now a real, explicit choice this function states, not an omission the type system happened to
// paper over.
[[nodiscard]] inline AgentSessionRecord make_tombstone_record(std::string session_id) {
    AgentSessionRecord rec;
    rec.session_id        = std::move(session_id);
    rec.deleted            = true;
    rec.require_authority = false;
    return rec;
}

// interaction_to_json()/interaction_from_json() live in interaction_codec.hpp -- shared with
// rt::WorkflowSupervisor's own record codec (see that header's own banner for why this used to be a
// duplicated copy here and isn't anymore).

[[nodiscard]] inline json::Value agent_session_record_to_json(AgentSessionRecord const& rec) {
    std::vector<json::Value> interactions;
    interactions.reserve(rec.open_interactions.size());
    for (Interaction const& i : rec.open_interactions) interactions.push_back(interaction_to_json(i));
    return json::Value::make_object({
        {"session_id", json::Value::make_string(rec.session_id)},
        {"principal_id", json::Value::make_string(rec.principal_id)},
        {"principal_tenant_id", json::Value::make_string(rec.principal_tenant_id)},
        {"deleted", json::Value::make_bool(rec.deleted)},
        {"run_counter", json::Value::make_number(static_cast<double>(rec.run_counter))},
        {"turn_index", json::Value::make_number(static_cast<double>(rec.turn_index))},
        {"open_interactions", json::Value::make_array(std::move(interactions))},
        {"require_authority", json::Value::make_bool(rec.require_authority)},
    });
}

// ADR-061 §22.1: `require_authority` is REQUIRED below, matching this function's own established
// strictness for every other field (deleted/run_counter/turn_index are all required, none defaulted-
// on-absence) -- a deliberate breaking change to the record wire schema: a pre-this-change persisted
// snapshot fails to deserialize ("malformed AgentSessionRecord") rather than silently defaulting
// require_authority to false. Accepted without a migration path: no real snapshot deployment exists
// yet (Milestones 8-9, where one first would, have not started).
[[nodiscard]] inline result<AgentSessionRecord> agent_session_record_from_json(json::Value const& v) {
    json::Value const* session_id           = v.find("session_id");
    json::Value const* principal_id         = v.find("principal_id");
    json::Value const* principal_tenant_id  = v.find("principal_tenant_id");
    json::Value const* deleted              = v.find("deleted");
    json::Value const* run_counter          = v.find("run_counter");
    json::Value const* turn_index           = v.find("turn_index");
    json::Value const* open_interactions    = v.find("open_interactions");
    json::Value const* require_authority    = v.find("require_authority");
    if (session_id == nullptr || !session_id->is_string() || principal_id == nullptr ||
        !principal_id->is_string() || principal_tenant_id == nullptr ||
        !principal_tenant_id->is_string() || deleted == nullptr || !deleted->is_bool() ||
        run_counter == nullptr || !run_counter->is_number() || turn_index == nullptr ||
        !turn_index->is_number() || open_interactions == nullptr || !open_interactions->is_array() ||
        require_authority == nullptr || !require_authority->is_bool()) {
        return std::unexpected(error{failure_class::contract, "malformed AgentSessionRecord",
                                      "rt.agent_session.record.malformed"});
    }
    AgentSessionRecord rec;
    rec.session_id          = session_id->as_string();
    rec.principal_id        = principal_id->as_string();
    rec.principal_tenant_id = principal_tenant_id->as_string();
    rec.deleted             = deleted->as_bool();
    rec.run_counter         = static_cast<std::uint64_t>(run_counter->as_number());
    rec.turn_index          = static_cast<std::uint64_t>(turn_index->as_number());
    rec.require_authority   = require_authority->as_bool();
    rec.open_interactions.reserve(open_interactions->as_array().size());
    for (json::Value const& item : open_interactions->as_array()) {
        result<Interaction> parsed = interaction_from_json(item);
        if (!parsed) return std::unexpected(parsed.error());
        rec.open_interactions.push_back(std::move(*parsed));
    }
    return rec;
}

[[nodiscard]] inline std::vector<std::byte> encode_agent_session_record(AgentSessionRecord const& rec) {
    std::string const text = json::dump(agent_session_record_to_json(rec));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] inline result<AgentSessionRecord> decode_agent_session_record(
    std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
    result<json::Value> parsed = json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return agent_session_record_from_json(*parsed);
}

namespace agent_session_detail {
// ADR-102 Phase 5: `fork_from()`'s own synchronous acquisition of `source.session_mutex_` --
// `AsyncMutex::lock()` is `co_await`-only, and `fork_from()` itself stays a plain, synchronous
// function (no call site anywhere in this codebase should need to change), so the acquisition is
// driven through `agentengine::rt::block_on()`, matching the same "drive an AsyncMutex-guarded
// operation from a non-coroutine call site" discipline `sandbox/mandatory_sandbox_provider.hpp`
// already established for the identical shape of problem, and safe for the identical reason: unlike a
// naive "resume until done" loop (a real, ASan-confirmed use-after-free hazard under genuine
// cross-thread contention, `rt/block_on.hpp`'s own top comment), `block_on()`'s dedicated
// final-suspend-as-last-touch driver survives a lock that genuinely parks and is later resumed by a
// DIFFERENT thread's `unlock()` -- exactly the scenario this fix exists to make `fork_from()` safe
// under.
[[nodiscard]] inline agentengine::rt::task<AsyncMutex::Guard> acquire_session_mutex(AsyncMutex& m) {
    co_return co_await m.lock();
}

// ADR-116 follow-on (2026-08-30, independent red-team, same day): a monotonically-increasing,
// process-wide, NEVER-reused session-identity counter. `ComposedContextProvider`'s own `owner_` tag
// used to be a raw `this` (a `void const*`) -- a real, empirically-confirmed ABA hole, not a
// theoretical one: heap-allocate a session, extract its `history_provider()` via move-construction
// (tagged with that session's address), destroy the session, heap-allocate a SECOND, completely
// unrelated session, and the CRT allocator handing back the exact same freed block (reliably
// reproduced against this codebase's own `ComposedQuickstartSessionBuilder`, which heap-allocates
// `AgentSession` via `make_unique`) makes the new session's own address collide with the stale
// extracted instance's tag -- `operator=`'s guard sees two "matching" non-null tags and wrongly allows
// the merge, leaking the FIRST session's content into the SECOND. A plain, ever-incrementing counter
// never repeats across the life of the process (2^64 sessions is not a real exhaustion path), so
// tagging with THIS instead of `this` closes the hole structurally rather than relying on allocator
// behavior never colliding. `inline` (C++17) so every translation unit -- regardless of which
// `AgentSession<...>` specialization it instantiates -- shares the exact same counter; a per-
// specialization static would let two different `ChatClientT`/`StateT`/`HistoryProviderT`
// combinations each start counting from 1 and collide with each other instead.
inline std::atomic<std::uint64_t> g_next_session_identity{1};
}  // namespace agent_session_detail

template <class ChatClientT, class StateT = NoSessionState,
          class HistoryProviderT = agentengine::HistoryProvider<agentengine::Window<0>>>
    requires (agentengine::ChatClient<ChatClientT> || agentengine::ModelCallGatewayLike<ChatClientT>) &&
             agentengine::ContextProvider<HistoryProviderT>
class AgentSession {
public:
    // Configuration-time, like every setter below -- called once, before the first start_run(), from
    // whatever owns this instance. No actor framework constructs this for you anymore (there is no
    // TestKit<A>-style default-construction expectation this slice needs to satisfy), so a normal
    // constructor is fine -- kept as a separate initialize() anyway, matching the original API shape
    // exactly, since most existing call sites (once ported in a later slice) construct-then-configure.
    void initialize(std::string session_id, agentengine::Principal principal,
                     std::optional<std::uint64_t> token_budget = std::nullopt,
                     std::optional<std::uint64_t> max_turns = std::nullopt) {
        session_id_   = std::move(session_id);
        principal_    = std::move(principal);
        token_budget_ = token_budget;
        max_turns_    = max_turns;
    }

    template <class... Args>
    ChatClientT& emplace_chat_client(Args&&... args) {
        return chat_client_.emplace(std::forward<Args>(args)...);
    }
    [[nodiscard]] bool has_chat_client() const noexcept { return chat_client_.has_value(); }

    // ADR-061 §26.1: no longer `noexcept` (§28.1) -- constructing the owning `shared_ptr`'s control
    // block is a real allocation (the non-owning `(pointer, deleter)` form still allocates a control
    // block even though it never deletes the pointee), so this can now throw `std::bad_alloc`. Cold
    // setup path only (CONVENTIONS.md: "Exceptions may surface only from cold setup paths") -- never
    // called from a per-request or protocol-dispatch path; every real caller is session construction/
    // wiring code. A caller of this function must not itself be `noexcept` without wrapping the call
    // (ADR-061 §29b proved the intended try/catch pattern degrades gracefully; a `noexcept` caller
    // that does not catch still terminates, confirmed by the same round's negative control).
    void set_capabilities(agentengine::CapabilitySet const* capabilities) {
        capabilities_ = std::shared_ptr<agentengine::CapabilitySet const>(
            capabilities, [](agentengine::CapabilitySet const*) noexcept {});
    }
    [[nodiscard]] agentengine::CapabilitySet const* capabilities() const noexcept {
        return capabilities_.get();
    }

    // ADR-061 §20.2: session-level, set once at wiring time -- replacing an earlier, abandoned design
    // (§17.2) that put this on every StartRun/ResolveInteraction message individually, which §21a
    // Finding proved forgettable on a session's second message. A Tier-3 listener wiring up a session
    // it fronts MUST call this with `true` unconditionally; this ADR does not consider a default-
    // `false` Tier-3 session a safe configuration (§20.2's own named residual -- no construction-level
    // guard catches a listener that forgets this call; it is a real, still-open, documented risk, not
    // silently assumed closed).
    void set_require_authority(bool require) noexcept { require_authority_ = require; }
    [[nodiscard]] bool require_authority() const noexcept { return require_authority_; }

    void set_approval_decider(agentengine::ApprovalDecider approve) { approval_decider_ = std::move(approve); }
    [[nodiscard]] agentengine::ApprovalDecider const& approval_decider() const noexcept {
        return approval_decider_;
    }

    // OQ-21 (core/tool_call_hook.hpp): unset (`nullptr`) by default -- every existing session is
    // completely unaffected until a host opts in. Runs once per round, per call, in `run_rounds()`'s
    // own hook-stage block, strictly BEFORE the suspend-for-approval pre-check and `ApprovalDecider`
    // -- see that block's own comment for why sequencing (not two independent gates) is what closes
    // OQ-21's own "two-independent-gates ambiguity" finding. Compile-time, host/deployer-assembled
    // only -- never a declarative YAML/JSON surface, matching CLAUDE.md's locked v1-authoring-surface
    // split (C++ CRTP and declarative are equivalent surfaces for AGENT authoring, not for this kind
    // of host-wiring decision).
    void set_tool_call_hook(agentengine::ToolCallHook hook) { tool_call_hook_ = std::move(hook); }
    [[nodiscard]] agentengine::ToolCallHook const& tool_call_hook() const noexcept {
        return tool_call_hook_;
    }

    // decisions/ADR-070-host-configurable-responsibility-boundary.md: unset (`nullptr`) by default --
    // every existing session is unaffected until it opts in. Consulted ONLY for
    // `approval_mode::policy_driven` calls, at exactly the two places that already decide anything
    // about that mode -- the main round loop's `invoke_tool()` call and the suspend-for-approval
    // pre-check just above it (both in `run_rounds()` below) -- never at the three "already resolved
    // by a real human" `invoke_tool()` call sites (`resolve_interaction()`'s approved branch,
    // `resolve_codeact_ask()`), which keep using their own `one_shot_approve` and this member's
    // default-`{}` trailing parameter, exactly as before this ADR.
    void set_policy_decider(agentengine::PolicyDecider decide) { policy_decider_ = std::move(decide); }
    [[nodiscard]] agentengine::PolicyDecider const& policy_decider() const noexcept {
        return policy_decider_;
    }

    // decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md, wired in for real: runs once
    // per round, in `run_rounds()`, after this turn's `ContextContribution` is fully assembled
    // (including the dynamically-injected `schedule_wakeup` tool, if any) but BEFORE it is turned
    // into that round's `ChatRequest` -- the real `pre_model`/`turn` seam 017 §4 and 002 §5 both name.
    // Unset (`nullptr`) by default -- every existing `AgentSession<...>` caller is completely
    // unaffected until it opts in.
    void set_turn_middleware_hook(agentengine::TurnMiddlewareHook hook) {
        turn_middleware_hook_ = std::move(hook);
    }
    [[nodiscard]] agentengine::TurnMiddlewareHook const& turn_middleware_hook() const noexcept {
        return turn_middleware_hook_;
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

    void set_suspend_for_approval(bool suspend) noexcept { suspend_for_approval_ = suspend; }
    [[nodiscard]] bool suspend_for_approval() const noexcept { return suspend_for_approval_; }

    void set_stream_model_calls(bool stream) noexcept { stream_model_calls_ = stream; }
    [[nodiscard]] bool stream_model_calls() const noexcept { return stream_model_calls_; }

    void set_scan_response_format_leaks(bool scan) noexcept { scan_response_format_leaks_ = scan; }
    [[nodiscard]] bool scan_response_format_leaks() const noexcept { return scan_response_format_leaks_; }

    // docs/planning/agent-spawn-runtime-design-draft.md §4.2/§9 RC-1 (Critical, Closed): additive,
    // unset (`false`) by default -- every existing session is unaffected until it opts in. A session
    // constructed and driven by `rt::run_child_agent_session()` (rt/agent_spawn_child_run.hpp, OQ-14
    // "agent.spawn"'s nested-run mechanism) sets this unconditionally on every child it builds, so
    // `start_background_task()` (below) fails closed instead of ever reaching
    // `tool_pipeline.hpp::background_task()`'s own detached `std::thread` -- a child driven by a
    // plain "resume until done" loop and then destroyed can never leave a second thread of control
    // touching the now-destroyed session object, closing the use-after-free that mechanism's own
    // "freshly constructed, referenced by nothing else, uncontended session_mutex_" precondition
    // would otherwise miss entirely.
    void set_background_execution_disabled(bool disabled) noexcept {
        background_execution_disabled_ = disabled;
    }
    [[nodiscard]] bool background_execution_disabled() const noexcept {
        return background_execution_disabled_;
    }

    void set_max_turns(std::optional<std::uint64_t> max_turns) noexcept { max_turns_ = max_turns; }
    [[nodiscard]] std::optional<std::uint64_t> max_turns() const noexcept { return max_turns_; }

    // docs/planning/agent-spawn-runtime-design-draft.md §4.6 (item 6, OQ-16, OpenQuestions.md).
    // New, small, opt-in -- every existing session unaffected until it's called, matching every
    // other set_*() on this class. Unlike `contribution->instructions` (materialized from a
    // `ContextProvider`'s `TaintedText`, `run_rounds()` below), this text is never model output and
    // never derived from tainted material -- it is the host/engine's own `trust::push_side_summary()`
    // rendering of a `CapabilitySet` this session was already constructed with (`core/
    // session_builder.hpp`'s `build()`, or `rt/agent_spawn_child_run.hpp`'s child construction path
    // for a spawned child's OWN granted surface) -- so no `TaintedText` declassification step is
    // needed the way a `ContextProvider`'s contribution needs one (I3: only model-derived content
    // requires that decision point; this string never touches model output at any point in its
    // derivation). Empty by default -- `run_rounds()`'s materialization step below is a no-op until
    // this is set.
    void set_static_instructions(std::string text) noexcept { static_instructions_ = std::move(text); }
    [[nodiscard]] std::string const& static_instructions() const noexcept { return static_instructions_; }

    // ADR-058 §8 (Design B) -- additive opt-in, same shape as set_suspend_for_approval/
    // set_stream_model_calls above: unset by default (has_output_schema() false), every existing
    // caller unaffected. `json` is 003 §4's OutputSchema<T> contract, already compiled to JSON
    // Schema text (schema::json_schema_of<T>()) by the caller -- AgentSession stores it erased and
    // never needs to know T (ADR-058 §4 B2). `validate` closes over the caller's real T
    // (schema::from_json<T>/schema::from_json_value<T>), matching this codebase's own "type-driven
    // parse is the validator" idiom (003 §4, corrected 2026-08-14). Returns `result<void>`, not a
    // bare bool (ADR-058 §4 B3), so a real failure carries a real message. `strategy` gates whether
    // the REQUEST carries a native structured-output constraint (run_rounds(), native only) --
    // validation of the RESPONSE, below, applies regardless of strategy.
    void set_output_schema(std::string json, agentengine::output_schema_strategy strategy,
                            std::function<result<void>(std::string_view)> validate) {
        output_schema_json_     = std::move(json);
        output_schema_strategy_ = strategy;
        output_schema_validate_ = std::move(validate);
    }
    [[nodiscard]] bool has_output_schema() const noexcept {
        return static_cast<bool>(output_schema_validate_);
    }

    [[nodiscard]] stream<RunEvent> enable_event_stream(std::pmr::memory_resource* mr,
                                                        stream_config<RunEvent> cfg = {}) {
        auto pair            = make_stream<RunEvent>(mr, cfg);
        run_event_producer_ = std::move(pair.producer);
        return std::move(pair.consumer);
    }

    // ADR-152 (issue #29): a second, INDEPENDENT tap into emit_run_event_for(), parallel to (never
    // replacing) enable_event_stream() above. A red-team pass found a real conflict in the first
    // design that would have had WorkflowSupervisor's own bridge call enable_event_stream() a
    // second time on this session: that unconditionally REPLACES run_event_producer_ (the same
    // "second call replaces the producer" convention WorkflowSupervisor::enable_live_view() also
    // uses), silently evicting any consumer an application had already attached directly to this
    // session -- a real, legitimate usage pattern (an app wanting both a workflow-level dashboard
    // AND a focused per-agent debug stream on the same node) would break with no error, just an
    // orphaned, permanently-empty consumer. A plain callback field sidesteps the single-consumer
    // contract entirely: both this tap and a channel-based enable_event_stream() consumer, if both
    // are wired, independently observe every event, from the same emit_run_event_for() call site.
    // Default no-op; call-scoped, matching report_progress's own bracket discipline (ADR-060) --
    // rt::agent_session_as_executor_body() (rt/agent_workflow_executor.hpp) sets a real closure
    // immediately before start_run() and resets it to the no-op immediately after, so a second,
    // unrelated call into this same session (a cyclic node revisited later, or an app calling
    // start_run() directly outside any workflow) never inherits a stale closure captured by
    // reference into a since-destroyed EffectContext.
    void set_run_event_tap(std::function<void(RunEvent const&)> tap) {
        run_event_tap_attached_ = static_cast<bool>(tap);
        run_event_tap_ = tap ? std::move(tap) : std::function<void(RunEvent const&)>([](RunEvent const&) {});
    }

    [[nodiscard]] std::vector<Message> const& history() const noexcept { return history_; }
    [[nodiscard]] std::string const& session_id() const noexcept { return session_id_; }
    [[nodiscard]] agentengine::Principal const& principal() const noexcept { return principal_; }
    [[nodiscard]] std::uint64_t admission_denied_count() const noexcept { return admission_denied_count_; }
    [[nodiscard]] StateT& state() noexcept { return state_; }
    [[nodiscard]] StateT const& state() const noexcept { return state_; }
    [[nodiscard]] std::unordered_map<std::string, std::string>& metadata() noexcept { return metadata_; }
    [[nodiscard]] std::unordered_map<std::string, std::string> const& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] std::string const& last_run_id() const noexcept { return last_run_id_; }
    [[nodiscard]] std::uint64_t last_turn_index() const noexcept { return effect_context_.turn_index; }
    [[nodiscard]] std::uint64_t run_tokens_consumed() const noexcept { return run_tokens_consumed_; }
    // ADR-163: the full-`Usage` sibling of the accessor above -- see `run_usage_`'s own comment for
    // why this is a separate, parallel field rather than a re-derivation of `run_tokens_consumed_`.
    [[nodiscard]] agentengine::Usage run_usage() const noexcept { return run_usage_; }

    [[nodiscard]] std::vector<Interaction> const& open_interactions() const noexcept {
        return open_interactions_;
    }
    [[nodiscard]] bool has_open_interactions() const noexcept { return !open_interactions_.empty(); }

    // Round 8 red-team: added so clear_in_process_state()'s "no residue left to read back through ANY
    // of this class's own accessors" contract (005 §6) can actually be checked against
    // pending_codeact_asks_ -- previously unobservable (no accessor existed), which is exactly how the
    // finding-17 leak (clear_in_process_state() never clearing this map) went undetected. Mirrors
    // admission_denied_count()'s own shape.
    [[nodiscard]] std::size_t pending_codeact_ask_count() const noexcept {
        return pending_codeact_asks_.size();
    }

    // decisions/ADR-029-suspend-for-human-approval.md §6 / ADR-070: closes the named gap that
    // `Interaction::expires_at_ns` is stored but nothing ever checks it. `interaction.hpp`'s own
    // comment is why this is a QUERY, not a wired timer: "no real wall-clock source wired in
    // anywhere in this project yet (Clock is not a wired capability)" -- an engine-internal poll
    // would have to invent exactly the untracked nondeterminism I5 forbids. The host supplies its
    // own notion of "now" instead (I5: nondeterminism crosses a recorded seam -- the caller's, not
    // an ambient one this function reads for itself); `expires_at_ns == 0` ("no expiry", 001 §2)
    // never matches. Deciding WHAT an expiry means is entirely the host's job -- typically calling
    // the ALREADY-EXISTING `resolve_interaction({id, approved=false})` for each id this returns,
    // which is already race-free against a concurrently-arriving real human answer via
    // `session_mutex_` (I1) -- this function adds no new resolution mechanism, only the query that
    // was missing.
    [[nodiscard]] std::vector<std::string> expired_interaction_ids(std::int64_t now_ns) const {
        std::vector<std::string> ids;
        for (Interaction const& i : open_interactions_) {
            if (i.expires_at_ns != 0 && now_ns >= i.expires_at_ns) ids.push_back(i.interaction_id);
        }
        return ids;
    }

    // decisions/ADR-029-suspend-for-human-approval.md §6 / ADR-070: the OTHER half of the same gap
    // -- nothing today ever POPULATES `expires_at_ns` either (`open_interaction()`'s own body sets
    // only `interaction_id`/`run_id`/`reason`). A host that learns of a new suspension (the
    // `input_required` run event already names the interaction_id) calls this, in its own wall-clock
    // terms, to opt that ONE interaction into the timeout policy `expired_interaction_ids()` above
    // can then observe -- an interaction nobody calls this for keeps `expires_at_ns == 0` ("no
    // expiry") exactly as before this ADR. Plain and unlocked, matching every other `set_*`
    // session-configuration method on this class (`set_approval_decider`, `set_policy_decider`,
    // `set_suspend_for_approval`) -- I1's "one session, one executor" contract, not a per-call lock,
    // is what makes that safe; a host driving this session from more than one concurrent caller is
    // already the require_authority_ Tier-3 path's own documented contract, unaffected by this.
    bool set_interaction_expiry(std::string const& interaction_id, std::int64_t expires_at_ns) noexcept {
        auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                                [&](Interaction const& i) { return i.interaction_id == interaction_id; });
        if (it == open_interactions_.end()) return false;
        it->expires_at_ns = expires_at_ns;
        return true;
    }

    // ---- The two real entry points -----------------------------------------------------------

    // Replaces `handle(quark::Ask<StartRun, AgentResponse> const&)`. Returns the response directly
    // (as `result<AgentResponse>`) instead of calling `m.respond(...)` -- there is no Quark Ask/reply-
    // cell mechanism anymore; the caller `co_await`s this task and gets the answer back the ordinary
    // way. A suspended-for-approval round or an admission denial or ANY fail-closed branch returns an
    // error result rather than a fabricated response -- see each branch's own comment for which error
    // code, matching the original's "never fabricate a response" rule exactly, just expressed as a
    // return value instead of a never-answered Ask.
    // ADR-061 §20.5: `now` is caller-supplied (I5), defaulted to `std::chrono::steady_clock::now()`
    // so the ~155 existing non-Tier-3 call sites (none of which pass `authority` either, and so never
    // reach the branch that reads `now` at all) are unaffected -- the default is evaluated at each
    // call site, not read from inside this function's body, so the "no internal clock read" discipline
    // still holds; it exists purely to bound the size of this mechanical migration.
    task<result<AgentResponse>> start_run(
            StartRun request,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner
        drain_background_completions_locked();  // Slice 3 -- see file banner

        // ADR-061 §20.4: branches on session MODE first, rather than widening a shared condition to
        // `caller.has_value() || authority.has_value()` -- the shape that broke once already (X3): a
        // `require_authority_` session consults ONLY `authority`; a `caller`-only request is rejected
        // outright, never silently admitted through the other branch. No code path reads both.
        if (require_authority_) {
            if (!request.authority.has_value()) {
                ++admission_denied_count_;
                co_return std::unexpected(error{failure_class::policy,
                    "this session requires per-request authority", "run.authority_required"});
            }
            if (!agentengine::principal_admitted_for(request.authority->principal, principal_)) {
                ++admission_denied_count_;
                co_return std::unexpected(error{failure_class::policy,
                    "caller not admitted for this session", "run.admission_denied"});
            }
        } else if (request.caller.has_value() &&
                   !agentengine::principal_admitted_for(
                       agentengine::Principal{request.caller->id, request.caller->tenant_id},
                       principal_)) {
            ++admission_denied_count_;
            co_return std::unexpected(error{
                failure_class::policy, "caller not admitted for this session",
                "run.admission_denied"});
        }

        // ADR-061 §20.3/§22.3: written here, before any other branch below could reach a `held`/
        // `effect_context_.capabilities` consumer.
        result<void> applied = apply_dispatch_authority(request.authority, now);
        if (!applied) co_return std::unexpected(applied.error());

        // ADR-057 §9 / §4 Finding A2: an open `codeact_ask` interaction must reject a fresh
        // `StartRun` too, unlike a plain `input`/`auth` interaction (ADR-029 finding #5's own,
        // deliberately narrower rule -- those "legitimately coexist with an ordinary fresh StartRun,
        // a host's own passivate/reactivate bookkeeping"). A codeact ask's replay state
        // (`pending_codeact_asks_`) is keyed to one specific suspended round's own `history_`/
        // `exec_state_` -- a second concurrent `StartRun` racing that state is exactly the I1
        // violation this check exists to prevent, the same reasoning `approval` already gets.
        // OQ-21: `hook_decision` joins this check for the identical reason `codeact_ask` already
        // does -- `pending_hook_decisions_`'s state is keyed to one specific suspended round, and a
        // concurrent fresh `StartRun` would race it.
        bool const has_open_approval_or_codeact_ask =
            std::any_of(open_interactions_.begin(), open_interactions_.end(), [](Interaction const& i) {
                return i.reason == interaction_reason::approval ||
                       i.reason == interaction_reason::codeact_ask ||
                       i.reason == interaction_reason::hook_decision;
            });
        if (has_open_approval_or_codeact_ask) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "a round is already suspended awaiting approval, an agent.ask() answer, or an "
                "external tool-call hook decision -- resolve it before starting a new run",
                "run.approval_pending"});
        }

        run_counter_ += 1;
        run_tokens_consumed_ = 0;
        run_usage_ = agentengine::Usage{};
        // ADR-061 §20.3: principal/capabilities are already set by apply_dispatch_authority() above
        // -- setting them again here from session-level state would silently overwrite a correctly-
        // resolved per-request authority, reproducing the exact bug this mechanism exists to close.
        effect_context_.run_id       = session_id_ + ":run:" + std::to_string(run_counter_);
        effect_context_.turn_index   = 0;
        last_run_id_ = effect_context_.run_id;

        emit_run_event(run_event_kind::run_started);
        if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
            // unified-streaming-design-draft.md §3 (Piece A), Rev 7 (Finding 5-new, 5th red-team pass):
            // this warning is a SEPARATE emission site from run_model_call()'s own dispatch fix -- fixing
            // only the dispatch would leave this one asserting "no live model_delta events" on every
            // gateway-routed run even after Piece A makes that false for a real ModelCallGateway with
            // stream_model_calls_ set. Gated identically to that dispatch's own condition.
            if constexpr (agentengine::ModelCallGatewayStreamLike<ChatClientT>) {
                if (stream_model_calls_) {
                    emit_run_event(
                        run_event_kind::warning,
                        run_event_payload::Warning{
                            "this run routes model calls through a ModelCallGateway (ADR-036) with "
                            "streaming enabled: live model_delta events fire once an attempt commits "
                            "(unified-streaming-design-draft.md §3), but retries/fallback tiers before "
                            "that first commit stay invisible to the caller, and the per-run token "
                            "budget is only checked once a response resolves"});
                } else {
                    emit_run_event(
                        run_event_kind::warning,
                        run_event_payload::Warning{
                            "this run routes model calls through a ModelCallGateway (ADR-036): no live "
                            "model_delta events fire for a gateway-routed round, and a single round may "
                            "make several real backend calls (retries/fallback tiers) before the per-run "
                            "token budget is ever checked"});
                }
            } else {
                emit_run_event(
                    run_event_kind::warning,
                    run_event_payload::Warning{
                        "this run routes model calls through a ModelCallGateway (ADR-036): no live "
                        "model_delta events fire for a gateway-routed round, and a single round may make "
                        "several real backend calls (retries/fallback tiers) before the per-run token "
                        "budget is ever checked"});
            }
        } else if (stream_model_calls_) {
            emit_run_event(run_event_kind::warning,
                            run_event_payload::Warning{
                                "this run streams each model call (ADR-034): failover/circuit-"
                                "breaker-feedback do not apply on the streaming path, even if the "
                                "bound ChatClientT would otherwise provide them"});
        }
        history_.push_back(request.input);

        co_return co_await run_rounds();
    }

    // Replaces `handle(quark::Ask<ResolveInteraction, AgentResponse> const&)`.
    task<result<AgentResponse>> resolve_interaction(
            ResolveInteraction request,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner
        drain_background_completions_locked();  // Slice 3 -- see file banner

        // ADR-061 §20.4: same mode-branch shape as start_run() -- see that function's own comment.
        if (require_authority_) {
            if (!request.authority.has_value()) {
                ++admission_denied_count_;
                co_return std::unexpected(error{failure_class::policy,
                    "this session requires per-request authority", "run.authority_required"});
            }
            if (!agentengine::principal_admitted_for(request.authority->principal, principal_)) {
                ++admission_denied_count_;
                co_return std::unexpected(error{failure_class::policy,
                    "caller not admitted for this session", "run.admission_denied"});
            }
        } else if (request.caller.has_value() &&
                   !agentengine::principal_admitted_for(
                       agentengine::Principal{request.caller->id, request.caller->tenant_id},
                       principal_)) {
            ++admission_denied_count_;
            co_return std::unexpected(error{
                failure_class::policy, "caller not admitted for this session",
                "run.admission_denied"});
        }

        // ADR-061 §22.3: placed HERE, immediately after admission and before EVERY later branch --
        // not just before the approved/!approved split further down. resolve_interaction() has five
        // real branches after admission (interaction-lookup validity, the codeact_ask early return at
        // the `resolve_codeact_ask()` call below, `resolve_interaction_record()`, then the approved/
        // !approved split) -- §21b/§23a found a placement claim that only named the last of these was
        // not actually safe against the codeact_ask branch. This dominates all of them.
        result<void> applied = apply_dispatch_authority(request.authority, now);
        if (!applied) co_return std::unexpected(applied.error());

        auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                                [&](Interaction const& i) {
                                    return i.interaction_id == request.interaction_id;
                                });
        if (it == open_interactions_.end()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract, "unknown interaction id",
                "session.resolve_interaction.unknown_id"});
        }

        if (history_.empty() || history_.back().role != role::assistant) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "session state has moved on since this interaction was opened",
                "session.resolve_interaction.stale"});
        }
        std::vector<ToolCall> const pending_calls = tool_calls_of(history_.back());
        if (pending_calls.empty()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract, "no pending tool call to resolve",
                "session.resolve_interaction.nothing_pending"});
        }

        // ADR-057 §9: a `codeact_ask` interaction resolves through a genuinely different mechanism
        // (host-driven replay against a STORED script, bypassing the model and `ExecuteCodeArgs`
        // entirely) -- branches out here, AFTER the identical validation the approval path above
        // already ran (open interaction found, `history_`'s tail is still the exact suspended
        // assistant tool-call message -- same order/shape ADR-029's own finding #4 established),
        // BEFORE `resolve_interaction_record()` erases the interaction (the codeact_ask branch may
        // need to keep it open, if the script ask-pends again).
        // Copied, not passed as `it->interaction_id` by reference: both callees below (via
        // `resolve_interaction_record()`) erase the exact `open_interactions_` element `it` points
        // at, then keep using their `interaction_id` parameter afterward (`resolve_hook_decision()`'s
        // own `emit_run_event(..., InteractionRef{interaction_id})` right after its erase call) --
        // a reference bound to `it->interaction_id` would dangle at that point (ASan container-
        // overflow: reproduced live via the H4a hook-decision-resume test path). A copy is immune to
        // the erase regardless of which callee's internal ordering changes later.
        std::string const resolved_interaction_id = it->interaction_id;

        if (it->reason == interaction_reason::codeact_ask) {
            co_return co_await resolve_codeact_ask(request, resolved_interaction_id);
        }

        // OQ-21: same branch-out shape as codeact_ask immediately above, for the identical reason --
        // a `hook_decision` resume folds in the external process's own dispatch answers against
        // STORED, hook-processed per-call state (`pending_hook_decisions_`), never against
        // `pending_calls` rebuilt from `history_` -- see resolve_hook_decision()'s own comment.
        if (it->reason == interaction_reason::hook_decision) {
            co_return co_await resolve_hook_decision(request, resolved_interaction_id);
        }

        result<void> const resolved = resolve_interaction_record(it->interaction_id);
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        emit_run_event(run_event_kind::input_resolved,
                        run_event_payload::InteractionRef{request.interaction_id});

        std::size_t const response_msg_index = history_.size() - 1;

        if (!request.approved) {
            // OQ-21: a hook-touched round being denied must deny through its STORED, hook-processed
            // state -- never rebuild from `pending_calls` (that would silently re-run calls the hook
            // already denied or rewrote, bypassing the hook stage on resume -- the fatal-finding
            // shape this whole mechanism exists to close, see run_rounds()'s hook-stage comment).
            if (auto hit = pending_hook_decisions_.find(request.interaction_id);
                hit != pending_hook_decisions_.end()) {
                PendingHookDecisionRound round = std::move(hit->second);
                pending_hook_decisions_.erase(hit);
                std::vector<ToolResult> results;
                results.reserve(round.calls.size());
                for (HookProcessedCall& hc : round.calls) {
                    emit_run_event(run_event_kind::approval_resolved,
                                    run_event_payload::ApprovalResolved{hc.request.call_id, false,
                                                                          request.interaction_id});
                    if (hc.outcome == hook_call_outcome::denied) {
                        // Already decided by the hook stage (or a prior external-dispatch answer) --
                        // reuse the SAME ToolResult verbatim rather than deriving a second one.
                        results.push_back(std::move(*hc.denial_result));
                    } else {
                        results.push_back(make_denial_result(hc.request.call_id, "denied by operator",
                                                                "tool.approval_denied"));
                    }
                }
                history_.push_back(tool_results_message(std::move(results)));
                (void)co_await history_provider_.on_turn_end(
                    TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                        history_.size() - response_msg_index}},
                    effect_context_);
                emit_run_event(run_event_kind::turn_finished,
                                run_event_payload::Turn{effect_context_.turn_index});
                ++effect_context_.turn_index;
                co_return co_await run_rounds();
            }

            std::vector<ToolResult> results;
            results.reserve(pending_calls.size());
            for (ToolCall const& call : pending_calls) {
                emit_run_event(run_event_kind::approval_resolved,
                                run_event_payload::ApprovalResolved{call.call_id, false,
                                                                      request.interaction_id});
                results.push_back(
                    make_denial_result(call.call_id, "denied by operator", "tool.approval_denied"));
            }
            history_.push_back(tool_results_message(std::move(results)));
            (void)co_await history_provider_.on_turn_end(
                TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                    history_.size() - response_msg_index}},
                effect_context_);
            emit_run_event(run_event_kind::turn_finished,
                            run_event_payload::Turn{effect_context_.turn_index});
            ++effect_context_.turn_index;
            co_return co_await run_rounds();
        }

        // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
        SessionContext session_ctx{session_id_, effect_context_.principal, history_};
        result<ContextContribution> contribution =
            co_await history_provider_.on_context(session_ctx, effect_context_);
        if (!contribution) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{"run.context_unavailable",
                                                          contribution.error().message});
            co_return std::unexpected(contribution.error());
        }
        ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);
        ApprovalDecider const one_shot_approve = [](Principal const&, std::string_view, std::string const&) {
            return true;
        };

        // OQ-21: this approval-suspend round was hook-touched -- use the STORED, hook-processed
        // requests, never rebuild from `pending_calls` (that would silently re-run calls the hook
        // already denied or rewrote, bypassing the hook stage on resume -- the fatal-finding shape
        // this whole mechanism exists to close, see run_rounds()'s hook-stage comment).
        //
        // Reusing `one_shot_approve` for the WHOLE round IS safe here, unlike a `hook_decision`
        // resume (resolve_hook_decision() never does this): reaching THIS branch means the round
        // suspended under `interaction_reason::approval` -- directly, or via
        // resolve_hook_decision()'s own cascade -- specifically because `any_needs_approval` was
        // computed in run_rounds() from the POST-HOOK, already provenance-downgraded requests. Every
        // `needs_decider` call remaining in this round is exactly what this human is being asked to
        // approve; `invoke_tool()`'s own step 5 only ever consults the decider for a call that
        // actually needs it, so a call that needs no approval at all (vendor_structured,
        // never_require) is unaffected by which decider is passed.
        if (auto hit = pending_hook_decisions_.find(request.interaction_id);
            hit != pending_hook_decisions_.end()) {
            PendingHookDecisionRound round = std::move(hit->second);
            pending_hook_decisions_.erase(hit);
            co_return co_await finish_hook_processed_round(std::move(round), tool_table, one_shot_approve);
        }

        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        // ADR-061 §20.6: per-request, not session-level -- effect_context_.capabilities is freshly
        // written by apply_dispatch_authority() at the top of every real entry point (start_run(),
        // resolve_interaction()) before this is ever reached.
        CapabilitySet const& held      = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;

        std::vector<ToolCallRequest> reqs;
        reqs.reserve(pending_calls.size());
        for (std::size_t i = 0; i < pending_calls.size(); ++i) {
            reqs.push_back(tool_call_request_of(pending_calls[i], i));
        }
        // ADR-160 §5: tool_call_started/delta/finished all fire from inside dispatch_tool_calls();
        // approval_resolved is THIS call site's own extra, per-call event, emitted afterward in
        // `reqs`' own order -- dispatch_tool_calls() always returns in that order regardless of
        // which concurrency class (or completion order) actually produced each result.
        std::vector<DispatchedCall> dispatched =
            dispatch_tool_calls(reqs, tool_table, held, one_shot_approve, PolicyDecider{});
        std::vector<ToolResult> results;
        results.reserve(dispatched.size());
        for (std::size_t i = 0; i < dispatched.size(); ++i) {
            emit_run_event(run_event_kind::approval_resolved,
                            run_event_payload::ApprovalResolved{reqs[i].call_id, true,
                                                                  request.interaction_id});
            results.push_back(std::move(dispatched[i].result));
        }
        history_.push_back(tool_results_message(std::move(results)));
        (void)co_await history_provider_.on_turn_end(
            TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                history_.size() - response_msg_index}},
            effect_context_);
        emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
        ++effect_context_.turn_index;
        co_return co_await run_rounds();
    }

    // ---- Pure bookkeeping, unchanged in behavior from core/agent_session.hpp -----------------
    // (no Quark dependency in the original either -- ported verbatim, not redesigned)

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
    void fork_from(AgentSession const& source, std::string new_session_id,
                    std::optional<std::size_t> history_prefix_len = std::nullopt) {
        AsyncMutex::Guard source_guard;
        if (!source.session_mutex_.is_held_by_current_thread()) {
            source_guard = agentengine::rt::block_on(
                agent_session_detail::acquire_session_mutex(source.session_mutex_));
        }
        session_id_ = std::move(new_session_id);
        principal_  = source.principal_;
        // ADR-061 §22.1/§21a Finding 1: fail-closed carry-forward -- a fork of a Tier-3 session must
        // stay Tier-3 by default. Not copying this alongside principal_ (the identity) was a real,
        // test-proven gap (a forked session was immediately start_run()-able with require_authority_
        // silently back at its unsafe default). capabilities_ is deliberately still NOT copied here --
        // a pre-existing gap unrelated to Tier 3, a non-issue for a require_authority_==true fork
        // (that mode never reads capabilities_) and unchanged behavior for a ==false fork (which
        // needs set_capabilities() re-called after fork_from(), exactly as before this change).
        require_authority_ = source.require_authority_;
        std::size_t const n = std::min(history_prefix_len.value_or(source.history_.size()),
                                        source.history_.size());
        history_.assign(source.history_.begin(), source.history_.begin() + static_cast<std::ptrdiff_t>(n));
        state_    = source.state_;
        metadata_ = source.metadata_;
        history_provider_ = source.history_provider_;
        run_counter_ = 0;
        last_run_id_.clear();
        effect_context_ = EffectContext{};
        open_interactions_.clear();
        interaction_counter_ = 0;
        run_tokens_consumed_ = 0;
        run_usage_ = agentengine::Usage{};
        admission_denied_count_ = 0;
        // Same "no run identity of its own" rationale open_interactions_ above already documents --
        // a fresh fork inherits none of the source's (or *this*'s own prior) outstanding background
        // work. Same fix category as clear_in_process_state()'s own comment below.
        standing_effects_registry_.reset();
    }

    [[nodiscard]] result<void> redact(std::string const& message_id, std::string reason, std::string actor) {
        for (Message& msg : history_) {
            if (msg.message_id != message_id) continue;
            json::Value tombstone = json::Value::make_object({
                {"reason", json::Value::make_string(std::move(reason))},
                {"actor", json::Value::make_string(std::move(actor))},
            });
            ContentItem item{};
            item.value   = Custom{"ae:redacted", json::dump(tombstone)};
            item.origin  = content_origin::system;
            item.tainted = false;
            msg.content.assign(1, item);
            return {};
        }
        return std::unexpected(error{failure_class::contract, "no message with that id in history",
                                      "session.redact.unknown_message_id"});
    }

    void clear_in_process_state() {
        session_id_.clear();
        principal_ = agentengine::Principal{};
        history_.clear();
        state_ = StateT{};
        metadata_.clear();
        run_counter_ = 0;
        last_run_id_.clear();
        effect_context_ = EffectContext{};
        open_interactions_.clear();
        interaction_counter_ = 0;
        // Round 8 red-team, finding 17 (LOW): this function's own contract is "no residue left to read
        // back through ANY of this class's own accessors" (005 §6, cited below for standing_effects_)
        // -- pending_codeact_asks_ was missing from this list entirely, unlike every other piece of
        // interaction state above. No public accessor exposes it, so this did not literally violate
        // the accessor wording, but it is a real leak: a pooled/reused AgentSession (the Stateless<N>
        // pooling pattern) that clears+reinitializes with a DIFFERENT session_id_ after an in-flight
        // codeact-ask permanently retains that PendingCodeActAsk record (full script source + every
        // answer given so far) for the remaining lifetime of the C++ object -- unreachable for erasure
        // since future interaction ids embed the new session_id and can never match the orphaned key.
        pending_codeact_asks_.clear();
        // OQ-21: same leak class `pending_codeact_asks_` immediately above was once found missing
        // from this list entirely (this function's own preceding comment) -- not reintroduced here.
        pending_hook_decisions_.clear();
        token_budget_ = std::nullopt;
        run_tokens_consumed_ = 0;
        run_usage_ = agentengine::Usage{};
        admission_denied_count_ = 0;
        max_turns_ = std::nullopt;
        history_provider_ = HistoryProviderT{};
        // A real gap found in the Quark original (core/agent_session.hpp's own clear_in_process_
        // state() never resets standing_effects_/standing_effect_counter_): this function's own
        // contract is "no residue left to read back through ANY of this class's own accessors" (005
        // §6), which list_standing_effects() would otherwise silently violate after a delete. Fixed
        // here rather than ported forward unchanged -- StandingEffectRegistry::reset() deliberately
        // does NOT reset its completion-queue shared_ptr's identity (a worker thread may already hold
        // a weak_ptr to it; dropping and reallocating it would not by itself invalidate anything, but
        // a queue full of stale entries for effects that no longer exist is intentionally harmless --
        // the drain loop's own find_if() already no-ops on an unknown handle_id, same as a canceled
        // one).
        standing_effects_registry_.reset();
    }

    [[nodiscard]] Interaction const& open_interaction(std::string run_id, interaction_reason reason) {
        interaction_counter_ += 1;
        Interaction interaction{};
        interaction.interaction_id = session_id_ + ":interaction:" + std::to_string(interaction_counter_);
        interaction.run_id         = std::move(run_id);
        interaction.reason         = reason;
        open_interactions_.push_back(std::move(interaction));
        return open_interactions_.back();
    }

    [[nodiscard]] result<void> resolve_interaction_record(std::string const& interaction_id) {
        auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                                [&](Interaction const& i) { return i.interaction_id == interaction_id; });
        if (it == open_interactions_.end()) {
            return std::unexpected(error{failure_class::contract, "no open interaction with that id",
                                          "session.resolve_interaction.unknown_id"});
        }
        open_interactions_.erase(it);
        return {};
    }

    // ---- Slice 2: snapshot/checkpoint (file banner has the design writeup) -------------------

    // Unlocked, synchronous -- matches the original's own to_record()/restore_from_record() shape
    // exactly (see file banner: field list is deliberately narrower). Not the caller's normal way
    // to take a snapshot; see snapshot_record() below for the locked path every real caller should
    // use instead. Kept public and separately callable anyway, matching the original, since a test
    // may reasonably want to assert the record shape without going through the lock.
    [[nodiscard]] AgentSessionRecord to_record() const {
        AgentSessionRecord rec;
        rec.session_id          = session_id_;
        rec.principal_id        = principal_.id;
        rec.principal_tenant_id = principal_.tenant_id;
        rec.run_counter         = run_counter_;
        rec.turn_index          = effect_context_.turn_index;
        rec.open_interactions   = open_interactions_;
        rec.require_authority   = require_authority_;  // ADR-061 §22.1
        return rec;
    }

    void restore_from_record(AgentSessionRecord const& rec) {
        session_id_ = rec.session_id;
        principal_  = agentengine::Principal{rec.principal_id, rec.principal_tenant_id};
        run_counter_ = rec.run_counter;
        last_run_id_ = run_counter_ > 0 ? session_id_ + ":run:" + std::to_string(run_counter_)
                                          : std::string{};
        effect_context_.turn_index = rec.turn_index;
        open_interactions_ = rec.open_interactions;
        require_authority_ = rec.require_authority;  // ADR-061 §22.1 -- fail-closed carry-forward
    }

    // The real, in-flight-safe way to read this session's durable state out: acquires
    // session_mutex_ for the whole read, the same I1 guard every other public entry point uses --
    // see file banner for why this replaces the Quark original's "called at the point quiesce(Drain)
    // reaches" assumption. save_agent_session_snapshot() (free function, below) is built on this.
    [[nodiscard]] task<AgentSessionRecord> snapshot_record() {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();
        co_return to_record();
    }

    // Locked wrapper around clear_in_process_state() -- delete_session() (free function, below)
    // needs this so a concurrently in-flight start_run()/resolve_interaction() can never race a
    // deletion, the write-side counterpart to snapshot_record()'s read-side guard.
    task<void> clear_in_process_state_locked() {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();
        clear_in_process_state();
        co_return;
    }

    // ---- Slice 3: standing effects / background tasks (file banner has the design writeup) --

    // ADR-061 §20.5/§24.2: NOW LOCKED -- this deliberately breaks the file banner's originally-
    // documented "PLAIN, UNLOCKED... matches the original's own asymmetry exactly" parity. Confirmed
    // (grep, repo-wide, re-verified across three separate red-team rounds) to have zero real callers
    // in product code -- only tests call it directly -- so nothing in shipped code depended on it
    // staying unlocked, and I1 makes an unlocked mutator of session-scoped state (standing_effects_,
    // standing_effect_counter_) a real hazard once Tier 3 makes a session reachable from more than one
    // concurrently-arriving caller. `authority`/`now` default so every existing non-Tier-3 test caller
    // needs only to add `co_await`.
    [[nodiscard]] task<result<agentengine::StandingEffect>> start_background_task(
        ToolTable const& table, ToolCallRequest const& request,
        std::optional<RequestAuthority> const& authority = std::nullopt,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
        ApprovalDecider const& approve = ApprovalDecider{}) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner

        // §9 RC-1 (design doc above) -- checked first: pure, local, no shared state touched, cheaper
        // than resolving dispatch authority for a request that is going to be refused either way.
        // set_background_execution_disabled()'s own comment has the full rationale.
        if (background_execution_disabled_) {
            co_return std::unexpected(error{failure_class::policy,
                                             "background task execution is disabled for this session",
                                             "standing_effect.background_execution_disabled"});
        }

        result<void> applied = apply_dispatch_authority(authority, now);  // <-- resolved FIRST
        if (!applied) co_return std::unexpected(applied.error());

        if (!effect_context_.capabilities) {  // reads the field THIS call just populated, never stale
            co_return std::unexpected(error{failure_class::policy,
                                             "session has no granted capabilities",
                                             "standing_effect.no_capabilities"});
        }
        std::size_t const current_count =
            standing_effects_registry_.count_of(agentengine::standing_effect_kind::background_task);

        std::string const handle_id = standing_effects_registry_.mint_handle_id(session_id_);
        std::string const owner_run_id       = effect_context_.run_id;
        std::string const owner_principal_id = effect_context_.principal.id;

        std::weak_ptr<BackgroundCompletionQueue> weak_queue = standing_effects_registry_.completion_queue();
        result<void> submitted = background_task(
            table, *effect_context_.capabilities, request, effect_context_, approve, current_count,
            [weak_queue, handle_id, call_id = request.call_id](ToolResult result_out,
                                                                 ToolInvocationAudit audit) mutable {
                (void)audit;
                if (std::shared_ptr<BackgroundCompletionQueue> q = weak_queue.lock()) {
                    std::lock_guard<std::mutex> lock(q->m);
                    q->pending.push_back(BackgroundTaskDone{
                        std::move(handle_id), std::move(call_id), std::move(result_out)});
                }  // else: session (and its queue) already gone -- drop, no UAF, no residue to clean up
            });
        if (!submitted) co_return std::unexpected(submitted.error());

        agentengine::StandingEffect effect;
        effect.handle_id    = handle_id;
        effect.session_id   = session_id_;
        effect.principal_id = owner_principal_id;
        effect.run_id       = owner_run_id;
        effect.kind         = agentengine::standing_effect_kind::background_task;
        effect.label        = request.tool_name;
        standing_effects_registry_.add(effect);

        emit_run_event_for(owner_run_id, run_event_kind::tool_call_started,
                            run_event_payload::ToolCallStarted{request.call_id, request.tool_name});
        co_return effect;
    }

    [[nodiscard]] std::vector<agentengine::StandingEffect> const& list_standing_effects() const noexcept {
        return standing_effects_registry_.list();
    }

    // Cancels the BOOKKEEPING only -- background_task() names no mechanism to interrupt an
    // already-running native invoke(); a late completion for a canceled handle simply finds nothing
    // to resolve in drain_background_completions_locked() (its own idempotent no-op), same as the
    // original.
    [[nodiscard]] result<void> cancel_standing_effect(std::string const& handle_id,
                                                       agentengine::Principal const& caller_principal) {
        return standing_effects_registry_.cancel(handle_id, caller_principal);
    }

    // ---- Slice 4: schedule_wakeup (file banner has the design writeup) -----------------------

    // ADR-061 §20.5: a FOURTH real entry point, found while designing Tier 3 -- this function was
    // exactly as unlocked/directly-callable as start_background_task() (no session_mutex_ guard, real
    // test callers exist), but it is ALSO called internally by the offer-gate closure in run_rounds()
    // (below), which runs ALREADY LOCKED. Locking this wrapper directly would self-deadlock against
    // that internal call on a non-reentrant AsyncMutex -- so, split: schedule_wakeup_impl() (private,
    // below) is the real unlocked logic taking exactly what it needs as parameters; this public
    // wrapper resolves authority and locks for a direct/host caller; the internal closure calls
    // schedule_wakeup_impl() directly, reusing what run_rounds() already resolved, no re-lock.
    [[nodiscard]] task<result<agentengine::StandingEffect>> schedule_wakeup(
        std::chrono::milliseconds delay, std::string label, std::chrono::steady_clock::time_point now,
        std::optional<RequestAuthority> const& authority = std::nullopt) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner
        result<void> applied = apply_dispatch_authority(authority, now);
        if (!applied) co_return std::unexpected(applied.error());
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        CapabilitySet const& held =
            effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;
        // `label` is passed by value (not moved) -- the registry's own copy is independent of this
        // one, which the state_changed emission below still needs after the call returns.
        result<agentengine::StandingEffect> effect = standing_effects_registry_.schedule_wakeup_impl(
            delay, label, now, held, effect_context_.principal, effect_context_.run_id, session_id_);
        if (effect) {
            emit_run_event(run_event_kind::state_changed,
                            run_event_payload::StateChanged{"schedule_wakeup armed: " + label});
        }
        co_return effect;
    }

    [[nodiscard]] std::vector<agentengine::StandingEffect> due_standing_effects(
        std::chrono::steady_clock::time_point now) const {
        return standing_effects_registry_.due(now);
    }

    // Explicit host-callable drain, for a host that wants to flush completions between runs rather
    // than waiting for the next start_run()/resolve_interaction() (which already drains
    // automatically as their first step -- see file banner).
    task<void> drain_background_completions() {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();
        drain_background_completions_locked();
        co_return;
    }

private:
    // Caller must already hold session_mutex_ (start_run()/resolve_interaction() call this as their
    // first step; drain_background_completions() -- public, above -- is the explicit-call path). See
    // file banner's "SLICE 3 ADDITION" paragraph for the full design.
    // Caller (this function's own two callers, both already inside session_mutex_'s held scope --
    // see this method's own top comment above) must keep that guard alive across BOTH the
    // drain_ready() call below AND this emit loop, in this same function scope -- see
    // rt/standing_effect_registry.hpp's DrainedCompletion comment and the design draft's §3 item 4
    // for why factoring this loop out from under the guard would reopen a real I1 window. This
    // function IS that single scope; nothing here may be pulled into a separately-called helper.
    void drain_background_completions_locked() {
        for (DrainedCompletion& d : standing_effects_registry_.drain_ready()) {
            emit_run_event_for(d.owner_run_id, run_event_kind::tool_call_finished,
                                run_event_payload::ToolCallFinished{d.call_id, std::move(d.result)});
        }
    }

    // ADR-061 §20.3/§26.1: the ONE place `effect_context_.principal`/`.capabilities` get (re)written
    // for a dispatch -- called immediately after admission passes, before any branch that could reach
    // a `held`/`effect_context_.capabilities` consumer (§22.3: this placement rule is what makes it
    // safe for every read downstream, in the SAME call, to never observe a stale value left over from
    // a previous call). `now` is caller-supplied, never read from an ambient clock (I5).
    [[nodiscard]] result<void> apply_dispatch_authority(
            std::optional<RequestAuthority> const& authority,
            std::chrono::steady_clock::time_point now) {
        if (require_authority_) {
            if (!authority.has_value()) {
                return std::unexpected(error{failure_class::policy,
                    "this session requires per-request authority; dispatcher supplied none",
                    "run.authority_required"});
            }
            if (!authority->live(now)) {
                return std::unexpected(error{failure_class::policy,
                    "per-request authority has expired", "run.authority_expired"});
            }
            effect_context_.principal    = authority->principal;
            effect_context_.capabilities = authority->capabilities;
            return {};
        }
        // Tier-3 does not front this session -- session-level state is the only authority that has
        // ever existed for it, unchanged from before this mechanism existed.
        effect_context_.principal    = principal_;
        effect_context_.capabilities = capabilities_;
        return {};
    }

    // ADR-061 §20.5/§20.6/§19.6: schedule_wakeup's real logic is now `StandingEffectRegistry::
    // schedule_wakeup_impl()` (rt/standing_effect_registry.hpp) -- UNLOCKED, taking exactly what it
    // needs as parameters, called both by the public schedule_wakeup() wrapper above (already
    // resolved authority, already locked) and by the offer-gate closure in run_rounds() (below),
    // which runs already locked via its own caller and must not re-lock. That type's own comment has
    // the rest of this rationale (deliberately outside tool_pipeline.hpp's held.bind() mechanism,
    // mirroring Background<max_concurrent>'s own in-body check for the identical reason). This
    // AgentSession no longer needs its own copy of that logic; see this method's own top comment for
    // the state-changed event AND state.

    void emit_run_event(run_event_kind kind, RunEventPayload payload = run_event_payload::Empty{}) {
        emit_run_event_for(effect_context_.run_id, kind, std::move(payload));
    }
    void emit_run_event_for(std::string const& run_id, run_event_kind kind,
                             RunEventPayload payload = run_event_payload::Empty{}) {
        // ADR-152: skip constructing/sequencing an event when NEITHER sink is attached -- the
        // pre-existing "zero cost when unattached" guarantee this method already provided for
        // run_event_producer_ alone, extended to cover the new tap without changing it for a
        // caller that only ever used the producer (run_event_tap_ defaults to a real, always-
        // callable no-op, so `static_cast<bool>` on it is not a meaningful "is a tap attached"
        // check -- the boolean guard belongs here, at the call site, not on the field itself).
        if (!run_event_producer_.valid() && !run_event_tap_attached_) return;
        // ADR-160 §5 MUST-FIX 2: the whole body below -- seq increment, tap call, producer push --
        // is now one critical section (see run_event_mutex_'s own comment). This restores
        // "effectively single producer" for run_event_producer_ regardless of which thread a
        // concurrently-dispatched parallel-batch call's own report_progress/agent_turn_sink fires
        // from, and eliminates the run_event_seq_by_run_ map race in the same stroke.
        std::lock_guard<std::mutex> lock(run_event_mutex_);
        RunEvent ev;
        ev.run_id  = run_id;
        ev.seq     = ++run_event_seq_by_run_[run_id];
        ev.kind    = kind;
        ev.payload = std::move(payload);
        run_event_tap_(ev);
        if (run_event_producer_.valid()) (void)run_event_producer_.push(std::move(ev));
    }

    // decisions/ADR-160-parallel-tool-batch-scheduler.md §5. Dispatches one already-filtered batch
    // of requests (the caller has already skipped/resolved anything a hook denied) through the
    // corrected design: admission (steps 1,4/7,5 -- admit_call(), tool_pipeline.hpp) sequentially,
    // in emitted order, exactly as invoke_tool() does inline -- then fans any batch
    // partition_batch() finds uniformly Parallelizable/ExclusivityGroup<Name>-eligible out to real,
    // bounded worker threads (rt::run_jobs_bounded, bounded_call_fanout.hpp) for step 8 (invoke)
    // alone, joining before returning. When the batch is NOT eligible -- every real batch today, no
    // shipped tool declares either tag yet -- this degrades to EXACTLY today's sequential loop, call
    // for call, byte for byte: each call still goes straight through invoke_tool() against the
    // session's own shared effect_context_, on the calling thread, in order.
    //
    // tool_call_started fires for every call up front, in emitted order, before any admission
    // begins. tool_call_delta/tool_call_finished fire per call as its own result becomes available
    // -- from whichever thread produced it, safe under run_event_mutex_ (§5 MUST-FIX 2) -- so for a
    // fanned-out class, its finished-event ORDER relative to a sibling class is a genuine, unordered
    // race (§5 SHOULD-FIX 9: monotonic per 013 §1, but not required to be deterministic; only the
    // RETURNED vector's order -- always `reqs`' own emitted order, regardless of completion order --
    // is I5-load-bearing). Returns one ToolResult and one ToolInvocationAudit per request, in
    // `reqs`' own order.
    //
    // NAMED RESIDUAL: `run_rounds()`'s own codeact_ask early-stop rule (ADR-057 §9: a multi-call
    // round where one call ask-pends fails closed WITHOUT running any call after it) is enforced by
    // that call site AFTER this function returns, over the now-complete result vector. For a batch
    // this function ran through the ELIGIBLE (fan-out) path, a call positioned after an ask-pending
    // one may already have executed its own side effects before the sentinel is detected -- unlike
    // the non-eligible path, where the original per-iteration loop still stops immediately. This can
    // only arise if a codeact-ask-capable tool were ALSO declared Parallelizable/
    // ExclusivityGroup<Name>: today's `execute_code` is `captures_session_state` (MUST-FIX 1 forces
    // it into its own sequential singleton class regardless of what it also declares), so this
    // residual is not reachable by any shipped tool -- named here rather than silently assumed
    // impossible.
    struct DispatchedCall {
        ToolResult result;
        ToolInvocationAudit audit;
    };

    // ADR-160 §6: the concurrency bound's ultimate SOURCE (a capability, a run-level config knob, or
    // a fixed constant) is still an open question -- this is a placeholder default, not a
    // host-configurable knob yet.
    static constexpr std::size_t kParallelBatchWorkerCap = 4;

    std::vector<DispatchedCall> dispatch_tool_calls(std::vector<ToolCallRequest> const& reqs,
                                                      ToolTable const& tool_table, CapabilitySet const& held,
                                                      ApprovalDecider const& approve,
                                                      PolicyDecider const& policy) {
        std::vector<DispatchedCall> out(reqs.size());
        if (reqs.empty()) return out;

        for (auto const& req : reqs) {
            emit_run_event(run_event_kind::tool_call_started,
                            run_event_payload::ToolCallStarted{req.call_id, req.tool_name});
        }

        std::vector<ConcurrencyClass> classes = partition_batch(reqs, tool_table);

        // A per-call EffectContext: a full struct copy of the run's LIVE effect_context_ at the
        // moment this call is admitted, with ONLY report_progress rebound to this call's own
        // call_id -- agent_turn_sink/moderator_delta_sink/sandbox_fs/blob_sink/capabilities/run_id/
        // ... are all carried through UNCHANGED by the copy itself (ADR-160 §5 MUST-FIX 3: this
        // deliberately does NOT reset the other two reverse-channel fields the way Backgroundable's
        // own copy does, since a parallel-batch call's deltas through them are still meant to reach
        // a live stream, unlike Backgroundable's deliberate suppression).
        auto make_call_ctx = [this](std::string const& call_id) {
            EffectContext ctx = effect_context_;
            ctx.report_progress = [this, run_id = ctx.run_id, call_id](ContentItem item) {
                detail::force_tainted(item);
                emit_run_event_for(run_id, run_event_kind::tool_call_delta,
                                     run_event_payload::ToolCallDelta{call_id, std::move(item)});
            };
            return ctx;
        };

        // One job per parallel/exclusivity_group CLASS (never per call, MUST-FIX 5) -- owns its
        // members' own bound-capability vectors and EffectContext copies (MUST-FIX 6), runs them as
        // an ordinary sequential loop internally, and writes each member's own DispatchedCall
        // directly into `out[call_indices[k]]`. `out` is sized once, above, and never reallocated,
        // so this indexed write is safe from any thread.
        struct ParallelJob {
            std::vector<std::size_t> call_indices;
            std::vector<ToolDescriptor const*> tools;
            std::vector<ToolCallRequest const*> requests;  // pointers into `reqs` -- outlives every
                                                              // job: this function does not return
                                                              // until the fan-out below has joined.
            std::vector<EffectContext> ctxs;
            std::vector<std::vector<BoundCapability>> bounds;
            std::vector<DispatchedCall*> slots;             // pointers into `out` -- see above.
        };
        std::vector<ParallelJob> jobs;

        for (auto const& cls : classes) {
            if (cls.kind == concurrency_class_kind::sequential) {
                // Exactly today's existing inline loop body (resolve_interaction()'s approved
                // branch / finish_hook_processed_round() / run_rounds()' own loop, before this ADR)
                // -- the live, shared effect_context_, mutated in place, one call at a time.
                for (std::size_t i : cls.call_indices) {
                    ToolCallRequest const& req = reqs[i];
                    effect_context_.report_progress = [this, call_id = req.call_id](ContentItem item) {
                        detail::force_tainted(item);
                        emit_run_event(run_event_kind::tool_call_delta,
                                        run_event_payload::ToolCallDelta{call_id, std::move(item)});
                    };
                    ToolInvocationAudit audit;
                    ToolResult result =
                        invoke_tool(tool_table, held, req, effect_context_, approve, &audit, policy);
                    effect_context_.report_progress = [](ContentItem) {};
                    emit_run_event(run_event_kind::tool_call_finished,
                                    run_event_payload::ToolCallFinished{audit.call_id, result});
                    out[i] = DispatchedCall{std::move(result), std::move(audit)};
                }
                continue;
            }

            // parallel or exclusivity_group: admit every member sequentially, still on this thread,
            // in emitted order (ADR-070's per-call-only contract; I1's single-executor sequencing
            // for every decision that can suspend the run or consult host policy) -- BEFORE any
            // fan-out begins.
            ParallelJob job;
            for (std::size_t i : cls.call_indices) {
                ToolCallRequest const& req = reqs[i];
                EffectContext ctx = make_call_ctx(req.call_id);
                auto admission = admit_call(tool_table, held, req, ctx.principal, approve, policy);
                if (!admission) {
                    error const& e = admission.error();
                    ToolResult result = tool_pipeline_detail::make_error_result(req.call_id, e);
                    ToolInvocationAudit audit;
                    audit.call_id = req.call_id;
                    audit.tool_name = req.tool_name;
                    audit.ok = false;
                    audit.error_code = e.code;
                    audit.idempotency_key = derive_idempotency_key(ctx, req.call_index, req.arguments);
                    audit.principal_id = ctx.principal.id;
                    audit.principal_tenant_id = ctx.principal.tenant_id;
                    audit.principal_on_behalf_of = ctx.principal.on_behalf_of;
                    emit_run_event_for(ctx.run_id, run_event_kind::tool_call_finished,
                                         run_event_payload::ToolCallFinished{audit.call_id, result});
                    out[i] = DispatchedCall{std::move(result), std::move(audit)};
                    continue;  // never enters the job -- nothing to fan out for an already-denied call
                }
                job.call_indices.push_back(i);
                job.tools.push_back(admission->tool);
                job.requests.push_back(&req);
                job.ctxs.push_back(std::move(ctx));
                job.bounds.push_back(std::move(admission->bound));
                job.slots.push_back(&out[i]);
            }
            if (!job.call_indices.empty()) jobs.push_back(std::move(job));
        }

        // -- Fan-out: every admission above already happened sequentially, in emitted order, on
        // this thread. Only step 8 (invoke) + 9 (normalize) + 10 (account) run concurrently below,
        // one real worker thread per concurrency class -- an exclusivity_group class's own members
        // still run as an ordinary sequential loop WITHIN that one job/thread (MUST-FIX 5); no group
        // member ever separately occupies a second worker slot.
        std::vector<std::function<void()>> runnables;
        runnables.reserve(jobs.size());
        for (auto& job : jobs) {
            runnables.push_back([this, job = std::move(job)]() mutable {
                for (std::size_t k = 0; k < job.call_indices.size(); ++k) {
                    auto const started = std::chrono::steady_clock::now();
                    ToolCallRequest const& req = *job.requests[k];
                    AdmittedCallOutcome outcome = run_admitted_call(*job.tools[k], req, job.ctxs[k], job.bounds[k]);
                    ToolInvocationAudit audit = make_call_audit(req, job.ctxs[k], started, outcome);
                    emit_run_event_for(job.ctxs[k].run_id, run_event_kind::tool_call_finished,
                                         run_event_payload::ToolCallFinished{audit.call_id, outcome.result});
                    *job.slots[k] = DispatchedCall{std::move(outcome.result), std::move(audit)};
                }
            });
        }
        if (!runnables.empty()) {
            agentengine::rt::run_jobs_bounded(runnables, kParallelBatchWorkerCap);
        }

        return out;
    }

    // `force_tainted()`, `filter_cross_provider_reasoning()`, and `drain_streaming_response()` now
    // live as free functions in `rt::detail` (rt/agent_session_trust.hpp, included above) --
    // docs/planning/agent-session-decomposition-design-draft.md §2b. Each takes an `EmitFn`
    // callback in place of directly calling `emit_run_event()`; this class's own `emit_run_event()`/
    // `emit_run_event_for()` are unchanged and unmoved (§2c of that draft) -- every call site below
    // constructs a thin `[this](k, p){ emit_run_event(k, std::move(p)); }` closure at the point of
    // use.

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
    task<result<ChatResponse>> run_model_call(ChatRequest const& request, EffectContext& ctx) {
        // Gap-audit finding 19, Phase 1: fail closed BEFORE any backend ever sees this request, when
        // it carries Media content the bound backend hasn't declared multimodal support for -- every
        // real backend's own outbound translation silently drops what it can't encode (chat_client.
        // hpp's own comment on `validate_outbound_media_capabilities`), so checking here is what
        // turns a silent, unattributable content loss into a real, attributable run failure instead.
        if (auto gate = validate_outbound_media_capabilities(request, chat_client_->capabilities());
            !gate) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{gate.error().code, gate.error().message});
            co_return std::unexpected(gate.error());
        }

        result<ChatResponse> response = std::unexpected(
            error{failure_class::contract, "unreachable: neither call path executed", "run.internal"});
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
                                                                  stream_model_calls_, emit);
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
                            emit_run_event(run_event_kind::run_failed,
                                            run_event_payload::RunFailed{leak.error().code, leak.error().message});
                            co_return std::unexpected(leak.error());
                        }
                    }
                    co_return response;
                }
            }
            response = detail::drain_streaming_response(chat_client_->chat_stream(request, ctx),
                                                          stream_model_calls_, emit);
        }

        if (response.has_value() && scan_response_format_leaks_) {
            response->message = apply_response_format_scan(std::move(response->message), request.tools);
        } else if (response.has_value() && chat_client_->capabilities().tool_calling) {
            // OQ-23 -- same check, same reasoning, as the non-streaming early-return branch above.
            if (auto leak = detect_undeclared_tool_call_leak(response->message, request.tools); !leak) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{leak.error().code, leak.error().message});
                co_return std::unexpected(leak.error());
            }
        }
        co_return response;
    }

    // ADR-057 §9 (Design B: abort-and-replay for `agent.ask()`): `resolve_interaction()`'s
    // `codeact_ask` branch, factored out here because it needs the SAME `on_context`/`invoke_tool`/
    // `on_turn_end` shape the approval branch (`resolve_interaction()`, above) already uses, but
    // diverges in two load-bearing ways -- (1) the call it re-invokes is built from the STORED
    // `source`/`language`, never from `pending_calls`' own arguments (bypassing the model and the
    // public `ExecuteCodeArgs` schema entirely, since this is host-driven replay, not a new
    // model-issued call); (2) it may need to leave the SAME interaction open a second/third time
    // (chained `agent.ask()` calls) rather than always closing it the way an approval resolution
    // always does. `interaction_id` has already been validated as open, with `history_`'s tail still
    // the exact suspended assistant tool-call message, by the caller (`resolve_interaction()`) before
    // this is reached -- this function does not re-check either.
    task<result<AgentResponse>> resolve_codeact_ask(ResolveInteraction const& request,
                                                       std::string const& interaction_id) {
        if (!request.answer.has_value()) {
            co_return std::unexpected(error{
                failure_class::contract,
                "resolving a codeact_ask interaction requires an answer",
                "session.resolve_interaction.answer_required"});
        }
        auto rec_it = pending_codeact_asks_.find(interaction_id);
        if (rec_it == pending_codeact_asks_.end()) {
            // Round 8 red-team, finding 16 (LOW): this comment used to claim "should be unreachable in
            // practice" -- WRONG, corrected here. `restore_from_record()` (below) restores
            // `open_interactions_`, which can contain a `codeact_ask`-reason `Interaction`, but never
            // restores `pending_codeact_asks_` (`PendingCodeActAsk`'s own comment already discloses
            // this as a deliberate, not-yet-solved durability gap) -- so resolving a codeact_ask
            // interaction that survived a session restore genuinely reaches here. Previously this
            // branch returned `fatal` WITHOUT erasing the interaction from `open_interactions_`,
            // leaving it stuck open forever with no cancel path (every future resolve attempt against
            // the same id hit this identical branch again). Fixed: erase it here too (best-effort --
            // if this ALSO fails there is nothing further to reconcile, the fatal error below is
            // returned either way) so the interaction closes cleanly even though the underlying work
            // cannot be resumed -- still fails closed, but recoverably instead of permanently stuck.
            result<void> const erased = resolve_interaction_record(interaction_id);
            (void)erased;
            co_return std::unexpected(error{
                failure_class::fatal,
                "internal error: no stored codeact-ask record for this open interaction (likely a "
                "session restore mid-ask -- pending_codeact_asks_ is not durably checkpointed)",
                "session.resolve_interaction.codeact_ask_record_missing"});
        }
        rec_it->second.answers_so_far.push_back(*request.answer);

        emit_run_event(run_event_kind::input_resolved, run_event_payload::InteractionRef{interaction_id});

        // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
        SessionContext session_ctx{session_id_, effect_context_.principal, history_};
        result<ContextContribution> contribution =
            co_await history_provider_.on_context(session_ctx, effect_context_);
        if (!contribution) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{"run.context_unavailable",
                                                          contribution.error().message});
            co_return std::unexpected(contribution.error());
        }
        ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        // ADR-061 §20.6: per-request, not session-level -- effect_context_.capabilities is freshly
        // written by apply_dispatch_authority() at the top of every real entry point (start_run(),
        // resolve_interaction()) before this is ever reached.
        CapabilitySet const& held      = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;
        ApprovalDecider const one_shot_approve = [](Principal const&, std::string_view, std::string const&) {
            return true;
        };

        // Rebuilt directly from the STORED record, never from `pending_calls`' own
        // `arguments_json` -- see this function's own top comment for why.
        json::Value const args = json::Value::make_object({
            {"code", json::Value::make_string(rec_it->second.source)},
            {"language", json::Value::make_string(rec_it->second.language)},
        });
        ToolCallRequest const req{rec_it->second.tool_call_id, "execute_code", args,
                                    /*arguments_tainted=*/false, /*call_index=*/0,
                                    call_provenance::vendor_structured};

        emit_run_event(run_event_kind::tool_call_started,
                        run_event_payload::ToolCallStarted{req.call_id, req.tool_name});
        // ADR-057 §9: the ONE place `EffectContext::codeact_preseeded_answers` is ever set -- for the
        // exact duration of this one `invoke_tool()` call, cleared immediately after regardless of
        // outcome, the same discipline `tool_pipeline.hpp`'s own `ctx.bound_capabilities` bracket
        // already uses for a different per-call field on this same struct.
        effect_context_.codeact_preseeded_answers = rec_it->second.answers_so_far;
        // ADR-060: same bracketing discipline as `codeact_preseeded_answers` immediately above, its
        // own independent set/clear pair around this same call -- both fields are per-call, neither
        // implies the other.
        effect_context_.report_progress = [this, call_id = req.call_id](ContentItem item) {
            detail::force_tainted(item);
            emit_run_event(run_event_kind::tool_call_delta,
                            run_event_payload::ToolCallDelta{call_id, std::move(item)});
        };
        ToolInvocationAudit audit;
        // Named `tool_result`, not `result` -- the latter would shadow the `agentengine::result<T>`
        // alias template for the rest of this function's scope (a real MSVC C2760 hit while writing
        // this, not a hypothetical style nit -- `result<void>` below would otherwise parse as
        // `(local variable result) < void` instead of a template-id).
        ToolResult tool_result =
            invoke_tool(tool_table, held, req, effect_context_, one_shot_approve, &audit);
        effect_context_.codeact_preseeded_answers.clear();
        effect_context_.report_progress = [](ContentItem) {};
        emit_run_event(run_event_kind::tool_call_finished,
                        run_event_payload::ToolCallFinished{audit.call_id, tool_result});

        if (!audit.ok && audit.error_code == "codeact.ask_pending") {
            // Red-team finding (docs/planning/quickstart-session-builder-design-draft.md's own §0-series
            // history names it as the session_builder.hpp "finding 7" investigation's byproduct, though
            // the bug itself lives here, in AgentSession, not in that facade): this branch used to
            // `co_return` WITHOUT ever touching `effect_context_.turn_index` -- the ONLY field
            // `run_rounds()`'s own `max_turns_` bound (below) ever inspects. Since `run_rounds()` is not
            // re-entered while an interaction keeps resolving to ask-pending (the "completed" branch
            // below is the only path that calls back into it), a CodeAct script that keeps asking
            // follow-up questions forever was COMPLETELY unbounded by `.max_turns()`/`.token_budget()` --
            // LIVE-REPRODUCED: 50 `resolve_interaction()` round trips against a scripted always-ask tool,
            // `max_turns_ == 3`, never once produced `run.max_turns_exceeded`, `turn_index` never left 0.
            // Fixed the same way the ordinary (non-codeact) approval-resume branches one function up
            // already do (`resolve_interaction()`'s own `:943`/`:998`, which increment once per call
            // regardless of approved/denied): count THIS round of ask/resolve work against the bound,
            // then refuse to suspend for yet another ask once the cap is reached -- fails closed with the
            // identical `run.max_turns_exceeded` `run_rounds()`'s own fallthrough produces, instead of
            // silently granting an ask-loop unlimited rounds no other resume path gets.
            ++effect_context_.turn_index;
            if (max_turns_.has_value() && effect_context_.turn_index >= *max_turns_) {
                pending_codeact_asks_.erase(rec_it);
                result<void> const erased = resolve_interaction_record(interaction_id);
                if (!erased) co_return std::unexpected(erased.error());
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{
                                    "run.max_turns_exceeded",
                                    "codeact ask loop did not converge within max_turns"});
                co_return std::unexpected(
                    error{failure_class::contract,
                          "codeact ask loop did not converge within max_turns",
                          "run.max_turns_exceeded"});
            }

            std::string prompt;
            if (!tool_result.content.empty()) {
                if (auto const* e = std::get_if<Error>(&tool_result.content.front().value)) prompt = e->message;
            }
            rec_it->second.prompt = prompt;
            // 013 SS2.2 hard ordering obligation: the prompt a resume needs must precede the
            // interrupt-bearing terminal event, so codeact_ask_requested is emitted FIRST and the
            // AG-UI projector carries its text out on input_required's own Interrupt.message.
            emit_run_event(run_event_kind::codeact_ask_requested,
                            run_event_payload::CodeActAskRequested{req.call_id, interaction_id, prompt});
            emit_run_event(run_event_kind::input_required,
                            run_event_payload::InteractionRef{interaction_id});
            co_return std::unexpected(error{failure_class::contract,
                                             "round suspended awaiting an agent.ask() answer",
                                             kSuspendedForCodeActAsk});
        }

        // Completed -- success or an ordinary tool failure, either way NOT another ask-pending.
        // Closes the interaction, folds the real ToolResult exactly where the original call would
        // have landed, and continues run_rounds() normally, matching the approval branch's own shape
        // one function up.
        pending_codeact_asks_.erase(rec_it);
        result<void> const erased = resolve_interaction_record(interaction_id);
        if (!erased) co_return std::unexpected(erased.error());

        std::size_t const response_msg_index = history_.size() - 1;
        std::vector<ToolResult> results;
        results.push_back(std::move(tool_result));
        history_.push_back(tool_results_message(std::move(results)));
        (void)co_await history_provider_.on_turn_end(
            TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                history_.size() - response_msg_index}},
            effect_context_);
        emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
        ++effect_context_.turn_index;
        co_return co_await run_rounds();
    }

    // OQ-21: the shared tail for a round whose hook-stage processing is already fully decided (every
    // call's outcome known -- pass_through or denied) -- used by BOTH resolve_hook_decision() below
    // (passing the real `approval_decider_`, only once every remaining pass_through call has already
    // been proven to need no further human decider) AND resolve_interaction()'s approved branch
    // (passing `one_shot_approve` -- see that branch's own comment for exactly why that reuse is safe
    // there and NOT safe to do directly from resolve_hook_decision()). `approve` is threaded through
    // explicitly as a parameter rather than read off a member so both callers can supply their own,
    // the same shape `invoke_tool()` itself already takes an `ApprovalDecider const&` one layer down.
    // `response_msg_index` is `history_.size() - 1`, matching resolve_interaction()'s own convention
    // (not run_rounds()'s `history_.size()`) -- both real callers reach here with `history_.back()`
    // already the pending assistant tool-call message that opened this interaction; this function
    // never itself pushes that message, only the tool-results message that answers it.
    //
    // `policy` defaults to `{}` (never consulted) -- matching the SAME "already resolved by a human,
    // never re-litigated by policy" discipline `set_policy_decider()`'s own comment documents for the
    // three pre-existing one-shot-approve call sites: resolve_interaction()'s approved branch (this
    // function's OTHER caller, which relies on that default) must NOT pass a real `PolicyDecider` here
    // -- a `policy_driven` tool's `auto_deny` verdict would otherwise silently override a human's
    // explicit round-level approval. resolve_hook_decision() below is the one caller that DOES pass
    // the real `policy_decider_` explicitly: reaching it means NO human ever approved this round (it
    // suspended purely for external dispatch, `any_still_needs_approval` proved false against that
    // SAME real `policy_decider_`) -- so a `pass_through` call here is exactly as unresolved-by-a-
    // human as `run_rounds()`'s own un-suspended loop, and must consult policy identically (omitting
    // it would make invoke_tool()'s own step 5 misclassify a `policy_driven` tool as `needs_decider`
    // purely from the absence of a policy argument, denying a call policy had already auto-approved).
    task<result<AgentResponse>> finish_hook_processed_round(PendingHookDecisionRound round,
                                                              ToolTable const& tool_table,
                                                              ApprovalDecider const& approve,
                                                              PolicyDecider const& policy = {}) {
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        CapabilitySet const& held = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;
        std::size_t const response_msg_index = history_.size() - 1;
        // ADR-160 §5: hook-denied calls are filtered out BEFORE reaching dispatch_tool_calls() --
        // "no tool_call_started/finished for a call that never actually ran" precedent, unchanged.
        // `results` is sized once, up front, so a denied call's slot and a dispatched call's slot
        // are both filled by index, regardless of dispatch order.
        std::vector<ToolResult> results(round.calls.size());
        std::vector<ToolCallRequest> reqs;
        std::vector<std::size_t> req_positions;  // reqs[j] belongs at results[req_positions[j]]
        reqs.reserve(round.calls.size());
        req_positions.reserve(round.calls.size());
        for (std::size_t i = 0; i < round.calls.size(); ++i) {
            HookProcessedCall& hc = round.calls[i];
            if (hc.outcome == hook_call_outcome::denied) {
                results[i] = std::move(*hc.denial_result);
                continue;
            }
            reqs.push_back(hc.request);
            req_positions.push_back(i);
        }
        std::vector<DispatchedCall> dispatched = dispatch_tool_calls(reqs, tool_table, held, approve, policy);
        for (std::size_t j = 0; j < dispatched.size(); ++j) {
            results[req_positions[j]] = std::move(dispatched[j].result);
        }
        history_.push_back(tool_results_message(std::move(results)));
        (void)co_await history_provider_.on_turn_end(
            TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                history_.size() - response_msg_index}},
            effect_context_);
        emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
        ++effect_context_.turn_index;
        co_return co_await run_rounds();
    }

    // OQ-21: `resolve_interaction()`'s `hook_decision` branch (`:hook_decision` dispatch above) --
    // needs the SAME on_context/finish-tail shape `resolve_codeact_ask()` above already uses, but
    // diverges in the one load-bearing way that closes this design's own red-team-confirmed fatal
    // finding: unlike `resolve_codeact_ask()`, this function NEVER reuses `one_shot_approve` for the
    // whole round. A `hook_decision` resume answers "did the external process allow/deny/rewrite the
    // call", which is a DIFFERENT question from "did a human approve its execution" -- so every
    // remaining `pass_through` call is re-checked against the REAL deciders (`approval_decider_`/
    // `policy_decider_`) after folding in the external answers, and cascades to a genuine
    // `interaction_reason::approval` suspend (carrying the same stored round forward) if a decider is
    // still needed and none is configured, rather than ever treating the hook's own answer as an
    // approval. `interaction_id` has already been validated as open by the caller
    // (`resolve_interaction()`) before this is reached -- this function does not re-check.
    //
    // NEVER acquires `session_mutex_` -- reached only as a sibling call from inside
    // `resolve_interaction()`'s already-held guard (I1), exactly like `resolve_codeact_ask()` above.
    // `AsyncMutex` (rt/async_mutex.hpp) has no owner tracking and no timeout: a second lock attempt
    // from the same coroutine parks itself as a waiter that only an `unlock()` from that same
    // (now-suspended) frame could ever release -- a permanent, unrecoverable deadlock of the whole
    // session. This function, and every helper it calls (including
    // `enforce_hook_rewritten_tool_call_provenance()`), must never call or `co_await`
    // `session_mutex_.lock()`.
    task<result<AgentResponse>> resolve_hook_decision(ResolveInteraction const& request,
                                                         std::string const& interaction_id) {
        auto it = pending_hook_decisions_.find(interaction_id);
        if (it == pending_hook_decisions_.end()) {
            co_return std::unexpected(error{
                failure_class::contract, "no pending hook-decision state for this interaction",
                "session.hook_decision.unknown"});
        }
        if (!request.hook_dispatch_answers) {
            co_return std::unexpected(error{
                failure_class::contract, "hook_decision resume requires hook_dispatch_answers",
                "session.hook_decision.missing_answers"});
        }
        PendingHookDecisionRound round = std::move(it->second);
        pending_hook_decisions_.erase(it);

        // Fold in the external answers. Fails closed on any gap -- no partial-round resolution,
        // since resolve_interaction_record() below closes the WHOLE interaction on this one resume.
        for (HookProcessedCall& hc : round.calls) {
            if (hc.outcome != hook_call_outcome::needs_external_dispatch) continue;
            auto ans = std::find_if(request.hook_dispatch_answers->begin(),
                                     request.hook_dispatch_answers->end(),
                                     [&](agentengine::HookDispatchAnswer const& a) {
                                         return a.call_id == hc.request.call_id;
                                     });
            if (ans == request.hook_dispatch_answers->end()) {
                co_return std::unexpected(error{
                    failure_class::contract,
                    "hook_decision resume missing an answer for call_id " + hc.request.call_id,
                    "session.hook_decision.incomplete"});
            }
            if (!ans->approved) {
                hc.outcome = hook_call_outcome::denied;
                hc.denial_result = make_denial_result(
                    hc.request.call_id, ans->denial_message.value_or("denied by external hook process"),
                    "tool.hook_denied");
                continue;
            }
            json::Value const original_arguments = hc.request.arguments;
            if (ans->rewritten_arguments) hc.request.arguments = *ans->rewritten_arguments;
            enforce_hook_rewritten_tool_call_provenance(hc.request, original_arguments);
            hc.outcome = hook_call_outcome::pass_through;
        }

        result<void> const resolved = resolve_interaction_record(interaction_id);
        if (!resolved) co_return std::unexpected(resolved.error());
        emit_run_event(run_event_kind::input_resolved, run_event_payload::InteractionRef{interaction_id});

        // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
        SessionContext session_ctx{session_id_, effect_context_.principal, history_};
        result<ContextContribution> contribution =
            co_await history_provider_.on_context(session_ctx, effect_context_);
        if (!contribution) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{"run.context_unavailable",
                                                          contribution.error().message});
            co_return std::unexpected(contribution.error());
        }
        ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);

        // Deliberately NOT `one_shot_approve` here, unlike resolve_interaction()'s approved branch --
        // see this function's own top comment. Re-checked with the SAME gating condition
        // run_rounds()'s own suspend-for-approval pre-check uses (`suspend_for_approval_ &&
        // !approval_decider_`) -- a session that never opts into suspending for approval, or that
        // already has a real ApprovalDecider wired, must behave identically here to everywhere else:
        // invoke_tool() consults `approval_decider_` directly (denying if unset), it never cascades
        // into a suspend that could not otherwise have happened for this session's configuration.
        bool any_still_needs_approval = false;
        if (suspend_for_approval_ && !approval_decider_) {
            for (HookProcessedCall const& hc : round.calls) {
                if (hc.outcome != hook_call_outcome::pass_through) continue;
                ToolDescriptor const* td = tool_table.find(hc.request.tool_name);
                if (td != nullptr &&
                    resolve_approval_outcome(*td, hc.request.provenance, effect_context_.principal,
                                              /*arguments_tainted=*/true, policy_decider_) ==
                        approval_outcome::needs_decider) {
                    any_still_needs_approval = true;
                    break;
                }
            }
        }
        if (any_still_needs_approval) {
            // Cascading suspend: this is a NEW, legitimate suspend, not a bug -- a hook answering its
            // own external-dispatch question does not itself satisfy a separate human-approval need.
            Interaction const& next = open_interaction(effect_context_.run_id, interaction_reason::approval);
            pending_hook_decisions_[next.interaction_id] = std::move(round);
            emit_run_event(run_event_kind::input_required,
                            run_event_payload::InteractionRef{next.interaction_id});
            for (HookProcessedCall const& hc : pending_hook_decisions_[next.interaction_id].calls) {
                if (hc.outcome == hook_call_outcome::pass_through) {
                    emit_run_event(run_event_kind::approval_requested,
                                    run_event_payload::ApprovalRequested{hc.request.call_id,
                                                                          next.interaction_id});
                }
            }
            co_return std::unexpected(error{failure_class::contract,
                                             "round suspended awaiting human approval after hook dispatch",
                                             kSuspendedForApproval});
        }

        // Nothing left needs a decider (already proven above) -- proceed exactly like run_rounds()'s
        // own invoke loop, folding results and continuing the turn. `policy_decider_` IS passed here
        // (unlike resolve_interaction()'s approved-branch call below) -- see finish_hook_processed_
        // round()'s own comment for exactly why this caller must thread it through.
        co_return co_await finish_hook_processed_round(std::move(round), tool_table, approval_decider_,
                                                          policy_decider_);
    }

    // Same shape as core/agent_session.hpp's own run_rounds() -- ported to rt::task<T>, no longer
    // templated on AskT (there is only one caller shape now, a plain `result<AgentResponse>` return),
    // otherwise byte-for-byte identical logic.
    task<result<AgentResponse>> run_rounds() {
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        // ADR-061 §20.6: per-request, not session-level -- effect_context_.capabilities is freshly
        // written by apply_dispatch_authority() at the top of every real entry point (start_run(),
        // resolve_interaction()) before this is ever reached.
        CapabilitySet const& held      = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;

        for (; !max_turns_.has_value() || effect_context_.turn_index < *max_turns_;
             ++effect_context_.turn_index) {
            emit_run_event(run_event_kind::turn_started, run_event_payload::Turn{effect_context_.turn_index});

            // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
        SessionContext session_ctx{session_id_, effect_context_.principal, history_};
            result<ContextContribution> contribution =
                co_await history_provider_.on_context(session_ctx, effect_context_);
            if (!contribution) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.context_unavailable",
                                                              contribution.error().message});
                co_return std::unexpected(contribution.error());
            }

            // Gap-audit finding 20 / 003 §8 Q2 ("exclude from context assembly, never translate"):
            // strip any `Reasoning` content item this turn's contribution carries that did NOT
            // originate from the currently-bound backend, before it ever reaches `ChatRequest`.
            // Skipped entirely (no filtering, unchanged behavior) when `ChatClientT` doesn't expose
            // an identity to compare against -- `if constexpr` on `HasProducerChatClientId`, never a
            // runtime branch, so every existing mock/test ChatClientT is completely unaffected.
            if constexpr (agentengine::HasProducerChatClientId<ChatClientT>) {
                if (chat_client_) {
                    detail::filter_cross_provider_reasoning(
                        *contribution, chat_client_->producer_chat_client_id(),
                        [this](run_event_kind k, RunEventPayload p) { emit_run_event(k, std::move(p)); });
                }
            }

            // ADR-053 §5 follow-up: offer `schedule_wakeup` as a real, callable tool this turn --
            // injected here, once per turn, rather than via the agent's own static `Tools<...>` policy
            // list (ADR-028's own precedent: session-scoped tools contributed dynamically, extended one
            // step further -- this closure captures the session itself, `this`, not a `ContextProvider`
            // member). ONLY offered when the session actually holds a `cap::Schedule` grant: never
            // advertise a tool the model could never successfully call (a confusing, wasted turn), and
            // never let an ungranted session synthesize a de facto capability grant by merely existing.
            // ADR-061 §20.6/§19.7: reuses the SAME `held` computed above (not a fresh session-level
            // re-derivation) -- the offer decision and the enforcement decision now read the identical
            // per-request source, closing the "offered but would-be-denied-differently" inconsistency
            // a session-level re-check here would otherwise reopen.
            if (held.find_schedule().has_value()) {
                contribution->tools.push_back(make_tool_descriptor_with_invoke<ScheduleWakeupTool>(
                    [this](ScheduleWakeupArgs args, EffectContext& ctx) -> result<ScheduleWakeupReply> {
                        // ADR-061 §20.6: `ctx` is the real, per-request EffectContext invoke_tool()
                        // hands this closure -- calls schedule_wakeup_impl() DIRECTLY (never the
                        // locking public schedule_wakeup() wrapper: this closure already runs inside
                        // the session_mutex_ lock via run_rounds() -> invoke_tool(), and a non-
                        // reentrant AsyncMutex would deadlock on a second co_await lock()).
                        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
                        CapabilitySet const& call_held =
                            ctx.capabilities ? *ctx.capabilities : empty_caps;
                        // `args.label` passed by value (not moved) -- the state_changed emission
                        // below still needs it after the registry call returns.
                        auto effect = standing_effects_registry_.schedule_wakeup_impl(
                            std::chrono::milliseconds(args.delay_ms), args.label,
                            std::chrono::steady_clock::now(), call_held, ctx.principal, ctx.run_id,
                            session_id_);
                        if (!effect) return std::unexpected(effect.error());
                        emit_run_event_for(ctx.run_id, run_event_kind::state_changed,
                                            run_event_payload::StateChanged{
                                                "schedule_wakeup armed: " + args.label});
                        return ScheduleWakeupReply{effect->handle_id};
                    }));
            }

            // decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md, wired in for real:
            // the ONE genuine `pre_model`/`turn` seam in this file -- unlike the `on_context()` call
            // sites in `resolve_interaction()`/`resolve_codeact_ask()` (above), which only ever
            // build a `ToolTable` to dispatch an ALREADY-DECIDED tool call and never reach the model
            // at all, THIS contribution is about to become the round's own `ChatRequest`. Runs after
            // the Reasoning filter and the dynamically-injected `schedule_wakeup` tool above, so a
            // turn middleware sees the FINAL tool surface, and before `.instructions` is materialized
            // into a plain `Message` below, so `redact_subspan()` still has a real `TaintedText` to
            // operate on (`turn_middleware.hpp`'s own documented ordering requirement). Wrapping the
            // raw `ContextContribution` in a fresh `ContextAssemblyResult` with an empty `drops` list
            // is the adapter this seam needs -- `AgentSession` calls `history_provider_.on_context()`
            // directly, never `assemble_context()` itself, so there is no `ContextAssemblyResult`
            // already in hand the way ADR-066's own seam has one.
            if (turn_middleware_hook_) {
                agentengine::ContextAssemblyResult assembled_for_turn{std::move(*contribution), {}};
                agentengine::TurnContext turn_ctx{assembled_for_turn};
                result<std::monostate> const turn_outcome = co_await turn_middleware_hook_(turn_ctx);
                *contribution = std::move(assembled_for_turn.combined);
                if (!turn_outcome) {
                    emit_run_event(run_event_kind::run_failed,
                                    run_event_payload::RunFailed{"run.turn_denied",
                                                                  turn_outcome.error().message});
                    co_return std::unexpected(turn_outcome.error());
                }
            }

            ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);
            // Gap-16 fix (2026-08-14): `contribution->instructions` used to be read this far and then
            // never referenced again -- silently dropped, never reaching the model. The ONE explicit
            // declassification site for the whole engine: `.unsafe_view()` here does not itself decide
            // anything is safe -- that decision was already made, explicitly, by whichever
            // `ContextProvider` constructed the `TaintedText` (context_provider.hpp's own comment).
            // This just materializes an already-vetted value onto the wire, prepended so it establishes
            // context ahead of everything else, matching a `ComposedContextProvider<Skills, History>`
            // declaration's own system-message-first wire convention (tools/cli_chat.cpp) -- a second, independent role::system
            // message from another contributor coexists fine (both real backends already concatenate
            // every role::system message they see, not just the first).
            if (contribution->instructions.has_value()) {
                Message instructions_msg;
                instructions_msg.role = role::system;
                ContentItem item;
                item.origin  = content_origin::system;
                item.tainted = false;  // already declassified above, not re-derived from tainted input
                item.value   = Text{contribution->instructions->unsafe_view()};
                instructions_msg.content.push_back(std::move(item));
                contribution->messages.insert(contribution->messages.begin(), std::move(instructions_msg));
            }
            // docs/planning/agent-spawn-runtime-design-draft.md §4.6 (item 6, OQ-16). A SECOND,
            // independent `role::system` message, built from `static_instructions_` -- see
            // `set_static_instructions()`'s own comment above for why this is unconditionally
            // untainted (host/engine-derived from a `CapabilitySet`, never model output, so no
            // `TaintedText` declassification step applies the way `contribution->instructions` above
            // needed one). No-op (empty string, nothing pushed) until a caller actually calls
            // `set_static_instructions()` -- every existing session unaffected.
            //
            // ORDERING FIX (found live 2026-09-03 -- the project owner inspected OpenRouter's own
            // dashboard/request-log view mid-run and spotted the system message NOT at index 0): this
            // used to be `contribution->messages.push_back(...)` -- appended at the ABSOLUTE END of
            // the fully-assembled message list, AFTER every turn of real conversation history AND the
            // current turn's own new user message. Invisible on a session's very first turn (nothing
            // else in `contribution->messages` yet to land after), which is exactly why
            // test_rt_agent_session_instructions.cpp's T4/T5 -- both single-turn -- never caught it; a
            // genuinely multi-turn session
            // (test_workflow_research_pipeline_large_context_live_e2e.cpp's market/technical
            // specialists) makes it visible on the real wire: the system prompt landed as the LAST
            // message in a growing conversation, directly contradicting `contribution->instructions`'
            // own comment just above ("prepended so it establishes context ahead of everything
            // else"). Fixed by inserting right after any already-prepended `contribution->instructions`
            // message (index 1) or at the very front (index 0) if there is none -- preserving T5's own
            // "contribution's own message first, static second" order, and now correctly ahead of
            // every history/turn message no matter how many turns have already accumulated.
            if (!static_instructions_.empty()) {
                Message static_instructions_msg;
                static_instructions_msg.role = role::system;
                ContentItem item;
                item.origin  = content_origin::system;
                item.tainted = false;  // host/engine-derived (CapabilitySet), never model output (I3)
                item.value   = Text{static_instructions_};
                static_instructions_msg.content.push_back(std::move(item));
                std::size_t const insert_pos = contribution->instructions.has_value() ? 1 : 0;
                contribution->messages.insert(
                    contribution->messages.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                    std::move(static_instructions_msg));
            }
            ChatRequest request{contribution->messages, contribution->tools};
            // ADR-058 §8 (Design B) -- scoped to `native` ONLY, deliberately. Both real backends'
            // own translation code (protocol/openai/chat_client.hpp:289-293,
            // protocol/anthropic/chat_client.hpp:409-413) serialize `request.output_schema_json`
            // onto the wire UNCONDITIONALLY whenever it is set -- neither checks
            // `ChatClientCapabilities.structured_output_native` first. So this scoping is a real,
            // load-bearing necessity, not belt-and-suspenders: if this field were populated while
            // `output_schema_strategy_` were `tool_shaped`/`parse_and_repair` (the two strategies
            // §3 deliberately leaves unimplemented), a backend with no real support contract for
            // constrained decoding would still send the field to the provider, an ADR-058 open
            // sub-question this line's own scoping resolves rather than assumes.
            if (output_schema_validate_ &&
                output_schema_strategy_ == agentengine::output_schema_strategy::native) {
                request.output_schema_json = output_schema_json_;
            }
            if (!chat_client_) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.no_chat_client", "no ChatClientT configured"});
                co_return std::unexpected(
                    error{failure_class::contract, "no ChatClientT configured", "run.no_chat_client"});
            }
            emit_run_event(run_event_kind::model_call_started);
            result<ChatResponse> response = co_await run_model_call(request, effect_context_);
            emit_run_event(run_event_kind::model_call_finished);
            if (!response) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.chat_failed", response.error().message});
                co_return std::unexpected(response.error());
            }

            run_tokens_consumed_ += response->usage.input_tokens + response->usage.output_tokens;
            // ADR-163: accumulated alongside run_tokens_consumed_ above, unconditionally, at the same
            // point -- see run_usage_'s own comment for why this is a full-fidelity parallel field.
            run_usage_.input_tokens += response->usage.input_tokens;
            run_usage_.output_tokens += response->usage.output_tokens;
            run_usage_.cached_input_tokens += response->usage.cached_input_tokens;
            run_usage_.reasoning_tokens += response->usage.reasoning_tokens;
            run_usage_.cost_estimate += response->usage.cost_estimate;
            run_usage_.cache_write_tokens += response->usage.cache_write_tokens;
            if (token_budget_.has_value() && run_tokens_consumed_ > *token_budget_) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.token_budget_exceeded",
                                                              "per-run token budget exceeded"});
                co_return std::unexpected(error{failure_class::resource, "per-run token budget exceeded",
                                                 "run.token_budget_exceeded"});
            }

            std::size_t const response_msg_index = history_.size();
            history_.push_back(response->message);

            std::vector<ToolCall> const calls = tool_calls_of(response->message);
            if (calls.empty()) {
                (void)co_await history_provider_.on_turn_end(
                    TurnView{std::span<Message const>{history_.data() + response_msg_index, 1}},
                    effect_context_);
                emit_run_event(run_event_kind::turn_finished,
                                run_event_payload::Turn{effect_context_.turn_index});

                // ADR-058 §8 (Design B) -- the round already decided "no more tool calls, this is
                // the final answer" (B5's identified seam). Applies REGARDLESS of which strategy
                // was chosen (native/tool_shaped/parse_and_repair all get validated the same way --
                // only whether the REQUEST carried a native constraint differed, above). A session
                // with no set_output_schema() call at all (output_schema_validate_ unset) never runs
                // this branch at all -- O3's own regression/positive-control claim.
                std::optional<std::string> structured_output_json;
                if (output_schema_validate_) {
                    std::string text_content = text_of(response->message);
                    result<void> const validated = output_schema_validate_(text_content);
                    if (!validated) {
                        emit_run_event(run_event_kind::run_failed,
                                        run_event_payload::RunFailed{
                                            "run.output_schema_validation_failed",
                                            validated.error().message});
                        // Fail the run closed -- never a silent pass-through of unvalidated text as
                        // if it were the structured result (ADR-058 §8, a deliberate, documented
                        // choice; 003 §4 does not itself specify this, per ADR-058 §7's own residual).
                        co_return std::unexpected(error{failure_class::contract,
                                                         validated.error().message,
                                                         "run.output_schema_validation_failed"});
                    }
                    structured_output_json = std::move(text_content);
                }

                emit_run_event(run_event_kind::run_finished);
                co_return AgentResponse{response->message, response->usage,
                                         std::move(structured_output_json)};
            }

            // OQ-21's tool-call hook stage (core/tool_call_hook.hpp) -- runs once per round, per
            // call, strictly BEFORE the suspend-for-approval pre-check below, gated on
            // `tool_call_hook_ != nullptr` so a session that never opts in pays nothing extra (no
            // extra branch, no extra allocation -- `processed` stays empty and every read below that
            // is itself gated on `hook_touched_round` takes the exact byte-for-byte original path).
            //
            // `processed` and `any_needs_approval` are computed from the SAME post-hook per-call
            // state, in the SAME pass, before choosing which single `Interaction` (if any) to open --
            // this is the fix for a red-team-confirmed fatal finding in an earlier draft: opening a
            // `hook_decision` interaction whenever ANY call needed external dispatch, independently of
            // whether some OTHER call in the round also needed human approval, let
            // `resolve_hook_decision()`'s own resume silently skip the approval question entirely (an
            // authority bypass). Here, `any_needs_approval` is always evaluated against the REAL
            // post-hook requests (hook-rewritten arguments already provenance-downgraded by
            // `enforce_hook_rewritten_tool_call_provenance()` below), regardless of which reason ends
            // up suspending the round -- so a call that needs a human decider can never be silently
            // skipped just because `hook_decision` "got there first".
            std::vector<HookProcessedCall> processed;      // built only if tool_call_hook_ is set
            bool const hook_touched_round = static_cast<bool>(tool_call_hook_);

            if (hook_touched_round) {
                processed.reserve(calls.size());
                for (std::size_t i = 0; i < calls.size(); ++i) {
                    ToolCallRequest req = tool_call_request_of(calls[i], i);
                    json::Value const original_arguments = req.arguments;  // snapshot BEFORE the hook runs

                    ToolCallHookContext hctx{
                        .call_id = req.call_id, .tool_name = req.tool_name, .arguments = req.arguments,
                        .provenance = req.provenance, .caller = effect_context_.principal,
                    };
                    result<std::monostate> const ran = co_await tool_call_hook_(hctx);
                    if (!ran) {
                        processed.push_back(HookProcessedCall{
                            req, hook_call_outcome::denied,
                            make_denial_result(req.call_id, "tool-call hook failed: " + ran.error().message,
                                                "tool.hook_error")});
                        continue;
                    }
                    if (hctx.rewritten_arguments) req.arguments = *hctx.rewritten_arguments;
                    // Unconditional -- never gated on what the hook itself claims about provenance
                    // (`ToolCallHookContext` has no field through which it could assert one, by
                    // design; see that struct's own file-top comment).
                    enforce_hook_rewritten_tool_call_provenance(req, original_arguments);

                    // `denial` wins if a hook body sets both `denial` and `needs_external_dispatch` --
                    // never "deny AND also dispatch".
                    if (hctx.denial) {
                        processed.push_back(HookProcessedCall{
                            req, hook_call_outcome::denied,
                            make_denial_result(req.call_id, hctx.denial->message,
                                                hctx.denial->code.empty() ? "tool.hook_denied"
                                                                            : hctx.denial->code)});
                    } else if (hctx.needs_external_dispatch) {
                        processed.push_back(
                            HookProcessedCall{req, hook_call_outcome::needs_external_dispatch, std::nullopt});
                    } else {
                        processed.push_back(HookProcessedCall{req, hook_call_outcome::pass_through, std::nullopt});
                    }
                }
            }

            bool const any_needs_external_dispatch =
                hook_touched_round &&
                std::any_of(processed.begin(), processed.end(), [](HookProcessedCall const& p) {
                    return p.outcome == hook_call_outcome::needs_external_dispatch;
                });

            bool any_needs_approval = false;
            if (suspend_for_approval_ && !approval_decider_) {
                for (std::size_t i = 0; i < calls.size(); ++i) {
                    // A call the hook stage already denied is finished -- its outcome is already
                    // decided, never re-litigated by approval.
                    if (hook_touched_round && processed[i].outcome == hook_call_outcome::denied) continue;
                    ToolCallRequest const& req_i =
                        hook_touched_round ? processed[i].request : tool_call_request_of(calls[i], i);
                    ToolDescriptor const* td = tool_table.find(req_i.tool_name);
                    // ADR-070: a `policy_decider_`-resolved policy_driven call (auto_approve/
                    // auto_deny) never needs a real human -- only `needs_decider` should count
                    // toward suspending this round; `resolve_approval_outcome` with `policy_decider_`
                    // unset reproduces `tool_call_requires_approval()`'s own boolean exactly, so this
                    // is unchanged behavior for every session that never wires a `PolicyDecider`.
                    // `req_i.provenance` reflects the hook's own rewrite (if any) when
                    // `hook_touched_round` -- `arguments_tainted` stays `true` unconditionally, the
                    // same "every ToolCall this loop sees originates from a model response" reasoning
                    // `tool_call_request_of`'s own comment already establishes (a hook rewrite never
                    // makes a model-originated call more trusted).
                    if (td != nullptr &&
                        resolve_approval_outcome(*td, req_i.provenance, effect_context_.principal,
                                                  /*arguments_tainted=*/true, policy_decider_) ==
                            approval_outcome::needs_decider) {
                        any_needs_approval = true;
                        break;
                    }
                }
            }

            if (any_needs_external_dispatch || any_needs_approval) {
                // `hook_decision` wins when both are true in the same round -- resolving it re-checks
                // approval need with the real deciders (resolve_hook_decision()'s own comment), so no
                // approval need is ever silently dropped by this choice.
                interaction_reason const reason = any_needs_external_dispatch
                                                       ? interaction_reason::hook_decision
                                                       : interaction_reason::approval;
                Interaction const& interaction = open_interaction(effect_context_.run_id, reason);

                // Populated whenever the hook stage touched this round AT ALL, for WHICHEVER reason
                // the round ends up suspending under -- this is the fix for a completeness finding
                // paired with the fatal one above: a hook-touched round that suspends for PLAIN
                // approval (any_needs_external_dispatch == false) must still carry its post-hook
                // state forward, or resolve_interaction()'s approved branch would silently rebuild
                // from `pending_calls` and bypass the hook stage entirely on resume.
                if (hook_touched_round) {
                    pending_hook_decisions_[interaction.interaction_id] =
                        PendingHookDecisionRound{std::move(processed)};
                }

                emit_run_event(run_event_kind::input_required,
                                run_event_payload::InteractionRef{interaction.interaction_id});
                if (reason == interaction_reason::hook_decision) {
                    // 013 §2.2 hard ordering obligation -- see the codeact_ask sibling site below.
                    for (HookProcessedCall const& hc : pending_hook_decisions_[interaction.interaction_id].calls) {
                        if (hc.outcome == hook_call_outcome::needs_external_dispatch) {
                            emit_run_event(run_event_kind::hook_decision_requested,
                                            run_event_payload::HookDecisionRequested{
                                                hc.request.call_id, interaction.interaction_id,
                                                hc.request.tool_name});
                        }
                    }
                    co_return std::unexpected(error{
                        failure_class::contract,
                        "round suspended awaiting an external tool-call hook decision",
                        kSuspendedForHookDecision});
                }
                for (ToolCall const& call : calls) {
                    emit_run_event(run_event_kind::approval_requested,
                                    run_event_payload::ApprovalRequested{call.call_id,
                                                                          interaction.interaction_id});
                }
                // Suspended -- no real response yet. Unlike the Quark original (an unanswered Ask,
                // left to resolve later via a completely separate ResolveInteraction message with
                // no return value of its own to reconcile), THIS task<T> must complete with SOME
                // result<AgentResponse> the instant this round decides to suspend -- there is no
                // "leave it unanswered" primitive here. Folded into the error channel with a named
                // sentinel code (kSuspendedForApproval) the caller checks FIRST, before treating a
                // non-value result as a genuine failure -- see that constant's own comment for why
                // this is a real, open design question for a later slice, not a settled shape.
                co_return std::unexpected(error{failure_class::contract,
                                                 "round suspended awaiting human approval",
                                                 kSuspendedForApproval});
            }

            // ADR-160 §5: hook-denied calls are filtered out BEFORE reaching dispatch_tool_calls()
            // -- "no tool_call_started/finished for a call that never actually ran" precedent,
            // unchanged. `results` is sized once, up front, so a denied call's slot and a
            // dispatched call's slot are both filled by index, regardless of dispatch order.
            //
            // ADR-070: the ONLY invoke_tool() call site (via dispatch_tool_calls() below) that
            // consults `policy_decider_` -- the other three (resolve_interaction()'s approved
            // branch, start_background_task(), resolve_codeact_ask()) keep their default-`{}`
            // trailing parameter deliberately, since a call reaching any of them has already been
            // resolved by a real human and must never be re-litigated by policy (see
            // set_policy_decider()'s own comment).
            std::vector<ToolResult> results(calls.size());
            std::vector<ToolCallRequest> reqs;
            std::vector<std::size_t> req_positions;  // reqs[j] belongs at results[req_positions[j]]
            reqs.reserve(calls.size());
            req_positions.reserve(calls.size());
            for (std::size_t i = 0; i < calls.size(); ++i) {
                if (hook_touched_round && processed[i].outcome == hook_call_outcome::denied) {
                    results[i] = std::move(*processed[i].denial_result);
                    continue;
                }
                reqs.push_back(hook_touched_round ? processed[i].request
                                                    : tool_call_request_of(calls[i], i));
                req_positions.push_back(i);
            }

            std::vector<DispatchedCall> dispatched =
                dispatch_tool_calls(reqs, tool_table, held, approval_decider_, policy_decider_);

            for (std::size_t j = 0; j < dispatched.size(); ++j) {
                std::size_t const i = req_positions[j];
                ToolCallRequest const& req = reqs[j];
                ToolResult& result = dispatched[j].result;
                ToolInvocationAudit const& audit = dispatched[j].audit;

                // ADR-057 §9: a script inside `execute_code` called `agent.ask()` with no answer yet
                // available (Design B: abort-and-replay) -- `real_execute_code()`'s own host
                // implementation (cli_chat.cpp) is what maps this outcome to the sentinel error code
                // checked here; this is a real, deliberate producer/consumer contract between a
                // host's `execute_code` tool and this generic session loop, the same shape
                // `kSuspendedForApproval`'s own sentinel already establishes one layer up.
                //
                // ADR-160 §5 NAMED RESIDUAL: dispatch_tool_calls() above already ran EVERY call in
                // this batch (sequentially if the batch wasn't fan-out-eligible -- today, always --
                // or with some classes fanned out otherwise) before this check runs. The ORIGINAL
                // per-iteration loop stopped immediately on an ask-pending call, never running
                // anything after it; this refactor can only differ from that when calls.size() > 1
                // AND the batch was fan-out-eligible (today, unreachable: `execute_code` is
                // `captures_session_state`, forced sequential by MUST-FIX 1 regardless of what else
                // it declares) -- the final OUTCOME (a hard error below) is identical either way; only
                // whether a later call's side effects already ran before that error surfaces differs.
                if (!audit.ok && audit.error_code == "codeact.ask_pending") {
                    if (calls.size() != 1) {
                        // ADR-057 §9: "a multi-call round where one call ask-pends fails closed... a
                        // named residual, not solved here" -- deliberately NOT folding whatever
                        // results (including this one) were already produced, and NOT invoking any
                        // remaining calls in this round. Any side effects already committed by an
                        // earlier call in this same round (if this ask-pending call wasn't first)
                        // are NOT undone -- the same kind of un-reconciled residual ADR-057 §4 already
                        // names for a script's OWN interior side effects on replay (§9's B7 test).
                        emit_run_event(
                            run_event_kind::run_failed,
                            run_event_payload::RunFailed{
                                "run.codeact_ask_in_multi_call_round_unsupported",
                                "a script called agent.ask() inside a round with more than one "
                                "pending tool call -- not supported (ADR-057 §9)"});
                        co_return std::unexpected(error{
                            failure_class::contract,
                            "agent.ask() is not supported in a round with more than one pending tool "
                            "call",
                            "run.codeact_ask_in_multi_call_round_unsupported"});
                    }

                    std::string prompt;
                    if (!result.content.empty()) {
                        if (auto const* e = std::get_if<Error>(&result.content.front().value)) {
                            prompt = e->message;
                        }
                    }
                    std::string code, language;
                    if (json::Value const* code_v = req.arguments.find("code");
                        code_v != nullptr && code_v->is_string()) {
                        code = code_v->as_string();
                    }
                    if (json::Value const* lang_v = req.arguments.find("language");
                        lang_v != nullptr && lang_v->is_string()) {
                        language = lang_v->as_string();
                    }

                    Interaction const& interaction =
                        open_interaction(effect_context_.run_id, interaction_reason::codeact_ask);
                    PendingCodeActAsk record;
                    record.source = std::move(code);
                    record.language = std::move(language);
                    record.tool_call_id = calls[i].call_id;
                    record.prompt = prompt;
                    pending_codeact_asks_[interaction.interaction_id] = std::move(record);

                    // 013 SS2.2 hard ordering obligation -- see the sibling site above.
                    emit_run_event(run_event_kind::codeact_ask_requested,
                                    run_event_payload::CodeActAskRequested{
                                        calls[i].call_id, interaction.interaction_id, prompt});
                    emit_run_event(run_event_kind::input_required,
                                    run_event_payload::InteractionRef{interaction.interaction_id});

                    // Suspended -- exactly the "never fold, never fabricate a response" shape the
                    // approval branch above already uses: no history mutation, a named sentinel error
                    // code the caller checks first.
                    co_return std::unexpected(error{failure_class::contract,
                                                     "round suspended awaiting an agent.ask() answer",
                                                     kSuspendedForCodeActAsk});
                }

                results[i] = std::move(result);
            }

            history_.push_back(tool_results_message(std::move(results)));
            (void)co_await history_provider_.on_turn_end(
                TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                    history_.size() - response_msg_index}},
                effect_context_);
            emit_run_event(run_event_kind::turn_finished,
                            run_event_payload::Turn{effect_context_.turn_index});
        }

        emit_run_event(run_event_kind::run_failed,
                        run_event_payload::RunFailed{"run.max_turns_exceeded",
                                                      "tool-call loop did not converge within max_turns"});
        co_return std::unexpected(error{failure_class::contract,
                                         "tool-call loop did not converge within max_turns",
                                         "run.max_turns_exceeded"});
    }

    [[nodiscard]] static std::optional<ChatClientT> make_default_chat_client() {
        if constexpr (std::is_default_constructible_v<ChatClientT>) {
            return std::optional<ChatClientT>(std::in_place);
        } else {
            return std::optional<ChatClientT>{};
        }
    }

public:
    // The "suspended for approval, not a failure" sentinel error code — start_run()/
    // resolve_interaction()'s caller checks `result.error().code == kSuspendedForApproval` to tell
    // this apart from a genuine failure. NOT the same shape as the Quark original (an Ask that simply
    // never resolves) because there is no such "never resolves" primitive here -- every task<T> this
    // slice returns DOES complete, with either a real answer or this named sentinel. A future slice
    // may want a richer three-way result type instead of overloading `result<AgentResponse>`'s error
    // channel this way -- named as a real design question, not silently assumed to be the final shape.
    static constexpr char const* kSuspendedForApproval = "run.suspended_for_approval";

    // ADR-057 §9's own sentinel, alongside `kSuspendedForApproval` immediately above -- the same
    // "never fold, never fabricate a response" shape, checked by the caller the identical way. Fires
    // from `run_rounds()`'s invoke loop (a script's FIRST `agent.ask()` call this round) AND from
    // `resolve_interaction()`'s `codeact_ask` branch (a chained SECOND/THIRD `agent.ask()` call in
    // the same script, discovered on replay) -- both cases leave the SAME `Interaction` open, just
    // possibly with an updated stored prompt.
    static constexpr char const* kSuspendedForCodeActAsk = "run.suspended_for_codeact_ask";

    // OQ-21's own sentinel, the same "never fold, never fabricate a response" shape as the two
    // above, checked by the caller the identical way. Fires from `run_rounds()`'s own hook-stage
    // block (a round where the hook stage left at least one call `needs_external_dispatch`) -- NOT
    // from `resolve_hook_decision()`'s own cascade, which opens a fresh `interaction_reason::
    // approval` interaction instead and returns `kSuspendedForApproval` (see that function's own
    // comment for why a hook-decision resume is never itself an approval).
    static constexpr char const* kSuspendedForHookDecision = "run.suspended_for_hook_decision";

private:
    std::string                                       session_id_;
    agentengine::Principal                             principal_;
    std::vector<Message>                               history_;
    StateT                                             state_{};
    std::unordered_map<std::string, std::string>       metadata_;
    std::uint64_t                                       run_counter_ = 0;
    std::string                                         last_run_id_;
    std::vector<Interaction>                            open_interactions_;
    std::uint64_t                                       interaction_counter_ = 0;
    // ADR-057 §9 -- guarded the same way `open_interactions_` above is: every access happens inside
    // `start_run()`/`resolve_interaction()`/`run_rounds()`, all of which run only while
    // `session_mutex_` is held by the calling coroutine's own `AsyncMutex::Guard` (I1). Keyed by
    // `interaction_id`, mirroring the `open_interactions_` vector's own identity for a `codeact_ask`
    // reason -- an entry here always corresponds to exactly one entry in `open_interactions_` with
    // the same id and `reason == interaction_reason::codeact_ask`, for as long as that interaction
    // stays open (erased from both together, `resolve_interaction()`'s codeact_ask branch below).
    std::unordered_map<std::string, PendingCodeActAsk> pending_codeact_asks_;
    // OQ-21: same guard/keying discipline as `pending_codeact_asks_` immediately above -- every
    // access happens inside `run_rounds()`/`resolve_interaction()`/`resolve_hook_decision()`, all of
    // which run only while `session_mutex_` is held (I1). An entry here always corresponds to
    // exactly one entry in `open_interactions_` with the same id, for as long as that interaction
    // stays open -- consumed and erased on every real resolution path (completion, the cascade
    // re-key into a fresh `interaction_reason::approval` interaction, and denial), never left to
    // grow unboundedly. NOT durably checkpointed -- the same disclosed limitation
    // `PendingCodeActAsk` already carries for its own map (that struct's own comment), inherited
    // here, not newly introduced.
    std::unordered_map<std::string, agentengine::PendingHookDecisionRound> pending_hook_decisions_;
    std::optional<ChatClientT>                          chat_client_ = make_default_chat_client();
    // ADR-061 §26.1: the session-level capability grant -- single source of truth, a shared_ptr (not
    // a raw pointer kept in sync with a separate alias field, §24.3's design, superseded) so it can be
    // copied directly into EffectContext::capabilities without constructing a second aliasing wrapper
    // at read time. Still non-owning in the sense that matters: set_capabilities()'s (pointer,
    // deleter) construction never deletes the pointee -- ownership of the real CapabilitySet stays
    // with whoever calls set_capabilities(), unchanged from before this type change.
    std::shared_ptr<CapabilitySet const>                capabilities_;
    // ADR-116 follow-on: this object's own permanent, process-wide-unique identity -- see
    // `agent_session_detail::g_next_session_identity`'s own comment for why this exists (a real,
    // empirically-confirmed ABA hole from using this session's raw address as `ComposedContextProvider
    // ::owner_`'s tag instead). Assigned ONCE, at construction, from a monotonic counter that never
    // repeats -- unlike this object's own address, which the heap allocator can and does hand to a
    // LATER, unrelated `AgentSession` once this one is destroyed.
    std::uint64_t const                                 session_identity_ =
        agent_session_detail::g_next_session_identity.fetch_add(1, std::memory_order_relaxed);
    HistoryProviderT                                    history_provider_;
    EffectContext                                       effect_context_;
    // ADR-061 §20.2: session-level, set once at wiring time by whichever Tier-3 listener fronts this
    // session (set_require_authority(), above). Defaults to false -- unchanged behavior for every
    // embedded/non-Tier-3 session.
    bool                                                  require_authority_ = false;
    std::optional<std::uint64_t>                        token_budget_;
    std::uint64_t                                        run_tokens_consumed_ = 0;
    // GitHub issue #35 follow-up (ADR-163): the full-fidelity sibling of `run_tokens_consumed_` above
    // -- that field deliberately collapses `Usage::input_tokens + output_tokens` into one number for
    // cheap budget comparison (`token_budget_`'s own check), which is exactly right for THAT job but
    // throws away the split (and `cached_input_tokens`/`reasoning_tokens`/`cost_estimate`/
    // `cache_write_tokens`) a caller wanting to REPORT real usage onward (rather than merely enforce a
    // ceiling) needs. Reset at the SAME 3 sites `run_tokens_consumed_` already is (this run's own
    // start, `fork_from()`, `clear_in_process_state()`), accumulated at the SAME site
    // (`run_model_call()`'s own round loop) -- never a second, independent tracking path that could
    // drift from what `run_tokens_consumed_` itself already counts.
    agentengine::Usage                                    run_usage_{};
    std::optional<std::uint64_t>                         max_turns_;
    ApprovalDecider                                      approval_decider_{};
    // decisions/ADR-070-host-configurable-responsibility-boundary.md. Unset by default -- see
    // set_policy_decider()'s own comment above for exactly where this is (and is not) consulted.
    PolicyDecider                                         policy_decider_{};
    // OQ-21 (core/tool_call_hook.hpp). Unset by default -- see set_tool_call_hook()'s own comment
    // above for exactly where this is (and is not) consulted.
    agentengine::ToolCallHook                             tool_call_hook_{};
    // decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md. Unset by default.
    agentengine::TurnMiddlewareHook                       turn_middleware_hook_{};
    bool                                                  suspend_for_approval_ = false;
    bool                                                  stream_model_calls_ = false;
    bool                                                  scan_response_format_leaks_ = false;
    // §9 RC-1 (design doc above) -- see set_background_execution_disabled()'s own comment.
    bool                                                  background_execution_disabled_ = false;
    // §4.6 (item 6, OQ-16) -- see set_static_instructions()'s own comment above. Empty by default.
    std::string                                           static_instructions_;
    // ADR-058 §8 (Design B) -- additive opt-in. Empty/unset by default; `output_schema_validate_`
    // holding no target IS the "unset" signal (has_output_schema()), not a separate bool -- the same
    // "the function itself is the presence flag" shape `approval_decider_` already uses one member
    // up (`suspend_for_approval_ && !approval_decider_`).
    std::string                                          output_schema_json_;
    agentengine::output_schema_strategy                  output_schema_strategy_ =
        agentengine::output_schema_strategy::native;
    std::function<result<void>(std::string_view)>        output_schema_validate_;
    std::uint64_t                                         admission_denied_count_ = 0;
    stream_producer<RunEvent>                             run_event_producer_;
    std::unordered_map<std::string, std::uint64_t>        run_event_seq_by_run_;
    // decisions/ADR-160-parallel-tool-batch-scheduler.md §5 MUST-FIX 2: `emit_run_event_for()`
    // mutates `run_event_seq_by_run_` (a plain `std::unordered_map`) and calls `run_event_producer_.
    // push()` (`rt::channel_producer<T,E>`, which documents "Multiple PRODUCERS... unsupported",
    // rt/channel.hpp) -- both unsafe under concurrent callers. A parallel-batch call's own
    // `report_progress`/`agent_turn_sink` may now call `emit_run_event_for()` from a worker thread
    // while a sibling call's does the same from a different one; this plain `std::mutex` (never
    // needs to suspend a coroutine, only exclude a few memory writes) is the single point every
    // caller -- worker thread or the session's own coroutine -- now funnels through. NAMED RESIDUAL
    // (not enforced by the type system): a tap/producer callback that itself reentrantly calls
    // `emit_run_event_for()` on the SAME thread would deadlock against this non-recursive mutex; no
    // such call exists in this tree today.
    std::mutex                                             run_event_mutex_;
    // ADR-152 (issue #29) -- see set_run_event_tap()'s own comment above. Default no-op;
    // run_event_tap_attached_ tracks whether the LAST set_run_event_tap() call passed a real
    // (non-empty) function, independent of what run_event_tap_ itself currently holds (which is
    // never truly empty -- see set_run_event_tap()'s own substitution) -- this is what lets
    // emit_run_event_for() skip constructing an event entirely when NEITHER this tap NOR
    // run_event_producer_ is attached, the same zero-cost-when-unattached guarantee that method
    // already provided for the producer alone.
    std::function<void(RunEvent const&)>                  run_event_tap_ = [](RunEvent const&) {};
    bool                                                   run_event_tap_attached_ = false;
    // Slice 3 -- see file banner's "SLICE 3 ADDITION" paragraph, and
    // docs/planning/agent-session-decomposition-design-draft.md §2a. Owns the standing-effect
    // storage and the background-completion queue (rt/standing_effect_registry.hpp); its own
    // completion-queue shared_ptr is never null, never reassigned after construction -- a
    // background worker's weak_ptr capture is only meaningful if that identity stays stable for
    // the AgentSession instance's whole lifetime, which it does: AgentSession is structurally
    // immovable (rt::AsyncMutex's deleted copy ctor with no declared move ctor suppresses every
    // implicit move member on this class), so this member subobject is pinned for the session's
    // whole life exactly as its absorbed fields were when they lived directly here.
    StandingEffectRegistry                                standing_effects_registry_;
    // I1 -- see file banner. Every public async entry point acquires this for its whole duration.
    // `mutable` (ADR-102 Phase 5): `fork_from()` below locks `source.session_mutex_` through a
    // `AgentSession const&` -- the same, already-established rationale `core/ledger.hpp`'s own
    // `mutable std::mutex mutex_` uses for the identical shape (a real synchronization primitive that
    // must remain lockable from a conceptually-const access path; taking the lock itself does not
    // change anything externally observable about `source`).
    mutable AsyncMutex                                    session_mutex_;
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
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] inline result<TurnDeltaRecord> decode_turn_delta_record(std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
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
