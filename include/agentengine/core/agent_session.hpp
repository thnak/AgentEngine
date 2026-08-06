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

#include <algorithm>
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
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"
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
    requires ChatClient<ChatClientT> && ContextProvider<HistoryProviderT>
class AgentSession
    : public quark::Actor<AgentSession<ChatClientT, StateT, HistoryProviderT>, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<StartRun, AgentResponse>, TimerWake>;

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

        // Milestone 4 Phase B2: what the model sees is now derived through a real
        // `ContextProvider` (005 §3/§5) instead of `ChatRequest{history_}` directly — still exactly
        // one contributor (`HistoryProviderT`) for now, not yet the fully general N-contributor
        // assembly (budgets/drop-order, Phase B3's own standalone `assemble_context()`); that
        // generalization is deferred to Phase G, when a second real provider (memory) actually
        // needs to be composed alongside this one (decision 7's build ordering).
        SessionContext session_ctx{session_id_, principal_, history_};
        result<ContextContribution> contribution = history_provider_.on_context(session_ctx, effect_context_);
        if (!contribution) {
            // Same fail-closed shape as the chat-failure branch below — never fabricate a context.
            return;
        }

        ChatRequest request{contribution->messages};
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
        history_provider_.on_turn_end(effect_context_);
        m.respond(AgentResponse{response->message, response->usage});
    }

    // Milestone 4 Phase E3: the tell a fired Quark reminder delivers (see `TimerWake`'s own
    // comment). No coroutine/suspend semantics attach to this — it is a plain, synchronous
    // acknowledgement, matching the fact that nothing in this handler ever left this actor mid-run
    // to begin with.
    void handle(TimerWake const&) noexcept { ++timer_wakes_; }

    [[nodiscard]] std::uint64_t timer_wakes() const noexcept { return timer_wakes_; }

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

        run_counter_ = 0;
        last_run_id_.clear();
        effect_context_ = EffectContext{};
        created_at_      = std::chrono::system_clock::time_point{};
        updated_at_      = std::chrono::system_clock::time_point{};
        open_interactions_.clear();
        interaction_counter_ = 0;
        timer_wakes_ = 0;
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

    // Milestone 4 Phase E1 (001 §2: "Entering InputRequired or AuthRequired mints one or more
    // durable Interaction records"). Host-callable, like fork/redact/delete — NOT wired into the
    // synchronous turn loop: 001 §3's step 3b ("request approval if policy demands it — may →
    // InputRequired") would be the real trigger, but that needs `ae::task<T>` coroutines to
    // actually suspend a run mid-turn, which stays unwired project-wide (chat_client.hpp/
    // tool_pipeline.hpp/sandbox's own runner.hpp all say the same thing: deferred past this
    // milestone). What IS real here: minting, tracking, and resolving `Interaction` records with
    // the exact correlation identity 001 §2 specifies — the vocabulary and lifecycle, proven
    // standalone, the same relationship Phase A1's `session_actor_id()` had to real addressing
    // before anything called it from a live turn loop.
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
    }

private:
    std::string                                       session_id_;
    Principal                                          principal_;
    std::vector<Message>                               history_;
    StateT                                             state_{};
    std::unordered_map<std::string, std::string>       metadata_;
    std::uint64_t                                      run_counter_ = 0;
    std::string                                        last_run_id_;
    std::vector<Interaction>                           open_interactions_;
    std::uint64_t                                      interaction_counter_ = 0;
    std::uint64_t                                      timer_wakes_ = 0;
    ChatClientT                                        chat_client_;
    HistoryProviderT                                    history_provider_;
    EffectContext                                      effect_context_;
    std::chrono::system_clock::time_point              created_at_{};
    std::chrono::system_clock::time_point              updated_at_{};
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
