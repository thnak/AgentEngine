#pragma once
// ADR-199 (issue #115 E3): the part of `rt::AgentSession<ChatClientT, StateT, HistoryProviderT>` that does not
// depend on its template parameters -- the turn loop, approval/hook/CodeAct resume, admission, events and
// bookkeeping -- as one non-template class compiled once, in src/rt/agent_session_core.cpp. The template
// (agent_session.hpp) owns the bound chat client, history provider and state and reaches the core's loop
// through the private hooks declared below. agent_session.hpp's banner documents the class as a whole.

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

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/system_channel_fence.hpp"  // is_automatic_approval_id (ADR-191 round 4)
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

// ADR-196 (issue #104): one call's decision within an `approval` interaction.
struct ApprovalCallDecision {  // ae-naming-lint: allow ApprovalCallDecision — ADR-196
    std::string call_id;
    bool        approved = false;
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
    // ADR-196 (issue #104): a decision per call, for an `approval` interaction. Each names a call this interaction
    // asked about (an `approval_requested` call id); a call it does not name takes `approved` above. Naming a call
    // the interaction did not ask about, or one call twice, is refused and the interaction stays open.
    std::optional<std::vector<ApprovalCallDecision>> call_decisions = std::nullopt;
    // ADR-196 (issue #108): who decided -- host-supplied, never derived from model output (I3), and carried on every
    // `approval_resolved` this resolve emits (I4). Unset means an anonymous decision, recorded as such (the event's
    // `approver_id` is empty). When set it must be non-blank with no control characters.
    std::optional<std::string> approver_id = std::nullopt;
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
    // Round-3 red team (ADR-192): the operator whose unattended approval let the original `execute_code` run, when
    // one did. The replay re-runs the whole script, so it is then re-checked against the session's CURRENT
    // setting -- clearing unattended mode (or a veto, or a deny policy) while the script waits on its question
    // must stop the replay too. Unset: the original approval came from a human or the host's own decider.
    std::optional<std::string> approved_unattended_by{};
};

// ADR-196: what a suspended round showed the model and which of its calls actually waited on a decision -- kept per
// open interaction, dropped with it. A resume dispatches against exactly `offered_tools` (round-3 red team: a resume
// used to rebuild the tool list from the raw provider, bringing back tools the host's turn middleware had removed), and
// an `approval` resume asks about exactly `gated_call_ids` (issue #104 BUG-1).
//
// ADR-196 §7 (red team round 1, issue #111): the record is the ONLY thing an interaction resolves against.
//   - `offered_tools` holds the descriptors themselves, as the host's turn middleware left them -- approval mode,
//     invoke, everything (A4: keeping only names lost a middleware's tightening or wrapped invoke on resume).
//   - `message_index` names the suspended assistant message in `history_`; a resolve checks it is still the tail.
//   - An interaction with no record (one restored from an `AgentSessionRecord`, which carries none) is closed with
//     nothing run -- it never resolves against whatever the history happens to hold (A1).
struct SuspendedRoundRecord {  // ae-naming-lint: allow SuspendedRoundRecord — ADR-196
    std::vector<ToolDescriptor> offered_tools;
    std::vector<std::string>    gated_call_ids;
    std::size_t                 message_index = 0;
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
    auto const* const first = reinterpret_cast<std::byte const*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

[[nodiscard]] inline result<AgentSessionRecord> decode_agent_session_record(
    std::vector<std::byte> const& bytes) {
    std::string const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
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

// ADR-199: how the bound chat client routes a model call -- read by start_run()'s warnings and should_retry_stream(),
// which used to branch on the client's type with `if constexpr`.
enum class model_route : std::uint8_t {  // ae-naming-lint: allow model_route — ADR-199
    direct,             // a ChatClient: chat() / chat_stream()
    gateway,            // a ModelCallGateway without call_stream()
    gateway_streaming,  // a ModelCallGateway with call_stream()
};

// ADR-199: the template (agent_session.hpp) is the one class that reaches the core's private members and overrides its
// hooks. Declared here so the core can befriend it; the defaults are on the definition.
template <class ChatClientT, class StateT, class HistoryProviderT>
    requires (agentengine::ChatClient<ChatClientT> || agentengine::ModelCallGatewayLike<ChatClientT>) &&
             agentengine::ContextProvider<HistoryProviderT>
class AgentSession;

// ae-naming-lint: allow AgentSessionCore — ADR-199
class AgentSessionCore {
    template <class ChatClientT, class StateT, class HistoryProviderT>
        requires (agentengine::ChatClient<ChatClientT> || agentengine::ModelCallGatewayLike<ChatClientT>) &&
                 agentengine::ContextProvider<HistoryProviderT>
    friend class AgentSession;

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


    void set_suspend_for_approval(bool suspend) noexcept { suspend_for_approval_ = suspend; }
    [[nodiscard]] bool suspend_for_approval() const noexcept { return suspend_for_approval_; }

    void set_stream_model_calls(bool stream) noexcept { stream_model_calls_ = stream; }
    [[nodiscard]] bool stream_model_calls() const noexcept { return stream_model_calls_; }

    // ADR-177: how many times, in total over ONE run, a model call whose stream died mid-answer is
    // re-issued against the same client. Default 0 == today's behaviour exactly. Clamped to
    // `kMaxStreamRetries` so a caller cannot configure an unbounded loop. Never applies to a
    // `ModelCallGateway` session (its own commit rule stands) -- see `should_retry_stream()`.
    static constexpr std::uint32_t kMaxStreamRetries = 3;
    void set_stream_retries(std::uint32_t n) noexcept {
        stream_retries_ = n > kMaxStreamRetries ? kMaxStreamRetries : n;
    }
    [[nodiscard]] std::uint32_t stream_retries() const noexcept { return stream_retries_; }
    // How many model calls THIS run discarded and re-issued. The token budget cannot see a dead
    // attempt's tokens (a failed stream reports no `Usage`, and inventing one would put a fabricated
    // number into a budget), so the COUNT is what a host has to apply its own estimate to.
    // docs/research/2026-09-21-billing-of-interrupted-streams.md: whether providers bill such an
    // attempt is not established.
    [[nodiscard]] std::uint32_t stream_retries_used() const noexcept { return stream_retries_used_; }
    // The ESTIMATED tokens this run's token budget was charged for discarded attempts (already inside
    // `run_tokens_consumed()`, and NOT inside `run_usage()`, which reports only what a provider said).
    [[nodiscard]] std::uint64_t discarded_tokens_estimate() const noexcept { return discarded_tokens_estimate_; }

    // ADR-178: ask the run this session is executing (or is suspended in) to stop. Callable from ANY thread,
    // and never blocks on the run -- it only requests stop on the current run's `std::stop_source`.
    // COOPERATIVE at named checkpoints, never preemptive: the top of each round, inside a streaming model
    // call (it abandons the stream and so reaches the transport's blocking read), and after a model call
    // returns but BEFORE its tool calls run. A tool sees the same signal as `EffectContext::cancellation`.
    // The run ends `run.canceled` (`failure_class::fatal`) after a `run_canceled` event; nothing the
    // canceled round had not yet committed is appended to history. A no-op when no run is in flight: the
    // next `start_run()` gets a FRESH source, so a stale cancel can never poison it.
    void cancel() noexcept;
    [[nodiscard]] std::stop_token cancellation_token() const noexcept {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        return cancel_source_.get_token();
    }

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

    // ADR-191, host opt-in: a lesson a human approved (the host's `ApprovedLessonRegistry`, scoped to this
    // session's principal) reaches the model as an approved-lesson block -- still tainted and fenced, but the
    // fence's preamble says it may be followed. Unset (the default) or null: every request is built exactly as
    // before. `registry` must outlive the session. Every approved delivery emits a `policy_decision` event naming
    // the approval it rests on (I4).
    //
    void set_approved_lessons(agentengine::ApprovedLessonRegistry const* registry) noexcept {
        approved_lessons_ = registry;
        approved_lesson_level_ = agentengine::approved_lesson_level::guidance;
        lesson_level_set_by_.reset();
    }

    // ADR-192: `level` is how an approved lesson is delivered -- `guidance` (ADR-191's fenced, tagged route, the
    // default) or `instructions` (plain, unfenced system text, as the host's own instructions). The item stays tainted
    // either way; only how the model is told to read it changes. Round-3 red team: `instructions` is one of the four
    // unattended opt-ins, so, like the other three, it names who set it (required, I4) and every delivery it affects
    // says so; before, it took no id and the audit named only the lesson's approver.
    [[nodiscard]] result<void> set_approved_lessons(agentengine::ApprovedLessonRegistry const* registry,
                                                    agentengine::approved_lesson_level level,
                                                    std::string operator_id);

    // ADR-192, host opt-in (unattended mode): every tainted system text -- memory, retrieved documents, summaries,
    // lessons -- goes to the model as plain system text, with no ADR-173 fence and no preamble. For a full-automation
    // deployment that trusts its own stores. The cost is the fence's whole point: text a tool, a document or an
    // earlier model turn wrote can then steer the model like the host's instructions. The text stays tainted (tool
    // calls from it are still `arguments_tainted`, and recordings keep the taint). `operator_id` names who turned it
    // off (required, I4; refused if empty); every request it affects emits one `policy_decision` event saying so. The
    // event reaches the host only through a run-event tap or stream it attached. Off by default.
    // ADR-193: host-supplied messages placed ahead of every request this session builds (after its providers'
    // contributions are assembled, before the delivery marks are granted) -- how a spawned child receives the
    // chain's shared lessons. Host code only; a tainted item stays tainted and is granted nothing it would not
    // otherwise get. Empty by default.
    void set_pinned_context(std::vector<Message> messages) { pinned_context_ = std::move(messages); }

    [[nodiscard]] result<void> disable_system_channel_fence(std::string operator_id) {
        if (!agentengine::is_attributable_id(operator_id)) return unattended_detail_refuse("disable_system_channel_fence");
        system_fence_disabled_by_ = std::move(operator_id);
        return {};
    }
    void enable_system_channel_fence() noexcept { system_fence_disabled_by_.reset(); }
    [[nodiscard]] bool system_channel_fence_enabled() const noexcept { return !system_fence_disabled_by_; }

    // ADR-192, host opt-in (unattended mode): every tool call that would wait for an approval is approved without
    // a human -- `approval_mode::always_require` tools and `text_derived` calls included -- and the session never
    // suspends for approval. It is the `ApprovalDecider` answering yes, so it decides only among authority the host
    // already granted (I2: no capability or grant changes; every capability check and hook-stage denial still apply).
    // A `PolicyDecider`'s `auto_deny` still denies -- and in this mode it is asked about EVERY call that reaches the
    // decider, whatever the tool's approval mode, so a host deny list keeps working (red team round 2: before, only
    // `policy_driven` tools asked it). Overrides any `set_approval_decider` while set, except as a veto: a `veto`
    // decider, when given, is asked about every call that would need approval, and a `false` from it denies the call
    // ("approve everything that needs approval, except X"). A later call without a veto keeps the one already set;
    // `clear_unattended_approvals` drops it. Every approval emits a `policy_decision` event naming the tool, the
    // caller, a hash of the arguments and `operator_id` (required, I4; refused if empty). Off by default.
    //
    // What it does not cover: `agent.ask` and hook-decision suspensions still wait for host code; `agent.spawn`
    // children and other sessions have their own settings (a spawned child takes only the decider its
    // `SpawnTargetDescriptor` names). The setters are session-thread-only, like every other session setting: a kill
    // switch on another thread must post to the session's thread. `approval_decider()` keeps returning the host's own
    // decider while this is set.
    [[nodiscard]] result<void> set_unattended_approvals(std::string operator_id,
                                                        agentengine::ApprovalDecider veto = {});
    void clear_unattended_approvals() noexcept {
        unattended_by_.reset();
        unattended_veto_.reset();
    }

    // ADR-192: the whole full-automation setup in one call -- approved lessons (if `registry` is given) delivered as
    // instructions, the system-channel fence off, and unattended approvals. Each part is the separate call above,
    // audited as such; capabilities are still whatever `set_capabilities` granted.
    [[nodiscard]] result<void> enable_unattended_mode(std::string const& operator_id,
                                                      agentengine::ApprovedLessonRegistry const* registry = nullptr,
                                                      agentengine::ApprovalDecider veto = {});
    [[nodiscard]] bool unattended_approvals() const noexcept { return unattended_by_.has_value(); }
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
    [[nodiscard]] std::unordered_map<std::string, std::string>& metadata() noexcept { return metadata_; }
    [[nodiscard]] std::unordered_map<std::string, std::string> const& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] std::string const& last_run_id() const noexcept { return last_run_id_; }
    [[nodiscard]] std::uint64_t last_turn_index() const noexcept { return effect_context_.turn_index; }
    // ADR-193: both include what delegated agents charged that has not been folded in yet -- a run that ended
    // without another model call (suspended, canceled, or its last round delegated) still reports it.
    [[nodiscard]] std::uint64_t run_tokens_consumed() const;
    // ADR-163: the full-`Usage` sibling of the accessor above -- see `run_usage_`'s own comment for
    // why this is a separate, parallel field rather than a re-derivation of `run_tokens_consumed_`.
    [[nodiscard]] agentengine::Usage run_usage() const;

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
    [[nodiscard]] std::vector<std::string> expired_interaction_ids(std::int64_t now_ns) const;

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
    bool set_interaction_expiry(std::string const& interaction_id, std::int64_t expires_at_ns) noexcept;

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
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Replaces `handle(quark::Ask<ResolveInteraction, AgentResponse> const&)`.
    task<result<AgentResponse>> resolve_interaction(
            ResolveInteraction request,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // ADR-196 / round-3 red team: the tool list a resumed round dispatches against. With the round's record, exactly
    // the tools the model was offered when it made these calls (and that the provider still offers -- a tool removed
    // since does not come back). Before, every resume rebuilt the list from the raw provider -- the host's turn
    // middleware never ran on it -- so a tool the host had hidden from the round could be dispatched on resume, and
    // with ADR-192's unattended approvals, with no human at all.
    //
    // ADR-196 §7 (issue #111 A4): the table is the round's RECORDED descriptors -- exactly as the turn middleware left
    // them (an approval mode it tightened, an invoke it wrapped) -- never the provider's current ones. The provider is
    // still asked, but only to NARROW: a recorded tool it no longer offers is dropped (and `schedule_wakeup` only while
    // the grant still holds). There is no record-less fallback any more: resolve_interaction() refuses a round it has
    // no record of before this is reached.
    task<result<ToolTable>> resume_tool_table(SuspendedRoundRecord const& round);

    // ADR-053 §5 follow-up: the session-scoped `schedule_wakeup` tool, offered only to a session holding `cap::Schedule`
    // (see run_rounds()). A member so a resumed round (resume_tool_table()) offers the identical tool.
    [[nodiscard]] ToolDescriptor schedule_wakeup_tool();

    // ---- Pure bookkeeping, unchanged in behavior from core/agent_session.hpp -----------------
    // (no Quark dependency in the original either -- ported verbatim, not redesigned)


    [[nodiscard]] result<void> redact(std::string const& message_id, std::string reason, std::string actor);

private:
    // ADR-199: `AgentSession::fork_from()`'s body, minus the template's own state and history provider -- the caller
    // holds `source.session_mutex_` (that function's comment has why).
    void fork_core_from(AgentSessionCore const& source, std::string new_session_id,
                        std::optional<std::size_t> history_prefix_len);
    // ADR-199: `AgentSession::clear_in_process_state()`'s body, minus the template's own state and history provider.
    void clear_core_state();

public:

    [[nodiscard]] Interaction const& open_interaction(std::string run_id, interaction_reason reason);

    [[nodiscard]] result<void> resolve_interaction_record(std::string const& interaction_id);

    // ---- Slice 2: snapshot/checkpoint (file banner has the design writeup) -------------------

    // Unlocked, synchronous -- matches the original's own to_record()/restore_from_record() shape
    // exactly (see file banner: field list is deliberately narrower). Not the caller's normal way
    // to take a snapshot; see snapshot_record() below for the locked path every real caller should
    // use instead. Kept public and separately callable anyway, matching the original, since a test
    // may reasonably want to assert the record shape without going through the lock.
    [[nodiscard]] AgentSessionRecord to_record() const;

    void restore_from_record(AgentSessionRecord const& rec);

    // The real, in-flight-safe way to read this session's durable state out: acquires
    // session_mutex_ for the whole read, the same I1 guard every other public entry point uses --
    // see file banner for why this replaces the Quark original's "called at the point quiesce(Drain)
    // reaches" assumption. save_agent_session_snapshot() (free function, below) is built on this.
    [[nodiscard]] task<AgentSessionRecord> snapshot_record();


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
        ApprovalDecider const& approve = ApprovalDecider{});

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
        std::optional<RequestAuthority> const& authority = std::nullopt);

    [[nodiscard]] std::vector<agentengine::StandingEffect> due_standing_effects(
        std::chrono::steady_clock::time_point now) const {
        return standing_effects_registry_.due(now);
    }

    // Explicit host-callable drain, for a host that wants to flush completions between runs rather
    // than waiting for the next start_run()/resolve_interaction() (which already drains
    // automatically as their first step -- see file banner).
    task<void> drain_background_completions();

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
            std::chrono::steady_clock::time_point now);

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
                             RunEventPayload payload = run_event_payload::Empty{});

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
                                                      PolicyDecider const& policy);

    // `force_tainted()`, `filter_cross_provider_reasoning()`, and `drain_streaming_response()` now
    // live as free functions in `rt::detail` (rt/agent_session_trust.hpp, included above) --
    // docs/planning/agent-session-decomposition-design-draft.md §2b. Each takes an `EmitFn`
    // callback in place of directly calling `emit_run_event()`; this class's own `emit_run_event()`/
    // `emit_run_event_for()` are unchanged and unmoved (§2c of that draft) -- every call site below
    // constructs a thin `[this](k, p){ emit_run_event(k, std::move(p)); }` closure at the point of
    // use.

    // ADR-177: is this failed model call worth re-issuing? A pure function of host-held DATA -- the
    // stream's own recorded error and a counter -- never of anything the model said (I3) and never of
    // a clock (I5: a wall-clock read in a control decision would not replay). Every clause exists
    // because of a specific way a retry would be WRONG:
    //   - gateway sessions: their own commit rule (004 §4) stands; a retry here could land on a
    //     different tier than the one that showed the partial answer.
    //   - not `run.stream_incomplete`: `run.usage_unavailable` is a COMPLETED stream, and re-issuing
    //     it would bill a finished call twice.
    //   - inner class must be `transient`: contract/policy/resource failures do not get better.
    //   - `net.cancelled` is ALSO classed transient by the transport, so the class alone would retry
    //     a cancellation; excluded by code, and by the run's own stop token.
    //   - `any_update_seen`: a failure before the first byte (rate limit, refused connect) is not the
    //     mid-answer death this exists for, and an immediate retry would hammer a provider that just
    //     said to slow down.
    [[nodiscard]] bool should_retry_stream(error const& err) const;


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
                                                       std::string const& interaction_id);

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
                                                              PolicyDecider const& policy = {});

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
                                                         std::string const& interaction_id);

    // Same shape as core/agent_session.hpp's own run_rounds() -- ported to rt::task<T>, no longer
    // templated on AskT (there is only one caller shape now, a plain `result<AgentResponse>` return),
    // otherwise byte-for-byte identical logic.
    // ADR-178: the one way a canceled run ends. `run_canceled` is the terminal event 013 already names.
    [[nodiscard]] std::unexpected<error> finish_canceled();

    task<result<AgentResponse>> run_rounds();


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
    agentengine::ApprovedLessonRegistry const*         approved_lessons_ = nullptr;  // ADR-191 opt-in

    agentengine::approved_lesson_level                 approved_lesson_level_ =
        agentengine::approved_lesson_level::guidance;                                    // ADR-192
    std::optional<std::string>                         lesson_level_set_by_;              // ADR-192 round 3
    std::optional<std::string>                         system_fence_disabled_by_;         // ADR-192 opt-in
    std::optional<std::string>                         unattended_by_;                    // ADR-192 opt-in
    std::vector<Message>                               pinned_context_;                   // ADR-193
    // Shared, so a veto that clears unattended mode from inside itself does not destroy itself while running.
    std::shared_ptr<agentengine::ApprovalDecider const> unattended_veto_;                // ADR-192, optional

    [[nodiscard]] static result<void> unattended_detail_refuse(char const* what);

    // The one place a request's delivery marks are set (ADR-191/192). Both are cleared on every item first, so
    // nothing a provider, a plugin or stored history carries survives; then each is granted only by a host setting.
    void apply_approved_lessons(ChatRequest& request);

    // ADR-192: the decider a round actually uses. In unattended mode it approves every call that reaches it and
    // audits each approval; otherwise it is the host's own (unset = deny, as before). Built per round, never stored,
    // so it never outlives the session it points at; approvals are admitted on this thread (ADR-160 §5).
    //
    // Red team: (MAJOR) the host's decider is passed by reference, never copied -- a copy reset any state a mutable
    // decider kept (an approval budget) every round; (MINOR) the operator id is captured by value and the setting is
    // re-read at each call, so clearing it mid-round stops the next approval instead of reading an emptied optional.
    // Round 2: (MAJOR) the host's `PolicyDecider` is asked about every call here and its `auto_deny` honoured, so a
    // deny list covers `always_require` and `never_require`+`text_derived` calls too; (MINOR) the veto is held through
    // a local shared_ptr while it runs.
    [[nodiscard]] agentengine::ApprovalDecider effective_approval_decider(ToolTable const& tool_table);
    [[nodiscard]] bool approval_waits_for_human() const noexcept {
        return suspend_for_approval_ && !approval_decider_ && !unattended_by_;
    }
    std::vector<Message>                               history_;
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
    std::unordered_map<std::string, SuspendedRoundRecord>                  suspended_rounds_;  // ADR-196
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
    // ADR-177. `stream_retries_used_` is per RUN (reset in start_run), never per round.
    std::uint32_t                                         stream_retries_ = 0;
    std::uint32_t                                         stream_retries_used_ = 0;
    std::uint64_t                                         discarded_tokens_estimate_ = 0;
    std::optional<detail::StreamFailure>                  last_stream_failure_;
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
    // ADR-193: usage a delegated agent reported through `charge_delegated_usage`, not yet folded into this run's.
    // Guarded because a tool may run on a worker thread (ADR-160); folded on the session's thread.
    // Round 2: held through a shared_ptr that the charge closure captures instead of `this`, so a charge that
    // arrives from a detached thread (a WorkflowChatClient worker whose stream the run abandoned on cancel) can
    // never touch a destroyed session; `run` tags each charge with the run it was issued for, and one aimed at an
    // earlier run is dropped.
    struct DelegatedCharges {
        std::mutex         mutex;
        std::uint64_t      run = 0;
        agentengine::Usage usage{};
        // Budget-only tokens a delegated agent reported (its discarded-stream estimates, ADR-177): they count
        // against this run's token budget, never into `run_usage_`, the same split this session keeps for its own.
        std::uint64_t      extra_tokens = 0;
    };
    std::shared_ptr<DelegatedCharges> delegated_charges_ = std::make_shared<DelegatedCharges>();

    // Folds pending delegated usage into this run's usage and token count; true if there was any.
    bool fold_delegated_usage();

    // ADR-193: a delegated agent's RunEvent, wrapped as this run's `delegated_event` (the child's own event inside,
    // unchanged) -- so a host watching the root sees every hop, while protocol projectors never mistake a child's
    // run_started/run_finished for this run's (red team round 1: the A2A task went `completed` mid-run).
    void forward_run_event(RunEvent const& ev);
    // ADR-178: guards `cancel_source_`'s handle (copy/assign), NOT the stop-state -- `request_stop()` on a
    // copy is itself thread-safe. `mutable` so `cancellation_token()` can stay const.
    mutable std::mutex                                     cancel_mutex_;
    std::stop_source                                       cancel_source_;
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

    // ---- ADR-199: the template's side of the loop ------------------------------------------------
    // Each is reached once per model call, context assembly or turn end -- never per token (CONVENTIONS.md: type
    // erasure only at declared seams, never in a turn's hot loop; the provider and context seams are declared ones).
    // Private, and every other member of this class is private too: only its friend AgentSession<...> (which is `final`)
    // reaches them. `TurnView const&`, not by value: the provider's task is lazy, so a provider taking `TurnView const&`
    // would otherwise keep a reference to this hook's own parameter after it returned (red team, ADR-199 §8).
private:
    virtual task<result<ChatResponse>> run_model_call(ChatRequest const& request, EffectContext& ctx) = 0;
    virtual task<result<ContextContribution>> bound_on_context(SessionContext& session_ctx, EffectContext& ctx) = 0;
    virtual task<std::monostate> bound_on_turn_end(TurnView const& turn, EffectContext& ctx) = 0;
    // Gap-audit finding 20: drops Reasoning items another backend produced; a no-op when the client names none.
    virtual void bound_filter_cross_provider_reasoning(ContextContribution& contribution) = 0;
    [[nodiscard]] virtual bool bound_has_chat_client() const noexcept = 0;
    [[nodiscard]] virtual model_route bound_model_route() const noexcept = 0;

public:
    virtual ~AgentSessionCore() = default;
};

}  // namespace agentengine::rt
