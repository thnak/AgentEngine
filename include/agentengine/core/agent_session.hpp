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
// M1 scope only (docs/planning/milestone-1-core-substrate-breakdown.md task 3): `handle()`
// implements 001 §3's turn loop in miniature — assemble context (the full history, trivially),
// call the provider, append the response. Steps 3a-3c (tool-call resolution, approval, invocation)
// don't apply with no tools declared yet (006/002 land in Milestone 2); policy resolution,
// checkpointing (019), and real timestamps (which would be an unrecorded wall-clock read, 001 §7 —
// premature before Clock is a wired capability) are deliberately not touched here.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/describe.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/snapshot.hpp"

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
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
struct StartRun {  // ae-naming-lint: allow StartRun — 001 §1 names this message type normatively; 027 has not been updated to list it (same tracked-gap category as the M0 backlog)
    Message input;  // the new turn to append to history and process (001 §3 step 1)
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

    friend bool operator==(AgentSessionRecord const&, AgentSessionRecord const&) = default;
};
QUARK_SERIALIZE(AgentSessionRecord, (1, session_id), (2, principal_id), (3, principal_tenant_id),
                 (4, created_at_ns), (5, updated_at_ns))

template <class ChatClientT, class StateT = NoSessionState>
    requires ChatClient<ChatClientT>
class AgentSession : public quark::Actor<AgentSession<ChatClientT, StateT>, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<StartRun, AgentResponse>>;

    // 001 §3's turn loop, in miniature. Synchronous: `ChatClientT::chat()` is itself synchronous at
    // this milestone (chat_client.hpp: `ae::task<T>` not wired in yet), so no coroutine is needed to
    // run a real one-turn run end to end.
    void handle(quark::Ask<StartRun, AgentResponse> const& m) {
        // 001 §1: "Run: An Ask<StartRun, RunResponse> to the session actor" — literally, one run
        // per StartRun ask. `run_id` is minted here, deterministically, from the session's own
        // monotonic counter (never wall-clock — 001 §7/I5), the identity 019 §3's idempotency-key
        // derivation and this milestone's own Phase D checkpoint records both need.
        run_counter_ += 1;
        effect_context_.run_id     = session_id_ + ":run:" + std::to_string(run_counter_);
        // 001 §2: "Turn: A segment of a run's coroutine between model calls." This milestone's
        // turn loop (still M1's own scope: no tool-call loop, one model call per run) makes exactly
        // one model call per run, so `turn_index` is always 0 here — real identity, not a fake
        // placeholder, for the one turn that actually executes; a future multi-turn tool-calling
        // loop would increment it once per additional model call within the SAME run, which is
        // exactly what this field exists for. That loop itself is a separate, un-named gap (001 §3
        // steps 3a-3c / 006's real tool pipeline are not wired into this handler at all) — not
        // built here, named rather than silently assumed done.
        effect_context_.turn_index = 0;
        last_run_id_ = effect_context_.run_id;

        history_.push_back(m.query.input);

        ChatRequest request{history_};
        result<ChatResponse> response = chat_client_.chat(request, effect_context_);
        if (!response) {
            // 001 §6's failure classification isn't wired up at this milestone — fail closed by
            // never responding, rather than fabricating a placeholder AgentResponse. The caller's
            // ask then resolves however the reply-cell's own "never replied" path surfaces it
            // (quark/core/testkit.hpp: "failed by reply-before-teardown if the handler never
            // replied — never a hang").
            return;
        }

        history_.push_back(response->message);
        m.respond(AgentResponse{response->message, response->usage});
    }

    [[nodiscard]] std::vector<Message> const& history() const noexcept { return history_; }

    // `quark::TestKit<A>`/a real `Engine` both default-construct the actor and hand out a mutable
    // `A&` for post-construction wiring (testkit.hpp:83, "state access for wiring + assertions") —
    // there is no constructor-argument-forwarding path through either, so `session_id`/`principal`
    // are set this way, matching how the mock `ChatClient`'s own canned behavior is already baked
    // in by construction rather than passed through TestKit. Named `initialize`, the same verb this
    // project already uses for "make an object ready to run, before capabilities/effects flow"
    // (`MediatedPythonRunner::initialize()`).
    void initialize(std::string session_id, Principal principal) {
        session_id_ = std::move(session_id);
        principal_  = std::move(principal);
    }

    [[nodiscard]] std::string const& session_id() const noexcept { return session_id_; }
    [[nodiscard]] Principal const&   principal() const noexcept { return principal_; }

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

    // Phase A4's narrowed durable record (see `AgentSessionRecord`'s own comment for exactly what
    // is and isn't covered) — `to_record()`/`restore_from_record()` are the only two places that
    // cross between the in-process type and its durable shape, so the field list can't drift
    // between them silently.
    [[nodiscard]] AgentSessionRecord to_record() const {
        return AgentSessionRecord{
            session_id_,
            principal_.id,
            principal_.tenant_id,
            static_cast<std::int64_t>(created_at_.time_since_epoch().count()),
            static_cast<std::int64_t>(updated_at_.time_since_epoch().count()),
        };
    }

    void restore_from_record(AgentSessionRecord const& rec) {
        session_id_ = rec.session_id;
        principal_  = Principal{rec.principal_id, rec.principal_tenant_id};
        created_at_ = std::chrono::system_clock::time_point{
            std::chrono::system_clock::duration{rec.created_at_ns}};
        updated_at_ = std::chrono::system_clock::time_point{
            std::chrono::system_clock::duration{rec.updated_at_ns}};
    }

private:
    std::string                                       session_id_;
    Principal                                          principal_;
    std::vector<Message>                               history_;
    StateT                                             state_{};
    std::unordered_map<std::string, std::string>       metadata_;
    std::uint64_t                                      run_counter_ = 0;
    std::string                                        last_run_id_;
    ChatClientT                                        chat_client_;
    EffectContext                                      effect_context_;
    std::chrono::system_clock::time_point              created_at_{};
    std::chrono::system_clock::time_point              updated_at_{};
};

// Save `session`'s narrowed durable record (Phase A4) under its own `session_actor_id()`, at the
// consistent point `quiesce(Drain)` reaches on a Sequential actor — mirrors Quark's own
// `persistence_snapshot_roundtrip_test.cpp` calling convention exactly: `through_seq` is fixed at
// 0 because this is the pure Snapshot model with no event log to subsume (matching that test's own
// comment, "through_seq=0 (Snapshot model, no log)"). `act` is the caller's `Activation` — a
// `TestKit<AgentSession<...>>` or a real `Engine` owns it, `AgentSession` itself does not, the same
// division of ownership `snapshot_sequential` already assumes for every Sequential actor.
template <class ChatClientT, class StateT, quark::Store S>
[[nodiscard]] quark::result<void> save_agent_session_snapshot(
    quark::Activation& act, S& store, AgentSession<ChatClientT, StateT> const& session,
    quark::FenceToken fence) {
    return quark::snapshot_sequential<AgentSessionRecord>(
        act, store, session_actor_id(session.session_id()), fence, /*through_seq=*/0,
        session.to_record());
}

// Load the latest durable record for `session_id`, or `std::nullopt` if it was never snapshotted
// (012 §Recovery's "a fresh actor reconstructs from its factory instead" — never an error).
template <quark::Store S>
[[nodiscard]] quark::result<std::optional<AgentSessionRecord>> load_agent_session_snapshot(
    S& store, std::string_view session_id) {
    auto rec = quark::load_snapshot<AgentSessionRecord>(store, session_actor_id(session_id));
    if (!rec) return std::unexpected(rec.error());
    if (!rec->has_value()) return std::optional<AgentSessionRecord>{};
    return std::optional<AgentSessionRecord>{std::move((*rec)->state)};
}

} // namespace agentengine
