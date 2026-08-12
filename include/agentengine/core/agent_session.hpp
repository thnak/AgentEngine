#pragma once
// Implements 005-Sessions-State-and-Memory.md §1 and 001-Execution-Model.md §1/§3. Terminology
// (027 §7): the type is `AgentSession`, not the bare `Session` an earlier draft used — "session"
// remains the ordinary word everywhere in prose.
//
// 001 §1: "One Quark actor instance, key = session_id" — `AgentSession` IS the actor, not a plain
// data struct with a separate actor wrapper; 027 gives no second name for a wrapper type. It is a
// template over its `ChatClient` backend and its declared scratch-state type
// (`AgentSession<ChatClientT, StateT = NoSessionState>`, 005 §8 Q1), following this project's
// CRTP-policy idiom (CONVENTIONS' `Sandbox<Strict>`/`MaxTurns<12>` examples) rather than
// type-erasing the seam this early — 004's real `ChatClient` seam (and any type-erasure decision
// for it) isn't due until Milestone 5.
//
// `handle()` implements 001 §3's real turn loop: assemble context, call the provider, resolve any
// tool calls the response carries via the real ten-step pipeline (006 §3, core/tool_pipeline.hpp),
// feed results back, and repeat — bounded by `max_turns_` — until a round converges with no tool
// call. See ADR-027-agent-session-tool-call-loop.md for the design/red-team/prove record; scoped
// there to `EffectContext&`-only tools and synchronous-decider-only approval (session-scoped
// stateful tools and suspend-for-human approval are separate, later design passes). Policy
// resolution, checkpointing (019) of mid-loop state, and real timestamps (which would be an
// unrecorded wall-clock read, 001 §7 — premature before Clock is a wired capability) are still
// deliberately not touched here.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/describe.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/snapshot.hpp"

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
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// 005 §8 Q1's resolution: `state` is a declared C++ type per agent/workflow, not an untyped bag —
// the same "declared, schema-typed" discipline 002 already applies to tools and output schemas
// (catches an author's own shape-drift at compile time, not at a runtime read). `NoSessionState`
// is the default for an agent that declares no scratch state — an empty tag, not `void`, so
// `AgentSession` can hold a `StateT state_` member uniformly with no special-casing at the storage
// layer for "no state" vs. "some state." Not separately versioned (005 §8 Q1): 024 §2's generic
// persistence-migration contract already covers every persisted format, `state` included.
struct NoSessionState {};  // ae-naming-lint: allow NoSessionState — 005 §8 Q1 names the concept normatively; 027 has not been updated to list a default-state type

// A stable, hand-picked ActorId tag for AgentSession — the same "arbitrary fixed constant, not a
// serialization fingerprint" precedent Quark's own `voice_channel.hpp` uses for `kRoomTypeKey`
// ("AGENTSN1", ASCII). Deliberately NOT `quark::durable_type_key<T>()` (016's `Described`
// fingerprint mechanism): that key names a wire/durable record SHAPE, and Milestone 4 Phase A4
// (session persistence, 005 §2) hasn't decided what AgentSession's own durable record type looks
// like yet — this tag is for in-process actor ADDRESSING only (session_id -> ActorId, needed for
// per-session isolation, 001 §1's "one Quark actor instance, key = session_id"), a narrower and
// earlier-needed property than durability. A4 may reuse this tag for its own record header or
// mint a different one once that record type exists; that is A4's decision, not this one's.
inline constexpr quark::TypeKey kAgentSessionTypeKey{0x4147'454E'5453'4E31ULL};  // "AGENTSN1"

// The stable ActorId a session's human-readable `session_id` maps to — mirrors
// `core/worktree.hpp`'s `ref_actor_id()` exactly (same bridge: Quark's `ActorId` key is a
// `std::uint64_t`, not a string). Same session_id -> same ActorId, always; different session_ids
// collide only as likely as an ordinary 64-bit hash collision (untested at scale here — that is
// 001 §9 G1's job, deferred to this milestone's own Phase H, not this task's).
[[nodiscard]] inline quark::ActorId session_actor_id(std::string_view session_id) noexcept {
    return quark::ActorId{kAgentSessionTypeKey, std::hash<std::string_view>{}(session_id)};
}

// 001 §1: "An Ask<StartRun, RunResponse> to the session actor." `StartRun` is the literal message
// name 001 gives; the reply type is `AgentResponse` (027 §2's canonical name for "a run's result"),
// not a separate `RunResponse` type — 001's "RunResponse" is prose describing the concept, and 027
// is normative for the actual identifier (027 §1: "Adopt MAF's name wherever the concept is the
// same"). One concept, one name.
// Milestone 5 Phase H2 (018 §2). A deliberately minimal, wire-sized identity for the admission
// check at the `StartRun` boundary — NOT the general `Principal` (trust/principal.hpp), which
// carries `kind`/`on_behalf_of`/`delegation_depth` and would not fit: `StartRun` crosses Quark's
// fixed-capacity message pool (`quark::detail::MessagePool::kMaxPayload`, 192 bytes — a Quark-side
// constant this project's "never fork/patch Quark in-tree" rule, CLAUDE.md, does not get to grow),
// and `quark::Ask<StartRun, AgentResponse>` (used by every `TestKit::ask`/real-Engine call) was
// measured at 208 bytes with a full `std::optional<Principal>` embedded — already over budget
// before `Responder<R>`'s own overhead. `id`/`tenant_id` are exactly what ownership needs (018 §2:
// "may this principal start this run on this session") — `on_behalf_of`-aware delegation admission
// is deliberately NOT expressible here: a `SessionCaller` cannot assert it is acting on another
// principal's behalf, so a session's own principal can only be started by an EXACT identity match,
// never by a caller claiming derivation from it. This is a strictly more conservative rule than
// the general `principal_admitted_for` predicate it's checked against (that predicate degrades to
// exact-match automatically here, since `on_behalf_of` is always empty on this narrower type) — the
// richer, delegation-aware version stays real and tested standalone (`test_principal_delegation.cpp`)
// for surfaces that aren't message-pool constrained the same way (e.g. Phase I's memory/sandbox
// checks, which compare `Principal`s in-process, never across this wire).
struct SessionCaller {  // ae-naming-lint: allow SessionCaller — new Phase H2 vocabulary; 027 has not been updated to list it
    std::string id;
    std::string tenant_id;
};

struct StartRun {  // ae-naming-lint: allow StartRun — 001 §1 names this message type normatively; 027 has not been updated to list it (same tracked-gap category as the M0 backlog)
    Message input;  // the new turn to append to history and process (001 §3 step 1)

    // Milestone 5 Phase H2 (018 §2: "Admission — may this principal start this run on this
    // session/agent at all? ... checked at the actor boundary, not by a later filter"). The
    // REQUESTER's identity, established at 018 §1 before any agent code runs — distinct from the
    // session's own OWNING principal (`AgentSession::principal()`, set once at `initialize()`),
    // which is what `caller` is checked against. `std::nullopt` (the default) means "no caller
    // asserted" and skips the check, matching this project's own established "additive, old call
    // sites keep compiling with their old behavior unchanged" precedent (`token_budget`,
    // `ChatClientRegistry const*`, both agent_registry.hpp) — the ~44 existing `StartRun{message}`
    // call sites across `tests/` predate H1's principal-establishment mechanism and are not
    // retroactively made to supply one. Named explicitly, not silently assumed to be a security
    // regression: any NEW caller that wants the real admission check need only supply `caller`,
    // proven for real in `test_agent_session_admission.cpp`.
    std::optional<SessionCaller> caller = std::nullopt;
};

// ADR-029: resumes a run `run_rounds()` suspended in its suspend-for-approval branch. A SEPARATE
// message from `StartRun`, not an added field on it -- red-team finding #1 measured
// `sizeof(quark::Ask<StartRun, AgentResponse>)` growing from 160 to 200 bytes with an added
// `std::optional<std::string> resolving_interaction_id`, over Quark's hard 192-byte
// `MessagePool::kMaxPayload` cap (CLAUDE.md: never fork/patch Quark in-tree to raise it). This
// narrower, separate type was measured for real (a compiled probe, not estimated) at
// `sizeof(quark::Ask<ResolveInteraction, AgentResponse>) == 136` bytes -- safely under the cap.
struct ResolveInteraction {  // ae-naming-lint: allow ResolveInteraction — new ADR-029 vocabulary; 027 has not been updated to list it
    std::string interaction_id;
    bool        approved = false;
    // Same admission shape and same narrowing rationale as `StartRun::caller` immediately above --
    // reused, not reinvented, for exactly the reason that comment gives.
    std::optional<SessionCaller> caller = std::nullopt;
};

// Milestone 4 Phase E3 (019 §2's "Timer/schedule" suspension wake row: "Quark durable reminders
// (027) — at-least-once, wall-clock, mass-due-safe"). Decision 4: pure reuse, not new design — a
// fired `quark::ReminderService` invokes a caller-supplied callback (`FireEvent`), and wiring that
// into an actual `tell()` on the target actor's lane is the documented "engine-integration seam"
// (reminder_service.hpp's own top comment) every caller, not just AgentEngine, has to supply for
// itself. `TimerWake` is that one small, real addition: a tell-only message a fired reminder
// delivers, proving a Quark reminder CAN target a session's real `session_actor_id()` (A1, reused
// unmodified) end to end through a live `quark::Engine`. Deliberately NOT wired to any business
// logic beyond acknowledging the wake (`timer_wakes_`) — what a real "resume the paused run this
// timer was arming" would DO needs 006 §6b's `schedule_wakeup`/`Backgroundable`, confirmed absent
// from this codebase (the M4 kickoff's own inventory table), which is a different, un-built
// vertical this task does not invent standing in for.
struct TimerWake {  // ae-naming-lint: allow TimerWake — 019 §2 names the wake condition normatively; 027 has not been updated to list a message type for it
    std::string reminder_name;
};

// Milestone 7 Phase B (019 §2's "Local background task completion" wake row -- the gap `TimerWake`'s
// own comment above named as un-built). Same shape as `TimerWake`: a tell-only message, delivered by
// a `background_task()` completion closure (`start_background_task()` below arms it, mirroring how
// `test_agent_session_timer_wake.cpp`'s own reminder callback arms `TimerWake` -- the host holds an
// `ActorRef<AgentSession...>` to itself and `tell()`s back, this actor never self-addresses). Carries
// only what `handle()` below needs to resolve the `StandingEffect` and emit `ToolCallFinished` for the
// run that asked for the background work -- not the full `ToolResult` (that would reintroduce
// `ContentItem`'s variant into a plain tell message for no reason `emit_run_event`'s own
// `ToolCallFinished` payload -- `{call_id, ok}`, run_event.hpp -- actually needs).
struct BackgroundTaskDone {  // ae-naming-lint: allow BackgroundTaskDone — 006 §6b/019 §2 name this concept normatively; 027 has not been updated to list a message type for it
    std::string handle_id;
    std::string call_id;
    bool        ok = false;
};

// 027 §2's canonical name; already listed there, no suppression needed.
struct AgentResponse {
    Message message;
    Usage   usage;
};

// Milestone 4 Phase A4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md),
// narrowed per the project owner's own scoping decision at kick-off: a session's real, durable
// identity fields — `session_id`, `principal`, `created_at`/`updated_at` — go through Quark's
// `Store` seam (005 §2) via the Snapshot model (`quark::snapshot_sequential`/`recover_snapshot`),
// mirroring `persistence_snapshot_roundtrip_test.cpp`'s own precedent exactly (the first real use
// of Quark's Snapshot API anywhere in this codebase — `core/worktree.hpp`'s `Ref` uses the
// EventSourced model instead, decision 1 there). **`history[]`/`state`/`metadata` are explicitly
// NOT persisted by this record** — `Message`/`ContentItem` (003) have no `QUARK_SERIALIZE` at all
// yet (`ContentItem::value` is a 9-way `std::variant`, a real, un-designed serialization question
// this task does not improvise an answer to), and `StateT` is caller-declared and not guaranteed
// `Described`. A real full-session snapshot is a follow-up task once Message/ContentItem
// serialization exists — named here, not silently assumed covered by this narrower record.
//
// Fields are flattened (`principal_id`/`principal_tenant_id`, not a nested `Principal`), matching
// the only pattern this codebase has actually exercised so far (`RefMoved`/`RefState` are likewise
// single flat string fields) rather than assuming untested nested-`Described`-type recursion works
// on the first real multi-field record. `created_at`/`updated_at` are stored as raw nanosecond
// counts (`Described` has no `std::chrono::time_point` support) — an exact, lossless round-trip of
// whatever `AgentSession` currently holds, not a claim that these fields carry real wall-clock
// values yet (001 §7: they still don't, unchanged from M1's own scope note above).
struct AgentSessionRecord {  // ae-naming-lint: allow AgentSessionRecord — 005 §2 names the concept ("session persistence") normatively; 027 has not been updated to list a durable-record type
    std::string   session_id;
    std::string   principal_id;
    std::string   principal_tenant_id;
    std::int64_t  created_at_ns = 0;
    std::int64_t  updated_at_ns = 0;
    // Milestone 4 Phase C3 (005 §6: "Delete — hard removal... with a completion receipt"). A
    // tombstone flag, not a physical erasure of prior bytes: Quark's `Store` seam is append-only
    // (`FileStore`'s WAL) with an overwrite-latest-state Snapshot model (005 §2's own table) — this
    // project adds no storage engine of its own, so "hard removal" at this seam means the latest
    // snapshot for a `session_id` becomes an explicit tombstone, and `load_agent_session_snapshot`
    // (below) treats a tombstoned record exactly like "never snapshotted" to its own caller: no
    // residue is OBSERVABLE through this project's own read path, which is the real capability this
    // milestone can prove. Physical byte-level erasure of earlier log/snapshot entries is a Quark
    // storage-engine capability this project doesn't build (005 §2's own "no storage engine of our
    // own" rule).
    bool          deleted = false;

    // Milestone 4 Phase D1 (019 §1: checkpoint content is "the run's position, session delta,
    // ... and the capability set (recorded as references, never live handles)"). 019 §1 also says
    // storage is "the Quark 012 Store seam, shared with sessions (005 §2). No second persistence
    // engine" — taken literally: a checkpoint at this milestone's scope IS this same record, taken
    // at a turn boundary, extended with the run's position. `run_counter`/`turn_index` are plain
    // integers (Phase A3's real identity), trivially `Described` — no serialization gap to name
    // for THESE two fields, unlike the ones below.
    //
    // `run_counter` (not the derived `run_id` string) is what's persisted: `run_id` is always
    // `session_id + ":run:" + run_counter` (agent_session.hpp's `handle()`), so persisting the
    // counter is sufficient and avoids parsing a string back apart on restore — and, more
    // importantly, restoring it is what keeps a POST-RESTORE session's next `StartRun` from
    // reminting a `run_id` that collides with a run that already happened before the crash this
    // checkpoint survived (001 §1: every `Run` gets a fresh id).
    std::uint64_t run_counter = 0;
    std::uint64_t turn_index  = 0;

    // NOT yet real (named, not silently defaulted to a value that looks meaningful): 019 §1's
    // other two checkpoint-content items.
    //   - "session delta" (the history/state change since the last checkpoint) needs
    //     Message/ContentItem serialization, the same gap A4 already named above — there is
    //     nothing here to hold it once that lands, this record just doesn't carry it yet.
    //   - "capability set... as references" needs two things this milestone doesn't build: (a) a
    //     `Capability`/`cap::*` `QUARK_SERIALIZE` (16-way variant, no smaller than
    //     `ContentItem::value`'s own gap), and (b) 007 itself doesn't yet specify how a SESSION
    //     (as opposed to a single tool call's per-invocation bind, `trust/capability.hpp`'s own
    //     real, built mechanism) acquires or holds a capability grant across its OWN lifetime —
    //     that is a real gap in 007's own design, not an implementation shortcut this task can
    //     close on its own authority.
    // Milestone 4 Phase E1: `Interaction` (001 §2) is all scalars/strings — no variant, no
    // serialization gap — so unlike "session delta" and "capability set as references" above,
    // 019 §1's "pending approvals/input requests" checkpoint-content item CAN be represented for
    // real, not as a placeholder. `Interaction` is `Described` (interaction.hpp), and
    // `std::vector<NestedDescribedType>` is an already-proven shape in this project's own
    // dependency (Quark's `serialize_roundtrip_test.cpp`: `Order.lines` is `std::vector<Line>`
    // where `Line` is itself `QUARK_SERIALIZE`'d) — not an untested first use.
    std::vector<Interaction> open_interactions;

    friend bool operator==(AgentSessionRecord const&, AgentSessionRecord const&) = default;
};
QUARK_SERIALIZE(AgentSessionRecord, (1, session_id), (2, principal_id), (3, principal_tenant_id),
                 (4, created_at_ns), (5, updated_at_ns), (6, deleted), (7, run_counter),
                 (8, turn_index), (9, open_interactions))

template <class ChatClientT, class StateT = NoSessionState,
          class HistoryProviderT = HistoryProvider<Window<0>>>
    requires (ChatClient<ChatClientT> || ModelCallGatewayLike<ChatClientT>) &&
             ContextProvider<HistoryProviderT>
class AgentSession
    : public quark::Actor<AgentSession<ChatClientT, StateT, HistoryProviderT>, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<StartRun, AgentResponse>,
                                      quark::Ask<ResolveInteraction, AgentResponse>, TimerWake,
                                      BackgroundTaskDone>;

    // 001 §3's turn loop, now a real bounded round loop (superseding M1's "one model call per run"
    // scope note that used to sit here — see ADR-027-agent-session-tool-call-loop.md for the
    // design/red-team/prove record). One `StartRun` ask can now trigger N internal rounds of
    // model-call -> tool-call -> feed-back, all within this one coroutine, before exactly one
    // `m.respond(...)` (or none, on any failure path — every branch below keeps the pre-existing
    // "never fabricate a response, never hang" fail-closed shape: `quark::TestKit`/a real `Engine`'s
    // reply-cell fails a never-answered `Ask` deterministically on handler completion, not a hang).
    //
    // Scope (decided, not re-litigated per-call): only tools whose `invoke()` needs nothing beyond
    // `EffectContext&` are reachable through this loop — a tool needing session-scoped mutable state
    // (a persistent interpreter, mounted-skill state) is a separate, later design (`Tool<>::invoke()`
    // has no path to `state_` today). Approval is synchronous-decider only by default
    // (`approval_decider_` below); ADR-029 adds an OPT-IN (`suspend_for_approval_`, default false)
    // path that suspends a round for a real human answer via the `open_interaction()`/
    // `resolve_interaction()` primitives this comment used to call "existing but unwired" — see
    // `run_rounds()` and `handle(Ask<ResolveInteraction, ...>)` below.
    quark::task<> handle(quark::Ask<StartRun, AgentResponse> const& m) {
        // Milestone 5 Phase H2 (018 §2): admission is checked FIRST, before `run_counter_` even
        // increments — a denied caller must not consume a run_id or mutate any session state, the
        // same "cheapest check first, before ChatClient::chat() is ever reached" ordering 018 §2's
        // own text asks for. `m.query.caller == std::nullopt` skips the check (see `StartRun`'s own
        // comment for why); when present, it's widened into a `Principal` (on_behalf_of/kind left
        // at their defaults — `SessionCaller` cannot express them, see its own comment) and checked
        // against `principal_admitted_for` (trust/principal.hpp), the one shared ownership rule,
        // reused rather than re-derived here — it degrades to an exact id/tenant match for this
        // narrower wire type, which is exactly the rule `StartRun::caller`'s own comment documents.
        if (m.query.caller.has_value() &&
            !principal_admitted_for(Principal{m.query.caller->id, m.query.caller->tenant_id}, principal_)) {
            ++admission_denied_count_;
            co_return;
        }

        // ADR-029 red-team finding #5, narrowed to `interaction_reason::approval` specifically (NOT
        // `has_open_interactions()` in general): `test_agent_session_suspend_resume.cpp` /
        // `test_suspended_zero_resources_e2e.cpp` already proved, before this ADR, that an open
        // `input`/`auth` `Interaction` (001 §2's general "durable, out-of-band wait" record — e.g. a
        // host's own passivate/reactivate bookkeeping) coexists fine with an ordinary new `StartRun`;
        // that Interaction names no pending, un-invoked tool call and leaves `history_` in an
        // otherwise-ordinary state a fresh run can safely append to. An open `approval` Interaction is
        // different in kind: `run_rounds()`'s suspend branch (below) mints it ONLY while `history_`
        // ends in a pending, un-invoked assistant tool-call round — a second concurrent `StartRun`
        // over that exact state would be genuinely incoherent (which round do the eventual tool
        // results belong to?), not merely redundant. So only THIS reason blocks a fresh run.
        bool const has_open_approval =
            std::any_of(open_interactions_.begin(), open_interactions_.end(),
                        [](Interaction const& i) { return i.reason == interaction_reason::approval; });
        if (has_open_approval) {
            co_return;
        }

        // 001 §1: "Run: An Ask<StartRun, RunResponse> to the session actor" — literally, one run
        // per StartRun ask, regardless of how many internal rounds it takes to converge. `run_id`
        // is minted here, deterministically, from the session's own monotonic counter (never
        // wall-clock — 001 §7/I5).
        run_counter_ += 1;
        run_tokens_consumed_ = 0;
        effect_context_.principal    = principal_;
        effect_context_.capabilities = capabilities_;
        effect_context_.run_id       = session_id_ + ":run:" + std::to_string(run_counter_);
        effect_context_.turn_index   = 0;
        last_run_id_ = effect_context_.run_id;

        emit_run_event(run_event_kind::run_started);
        if constexpr (ModelCallGatewayLike<ChatClientT>) {
            // ADR-036: the gateway-routed branch's own trade, distinct from ADR-034's
            // stream_model_calls_ warning below (which never fires here -- see run_model_call()'s
            // own comment for why that flag is simply irrelevant on this branch). Named per I8: a
            // single round can now make up to `max_attempts * (1 + num_fallbacks)` real backend
            // calls -- any of which may have consumed real provider-side compute -- before
            // run_tokens_consumed_ is ever updated (that only happens once, after this whole call
            // returns, from the one response that actually succeeds). Not a new hazard class (the
            // single-attempt streaming path already only checks the budget post-hoc, agent_session.hpp
            // own `run_rounds()`), but this branch is designed to make retry/failover the ergonomic
            // default, so it's exercised far more often -- worth a visible fact about the run, not a
            // silent one, matching this codebase's own established pattern for named trade-offs.
            emit_run_event(run_event_kind::warning,
                            run_event_payload::Warning{
                                "this run routes model calls through a ModelCallGateway (ADR-036): no "
                                "live model_delta events fire for a gateway-routed round, and a single "
                                "round may make several real backend calls (retries/fallback tiers) "
                                "before the per-run token budget is ever checked"});
        } else if (stream_model_calls_) {
            // ADR-034: a visible fact about this run, not a silent one -- see
            // `set_stream_model_calls()`'s own comment for exactly what's traded away.
            // Response-format-leak-scanning is NOT on this list (ADR-035 Phase 1): it now runs
            // inside `run_model_call()` itself, applying uniformly whether or not this run streams
            // -- see `set_scan_response_format_leaks()`'s own comment.
            emit_run_event(run_event_kind::warning,
                            run_event_payload::Warning{
                                "this run streams each model call (ADR-034): failover/circuit-"
                                "breaker-feedback do not apply on the streaming path, even if the "
                                "bound ChatClientT would otherwise provide them"});
        }
        history_.push_back(m.query.input);

        co_await run_rounds(m);
    }

    // ADR-029: resumes a run `run_rounds()` suspended in its suspend-for-approval branch, with a
    // real human's answer. Ordering below fixes red-team finding #4 (validate BEFORE resolving, not
    // resolve-then-discover-a-mismatch): an unknown interaction id, or a `history_` tail that no
    // longer looks like the exact pending round that was suspended, fails closed WITHOUT mutating
    // `open_interactions_` or `history_` at all.
    quark::task<> handle(quark::Ask<ResolveInteraction, AgentResponse> const& m) {
        // Same admission shape as `StartRun::caller` (018 §2) above, reused for the same reason
        // (red-team finding #6): an unrelated principal must not resolve another principal's pending
        // approval by guessing an interaction_id.
        if (m.query.caller.has_value() &&
            !principal_admitted_for(Principal{m.query.caller->id, m.query.caller->tenant_id}, principal_)) {
            ++admission_denied_count_;
            co_return;
        }

        auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                                [&](Interaction const& i) { return i.interaction_id == m.query.interaction_id; });
        if (it == open_interactions_.end()) {
            co_return;  // unknown id -- fail closed, nothing mutated
        }

        // `run_rounds()`'s suspend branch always leaves `history_` ending in the assistant message
        // that carries the pending tool call(s) (it suspends BEFORE folding any tool-results message
        // in) -- if that shape isn't still true, session state has moved on since suspension (a
        // second resolver already ran, or something else mutated history_) and resuming now would
        // resolve calls against a round that may no longer be the one a human actually reviewed.
        if (history_.empty() || history_.back().role != role::assistant) {
            co_return;
        }
        std::vector<ToolCall> const pending_calls = tool_calls_of(history_.back());
        if (pending_calls.empty()) {
            co_return;
        }

        result<void> const resolved = resolve_interaction(it->interaction_id);
        if (!resolved) {
            co_return;  // lost a race with another resolver for this same id -- fail closed
        }
        emit_run_event(run_event_kind::input_resolved,
                        run_event_payload::InteractionRef{m.query.interaction_id});

        std::size_t const response_msg_index = history_.size() - 1;

        if (!m.query.approved) {
            // Denied: fold a denial `ToolResult` for every pending call (the same shape
            // `invoke_tool`'s own fail-closed-missing-decider path already produces) and feed it back
            // to the model like any other tool error -- it may retry with different arguments,
            // explain to the user why it can't proceed, or ask again. `tool_call_requires_approval`
            // in `run_rounds()` is what decides whether a NEW attempt suspends again.
            std::vector<ToolResult> results;
            results.reserve(pending_calls.size());
            for (ToolCall const& call : pending_calls) {
                emit_run_event(run_event_kind::approval_resolved,
                                run_event_payload::ApprovalResolved{call.call_id, false,
                                                                      m.query.interaction_id});
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
            co_await run_rounds(m);
            co_return;
        }

        // Approved: re-derive the same tool table this round would have used (a fresh `on_context`
        // call, matching `run_rounds()`'s own "re-assembled fresh every round" rule) and invoke every
        // pending call for real through the ordinary `invoke_tool` pipeline (not bypassed -- I3: a
        // human's real decision drives this, but every other pipeline step -- capability, taint,
        // idempotency, accounting -- still runs exactly as it would have inline). The one-shot
        // decider approving unconditionally is safe specifically BECAUSE the validation above already
        // confirmed `pending_calls` is the exact, unchanged set a human was shown.
        SessionContext session_ctx{session_id_, principal_, history_};
        result<ContextContribution> contribution =
            co_await history_provider_.on_context(session_ctx, effect_context_);
        if (!contribution) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{"run.context_unavailable",
                                                          contribution.error().message});
            co_return;
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
                                                                  m.query.interaction_id});
            results.push_back(std::move(result));
        }
        history_.push_back(tool_results_message(std::move(results)));
        co_await history_provider_.on_turn_end(
            TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                history_.size() - response_msg_index}},
            effect_context_);
        emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
        ++effect_context_.turn_index;
        co_await run_rounds(m);
    }

private:
    // ADR-034: routes to `chat_stream()` when a caller has opted the session INTO streaming
    // (`stream_model_calls_`, default false — see `set_stream_model_calls()`'s own comment for the
    // real tradeoffs an opted-in caller accepts); otherwise calls `chat()` exactly as ADR-027
    // shipped it, byte-for-byte, so every existing/default caller is completely unaffected.
    //
    // Text deltas fire a real `run_event_kind::model_delta` as they arrive (013 §1's own vocabulary,
    // previously declared with zero real emitter). Every delta's `ContentItem` — Text or an
    // eventually-assembled ToolCall — is accumulated, IN ORDER, into one reconstructed `Message`;
    // `tool_call_extraction.hpp`'s `tool_calls_of()`/`text_of()` (what this loop and its callers
    // actually read the result through) are genuinely position/count-agnostic across however many
    // separate content items a Message carries, so this reconstruction needs no merging step to be
    // equivalent to what `chat()`'s own one-shot parse would have produced.
    //
    // Fails closed, hard, on missing usage: 004 §5's `TokenBudget<N>` depends on
    // `run_tokens_consumed_` being a true per-call count. `ChatResponseUpdate::usage` is
    // `std::optional` because not every backend/config populates it on the streaming path yet — this
    // function refuses to treat "the backend reported nothing" as "this call cost nothing", which is
    // the one way a silent budget bypass could otherwise happen.
    //
    // ADR-035 Phase 1: both branches converge on ONE tail below that applies
    // `apply_response_format_scan()` when `scan_response_format_leaks_` is armed — backend-agnostic
    // (OpenAI or Anthropic) and path-agnostic (streamed or not), unlike ADR-023's original scan,
    // which only ever ran inside `OpenAIChatClient::chat()` itself. `request.tools` is the same
    // `ChatRequest` already sent to the backend this round — no drift between what was declared and
    // what the scan matches candidates against.
    // ADR-036: when `ChatClientT` is a `ModelCallGatewayLike` conformer (`core/model_call_gateway.hpp`'s
    // `ModelCallGateway<Primary, Fallback...>`) instead of a raw `ChatClient`, this whole function
    // collapses to one `co_await chat_client_->call(request, ctx)` — retry, circuit-breaking,
    // failover, and middleware hooks all live inside the gateway's own `call()`, which is itself a
    // real coroutine (unlike `chat_stream()`), so a middleware hook is a plain, safe `co_await` there
    // with no leaked-frame hazard. No live `model_delta` events fire for a gateway-routed round (an
    // accepted, named trade — see `model_call_gateway.hpp`'s own top comment for why a retried/
    // failed-over/middleware-reviewed attempt cannot safely be shown to the caller live, token by
    // token, without risking a silent mid-stream backend substitution). `stream_model_calls_` is
    // simply irrelevant on this branch — a gateway-backed session's rounds are always
    // buffer-then-return, never live-streamed, regardless of that flag's value.
    quark::task<result<ChatResponse>> run_model_call(ChatRequest const& request, EffectContext& ctx) {
        result<ChatResponse> response = std::unexpected(
            error{failure_class::contract, "unreachable: neither call path executed", "run.internal"});

        if constexpr (ModelCallGatewayLike<ChatClientT>) {
            response = co_await chat_client_->call(request, ctx);
        } else {
            if (!stream_model_calls_) {
                response = co_await chat_client_->chat(request, ctx);
            } else {
                stream<ChatResponseUpdate> s = chat_client_->chat_stream(request, ctx);
                Message accumulated;
                accumulated.role = role::assistant;
                std::optional<Usage> usage;
                while (!s.done()) {
                    while (std::optional<ChatResponseUpdate> upd = s.next()) {
                        if (auto const* t = std::get_if<Text>(&upd->delta.value);
                            t != nullptr && !t->text.empty()) {
                            emit_run_event(run_event_kind::model_delta, run_event_payload::ModelDelta{t->text});
                        }
                        accumulated.content.push_back(upd->delta);
                        if (upd->is_final && upd->usage.has_value()) usage = upd->usage;
                    }
                    // The ring is momentarily empty but the producer thread is still live (real
                    // backends run their blocking HTTP/SSE read loop on a detached worker thread, see
                    // chat_stream()'s own implementation) — a bounded sleep, not a bare spin, so this
                    // doesn't burn the actor's own worker-thread CPU for the whole call the way a
                    // tight yield()-loop would.
                    if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (s.terminal() != quark::ReplyStreamTerminal::Closed) {
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
        }

        if (response.has_value() && scan_response_format_leaks_) {
            response->message = apply_response_format_scan(std::move(response->message), request.tools);
        }
        co_return response;
    }

    // ADR-029: the shared "assemble context -> call model -> invoke calls -> fold results ->
    // continue" control flow both `handle(Ask<StartRun, ...>)`'s fresh run and
    // `handle(Ask<ResolveInteraction, ...>)`'s resumed continuation drive, written exactly once —
    // previously this was `handle(Ask<StartRun, ...>)`'s entire loop body inline; ADR-029 factors it
    // out rather than duplicating it a second time for the resume path. `AskT` is whichever of the
    // two `quark::Ask<...>` types is currently driving the run; both share the same `respond
    // (AgentResponse)` shape (Quark's `Ask<Q, R>` is uniform in `R` regardless of `Q`), which is all
    // this needs from `m`. Returns `quark::task<std::monostate>`, never bare `quark::task<>` --
    // ADR-047 (see task.hpp's own banner comment, and `context_provider.hpp`'s `on_turn_end` for
    // this codebase's own established precedent): only `task<T>` for `T != void` is a genuinely
    // awaitable nested coroutine a handler's own `task<void>` frame can `co_await`; a second
    // top-level `task<void>` is the executor-only, detach()-only handler-frame type and has no
    // `await_resume()` at all.
    template <class AskT>
    quark::task<std::monostate> run_rounds(AskT const& m) {
        // ADR-018's non-owning capability pointer, `nullptr` by default ("this session may reach no
        // effect", I2). `invoke_tool` takes `CapabilitySet const&`, not a pointer — a null
        // `capabilities_` falls back to a real, empty, root-granted set so every tool call this run
        // attempts fails CLOSED at `invoke_tool`'s own step-4 authorize check (`tool.capability_not_held`,
        // fed back to the model like any other tool error) rather than this handler needing a second,
        // bespoke "no capabilities at all" failure path. Reachable: a `HistoryProviderT` can declare
        // tools independent of whether a host ever called `set_capabilities()`.
        CapabilitySet const empty_caps = CapabilitySet::grant_root({});
        CapabilitySet const& held      = capabilities_ ? *capabilities_ : empty_caps;

        for (; !max_turns_.has_value() || effect_context_.turn_index < *max_turns_;
             ++effect_context_.turn_index) {
            emit_run_event(run_event_kind::turn_started, run_event_payload::Turn{effect_context_.turn_index});

            // Milestone 4 Phase B2: what the model sees is derived through a real `ContextProvider`
            // (005 §3/§5), re-assembled fresh every round — a `SkillsProvider`-composing provider may
            // legitimately change what it declares between rounds (e.g. a `mount_skill` call landing
            // mid-run), and re-deriving from the same live state every round is what keeps declared
            // and invocable tools the SAME snapshot (see `tool_table` below), matching ADR-024's
            // "declared and invocable stay derived from the same live state, on the same cadence"
            // invariant rather than merely conventionally.
            SessionContext session_ctx{session_id_, principal_, history_};
            result<ContextContribution> contribution =
                co_await history_provider_.on_context(session_ctx, effect_context_);
            if (!contribution) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.context_unavailable",
                                                              contribution.error().message});
                co_return std::monostate{};
            }

            // Same `contribution->tools` snapshot backs BOTH the declaration (`ChatRequest` below)
            // and the invocation table (`invoke_tool` calls further down) — one `ToolTable`, not two
            // independently constructed ones, closing structurally (for this loop) the "declared ≠
            // invocable" trap ADR-024 §3a/§7 named as enforced only by convention in `cli_chat.cpp`.
            ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);

            ChatRequest request{contribution->messages, contribution->tools};
            if (!chat_client_) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.no_chat_client", "no ChatClientT configured"});
                co_return std::monostate{};
            }
            emit_run_event(run_event_kind::model_call_started);
            result<ChatResponse> response = co_await run_model_call(request, effect_context_);
            emit_run_event(run_event_kind::model_call_finished);
            if (!response) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.chat_failed", response.error().message});
                co_return std::monostate{};
            }

            // Milestone 5 Phase F4 (004 §5: "per-run TokenBudget<N>"), now checked after EVERY
            // round's model call, not just one — the accumulator is per-RUN (reset above), so a
            // multi-round run's total is the sum across every round, exactly what "per-run" means
            // once a run can contain more than one model call.
            run_tokens_consumed_ += response->usage.input_tokens + response->usage.output_tokens;
            if (token_budget_.has_value() && run_tokens_consumed_ > *token_budget_) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{"run.token_budget_exceeded",
                                                              "per-run token budget exceeded"});
                co_return std::monostate{};
            }

            std::size_t const response_msg_index = history_.size();
            history_.push_back(response->message);

            std::vector<ToolCall> const calls = tool_calls_of(response->message);
            if (calls.empty()) {
                // Converged: this round's response carries no tool call, so it's the run's final
                // answer. `TurnView` here covers exactly this round's own addition (the response) —
                // see the ADR's named residual on why this is narrower than the old single-call
                // shape ({input, response}) and why nothing observes the difference yet.
                co_await history_provider_.on_turn_end(
                    TurnView{std::span<Message const>{history_.data() + response_msg_index, 1}},
                    effect_context_);
                emit_run_event(run_event_kind::turn_finished,
                                run_event_payload::Turn{effect_context_.turn_index});
                emit_run_event(run_event_kind::run_finished);
                m.respond(AgentResponse{response->message, response->usage});
                co_return std::monostate{};
            }

            // ADR-029: before invoking anything this round, decide whether ANY pending call needs
            // real human approval that a synchronous `approval_decider_` cannot supply. Scope
            // (red-team-confirmed, not a simplification): the WHOLE round suspends atomically -- no
            // call in a round containing even one approval-needing call is invoked yet, matching the
            // live-proven fact (MT-2) that a real round can carry more than one call, and this loop
            // already treats a round as one unit (the single `tool_results_message` fold below).
            // Opt-in only (`suspend_for_approval_` defaults false), and only when no real
            // `approval_decider_` is configured -- with either condition false, behavior is
            // byte-for-byte what ADR-027 shipped: every approval-needing call is evaluated (and,
            // absent a decider, denied) by `invoke_tool` itself, same as before this ADR existed.
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
                    // Suspended -- deliberately no `m.respond()` here (same "never fabricate a
                    // response, never hang" shape every other fail-closed branch in this function
                    // uses): the caller's `Ask` is left unanswered until a matching
                    // `Ask<ResolveInteraction, AgentResponse>` resumes this exact round.
                    co_return std::monostate{};
                }
            }

            // Sequential, not concurrent (matching the one proven reference shape, `cli_chat.cpp`'s
            // former external loop) — 006 §6b's `Parallelizable`/concurrent dispatch is declared
            // vocabulary this loop does not wire up. A tool call failing (`r.is_error`) does NOT
            // abort the run: the error is folded into the tool-results message like any successful
            // result and fed back to the model, which may retry, adjust, or explain — matching the
            // one live-proven behavior this loop replaces (cli_chat.cpp's former external loop).
            std::vector<ToolResult> results;
            results.reserve(calls.size());
            for (std::size_t i = 0; i < calls.size(); ++i) {
                // `tool_call_request_of` (tool_call_extraction.hpp) is what threads `ToolCall::
                // provenance` into `ToolCallRequest::provenance` — the fix for a real bug found
                // while designing this loop: omitting it silently defaults every call to
                // `vendor_structured`, letting a `text_derived` (laundered, model-injected) call
                // bypass ADR-023 §4b Finding 1's declassification gate and be evaluated under the
                // tool's own possibly-`never_require` approval mode instead. See that header's own
                // comment for the full finding.
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

            std::size_t const tool_msg_index = history_.size();
            history_.push_back(tool_results_message(std::move(results)));

            co_await history_provider_.on_turn_end(
                TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                                    history_.size() - response_msg_index}},
                effect_context_);
            emit_run_event(run_event_kind::turn_finished,
                            run_event_payload::Turn{effect_context_.turn_index});
            (void)tool_msg_index;  // named for readability of the span math above, not read separately
        }

        // Loop exhausted `max_turns_` without ever reaching a tool-call-free round — fails the whole
        // run closed (no `m.respond()`), the same shape as every other branch above, rather than
        // returning the last, still-incomplete model message. A session with an approval-gated tool
        // and no `set_approval_decider()`/`suspend_for_approval_` configured lands here too (every
        // call denied every round) — that specific cause is not yet distinguishable from ordinary
        // non-convergence in this event, a named residual (see the ADR).
        emit_run_event(run_event_kind::run_failed,
                        run_event_payload::RunFailed{"run.max_turns_exceeded",
                                                      "tool-call loop did not converge within max_turns"});
        co_return std::monostate{};
    }

public:

    // Milestone 4 Phase E3: the tell a fired Quark reminder delivers (see `TimerWake`'s own
    // comment). No coroutine/suspend semantics attach to this — it is a plain, synchronous
    // acknowledgement, matching the fact that nothing in this handler ever left this actor mid-run
    // to begin with.
    void handle(TimerWake const&) noexcept { ++timer_wakes_; }

    [[nodiscard]] std::uint64_t timer_wakes() const noexcept { return timer_wakes_; }

    // Milestone 7 Phase B (019 §2's "Local background task completion" wake row; 013 §1's
    // ToolCallFinished). Finds the effect FIRST, to capture the run_id it was registered under, THEN
    // erases it -- 013 §1's event must be attributed to the run that ASKED for the background work,
    // never whatever run happens to be current on this actor right now (see
    // `emit_run_event_for()`'s own comment for why a shared counter would have made this wrong). A
    // handle_id with no matching entry (already resolved, or `cancel_standing_effect()`d in the
    // meantime) is a silent, idempotent no-op -- exactly `BoundCapability::revoke()`'s own idempotency
    // shape one layer down.
    void handle(BackgroundTaskDone const& m) {
        auto it = std::find_if(standing_effects_.begin(), standing_effects_.end(),
                                [&](StandingEffect const& e) { return e.handle_id == m.handle_id; });
        if (it == standing_effects_.end()) return;
        std::string const owner_run_id = it->run_id;
        standing_effects_.erase(it);
        emit_run_event_for(owner_run_id, run_event_kind::tool_call_finished,
                            run_event_payload::ToolCallFinished{m.call_id, m.ok});
    }

    // Milestone 7 Phase B (006 §6b's "one introspection/kill surface"). A plain read -- the caller
    // decides what "currently outstanding" means to show a human or a protocol surface.
    [[nodiscard]] std::vector<StandingEffect> const& list_standing_effects() const noexcept {
        return standing_effects_;
    }

    // 006 §6b G8: cross-principal `cancel_standing_effect()` denial, proven both same- and
    // cross-principal. Checks the EFFECT's own recorded `principal_id` (the run that registered it),
    // not this session's own `principal_` -- today `start_background_task()` below always registers
    // under the current run's principal so the two coincide, but checking the effect's own record
    // keeps this correct if a future caller ever registers on behalf of someone else. Cancelling
    // removes the BOOKKEEPING only -- 006 §6b names no mechanism to interrupt step 8's already-running
    // native `invoke()` (tool_pipeline.hpp's own `background_task()` comment), so a late
    // `BackgroundTaskDone` for a canceled handle simply finds nothing to resolve (this handler's own
    // idempotent no-op above).
    [[nodiscard]] result<void> cancel_standing_effect(std::string const& handle_id,
                                                       Principal const& caller_principal) {
        auto it = std::find_if(standing_effects_.begin(), standing_effects_.end(),
                                [&](StandingEffect const& e) { return e.handle_id == handle_id; });
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

    // Milestone 7 Phase B (006 §6b): the real StandingEffect producer this phase builds.
    // `self` is the CALLER's own `ActorRef` to THIS session -- the same "host arms the callback, the
    // actor never self-addresses" shape `test_agent_session_timer_wake.cpp` already established for
    // `TimerWake` (Quark gives an actor no implicit handle to its own address, and this codebase has
    // never needed one before this). Runs `tool_pipeline.hpp`'s `background_task()` (steps 1-7
    // synchronous -- including Backgroundable/Background<max_concurrent> enforcement -- step 8 onward
    // detached), wired so `on_complete` fires a `self.tell(BackgroundTaskDone{...})`.
    [[nodiscard]] result<StandingEffect> start_background_task(
        quark::ActorRef<AgentSession> self, ToolTable const& table, ToolCallRequest const& request,
        ApprovalDecider const& approve = ApprovalDecider{}) {
        if (!capabilities_) {
            return std::unexpected(error{failure_class::policy, "session has no granted capabilities",
                                          "standing_effect.no_capabilities"});
        }
        std::size_t const current_count = static_cast<std::size_t>(std::count_if(
            standing_effects_.begin(), standing_effects_.end(),
            [](StandingEffect const& e) { return e.kind == standing_effect_kind::background_task; }));

        std::string const handle_id =
            session_id_ + ":standing:" + std::to_string(++standing_effect_counter_);
        std::string const owner_run_id       = effect_context_.run_id;
        std::string const owner_principal_id = effect_context_.principal.id;

        result<void> submitted = background_task(
            table, *capabilities_, request, effect_context_, approve, current_count,
            [self, handle_id, call_id = request.call_id](ToolResult /*result_out*/,
                                                           ToolInvocationAudit audit) mutable {
                self.tell(BackgroundTaskDone{handle_id, call_id, audit.ok});
            });
        if (!submitted) return std::unexpected(submitted.error());

        StandingEffect effect;
        effect.handle_id    = handle_id;
        effect.session_id   = session_id_;
        effect.principal_id = owner_principal_id;
        effect.run_id       = owner_run_id;
        effect.kind         = standing_effect_kind::background_task;
        effect.label        = request.tool_name;
        standing_effects_.push_back(effect);

        emit_run_event_for(owner_run_id, run_event_kind::tool_call_started,
                            run_event_payload::ToolCallStarted{request.call_id, request.tool_name});
        return effect;
    }

    // Milestone 7 Phase A (013 §1). Mirrors `WorkflowSupervisor::enable_live_view()` exactly (M6
    // Phase G): build a fresh producer/consumer pair over `run_event.hpp`'s `RunEvent`, keep the
    // producer, hand the caller the consumer. Call before the first `StartRun` a caller wants
    // observed -- an event fired before this is called is simply not pushed (`emit_run_event`'s own
    // `.valid()` guard), the same "invalid until enabled" default `live_view_producer_` has.
    [[nodiscard]] stream<RunEvent> enable_event_stream(std::pmr::memory_resource* mr,
                                                        stream_config<RunEvent> cfg = {}) {
        auto pair            = make_stream<RunEvent>(mr, cfg);
        run_event_producer_ = std::move(pair.producer);
        return std::move(pair.consumer);
    }

    [[nodiscard]] std::vector<Message> const& history() const noexcept { return history_; }

    // `quark::TestKit<A>`/a real `Engine` both default-construct the actor and hand out a mutable
    // `A&` for post-construction wiring (testkit.hpp:83, "state access for wiring + assertions") —
    // there is no constructor-argument-forwarding path through either, so `session_id`/`principal`
    // are set this way, matching how the mock `ChatClient`'s own canned behavior is already baked
    // in by construction rather than passed through TestKit. Named `initialize`, the same verb this
    // project already uses for "make an object ready to run, before capabilities/effects flow"
    // (`MediatedPythonRunner::initialize()`).
    //
    // Milestone 5 Phase F4: `token_budget` is ADDITIVE, appended last with a default of
    // `std::nullopt` (= unbounded, matching `AgentMetadata::token_budget`'s own "nullopt = unbounded"
    // convention, agent_registry.hpp:60) -- the same "old call sites keep compiling with their old
    // behavior unchanged" precedent `register_agent<A>()`'s own additive `ChatClientRegistry const*`
    // parameter set (agent_registry.hpp:489), so every pre-M5 two-argument `.initialize(id,
    // principal)` call site across `tests/` keeps compiling and stays unbounded, unchanged. The
    // natural source of this value is a registered agent's compiled `AgentMetadata::token_budget`
    // (agent_registry.hpp:60, itself compiled from an agent's declared `TokenBudget<N>` policy tag) --
    // wiring that end-to-end through however a session gets constructed from a registered agent is
    // left as a named follow-up; this parameter is the `AgentSession`-side mechanism that consumes it.
    // `max_turns` is ADDITIVE, appended last with a default of `16` (matching `AgentMetadata::
    // max_turns`'s own default, agent_registry.hpp) -- the same "old call sites keep compiling with
    // their old behavior unchanged" precedent `token_budget` established immediately above it.
    //
    // DELIBERATE DEPARTURE (2026-08-12, explicit project-owner decision): defaults to
    // `std::nullopt` (unbounded), the same "nullopt = unbounded" convention `token_budget` already
    // uses. This is knowingly NOT what 014 §2's "an unbounded workflow does not run — the bound is
    // required" rule would say for a `Workflow`, and knowingly loosens I8 ("budgets are enforced")
    // for this specific loop's default: nothing stops a model that never stops requesting tool
    // calls from running forever (`test_agent_session_tool_call_loop.cpp`'s own R4/R5 already prove
    // the bounded case still works correctly -- both pin an explicit `max_turns=3`, unaffected by
    // this default change). A caller that wants the old safety valve back sets `max_turns` (or
    // `token_budget`, which still defaults unbounded too and is the other real backstop) explicitly.
    void initialize(std::string session_id, Principal principal,
                     std::optional<std::uint64_t> token_budget = std::nullopt,
                     std::optional<std::uint64_t> max_turns = std::nullopt) {
        session_id_   = std::move(session_id);
        principal_    = std::move(principal);
        token_budget_ = token_budget;
        max_turns_    = max_turns;
    }

    // ADR-018 (closes the Milestone 5 Phase J1 residual): construct this session's `ChatClientT` IN
    // PLACE, after the actor itself exists. This is what lets a REAL Phase D/E backend -- which is not
    // default-constructible, since `OpenAIChatClient<Store>`/`AnthropicChatClient<Store>` hold a
    // `Store const&` -- be driven through this actor's real turn loop.
    //
    // In-place rather than a `set_chat_client(ChatClientT)` setter on purpose: a backend holding a
    // reference member is not assignable, so a setter would not compile for exactly the types this
    // exists to serve. Emplacement needs only that the type be constructible from `args`.
    //
    // Configuration-time only, like `initialize()`: call it before the first `StartRun`, from the
    // owner that also owns the `SecretStore` the client references (that store must outlive this
    // session). Nothing here reads model output, so this is not a taint or authority surface (I3).
    template <class... Args>
    ChatClientT& emplace_chat_client(Args&&... args) {
        return chat_client_.emplace(std::forward<Args>(args)...);
    }

    [[nodiscard]] bool has_chat_client() const noexcept { return chat_client_.has_value(); }

    // ADR-018: the capability set every run of this session executes under, threaded into
    // `EffectContext` at the top of each turn. Non-owning -- the granting host owns it and must
    // outlive the session, the same contract `EffectContext::capabilities` already has.
    //
    // Configuration-time, like `initialize()`/`emplace_chat_client()`, and never derived from model
    // output (I3) or from anything a turn produced: a run cannot widen its own authority.
    void set_capabilities(CapabilitySet const* capabilities) noexcept { capabilities_ = capabilities; }
    [[nodiscard]] CapabilitySet const* capabilities() const noexcept { return capabilities_; }

    // The synchronous approval gate every round's `invoke_tool` calls thread through (tool_pipeline.hpp
    // step 5). Defaults to an empty `ApprovalDecider{}` — `invoke_tool`'s own `approve &&
    // approve(...)` short-circuits to `false`, i.e. fail-closed: any tool declaring
    // `approval_mode != never_require` is denied on every call until a real decider is configured.
    // Configuration-time, like `set_capabilities()` immediately above — never derived from model
    // output (I3).
    void set_approval_decider(ApprovalDecider approve) { approval_decider_ = std::move(approve); }
    [[nodiscard]] ApprovalDecider const& approval_decider() const noexcept { return approval_decider_; }

    // ADR-030: a mutable accessor to the session's own `HistoryProviderT` instance, host-callable
    // like `set_capabilities()`/`emplace_chat_client()` immediately above -- needed the moment a
    // provider carries real, host-supplied configuration beyond what its own default constructor can
    // know (ADR-028 already established a provider MAY own session-scoped data; this is the
    // symmetric case of a provider needing session-external, host-owned configuration handed to it
    // after construction, e.g. a non-owning pointer to a process-wide-shared resource). Configuration-
    // time only, never derived from model output (I3) -- the same contract every other accessor in
    // this group already documents.
    [[nodiscard]] HistoryProviderT& history_provider() noexcept { return history_provider_; }

    // ADR-029: opts a session INTO suspending a round for a real human answer
    // (`Ask<ResolveInteraction, AgentResponse>`) instead of `invoke_tool`'s ordinary fail-closed-deny
    // when an approval-needing call has no synchronous `approval_decider_` configured. Defaults
    // false, like `approval_decider_` defaults empty — an existing caller that configures neither
    // gets the exact ADR-027 behavior (every approval-needing call denied every round, run eventually
    // fails `run.max_turns_exceeded`), not a silent behavior change. Configuration-time, same
    // category as `set_capabilities()`/`set_approval_decider()` immediately above.
    void set_suspend_for_approval(bool suspend) noexcept { suspend_for_approval_ = suspend; }
    [[nodiscard]] bool suspend_for_approval() const noexcept { return suspend_for_approval_; }

    // ADR-034: opts a session INTO real per-token streaming of each round's model call
    // (`ChatClientT::chat_stream()` instead of `chat()`), emitting `run_event_kind::model_delta` as
    // text arrives. Defaults false — an existing caller that never calls this keeps calling `chat()`
    // exactly as ADR-027 shipped it, bit-for-bit.
    //
    // A DELIBERATE, DOCUMENTED TRADEOFF an opted-in caller accepts, not a strictly-better mode:
    // `FailoverChatClient::chat_stream()` never falls over to a secondary backend (that type's own
    // file-top comment states this scoping outright — failover is `chat()`-only); and
    // `ResilientChatClient::chat_stream()` never reports an outcome to its circuit breaker (same
    // file, same reason) — neither is silently broken by streaming so much as simply not part of
    // what streaming exercises, and a caller composing either underneath a streaming session accepts
    // losing that protection for this session. A `run_event_kind::warning` fires once per run when
    // streaming is active, so this is a visible fact about the run, not a silent one.
    //
    // NOT on that tradeoff list (ADR-035 Phase 1, unlike ADR-034's original scope): response-format-
    // leak-scanning. `run_model_call()` applies `apply_response_format_scan()` itself, uniformly,
    // whether or not this flag is set — see `set_scan_response_format_leaks()` immediately below.
    void set_stream_model_calls(bool stream) noexcept { stream_model_calls_ = stream; }
    [[nodiscard]] bool stream_model_calls() const noexcept { return stream_model_calls_; }

    // ADR-035 Phase 1: opts a session INTO `apply_response_format_scan()` (ADR-023 §6 points 3-4,
    // now `core/response_format_leak_scan.hpp`) running once per model call, on the reconstructed
    // `Message`, in `run_model_call()` — regardless of which `ChatClientT` backend produced it or
    // whether this run streams. Defaults false, matching ADR-023 Finding 6: scanning is
    // operator-armed, never content-triggered. Previously this protection existed ONLY as
    // `OpenAIChatClient`'s own `scan_response_format_leaks` constructor flag, reachable solely
    // through that backend's non-streaming `chat()` — `AnthropicChatClient` had no equivalent in
    // either path. Arming THIS flag instead is now the general way to get the protection, on any
    // backend, on any path; `OpenAIChatClient`'s own flag still exists (unchanged behavior, for a
    // caller using that client directly without an `AgentSession`) but the two are independent knobs
    // — arming both for the same `OpenAIChatClient`-backed, non-streaming session runs the scan
    // twice, which is a safe no-op (`apply_response_format_scan` skips already-`tainted` items by
    // construction — see that function's own comment for why re-scanning a diagnostic it already
    // produced would otherwise be a real laundering path), not a behavior change, just wasted work.
    void set_scan_response_format_leaks(bool scan) noexcept { scan_response_format_leaks_ = scan; }
    [[nodiscard]] bool scan_response_format_leaks() const noexcept { return scan_response_format_leaks_; }

    // The per-run bound on internal tool-call rounds (see `handle()`'s own loop and `initialize()`'s
    // `max_turns` parameter, which is the normal way this gets set before the first `StartRun`).
    void set_max_turns(std::optional<std::uint64_t> max_turns) noexcept { max_turns_ = max_turns; }
    [[nodiscard]] std::optional<std::uint64_t> max_turns() const noexcept { return max_turns_; }

    // Milestone 4 Phase C1 (005 §6: "Fork — copy-on-write new session id from a history prefix;
    // the sanctioned answer to concurrent runs (001 §4) and to 'what if' exploration"). Copies
    // `source`'s `principal`/`history` (up to `history_prefix_len`, or the whole history if
    // absent)/`state`/`metadata` into `*this` under a NEW `session_id` — reusing A1's real
    // `session_actor_id()` isolation directly (the fork is just another session_id, indistinguishable
    // in kind from any other), and A4's persistence directly (the fork snapshots/loads through the
    // exact same `save_agent_session_snapshot`/`load_agent_session_snapshot` free functions, no
    // special-cased "forked session" storage path).
    //
    // Deliberately NOT copied: `run_counter_`/`last_run_id_` (001 §1 — a fork has had no `Run`s of
    // its own yet; inheriting the source's counter would make its own first run_id look like a
    // continuation of the source's run sequence, which it isn't), `created_at_`/`updated_at_`
    // (both are still unwired placeholders project-wide, 001 §7 — copying them would imply this
    // fork has real provenance timestamps neither session actually has yet), and (Phase E1)
    // `open_interactions_` — every open `Interaction` names a `run_id` (001 §2), and since the
    // fork inherits none of the source's run identity, an interaction referencing a run that never
    // happened on the fork would be incoherent, not merely stale.
    void fork_from(AgentSession const& source, std::string new_session_id,
                    std::optional<std::size_t> history_prefix_len = std::nullopt) {
        session_id_ = std::move(new_session_id);
        principal_  = source.principal_;

        std::size_t const n = std::min(history_prefix_len.value_or(source.history_.size()),
                                        source.history_.size());
        history_.assign(source.history_.begin(), source.history_.begin() + static_cast<std::ptrdiff_t>(n));

        state_    = source.state_;
        metadata_ = source.metadata_;
        // ADR-028 addendum: this was correctly left uncopied when `history_provider_` could only
        // ever be pure wiring/configuration (the same category `chat_client_`/`capabilities_` still
        // are, deliberately left untouched below). Since ADR-028, a `HistoryProviderT` MAY also own
        // real session-scoped DATA (e.g. a stateful tool's accumulated counter/exec state) via
        // `make_tool_descriptor_with_invoke` -- copying it here is what keeps a fork's `history_`
        // (already copied above, which may reference that provider's PAST results) consistent with
        // the state that would actually produce the NEXT one, instead of silently resetting to a
        // fresh default the moment a provider adopts ADR-028's mechanism. Requires `HistoryProviderT`
        // to be copy-assignable -- checked only at THIS method's own instantiation (a provider that
        // owns something non-copyable, e.g. a real `MediatedPythonRunner`, correctly fails to compile
        // here rather than silently forking into an unsafe partial copy).
        history_provider_ = source.history_provider_;

        run_counter_ = 0;
        last_run_id_.clear();
        effect_context_ = EffectContext{};
        created_at_      = std::chrono::system_clock::time_point{};
        updated_at_      = std::chrono::system_clock::time_point{};
        open_interactions_.clear();
        interaction_counter_ = 0;
        timer_wakes_ = 0;
        // Phase F4: per-run, same category as run_counter_ above -- a fork has had no runs of its
        // own yet, so it starts with no accumulated consumption either. `token_budget_` (the
        // configured ceiling) is NOT reset here, same reasoning as `chat_client_`/`capabilities_`
        // staying untouched: it is session configuration, not per-run counter state.
        // `history_provider_` moved OUT of that "untouched" set above (ADR-028 addendum) -- it can
        // now carry real session-scoped data, not just wiring.
        run_tokens_consumed_ = 0;
        // Phase H2: same reasoning again -- a fork has had no admission denials of its own yet.
        admission_denied_count_ = 0;
    }

    // Milestone 4 Phase C2 (005 §6: "Redact — replace content in place with a tombstone carrying
    // reason and actor"). Reuses `Custom` (003 §1's own designed escape hatch: "unknown kinds
    // round-trip via Custom", content.hpp) rather than adding a tenth `ContentItem::value`
    // alternative — a redaction tombstone is exactly a namespaced, non-core content kind, the case
    // `Custom` exists for; adding a new variant member would mean touching every exhaustive
    // `ContentItem::value` match in the codebase for a need `Custom` already covers.
    //
    // `reason`/`actor` are recorded on the tombstone (I4: every effect is attributable — a
    // redaction is one). ALL of the message's original content items are replaced by the single
    // tombstone; nothing about the original text/media survives in `history_` afterward, which is
    // what makes "propagates to derived summaries" (005 §6) true by construction for anything
    // computed from `history_` AFTER this call — `HistoryProvider<Summarize<N,...>>` (Phase B4)
    // folding this message into a summary sees only the tombstone, never the original text
    // (proven in test_agent_session_redact.cpp).
    //
    // NOT yet covered (named, not silently assumed complete): propagation into a durable
    // checkpoint. Phase A4's own narrowed `AgentSessionRecord` does not persist `history_` at all
    // (Message/ContentItem have no `QUARK_SERIALIZE` yet, the same gap A4 named), so there is no
    // checkpoint containing this history for the redaction to reach — that half of 005 §6's rule
    // becomes provable only once both that serialization gap closes AND Phase D's real checkpoint
    // mechanism exists. "Recordings" (016 telemetry) are likewise not built anywhere yet.
    [[nodiscard]] result<void> redact(std::string const& message_id, std::string reason,
                                        std::string actor) {
        for (Message& msg : history_) {
            if (msg.message_id != message_id) continue;

            json::Value tombstone = json::Value::make_object({
                {"reason", json::Value::make_string(std::move(reason))},
                {"actor", json::Value::make_string(std::move(actor))},
            });

            ContentItem item{};
            item.value   = Custom{"ae:redacted", json::dump(tombstone)};
            item.origin  = content_origin::system;
            item.tainted = false;  // host-authored tombstone, not model/tool-originated data

            msg.content.assign(1, item);
            return {};
        }
        return std::unexpected(
            error{failure_class::contract, "no message with that id in history", "session.redact.unknown_message_id"});
    }

    [[nodiscard]] std::string const& session_id() const noexcept { return session_id_; }
    [[nodiscard]] Principal const&   principal() const noexcept { return principal_; }

    // Milestone 5 Phase H2 — how many `StartRun` asks this session has denied at admission (a
    // `caller` was asserted and failed `principal_admitted_for`). Exposed the same way
    // `run_tokens_consumed()` exposes the token-budget accumulator even on its own fail-closed
    // path: the ask itself never resolves (never a hang, per `TestKit::ask`'s documented "failed by
    // reply-before-teardown" path), so a test needs this accessor to observe that denial actually
    // happened rather than some other, unrelated reason for a never-responded ask.
    [[nodiscard]] std::uint64_t admission_denied_count() const noexcept { return admission_denied_count_; }

    // Workflow/agent scratch state (005 §1), checkpointed with the session (019, this milestone's
    // own Phase D/A4) — mutable in place, the same shape a real turn handler needs to accumulate
    // into across turns without replacing the whole object each time.
    [[nodiscard]] StateT&       state() noexcept { return state_; }
    [[nodiscard]] StateT const& state() const noexcept { return state_; }

    // 005 §1's `metadata` — free-form session bookkeeping (e.g. client-supplied tags), distinct
    // from `state` (005 §8 Q1's typed scratch state) precisely because it is NOT schema-typed per
    // agent; a string bag is the right shape for "whatever the caller wants to attach," the same
    // role `Message::metadata` names in 003 §1 for a single message.
    [[nodiscard]] std::unordered_map<std::string, std::string>&       metadata() noexcept { return metadata_; }
    [[nodiscard]] std::unordered_map<std::string, std::string> const& metadata() const noexcept { return metadata_; }

    // The most recently minted `run_id` (001 §1/§2, Phase A3) — exposed for tests and for Phase D's
    // checkpoint records, which need to name the run a checkpoint was taken during.
    [[nodiscard]] std::string const& last_run_id() const noexcept { return last_run_id_; }

    // The `turn_index` of the most recently executed turn (Phase A3/D1) — `effect_context_` isn't
    // reset between `handle()` calls, so right after a turn completes this still holds that turn's
    // own position, exactly what Phase D1's checkpoint content needs to name.
    [[nodiscard]] std::uint64_t last_turn_index() const noexcept { return effect_context_.turn_index; }

    // Milestone 5 Phase F4 (004 §5) -- the current run's accumulated token count, exposed so a test
    // can assert the accumulator's own state directly rather than only through the fail-closed
    // behavior it gates. Reset to 0 at the top of every `handle(Ask<StartRun,...>)` (per-run, not
    // per-session-lifetime); right after a turn completes this still holds that turn's own total,
    // the same "not reset until the next handler runs" property `last_turn_index()` above documents.
    [[nodiscard]] std::uint64_t run_tokens_consumed() const noexcept { return run_tokens_consumed_; }

    // Milestone 4 Phase E1 (001 §2: "Entering InputRequired or AuthRequired mints one or more
    // durable Interaction records"). Host-callable, like fork/redact/delete. ADR-029 (2026-08-11) is
    // the first REAL caller from inside the turn loop itself: `run_rounds()`'s suspend-for-approval
    // branch mints one of these with `interaction_reason::approval` when a pending tool call needs
    // real human sign-off and no synchronous `approval_decider_` is configured. `input`/`auth`
    // (001 §3 step 3b's InputRequired/AuthRequired) stay exactly as unwired as this comment
    // originally said — this task only wires the `approval` reason, the one ADR-029 scoped.
    [[nodiscard]] Interaction const& open_interaction(std::string run_id, interaction_reason reason) {
        interaction_counter_ += 1;
        Interaction interaction{};
        interaction.interaction_id = session_id_ + ":interaction:" + std::to_string(interaction_counter_);
        interaction.run_id         = std::move(run_id);
        interaction.reason         = reason;
        open_interactions_.push_back(std::move(interaction));
        return open_interactions_.back();
    }

    // 001 §2: "A run does not leave InputRequired/Suspended for its 'waiting' reason until every
    // Interaction a given resolution call names is resolved." Fails closed on an unknown id rather
    // than silently no-op'ing, matching `redact()`'s own precedent for "the id you named isn't
    // here."
    [[nodiscard]] result<void> resolve_interaction(std::string const& interaction_id) {
        auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                                [&](Interaction const& i) { return i.interaction_id == interaction_id; });
        if (it == open_interactions_.end()) {
            return std::unexpected(error{failure_class::contract, "no open interaction with that id",
                                          "session.resolve_interaction.unknown_id"});
        }
        open_interactions_.erase(it);
        return {};
    }

    [[nodiscard]] std::vector<Interaction> const& open_interactions() const noexcept {
        return open_interactions_;
    }
    [[nodiscard]] bool has_open_interactions() const noexcept { return !open_interactions_.empty(); }

    // Phase A4's narrowed durable record, extended in Phase D1 with run position (see
    // `AgentSessionRecord`'s own comment for exactly what is and isn't covered) —
    // `to_record()`/`restore_from_record()` are the only two places that cross between the
    // in-process type and its durable shape, so the field list can't drift between them silently.
    [[nodiscard]] AgentSessionRecord to_record() const {
        AgentSessionRecord rec;
        rec.session_id           = session_id_;
        rec.principal_id         = principal_.id;
        rec.principal_tenant_id  = principal_.tenant_id;
        rec.created_at_ns        = static_cast<std::int64_t>(created_at_.time_since_epoch().count());
        rec.updated_at_ns        = static_cast<std::int64_t>(updated_at_.time_since_epoch().count());
        rec.run_counter          = run_counter_;
        rec.turn_index           = effect_context_.turn_index;
        rec.open_interactions    = open_interactions_;
        return rec;
    }

    void restore_from_record(AgentSessionRecord const& rec) {
        session_id_ = rec.session_id;
        // Milestone 5 Phase H1: `AgentSessionRecord` (Phase A4, pre-dating H1) only flattens
        // `id`/`tenant_id` -- `kind`/`on_behalf_of`/`delegation_depth` are NOT round-tripped through
        // the durable record, same narrowing category as this record's own already-named gaps
        // (history/state/capability-set). A restored session's principal is always reconstructed
        // with the default `kind` (`anonymous`) and no delegation info, regardless of what it
        // actually was before the checkpoint -- named here, not silently assumed complete.
        principal_  = Principal{rec.principal_id, rec.principal_tenant_id};
        created_at_ = std::chrono::system_clock::time_point{
            std::chrono::system_clock::duration{rec.created_at_ns}};
        updated_at_ = std::chrono::system_clock::time_point{
            std::chrono::system_clock::duration{rec.updated_at_ns}};

        // Phase D1: restoring run position is what keeps a post-restore session's NEXT StartRun
        // from reminting a run_id that collides with one that already happened before whatever
        // this checkpoint survived (001 §1's "every Run gets a fresh id").
        run_counter_ = rec.run_counter;
        last_run_id_ = run_counter_ > 0 ? session_id_ + ":run:" + std::to_string(run_counter_)
                                          : std::string{};
        effect_context_.turn_index = rec.turn_index;
        open_interactions_ = rec.open_interactions;
    }

    // Milestone 4 Phase C3 (005 §6: "Delete — hard removal, including derived artifacts... with a
    // completion receipt"). The in-process half of delete: every field this session holds is reset
    // to its own fresh-construction default, including `session_id_`/`principal_` — a deleted
    // session has no residue left to read back through ANY of this class's own accessors, not just
    // history/state/metadata. Called by `delete_session()` (below), which captures `session_id()`
    // for the receipt BEFORE calling this.
    void clear_in_process_state() {
        session_id_.clear();
        principal_       = Principal{};
        history_.clear();
        state_           = StateT{};
        metadata_.clear();
        run_counter_     = 0;
        last_run_id_.clear();
        effect_context_  = EffectContext{};
        created_at_      = std::chrono::system_clock::time_point{};
        updated_at_      = std::chrono::system_clock::time_point{};
        open_interactions_.clear();
        interaction_counter_ = 0;
        timer_wakes_     = 0;
        // Phase F4: this function's own contract above is "every field... reset to its own
        // fresh-construction default" -- taken literally for both the configured ceiling and its
        // accumulator, matching every other field in this list.
        token_budget_         = std::nullopt;
        run_tokens_consumed_  = 0;
        admission_denied_count_ = 0;
        // Same "configured ceiling" category as `token_budget_` immediately above -- reset to its
        // own fresh-construction default (unbounded, see initialize()'s own comment), not left at
        // whatever the deleted session had configured.
        max_turns_            = std::nullopt;
        // ADR-028 addendum: this function's own contract is "no residue left to read back through
        // ANY of this class's own accessors" (005 §6's "hard removal" promise) -- since ADR-028, a
        // `HistoryProviderT` MAY hold real session-scoped data (a stateful tool's accumulated
        // state), which would otherwise survive a "delete" as exactly the kind of readable residue
        // this method exists to eliminate. Reset here for the same reason `state_` above already is.
        history_provider_    = HistoryProviderT{};
    }

private:
    // Milestone 7 Phase A (013 §1: "Ordered and monotonic per run, with a sequence number"). A
    // no-op before `enable_event_stream()` is called -- exactly `WorkflowSupervisor`'s own
    // `live_view_producer_.valid()` guard (M6 Phase G, supervisor.hpp). Convenience overload for the
    // common case: emit against THIS handler's own current run.
    void emit_run_event(run_event_kind kind, RunEventPayload payload = run_event_payload::Empty{}) {
        emit_run_event_for(effect_context_.run_id, kind, std::move(payload));
    }

    // Milestone 7 Phase B fix to Phase A's own design: the sequence counter is keyed PER run_id
    // (`run_event_seq_by_run_`), not a single scalar reset at the top of `handle()`. A single scalar
    // would have been correct for every Phase A call site (all fire synchronously within the SAME
    // `handle()` invocation that owns the current run), but Phase B's `BackgroundTaskDone` handler
    // can fire an event for an OLDER run_id after a NEWER `StartRun` has already reset the counter --
    // with a shared scalar, that stale event would collide with the newer run's own seq numbers
    // (both incrementing the same counter). A map isolates each run_id's own monotonic-from-1
    // subsequence regardless of how many other runs started in between -- proven unchanged for the
    // ordinary case (a fresh run_id's first lookup still starts at 0, so its first emitted event is
    // still seq 1, exactly Phase A's own A2/A3 assertions).
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

    // Engaged when `ChatClientT` is default-constructible (every pre-ADR-018 conformer), empty
    // otherwise -- which is precisely what keeps `AgentSession` itself default-constructible, and so
    // usable under `quark::TestKit<A>`, for a backend that is not.
    [[nodiscard]] static std::optional<ChatClientT> make_default_chat_client() {
        if constexpr (std::is_default_constructible_v<ChatClientT>) {
            return std::optional<ChatClientT>(std::in_place);
        } else {
            return std::optional<ChatClientT>{};
        }
    }

    std::string                                       session_id_;
    Principal                                          principal_;
    std::vector<Message>                               history_;
    StateT                                             state_{};
    std::unordered_map<std::string, std::string>       metadata_;
    std::uint64_t                                      run_counter_ = 0;
    std::string                                        last_run_id_;
    std::vector<Interaction>                           open_interactions_;
    std::uint64_t                                      interaction_counter_ = 0;
    // Milestone 7 Phase B (006 §6b) -- currently outstanding StandingEffects (today: only
    // `background_task` registrations; `standing_effect_counter_` mints each `handle_id`'s numeric
    // suffix, the same "session_id + a monotonic counter" shape `interaction_counter_` already has).
    std::vector<StandingEffect>                        standing_effects_;
    std::uint64_t                                      standing_effect_counter_ = 0;
    std::uint64_t                                      timer_wakes_ = 0;
    // Milestone 5 Phase J1 residual, closed by ADR-018: held as an `optional` ONLY so that
    // `AgentSession` stays default-constructible when `ChatClientT` is not.
    //
    // `quark::TestKit<A>` declares `A actor_;` and default-constructs it, and a real Phase D/E
    // backend is not default-constructible -- `OpenAIChatClient<Store>` holds a `Store const&`. So
    // `AgentSession<OpenAIChatClient<...>>` did not merely fail to be CONFIGURABLE, it failed to
    // COMPILE under TestKit at all, which is why no real backend had ever been driven through this
    // turn loop. See `emplace_chat_client()` below.
    //
    // Default-ENGAGED whenever `ChatClientT` allows it, so every existing conformer (all of which are
    // default-constructible mocks) behaves exactly as it did when this was a plain value member: the
    // engaged check below can only fail for a type that could not have been a value member anyway.
    std::optional<ChatClientT>                         chat_client_ = make_default_chat_client();
    // ADR-018. Non-owning; null means "this session may reach no effect", which is both the default
    // and the pre-ADR-018 behaviour. Configuration, so -- like `chat_client_`/`history_provider_` --
    // deliberately NOT cleared by `reset()`.
    CapabilitySet const*                               capabilities_ = nullptr;
    HistoryProviderT                                    history_provider_;
    EffectContext                                      effect_context_;
    std::chrono::system_clock::time_point              created_at_{};
    std::chrono::system_clock::time_point              updated_at_{};
    // Milestone 5 Phase F4 (004 §5) -- the configured per-run ceiling (nullopt = unbounded, set via
    // `initialize()`) and this run's running total (reset at the top of every `handle()`).
    std::optional<std::uint64_t>                       token_budget_;
    std::uint64_t                                      run_tokens_consumed_ = 0;
    // The internal tool-call loop's own bound (`handle()`) -- configured via `initialize()`/
    // `set_max_turns()`, same "configured ceiling" category as `token_budget_` immediately above.
    // Defaults unbounded (nullopt) -- see `initialize()`'s own comment for the deliberate tradeoff.
    std::optional<std::uint64_t>                        max_turns_;
    // The synchronous approval gate every round's `invoke_tool` call receives -- empty (fail-closed)
    // by default, configured via `set_approval_decider()`.
    ApprovalDecider                                     approval_decider_{};
    // ADR-029 -- opts a session into suspending for a real human answer instead of `invoke_tool`'s
    // ordinary fail-closed deny; see `set_suspend_for_approval()`'s own comment. Default false.
    bool                                                 suspend_for_approval_ = false;
    // ADR-034 -- opts a session into streaming each round's model call; see
    // `set_stream_model_calls()`'s own comment for the real tradeoffs. Default false.
    bool                                                 stream_model_calls_ = false;
    // ADR-035 Phase 1 -- opts a session into `apply_response_format_scan()` inside
    // `run_model_call()`, backend-agnostically; see `set_scan_response_format_leaks()`'s own
    // comment. Default false (ADR-023 Finding 6: operator-armed, never content-triggered).
    bool                                                 scan_response_format_leaks_ = false;
    // Milestone 5 Phase H2 (018 §2) -- how many `StartRun` asks this session has denied at
    // admission. Same "counter the caller/tests can observe even on the fail-closed path" shape as
    // `run_tokens_consumed_` immediately above it.
    std::uint64_t                                      admission_denied_count_ = 0;
    // Milestone 7 Phase A (013 §1) -- invalid until `enable_event_stream()` is called, the same
    // "invalid until enabled" shape `WorkflowSupervisor::live_view_producer_` already established
    // (M6 Phase G). Milestone 7 Phase B: keyed per run_id (`emit_run_event_for()`'s own comment) --
    // 013 §1's sequence number is monotonic PER RUN, never across this actor's whole lifetime, and
    // never colliding with a DIFFERENT run's own numbering even when a background completion for an
    // older run arrives after a newer run has already started.
    stream_producer<RunEvent>                           run_event_producer_;
    std::unordered_map<std::string, std::uint64_t>       run_event_seq_by_run_;
};

// Save `session`'s narrowed durable record (Phase A4, extended with run position in Phase D1)
// under its own `session_actor_id()`, at the consistent point `quiesce(Drain)` reaches on a
// Sequential actor — mirrors Quark's own `persistence_snapshot_roundtrip_test.cpp` calling
// convention exactly: `through_seq` is fixed at 0 because this is the pure Snapshot model with no
// event log to subsume (matching that test's own comment, "through_seq=0 (Snapshot model, no
// log)"). `act` is the caller's `Activation` — a `TestKit<AgentSession<...>>` or a real `Engine`
// owns it, `AgentSession` itself does not, the same division of ownership `snapshot_sequential`
// already assumes for every Sequential actor.
//
// Milestone 4 Phase D1 (019 §1): calling this right after a turn completes IS "taking a
// checkpoint" at this milestone's scope — 019 §1's own text says checkpoint storage is "the Quark
// 012 Store seam, shared with sessions (005 §2). No second persistence engine," which this project
// takes literally rather than adding a parallel `TurnCheckpoint` schema: one record, one save
// path, now carrying the run's position alongside session identity. `checkpoint_if_due()` (below,
// Phase D2) is the cadence-gated way a host would normally call this at a turn boundary; nothing
// stops calling it directly, unconditionally, the way A4's own tests already do.
template <class ChatClientT, class StateT, class HistoryProviderT, quark::Store S>
[[nodiscard]] quark::result<void> save_agent_session_snapshot(
    quark::Activation& act, S& store, AgentSession<ChatClientT, StateT, HistoryProviderT> const& session,
    quark::FenceToken fence) {
    return quark::snapshot_sequential<AgentSessionRecord>(
        act, store, session_actor_id(session.session_id()), fence, /*through_seq=*/0,
        session.to_record());
}

// Load the latest durable record for `session_id`, or `std::nullopt` if it was never snapshotted
// (012 §Recovery's "a fresh actor reconstructs from its factory instead" — never an error), OR if
// it was deleted (Phase C3's tombstone, 005 §6): a caller of THIS function sees no distinction
// between "never existed" and "deleted" — which is exactly the "no residue" property 005 §6 asks
// for at this project's own read path. `delete_session()` (below) is where the distinction is
// still observable, via its receipt, at the moment of deletion itself.
template <quark::Store S>
[[nodiscard]] quark::result<std::optional<AgentSessionRecord>> load_agent_session_snapshot(
    S& store, std::string_view session_id) {
    auto rec = quark::load_snapshot<AgentSessionRecord>(store, session_actor_id(session_id));
    if (!rec) return std::unexpected(rec.error());
    if (!rec->has_value()) return std::optional<AgentSessionRecord>{};
    if ((*rec)->state.deleted) return std::optional<AgentSessionRecord>{};
    return std::optional<AgentSessionRecord>{std::move((*rec)->state)};
}

// Milestone 4 Phase D2 (019 §1: "Cost is bounded: incremental deltas plus periodic full
// checkpoints, with the cadence a policy"). This milestone's checkpoint content
// (`AgentSessionRecord`, Phase D1) has no meaningful incremental-vs-full distinction to draw yet —
// it is already a small, flat record with nothing larger to diff against (the same
// Message/ContentItem serialization gap `AgentSessionRecord`'s own comment names). What IS real
// and provable now is the OTHER half of "cost is bounded": not writing to the `Store` on every
// single turn. `CheckpointCadence<N>` (this project's CRTP-policy idiom, matching
// `MaxTurns<N>`/`TokenBudget<N>`) answers "is a checkpoint due" as a pure function of how many
// turns have completed since the last one actually written — `N == 1` checkpoints every turn (A4's
// own until-now-implicit default); `N > 1` skips `N - 1` writes between checks, the cheaper
// "incremental" side of 019 §1's own cadence language, even though what's skipped isn't a
// content-shaped delta yet, just the write itself.
template <std::uint32_t EveryNTurns>
    requires(EveryNTurns >= 1)
struct CheckpointCadence {  // ae-naming-lint: allow CheckpointCadence — 019 §1 names "cadence" normatively; 027 has not been updated to list this policy type
    [[nodiscard]] static constexpr bool due(std::uint64_t turns_since_last_checkpoint) noexcept {
        return turns_since_last_checkpoint >= EveryNTurns;
    }
};

// The cadence-gated way a host calls `save_agent_session_snapshot()` at a turn boundary:
// `turns_since_last_checkpoint` is the CALLER's own count (this function does no bookkeeping of
// its own — `AgentSession` has no ambient `Store` access, I2, so nothing here can track "since
// when" on its behalf) — the caller increments it after every completed turn and resets it to 0
// whenever this returns `true`. Returns `false` (not an error) when the cadence skips a write;
// `result<bool>` still surfaces a real `Store` failure on the turns that DO attempt one.
template <class CadenceT, class ChatClientT, class StateT, class HistoryProviderT, quark::Store S>
[[nodiscard]] quark::result<bool> checkpoint_if_due(
    quark::Activation& act, S& store, AgentSession<ChatClientT, StateT, HistoryProviderT> const& session,
    quark::FenceToken fence, std::uint64_t turns_since_last_checkpoint) {
    if (!CadenceT::due(turns_since_last_checkpoint)) return false;
    auto saved = save_agent_session_snapshot(act, store, session, fence);
    if (!saved) return std::unexpected(saved.error());
    return true;
}

// Milestone 4 Phase E4 (019 §4: "Poison runs: a run that fails repeatedly on resume is
// quarantined after a bounded number of attempts, with its state preserved for inspection — not
// retried forever, and not discarded"). Host-side bookkeeping, the same "pure function of a
// counter the CALLER maintains" shape `CheckpointCadence<N>` already uses above — `AgentSession`
// itself has no notion of "this run keeps failing on resume" (that is a property of repeated
// EXTERNAL retry attempts a host makes, not something the actor tracks about its own single
// `handle()` call). "State preserved" is true by simple absence: nothing in this policy, or in
// anything that would call it, ever touches `clear_in_process_state()`/`delete_session()` —
// quarantining is a HOST-side decision to stop retrying, never an AgentSession-side deletion.
template <std::uint32_t MaxAttempts>
    requires(MaxAttempts >= 1)
struct PoisonRunPolicy {  // ae-naming-lint: allow PoisonRunPolicy — 019 §4 names "poison run" normatively; 027 has not been updated to list this policy type
    [[nodiscard]] static constexpr bool is_quarantined(std::uint32_t consecutive_failures) noexcept {
        return consecutive_failures >= MaxAttempts;
    }
};

// Milestone 4 Phase C3 (005 §6: "Delete — hard removal... with a completion receipt"). Not a
// gate-numbered claim in 005 §7 (only redaction's G4 is) — §6's own prose is the whole
// specification for this operation, and this is exactly what it asks for at this project's own
// seams: the durable snapshot becomes an explicit tombstone (so `load_agent_session_snapshot`
// reports nothing, "no residue" through this project's own read path) and the in-process actor's
// own state is cleared to fresh-construction defaults (`clear_in_process_state()`) — a receipt
// naming which of the two actually happened, since either can independently fail.
struct SessionDeletionReceipt {
    std::string session_id;
    bool        durable_record_removed = false;
    bool        in_process_state_cleared = false;
};

template <class ChatClientT, class StateT, class HistoryProviderT, quark::Store S>
[[nodiscard]] quark::result<SessionDeletionReceipt> delete_session(
    quark::Activation& act, S& store, AgentSession<ChatClientT, StateT, HistoryProviderT>& session,
    quark::FenceToken fence) {
    SessionDeletionReceipt receipt{};
    receipt.session_id = session.session_id();

    AgentSessionRecord tombstone{};
    tombstone.session_id = receipt.session_id;
    tombstone.deleted    = true;
    auto saved = quark::snapshot_sequential<AgentSessionRecord>(
        act, store, session_actor_id(receipt.session_id), fence, /*through_seq=*/0, tombstone);
    if (!saved) return std::unexpected(saved.error());
    receipt.durable_record_removed = true;

    session.clear_in_process_state();
    receipt.in_process_state_cleared = true;

    return receipt;
}

} // namespace agentengine
