// #120 S5 (ADR-201): the tool-call pipeline's function bodies (006 §3), moved verbatim out of
// include/agentengine/core/tool_pipeline.hpp so they are compiled once here instead of in every file that
// includes the header (206 at the time of the move). The header keeps every type, every declaration and its
// comment, and bodies under 6 lines. The rules each function implements are
// documented at its declaration in the header.

#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

std::uint64_t argument_digest(std::string_view canonical_args_json) noexcept {
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    for (unsigned char c : canonical_args_json) {
        h ^= c;
        h *= 0x0000'0100'0000'01B3ULL;
    }
    return h;
}

result<void> authorize_reexecution(effect_class cls, bool operator_acknowledged) {
    switch (cls) {
        case effect_class::pure:
        case effect_class::idempotent:
            return {};  // re-run freely / under the original key (F1 already derives that key)
        case effect_class::at_most_once:
            if (!operator_acknowledged) {
                return std::unexpected(error{
                    failure_class::policy,
                    "at-most-once effect requires explicit operator acknowledgement before re-execution",
                    "effect.reexecution_requires_ack"});
            }
            return {};
    }
    return std::unexpected(error{failure_class::fatal, "unreachable effect_class", "effect.unreachable_class"});
}

}  // namespace agentengine

namespace agentengine {
namespace tool_pipeline_detail {

ToolResult make_error_result(std::string call_id, error const& e) {
    ToolResult r;
    r.call_id = std::move(call_id);
    r.is_error = true;
    ContentItem item;
    item.value = Error{e.message};
    item.origin = content_origin::tool;
    item.tainted = false;  // host/pipeline-authored, not tool-returned data
    r.content.push_back(std::move(item));
    return r;
}

result<std::pair<ToolResult, std::size_t>> normalize_success(
    std::string call_id, json::Value const& reply_value, EffectContext& ctx) {
    std::string reply_json = json::dump(reply_value);
    std::size_t const reply_bytes = reply_json.size();

    ContentItem item;
    item.origin = content_origin::tool;
    // A tool result is external content and the primary prompt-injection vector (006 §7) --
    // provenance-marked regardless of whether the CALL's own arguments were tainted, and regardless
    // of which branch below runs.
    item.tainted = true;

    if (ctx.tool_result_byte_threshold.has_value() && reply_bytes > *ctx.tool_result_byte_threshold) {
        if (!ctx.blob_sink) {
            return std::unexpected(error{
                failure_class::resource,
                "tool result (" + std::to_string(reply_bytes) + " bytes) exceeds the run's byte "
                "threshold (" + std::to_string(*ctx.tool_result_byte_threshold) + ") and no blob "
                "sink is configured to promote it",
                "tool.result_oversized_no_sink"});
        }
        std::span<std::byte const> const bytes{
            reinterpret_cast<std::byte const*>(reply_json.data()), reply_json.size()};
        auto blob = ctx.blob_sink(bytes, "application/json");
        if (!blob) return std::unexpected(blob.error());
        item.value = Media{*blob, "application/json"};
    } else {
        // `Data::schema_id` is a schema *reference* (a registry id/URI), not the schema body itself
        // -- this milestone has no schema registry (that's 011 MCP-conformance territory), so it
        // stays unset rather than misusing the field to carry `reply_schema_json`'s full text.
        item.value = Data{std::move(reply_json), std::nullopt};
    }

    ToolResult ok_result;
    ok_result.call_id = std::move(call_id);
    ok_result.is_error = false;
    ok_result.content.push_back(std::move(item));
    return std::make_pair(std::move(ok_result), reply_bytes);
}

bool is_auto_declassifiable_text_derived_call(ToolDescriptor const& tool) noexcept {
    if (tool.effect_class != agentengine::effect_class::pure) return false;
    return std::all_of(tool.capability_ceiling.begin(), tool.capability_ceiling.end(),
                        [](Capability const& c) {
                            return is_inert_for_text_derived_declassification(capability_kind_of(c));
                        });
}

}  // namespace tool_pipeline_detail
}  // namespace agentengine

namespace agentengine {

bool tool_call_requires_approval(ToolDescriptor const& tool,
                                 call_provenance provenance) noexcept {
    using namespace tool_pipeline_detail;
    bool const tool_requires = tool.approval != approval_mode::never_require;
    if (provenance == call_provenance::text_derived) {
        return tool_requires || !is_auto_declassifiable_text_derived_call(tool);
    }
    return tool_requires;
}

approval_outcome resolve_approval_outcome(ToolDescriptor const& tool,
                                          call_provenance provenance,
                                          Principal const& caller,
                                          bool arguments_tainted,
                                          PolicyDecider const& policy) {
    // `text_derived` never gets a policy APPROVAL -- 007 §4's closed declassifier list is untouched
    // by ADR-070; `is_auto_declassifiable_text_derived_call` (via `tool_call_requires_approval`
    // below) stays the sole gate that can lift approval for that provenance. ADR-184: the policy IS
    // consulted for a text_derived call, but only its `auto_deny` is honoured. A deny only narrows,
    // and without it a call the host's policy refuses outright as vendor_structured would instead
    // reach the ApprovalDecider as text_derived -- lower trust, laxer gate.
    if (tool.approval == approval_mode::policy_driven && provenance == call_provenance::text_derived &&
        policy && policy(caller, tool, arguments_tainted) == policy_decision::auto_deny) {
        return approval_outcome::deny;
    }
    if (tool.approval == approval_mode::policy_driven &&
        provenance != call_provenance::text_derived && policy) {
        switch (policy(caller, tool, arguments_tainted)) {
            case policy_decision::auto_approve:
                return approval_outcome::proceed;
            case policy_decision::auto_deny:
                return approval_outcome::deny;
            case policy_decision::require_approval:
                break;  // fall through to the unchanged ApprovalDecider path below
        }
    }
    return tool_call_requires_approval(tool, provenance) ? approval_outcome::needs_decider
                                                            : approval_outcome::proceed;
}

result<AdmittedCall> admit_call(ToolTable const& table, CapabilitySet const& held,
                                ToolCallRequest const& request,
                                Principal const& caller,
                                ApprovalDecider const& approve,
                                PolicyDecider const& policy) {
    // -- step 1: resolve -------------------------------------------------------------------------
    ToolDescriptor const* tool = table.find(request.tool_name);
    if (!tool) {
        return std::unexpected(
            error{failure_class::contract, "unknown tool: " + request.tool_name, "tool.unknown_name"});
    }

    // -- step 2: validate (+ step 3: taint, recorded not deeply propagated) ----------------------
    // ADR-197: argument text that did not parse is refused here, never coerced to `{}` -- a tool
    // whose arguments are all optional would otherwise run on garbage. Nothing is bound yet.
    if (!request.arguments_parse_error.empty()) return std::unexpected(malformed_arguments_error(request));
    // Shape checking is deferred to `tool->invoke`'s call into schema::from_json<Args> below -- a
    // single point of truth for "does this JSON match the declared shape", never a second,
    // hand-rolled check here that could drift from what actually gets parsed.

    // -- step 4/7: authorize + bind --------------------------------------------------------------
    // ADR-009's CapabilitySet::bind() performs both atomically (contains-check, then mint a fresh
    // per-invocation ticket) -- there is no observable difference from doing them as two separate
    // steps within one synchronous call (no concurrent caller could interleave between them here).
    std::vector<BoundCapability> bound;
    bound.reserve(tool->capability_ceiling.size());
    for (Capability const& requirement : tool->capability_ceiling) {
        auto handle = held.bind(requirement);
        if (!handle) {
            // No leaked capability: the error names neither what's missing nor what IS held.
            error e{failure_class::policy, "required capability not held", "tool.capability_not_held"};
            return std::unexpected(e);
        }
        bound.push_back(std::move(*handle));
    }

    // -- step 5: approve ---------------------------------------------------------------------------
    // ADR-023 §6 point 4 / 007 §4 amendment: a tool's `approval_mode::never_require` was authored by
    // its declarer for VENDOR-STRUCTURED calls (a real, trusted wire-format field). A call
    // reconstructed from raw model text is a different, weaker trust class by construction (007 §4:
    // model-supplied text is never itself an authorization decision), so it gets an ADDITIONAL gate
    // (`is_auto_declassifiable_text_derived_call`) that overrides `never_require` for anything with a
    // real capability ceiling -- the exact override the confused-deputy scenario (ADR-023 §4b
    // Finding 1) forced. ADR-184: the extra gate is only ever added on top of the tool's own
    // setting, never substituted for it, so `always_require`/`policy_driven` still apply. A
    // `vendor_structured` call (every caller before this amendment, and every caller that never sets
    // `provenance`) takes the ORIGINAL branch, byte-for-byte unchanged.
    // ADR-070 (decisions/ADR-070-host-configurable-responsibility-boundary.md): `policy` only ever
    // narrows this decision further -- an auto_approve/auto_deny verdict short-circuits
    // `policy_driven`'s existing fail-closed degrade; an unset `policy` reproduces
    // `tool_call_requires_approval()`'s own boolean exactly (see `resolve_approval_outcome`'s own
    // comment), so this is a strict extension of, not a change to, the pre-ADR-070 behavior.
    approval_outcome const outcome = resolve_approval_outcome(*tool, request.provenance, caller,
                                                                request.arguments_tainted, policy);
    if (outcome == approval_outcome::deny) {
        for (auto const& b : bound) b.revoke();  // never bind authority we then refuse to use
        error e{failure_class::policy, "denied by policy", "tool.policy_denied"};
        return std::unexpected(e);
    }
    if (outcome == approval_outcome::needs_decider) {
        std::string canonical_args = json::dump(request.arguments);
        bool approved = approve && approve(caller, request.tool_name, canonical_args);
        if (!approved) {
            for (auto const& b : bound) b.revoke();  // never bind authority we then refuse to use
            error e{failure_class::policy, "approval required and not granted", "tool.approval_denied"};
            return std::unexpected(e);
        }
    }

    // -- step 6: admit -- deferred (Quark 022 not in M2 scope; documented no-op, see file top) ----

    return AdmittedCall{tool, std::move(bound)};
}

AdmittedCallOutcome run_admitted_call(ToolDescriptor const& tool, ToolCallRequest const& request,
                                      EffectContext& ctx, std::vector<BoundCapability>& bound) {
    using namespace tool_pipeline_detail;
    // -- step 8: invoke (deadline checked at the call boundary, not preemptible mid-call) ---------
    if (ctx.deadline.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() > ctx.deadline) {
        for (auto const& b : bound) b.revoke();
        error e{failure_class::resource, "deadline already exceeded", "tool.deadline_exceeded"};
        return AdmittedCallOutcome{make_error_result(request.call_id, e), e, 0};
    }

    ctx.bound_capabilities = &bound;
    result<json::Value> invoke_result = tool.invoke(request.arguments, ctx);
    ctx.bound_capabilities = nullptr;

    // -- step 10: account (revoke unconditionally, success or failure) ----------------------------
    for (auto const& b : bound) b.revoke();

    // -- step 9: normalize --------------------------------------------------------------------------
    if (!invoke_result) {
        error const& e = invoke_result.error();
        return AdmittedCallOutcome{make_error_result(request.call_id, e), e, 0};
    }

    auto normalized = normalize_success(request.call_id, *invoke_result, ctx);
    if (!normalized) {
        error const& e = normalized.error();
        return AdmittedCallOutcome{make_error_result(request.call_id, e), e, 0};
    }
    return AdmittedCallOutcome{std::move(normalized->first), std::nullopt, normalized->second};
}

ToolInvocationAudit make_call_audit(ToolCallRequest const& request,
                                    EffectContext const& ctx,
                                    std::chrono::steady_clock::time_point started,
                                    AdmittedCallOutcome const& outcome) {
    ToolInvocationAudit audit;
    audit.call_id = request.call_id;
    audit.tool_name = request.tool_name;
    audit.ok = !outcome.failure.has_value();
    audit.error_code = outcome.failure ? outcome.failure->code : std::string{};
    audit.result_bytes = outcome.bytes;
    audit.duration = std::chrono::steady_clock::now() - started;
    audit.idempotency_key = derive_idempotency_key(ctx, request.call_index, request.arguments);
    audit.principal_id           = ctx.principal.id;
    audit.principal_tenant_id    = ctx.principal.tenant_id;
    audit.principal_on_behalf_of = ctx.principal.on_behalf_of;
    audit.principal_delegation_root = ctx.principal.delegation_root;  // ADR-193
    return audit;
}

ToolResult invoke_tool(ToolTable const& table, CapabilitySet const& held,
                       ToolCallRequest const& request, EffectContext& ctx,
                       ApprovalDecider const& approve,
                       ToolInvocationAudit* audit_out,
                       // ADR-070: appended last (this file's own established
                       // convention for additive parameters), default `{}` so
                       // every existing positional call site -- including the
                       // three "already resolved by a human" one-shot-approve
                       // sites in rt/agent_session.hpp -- is unaffected and
                       // never re-litigates a decision policy_driven already
                       // deferred to a real human.
                       PolicyDecider const& policy) {
    using namespace tool_pipeline_detail;
    auto const started = std::chrono::steady_clock::now();
    // Phase F1: derived once, unconditionally, from exactly the four inputs 019 §3 names -- never
    // wall-clock, never randomness, so the SAME {run_id, turn_index, call_index, arguments} always
    // yields the SAME key, restart or not.
    IdempotencyKey const idempotency_key = derive_idempotency_key(ctx, request.call_index, request.arguments);

    auto finish = [&](ToolResult result, error const* failure, std::size_t bytes = 0) -> ToolResult {
        if (audit_out) {
            audit_out->call_id = request.call_id;
            audit_out->tool_name = request.tool_name;
            audit_out->ok = (failure == nullptr);
            audit_out->error_code = failure ? failure->code : std::string{};
            audit_out->result_bytes = bytes;
            audit_out->duration = std::chrono::steady_clock::now() - started;
            audit_out->idempotency_key = idempotency_key;
            // ADR-061 §7 R26 / 007 §8. Taken from `ctx`, the identity the call actually ran under.
            audit_out->principal_id            = ctx.principal.id;
            audit_out->principal_tenant_id     = ctx.principal.tenant_id;
            audit_out->principal_on_behalf_of  = ctx.principal.on_behalf_of;
            audit_out->principal_delegation_root = ctx.principal.delegation_root;  // ADR-193
        }
        return result;
    };

    // -- steps 1, 4/7, 5 (resolve, authorize+bind, approve): `admit_call()` above, ADR-160 §5 -------
    auto admission = admit_call(table, held, request, ctx.principal, approve, policy);
    if (!admission) {
        error const& e = admission.error();
        return finish(make_error_result(request.call_id, e), &e);
    }
    ToolDescriptor const* tool = admission->tool;
    std::vector<BoundCapability> bound = std::move(admission->bound);

    // -- step 6: admit -- deferred (Quark 022 not in M2 scope; documented no-op, see file top) ----

    // -- steps 8, 9, 10 (invoke, normalize, account): `run_admitted_call()` above, ADR-160 §5 -------
    AdmittedCallOutcome outcome = run_admitted_call(*tool, request, ctx, bound);
    return finish(std::move(outcome.result), outcome.failure ? &*outcome.failure : nullptr, outcome.bytes);
}

std::vector<ConcurrencyClass> partition_batch(
    std::vector<ToolCallRequest> const& reqs, ToolTable const& table) {
    std::vector<ConcurrencyClass> classes;
    if (reqs.empty()) return classes;

    bool batch_eligible = true;
    for (auto const& req : reqs) {
        ToolDescriptor const* tool = table.find(req.tool_name);
        if (!tool || !(tool->parallelizable || tool->exclusivity_group.has_value())) {
            batch_eligible = false;
            break;
        }
    }

    if (!batch_eligible) {
        for (std::size_t i = 0; i < reqs.size(); ++i) {
            classes.push_back(ConcurrencyClass{concurrency_class_kind::sequential, {i}, {}});
        }
        return classes;
    }

    std::unordered_map<std::string, std::size_t> group_class_index;  // group name -> index into `classes`
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        ToolDescriptor const* tool = table.find(reqs[i].tool_name);  // non-null: proved above
        if (tool->captures_session_state) {
            classes.push_back(ConcurrencyClass{concurrency_class_kind::sequential, {i}, {}});
        } else if (tool->exclusivity_group.has_value()) {
            std::string const& name = *tool->exclusivity_group;
            auto [it, inserted] = group_class_index.try_emplace(name, classes.size());
            if (inserted) {
                classes.push_back(ConcurrencyClass{concurrency_class_kind::exclusivity_group, {i}, name});
            } else {
                classes[it->second].call_indices.push_back(i);
            }
        } else {
            classes.push_back(ConcurrencyClass{concurrency_class_kind::parallel, {i}, {}});
        }
    }
    return classes;
}

result<void> background_task(ToolTable const& table, CapabilitySet const& held,
                             ToolCallRequest const& request, EffectContext ctx,
                             ApprovalDecider const& approve,
                             std::size_t current_background_count,
                             BackgroundTaskCompletion on_complete) {
    using namespace tool_pipeline_detail;

    // -- step 1: resolve ---------------------------------------------------------------------------
    ToolDescriptor const* tool = table.find(request.tool_name);
    if (!tool) {
        return std::unexpected(
            error{failure_class::contract, "unknown tool: " + request.tool_name, "tool.unknown_name"});
    }
    // -- step 2: validate -- ADR-197, the same refusal `admit_call()` makes ----------------------
    if (!request.arguments_parse_error.empty()) return std::unexpected(malformed_arguments_error(request));

    // 006 §6b: an undeclared tool may never be backgrounded -- Tool::declared_backgroundable()'s own
    // fail-closed default.
    if (!tool->backgroundable) {
        return std::unexpected(error{failure_class::policy, "tool is not declared Backgroundable",
                                      "tool.not_backgroundable"});
    }

    // ADR-028: a state-capturing descriptor's `invoke` closure holds a reference into its owning
    // provider's session-scoped state -- backgrounding it would detach a real `std::thread` (below)
    // holding that same reference with no synchronization against `AgentSession::fork_from()`/
    // `clear_in_process_state()` (neither is part of `protocol`, so neither is Quark-`Sequential`-
    // serialized against this detached thread). Refused here, structurally, at the same authorize
    // step the plain `backgroundable` check above already gates -- never reached, not merely
    // discouraged by convention.
    if (tool->captures_session_state) {
        return std::unexpected(error{failure_class::policy,
                                      "a session-state-capturing tool may never be backgrounded",
                                      "tool.state_capturing_not_backgroundable"});
    }

    // ADR-060 §4 (red-team finding, must-fix): `ctx` above is a BY-VALUE parameter -- a real copy of
    // whatever the caller's own `EffectContext` looked like at the call site. `AgentSession::
    // start_background_task()` (rt/agent_session.hpp) is documented "PLAIN, UNLOCKED" and can race a
    // `report_progress` bracket window (rt/agent_session.hpp's own three `invoke_tool()` call sites)
    // open on a different thread of control -- if that race let a live `report_progress` closure
    // (captured `[this, call_id]` into the ORIGINATING session) survive into this copy, a backgrounded
    // tool's ORDINARY call to `ctx.report_progress(...)` below would reach back into that session's
    // `emit_run_event()` from THIS function's own detached `std::thread` (below) -- a genuine, unlocked
    // data race on `run_event_seq_by_run_` (an `std::unordered_map`), reachable with no tool misuse at
    // all, just the intended use of the very feature ADR-060 adds. Reset structurally, here, on this
    // function's own local copy -- unconditionally, not left as a documented-only rule a future caller
    // could violate -- so step 8 below never runs a tool against a live callback into a foreign thread.
    // 006 §6b's own scope (ADR-060 §3) never promised `Backgroundable`/`StandingEffect` progress
    // delivery through this channel; this makes that exclusion true by construction, not merely true
    // because nobody has tried yet.
    ctx.report_progress = [](ContentItem) {};

    // ADR-170 (issue #64): identical hazard, identical fix. `sandbox_exec_sink` is bound at the same
    // three bracket sites `report_progress` is, captures the same `[this]` into the originating
    // session, and ends at the same `emit_run_event_for()` -- so a backgrounded tool that reaches a
    // sandbox (exactly the long-running shape `Backgroundable` exists for) would otherwise emit from
    // this function's detached thread into a session that may already have been forked, cleared, or
    // destroyed. `emit_run_event_for()` taking `run_event_mutex_` since ADR-160 fixes the map race
    // but NOT the object-lifetime half, which is what this reset closes. Backgrounded work is
    // deliberately silent on this channel, by construction, for the same reason it is on the other.
    ctx.sandbox_exec_sink = [](run_event_kind, run_event_payload::SandboxExec) {};

    // Same hazard class as `report_progress` above, same fix: `sandbox_fs` is a raw pointer into
    // session-owned mediated-filesystem state (docs/planning/session-sandbox-lifecycle-wiring-
    // design-draft.md), not synchronized against `fork_from()`/`clear_in_process_state()`/session
    // teardown racing this function's own detached thread below. `captures_session_state` (checked
    // above) only refuses a stateful-descriptor TOOL -- it says nothing about an ordinary tool that
    // merely reads `ctx.sandbox_fs` directly, so that guard does not cover this pointer. Reset
    // unconditionally, on this function's own local copy, before step 8 ever runs a tool against it.
    ctx.sandbox_fs = nullptr;
    // ADR-193 (red team round 1): both capture the session that dispatched this call, which a backgrounded call may
    // outlive -- the same class ADR-060/ADR-170 closed for the sinks above.
    ctx.delegated_event_sink = [](RunEvent const&) {};
    ctx.charge_delegated_usage = [](Usage const&, std::uint64_t) {};

    // -- step 4/7: authorize + bind (the tool's own capability ceiling) -----------------------------
    std::vector<BoundCapability> bound;
    bound.reserve(tool->capability_ceiling.size());
    for (Capability const& requirement : tool->capability_ceiling) {
        auto handle = held.bind(requirement);
        if (!handle) {
            error e{failure_class::policy, "required capability not held", "tool.capability_not_held"};
            return std::unexpected(e);
        }
        bound.push_back(std::move(*handle));
    }

    // -- step 4/7 (G9): the CALLER's own Background<max_concurrent> ceiling, checked against a LIVE
    // count -- find_background()'s own comment: no grant at all means no background call is ever
    // authorized, regardless of what the tool declares.
    auto background_cap = held.find_background();
    if (!background_cap.has_value() || current_background_count >= background_cap->max_concurrent) {
        for (auto const& b : bound) b.revoke();
        error e{failure_class::resource, "Background<max_concurrent> ceiling reached or not granted",
                 "tool.background_capacity_exceeded"};
        return std::unexpected(e);
    }

    // -- step 5: approve ------------------------------------------------------------------------------
    // ADR-184: the shared predicate, not `tool->approval` alone -- before, a text_derived call to a
    // never_require tool with a real capability ceiling was backgrounded with no approval, skipping
    // ADR-023's override on this path only.
    if (tool_call_requires_approval(*tool, request.provenance)) {
        std::string canonical_args = json::dump(request.arguments);
        bool approved = approve && approve(ctx.principal, request.tool_name, canonical_args);
        if (!approved) {
            for (auto const& b : bound) b.revoke();
            error e{failure_class::policy, "approval required and not granted", "tool.approval_denied"};
            return std::unexpected(e);
        }
    }

    // -- step 8 onward: detached. The calling turn is never blocked past this point. -----------------
    std::thread worker([tool, request, ctx = std::move(ctx), bound = std::move(bound),
                         on_complete = std::move(on_complete)]() mutable {
        auto const started = std::chrono::steady_clock::now();
        ctx.bound_capabilities = &bound;
        result<json::Value> invoke_result = tool->invoke(request.arguments, ctx);
        ctx.bound_capabilities = nullptr;
        for (auto const& b : bound) b.revoke();  // step 10, unconditional -- same as invoke_tool()

        ToolInvocationAudit audit;
        audit.call_id   = request.call_id;
        audit.tool_name = request.tool_name;
        audit.duration  = std::chrono::steady_clock::now() - started;
        // ADR-061 §7 R26 / 007 §8, found not fully applied here while proving §46: `invoke_tool()`'s
        // own `finish()` lambda has stamped `ToolInvocationAudit` with `idempotency_key` and the
        // caller's identity since R26, but THIS function's own audit construction was never updated
        // to match -- a backgrounded call's completion record carried no identity and no idempotency
        // key at all, unattributable in exactly the way R26's own comment on `EffectContext::principal`
        // describes. Same derivation `invoke_tool()` uses, from the same `ctx` this closure already
        // captured by value; not a second, independently-derived value that could disagree with it.
        audit.idempotency_key    = derive_idempotency_key(ctx, request.call_index, request.arguments);
        audit.principal_id       = ctx.principal.id;
        audit.principal_tenant_id = ctx.principal.tenant_id;
        audit.principal_on_behalf_of = ctx.principal.on_behalf_of;
        audit.principal_delegation_root = ctx.principal.delegation_root;  // ADR-193

        if (!invoke_result) {
            error const& e = invoke_result.error();
            audit.ok         = false;
            audit.error_code = e.code;
            on_complete(make_error_result(request.call_id, e), std::move(audit));
            return;
        }

        auto normalized = normalize_success(request.call_id, *invoke_result, ctx);
        if (!normalized) {
            error const& e = normalized.error();
            audit.ok         = false;
            audit.error_code = e.code;
            on_complete(make_error_result(request.call_id, e), std::move(audit));
            return;
        }
        audit.ok           = true;
        audit.result_bytes = normalized->second;
        on_complete(std::move(normalized->first), std::move(audit));
    });
    worker.detach();

    return {};
}

}  // namespace agentengine
