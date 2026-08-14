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
//   - TimerWake (the reminder-service wake acknowledgement) -- depended on a live quark::Engine's
//     ReminderService, a real integration-test-only surface (test_agent_session_timer_wake.cpp) this
//     slice has no standalone replacement design for yet. `standing_effects_`'s OTHER wake row,
//     "Local background task completion" (BackgroundTaskDone), IS migrated -- see Slice 3 below.
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
//   - `start_background_task()`/`cancel_standing_effect()`/`list_standing_effects()` themselves stay
//     PLAIN, UNLOCKED methods (no session_mutex_ acquisition) -- this is not an oversight, it matches
//     the ORIGINAL's own asymmetry exactly: those three were never part of Quark's `Messages` type
//     list for this actor either (unlike `BackgroundTaskDone`, which WAS), so they were already
//     unserialized-by-the-mailbox in the Quark version too, the same category `fork_from()`/`redact()`/
//     `clear_in_process_state()` above already fall into. NAMED, NOT SILENTLY ASSUMED SAFE: a host that
//     calls these three concurrently with a `start_run()`/`resolve_interaction()` in flight on another
//     "thread of control" races `standing_effects_` directly -- a real, pre-existing-in-kind
//     precondition (not a new one introduced here), but easy to wrongly assume is now covered just
//     because a completion queue exists. `cancel_standing_effect()` racing an already-queued-but-not-
//     yet-drained completion is safe by construction: drain looks up by `handle_id`; if cancel already
//     erased the entry, drain's lookup misses and no-ops, exactly the original's own documented
//     idempotent-no-op behavior for "a late BackgroundTaskDone for a canceled handle."
//   - `rt::channel<T>` (already built, Phase 1) was considered and rejected for this queue: its
//     producer side is move-only and auto-closes on destruction, so sharing it across many independent
//     detached-thread closures would need the exact same weak_ptr-to-a-shared-instance dance for zero
//     benefit -- draining here is synchronous/opportunistic (never suspends), and a
//     `{handle_id,call_id,bool}` payload needs no bounded-backpressure story `Background<max_concurrent>`
//     doesn't already provide at the authorize step. A hand-rolled mutex+deque is simpler and has no
//     close/terminal semantics to reason about for a queue that is never itself "done."
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
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/interaction_codec.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::rt {

// Reused, unchanged shape (matching agentengine::NoSessionState). A distinct type from the
// core/agent_session.hpp one, deliberately -- Slice 1 does not depend on that header at all, keeping
// this file's own Quark-free claim easy to verify by inspection (no transitive include of anything
// that pulls quark/*).
struct NoSessionState {};

// Same narrowed, wire-shape-motivated-but-still-correct admission identity as
// agentengine::SessionCaller -- see file banner for why this slice keeps the shape even though the
// byte-budget that originally forced it no longer applies.
struct SessionCaller {
    std::string id;
    std::string tenant_id;
};

struct StartRun {
    Message input;
    std::optional<SessionCaller> caller = std::nullopt;
};

struct ResolveInteraction {
    std::string interaction_id;
    bool        approved = false;
    std::optional<SessionCaller> caller = std::nullopt;
};

struct AgentResponse {
    Message message;
    Usage   usage;
};

// Slice 3 -- what a completed background native tool call hands back. See file banner's "SLICE 3
// ADDITION" paragraph for the full delivery-path design. Deliberately NOT the original's Quark
// message type (no Ask/tell shape here -- this is plain data pushed into a BackgroundCompletionQueue,
// never routed through anything actor-shaped).
struct BackgroundTaskDone {
    std::string handle_id;
    std::string call_id;
    bool        ok = false;
};

// The thread-safe handoff point between a detached worker thread (tool_pipeline.hpp's
// background_task() step 8) and whichever coroutine later drains it under session_mutex_. Its OWN
// mutex, never session_mutex_ -- a plain std::thread cannot co_await anything, so the one lock it
// touches must be acquirable synchronously. Held behind a shared_ptr on AgentSession specifically so
// a worker's completion closure can capture a weak_ptr instead of a reference into the (possibly by
// then destroyed) AgentSession itself -- see file banner for the full lifetime rationale.
struct BackgroundCompletionQueue {
    std::mutex                     m;
    std::deque<BackgroundTaskDone> pending;
};

// Slice 2's narrowed durable record -- see file banner for exactly what is and isn't carried
// (notably: no created_at_ns/updated_at_ns, a deliberate narrowing vs. the Quark original's own
// AgentSessionRecord; history/state/metadata are likewise not carried, same "no Message/ContentItem
// serialization yet" gap the original named). `to_record()`/`restore_from_record()` (AgentSession
// member functions, below) are the only two places that cross between the in-process type and this
// shape, so the field list can't drift between them silently.
struct AgentSessionRecord {
    std::string session_id;
    std::string principal_id;
    std::string principal_tenant_id;
    bool deleted = false;
    std::uint64_t run_counter = 0;
    std::uint64_t turn_index = 0;
    std::vector<Interaction> open_interactions;
};

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
    });
}

[[nodiscard]] inline result<AgentSessionRecord> agent_session_record_from_json(json::Value const& v) {
    json::Value const* session_id           = v.find("session_id");
    json::Value const* principal_id         = v.find("principal_id");
    json::Value const* principal_tenant_id  = v.find("principal_tenant_id");
    json::Value const* deleted              = v.find("deleted");
    json::Value const* run_counter          = v.find("run_counter");
    json::Value const* turn_index           = v.find("turn_index");
    json::Value const* open_interactions    = v.find("open_interactions");
    if (session_id == nullptr || !session_id->is_string() || principal_id == nullptr ||
        !principal_id->is_string() || principal_tenant_id == nullptr ||
        !principal_tenant_id->is_string() || deleted == nullptr || !deleted->is_bool() ||
        run_counter == nullptr || !run_counter->is_number() || turn_index == nullptr ||
        !turn_index->is_number() || open_interactions == nullptr || !open_interactions->is_array()) {
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

    void set_capabilities(agentengine::CapabilitySet const* capabilities) noexcept {
        capabilities_ = capabilities;
    }
    [[nodiscard]] agentengine::CapabilitySet const* capabilities() const noexcept { return capabilities_; }

    void set_approval_decider(agentengine::ApprovalDecider approve) { approval_decider_ = std::move(approve); }
    [[nodiscard]] agentengine::ApprovalDecider const& approval_decider() const noexcept {
        return approval_decider_;
    }

    [[nodiscard]] HistoryProviderT& history_provider() noexcept { return history_provider_; }

    void set_suspend_for_approval(bool suspend) noexcept { suspend_for_approval_ = suspend; }
    [[nodiscard]] bool suspend_for_approval() const noexcept { return suspend_for_approval_; }

    void set_stream_model_calls(bool stream) noexcept { stream_model_calls_ = stream; }
    [[nodiscard]] bool stream_model_calls() const noexcept { return stream_model_calls_; }

    void set_scan_response_format_leaks(bool scan) noexcept { scan_response_format_leaks_ = scan; }
    [[nodiscard]] bool scan_response_format_leaks() const noexcept { return scan_response_format_leaks_; }

    void set_max_turns(std::optional<std::uint64_t> max_turns) noexcept { max_turns_ = max_turns; }
    [[nodiscard]] std::optional<std::uint64_t> max_turns() const noexcept { return max_turns_; }

    [[nodiscard]] stream<RunEvent> enable_event_stream(std::pmr::memory_resource* mr,
                                                        stream_config<RunEvent> cfg = {}) {
        auto pair            = make_stream<RunEvent>(mr, cfg);
        run_event_producer_ = std::move(pair.producer);
        return std::move(pair.consumer);
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

    [[nodiscard]] std::vector<Interaction> const& open_interactions() const noexcept {
        return open_interactions_;
    }
    [[nodiscard]] bool has_open_interactions() const noexcept { return !open_interactions_.empty(); }

    // ---- The two real entry points -----------------------------------------------------------

    // Replaces `handle(quark::Ask<StartRun, AgentResponse> const&)`. Returns the response directly
    // (as `result<AgentResponse>`) instead of calling `m.respond(...)` -- there is no Quark Ask/reply-
    // cell mechanism anymore; the caller `co_await`s this task and gets the answer back the ordinary
    // way. A suspended-for-approval round or an admission denial or ANY fail-closed branch returns an
    // error result rather than a fabricated response -- see each branch's own comment for which error
    // code, matching the original's "never fabricate a response" rule exactly, just expressed as a
    // return value instead of a never-answered Ask.
    task<result<AgentResponse>> start_run(StartRun request) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner
        drain_background_completions_locked();  // Slice 3 -- see file banner

        if (request.caller.has_value() &&
            !agentengine::principal_admitted_for(
                agentengine::Principal{request.caller->id, request.caller->tenant_id}, principal_)) {
            ++admission_denied_count_;
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy, "caller not admitted for this session",
                "run.admission_denied"});
        }

        bool const has_open_approval =
            std::any_of(open_interactions_.begin(), open_interactions_.end(), [](Interaction const& i) {
                return i.reason == interaction_reason::approval;
            });
        if (has_open_approval) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "a round is already suspended awaiting approval -- resolve it before starting a new run",
                "run.approval_pending"});
        }

        run_counter_ += 1;
        run_tokens_consumed_ = 0;
        effect_context_.principal    = principal_;
        effect_context_.capabilities = capabilities_;
        effect_context_.run_id       = session_id_ + ":run:" + std::to_string(run_counter_);
        effect_context_.turn_index   = 0;
        last_run_id_ = effect_context_.run_id;

        emit_run_event(run_event_kind::run_started);
        if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
            emit_run_event(run_event_kind::warning,
                            run_event_payload::Warning{
                                "this run routes model calls through a ModelCallGateway (ADR-036): no "
                                "live model_delta events fire for a gateway-routed round, and a single "
                                "round may make several real backend calls (retries/fallback tiers) "
                                "before the per-run token budget is ever checked"});
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
    task<result<AgentResponse>> resolve_interaction(ResolveInteraction request) {
        AsyncMutex::Guard guard = co_await session_mutex_.lock();  // I1 -- see file banner
        drain_background_completions_locked();  // Slice 3 -- see file banner

        if (request.caller.has_value() &&
            !agentengine::principal_admitted_for(
                agentengine::Principal{request.caller->id, request.caller->tenant_id}, principal_)) {
            ++admission_denied_count_;
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy, "caller not admitted for this session",
                "run.admission_denied"});
        }

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

        result<void> const resolved = resolve_interaction_record(it->interaction_id);
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        emit_run_event(run_event_kind::input_resolved,
                        run_event_payload::InteractionRef{request.interaction_id});

        std::size_t const response_msg_index = history_.size() - 1;

        if (!request.approved) {
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
            co_await history_provider_.on_turn_end(
                TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                    history_.size() - response_msg_index}},
                effect_context_);
            emit_run_event(run_event_kind::turn_finished,
                            run_event_payload::Turn{effect_context_.turn_index});
            ++effect_context_.turn_index;
            co_return co_await run_rounds();
        }

        SessionContext session_ctx{session_id_, principal_, history_};
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
        CapabilitySet const& held      = capabilities_ ? *capabilities_ : empty_caps;
        ApprovalDecider const one_shot_approve = [](std::string_view, std::string const&) { return true; };

        std::vector<ToolResult> results;
        results.reserve(pending_calls.size());
        for (std::size_t i = 0; i < pending_calls.size(); ++i) {
            ToolCallRequest const req = tool_call_request_of(pending_calls[i], i);
            emit_run_event(run_event_kind::tool_call_started,
                            run_event_payload::ToolCallStarted{pending_calls[i].call_id,
                                                                pending_calls[i].tool_name});
            ToolInvocationAudit audit;
            ToolResult result =
                invoke_tool(tool_table, held, req, effect_context_, one_shot_approve, &audit);
            emit_run_event(run_event_kind::tool_call_finished,
                            run_event_payload::ToolCallFinished{audit.call_id, audit.ok});
            emit_run_event(run_event_kind::approval_resolved,
                            run_event_payload::ApprovalResolved{pending_calls[i].call_id, true,
                                                                  request.interaction_id});
            results.push_back(std::move(result));
        }
        history_.push_back(tool_results_message(std::move(results)));
        co_await history_provider_.on_turn_end(
            TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                history_.size() - response_msg_index}},
            effect_context_);
        emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
        ++effect_context_.turn_index;
        co_return co_await run_rounds();
    }

    // ---- Pure bookkeeping, unchanged in behavior from core/agent_session.hpp -----------------
    // (no Quark dependency in the original either -- ported verbatim, not redesigned)

    void fork_from(AgentSession const& source, std::string new_session_id,
                    std::optional<std::size_t> history_prefix_len = std::nullopt) {
        session_id_ = std::move(new_session_id);
        principal_  = source.principal_;
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
        admission_denied_count_ = 0;
        // Same "no run identity of its own" rationale open_interactions_ above already documents --
        // a fresh fork inherits none of the source's (or *this*'s own prior) outstanding background
        // work. Same fix category as clear_in_process_state()'s own comment below.
        standing_effects_.clear();
        standing_effect_counter_ = 0;
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
        token_budget_ = std::nullopt;
        run_tokens_consumed_ = 0;
        admission_denied_count_ = 0;
        max_turns_ = std::nullopt;
        history_provider_ = HistoryProviderT{};
        // A real gap found in the Quark original (core/agent_session.hpp's own clear_in_process_
        // state() never resets standing_effects_/standing_effect_counter_): this function's own
        // contract is "no residue left to read back through ANY of this class's own accessors" (005
        // §6), which list_standing_effects() would otherwise silently violate after a delete. Fixed
        // here rather than ported forward unchanged -- background_completions_ is deliberately NOT
        // reset (a shared_ptr whose identity a worker thread may already hold a weak_ptr to; dropping
        // and reallocating it would not by itself invalidate anything, but a queue full of stale
        // entries for effects that no longer exist is intentionally harmless -- the drain loop's own
        // find_if() already no-ops on an unknown handle_id, same as a canceled one).
        standing_effects_.clear();
        standing_effect_counter_ = 0;
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

    // PLAIN, UNLOCKED -- matches the original's own asymmetry exactly; see file banner. Runs
    // tool_pipeline.hpp's background_task() (steps 1-7 synchronous, step 8 onward a detached
    // std::thread) unchanged, wiring on_complete to push into background_completions_ instead of
    // the original's self.tell(BackgroundTaskDone{...}).
    [[nodiscard]] result<agentengine::StandingEffect> start_background_task(
        ToolTable const& table, ToolCallRequest const& request,
        ApprovalDecider const& approve = ApprovalDecider{}) {
        if (!capabilities_) {
            return std::unexpected(error{failure_class::policy, "session has no granted capabilities",
                                          "standing_effect.no_capabilities"});
        }
        std::size_t const current_count = static_cast<std::size_t>(std::count_if(
            standing_effects_.begin(), standing_effects_.end(),
            [](agentengine::StandingEffect const& e) {
                return e.kind == agentengine::standing_effect_kind::background_task;
            }));

        std::string const handle_id =
            session_id_ + ":standing:" + std::to_string(++standing_effect_counter_);
        std::string const owner_run_id       = effect_context_.run_id;
        std::string const owner_principal_id = effect_context_.principal.id;

        std::weak_ptr<BackgroundCompletionQueue> weak_queue = background_completions_;
        result<void> submitted = background_task(
            table, *capabilities_, request, effect_context_, approve, current_count,
            [weak_queue, handle_id, call_id = request.call_id](ToolResult /*result_out*/,
                                                                 ToolInvocationAudit audit) mutable {
                if (std::shared_ptr<BackgroundCompletionQueue> q = weak_queue.lock()) {
                    std::lock_guard<std::mutex> lock(q->m);
                    q->pending.push_back(
                        BackgroundTaskDone{std::move(handle_id), std::move(call_id), audit.ok});
                }  // else: session (and its queue) already gone -- drop, no UAF, no residue to clean up
            });
        if (!submitted) return std::unexpected(submitted.error());

        agentengine::StandingEffect effect;
        effect.handle_id    = handle_id;
        effect.session_id   = session_id_;
        effect.principal_id = owner_principal_id;
        effect.run_id       = owner_run_id;
        effect.kind         = agentengine::standing_effect_kind::background_task;
        effect.label        = request.tool_name;
        standing_effects_.push_back(effect);

        emit_run_event_for(owner_run_id, run_event_kind::tool_call_started,
                            run_event_payload::ToolCallStarted{request.call_id, request.tool_name});
        return effect;
    }

    [[nodiscard]] std::vector<agentengine::StandingEffect> const& list_standing_effects() const noexcept {
        return standing_effects_;
    }

    // Cancels the BOOKKEEPING only -- background_task() names no mechanism to interrupt an
    // already-running native invoke(); a late completion for a canceled handle simply finds nothing
    // to resolve in drain_background_completions_locked() (its own idempotent no-op), same as the
    // original.
    [[nodiscard]] result<void> cancel_standing_effect(std::string const& handle_id,
                                                       agentengine::Principal const& caller_principal) {
        auto it = std::find_if(standing_effects_.begin(), standing_effects_.end(),
                                [&](agentengine::StandingEffect const& e) { return e.handle_id == handle_id; });
        if (it == standing_effects_.end()) {
            return std::unexpected(
                error{failure_class::contract, "no such standing effect", "standing_effect.not_found"});
        }
        if (it->principal_id != caller_principal.id) {
            return std::unexpected(error{failure_class::policy,
                                          "cannot cancel a standing effect owned by a different principal",
                                          "standing_effect.cross_principal_denied"});
        }
        standing_effects_.erase(it);
        return {};
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
    void drain_background_completions_locked() {
        std::vector<BackgroundTaskDone> ready;
        {
            std::lock_guard<std::mutex> lock(background_completions_->m);
            ready.assign(std::make_move_iterator(background_completions_->pending.begin()),
                         std::make_move_iterator(background_completions_->pending.end()));
            background_completions_->pending.clear();
        }
        for (BackgroundTaskDone const& m : ready) {
            auto it = std::find_if(standing_effects_.begin(), standing_effects_.end(),
                                    [&](agentengine::StandingEffect const& e) {
                                        return e.handle_id == m.handle_id;
                                    });
            if (it == standing_effects_.end()) continue;  // canceled or already resolved -- no-op
            std::string const owner_run_id = it->run_id;
            standing_effects_.erase(it);
            emit_run_event_for(owner_run_id, run_event_kind::tool_call_finished,
                                run_event_payload::ToolCallFinished{m.call_id, m.ok});
        }
    }

    void emit_run_event(run_event_kind kind, RunEventPayload payload = run_event_payload::Empty{}) {
        emit_run_event_for(effect_context_.run_id, kind, std::move(payload));
    }
    void emit_run_event_for(std::string const& run_id, run_event_kind kind,
                             RunEventPayload payload = run_event_payload::Empty{}) {
        if (!run_event_producer_.valid()) return;
        RunEvent ev;
        ev.run_id  = run_id;
        ev.seq     = ++run_event_seq_by_run_[run_id];
        ev.kind    = kind;
        ev.payload = std::move(payload);
        (void)run_event_producer_.push(std::move(ev));
    }

    // Gap-audit finding 20 / 003 §8 Q2. Drops every `Reasoning` content item whose
    // `producer_chat_client_id` does not exactly match `current_chat_client_id` -- including an
    // EMPTY stamp (a record written before this field existed, or a `text_derived` leak-scan
    // extraction, core/response_format_codec.hpp, whose provenance is already untrustworthy by
    // construction): Q2's own rule is an allowlist ("included only when it originated from..."),
    // not a denylist, so unknown provenance is excluded, never assumed safe. A message that becomes
    // empty SOLELY because of this filter is dropped entirely (never sent as an empty-content
    // message); a message that was already empty for an unrelated reason is left alone. The excluded
    // item is not deleted from anything durable -- `contribution` is this turn's own transient
    // ContextContribution, not `history_`, so the original item stays intact there for audit/replay
    // (Q2's own "excluded... not deleted" wording), and each exclusion fires a real
    // `run_event_kind::policy_decision` (013 §1's own vocabulary; this is its first real producer).
    void filter_cross_provider_reasoning(ContextContribution& contribution,
                                          std::string const& current_chat_client_id) {
        std::vector<Message> filtered;
        filtered.reserve(contribution.messages.size());
        for (Message& m : contribution.messages) {
            bool const originally_empty = m.content.empty();
            std::vector<ContentItem> kept;
            kept.reserve(m.content.size());
            for (ContentItem& item : m.content) {
                auto const* r = std::get_if<Reasoning>(&item.value);
                if (r != nullptr && r->producer_chat_client_id != current_chat_client_id) {
                    emit_run_event(
                        run_event_kind::policy_decision,
                        run_event_payload::PolicyDecision{
                            "excluded a Reasoning content item from message '" + m.message_id +
                            "' -- produced by '" +
                            (r->producer_chat_client_id.empty() ? "(unknown)" : r->producer_chat_client_id) +
                            "', currently bound backend is '" + current_chat_client_id +
                            "' (003 §8 Q2: reasoning is vendor-specific, never translated across "
                            "providers)"});
                    continue;
                }
                kept.push_back(std::move(item));
            }
            m.content = std::move(kept);
            if (!m.content.empty() || originally_empty) filtered.push_back(std::move(m));
        }
        contribution.messages = std::move(filtered);
    }

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

        if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
            response = co_await chat_client_->call(request, ctx);
        } else {
            if constexpr (requires(ChatClientT& c, ChatRequest const& r, EffectContext& e) {
                              { c.chat(r, e) } -> std::same_as<agentengine::task<result<ChatResponse>>>;
                          }) {
                if (!stream_model_calls_) {
                    response = co_await chat_client_->chat(request, ctx);
                    if (response.has_value() && scan_response_format_leaks_) {
                        response->message = apply_response_format_scan(std::move(response->message), request.tools);
                    }
                    co_return response;
                }
            }
            stream<ChatResponseUpdate> s = chat_client_->chat_stream(request, ctx);
            Message accumulated;
            accumulated.role = role::assistant;
            std::optional<Usage> usage;
            while (!s.done()) {
                while (std::optional<ChatResponseUpdate> upd = s.next()) {
                    if (stream_model_calls_) {
                        if (auto const* t = std::get_if<Text>(&upd->delta.value);
                            t != nullptr && !t->text.empty()) {
                            emit_run_event(run_event_kind::model_delta, run_event_payload::ModelDelta{t->text});
                        }
                    }
                    accumulated.content.push_back(upd->delta);
                    if (upd->is_final && upd->usage.has_value()) usage = upd->usage;
                }
                if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (s.terminal() != stream_terminal::closed) {
                response = std::unexpected(error{failure_class::transient,
                                                  "chat_stream() did not reach a clean terminal",
                                                  "run.stream_incomplete"});
            } else if (!usage.has_value()) {
                response = std::unexpected(
                    error{failure_class::contract,
                          "streaming chat call completed with no reported token usage — refusing "
                          "to treat it as zero-cost against the per-run token budget (004 §5)",
                          "run.usage_unavailable"});
            } else {
                response = ChatResponse{std::move(accumulated), *usage};
            }
        }

        if (response.has_value() && scan_response_format_leaks_) {
            response->message = apply_response_format_scan(std::move(response->message), request.tools);
        }
        co_return response;
    }

    // Same shape as core/agent_session.hpp's own run_rounds() -- ported to rt::task<T>, no longer
    // templated on AskT (there is only one caller shape now, a plain `result<AgentResponse>` return),
    // otherwise byte-for-byte identical logic.
    task<result<AgentResponse>> run_rounds() {
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        CapabilitySet const& held      = capabilities_ ? *capabilities_ : empty_caps;

        for (; !max_turns_.has_value() || effect_context_.turn_index < *max_turns_;
             ++effect_context_.turn_index) {
            emit_run_event(run_event_kind::turn_started, run_event_payload::Turn{effect_context_.turn_index});

            SessionContext session_ctx{session_id_, principal_, history_};
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
                    filter_cross_provider_reasoning(*contribution, chat_client_->producer_chat_client_id());
                }
            }

            ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);
            // Gap-16 fix (2026-08-14): `contribution->instructions` used to be read this far and then
            // never referenced again -- silently dropped, never reaching the model. The ONE explicit
            // declassification site for the whole engine: `.unsafe_view()` here does not itself decide
            // anything is safe -- that decision was already made, explicitly, by whichever
            // `ContextProvider` constructed the `TaintedText` (context_provider.hpp's own comment).
            // This just materializes an already-vetted value onto the wire, prepended so it establishes
            // context ahead of everything else, matching `HistoryAndSkillsProvider`'s own
            // system-message-first convention (tools/cli_chat.cpp) -- a second, independent role::system
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
            ChatRequest request{contribution->messages, contribution->tools};
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
                co_await history_provider_.on_turn_end(
                    TurnView{std::span<Message const>{history_.data() + response_msg_index, 1}},
                    effect_context_);
                emit_run_event(run_event_kind::turn_finished,
                                run_event_payload::Turn{effect_context_.turn_index});
                emit_run_event(run_event_kind::run_finished);
                co_return AgentResponse{response->message, response->usage};
            }

            if (suspend_for_approval_ && !approval_decider_) {
                bool any_needs_approval = false;
                for (ToolCall const& call : calls) {
                    ToolDescriptor const* td = tool_table.find(call.tool_name);
                    if (td != nullptr && tool_call_requires_approval(*td, call.provenance)) {
                        any_needs_approval = true;
                        break;
                    }
                }
                if (any_needs_approval) {
                    Interaction const& interaction =
                        open_interaction(effect_context_.run_id, interaction_reason::approval);
                    emit_run_event(run_event_kind::input_required,
                                    run_event_payload::InteractionRef{interaction.interaction_id});
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
            }

            std::vector<ToolResult> results;
            results.reserve(calls.size());
            for (std::size_t i = 0; i < calls.size(); ++i) {
                ToolCallRequest const req = tool_call_request_of(calls[i], i);
                emit_run_event(run_event_kind::tool_call_started,
                                run_event_payload::ToolCallStarted{calls[i].call_id, calls[i].tool_name});
                ToolInvocationAudit audit;
                ToolResult result = invoke_tool(tool_table, held, req, effect_context_, approval_decider_,
                                                 &audit);
                emit_run_event(run_event_kind::tool_call_finished,
                                run_event_payload::ToolCallFinished{audit.call_id, audit.ok});
                results.push_back(std::move(result));
            }

            history_.push_back(tool_results_message(std::move(results)));
            co_await history_provider_.on_turn_end(
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
    std::optional<ChatClientT>                          chat_client_ = make_default_chat_client();
    CapabilitySet const*                                capabilities_ = nullptr;
    HistoryProviderT                                    history_provider_;
    EffectContext                                       effect_context_;
    std::optional<std::uint64_t>                        token_budget_;
    std::uint64_t                                        run_tokens_consumed_ = 0;
    std::optional<std::uint64_t>                         max_turns_;
    ApprovalDecider                                      approval_decider_{};
    bool                                                  suspend_for_approval_ = false;
    bool                                                  stream_model_calls_ = false;
    bool                                                  scan_response_format_leaks_ = false;
    std::uint64_t                                         admission_denied_count_ = 0;
    stream_producer<RunEvent>                             run_event_producer_;
    std::unordered_map<std::string, std::uint64_t>        run_event_seq_by_run_;
    // Slice 3 -- see file banner's "SLICE 3 ADDITION" paragraph. Never null, never reassigned after
    // construction -- a background worker's weak_ptr capture is only meaningful if this shared_ptr's
    // identity stays stable for the AgentSession instance's whole lifetime.
    std::shared_ptr<BackgroundCompletionQueue>            background_completions_ =
        std::make_shared<BackgroundCompletionQueue>();
    std::vector<agentengine::StandingEffect>              standing_effects_;
    std::uint64_t                                         standing_effect_counter_ = 0;
    // I1 -- see file banner. Every public async entry point acquires this for its whole duration.
    AsyncMutex                                            session_mutex_;
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
enum class ack_policy : std::uint8_t { at_most_once, require_durable };

// 005 §2's "history delta" specifically -- the messages ONE turn added, not the whole conversation.
// AgentSessionRecord's own comment already names full-history serialization as a separate, larger,
// not-yet-built gap; this is deliberately narrower and, unlike that, tractable today: reuses
// rt/message_codec.hpp's already-proven Message<->JSON codec (built for WorkflowSupervisor's own
// checkpoint record, ADR-037 Phase 3 Slice 2) rather than inventing a second one.
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

    AgentSessionRecord tombstone{};
    tombstone.session_id = receipt.session_id;
    tombstone.deleted    = true;
    result<void> saved = store.save(receipt.session_id, encode_agent_session_record(tombstone));
    if (!saved) co_return std::unexpected(saved.error());
    receipt.durable_record_removed = true;

    co_await session.clear_in_process_state_locked();
    receipt.in_process_state_cleared = true;

    co_return receipt;
}

}  // namespace agentengine::rt
