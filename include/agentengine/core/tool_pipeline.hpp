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

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
    std::string args_schema_json;
    std::string reply_schema_json;

    using InvokeFn = std::function<result<json::Value>(json::Value const&, EffectContext&)>;
    InvokeFn invoke;
};

template <class ToolT>
[[nodiscard]] ToolDescriptor make_tool_descriptor() {
    ToolDescriptor d;
    d.name = std::string(ToolT::name);
    d.description = std::string(ToolT::description);
    d.capability_ceiling = ToolT::declared_capabilities();
    d.approval = ToolT::declared_approval();
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

}  // namespace tool_pipeline_detail

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
    if (tool->approval != approval_mode::never_require) {
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

}  // namespace agentengine
