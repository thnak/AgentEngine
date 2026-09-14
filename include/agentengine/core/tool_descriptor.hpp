#pragma once
// Implements 006-Tool-and-Function-Plane.md §6 -- the DECLARATION half of the tool plane: what a tool
// is at runtime (`ToolDescriptor`), how a compile-time `Tool<Derived, Policies...>` conformer becomes
// one (`make_tool_descriptor<ToolT>()`, `make_tool_descriptor_with_invoke<ToolT>()`), and the
// immutable per-run table they are resolved into (`ToolTable`). This is the I6 mechanism for tools:
// the declarative surface and the native CRTP surface must agree on exactly these fields.
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

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
// ae-naming-lint: allow ToolDescriptor — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
    // The type name is written QUALIFIED, deliberately. A member named `effect_class` whose type is
    // also spelled `effect_class` makes the unqualified name mean the type before the declaration and
    // the member after it -- which [basic.scope.class] makes ill-formed, and GCC 14 rejects outright
    // (`-Wchanges-meaning`), while MSVC and clang accept it. Qualifying both uses means the
    // unqualified name never denotes the type inside this class, so there is no change of meaning.
    // The member name is kept as-is: `d.effect_class` is the reading call sites already use.
    agentengine::effect_class effect_class = agentengine::effect_class::at_most_once;

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

    // decisions/ADR-066-context-provider-attribution-provenance.md: same stamp as
    // `Message::attribution`, same seam (`assemble_context()`), same "nullopt means not
    // contributor-sourced" convention (a tool from `make_tool_descriptor<ToolT>()`'s ordinary,
    // host-declared static tool table is never contributor-sourced, so stays nullopt). MAF has no
    // equivalent for this field (`AIContextProvider.cs`'s `mergedTools` is a bare, unstamped
    // `Concat`) -- this is a place this project's design goes further than its own surveyed prior
    // art, needed for the not-yet-implemented turn-middleware tool-arbitration use case
    // (decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md). Appended last, this
    // struct's own established convention.
    std::optional<ContributorProvenance> attribution;

    // Issue #13: `args_schema_json` is compile-time-derived and fixed for the lifetime of a
    // `ToolTable`/session, but the wire-format `translate_tool()` in the OpenAI/Anthropic backends
    // re-parsed it into a fresh `json::Value` on every single `chat()`/`chat_stream()` call that
    // included this tool -- redundant work repeated once per turn per tool, for the whole run.
    // `make_tool_descriptor<ToolT>()`/`make_tool_descriptor_with_invoke<ToolT>()` below parse it
    // ONCE, here, at registration time; `args_schema_value_cached` is `false` for any hand-built
    // descriptor that predates this field (memory_provider.hpp/vector_rag_context_provider.hpp/
    // mcp_tool_bridge.hpp/native_providers.hpp all assign `args_schema_json` directly, not through
    // either factory) -- those fail CLOSED to the pre-existing re-parse-every-call behavior, never
    // silently trusting a default-constructed (null) `args_schema_value`.
    json::Value args_schema_value;
    bool args_schema_value_cached = false;

    // decisions/ADR-158-tool-concurrency-exclusivity-policy.md §4: `nullopt` unless the tool
    // declared `ExclusivityGroup<Name>` (`core/tool.hpp`). Appended last, this struct's own
    // established convention.
    std::optional<std::string> exclusivity_group;

    // decisions/ADR-160-parallel-tool-batch-scheduler.md §5: `true` only when the tool declared bare
    // `Parallelizable` (`Tool<Derived,...>::kHasParallelizable`) -- distinct from `exclusivity_group`
    // above (mutually exclusive by `Tool<>`'s own static_assert, ADR-158). Read by
    // `AgentSession::partition_batch()` (rt/agent_session.hpp) to decide batch eligibility and
    // per-call concurrency class. `exclusivity_group`/`parallelizable` together are the only two
    // ways a `ToolDescriptor` opts into anything other than the default fully-sequential class.
    bool parallelizable = false;
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
    d.exclusivity_group = ToolT::declared_exclusivity_group();
    d.parallelizable = ToolT::kHasParallelizable;
    d.args_schema_json = ToolT::args_schema();
    d.reply_schema_json = ToolT::reply_schema();
    if (auto parsed = json::parse(d.args_schema_json)) {
        d.args_schema_value = std::move(*parsed);
        d.args_schema_value_cached = true;
    }
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
    d.exclusivity_group = ToolT::declared_exclusivity_group();
    d.parallelizable = ToolT::kHasParallelizable;
    d.args_schema_json = ToolT::args_schema();
    d.reply_schema_json = ToolT::reply_schema();
    if (auto parsed = json::parse(d.args_schema_json)) {
        d.args_schema_value = std::move(*parsed);
        d.args_schema_value_cached = true;
    }
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

// Forward-declared only -- the real type lives in core/tool_registry.hpp (gap-4 closure, ADR-054),
// which itself depends on ToolDescriptor/ToolTable from THIS header, so the dependency can only run
// one direction. `ToolTable::from_names()` below is declared here (next to `from_tools`/
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
