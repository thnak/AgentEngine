#pragma once
// Implements 006-Tool-and-Function-Plane.md §3 — the ten-step invocation pipeline every tool call
// traverses, as real host machinery (not modeled on Tool itself, core/tool.hpp's own comment).
//
// M2 Phase B scope (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md): a single
// NATIVE tool call, synchronous (`ae::task<T>` deferred, decision 2), against the mechanical
// possession/attenuation checks ADR-009 already proves (007 §5's declarative policy DSL is out of
// scope, decision 4). Steps not meaningfully exercisable without machinery this milestone doesn't
// build are named, not silently skipped:
//   - step 3 "taint": tracked as one bool on the call (the arguments came from model output or
//     not) and stamped onto the result's ContentItem, not deep per-field Tainted<T> propagation
//     into every Args member -- that would need each Args field to individually be Tainted<T>,
//     a bigger design 003/006 don't yet specify at the field level (003 §2's mechanism taints
//     whole content items, which is exactly the granularity used here).
//   - step 5 "approve": `never_require` auto-approves; `always_require` calls the injected
//     ApprovalDecider over the call's canonical JSON args (006 §4's "approval is bound to the
//     exact call" -- trivially satisfied here since decider and execution see the identical args
//     value in one synchronous call, not a stored token checked later); `policy_driven` degrades
//     to requiring the same decider (fail-closed) until 007 §5's rule language exists (decision 4).
//   - step 6 "admit" (rate limit/concurrency/quota, Quark 022): out of scope for M2 (not named in
//     the roadmap's M2 build order); a documented no-op, not a silently dropped step.
//   - step 8's declared isolation: a native tool runs in-process (008's sandbox profiles are
//     Phase C); its deadline is enforced only at the call boundary (checked before invoking, not
//     preemptible mid-call without real coroutines -- ae::task<T> is what would let this be a real
//     mid-call timeout, decision 2 defers it).
//   - step 10's audit record is a minimal in-memory struct; 016's full span/telemetry shape is out
//     of scope for M2.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// One entry in the immutable per-run tool table (006 §6: "resolved at run start into an immutable
// per-run tool table"). Type-erased: `invoke` closes over ToolT's real Args/Reply types so the
// pipeline itself never needs to be a template.
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::vector<Capability> capability_ceiling;  // from the tool's declared Capabilities<...>
    approval_mode approval = approval_mode::never_require;
    // Milestone 7 Phase B (006 §6b): `false` unless the tool declared `Backgroundable` -- read by
    // `background_task()` (agent_session.hpp) to reject an undeclared tool at authorize, before it
    // ever reaches step 8.
    bool backgroundable = false;
    std::string args_schema_json;
    std::string reply_schema_json;

    using InvokeFn = std::function<result<json::Value>(json::Value const&, EffectContext&)>;
    InvokeFn invoke;

    // ADR-023 §6 point 4 / 007 §4 amendment: `invoke_tool`'s step 5 reads this ONLY for a
    // `text_derived` call (`ToolCallRequest::provenance` below) -- `Tool<Derived,...>::
    // declared_effect_class()` (019 §3) already existed as a compile-time accessor but was never
    // previously copied onto the runtime descriptor because nothing needed it at this layer before
    // now. Appended last (this struct's own established convention) -- `at_most_once` is the same
    // conservative default `declared_effect_class()` itself uses, so a hand-built `ToolDescriptor`
    // that predates this field (not going through `make_tool_descriptor<T>()` below) fails CLOSED
    // against auto-declassification rather than silently qualifying.
    effect_class effect_class = effect_class::at_most_once;

    // Session-scoped-stateful-tools mechanism (ADR-028): `true` only for a descriptor built via
    // `make_tool_descriptor_with_invoke<ToolT>()` below, whose `invoke` closure captures a
    // reference into its owning provider's own session-scoped state (e.g. a persistent
    // interpreter's exec state, mounted-skills tracking) rather than being a pure, ownership-free
    // static call. `false` (the default) for every ordinary `make_tool_descriptor<ToolT>()` tool,
    // unaffected. `background_task()` below refuses to background any descriptor with this set —
    // `start_background_task()` (agent_session.hpp) detaches a real `std::thread` that would then
    // hold a reference into session state with no synchronization against `AgentSession::
    // fork_from()`/`clear_in_process_state()` (neither is part of `protocol`, so neither is
    // Quark-`Sequential`-serialized against a detached background thread) — a real dangling-
    // reference/data-race hazard found by design review, closed here structurally rather than left
    // as a documented-only rule a future caller could violate by accident.
    bool captures_session_state = false;
};

template <class ToolT>
[[nodiscard]] ToolDescriptor make_tool_descriptor() {
    ToolDescriptor d;
    d.name = std::string(ToolT::name);
    d.description = std::string(ToolT::description);
    d.capability_ceiling = ToolT::declared_capabilities();
    d.approval = ToolT::declared_approval();
    d.backgroundable = ToolT::declared_backgroundable();
    d.effect_class = ToolT::declared_effect_class();
    d.args_schema_json = ToolT::args_schema();
    d.reply_schema_json = ToolT::reply_schema();
    d.invoke = [](json::Value const& args_value, EffectContext& ctx) -> result<json::Value> {
        auto args = schema::from_json<typename ToolT::Args>(args_value);
        if (!args) return std::unexpected(args.error());
        auto reply = ToolT::invoke(*args, ctx);
        if (!reply) return std::unexpected(reply.error());
        return schema::to_json(*reply);
    };
    return d;
}

// ADR-028 -- the general session-scoped-stateful-tools mechanism. Identical to
// `make_tool_descriptor<ToolT>()` above (same compile-time extraction of `ToolT`'s declared
// `Capabilities<...>`/`Approval<...>`/`EffectClass<...>`/schemas -- a state-capturing tool is
// still a real `Tool<Derived, Policies...>` conformer, never a hand-built-from-scratch descriptor
// that would silently default to an empty capability ceiling and `never_require` approval), except
// `custom_invoke` runs INSTEAD OF `ToolT::invoke` -- letting the caller supply a callable that
// captures whatever session-scoped state it needs (typically a non-static member function on the
// state-owning `ContextProvider`, e.g. `[this](Args a, EffectContext& ctx) { return
// this->real_invoke(a, ctx); }`). `ToolT::invoke` itself is never called on this path; it may even
// be left undefined by `ToolT` if every call site uses this factory instead of the plain one.
template <class ToolT, class InvokeFn>
[[nodiscard]] ToolDescriptor make_tool_descriptor_with_invoke(InvokeFn custom_invoke) {
    ToolDescriptor d;
    d.name = std::string(ToolT::name);
    d.description = std::string(ToolT::description);
    d.capability_ceiling = ToolT::declared_capabilities();
    d.approval = ToolT::declared_approval();
    d.backgroundable = ToolT::declared_backgroundable();
    d.effect_class = ToolT::declared_effect_class();
    d.args_schema_json = ToolT::args_schema();
    d.reply_schema_json = ToolT::reply_schema();
    d.captures_session_state = true;
    d.invoke = [custom_invoke = std::move(custom_invoke)](
                   json::Value const& args_value, EffectContext& ctx) -> result<json::Value> {
        auto args = schema::from_json<typename ToolT::Args>(args_value);
        if (!args) return std::unexpected(args.error());
        auto reply = custom_invoke(*args, ctx);
        if (!reply) return std::unexpected(reply.error());
        return schema::to_json(*reply);
    };
    return d;
}

// 006 §6: static tools are resolved once into an immutable table at run start -- a mid-run change
// to what's registered cannot alter what a run is allowed to call. Linear lookup: tool counts in
// this milestone's scope are single digits, not a hot path.
class ToolTable {
public:
    template <class... ToolTs>
    [[nodiscard]] static ToolTable from_tools() {
        ToolTable t;
        (t.descriptors_.push_back(make_tool_descriptor<ToolTs>()), ...);
        return t;
    }

    // A second, runtime construction path alongside `from_tools<ToolTs...>()` -- needed when the
    // actual offered set is a runtime-computed SUBSET of a compile-time-declared universe (e.g.
    // `core/skill_tool_scoping.hpp` filtering by which skills are currently mounted). The result is
    // just as immutable once built as one from `from_tools<...>()` -- this does not weaken 006 §6's
    // "resolved once into an immutable table at run start" invariant, it only adds a second way to
    // reach that same shape.
    [[nodiscard]] static ToolTable from_descriptors(std::vector<ToolDescriptor> descriptors) {
        ToolTable t;
        t.descriptors_ = std::move(descriptors);
        return t;
    }

    [[nodiscard]] ToolDescriptor const* find(std::string_view name) const {
        for (auto const& d : descriptors_) {
            if (d.name == name) return &d;
        }
        return nullptr;
    }

    // M2 Phase E task E2 (core/agent_registry.hpp): register_agent<A>()'s validation needs to walk
    // every declared tool (name-collision, capability-ceiling coverage) -- `find()` alone can't
    // answer "are there duplicates" or "for each tool, what capabilities does it need".
    [[nodiscard]] std::vector<ToolDescriptor> const& descriptors() const { return descriptors_; }

private:
    std::vector<ToolDescriptor> descriptors_;
};

struct ToolCallRequest {
    std::string call_id;
    std::string tool_name;
    json::Value arguments;
    // 003 §2 / 006 §3 step 3: true if these arguments carry model-originated content. Stamped onto
    // the result's ContentItem (see the file-top comment for why this is item-level, not per-field).
    bool arguments_tainted = false;
    // Milestone 4 Phase F1 (019 §3: idempotency key = {run_id, turn_index, call_index,
    // argument_digest}). `call_index` is the CALLER's own ordinal for this call within its current
    // turn (the pipeline does not track a per-turn call counter itself -- that bookkeeping belongs
    // to whatever drives the turn's own tool-call loop, the same "no ambient state" shape
    // `EffectContext::run_id`/`turn_index` already have). Defaults to 0 for every M2-era caller
    // that predates this field (aggregate init with fewer braces than members is unaffected).
    std::uint64_t call_index = 0;
    // ADR-023 §6 point 4 / 007 §4 amendment: appended last, defaults to `vendor_structured` so
    // every existing positional/aggregate `ToolCallRequest{...}` call site is unaffected. Read by
    // `invoke_tool`'s step 5 below -- see that function's own comment for the full rule.
    call_provenance provenance = call_provenance::vendor_structured;
};

// Milestone 4 Phase F1 (019 §3): "Every effect carries an idempotency key derived
// deterministically from {run_id, turn_index, call_index, argument_digest}. Deterministic
// derivation is what makes the key survive a restart." `argument_digest` is an FNV-1a hash of the
// call's own canonical JSON arguments -- the same deterministic-hash idiom Quark's own
// `reminder_name_hash()` (reminder_service.hpp) already uses, not a cryptographic commitment (019
// names no collision-resistance requirement, only determinism).
[[nodiscard]] inline std::uint64_t argument_digest(std::string_view canonical_args_json) noexcept {
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    for (unsigned char c : canonical_args_json) {
        h ^= c;
        h *= 0x0000'0100'0000'01B3ULL;
    }
    return h;
}

struct IdempotencyKey {
    std::string   run_id;
    std::uint64_t turn_index = 0;
    std::uint64_t call_index = 0;
    std::uint64_t argument_digest = 0;

    friend bool operator==(IdempotencyKey const&, IdempotencyKey const&) = default;

    // A single string form for use as a journal/outbox dedup key (F2's `EffectJournalEntry`).
    [[nodiscard]] std::string to_string() const {
        return run_id + ":" + std::to_string(turn_index) + ":" + std::to_string(call_index) + ":" +
               std::to_string(argument_digest);
    }
};

[[nodiscard]] inline IdempotencyKey derive_idempotency_key(EffectContext const& ctx,
                                                            std::uint64_t call_index,
                                                            json::Value const& arguments) {
    return IdempotencyKey{ctx.run_id, ctx.turn_index, call_index,
                          argument_digest(json::dump(arguments))};
}

// Milestone 4 Phase F4 (019 §6's rewind-then-reexecute rule): "On re-execution: pure effects
// re-run freely; idempotent effects re-run under their original keys; at-most-once effects
// require explicit operator acknowledgement before re-execution." Provable now against a
// manually-triggered re-execution (a caller deliberately re-invoking a tool call it already ran
// once) -- full integration with workflow rewind (014 §5) waits for M6, since 014 doesn't exist
// yet (the same "narrower than the RFC's own gate, not silently dropped" discipline this
// milestone's decision 3 already applies to checkpoint boundaries). `operator_acknowledged` is an
// explicit, host/human-supplied bool, never invented ambiently or derived from model output (I3)
// -- the same shape `ApprovalDecider` above already has for exactly this reason.
[[nodiscard]] inline result<void> authorize_reexecution(effect_class cls, bool operator_acknowledged) {
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

// Step 5's decision point. Approvals never come from a model (I3) -- this is an explicit,
// host/human-supplied callable, never invented ambiently inside the pipeline. Receives the
// canonical JSON of the exact arguments about to execute (006 §4: "approval is bound to the exact
// call").
using ApprovalDecider = std::function<bool(std::string_view tool_name, std::string const& canonical_args_json)>;

// Step 10's minimal audit record (016/013's full span shape is out of scope for M2, see file-top
// comment). Phase F1 adds the call's own idempotency key -- computed unconditionally (it costs one
// digest of bytes already being canonicalized for step 5's approval check) so ANY caller journaling
// effects (F2) has it without re-deriving it from the request a second time.
struct ToolInvocationAudit {
    std::string call_id;
    std::string tool_name;
    bool ok = false;
    std::string error_code;  // empty iff ok
    std::size_t result_bytes = 0;
    std::chrono::steady_clock::duration duration{};
    IdempotencyKey idempotency_key;
};

namespace tool_pipeline_detail {

[[nodiscard]] inline ToolResult make_error_result(std::string call_id, error const& e) {
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

// ADR-023 §6 point 4 / 007 §4 amendment, declassifier (a′): a `text_derived` call auto-declassifies
// (skips step 5's approval entirely) ONLY when the target tool's declared capability ceiling is
// made ENTIRELY of kinds `trust::is_inert_for_text_derived_declassification` proves safe (read-only/
// informational -- an empty ceiling trivially qualifies, `std::all_of` over an empty range is `true`
// by definition, which is exactly "no capabilities at all" auto-declassifying, the strongest case)
// AND the tool is declared `effect_class::pure`. Everything else -- ANY other capability kind, or a
// non-pure effect class -- requires approval, unconditionally. This function answers ONLY that
// static question; it is never itself an approval decision (007 §4).
[[nodiscard]] inline bool is_auto_declassifiable_text_derived_call(ToolDescriptor const& tool) noexcept {
    if (tool.effect_class != agentengine::effect_class::pure) return false;
    return std::all_of(tool.capability_ceiling.begin(), tool.capability_ceiling.end(),
                        [](Capability const& c) {
                            return is_inert_for_text_derived_declassification(capability_kind_of(c));
                        });
}

}  // namespace tool_pipeline_detail

// ADR-029 needs the step-5 predicate OUTSIDE `invoke_tool()` itself, to decide BEFORE calling it
// whether a round's pending calls need real human approval (`AgentSession::handle()`'s
// suspend-for-approval path). Public re-export, single source of truth: `invoke_tool()`'s own step
// 5 below calls this exact function too, so the two can never drift apart.
[[nodiscard]] inline bool tool_call_requires_approval(ToolDescriptor const& tool,
                                                        call_provenance provenance) noexcept {
    using namespace tool_pipeline_detail;
    return (provenance == call_provenance::text_derived)
               ? !is_auto_declassifiable_text_derived_call(tool)
               : (tool.approval != approval_mode::never_require);
}

// ADR-029 needs this outside `invoke_tool()` too, to build a denial `ToolResult` for a pending call
// a human explicitly rejected (`AgentSession::handle()`'s resume-and-deny path) in exactly the same
// shape a synchronous `ApprovalDecider`'s own denial already produces.
[[nodiscard]] inline ToolResult make_denial_result(std::string call_id, std::string message,
                                                     std::string error_code) {
    return tool_pipeline_detail::make_error_result(
        std::move(call_id), error{failure_class::policy, std::move(message), std::move(error_code)});
}

// The ten-step pipeline (006 §3), against a single native tool call. `held` is the run's actual
// granted set (never mutated here); `ctx` is filled in with this call's per-invocation
// `bound_capabilities` (step 7) for the duration of `invoke`, and cleared again before returning
// regardless of outcome (step 10) -- see the RAII guard below, which is what makes G3 ("a
// capability handle from call n is unusable in call n+1") true by construction rather than by
// convention.
[[nodiscard]] inline ToolResult invoke_tool(ToolTable const& table, CapabilitySet const& held,
                                             ToolCallRequest const& request, EffectContext& ctx,
                                             ApprovalDecider const& approve,
                                             ToolInvocationAudit* audit_out = nullptr) {
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
        }
        return result;
    };

    // -- step 1: resolve -------------------------------------------------------------------------
    ToolDescriptor const* tool = table.find(request.tool_name);
    if (!tool) {
        error e{failure_class::contract, "unknown tool: " + request.tool_name, "tool.unknown_name"};
        return finish(make_error_result(request.call_id, e), &e);
    }

    // -- step 2: validate (+ step 3: taint, recorded not deeply propagated) ----------------------
    // Deferred to `tool->invoke`'s call into schema::from_json<Args> below -- a single point of
    // truth for "does this JSON match the declared shape", never a second, hand-rolled check here
    // that could drift from what actually gets parsed.

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
            return finish(make_error_result(request.call_id, e), &e);
        }
        bound.push_back(std::move(*handle));
    }

    // -- step 5: approve ---------------------------------------------------------------------------
    // ADR-023 §6 point 4 / 007 §4 amendment: a `text_derived` call NEVER consults `tool->approval`
    // at all -- that setting was authored by the tool's declarer for VENDOR-STRUCTURED calls (a
    // real, trusted wire-format field). A call reconstructed from raw model text is a different,
    // weaker trust class by construction (007 §4: model-supplied text is never itself an
    // authorization decision), so it gets its OWN gate (`is_auto_declassifiable_text_derived_call`)
    // that can only ever be MORE restrictive than the tool's own setting, including overriding a
    // tool's own `approval_mode::never_require` for anything with a real capability ceiling -- the
    // exact override the confused-deputy scenario (ADR-023 §4b Finding 1) forced. A
    // `vendor_structured` call (every caller before this amendment, and every caller that never sets
    // `provenance`) takes the ORIGINAL branch, byte-for-byte unchanged.
    bool const requires_approval = tool_call_requires_approval(*tool, request.provenance);
    if (requires_approval) {
        std::string canonical_args = json::dump(request.arguments);
        bool approved = approve && approve(request.tool_name, canonical_args);
        if (!approved) {
            for (auto const& b : bound) b.revoke();  // never bind authority we then refuse to use
            error e{failure_class::policy, "approval required and not granted", "tool.approval_denied"};
            return finish(make_error_result(request.call_id, e), &e);
        }
    }

    // -- step 6: admit -- deferred (Quark 022 not in M2 scope; documented no-op, see file top) ----

    // -- step 8: invoke (deadline checked at the call boundary, not preemptible mid-call) ---------
    if (ctx.deadline.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() > ctx.deadline) {
        for (auto const& b : bound) b.revoke();
        error e{failure_class::resource, "deadline already exceeded", "tool.deadline_exceeded"};
        return finish(make_error_result(request.call_id, e), &e);
    }

    ctx.bound_capabilities = &bound;
    result<json::Value> invoke_result = tool->invoke(request.arguments, ctx);
    ctx.bound_capabilities = nullptr;

    // -- step 10: account (revoke unconditionally, success or failure) ----------------------------
    for (auto const& b : bound) b.revoke();

    // -- step 9: normalize --------------------------------------------------------------------------
    if (!invoke_result) {
        error const& e = invoke_result.error();
        return finish(make_error_result(request.call_id, e), &e);
    }

    std::string reply_json = json::dump(*invoke_result);
    std::size_t const reply_bytes = reply_json.size();

    ToolResult ok_result;
    ok_result.call_id = request.call_id;
    ok_result.is_error = false;
    ContentItem item;
    // `Data::schema_id` is a schema *reference* (a registry id/URI), not the schema body itself --
    // this milestone has no schema registry (that's 011 MCP-conformance territory), so it stays
    // unset rather than misusing the field to carry `reply_schema_json`'s full text.
    item.value = Data{std::move(reply_json), std::nullopt};
    item.origin = content_origin::tool;
    // A tool result is external content and the primary prompt-injection vector (006 §7) --
    // provenance-marked regardless of whether the CALL's own arguments were tainted.
    item.tainted = true;
    ok_result.content.push_back(std::move(item));
    return finish(std::move(ok_result), nullptr, reply_bytes);
}

// Milestone 7 Phase B (006 §6b): "Still runs the full 10-step tool pipeline (§3); only step 8
// (invoke) stops blocking the turn on completion." Steps 1 (resolve), 4/7 (authorize + bind -- the
// tool's own capability ceiling, AND the caller's `Background<max_concurrent>` ceiling checked
// against a LIVE count the caller supplies, G9), and 5 (approve) all run SYNCHRONOUSLY on the calling
// thread, exactly like `invoke_tool()` above -- this function returns an error immediately, before
// ever spawning anything, for the same reasons `invoke_tool()` would refuse the call. Step 8 (invoke)
// alone runs on its own detached `std::thread`: deliberately `std::thread` + `.detach()`, not a
// `std::jthread` some caller has to keep alive -- a detached thread needs no handle to manage, and
// 006 §6b names no cancellation mechanism for IN-FLIGHT native `invoke()` work (`invoke_tool()`'s own
// step 8 comment already notes step 8 is "not preemptible mid-call without real coroutines" on the
// synchronous foreground path; backgrounding does not change that). Steps 9 (normalize) and 10
// (account: revoke bound capabilities) also run on that same background thread, immediately before
// `on_complete` fires.
//
// `ctx` is copied into the background thread's own closure -- `ctx.capabilities`/
// `ctx.bound_capabilities` are non-owning pointers, the SAME "host owns it, must outlive" contract
// `EffectContext::capabilities` already carries everywhere in this codebase (effect_context.hpp), not
// a new hazard this function introduces. `bound` (the per-invocation `BoundCapability` handles from
// step 7) is moved into the same closure so its RAII revoke-at-step-10 semantics are preserved
// exactly, just on a different thread than the one that minted them.
//
// `on_complete` fires from the BACKGROUND thread, exactly once. What "deliver this back to a run"
// means is entirely the caller's job -- `AgentSession::start_background_task()` (agent_session.hpp)
// wires this to a self-`tell()`, mirroring `TimerWake`'s own established "host arms the callback"
// shape (`test_agent_session_timer_wake.cpp`'s own precedent), not a mechanism this function invents.
using BackgroundTaskCompletion = std::function<void(ToolResult, ToolInvocationAudit)>;

[[nodiscard]] inline result<void> background_task(ToolTable const& table, CapabilitySet const& held,
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
    if (tool->approval != approval_mode::never_require) {
        std::string canonical_args = json::dump(request.arguments);
        bool approved = approve && approve(request.tool_name, canonical_args);
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

        if (!invoke_result) {
            error const& e = invoke_result.error();
            audit.ok         = false;
            audit.error_code = e.code;
            on_complete(make_error_result(request.call_id, e), std::move(audit));
            return;
        }

        std::string reply_json  = json::dump(*invoke_result);
        audit.ok                = true;
        audit.result_bytes      = reply_json.size();

        ToolResult ok_result;
        ok_result.call_id  = request.call_id;
        ok_result.is_error = false;
        ContentItem item;
        item.value   = Data{std::move(reply_json), std::nullopt};
        item.origin  = content_origin::tool;
        item.tainted = true;
        ok_result.content.push_back(std::move(item));
        on_complete(std::move(ok_result), std::move(audit));
    });
    worker.detach();

    return {};
}

}  // namespace agentengine
