#pragma once
// Implements rfcs/006-Tool-and-Function-Plane.md §6 -- the DECLARATION half of the tool plane: what a tool
// is at runtime (`ToolDescriptor`), how a compile-time `Tool<Derived, Policies...>` conformer becomes
// one (ADR-237 §5.2's instance form `make_tool_descriptor(ToolT{...})` / `ToolTable::from(...)`, the only
// form since #175 deleted the static-invoke `make_tool_descriptor<ToolT>()` / `ToolTable::from_tools<Ts...>()`),
// and the immutable per-run table they are resolved into (`ToolTable`). This is the I6 mechanism for
// tools: the declarative surface and the native CRTP surface must agree on exactly these fields.
//
// Moved verbatim out of `core/tool_pipeline.hpp`, which keeps the INVOCATION half (006 §3's ten
// steps, idempotency, approval and policy deciders, the audit record, batch partitioning, background
// tasks) and includes this header, so every existing includer of `tool_pipeline.hpp` compiles
// unchanged. The split follows how the two halves actually change. Measured when this was done: eight
// headers (`chat_client.hpp`, `chat_recording.hpp`, `codeact_tool_union.hpp`, `context_provider.hpp`,
// `todo_provider.hpp`, `tool_registry.hpp`, `trust/policy_reachability.hpp`,
// `trust/secret_quarantine.hpp`) referenced no invocation-side symbol at all, yet pulled in the whole
// pipeline, and through `chat_client.hpp` so did nearly every translation unit in the tree. Every
// ADR that widened the approval seam or the batch scheduler (ADR-070, ADR-158, ADR-160, ADR-061
// §46/47) therefore rebuilt consumers that only ever wanted to know what a tool looks like. Those
// eight now include this header instead.
//
// What this does NOT change: 006 §3's pipeline, its single entry point `invoke_tool()`, and the rule
// that no step is skippable. Nothing here invokes a tool; `ToolDescriptor::invoke` is only ever called
// from `tool_pipeline.hpp`'s admitted path.

#include <array>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "agentengine/core/choice_error.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// ADR-237 §9 D3: what a tool with a per-call requirement hands the pipeline after parsing its arguments ONCE. `required`
// is what must be bound for this call (steps 4/7); `arguments` is the parse itself, which the pipeline puts in
// `EffectContext::prepared_arguments` for the one `invoke` it makes, so the requirement and the effect cannot see
// different arguments.
struct PreparedCall {  // ae-naming-lint: allow PreparedCall — ADR-237 D3; 027 has not been updated to list it
    std::vector<Capability> required;
    std::shared_ptr<PreparedArguments const> arguments;
};

// One entry in the immutable per-run tool table (006 §6: "resolved at run start into an immutable
// per-run tool table"). Type-erased: `invoke` closes over ToolT's real Args/Reply types so the
// pipeline itself never needs to be a template.
// ae-naming-lint: allow ToolDescriptor — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::vector<Capability> capability_ceiling;  // from the tool's declared Capabilities<...>
    // ADR-244 §2.2 (slice 4a): stated, never defaulted -- nullopt is refused at the session's tool table and at
    // admission (`choice.missing.tool.approval`). A `Tool<>` fills it from its `Approval<M>`; an
    // `ApprovalChosenByHost` tool's provider, an MCP/WASM bridge or a hand-built descriptor states it.
    std::optional<approval_mode> approval;
    // Milestone 7 Phase B (006 §6b): `false` unless the tool declared `Backgroundable` -- read by
    // `background_task()` (agent_session.hpp) to reject an undeclared tool at authorize, before it
    // ever reaches step 8.
    bool backgroundable = false;
    std::string args_schema_json;
    std::string reply_schema_json;

    // ADR-237 §5.2: `invoke` is asynchronous -- returns `task<result<json::Value>>` (006 §1's
    // `ae::task<result<Reply>>`, type-erased). The pipeline awaits it; the two detached background workers
    // drive it with `rt::block_on` (`rt::drive_leaf_task`, which did this before, is deleted, #175).
    using InvokeFn = std::function<task<result<json::Value>>(json::Value const&, EffectContext&)>;
    InvokeFn invoke;

    // ADR-023 §6 point 4 / 007 §4 amendment: `invoke_tool`'s step 5 reads this ONLY for a
    // `text_derived` call (`ToolCallRequest::provenance` below) -- `Tool<Derived,...>::
    // declared_effect_class()` (019 §3) already existed as a compile-time accessor but was never
    // previously copied onto the runtime descriptor because nothing needed it at this layer before
    // now. Appended last (this struct's own established convention) -- `at_most_once` is the same
    // conservative default `declared_effect_class()` itself uses, so a hand-built `ToolDescriptor`
    // that predates this field (not going through `make_tool_descriptor(T{})` below) fails CLOSED
    // against auto-declassification rather than silently qualifying.
    // The type name is written QUALIFIED, deliberately. A member named `effect_class` whose type is
    // also spelled `effect_class` makes the unqualified name mean the type before the declaration and
    // the member after it -- which [basic.scope.class] makes ill-formed, and GCC 14 rejects outright
    // (`-Wchanges-meaning`), while MSVC and clang accept it. Qualifying both uses means the
    // unqualified name never denotes the type inside this class, so there is no change of meaning.
    // The member name is kept as-is: `d.effect_class` is the reading call sites already use.
    agentengine::effect_class effect_class = agentengine::effect_class::at_most_once;

    // Session-scoped-stateful-tools mechanism (ADR-028, ADR-237 §5.2): `true` only for a descriptor
    // whose tool declares `CapturesSessionState` (`core/tool.hpp`), i.e. whose `invoke` reads
    // session-scoped state (a persistent interpreter's exec state, mounted-skills tracking) rather
    // than being a pure, ownership-free call. `false` (the default) for every ordinary stateless
    // tool, unaffected. `background_task()` below refuses to background any descriptor with this set —
    // `start_background_task()` (agent_session.hpp) detaches a real `std::thread` that would then
    // hold a reference into session state with no synchronization against `AgentSession::
    // fork_from()`/`clear_in_process_state()` (neither is part of `protocol`, so neither is
    // Quark-`Sequential`-serialized against a detached background thread) — a real dangling-
    // reference/data-race hazard found by design review, closed here structurally rather than left
    // as a documented-only rule a future caller could violate by accident.
    bool captures_session_state = false;

    // decisions/archive/ADR-066-context-provider-attribution-provenance.md: same stamp as
    // `Message::attribution`, same seam (`assemble_context()`), same "nullopt means not
    // contributor-sourced" convention (a tool from `make_tool_descriptor(ToolT{})`'s ordinary,
    // host-declared static tool table is never contributor-sourced, so stays nullopt). MAF has no
    // equivalent for this field (`AIContextProvider.cs`'s `mergedTools` is a bare, unstamped
    // `Concat`) -- this is a place this project's design goes further than its own surveyed prior
    // art, needed for the not-yet-implemented turn-middleware tool-arbitration use case
    // (decisions/archive/ADR-067-middleware-turn-point-pre-model-enforcement.md). Appended last, this
    // struct's own established convention.
    std::optional<ContributorProvenance> attribution;

    // Issue #13: `args_schema_json` is compile-time-derived and fixed for the lifetime of a
    // `ToolTable`/session, but the wire-format `translate_tool()` in the OpenAI/Anthropic backends
    // re-parsed it into a fresh `json::Value` on every single `chat()`/`chat_stream()` call that
    // included this tool -- redundant work repeated once per turn per tool, for the whole run.
    // `make_tool_descriptor(ToolT{})`/`make_tool_descriptor(ToolT{...})` below parse it
    // ONCE, here, at registration time; `args_schema_value_cached` is `false` for any hand-built
    // descriptor that predates this field (memory_provider.hpp/vector_rag_context_provider.hpp/
    // mcp_tool_bridge.hpp/native_providers.hpp all assign `args_schema_json` directly, not through
    // either factory) -- those fail CLOSED to the pre-existing re-parse-every-call behavior, never
    // silently trusting a default-constructed (null) `args_schema_value`.
    json::Value args_schema_value;
    bool args_schema_value_cached = false;

    // decisions/archive/ADR-158-tool-concurrency-exclusivity-policy.md §4: `nullopt` unless the tool
    // declared `ExclusivityGroup<Name>` (`core/tool.hpp`). Appended last, this struct's own
    // established convention.
    std::optional<std::string> exclusivity_group;

    // decisions/archive/ADR-160-parallel-tool-batch-scheduler.md §5: `true` only when the tool declared bare
    // `Parallelizable` (`Tool<Derived,...>::kHasParallelizable`) -- distinct from `exclusivity_group`
    // above (mutually exclusive by `Tool<>`'s own static_assert, ADR-158). Read by
    // `AgentSession::partition_batch()` (rt/agent_session.hpp) to decide batch eligibility and
    // per-call concurrency class. `exclusivity_group`/`parallelizable` together are the only two
    // ways a `ToolDescriptor` opts into anything other than the default fully-sequential class.
    bool parallelizable = false;

    // decisions/archive/ADR-212-deterministic-tool-output-declaration.md: `true` only when the tool declared
    // `Deterministic` and `make_tool_descriptor(ToolT{})` built this descriptor around `ToolT::invoke`.
    // Hand-built descriptors (MCP bridge, WASM, providers) leave it `false`: no source they are built
    // from can express the claim (ADR-212 §3.4). Code that replaces `invoke` on an existing descriptor
    // must clear it unless the wrapper returns the inner success value unchanged (§3.5). The engine
    // never reads it; hosts use `rerun_comparable()` below. Appended last, this struct's own
    // established convention.
    bool deterministic = false;

    // decisions/archive/ADR-217-capped-grant-admits-grant-enforcing-tools.md: `true` only when the tool
    // declared `EnforcesGrantedCaps` (core/tool.hpp) -- its invoke enforces the quantity caps of the
    // grant it is bound under, not numbers from its own declaration. Admission then binds each ceiling
    // entry with `CapabilitySet::bind_clamped()` (see `bind_ceiling_entry()` below). `false` (every
    // hand-built descriptor, every tool that did not declare it) keeps the exact `contains()` rule.
    // Code that replaces `invoke` on an existing descriptor must clear it unless the new invoke either
    // performs every effect through the inner invoke (a recording wrapper) or performs none (a replay
    // double) -- the same rule ADR-212 §3.5 gives `deterministic`.
    // Appended last, this struct's own established convention.
    bool enforces_granted_caps = false;

    // ADR-237 §9 D3 (#176): a tool that declared `PerCallRequirement<...>`. `requirement_bounds` is the declared bound
    // (kind and mount); `prepare` parses the arguments once and returns the requirement plus the run over those same
    // arguments. When `prepare` is set, `invoke` above runs only with the parse in `EffectContext::prepared_arguments`
    // and refuses (`tool.requirement_unbound`) without it: a caller that reaches for `invoke` instead of admission would
    // skip binding the requirement, so that path fails closed. The pipeline still calls `invoke` (never around it), so a
    // wrapper that replaces `invoke` stays in the path. Empty/unset for every tool that did not declare one.
    std::vector<Capability> requirement_bounds;
    using PrepareFn = std::function<result<PreparedCall>(json::Value const&)>;
    PrepareFn prepare;
};

// ADR-217: the one admission rule for a tool's ceiling entry, shared by the synchronous pipeline, the
// background pipeline, the background job runner and the policy-reachability tool, so the static
// analysis and the runtime cannot disagree. `bind()` (exact `contains()`) unless the tool declared
// `EnforcesGrantedCaps`, then `bind_clamped()`.
[[nodiscard]] inline result<BoundCapability> bind_ceiling_entry(CapabilitySet const&  held,
                                                                ToolDescriptor const& tool,
                                                                Capability const&     requirement) {
    return tool.enforces_granted_caps ? held.bind_clamped(requirement) : held.bind(requirement);
}
[[nodiscard]] inline bool ceiling_entry_admitted(CapabilitySet const& held, ToolDescriptor const& tool,
                                                 Capability const& requirement) {
    return tool.enforces_granted_caps ? held.clamped_binding(requirement).has_value()
                                      : held.contains(requirement);
}

// ADR-212 §3.3: whether a host may re-run this call on another node and compare the two successful
// replies with `schema::json_value_equal`. `Tool<>` already rejects `Deterministic` without `pure` at
// compile time; this repeats the rule for hand-built descriptors, which never pass through `Tool<>`,
// and excludes session-state-capturing tools, whose hidden input a verifying node does not have.
[[nodiscard]] inline bool rerun_comparable(ToolDescriptor const& d) noexcept {
    return d.deterministic && d.effect_class == agentengine::effect_class::pure &&
           !d.captures_session_state;
}

// ADR-237 §5.2 (transitional): lift a synchronous json-level invoke into the (now async) seam, so a
// hand-built descriptor need not be rewritten as a coroutine before it is migrated to `Tool<>`.
// Deleted once §5.2's shape-3 descriptors are migrated (ADR-237 §5.2 step D).
[[nodiscard]] inline ToolDescriptor::InvokeFn make_sync_invoke(
    std::function<result<json::Value>(json::Value const&, EffectContext&)> fn) {
    return [fn = std::move(fn)](json::Value const& args, EffectContext& ctx)
               -> task<result<json::Value>> { co_return fn(args, ctx); };
}

// ADR-237 §5.2: `make_tool_descriptor_with_invoke` lived here. It is DELETED -- every one of its call
// sites (production and test) is now an instance-form `Tool<>` conformer registered through the
// value-taking `make_tool_descriptor(ToolT)` below, so a descriptor's invoke IS the declared code and no
// closure-over-session-state path remains. Its old implicit `captures_session_state = true` is now the
// explicit `CapturesSessionState` policy tag (`core/tool.hpp`).

// ADR-237 §5.2: the instance-invoke form. `ToolT` declares a non-static `const`
// `task<result<Reply>> invoke(Args, EffectContext&)`; the tool object is moved into a descriptor-owned
// `shared_ptr` and the seam calls it through that owner. A stateless tool is an empty object
// (`make_tool_descriptor(ToolT{})`); a stateful one carries its injected state by value, which is how
// the former `make_tool_descriptor_with_invoke` call sites become real `Tool<>` conformers.
// Registration is `ToolTable::from(ToolT{...}, ...)`. This is the §5.2 shape and, since #175, the only one:
// the zero-argument static-invoke `make_tool_descriptor<ToolT>()` that preceded it is deleted.
// ADR-237 §9 D3: a tool with this member proposes its per-call requirement from the parsed arguments.
template <class ToolT>
concept HasRequiredCapabilities = requires(ToolT const& t, typename ToolT::Args const& a) {  // ae-naming-lint: allow HasRequiredCapabilities — ADR-237 D3; 027 has not been updated to list it
    { t.required_capabilities(a) } -> std::convertible_to<std::vector<Capability>>;
};

namespace tool_descriptor_detail {
// A named coroutine taking its state BY VALUE, so the frame owns the tool and the parsed arguments (a coroutine lambda
// would reference its closure, which the caller may destroy first).
template <class ToolT>
[[nodiscard]] task<result<json::Value>> run_prepared(std::shared_ptr<ToolT const> owned,
                                                     std::shared_ptr<typename ToolT::Args const> args,
                                                     EffectContext& ctx) {
    auto reply = co_await owned->invoke(*args, ctx);
    if (!reply) co_return std::unexpected(reply.error());
    co_return schema::to_json(*reply);
}
[[nodiscard]] inline error requirement_unbound_error() {
    return error{failure_class::contract,
                 "this tool declares a per-call requirement; it can only run with the arguments admission parsed",
                 "tool.requirement_unbound"};
}
// Takes the parse out of the context (single use) and returns it only if it is this tool's, for this `Args` type.
template <class ToolT>
[[nodiscard]] std::shared_ptr<typename ToolT::Args const> take_prepared_arguments(EffectContext& ctx) {
    std::shared_ptr<PreparedArguments const> const prepared = std::move(ctx.prepared_arguments);
    ctx.prepared_arguments.reset();
    if (!prepared || prepared->tool_name != ToolT::name || prepared->type == nullptr ||
        *prepared->type != typeid(typename ToolT::Args)) {
        return nullptr;
    }
    return std::static_pointer_cast<typename ToolT::Args const>(
        std::const_pointer_cast<void>(prepared->arguments));
}
}  // namespace tool_descriptor_detail

template <class ToolT>
[[nodiscard]] ToolDescriptor make_tool_descriptor(ToolT tool) {
    static_assert(ToolT::kHasPerCallRequirement == HasRequiredCapabilities<ToolT>,
                  "ADR-237 D3: PerCallRequirement<...> and required_capabilities(Args const&) const go together -- "
                  "declare both or neither");
    ToolDescriptor d;
    d.name = std::string(ToolT::name);
    d.description = std::string(ToolT::description);
    d.capability_ceiling = ToolT::declared_capabilities();
    d.approval = ToolT::declared_approval();
    d.backgroundable = ToolT::declared_backgroundable();
    d.effect_class = ToolT::declared_effect_class();
    d.exclusivity_group = ToolT::declared_exclusivity_group();
    d.parallelizable = ToolT::kHasParallelizable;
    // ADR-212/§5.2: the instance invoke IS the declared code, so the Deterministic/
    // EnforcesGrantedCaps claims carry over from the declaration, exactly as the static factory above.
    d.deterministic = ToolT::declared_deterministic();
    d.enforces_granted_caps = ToolT::declared_enforces_granted_caps();
    d.captures_session_state = ToolT::declared_captures_session_state();  // ADR-237 §5.2
    d.args_schema_json = ToolT::args_schema();
    d.reply_schema_json = ToolT::reply_schema();
    if (auto parsed = json::parse(d.args_schema_json)) {
        d.args_schema_value = std::move(*parsed);
        d.args_schema_value_cached = true;
    }
    auto owned = std::make_shared<ToolT>(std::move(tool));
    if constexpr (ToolT::kHasPerCallRequirement) {
        d.requirement_bounds = ToolT::declared_requirement_bounds();
        d.prepare = [owned](json::Value const& args_value) -> result<PreparedCall> {
            auto parsed = schema::from_json<typename ToolT::Args>(args_value);
            if (!parsed) return std::unexpected(parsed.error());
            auto args = std::make_shared<typename ToolT::Args const>(std::move(*parsed));
            PreparedCall call;
            call.required  = owned->required_capabilities(*args);
            call.arguments = std::make_shared<PreparedArguments const>(PreparedArguments{
                std::string(ToolT::name), &typeid(typename ToolT::Args), std::move(args)});
            return call;
        };
        // The arguments are NOT re-parsed from `args_value`: the requirement was computed from the parse in the context.
        d.invoke = [owned](json::Value const&, EffectContext& ctx) -> task<result<json::Value>> {
            auto args = tool_descriptor_detail::take_prepared_arguments<ToolT>(ctx);
            if (!args) co_return std::unexpected(tool_descriptor_detail::requirement_unbound_error());
            co_return co_await tool_descriptor_detail::run_prepared<ToolT>(owned, std::move(args), ctx);
        };
    } else {
        d.invoke = [owned](json::Value const& args_value, EffectContext& ctx) -> task<result<json::Value>> {
            auto args = schema::from_json<typename ToolT::Args>(args_value);
            if (!args) co_return std::unexpected(args.error());
            auto reply = co_await owned->invoke(*args, ctx);
            if (!reply) co_return std::unexpected(reply.error());
            co_return schema::to_json(*reply);
        };
    }
    return d;
}

// ADR-244 §2.2 (slice 4a): the stated approval modes, the table `choice.missing.tool.approval` lists.
inline constexpr std::array<std::string_view, 3> kToolApprovalOptions{
    "approval_mode::never_require",
    "approval_mode::always_require",
    "approval_mode::policy_driven",
};

// The refusal for a descriptor whose approval was never stated. Read at the session's per-turn tool table
// (the whole turn is refused) and again at admission (`admit_call`), so a table built some other way still
// cannot run the tool.
[[nodiscard]] inline error tool_approval_missing(std::string_view tool_name) {
    error e = choice_missing("tool.approval", kToolApprovalOptions);
    e.message = "tool '" + std::string(tool_name) + "': " + e.message;
    return e;
}

// Forward-declared only -- the real type lives in core/tool_registry.hpp (gap-4 closure, ADR-054),
// which itself depends on ToolDescriptor/ToolTable from THIS header, so the dependency can only run
// one direction. `ToolTable::from_names()` below is declared here (next to `from`/
// `from_descriptors`, where a caller would look for it) and DEFINED out-of-line in
// core/tool_registry.hpp -- a caller of `from_names()` must include that header, not just this one.
// ae-naming-lint: allow ToolRegistry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ToolRegistry;

// 006 §6: static tools are resolved once into an immutable table at run start -- a mid-run change
// to what's registered cannot alter what a run is allowed to call. Linear lookup: tool counts in
// this milestone's scope are single digits, not a hot path.
// ae-naming-lint: allow ToolTable — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ToolTable {
public:
    // ADR-237 §5.2: the instance-form registration path. Each argument is a `Tool<>` instance -- a
    // stateless tool is `ToolT{}`, a stateful one carries its injected state by value -- moved into
    // its descriptor (see the value-taking `make_tool_descriptor` above). This is where the former
    // `make_tool_descriptor_with_invoke` call sites went. `from()` with no arguments is an empty
    // table.
    template <class... ToolTs>
    [[nodiscard]] static ToolTable from(ToolTs&&... tools) {
        ToolTable t;
        (t.descriptors_.push_back(
             make_tool_descriptor<std::decay_t<ToolTs>>(std::forward<ToolTs>(tools))),
         ...);
        return t;
    }

    // A second, runtime construction path alongside `from(ToolTs{}...)` -- needed when the
    // actual offered set is a runtime-computed SUBSET of a compile-time-declared universe (e.g.
    // `core/skill_tool_scoping.hpp` filtering by which skills are currently mounted). The result is
    // just as immutable once built as one from `from(...)` -- this does not weaken 006 §6's
    // "resolved once into an immutable table at run start" invariant, it only adds a second way to
    // reach that same shape.
    [[nodiscard]] static ToolTable from_descriptors(std::vector<ToolDescriptor> descriptors) {
        ToolTable t;
        t.descriptors_ = std::move(descriptors);
        return t;
    }

    // A THIRD runtime construction path, name-keyed rather than descriptor-keyed -- gap-4/gap-5
    // closure (ADR-054). DECLARED here, DEFINED in core/tool_registry.hpp (see the ToolRegistry
    // forward declaration above for why) -- delegates to from_descriptors() above, no new ToolTable
    // machinery.
    [[nodiscard]] static result<ToolTable> from_names(std::vector<std::string> const& names,
                                                        ToolRegistry const& registry);

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

}  // namespace agentengine
