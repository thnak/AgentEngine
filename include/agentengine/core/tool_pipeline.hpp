#pragma once
// ADR-201 (#120 S5): the function bodies live in src/core/tool_pipeline.cpp, compiled once into
// agentengine_tool_pipeline; this header keeps every type, every declaration with its comment, and bodies under
// 6 lines. Bodies were moved verbatim.
// Implements 006-Tool-and-Function-Plane.md §3 — the ten-step invocation pipeline every tool call
// traverses, as real host machinery (not modeled on Tool itself, core/tool.hpp's own comment).
//
// The declaration half (`ToolDescriptor`, `make_tool_descriptor<ToolT>()`, `ToolTable`) lives in
// core/tool_descriptor.hpp, included below; see that file's banner for why the two were split.
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
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_descriptor.hpp"  // ToolDescriptor, make_tool_descriptor, ToolTable (006 §6)
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// ae-naming-lint: allow ToolCallRequest — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
    // ADR-197: the parser's message when the model's argument text was not valid JSON; empty iff it
    // parsed. `arguments` is then only a placeholder: `admit_call()`/`background_task()` refuse the
    // call at step 2 (006 §3, "reject; do not coerce") before any capability is bound or any
    // approval asked. Appended last, like `provenance`, so existing aggregate call sites are unaffected.
    // (The explicit `{}` initializer keeps clang's -Wmissing-field-initializers quiet at those sites.)
    std::string arguments_parse_error{};
};

// ADR-197: step 2's refusal for a call whose argument text did not parse -- one definition, shared
// by `admit_call()` and `background_task()`. The model sees the message and can resend the call.
[[nodiscard]] inline error malformed_arguments_error(ToolCallRequest const& request) {
    return error{failure_class::contract,
                 "tool arguments are not valid JSON (" + request.arguments_parse_error +
                     "); the tool was not run -- resend the call with a JSON object",
                 "tool.malformed_arguments"};
}

// Milestone 4 Phase F1 (019 §3): "Every effect carries an idempotency key derived
// deterministically from {run_id, turn_index, call_index, argument_digest}. Deterministic
// derivation is what makes the key survive a restart." `argument_digest` is an FNV-1a hash of the
// call's own canonical JSON arguments -- the same deterministic-hash idiom Quark's own
// `reminder_name_hash()` (reminder_service.hpp) already uses, not a cryptographic commitment (019
// names no collision-resistance requirement, only determinism).
[[nodiscard]] std::uint64_t argument_digest(std::string_view canonical_args_json) noexcept;

// ae-naming-lint: allow IdempotencyKey — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
[[nodiscard]] result<void> authorize_reexecution(effect_class cls, bool operator_acknowledged);

// Step 5's decision point. Approvals never come from a model (I3) -- this is an explicit,
// host/human-supplied callable, never invented ambiently inside the pipeline. Receives the
// canonical JSON of the exact arguments about to execute (006 §4: "approval is bound to the exact
// call").
//
// ADR-061 §46: `caller` is the identity `EffectContext::principal` already carries at both call
// sites below -- the SAME identity `ToolInvocationAudit` already records, not a second,
// independently-suppliable value. Lets a decider branch on WHO is asking (006 §4/007 §5's own
// "principal" axis), which the prior two-argument shape made structurally impossible; 007 §5's
// policy engine still does not exist, so nothing here derives a decision from `caller` -- a decider
// that ignores it behaves exactly as before.
// ae-naming-lint: allow ApprovalDecider — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using ApprovalDecider = std::function<bool(Principal const& caller, std::string_view tool_name,
                                            std::string const& canonical_args_json)>;

// Delegated Decision Seam (decisions/ADR-070-host-configurable-responsibility-boundary.md): a
// SECOND, optional host seam, consulted ONLY for `approval_mode::policy_driven` -- unlike
// `ApprovalDecider` above (a binary yes/no over one already-pending call), this lets a host resolve
// `policy_driven`'s existing fail-closed degrade (file-top comment: "policy_driven degrades to
// requiring the same decider... until 007 §5's rule language exists") WITHOUT waiting for that
// still-unbuilt declarative DSL, by writing the graduated logic itself. Defaults to `nullptr`: with
// no `PolicyDecider` wired, `policy_driven` behaves BYTE-FOR-BYTE as it always has (falls through to
// `ApprovalDecider`, exactly like `always_require`) -- this seam only ever makes a call MORE resolved
// (auto_approve/auto_deny) than today, never less, and never widens past the tool's own declared
// `capability_ceiling` (it decides among already-possessed authority, it does not grant any).
// Never consulted for `never_require` or `always_require`. For a `text_derived` call its `auto_approve`
// is never an approval: 007 §4's closed declassifier list stays closed (ADR-023's own red-team already
// found a laxer version of THAT gate unsafe). Its `auto_deny` IS honoured there since ADR-184 (denying
// only narrows; before, a denied text_derived call skipped the deny and reached the ApprovalDecider).
// ADR-192's unattended mode additionally asks it about every call that reaches the decider.
// `resolve_approval_outcome` below enforces the distinction structurally, not just by convention.
enum class policy_decision { auto_approve, auto_deny, require_approval };  // ae-naming-lint: allow policy_decision — ADR-070, same idiom as approval_mode/call_provenance
// ae-naming-lint: allow PolicyDecider — ADR-070, same idiom as ApprovalDecider above
using PolicyDecider = std::function<policy_decision(Principal const& caller, ToolDescriptor const& tool,
                                                     bool arguments_tainted)>;

// Step 10's minimal audit record (016/013's full span shape is out of scope for M2, see file-top
// comment). Phase F1 adds the call's own idempotency key -- computed unconditionally (it costs one
// digest of bytes already being canonicalized for step 5's approval check) so ANY caller journaling
// effects (F2) has it without re-deriving it from the request a second time.
// ae-naming-lint: allow ToolInvocationAudit — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ToolInvocationAudit {
    std::string call_id;
    std::string tool_name;
    bool ok = false;
    std::string error_code;  // empty iff ok
    std::size_t result_bytes = 0;
    std::chrono::steady_clock::duration duration{};
    IdempotencyKey idempotency_key;

    // ADR-061 §7 R26: this record carried no identity at all, while 007 §8 requires the principal
    // AND the delegation chain on every audit record, and I4 ("every effect is attributable") rests
    // on it. It was survivable only while one server object served one principal; the moment two
    // principals share a dispatcher -- which is exactly what an inbound protocol surface makes
    // normal -- an audit trail without identity cannot attribute anything. Copied from
    // `EffectContext::principal`, so it is whatever the pipeline actually executed as, never a
    // separately-passed claim that could disagree with it.
    //
    // `on_behalf_of` is 007 §2's delegation link. It is the IMMEDIATE parent only, not the full
    // chain -- `Principal` itself carries only that much (trust/principal.hpp's own comment), so
    // recording more here would be inventing precision the identity type does not have.
    std::string principal_id;
    std::string principal_tenant_id;
    std::string principal_on_behalf_of;
    std::string principal_delegation_root{};  // ADR-193: the chain's root principal; empty when not delegated
};

namespace tool_pipeline_detail {

[[nodiscard]] ToolResult make_error_result(std::string call_id, error const& e);

// Step 9 (006 §3), the success path: turns a tool's raw JSON reply into the `ToolResult` the model
// actually sees, applying 006 §7 / 028 §2's promotion rule -- shared by `invoke_tool()` and
// `background_task()` below so the rule can never drift between the synchronous and backgrounded
// paths (this file's own established "single source of truth" discipline, e.g.
// `tool_call_requires_approval`'s own comment for the identical reason on step 5).
//
// Under `ctx.tool_result_byte_threshold` (or when no threshold is set at all): an ordinary `Data`
// content item, byte-for-byte the same shape this pipeline has always produced -- a caller that
// never wires the two `EffectContext` fields this reads sees no behavior change whatsoever.
//
// At or above the threshold: promoted to a `Media` item carrying a `BlobRef` (003 §3), written via
// `ctx.blob_sink`. No sink configured is treated as "cannot promote" and fails closed
// (`tool.result_oversized_no_sink`) rather than falling back to inlining the oversized result anyway
// -- inlining exactly what crossed the threshold is the hazard 006 §7 exists to prevent, so silently
// doing it as a fallback would defeat the whole mechanism the moment a caller forgot to wire a sink.
[[nodiscard]] result<std::pair<ToolResult, std::size_t>> normalize_success(
        std::string call_id, json::Value const& reply_value, EffectContext& ctx);

// ADR-023 §6 point 4 / 007 §4 amendment, declassifier (a′): a `text_derived` call auto-declassifies
// (skips step 5's approval entirely) ONLY when the target tool's declared capability ceiling is
// made ENTIRELY of kinds `trust::is_inert_for_text_derived_declassification` proves safe (read-only/
// informational -- an empty ceiling trivially qualifies, `std::all_of` over an empty range is `true`
// by definition, which is exactly "no capabilities at all" auto-declassifying, the strongest case)
// AND the tool is declared `effect_class::pure`. Everything else -- ANY other capability kind, or a
// non-pure effect class -- requires approval, unconditionally. This function answers ONLY that
// static question; it is never itself an approval decision (007 §4).
[[nodiscard]] bool is_auto_declassifiable_text_derived_call(ToolDescriptor const& tool) noexcept;

}  // namespace tool_pipeline_detail

// ADR-029 needs the step-5 predicate OUTSIDE `invoke_tool()` itself, to decide BEFORE calling it
// whether a round's pending calls need real human approval (`AgentSession::handle()`'s
// suspend-for-approval path). Public re-export, single source of truth: `invoke_tool()`'s own step
// 5 below calls this exact function too, so the two can never drift apart.
//
// ADR-184: a `text_derived` call is never less gated than the same call `vendor_structured`. The
// declassifier lifts only the approval that text-derived provenance itself imposes; it never lifts
// one the tool's own declaration imposes. So it needs approval when the tool's own mode would
// require it for a trusted call (`always_require`, `policy_driven`) OR the tool is not
// declassifiable. Before ADR-184 the first half was missing, and a text-derived call to a pure,
// capability-free `always_require` tool ran with no approval at all.
[[nodiscard]] bool tool_call_requires_approval(ToolDescriptor const& tool,
                                               call_provenance provenance) noexcept;

// ADR-070: single source of truth for step 5's THREE-way outcome once a `PolicyDecider` may be in
// play -- both `invoke_tool()`'s own step 5 below AND `AgentSession`'s suspend-for-approval
// pre-check (rt/agent_session.hpp) call this exact function, the same "outside invoke_tool() too"
// reason `tool_call_requires_approval` above is itself exported for, so the two can never drift
// apart. With `policy` unset (`{}`) this reproduces `tool_call_requires_approval()`'s own boolean
// exactly -- `needs_decider` wherever that function would have returned `true`, `proceed` otherwise
// -- so every existing caller that never wires a `PolicyDecider` sees byte-identical behavior.
enum class approval_outcome { proceed, deny, needs_decider };  // ae-naming-lint: allow approval_outcome — ADR-070, same idiom as call_provenance/approval_mode

[[nodiscard]] approval_outcome resolve_approval_outcome(ToolDescriptor const& tool,
                                                        call_provenance provenance,
                                                        Principal const& caller,
                                                        bool arguments_tainted,
                                                        PolicyDecider const& policy);

// ADR-029 needs this outside `invoke_tool()` too, to build a denial `ToolResult` for a pending call
// a human explicitly rejected (`AgentSession::handle()`'s resume-and-deny path) in exactly the same
// shape a synchronous `ApprovalDecider`'s own denial already produces.
[[nodiscard]] inline ToolResult make_denial_result(std::string call_id, std::string message,
                                                     std::string error_code) {
    return tool_pipeline_detail::make_error_result(
        std::move(call_id), error{failure_class::policy, std::move(message), std::move(error_code)});
}

// OQ-21's tool-call hook stage (core/tool_call_hook.hpp): mirrors
// `middleware_detail::enforce_backend_tool_call_provenance()` (middleware.hpp) -- the identical
// "diff the bytes, downgrade the trust class" idiom, applied at a different seam. Called
// UNCONDITIONALLY within the hook stage's own per-call loop (rt/agent_session.hpp) for every call
// the hook stage touched -- never gated on what the hook itself claims, because
// `ToolCallHookContext` (core/tool_call_hook.hpp) has no field through which a hook could assert a
// provenance directly (that struct's own file-top comment). `json::Value` has no `operator==`
// (json_value.hpp), so the comparison goes through canonical `json::dump()` text, the same
// equality-by-serialization idiom this file's own `derive_idempotency_key()` already relies on.
// Once downgraded, `tool_call_requires_approval()`/`is_auto_declassifiable_text_derived_call()`
// (unchanged, above) already do the right thing -- a `text_derived` call only auto-declassifies when
// the target tool is `effect_class::pure` with an entirely inert capability ceiling; ADR-023's
// confused-deputy closure applies here exactly as it does to a genuinely model-issued call.
inline void enforce_hook_rewritten_tool_call_provenance(ToolCallRequest& req,
                                                          json::Value const& original_arguments) {
    if (json::dump(req.arguments) != json::dump(original_arguments)) {
        req.provenance = call_provenance::text_derived;
    }
}

// decisions/ADR-160-parallel-tool-batch-scheduler.md §5: steps 1 (resolve), 4/7 (authorize+bind),
// 5 (approve) extracted out of `invoke_tool()` below into their own callable, byte-for-byte the
// same logic -- so the parallel-batch admission path (`AgentSession`, rt/agent_session.hpp) can
// share EXACTLY this admission logic rather than carry a second, independently-drifting copy of a
// security-critical policy decision. `invoke_tool()` is refactored below to call this; its own
// signature, observable behavior, and audit fields are unchanged.
struct AdmittedCall {
    ToolDescriptor const* tool = nullptr;
    std::vector<BoundCapability> bound;
};

[[nodiscard]] result<AdmittedCall> admit_call(ToolTable const& table, CapabilitySet const& held,
                                              ToolCallRequest const& request,
                                              Principal const& caller,
                                              ApprovalDecider const& approve,
                                              PolicyDecider const& policy);

// decisions/ADR-160-parallel-tool-batch-scheduler.md §5: steps 8 (invoke), 9 (normalize), 10
// (account) extracted from invoke_tool() below, alongside admit_call() above -- returns the raw
// outcome without touching any audit bookkeeping, DELIBERATELY: `invoke_tool()`'s own audit
// measures duration from before admission even starts, while the parallel-batch fan-out path
// (AgentSession::dispatch_tool_calls()) measures a job's own audit from its own admission-to-
// completion span -- sharing a `finish()`-style closure between the two would silently change one
// of their two, deliberately different, "duration" semantics. `failure` is a real copy (an
// `error`, not a pointer into a temporary) so it safely outlives this call.
struct AdmittedCallOutcome {
    ToolResult result;
    std::optional<error> failure;
    std::size_t bytes = 0;
};

[[nodiscard]] AdmittedCallOutcome run_admitted_call(ToolDescriptor const& tool, ToolCallRequest const& request,
                                                    EffectContext& ctx, std::vector<BoundCapability>& bound);

// ADR-160 §5: a small, shared audit-record builder for the parallel-batch fan-out path only --
// `invoke_tool()`'s own inline `finish()` closure and `background_task()`'s own closure each keep
// their pre-existing, independent audit construction, untouched (ADR-160 §4 non-goals: this ADR
// does not touch `background_task()`).
[[nodiscard]] ToolInvocationAudit make_call_audit(ToolCallRequest const& request,
                                                  EffectContext const& ctx,
                                                  std::chrono::steady_clock::time_point started,
                                                  AdmittedCallOutcome const& outcome);

// The ten-step pipeline (006 §3), against a single native tool call. `held` is the run's actual
// granted set (never mutated here); `ctx` is filled in with this call's per-invocation
// `bound_capabilities` (step 7) for the duration of `invoke`, and cleared again before returning
// regardless of outcome (step 10) -- see the RAII guard below, which is what makes G3 ("a
// capability handle from call n is unusable in call n+1") true by construction rather than by
// convention.
[[nodiscard]] ToolResult invoke_tool(ToolTable const& table, CapabilitySet const& held,
                                     ToolCallRequest const& request, EffectContext& ctx,
                                     ApprovalDecider const& approve,
                                     ToolInvocationAudit* audit_out = nullptr,
                                     // ADR-070: appended last (this file's own established
                                     // convention for additive parameters), default `{}` so
                                     // every existing positional call site -- including the
                                     // three "already resolved by a human" one-shot-approve
                                     // sites in rt/agent_session.hpp -- is unaffected and
                                     // never re-litigates a decision policy_driven already
                                     // deferred to a real human.
                                     PolicyDecider const& policy = {});

// decisions/ADR-160-parallel-tool-batch-scheduler.md §5 "Batch partitioning". A pure function: given
// the batch of requests actually being dispatched (the caller has already filtered out anything a
// hook denied) and the table used to resolve each one, decides whether the batch is fan-out-eligible
// at all and, if so, partitions it into concurrency classes -- no I/O, no admission, no invocation;
// safe to call and test in complete isolation from `AgentSession`.
//
// ELIGIBILITY (006 §5's existing all-or-nothing gate, unchanged by this ADR -- ADR-160 §6 names
// loosening this to a per-class partition as a real, still-open RFC-amendment question, not decided
// here): the batch is eligible only when EVERY request resolves to a descriptor that declares bare
// `Parallelizable` or `ExclusivityGroup<Name>`. An unresolved tool name counts as NOT eligible --
// resolution itself is deferred to admission (`admit_call()`'s own step 1), so an unknown-tool call
// simply falls back to running through the ordinary sequential path and fails there exactly as it
// does today; this function never invents a second "unknown tool" error path.
//
// `captures_session_state` (ADR-160 §5 MUST-FIX 1) is evaluated INDEPENDENTLY of the eligibility
// gate above: a tool that declares it forces its own call into a sequential singleton class
// unconditionally, even inside an otherwise-fully-eligible batch, and even if it ALSO declares
// `Parallelizable`/`ExclusivityGroup<Name>` -- but it still counts as "declares one of the two tags"
// for the gate itself, since 006 §5's literal text is about the declared TAG, not about what this
// scheduler is willing to actually run concurrently.
//
// `ExclusivityGroup<Name>` members sharing one `Name` land in exactly ONE class together (ADR-160
// §5 MUST-FIX 5: the whole group, not each member, is the unit of fan-out) -- never one class per
// member.
enum class concurrency_class_kind : std::uint8_t { sequential, parallel, exclusivity_group };  // ae-naming-lint: allow concurrency_class_kind — ADR-160, same enum-tag idiom as approval_outcome/policy_decision

struct ConcurrencyClass {
    concurrency_class_kind kind = concurrency_class_kind::sequential;
    std::vector<std::size_t> call_indices;  // indices into the batch passed to partition_batch(), in
                                             // emitted order; a `parallel`/`sequential` class always
                                             // holds exactly one, an `exclusivity_group` class one or
                                             // more sharing `group_name`.
    std::string group_name;                 // only meaningful when kind == exclusivity_group
};

[[nodiscard]] std::vector<ConcurrencyClass> partition_batch(
        std::vector<ToolCallRequest> const& reqs, ToolTable const& table);

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
// `ctx` is copied into the background thread's own closure. `ctx.capabilities` is an OWNED
// `std::shared_ptr<CapabilitySet const>` (ADR-061 §20.3) -- refcounted, safe to copy across the
// detach regardless of the caller's own stack frame having already returned; it was a raw, non-owning
// pointer before that fix, and copying it into a detached thread was unsafe until it landed
// (ADR-061 §7 R16). `ctx.bound_capabilities`, by contrast, genuinely remains a non-owning
// `std::vector<BoundCapability> const*` -- the SAME "host owns it, must outlive" contract as before,
// unchanged by that fix and not a hazard this function introduces. `bound` (the per-invocation
// `BoundCapability` handles from step 7) is moved into the same closure so its RAII revoke-at-step-10
// semantics are preserved exactly, just on a different thread than the one that minted them.
//
// `on_complete` fires from the BACKGROUND thread, exactly once. What "deliver this back to a run"
// means is entirely the caller's job -- `AgentSession::start_background_task()` (agent_session.hpp)
// wires this to a self-`tell()`, mirroring `TimerWake`'s own established "host arms the callback"
// shape (`test_agent_session_timer_wake.cpp`'s own precedent), not a mechanism this function invents.
// ae-naming-lint: allow BackgroundTaskCompletion — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using BackgroundTaskCompletion = std::function<void(ToolResult, ToolInvocationAudit)>;

[[nodiscard]] result<void> background_task(ToolTable const& table, CapabilitySet const& held,
                                           ToolCallRequest const& request, EffectContext ctx,
                                           ApprovalDecider const& approve,
                                           std::size_t current_background_count,
                                           BackgroundTaskCompletion on_complete);

}  // namespace agentengine
