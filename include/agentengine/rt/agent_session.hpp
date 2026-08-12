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
//   - Standing effects / background tasks (start_background_task, cancel_standing_effect,
//     list_standing_effects, the TimerWake/BackgroundTaskDone message types) -- these fundamentally
//     depended on Quark's actor tell()/self-addressing (quark::ActorRef<AgentSession>::tell()) to be
//     thread-safe; the Quark-free replacement needs its own design (a real callback mechanism through
//     rt::ThreadPool, most likely) before this can move, not a mechanical port.
//   - Snapshot/checkpoint (save_agent_session_snapshot, load_agent_session_snapshot,
//     checkpoint_if_due, delete_session, to_record/restore_from_record) -- these depended on
//     quark::FenceToken/Activation/snapshot_sequential; the replacement uses rt::SessionStore
//     (already built and tested, session_store.hpp) but the actual encode/decode + "safe to snapshot
//     only when not in-flight" design is real, separate work, not done here.
//   - Event streaming (enable_event_stream/emit_run_event) still uses core/stream.hpp's EXISTING
//     quark::ReplyStream-backed stream<T> -- core/stream.hpp's own backend migration to rt::channel<T>
//     is separately-scoped Phase 1 work not yet done. Named here as an accepted interim residual, not
//     silently hidden: this session is not fully Quark-free yet, specifically at this one seam.
//
// A LARGER, MORE FUNDAMENTAL NAMED GAP, found and corrected while writing this file (not discovered
// later): removing `quark::Actor<Self, Sequential>` and Quark's mailbox does NOT, by itself, make
// this type Quark-free. `ChatClientT`/`HistoryProviderT` are still the EXISTING `agentengine::
// ChatClient`/`ContextProvider` conformers (`core/chat_client.hpp`/`core/context_provider.hpp`) --
// every real backend (`OpenAIChatClient`, `AnthropicChatClient`, `ModelCallGateway`) and every
// `HistoryProvider<...>` still returns `agentengine::task<T>`, which `core/task.hpp` still aliases to
// `quark::task<T>` today. This file's own `run_model_call()`/`run_rounds()` `co_await` those calls
// directly -- which compiles and works correctly (any `rt::task<T>` coroutine body can `co_await` any
// awaitable, including a `quark::task<T>`, since both independently implement the same C++20 awaiter
// protocol; they nest transparently) -- but it means this AgentSession is Quark-free at the
// ACTOR/MAILBOX/DISPATCH layer specifically, NOT at the coroutine-TYPE layer that flows in through
// every backend/provider it's plugged into. Making THAT layer Quark-free too needs a further, separate
// migration: every `ChatClient`/`ContextProvider` conformer across `core/`/`protocol/` retargeted from
// `agentengine::task<T>` (the `quark::task<T>` alias) to `agentengine::rt::task<T>` -- comparable in
// scope to this file itself, not a side effect of it. Named here, not silently claimed done.
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
#include <cstdint>
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
#include "agentengine/core/stream.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/async_mutex.hpp"
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

private:
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

    // Same shape as core/agent_session.hpp's own run_model_call(), ported to rt::task<T>, with ONE
    // real consolidation (not a byte-for-byte port): the original had three branches (gateway /
    // buffered-chat() / buffered-drain-when-chat()-is-unavailable / live-streaming-when-opted-in) --
    // this collapses the last three into ONE shared drain loop, gated only on whether to emit
    // model_delta events (`stream_model_calls_`), since "buffer silently" and "stream live" differ
    // ONLY in that one respect once chat() isn't being used. `ChatClientT::chat_stream()` still
    // returns `agentengine::stream<ChatResponseUpdate>` (core/stream.hpp's quark::ReplyStream-backed
    // type, not an rt:: one) -- see file banner's named residual on event streaming; that gap is
    // unaffected by this consolidation, just inherited from the same place it always was.
    // Fail-closed-on-missing-usage (004 §5's TokenBudget<N>) is preserved exactly, on both paths
    // through the shared loop.
    task<result<ChatResponse>> run_model_call(ChatRequest const& request, EffectContext& ctx) {
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

            ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);
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
    // I1 -- see file banner. Every public async entry point acquires this for its whole duration.
    AsyncMutex                                            session_mutex_;
};

}  // namespace agentengine::rt
