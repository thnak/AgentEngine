// ADR-199 (issue #115 E3): the bodies of `rt::AgentSessionCore` (include/agentengine/rt/agent_session_core.hpp) --
// AgentSession's turn loop and everything else that does not depend on its template parameters, compiled once
// here instead of once per session type in every translation unit that includes agent_session.hpp. Moved
// verbatim from that header; agent_session.hpp's banner documents the design.

#include "agentengine/rt/agent_session_core.hpp"

namespace agentengine::rt {

// agent_session.hpp:785
void AgentSessionCore::cancel() noexcept {
    std::stop_source source;
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        source = cancel_source_;
    }
    source.request_stop();
}

// agent_session.hpp:852
result<void> AgentSessionCore::set_approved_lessons(agentengine::ApprovedLessonRegistry const* registry,
                                                agentengine::approved_lesson_level level,
                                                std::string operator_id) {
    if (level == agentengine::approved_lesson_level::instructions &&
        !agentengine::is_attributable_id(operator_id)) {
        return unattended_detail_refuse("set_approved_lessons(instructions)");
    }
    approved_lessons_ = registry;
    approved_lesson_level_ = level;
    if (operator_id.empty()) lesson_level_set_by_.reset();
    else lesson_level_set_by_ = std::move(operator_id);
    return {};
}

// agent_session.hpp:904
result<void> AgentSessionCore::set_unattended_approvals(std::string operator_id,
                                                    agentengine::ApprovalDecider veto) {
    if (!agentengine::is_attributable_id(operator_id)) return unattended_detail_refuse("set_unattended_approvals");
    unattended_by_ = std::move(operator_id);
    if (veto) unattended_veto_ = std::make_shared<agentengine::ApprovalDecider const>(std::move(veto));
    return {};
}

// agent_session.hpp:919
result<void> AgentSessionCore::enable_unattended_mode(std::string const& operator_id,
                                                  agentengine::ApprovedLessonRegistry const* registry,
                                                  agentengine::ApprovalDecider veto) {
    if (!agentengine::is_attributable_id(operator_id)) return unattended_detail_refuse("enable_unattended_mode");
    if (registry != nullptr) {
        (void)set_approved_lessons(registry, agentengine::approved_lesson_level::instructions, operator_id);
    }
    (void)disable_system_channel_fence(operator_id);
    return set_unattended_approvals(operator_id, std::move(veto));  // an existing veto is kept if none is given
}

// agent_session.hpp:995
std::uint64_t AgentSessionCore::run_tokens_consumed() const {
    std::lock_guard<std::mutex> lock(delegated_charges_->mutex);
    return run_tokens_consumed_ + delegated_charges_->usage.input_tokens + delegated_charges_->usage.output_tokens +
           delegated_charges_->extra_tokens;
}

// agent_session.hpp:1002
agentengine::Usage AgentSessionCore::run_usage() const {
    agentengine::Usage u = run_usage_;
    std::lock_guard<std::mutex> lock(delegated_charges_->mutex);
    u.input_tokens += delegated_charges_->usage.input_tokens;
    u.output_tokens += delegated_charges_->usage.output_tokens;
    u.cached_input_tokens += delegated_charges_->usage.cached_input_tokens;
    u.reasoning_tokens += delegated_charges_->usage.reasoning_tokens;
    u.cost_estimate += delegated_charges_->usage.cost_estimate;
    u.cache_write_tokens += delegated_charges_->usage.cache_write_tokens;
    return u;
}

// agent_session.hpp:1040
std::vector<std::string> AgentSessionCore::expired_interaction_ids(std::int64_t now_ns) const {
    std::vector<std::string> ids;
    for (Interaction const& i : open_interactions_) {
        if (i.expires_at_ns != 0 && now_ns >= i.expires_at_ns) ids.push_back(i.interaction_id);
    }
    return ids;
}

// agent_session.hpp:1059
bool AgentSessionCore::set_interaction_expiry(std::string const& interaction_id, std::int64_t expires_at_ns) noexcept {
    auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                            [&](Interaction const& i) { return i.interaction_id == interaction_id; });
    if (it == open_interactions_.end()) return false;
    it->expires_at_ns = expires_at_ns;
    return true;
}

// agent_session.hpp:1081
task<result<AgentResponse>> AgentSessionCore::start_run(
        StartRun request,
        std::chrono::steady_clock::time_point now) {
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
    stream_retries_used_ = 0;
    discarded_tokens_estimate_ = 0;
    run_tokens_consumed_ = 0;
    run_usage_ = agentengine::Usage{};
    // ADR-061 §20.3: principal/capabilities are already set by apply_dispatch_authority() above
    // -- setting them again here from session-level state would silently overwrite a correctly-
    // resolved per-request authority, reproducing the exact bug this mechanism exists to close.
    effect_context_.run_id       = session_id_ + ":run:" + std::to_string(run_counter_);
    effect_context_.turn_index   = 0;
    last_run_id_ = effect_context_.run_id;
    // ADR-193: a tool that runs another agent reports the child's events and usage back through these. Both
    // capture `this`: every tool call of this run finishes before the run does, and the session outlives it.
    effect_context_.delegated_event_sink = [this](RunEvent const& ev) { forward_run_event(ev); };
    std::uint64_t charge_run = 0;
    {
        std::lock_guard<std::mutex> lock(delegated_charges_->mutex);
        charge_run = ++delegated_charges_->run;
        delegated_charges_->usage = agentengine::Usage{};
        delegated_charges_->extra_tokens = 0;
    }
    // Round 2: captures the shared state, not `this` (see `DelegatedCharges`), so it is safe to hand to a
    // detached worker.
    effect_context_.charge_delegated_usage = [charges = delegated_charges_, charge_run](
                                                 agentengine::Usage const& u, std::uint64_t extra_budget_tokens) {
        std::lock_guard<std::mutex> lock(charges->mutex);
        if (charges->run != charge_run) return;  // a late charge for an earlier run
        charges->extra_tokens += extra_budget_tokens;
        charges->usage.input_tokens += u.input_tokens;
        charges->usage.output_tokens += u.output_tokens;
        charges->usage.cached_input_tokens += u.cached_input_tokens;
        charges->usage.reasoning_tokens += u.reasoning_tokens;
        charges->usage.cost_estimate += u.cost_estimate;
        charges->usage.cache_write_tokens += u.cache_write_tokens;
    };
    {
        // ADR-178: one source PER RUN. Replaced (not reset) so a cancel aimed at the previous run,
        // still holding the old source, cannot reach this one; guarded because `cancel()` may be
        // running on another thread this instant.
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        cancel_source_ = std::stop_source{};
        effect_context_.cancellation = cancel_source_.get_token();
    }

    emit_run_event(run_event_kind::run_started);
    // ADR-199: branches on bound_model_route(), which the template answers from the client's type exactly as these
    // branches used to with `if constexpr`.
    if (model_route const route = bound_model_route(); route != model_route::direct) {
        // unified-streaming-design-draft.md §3 (Piece A), Rev 7 (Finding 5-new, 5th red-team pass):
        // this warning is a SEPARATE emission site from run_model_call()'s own dispatch fix -- fixing
        // only the dispatch would leave this one asserting "no live model_delta events" on every
        // gateway-routed run even after Piece A makes that false for a real ModelCallGateway with
        // stream_model_calls_ set. Gated identically to that dispatch's own condition.
        if (route == model_route::gateway_streaming) {
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

// agent_session.hpp:1233
task<result<AgentResponse>> AgentSessionCore::resolve_interaction(
        ResolveInteraction request,
        std::chrono::steady_clock::time_point now) {
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

    // Issue #107: an `input`/`auth` interaction (only a host-supplied record can carry one) used to fall through
    // into the approval branch.
    if (it->reason == interaction_reason::input || it->reason == interaction_reason::auth) {
        co_return std::unexpected(error{failure_class::contract,
                                         "this interaction's reason is not one resolve_interaction() handles",
                                         "session.resolve_interaction.unsupported_reason"});
    }

    // ADR-196 §7 (issue #111 A5/A6): everything about the request is checked, for EVERY interaction kind, before
    // anything closes, is announced or runs -- so a malformed resolve is refused and can be retried. The hook and
    // CodeAct branches used to return before the approver check, and ignored `call_decisions`.
    if (request.approver_id && !agentengine::is_attributable_id(*request.approver_id)) {
        co_return std::unexpected(error{failure_class::contract,
                                         "an approver id must be non-blank with no control characters (I4)",
                                         "session.resolve_interaction.bad_approver"});
    }
    if (request.call_decisions && it->reason != interaction_reason::approval) {
        co_return std::unexpected(error{failure_class::contract,
                                         "call_decisions apply only to an approval interaction",
                                         "session.resolve_interaction.call_decisions_not_applicable"});
    }
    if (it->reason == interaction_reason::codeact_ask && !request.answer.has_value()) {
        co_return std::unexpected(error{failure_class::contract,
                                         "resolving a codeact_ask interaction requires an answer",
                                         "session.resolve_interaction.answer_required"});
    }
    if (it->reason == interaction_reason::hook_decision && !request.hook_dispatch_answers) {
        co_return std::unexpected(error{failure_class::contract,
                                         "hook_decision resume requires hook_dispatch_answers",
                                         "session.hook_decision.missing_answers"});
    }

    // ADR-196 §7 (issue #111 A1): an interaction resolves only against the round this session recorded when it
    // suspended. One without a record -- restored from an AgentSessionRecord, which carries none (OQ-21) -- used
    // to fall back to "every call in the last history message", which after a restore onto a live session was a
    // DIFFERENT round: a human approved one call and another ran, attributed to them. Now it is closed with nothing
    // run, so it cannot keep start_run() refused either (the codeact_ask branch's own precedent).
    if (!suspended_rounds_.contains(it->interaction_id)) {
        std::string const orphan_id = it->interaction_id;
        pending_hook_decisions_.erase(orphan_id);
        pending_codeact_asks_.erase(orphan_id);
        (void)resolve_interaction_record(orphan_id);
        co_return std::unexpected(error{
            failure_class::contract,
            "this interaction has no recorded round in this session (restored or replaced) -- closed, nothing ran",
            "session.resolve_interaction.round_not_recorded"});
    }

    if (history_.empty() || history_.back().role != role::assistant ||
        suspended_rounds_.at(it->interaction_id).message_index != history_.size() - 1) {
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

    // ADR-196 (issues #104/#108): everything about the request is checked BEFORE the interaction closes, so a
    // malformed resolve leaves the round open and retryable (the approver was checked above, for every kind).
    auto const hook_hit = pending_hook_decisions_.find(resolved_interaction_id);
    // Copied: resolve_interaction_record() below drops the record with the interaction.
    SuspendedRoundRecord const round_record = suspended_rounds_.at(resolved_interaction_id);
    // The calls this interaction asked about: the recorded list (ADR-196 §7: there is no fallback any more).
    std::vector<std::string> const asked = hook_hit != pending_hook_decisions_.end()
                                               ? hook_hit->second.approval_requested_call_ids
                                               : round_record.gated_call_ids;
    auto const was_asked = [&asked](std::string const& call_id) {
        return std::find(asked.begin(), asked.end(), call_id) != asked.end();
    };
    if (request.call_decisions) {
        std::vector<std::string> seen;
        for (ApprovalCallDecision const& d : *request.call_decisions) {
            if (!was_asked(d.call_id)) {
                co_return std::unexpected(error{
                    failure_class::contract,
                    "call_id " + d.call_id + " did not wait on this interaction's decision",
                    "session.resolve_interaction.call_not_pending"});
            }
            if (std::find(seen.begin(), seen.end(), d.call_id) != seen.end()) {
                co_return std::unexpected(error{failure_class::contract,
                                                 "call_id " + d.call_id + " is decided twice",
                                                 "session.resolve_interaction.duplicate_call_decision"});
            }
            seen.push_back(d.call_id);
        }
    }
    auto const decision_for = [&request](std::string const& call_id) {
        if (request.call_decisions) {
            for (ApprovalCallDecision const& d : *request.call_decisions) {
                if (d.call_id == call_id) return d.approved;
            }
        }
        return request.approved;
    };
    // One shape for both kinds of round: a plain round becomes a hook-processed one whose calls all pass through.
    PendingHookDecisionRound round;
    if (hook_hit != pending_hook_decisions_.end()) {
        round = std::move(hook_hit->second);
    } else {
        for (std::size_t i = 0; i < pending_calls.size(); ++i) {
            round.calls.push_back(HookProcessedCall{tool_call_request_of(pending_calls[i], i),
                                                    hook_call_outcome::pass_through, std::nullopt});
        }
    }
    // Issue #107: the stored round goes with the interaction, before anything below can fail (an on_context()
    // failure used to leave it behind).
    pending_hook_decisions_.erase(resolved_interaction_id);
    result<void> const resolved = resolve_interaction_record(resolved_interaction_id);
    if (!resolved) co_return std::unexpected(resolved.error());
    emit_run_event(run_event_kind::input_resolved,
                    run_event_payload::InteractionRef{request.interaction_id,
                                                      request.approver_id.value_or(std::string{})});

    // ADR-183: the decision is announced before anything acts on it -- one approval_resolved per
    // approval_requested this interaction emitted, in the same order, before on_context() and before any
    // tool_call_started. ADR-196: each carries its own call's decision and the approver.
    for (std::string const& call_id : asked) {
        emit_run_event(run_event_kind::approval_resolved,
                        run_event_payload::ApprovalResolved{call_id, decision_for(call_id), request.interaction_id,
                                                              request.approver_id.value_or(std::string{})});
    }

    // A call denied here is settled and never dispatched. A call approved here is approved for exactly its tool
    // and arguments. A call this interaction never asked about is dispatched under the session's ordinary rules.
    std::vector<std::pair<std::string, std::string>> approved_exact;  // tool name, canonical arguments
    bool any_to_run = false;
    for (HookProcessedCall& hc : round.calls) {
        if (hc.outcome == hook_call_outcome::denied) continue;  // the hook stage already settled it
        if (was_asked(hc.request.call_id)) {
            if (!decision_for(hc.request.call_id)) {
                hc.outcome       = hook_call_outcome::denied;
                hc.denial_result = make_denial_result(hc.request.call_id, "denied by operator",
                                                      "tool.approval_denied");
                continue;
            }
            approved_exact.emplace_back(hc.request.tool_name, json::dump(hc.request.arguments));
        }
        any_to_run = true;
    }
    if (!any_to_run) {
        co_return co_await finish_hook_processed_round(std::move(round), ToolTable::from_descriptors({}),
                                                       ApprovalDecider{});
    }
    result<ToolTable> const tool_table = co_await resume_tool_table(round_record);
    if (!tool_table) co_return std::unexpected(tool_table.error());
    // ADR-196: this used to dispatch the whole round under an always-yes decider with no policy, so a call nobody
    // was asked about -- a policy_driven call the host's policy denies, say -- ran on the strength of someone
    // else's approval. Now the human's yes covers only what they were shown; everything else goes through the
    // session's own decider and policy exactly as run_rounds() would have sent it.
    ApprovalDecider const fallback = effective_approval_decider(*tool_table);
    ApprovalDecider const approve_exact = [approved_exact, fallback](Principal const& caller,
                                                                    std::string_view tool_name,
                                                                    std::string const& canonical_args) {
        for (auto const& [name, args] : approved_exact) {
            if (name == tool_name && args == canonical_args) return true;
        }
        return fallback ? fallback(caller, tool_name, canonical_args) : false;
    };
    co_return co_await finish_hook_processed_round(std::move(round), *tool_table, approve_exact, policy_decider_);
}

// agent_session.hpp:1485
task<result<ToolTable>> AgentSessionCore::resume_tool_table(SuspendedRoundRecord const& round) {
    // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
    SessionContext session_ctx{session_id_, effect_context_.principal, history_};
    result<ContextContribution> contribution =
        co_await bound_on_context(session_ctx, effect_context_);
    if (!contribution) {
        emit_run_event(run_event_kind::run_failed,
                        run_event_payload::RunFailed{contribution.error().code, contribution.error().message,
                                                      "run.context_unavailable"});
        co_return std::unexpected(contribution.error());
    }
    CapabilitySet const empty_caps = CapabilitySet::grant_root({});
    CapabilitySet const& held = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;
    bool const schedule_held = held.find_schedule().has_value();
    std::vector<ToolDescriptor> tools;
    tools.reserve(round.offered_tools.size());
    for (ToolDescriptor const& recorded : round.offered_tools) {
        bool const still_offered =
            (schedule_held && recorded.name == ScheduleWakeupTool::name) ||
            std::any_of(contribution->tools.begin(), contribution->tools.end(),
                        [&recorded](ToolDescriptor const& d) { return d.name == recorded.name; });
        if (still_offered) tools.push_back(recorded);
    }
    co_return ToolTable::from_descriptors(std::move(tools));
}

// agent_session.hpp:1513
ToolDescriptor AgentSessionCore::schedule_wakeup_tool() {
    return make_tool_descriptor_with_invoke<ScheduleWakeupTool>(
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
        });
}

// agent_session.hpp:1657
result<void> AgentSessionCore::redact(std::string const& message_id, std::string reason, std::string actor) {
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

// agent_session.hpp:1675
// agent_session.hpp's fork_from() -- the caller holds source.session_mutex_; see that function's comment.
void AgentSessionCore::fork_core_from(AgentSessionCore const& source, std::string new_session_id,
                                      std::optional<std::size_t> history_prefix_len) {
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
    metadata_ = source.metadata_;
    run_counter_ = 0;
    stream_retries_used_ = 0;
    discarded_tokens_estimate_ = 0;
    last_run_id_.clear();
    effect_context_ = EffectContext{};
    open_interactions_.clear();
    interaction_counter_ = 0;
    // Issue #107: every per-interaction record goes with the interactions -- a stale entry kept here matched a
    // reused interaction id and ran an old round's stored requests.
    pending_hook_decisions_.clear();
    pending_codeact_asks_.clear();
    suspended_rounds_.clear();
    run_tokens_consumed_ = 0;
    run_usage_ = agentengine::Usage{};
    admission_denied_count_ = 0;
    // Same "no run identity of its own" rationale open_interactions_ above already documents --
    // a fresh fork inherits none of the source's (or *this*'s own prior) outstanding background
    // work. Same fix category as clear_core_state()'s own comment below.
    standing_effects_registry_.reset();
}

void AgentSessionCore::clear_core_state() {
    session_id_.clear();
    principal_ = agentengine::Principal{};
    history_.clear();
    metadata_.clear();
    run_counter_ = 0;
    stream_retries_used_ = 0;
    discarded_tokens_estimate_ = 0;
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
    suspended_rounds_.clear();  // ADR-196
    token_budget_ = std::nullopt;
    run_tokens_consumed_ = 0;
    run_usage_ = agentengine::Usage{};
    admission_denied_count_ = 0;
    max_turns_ = std::nullopt;
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

// agent_session.hpp:1721
Interaction const& AgentSessionCore::open_interaction(std::string run_id, interaction_reason reason) {
    interaction_counter_ += 1;
    Interaction interaction{};
    interaction.interaction_id = session_id_ + ":interaction:" + std::to_string(interaction_counter_);
    interaction.run_id         = std::move(run_id);
    interaction.reason         = reason;
    open_interactions_.push_back(std::move(interaction));
    return open_interactions_.back();
}

// agent_session.hpp:1731
result<void> AgentSessionCore::resolve_interaction_record(std::string const& interaction_id) {
    auto it = std::find_if(open_interactions_.begin(), open_interactions_.end(),
                            [&](Interaction const& i) { return i.interaction_id == interaction_id; });
    if (it == open_interactions_.end()) {
        return std::unexpected(error{failure_class::contract, "no open interaction with that id",
                                      "session.resolve_interaction.unknown_id"});
    }
    open_interactions_.erase(it);
    suspended_rounds_.erase(interaction_id);  // ADR-196: a round's record never outlives its interaction
    return {};
}

// agent_session.hpp:1750
AgentSessionRecord AgentSessionCore::to_record() const {
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

// agent_session.hpp:1762
void AgentSessionCore::restore_from_record(AgentSessionRecord const& rec) {
    session_id_ = rec.session_id;
    principal_  = agentengine::Principal{rec.principal_id, rec.principal_tenant_id};
    run_counter_ = rec.run_counter;
    last_run_id_ = run_counter_ > 0 ? session_id_ + ":run:" + std::to_string(run_counter_)
                                      : std::string{};
    effect_context_.turn_index = rec.turn_index;
    open_interactions_ = rec.open_interactions;
    require_authority_ = rec.require_authority;  // ADR-061 §22.1 -- fail-closed carry-forward
    // Issue #107: the in-memory per-interaction records describe the session being replaced, never this record
    // -- drop them all (a restored interaction resolves without them, see resolve_interaction()). And never hand
    // out an interaction id the restored set already uses: the counter continues past the highest one.
    pending_hook_decisions_.clear();
    pending_codeact_asks_.clear();
    suspended_rounds_.clear();
    interaction_counter_ = 0;
    std::string const id_prefix = session_id_ + ":interaction:";
    for (Interaction const& i : open_interactions_) {
        if (!i.interaction_id.starts_with(id_prefix)) continue;
        std::uint64_t n = 0;
        bool digits = i.interaction_id.size() > id_prefix.size();
        for (std::size_t k = id_prefix.size(); k < i.interaction_id.size() && digits; ++k) {
            char const c = i.interaction_id[k];
            digits = c >= '0' && c <= '9' && n <= (UINT64_MAX - 9) / 10;
            if (digits) n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (digits) interaction_counter_ = std::max(interaction_counter_, n);
    }
}

// agent_session.hpp:1796
task<AgentSessionRecord> AgentSessionCore::snapshot_record() {
    AsyncMutex::Guard guard = co_await session_mutex_.lock();
    co_return to_record();
}

// agent_session.hpp:1820
task<result<agentengine::StandingEffect>> AgentSessionCore::start_background_task(
    ToolTable const& table, ToolCallRequest const& request,
    std::optional<RequestAuthority> const& authority,
    std::chrono::steady_clock::time_point now,
    ApprovalDecider const& approve) {
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

// agent_session.hpp:1902
task<result<agentengine::StandingEffect>> AgentSessionCore::schedule_wakeup(
    std::chrono::milliseconds delay, std::string label, std::chrono::steady_clock::time_point now,
    std::optional<RequestAuthority> const& authority) {
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

// agent_session.hpp:1930
task<void> AgentSessionCore::drain_background_completions() {
    AsyncMutex::Guard guard = co_await session_mutex_.lock();
    drain_background_completions_locked();
    co_return;
}

// agent_session.hpp:1958
result<void> AgentSessionCore::apply_dispatch_authority(
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

// agent_session.hpp:1995
void AgentSessionCore::emit_run_event_for(std::string const& run_id, run_event_kind kind,
                         RunEventPayload payload) {
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

// agent_session.hpp:2060
std::vector<AgentSessionCore::DispatchedCall> AgentSessionCore::dispatch_tool_calls(std::vector<ToolCallRequest> const& reqs,
                                                  ToolTable const& tool_table, CapabilitySet const& held,
                                                  ApprovalDecider const& approve,
                                                  PolicyDecider const& policy) {
    std::vector<DispatchedCall> out(reqs.size());
    if (reqs.empty()) return out;
    // ADR-193: what this run may still spend, so a tool that runs another agent caps the child's budget by it
    // (red team round 1: each child got its own full `child_token_budget` whatever the parent had left).
    // Recomputed before EVERY sequential call (round 2: set once per batch, every spawn in one response saw the
    // same pre-batch remainder, and batches compounded with depth), since each child's charge lands in
    // `run_tokens_consumed()` the moment it returns.
    auto const remaining_budget = [this]() -> std::optional<std::uint64_t> {
        if (!token_budget_.has_value()) return std::nullopt;
        std::uint64_t const used = run_tokens_consumed();
        return used >= *token_budget_ ? 0 : *token_budget_ - used;
    };
    effect_context_.remaining_token_budget = remaining_budget();

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
        // ADR-170 (issue #64): bound on the per-call COPY, so no reset is needed -- the copy dies
        // with the job. Unlike report_progress it captures no `call_id`: a sandbox exec carries
        // its own correlation id (`SandboxExec::exec_id`) and is emitted from beneath the tool,
        // by sandbox-layer code that has no notion of which model tool call it serves.
        ctx.sandbox_exec_sink = [this, run_id = ctx.run_id](
                                     run_event_kind kind, run_event_payload::SandboxExec p) {
            emit_run_event_for(run_id, kind, std::move(p));
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
                // ADR-170 (issue #64): its own independent set/clear pair around this same call,
                // the discipline `codeact_preseeded_answers`/`report_progress` already establish.
                effect_context_.sandbox_exec_sink =
                    [this](run_event_kind kind, run_event_payload::SandboxExec p) {
                        emit_run_event(kind, std::move(p));
                    };
                effect_context_.remaining_token_budget = remaining_budget();  // ADR-193 round 2
                ToolInvocationAudit audit;
                ToolResult result =
                    invoke_tool(tool_table, held, req, effect_context_, approve, &audit, policy);
                effect_context_.report_progress = [](ContentItem) {};
                effect_context_.sandbox_exec_sink =
                    [](run_event_kind, run_event_payload::SandboxExec) {};
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
                audit.principal_delegation_root = ctx.principal.delegation_root;  // ADR-193
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
    // ADR-193 round 2: concurrent calls cannot see each other's charges, so the remainder left after every
    // sequential call above is SPLIT evenly among them (a share of 0 refuses a spawn). Each can then overshoot
    // its share by one in-flight model call of its own; without the split every one could spend the whole.
    if (std::optional<std::uint64_t> const left = remaining_budget(); left.has_value()) {
        std::size_t members = 0;
        for (auto const& job : jobs) members += job.ctxs.size();
        if (members > 0) {
            for (auto& job : jobs)
                for (auto& ctx : job.ctxs) ctx.remaining_token_budget = *left / members;
        }
    }
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

// agent_session.hpp:2256
bool AgentSessionCore::should_retry_stream(error const& err) const {
    if (bound_model_route() != model_route::direct) {  // ADR-199: was `if constexpr` on the client's type
        (void)err;
        return false;
    } else {
        if (stream_retries_used_ >= stream_retries_) return false;
        if (err.code != "run.stream_incomplete") return false;
        if (!last_stream_failure_.has_value()) return false;
        if (effect_context_.cancellation.stop_requested()) return false;
        detail::StreamFailure const& f = *last_stream_failure_;
        // "Mid-answer" is `any_update_seen` OR a truncation: the provider workers hold updates back
        // until a block completes, so a stream cut after a 200 head but before the first finished
        // block has delivered nothing yet -- while `net.stream_truncated` can only be raised AFTER
        // a successful response head, so it is mid-response by construction, not a rejection.
        // `net.stream_read_failed` is the same by construction: a read that fails after the head.
        return (f.any_update_seen || f.inner.code == "net.stream_truncated" ||
                f.inner.code == "net.stream_read_failed") &&
               f.inner.klass == failure_class::transient && f.inner.code != "net.cancelled";
    }
}

// agent_session.hpp:2384
task<result<AgentResponse>> AgentSessionCore::resolve_codeact_ask(ResolveInteraction const& request,
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
    // ADR-196 §7 (issue #111 A6): validate first, commit second. The answer used to be appended (and
    // `input_resolved` announced) BEFORE the fallible tool-table step below; when that failed, the interaction
    // stayed open with the answer kept, so the retry's answer landed on the NEXT question -- the script ran with
    // [first try, retry] while a human had only ever seen one question. Now every fallible step runs first; the
    // answer is committed only once nothing before the replay can fail.
    result<ToolTable> const resumed_table = co_await resume_tool_table(suspended_rounds_.at(interaction_id));
    if (!resumed_table) co_return std::unexpected(resumed_table.error());
    ToolTable const& tool_table = *resumed_table;
    rec_it = pending_codeact_asks_.find(interaction_id);  // re-found after the await, not assumed still valid
    if (rec_it == pending_codeact_asks_.end()) {
        co_return std::unexpected(error{failure_class::fatal, "the codeact-ask record vanished during the resume",
                                         "session.resolve_interaction.codeact_ask_record_missing"});
    }
    rec_it->second.answers_so_far.push_back(*request.answer);

    emit_run_event(run_event_kind::input_resolved,
                    run_event_payload::InteractionRef{interaction_id, request.approver_id.value_or(std::string{})});
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
    // ADR-170 (issue #64): the third and last of this class's `report_progress` bracket sites --
    // a codeact-ask replay re-invokes `execute_code`, which is exactly a sandbox-executing tool,
    // so leaving this one out would make the replayed run silently quieter than the original.
    effect_context_.sandbox_exec_sink =
        [this](run_event_kind kind, run_event_payload::SandboxExec p) {
            emit_run_event(kind, std::move(p));
        };
    ToolInvocationAudit audit;
    // Named `tool_result`, not `result` -- the latter would shadow the `agentengine::result<T>`
    // alias template for the rest of this function's scope (a real MSVC C2760 hit while writing
    // this, not a hypothetical style nit -- `result<void>` below would otherwise parse as
    // `(local variable result) < void` instead of a template-id).
    // Round-3 red team (ADR-192): a script that unattended mode approved is re-checked against the session's
    // current setting, policy and veto before the replay re-runs it from the top -- clearing unattended mode while
    // the script waited on its question used to change nothing. One approved by a human or by the host's own
    // decider replays as before.
    bool replay_allowed = true;
    if (rec_it->second.approved_unattended_by) {
        ToolDescriptor const* td = tool_table.find(req.tool_name);
        approval_outcome const outcome =
            td == nullptr ? approval_outcome::deny
                          : resolve_approval_outcome(*td, call_provenance::text_derived, effect_context_.principal,
                                                     /*arguments_tainted=*/true, policy_decider_);
        if (outcome == approval_outcome::deny) {
            replay_allowed = false;
        } else if (outcome == approval_outcome::needs_decider) {
            ApprovalDecider const now = effective_approval_decider(tool_table);
            replay_allowed = now && now(effect_context_.principal, req.tool_name, json::dump(req.arguments));
        }
    }
    ToolResult tool_result =
        replay_allowed
            ? invoke_tool(tool_table, held, req, effect_context_, one_shot_approve, &audit)
            : make_denial_result(req.call_id, "the replay was not approved again after unattended mode changed",
                                 "tool.approval_denied");
    if (!replay_allowed) {
        audit.call_id = req.call_id;
        audit.ok      = false;
    }
    effect_context_.codeact_preseeded_answers.clear();
    effect_context_.report_progress = [](ContentItem) {};
    effect_context_.sandbox_exec_sink = [](run_event_kind, run_event_payload::SandboxExec) {};
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
    (void)co_await bound_on_turn_end(
        TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                            history_.size() - response_msg_index}},
        effect_context_);
    emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
    ++effect_context_.turn_index;
    co_return co_await run_rounds();
}

// agent_session.hpp:2605
task<result<AgentResponse>> AgentSessionCore::finish_hook_processed_round(PendingHookDecisionRound round,
                                                          ToolTable const& tool_table,
                                                          ApprovalDecider const& approve,
                                                          PolicyDecider const& policy) {
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
    (void)co_await bound_on_turn_end(
        TurnView{std::span<Message const>{history_.data() + response_msg_index,
                                            history_.size() - response_msg_index}},
        effect_context_);
    emit_run_event(run_event_kind::turn_finished, run_event_payload::Turn{effect_context_.turn_index});
    ++effect_context_.turn_index;
    co_return co_await run_rounds();
}

// agent_session.hpp:2665
task<result<AgentResponse>> AgentSessionCore::resolve_hook_decision(ResolveInteraction const& request,
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
    // ADR-179 stage 0: work on a COPY and erase the stored state only once the interaction has actually
    // been resolved (below). This used to move-and-erase HERE, before the answer check, so a resume
    // missing one answer returned `session.hook_decision.incomplete` with the interaction still open
    // and nothing left to resume -- the retry hit `session.hook_decision.unknown` and start_run() stayed
    // refused for good (`run.approval_pending`). A validation failure must leave the round retryable.
    PendingHookDecisionRound round = it->second;

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

    // Copied: resolve_interaction_record() below drops the record with the interaction.
    SuspendedRoundRecord const round_record = suspended_rounds_.at(interaction_id);
    result<void> const resolved = resolve_interaction_record(interaction_id);
    if (!resolved) co_return std::unexpected(resolved.error());
    pending_hook_decisions_.erase(interaction_id);  // resolved: the stored round is no longer resumable
    // ADR-196 §7 (issue #111 A5): the resolve names who answered, like an approval does.
    emit_run_event(run_event_kind::input_resolved,
                    run_event_payload::InteractionRef{interaction_id, request.approver_id.value_or(std::string{})});

    // ADR-196 §7 (issue #111 A4): the round's recorded descriptors -- so an approval mode the turn middleware
    // tightened for this turn is what the check below sees (it used to see the provider's raw one, and a call the
    // middleware made always_require ran with no human after the hook allowed it).
    result<ToolTable> const resumed_table = co_await resume_tool_table(round_record);
    if (!resumed_table) co_return std::unexpected(resumed_table.error());
    ToolTable const& tool_table = *resumed_table;

    // Deliberately NOT `one_shot_approve` here, unlike resolve_interaction()'s approved branch --
    // see this function's own top comment. Re-checked with the SAME gating condition
    // run_rounds()'s own suspend-for-approval pre-check uses (`suspend_for_approval_ &&
    // !approval_decider_`) -- a session that never opts into suspending for approval, or that
    // already has a real ApprovalDecider wired, must behave identically here to everywhere else:
    // invoke_tool() consults `approval_decider_` directly (denying if unset), it never cascades
    // into a suspend that could not otherwise have happened for this session's configuration.
    bool any_still_needs_approval = false;
    if (approval_waits_for_human()) {
        for (HookProcessedCall const& hc : round.calls) {
            if (hc.outcome != hook_call_outcome::pass_through) continue;
            if (!hc.request.arguments_parse_error.empty()) continue;  // ADR-197: refused at dispatch anyway
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
        // ADR-196 §7: the cascade carries the same round forward -- its descriptors and its message.
        SuspendedRoundRecord& next_record = suspended_rounds_[next.interaction_id];
        next_record.offered_tools = tool_table.descriptors();
        next_record.message_index = round_record.message_index;
        emit_run_event(run_event_kind::input_required,
                        run_event_payload::InteractionRef{next.interaction_id});
        for (HookProcessedCall const& hc : pending_hook_decisions_[next.interaction_id].calls) {
            if (hc.outcome == hook_call_outcome::pass_through) {
                // ADR-182 P1: same payload fields as run_rounds()'s own emit site. ADR-196 (issue #104
                // BUG-1): only a call that actually needs a decider is named.
                ToolDescriptor const* td = tool_table.find(hc.request.tool_name);
                bool const needs = td != nullptr && hc.request.arguments_parse_error.empty() &&
                                   resolve_approval_outcome(*td, hc.request.provenance,
                                                            effect_context_.principal,
                                                            /*arguments_tainted=*/true, policy_decider_) ==
                                       approval_outcome::needs_decider;
                if (!needs) continue;
                emit_run_event(run_event_kind::approval_requested,
                                run_event_payload::ApprovalRequested{
                                    hc.request.call_id, next.interaction_id, hc.request.tool_name,
                                    json::dump(hc.request.arguments), needs});
                pending_hook_decisions_[next.interaction_id].approval_requested_call_ids.push_back(
                    hc.request.call_id);  // ADR-183
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
    co_return co_await finish_hook_processed_round(std::move(round), tool_table, effective_approval_decider(tool_table),
                                                      policy_decider_);
}

// agent_session.hpp:2798
std::unexpected<error> AgentSessionCore::finish_canceled() {
    emit_run_event(run_event_kind::run_canceled);
    return std::unexpected(error{failure_class::fatal, "the run was canceled", "run.canceled"});
}

// agent_session.hpp:2803
task<result<AgentResponse>> AgentSessionCore::run_rounds() {
    CapabilitySet const empty_caps = CapabilitySet::grant_root({});
    // ADR-061 §20.6: per-request, not session-level -- effect_context_.capabilities is freshly
    // written by apply_dispatch_authority() at the top of every real entry point (start_run(),
    // resolve_interaction()) before this is ever reached.
    CapabilitySet const& held      = effect_context_.capabilities ? *effect_context_.capabilities : empty_caps;

    for (; !max_turns_.has_value() || effect_context_.turn_index < *max_turns_;
         ++effect_context_.turn_index) {
        if (effect_context_.cancellation.stop_requested()) co_return finish_canceled();  // ADR-178
        emit_run_event(run_event_kind::turn_started, run_event_payload::Turn{effect_context_.turn_index});

        // ADR-061 §20.7: effect_context_.principal, not principal_ -- per-request, not session-level.
    SessionContext session_ctx{session_id_, effect_context_.principal, history_};
        result<ContextContribution> contribution =
            co_await bound_on_context(session_ctx, effect_context_);
        if (!contribution) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{contribution.error().code, contribution.error().message,
                                                      "run.context_unavailable"});
            co_return std::unexpected(contribution.error());
        }

        // Gap-audit finding 20 / 003 §8 Q2 ("exclude from context assembly, never translate"):
        // strip any `Reasoning` content item this turn's contribution carries that did NOT
        // originate from the currently-bound backend, before it ever reaches `ChatRequest`.
        // Skipped entirely (no filtering, unchanged behavior) when `ChatClientT` doesn't expose
        // an identity to compare against -- the template's hook (ADR-199) decides that with
        // `if constexpr` on `HasProducerChatClientId`, so every mock/test ChatClientT is unaffected.
        bound_filter_cross_provider_reasoning(*contribution);

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
        if (held.find_schedule().has_value()) contribution->tools.push_back(schedule_wakeup_tool());

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
        // is the adapter this seam needs -- `AgentSession` calls `bound_on_context()`
        // directly, never `assemble_context()` itself, so there is no `ContextAssemblyResult`
        // already in hand the way ADR-066's own seam has one.
        if (turn_middleware_hook_) {
            agentengine::ContextAssemblyResult assembled_for_turn{std::move(*contribution), {}};
            agentengine::TurnContext turn_ctx{assembled_for_turn};
            result<std::monostate> const turn_outcome = co_await turn_middleware_hook_(turn_ctx);
            *contribution = std::move(assembled_for_turn.combined);
            if (!turn_outcome) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{turn_outcome.error().code, turn_outcome.error().message,
                                                          "run.turn_denied"});
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
        // Moved, not copied: `contribution` is this iteration's own local and is never read again
        // after this line, and `tool_table` above already took the copy of the tools it keeps.
        // This used to deep-copy the whole assembled history into the request once per turn.
        ChatRequest request{std::move(contribution->messages), std::move(contribution->tools)};
        // ADR-191/192: the ONE place `ContentItem::approval` and `deliver_as_instructions` are granted -- after
        // every provider has contributed and history is in, just before the request leaves. Both are cleared on
        // every item first, so nothing a provider, a plugin or a stored message carries survives; then granted only
        // by the host's settings (see the method).
        if (!pinned_context_.empty()) {  // ADR-193
            request.messages.insert(request.messages.begin(), pinned_context_.begin(), pinned_context_.end());
        }
        apply_approved_lessons(request);
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
        if (!bound_has_chat_client()) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{"run.no_chat_client", "no ChatClientT configured"});
            co_return std::unexpected(
                error{failure_class::contract, "no ChatClientT configured", "run.no_chat_client"});
        }
        // ADR-177: each attempt is a COMPLETE started..finished bracket, so a consumer's per-call
        // state (AG-UI's open message, a UI's spinner) closes cleanly before the next one opens.
        // A failed attempt appended nothing to `history_` (that happens below, only on success),
        // so the retry re-sends the identical request.
        result<ChatResponse> response = std::unexpected(
            error{failure_class::contract, "unreachable: no model call attempted", "run.internal"});
        // ADR-193: what delegated agents spent since the last model call is this run's spending too -- folded
        // in, and the budget checked, before another call is made (I8 across a delegation tree).
        if (fold_delegated_usage() && token_budget_.has_value() && run_tokens_consumed_ > *token_budget_) {
            emit_run_event(run_event_kind::run_failed,
                           run_event_payload::RunFailed{"run.token_budget_exceeded",
                                                        "token budget exceeded (delegated agents included)"});
            co_return std::unexpected(error{failure_class::resource,
                                            "token budget exceeded (delegated agents included)",
                                            "run.token_budget_exceeded"});
        }
        for (;;) {
            emit_run_event(run_event_kind::model_call_started);
            response = co_await run_model_call(request, effect_context_);
            emit_run_event(run_event_kind::model_call_finished);
            if (response || !should_retry_stream(response.error())) break;
            ++stream_retries_used_;
            detail::StreamFailure const& f = *last_stream_failure_;
            // 013 §1: the output streamed since the `model_call_started` above is VOID. Emitted after
            // that call's `model_call_finished` and before the next `model_call_started`, so a
            // consumer that keeps per-call state can retract it at a clean boundary.
            // I8: providers do not say whether they bill a stream that never finished, so it is
            // treated as billed (project owner, 2026-09-21; docs/research/2026-09-21-billing-of-
            // interrupted-streams.md). The dead stream reported no `Usage`, so the charge is an
            // ESTIMATE -- the request as input plus the output it delivered -- and it goes against the
            // token BUDGET only, never into `run_usage_` (which reports what a provider actually said).
            std::uint64_t const charged =
                detail::estimate_request_tokens(request) + agentengine::estimate_tokens_for_bytes(f.bytes_seen);
            discarded_tokens_estimate_ += charged;
            run_tokens_consumed_ += charged;
            emit_run_event(run_event_kind::model_output_discarded,
                            run_event_payload::ModelOutputDiscarded{
                                stream_retries_used_, stream_retries_ + 1,
                                f.inner.message + (f.inner.code.empty() ? "" : " (" + f.inner.code + ")"),
                                charged});
            // The charge can itself break the budget -- and a retry must not go ahead if it has.
            if (token_budget_.has_value() && run_tokens_consumed_ > *token_budget_) {
                emit_run_event(run_event_kind::run_failed,
                                run_event_payload::RunFailed{
                                    "run.token_budget_exceeded",
                                    "per-run token budget exceeded (including an estimate for a discarded attempt)"});
                co_return std::unexpected(error{failure_class::resource,
                                                 "per-run token budget exceeded (including an estimate for a "
                                                 "discarded attempt)",
                                                 "run.token_budget_exceeded"});
            }
        }
        if (!response && (response.error().code == "run.canceled" ||
                           effect_context_.cancellation.stop_requested())) {
            co_return finish_canceled();  // ADR-178: a call the run's own cancel cut short is not a failure
        }
        if (!response) {
            emit_run_event(run_event_kind::run_failed,
                            run_event_payload::RunFailed{response.error().code, response.error().message, "run.chat_failed"});
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

        // ADR-178: the model answered but the run was told to stop while it did. Its usage above is
        // real and stays charged (attributable); the response is dropped, so no tool it asked for runs.
        if (effect_context_.cancellation.stop_requested()) co_return finish_canceled();

        // ADR-196 §7 (issue #111 A2/A3): the engine owns call identity. A call id repeated within this response
        // is renamed before anything records it, so every approval, decision, hook answer, audit record and tool
        // result addresses exactly one call. Announced, so the rename is attributable (I4).
        for (agentengine::CallIdRename const& rename : agentengine::make_call_ids_unique(response->message)) {
            emit_run_event(run_event_kind::warning,
                            run_event_payload::Warning{"the model repeated call id \"" + rename.from +
                                                       "\" in one response; the engine renamed that call to \"" +
                                                       rename.to + "\""});
        }
        std::size_t const response_msg_index = history_.size();
        history_.push_back(response->message);

        std::vector<ToolCall> const calls = tool_calls_of(response->message);
        if (calls.empty()) {
            (void)co_await bound_on_turn_end(
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
        // ADR-182 P1: which calls needed a decider, per call, for the approval_requested
        // payload's `needs_approval` field. Filled by the loop below, which therefore no longer
        // stops at the first gated call.
        std::vector<bool> call_needs_approval(calls.size(), false);
        if (approval_waits_for_human()) {  // ADR-192: unattended mode never suspends
            for (std::size_t i = 0; i < calls.size(); ++i) {
                // A call the hook stage already denied is finished -- its outcome is already
                // decided, never re-litigated by approval.
                if (hook_touched_round && processed[i].outcome == hook_call_outcome::denied) continue;
                // Both arms are lvalues, so the reference binds to `processed[i].request` itself.
                // It used to be `cond ? processed[i].request : tool_call_request_of(...)`: an
                // lvalue and a prvalue make the whole conditional a prvalue, which silently COPIED
                // the post-hook request, parsed argument tree included, just to read two fields.
                std::optional<ToolCallRequest> parsed_i;
                ToolCallRequest const& req_i = hook_touched_round
                                                   ? processed[i].request
                                                   : parsed_i.emplace(tool_call_request_of(calls[i], i));
                // ADR-197: a call whose arguments are not valid JSON is refused at dispatch whatever anyone
                // decides, so it never waits on a human (the approver would only be shown unparseable text).
                if (!req_i.arguments_parse_error.empty()) continue;
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
                    call_needs_approval[i] = true;
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
            // ADR-196: what the model was offered this round, and which calls wait on the decision.
            SuspendedRoundRecord& round_record = suspended_rounds_[interaction.interaction_id];
            round_record.offered_tools = tool_table.descriptors();  // ADR-196 §7: as the turn middleware left them
            round_record.message_index = response_msg_index;
            for (std::size_t i = 0; i < calls.size(); ++i) {
                if (call_needs_approval[i]) round_record.gated_call_ids.push_back(calls[i].call_id);
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
            for (std::size_t i = 0; i < calls.size(); ++i) {
                // ADR-196 (issue #104 BUG-1): only a call that actually waits on the decision is named. It used
                // to be every call in the round, so a UI could not tell which calls were waiting on a human.
                if (!call_needs_approval[i]) continue;
                // ADR-182 P1: the arguments the approval check actually judged -- the post-hook
                // request's when the hook stage ran (it may have rewritten them), else the
                // model's own text. `processed` was moved into pending_hook_decisions_ above,
                // so a hook-touched round reads it back from there.
                std::string arguments_json =
                    hook_touched_round
                        ? json::dump(pending_hook_decisions_[interaction.interaction_id].calls[i].request.arguments)
                        : calls[i].arguments_json;
                // ADR-183: record what was asked, so resolution pairs with it exactly (a plain round's list
                // is in `suspended_rounds_`, ADR-196).
                if (hook_touched_round) {
                    pending_hook_decisions_[interaction.interaction_id].approval_requested_call_ids.push_back(
                        calls[i].call_id);
                }
                emit_run_event(run_event_kind::approval_requested,
                                run_event_payload::ApprovalRequested{
                                    calls[i].call_id, interaction.interaction_id, calls[i].tool_name,
                                    std::move(arguments_json), call_needs_approval[i]});
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
            // Moved out of `processed`, which nothing reads past this loop (the suspend branch that
            // keeps it for resume has already returned). A conditional mixing `processed[i].request`
            // with a prvalue copied it instead.
            if (hook_touched_round) {
                reqs.push_back(std::move(processed[i].request));
            } else {
                reqs.push_back(tool_call_request_of(calls[i], i));
            }
            req_positions.push_back(i);
        }

        std::vector<DispatchedCall> dispatched =
            dispatch_tool_calls(reqs, tool_table, held, effective_approval_decider(tool_table), policy_decider_);

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
                // Round-3 red team (ADR-192): when unattended mode is what approved this call, the replay
                // re-checks it (resolve_codeact_ask()).
                if (unattended_by_) {
                    ToolDescriptor const* td = tool_table.find(req.tool_name);
                    if (td != nullptr &&
                        resolve_approval_outcome(*td, req.provenance, effect_context_.principal,
                                                 req.arguments_tainted, policy_decider_) ==
                            approval_outcome::needs_decider) {
                        record.approved_unattended_by = *unattended_by_;
                    }
                }
                pending_codeact_asks_[interaction.interaction_id] = std::move(record);
                SuspendedRoundRecord& ask_record = suspended_rounds_[interaction.interaction_id];  // ADR-196 §7
                ask_record.offered_tools = tool_table.descriptors();
                ask_record.message_index = response_msg_index;

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
        (void)co_await bound_on_turn_end(
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

// agent_session.hpp:3492
result<void> AgentSessionCore::unattended_detail_refuse(char const* what) {
    return std::unexpected(error{failure_class::contract,
                                 std::string(what) +
                                     ": an operator id is required (non-blank, no control characters) -- the audit "
                                     "names it (I4)",
                                 "session.unattended_operator_missing"});
}

// agent_session.hpp:3502
void AgentSessionCore::apply_approved_lessons(ChatRequest& request) {
    // Pass 1: clear every mark, and find the approval each tainted system text rests on.
    struct Candidate {
        ContentItem*        item;
        Text*               text;
        std::string         lesson;
        ApprovedLessonMatch match;
        bool                deliver = true;
    };
    std::vector<Candidate> candidates;
    for (Message& m : request.messages) {
        for (ContentItem& item : m.content) {
            item.approval.clear();
            item.deliver_as_instructions = false;
            if (m.role != role::system || !item.tainted || approved_lessons_ == nullptr) continue;
            auto* text = std::get_if<Text>(&item.value);
            if (text == nullptr) continue;
            std::string_view const candidate = agentengine::approved_lesson_candidate_text(text->text);
            // ADR-193: a delegated principal (a spawned child, a fresh id nobody could approve for) is matched
            // against its chain's root -- the owner the approval was actually given to. Round 3: scoped by tenant
            // too -- an approval never reaches a same-named principal of another tenant. A chain stays inside its
            // root's tenant (derive_on_behalf_of copies it).
            auto match = approved_lessons_->find({principal_.tenant_id, principal_.id}, candidate);
            if (!match && !principal_.delegation_root.empty()) {
                match = approved_lessons_->find({principal_.tenant_id, principal_.delegation_root}, candidate);
            }
            if (match) candidates.push_back(Candidate{&item, text, std::string(candidate), std::move(*match)});
        }
    }

    // Pass 2 (ADR-191 round 4): an approval that names the form its Tier-1 screen measured is delivered only in
    // that form -- a lesson screened fenced with the human wording is never shipped unfenced, or with the
    // automated-reviewer wording, on the strength of that screen. The guidance wording is request-wide: it names
    // the automated reviewer when any automatic approval is delivered. So automatic approvals are settled first
    // (a delivered one always ships with that wording, its own form), then human ones against the result.
    bool const fence_off = system_fence_disabled_by_.has_value();
    auto const fits = [this, fence_off](Candidate const& c, bool any_automatic) {
        std::string const& screened = c.match.approval.screened_delivery;
        return screened.empty() || screened == agentengine::approved_lesson_delivery_name(
                                                   any_automatic, approved_lesson_level_, fence_off);
    };
    bool any_automatic = false;
    for (Candidate& c : candidates) {
        if (!agentengine::is_automatic_approval_id(c.match.approval_id)) continue;
        c.deliver = fits(c, /*any_automatic=*/true);
        any_automatic = any_automatic || c.deliver;
    }
    for (Candidate& c : candidates) {
        if (agentengine::is_automatic_approval_id(c.match.approval_id)) continue;
        c.deliver = fits(c, any_automatic);
    }
    for (Candidate& c : candidates) {
        LessonApproval const& a = c.match.approval;
        if (!c.deliver) {
            // Delivered as the ordinary memory it is (fenced, label kept) -- the approval is not used.
            emit_run_event(run_event_kind::policy_decision,
                           run_event_payload::PolicyDecision{
                               "approved lesson withheld: approval " + c.match.approval_id +
                               " was screened as " + a.screened_delivery + " but would ship as " +
                               std::string(agentengine::approved_lesson_delivery_name(
                                   any_automatic || agentengine::is_automatic_approval_id(c.match.approval_id),
                                   approved_lesson_level_, fence_off)) +
                               "; delivered as ordinary memory"});
            continue;
        }
        // The confidence label ("model-inferred, unverified") is dropped: the fence now says what the block is,
        // and a label that says the opposite would contradict it.
        c.text->text = std::move(c.lesson);
        c.item->approval = c.match.approval_id;
        // Red team (MINOR): with the fence off an approved lesson goes out unfenced whatever its level, so the
        // event names what is actually sent.
        bool const as_instructions = approved_lesson_level_ == agentengine::approved_lesson_level::instructions;
        c.item->deliver_as_instructions = as_instructions || fence_off;
        std::string const delivered =
            as_instructions ? "instructions (level set by operator " + lesson_level_set_by_.value_or("?") + ")"
                            : (fence_off ? "instructions (fence off by operator " + *system_fence_disabled_by_ + ")"
                                         : std::string("guidance"));
        emit_run_event(run_event_kind::policy_decision,
                       run_event_payload::PolicyDecision{
                           "approved lesson delivered as " + delivered + ": approval " + c.match.approval_id +
                           ", approved by " + a.approver_id + (a.simulated ? " (simulated)" : "") +
                           (a.automatic ? " (automatic)" : "") + ", acknowledgement " +
                           (a.acknowledgement.empty() ? std::string("none") : a.acknowledgement) +
                           (a.screened_delivery.empty() ? std::string{}
                                                        : ", screened as " + a.screened_delivery)});
    }

    // ADR-192: with the fence off, every remaining tainted system text goes out as instructions too.
    std::size_t unfenced = 0;
    if (fence_off) {
        for (Message& m : request.messages) {
            if (m.role != role::system) continue;
            for (ContentItem& item : m.content) {
                if (!item.tainted || item.deliver_as_instructions) continue;
                auto const* text = std::get_if<Text>(&item.value);
                if (text == nullptr || text->text.empty()) continue;
                item.deliver_as_instructions = true;
                ++unfenced;
            }
        }
    }
    if (unfenced != 0) {
        emit_run_event(run_event_kind::policy_decision,
                       run_event_payload::PolicyDecision{
                           "system-channel fence off (host setting, operator " + *system_fence_disabled_by_ +
                           "): " + std::to_string(unfenced) + " tainted system item(s) delivered as instructions"});
    }
}

// agent_session.hpp:3621
agentengine::ApprovalDecider AgentSessionCore::effective_approval_decider(ToolTable const& tool_table) {
    if (!unattended_by_) {
        return approval_decider_ ? agentengine::ApprovalDecider{std::ref(approval_decider_)}
                                 : agentengine::ApprovalDecider{};
    }
    return [this, op = *unattended_by_, tools = &tool_table](Principal const& caller, std::string_view tool_name,
                                                             std::string const& canonical_args) {
        if (!unattended_by_) return approval_decider_ ? approval_decider_(caller, tool_name, canonical_args) : false;
        // The call id is not known here; a short hash of the exact arguments tells two calls to one tool apart
        // (correlation only, not a digest).
        char args_id[17];
        std::snprintf(args_id, sizeof(args_id), "%016llx",
                      static_cast<unsigned long long>(std::hash<std::string>{}(canonical_args)));
        if (policy_decider_) {
            ToolDescriptor const* td = tools->find(std::string(tool_name));
            if (td != nullptr &&
                policy_decider_(caller, *td, /*arguments_tainted=*/true) == policy_decision::auto_deny) {
                emit_run_event(run_event_kind::policy_decision,
                               run_event_payload::PolicyDecision{
                                   "unattended mode (operator " + op + "): tool " + std::string(tool_name) +
                                   " denied by the host's policy, caller " + caller.id + ", arguments #" + args_id});
                return false;
            }
        }
        if (std::shared_ptr<agentengine::ApprovalDecider const> const veto = unattended_veto_;
            veto && !(*veto)(caller, tool_name, canonical_args)) {
            emit_run_event(run_event_kind::policy_decision,
                           run_event_payload::PolicyDecision{
                               "unattended mode (operator " + op + "): tool " + std::string(tool_name) +
                               " vetoed by the host, caller " + caller.id + ", arguments #" + args_id});
            return false;
        }
        // The veto switched unattended mode off while it ran: the default is back for this call too.
        if (!unattended_by_) return approval_decider_ ? approval_decider_(caller, tool_name, canonical_args) : false;
        emit_run_event(run_event_kind::policy_decision,
                       run_event_payload::PolicyDecision{
                           "unattended approval (host setting, operator " + op + "): tool " +
                           std::string(tool_name) + " approved with no human, caller " + caller.id +
                           ", arguments #" + args_id});
        return true;
    };
}

// agent_session.hpp:3788
bool AgentSessionCore::fold_delegated_usage() {
    agentengine::Usage u;
    std::uint64_t extra = 0;
    {
        std::lock_guard<std::mutex> lock(delegated_charges_->mutex);
        u = delegated_charges_->usage;
        extra = delegated_charges_->extra_tokens;
        delegated_charges_->usage = agentengine::Usage{};
        delegated_charges_->extra_tokens = 0;
    }
    std::uint64_t const tokens = u.input_tokens + u.output_tokens;
    if (tokens == 0 && extra == 0 && u.cost_estimate == 0.0) return false;
    run_tokens_consumed_ += tokens + extra;
    discarded_tokens_estimate_ += extra;
    run_usage_.input_tokens += u.input_tokens;
    run_usage_.output_tokens += u.output_tokens;
    run_usage_.cached_input_tokens += u.cached_input_tokens;
    run_usage_.reasoning_tokens += u.reasoning_tokens;
    run_usage_.cost_estimate += u.cost_estimate;
    run_usage_.cache_write_tokens += u.cache_write_tokens;
    return true;
}

// agent_session.hpp:3814
void AgentSessionCore::forward_run_event(RunEvent const& ev) {
    if (!run_event_producer_.valid() && !run_event_tap_attached_) return;
    run_event_payload::DelegatedEvent wrapped;
    wrapped.child_run_id = ev.run_id;
    if (ev.kind == run_event_kind::delegated_event) {
        if (auto const* inner = std::get_if<run_event_payload::DelegatedEvent>(&ev.payload)) {
            wrapped.child_run_id = inner->child_run_id;
            wrapped.depth = inner->depth + 1;
            wrapped.inner = inner->inner;
        }
    }
    if (!wrapped.inner) wrapped.inner = std::make_shared<RunEvent const>(ev);
    emit_run_event_for(effect_context_.run_id, run_event_kind::delegated_event, std::move(wrapped));
}

}  // namespace agentengine::rt
