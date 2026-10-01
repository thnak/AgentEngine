#pragma once
// Implements 002-Agent-Model-and-Authoring.md §2.1 ("Running an agent", as amended by
// decisions/ADR-226-agent-run-bridge.md) and the run half of 015 §1's equivalence rule (I6): the bridge
// from a declared agent -- `struct A : Agent<A, Policies...>` compiled by `register_agent<A>()`, or a
// 015 Agent document compiled by `compile_agent_document()` -- to a RUNNING `rt::AgentSession`
// (issue #46, issue #32 Layer A).
//
// Before this header the two halves were disconnected: `register_agent<A>()` produced a validated,
// read-only `AgentMetadata` that nothing consumed, and the only ways to run a turn were constructing
// `rt::AgentSession<ChatClientT, ...>` by hand or the quickstart builders -- neither of which reads an
// agent's declared policies, so a host had to re-derive the whole policy set a second time by hand.
//
// Shape (ADR-226 §2): ONE bridge over the compiled `AgentMetadata` value, `bind_agent_session()`,
// serving both authoring surfaces; `make_agent_session<A>()` is that bridge plus `register_agent<A>()`
// plus the one thing only the C++ type can supply (`OutputSchema<T>`'s typed response validator).
// `ChatClientT` is a template parameter because it is the caller's already-constructed client -- the
// session type depends on it, the agent's metadata does not. The result is the quickstart `Bundle`
// (core/session_builder.hpp), reused rather than paralleled: the same ownership/lifetime ordering, the
// same `ask()`/`ask_stream()`, plus `run()` for the whole `AgentResponse`.
//
// How each compiled field reaches the running session (ADR-226 §3 has the table, with what is NOT
// enforced and why):
//   instructions      -> the session's static system instructions (host-authored, untainted), ahead of
//                        the engine's own capability summary.
//   tools             -> `AgentToolSurface<>`, the session's context-provider slot, contributes exactly
//                        the declared descriptors every round (and to every resumed round, so an approval
//                        resume dispatches against the same set). An undeclared tool is never offered and
//                        a call to one fails `tool.unknown` in the ordinary pipeline.
//   capability_ceiling-> the session's CapabilitySet is the CALLER's grants narrowed to the ceiling --
//                        never the ceiling itself (I2: the ceiling is a bound, not a grant; the
//                        `invoke_agent_tool()` ambient-authority bug ADR-059 fixed, not reintroduced).
//   max_turns / token_budget -> `AgentSession::initialize()`; a per-run override may only lower them
//                        (002 §9 Q2), a higher one is refused, not clamped.
//   approval          -> `always_require` raises every declared tool to `always_require`; the other two
//                        modes leave each tool's own declaration alone (an agent may not loosen a tool).
//   concurrency       -> `sequential` (the default) strips `Parallelizable`/`ExclusivityGroup<Name>` from
//                        the descriptors, so a batch never fans out (001 §4's two-axis rule).
//   output_schema     -> `set_output_schema()` with a validator for the declared `T` -- native path only;
//                        a metadata-only bind of an agent with a schema fails closed (no validator).
//   chat_client_id, telemetry, sandbox_profile, stateless_pool_size, name/version -> recorded in the
//                        session's metadata map for the host; NOT enforced by the session (no session-level
//                        consumer exists -- ADR-226 §3 names each one).

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/agent_library_manifest.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine {

// The session's context-provider slot for a declared agent: forwards to `Inner` (plain unbounded history
// by default) and appends the agent's declared tool descriptors to every contribution. The descriptors
// are bound ONCE, by the bridge, after the session exists (`AgentSession::history_provider_` is always
// default-constructed, the same reason `ComposedContextProvider::engage()` exists).
//
// Three properties, each from a finding of ADR-226's own red-team pass:
//   - not bound -> `agent_tool_surface.not_bound`. A default-constructed surface is what
//     `AgentSession::clear_in_process_state()` leaves behind -- and that call also resets `max_turns`/
//     `token_budget` to unbounded. Running on would be an agent with no tools AND no budget; refusing is
//     the only safe reading (R6).
//   - `Inner` contributes a tool with a declared tool's name -> `agent_tool_surface.name_collision`.
//     Silently appending would offer the model two descriptors under one name; which one dispatches would
//     depend on `ToolTable::find()`'s first-match order, not on the declaration (R7).
//   - MOVE-ONLY (R9). `AgentSession::fork_from()` copy-assigns this slot -- and copies neither the
//     source's capabilities nor its max_turns/token_budget (fork_core_from() leaves those to the
//     target). A copyable surface would carry `bound_ == true` into a fork whose turn bound is unset:
//     the declared tools, runnable with no budget, exactly what the `not_bound` guard exists to stop.
//     Deleting the copy turns `fork_from()` on an agent session into a compile error at that call site
//     (fork_from() is an ordinary member, instantiated only when called), the precedent
//     `ComposedContextProvider` set for the same slot (ADR-074, quickstart finding 9). A move leaves the
//     source unbound (quickstart finding 11's lesson: a moved-from instance must fail closed, not run
//     empty).
template <class Inner = HistoryProvider<Window<0>>>
    requires ContextProvider<Inner> && std::default_initializable<Inner>
// ae-naming-lint: allow AgentToolSurface — ADR-226 (002 §2.1 run bridge); 027 not yet updated
class AgentToolSurface {
public:
    static constexpr std::string_view name = "agent_tools";  // ae-naming-lint: allow name — ADR-066 §3 contributor identity

    AgentToolSurface() = default;
    AgentToolSurface(AgentToolSurface const&)            = delete;
    AgentToolSurface& operator=(AgentToolSurface const&) = delete;
    AgentToolSurface(AgentToolSurface&& other) noexcept(std::is_nothrow_move_constructible_v<Inner>)
        : inner_(std::move(other.inner_)), tools_(std::move(other.tools_)), bound_(other.bound_) {
        other.tools_ = ToolTable{};
        other.bound_ = false;
    }
    AgentToolSurface& operator=(AgentToolSurface&& other) noexcept(std::is_nothrow_move_assignable_v<Inner>) {
        if (this != &other) {
            inner_       = std::move(other.inner_);
            tools_       = std::move(other.tools_);
            bound_       = other.bound_;
            other.tools_ = ToolTable{};
            other.bound_ = false;
        }
        return *this;
    }

    // Single use per instance: the declared set is fixed for the session's life (006 §6's immutable
    // per-run table, applied to the agent's declaration).
    [[nodiscard]] result<void> bind(ToolTable tools) {
        if (bound_) {
            return std::unexpected(error{failure_class::contract,
                                         "AgentToolSurface::bind() called twice -- an agent's declared tool set "
                                         "is fixed once the session is bound",
                                         "agent_tool_surface.already_bound"});
        }
        tools_ = std::move(tools);
        bound_ = true;
        return {};
    }
    [[nodiscard]] bool bound() const noexcept { return bound_; }
    [[nodiscard]] ToolTable const& tools() const noexcept { return tools_; }
    [[nodiscard]] Inner& inner() noexcept { return inner_; }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx, EffectContext& ctx) {
        if (!bound_) {
            co_return std::unexpected(error{failure_class::contract,
                                            "this agent session's declared tool surface is not bound (a session "
                                            "built by bind_agent_session() was cleared, or the slot was replaced)",
                                            "agent_tool_surface.not_bound"});
        }
        result<ContextContribution> contribution = co_await inner_.on_context(session_ctx, ctx);
        if (!contribution) co_return contribution;
        for (ToolDescriptor const& declared : tools_.descriptors()) {
            bool const clash = std::any_of(contribution->tools.begin(), contribution->tools.end(),
                                           [&declared](ToolDescriptor const& d) { return d.name == declared.name; });
            if (clash) {
                co_return std::unexpected(error{failure_class::contract,
                                                "a context provider contributed a tool named '" + declared.name +
                                                    "', which the agent already declares",
                                                "agent_tool_surface.name_collision"});
            }
            contribution->tools.push_back(declared);
        }
        co_return contribution;
    }

    [[nodiscard]] task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        return inner_.on_turn_end(turn, ctx);
    }

private:
    Inner     inner_{};
    ToolTable tools_{};
    bool      bound_ = false;
};

static_assert(ContextProvider<AgentToolSurface<>>, "AgentToolSurface must be a ContextProvider (005 §5)");

// What the CALLER supplies to run a declared agent -- everything here is host code (I3: nothing in this
// struct may be derived from model output). The agent's own declaration supplies everything else.
// ae-naming-lint: allow AgentSessionOptions — ADR-226 (002 §2.1 run bridge); 027 not yet updated
struct AgentSessionOptions {
    std::string session_id = "s-agent";
    Principal   principal{"p-agent", ""};

    // The authority the caller actually holds and is willing to hand this session. Narrowed to the agent's
    // declared `Capabilities<...>` ceiling (`narrow_to_ceiling()` below) -- never widened to it: a ceiling
    // entry the caller did not grant is simply absent from the session.
    std::vector<Capability> grants;

    // Credentials the bound ChatClient itself resolves (a `cap::Secret` for its API key): deployment
    // configuration, not something the agent is (002 §3's policy-vs-configuration rule), so NOT narrowed by
    // the agent's ceiling -- otherwise every real backend would need its key in every agent's
    // `Capabilities<...>`. `cap::Secret` ONLY: anything else here is refused (`agent_session.
    // chat_client_grant_not_secret`), so this field cannot smuggle `cap::Schedule`/`cap::AgentCall`/... past
    // the ceiling (ADR-226 red-team R2). A declared tool still cannot reach these: a tool's per-call handle
    // is bound from the tool's OWN declared capabilities, which `register_agent<A>()` already proved lie
    // inside the ceiling.
    std::vector<Capability> chat_client_grants;

    // 002 §9 Q2 per-run overrides: narrowing only. A value above the agent's compiled bound is REFUSED
    // (`agent_session.override_widens`), not clamped -- a caller who asked for more than the agent allows
    // should hear about it.
    std::optional<std::uint64_t> max_turns;
    std::optional<std::uint64_t> token_budget;

    // Same semantics as `RawQuickstartSessionBuilder::approve_tools()`/`.policy()`: decide among calls that
    // ALREADY need a decision; never make a call need less approval than its tool (or the agent) declares.
    std::optional<std::vector<std::string>> approve_tools;
    PolicyDecider policy;
};

// The running form of a declared agent: the quickstart `Bundle` with the agent's tool surface in the
// context slot. `Store` is an empty `InMemorySecretStore` -- a caller-constructed ChatClient that needs a
// real secret store owns that store itself, outside the bundle, exactly as it owns the client's other
// constructor arguments.
template <class ChatClientT>
// ae-naming-lint: allow AgentBundle — ADR-226 (002 §2.1 run bridge); 027 not yet updated
using AgentBundle = quickstart::Bundle<ChatClientT, InMemorySecretStore, AgentToolSurface<>>;

// decisions/ADR-234 (issue #60): the same bundle with a caller-chosen `Inner` behind the agent's tool
// surface -- how the harness (core/harness.hpp) composes ON this bridge instead of a third session-
// construction path. `AgentBundle<C>` is exactly `AgentBundleWith<C>`.
template <class ChatClientT, class Inner = HistoryProvider<Window<0>>>
// ae-naming-lint: allow AgentBundleWith — ADR-234 (harness composes on the ADR-226 bridge); 027 not yet updated
using AgentBundleWith = quickstart::Bundle<ChatClientT, InMemorySecretStore, AgentToolSurface<Inner>>;

// The caller's grants, narrowed to the agent's ceiling. Each returned entry is covered BOTH by something
// the caller granted and by something the agent declared:
//   - a grant the ceiling covers is kept as granted (with its own, possibly tighter, quota/size caps);
//   - a ceiling entry the grants cover is kept as declared (the caller granted something broader, e.g. a
//     whole mount where the agent declared one path -- the session gets the agent's narrower entry).
// A grant the ceiling does not cover, and a ceiling entry no grant covers, are dropped. Nothing here can
// produce an entry outside either side. `CapabilitySet::grant_root()` is used only as a local coverage
// oracle over the two host-supplied lists; neither oracle set leaves this function.
[[nodiscard]] inline std::vector<Capability> narrow_to_ceiling(std::vector<Capability> const& grants,
                                                               std::vector<Capability> const& ceiling) {
    CapabilitySet const ceiling_set = CapabilitySet::grant_root(ceiling);
    CapabilitySet const grant_set   = CapabilitySet::grant_root(grants);
    std::vector<Capability> out;
    for (Capability const& g : grants) {
        if (ceiling_set.contains(g)) out.push_back(g);
    }
    for (Capability const& c : ceiling) {
        if (grant_set.contains(c) && !CapabilitySet::grant_root(out).contains(c)) out.push_back(c);
    }
    return out;
}

namespace agent_bind_detail {

using OutputValidator = std::function<result<void>(std::string_view)>;

// decisions/ADR-234: a host-code step over the agent's declared descriptors, applied AFTER the agent-level
// floors below. The bridge's own callers pass none; the harness passes one that may only make a tool MORE
// gated (core/harness.hpp's plan-gate coverage), never less -- the bridge cannot check that direction for an
// arbitrary callable, so this stays in `agent_bind_detail`, not on any public signature.
using DescriptorFilter = std::function<void(std::vector<ToolDescriptor>&)>;

// Bundle's constructor is private; this is the one friend that reaches it (session_builder.hpp).
struct BundleFactory {
    template <class BundleT, class Store, class SessionT>
    [[nodiscard]] static BundleT make(std::unique_ptr<Store> store, std::unique_ptr<CapabilitySet> capabilities,
                                      std::unique_ptr<SessionT> session) {
        return BundleT(std::move(store), std::move(capabilities), std::move(session));
    }
};

[[nodiscard]] inline std::string_view telemetry_name(telemetry_capture c) noexcept {
    switch (c) {
        case telemetry_capture::none: return "none";
        case telemetry_capture::metadata_only: return "metadata_only";
        case telemetry_capture::full: return "full";
    }
    return "metadata_only";
}

[[nodiscard]] inline error override_widens(std::string what) {
    return error{failure_class::policy,
                 "a per-run " + what + " override may only lower the agent's declared bound (002 §9 Q2)",
                 "agent_session.override_widens"};
}

// The agent's declared descriptors, with the agent-level policies that act on tools applied. Both moves
// are in the narrowing direction only.
[[nodiscard]] inline std::vector<ToolDescriptor> declared_descriptors(AgentMetadata const& meta) {
    std::vector<ToolDescriptor> tools = meta.tools.descriptors();
    for (ToolDescriptor& d : tools) {
        // 002 §3 Approval<Mode> at agent level is a FLOOR: always_require raises every tool; policy_driven
        // (the default) and never_require leave each tool's own declaration exactly as written -- an agent
        // declaring never_require does not un-gate a tool that declared always_require (002 §9 Q2's
        // "more cautious, never less" rule applied at the declaration, ADR-226 §3).
        if (meta.approval == approval_mode::always_require) d.approval = approval_mode::always_require;
        // 001 §4 / 006 §5: parallel only when the AGENT allows it AND every tool in the batch declares it.
        if (meta.concurrency == concurrency_mode::sequential) {
            d.parallelizable = false;
            d.exclusivity_group.reset();
        }
    }
    return tools;
}

template <class ChatClientT, class Inner = HistoryProvider<Window<0>>>
[[nodiscard]] result<AgentBundleWith<ChatClientT, Inner>> bind(AgentMetadata const& meta, ChatClientT client,
                                                               AgentSessionOptions opts, OutputValidator validator,
                                                               ChatClientRegistry const* registry,
                                                               SandboxBackendRegistry const* sandbox_registry,
                                                               DescriptorFilter const& filter = {}) {
    using BundleT  = AgentBundleWith<ChatClientT, Inner>;
    using SessionT = typename BundleT::SessionT;

    // 002 §6 / I6: the same validator for every metadata, native or declarative.
    if (auto ok = validate_agent_metadata(meta, registry, sandbox_registry); !ok) {
        return std::unexpected(ok.error());
    }
    if (meta.output_schema_json.has_value() && !validator) {
        // No generic JSON-Schema validator exists in this tree; enforcing a schema needs the declared C++
        // type. Running anyway would hand back unvalidated text where the agent promised structure.
        return std::unexpected(error{failure_class::contract,
                                     "the agent declares an output schema but no validator for it was supplied "
                                     "(bind a native agent with make_agent_session<A>())",
                                     "agent_session.output_schema_unvalidated"});
    }

    // 002 §9 Q2: narrowing-only overrides, refused rather than clamped.
    std::uint64_t max_turns = meta.max_turns;
    if (opts.max_turns.has_value()) {
        if (*opts.max_turns > meta.max_turns) return std::unexpected(override_widens("max_turns"));
        max_turns = *opts.max_turns;
    }
    std::optional<std::uint64_t> token_budget = meta.token_budget;
    if (opts.token_budget.has_value()) {
        if (meta.token_budget.has_value() && *opts.token_budget > *meta.token_budget) {
            return std::unexpected(override_widens("token_budget"));
        }
        token_budget = opts.token_budget;
    }

    for (Capability const& c : opts.chat_client_grants) {
        if (capability_kind_of(c) != capability_kind::secret) {
            return std::unexpected(error{failure_class::policy,
                                         "chat_client_grants may only carry cap::Secret (the bound ChatClient's own "
                                         "credentials); every other authority goes through grants, narrowed to the "
                                         "agent's ceiling",
                                         "agent_session.chat_client_grant_not_secret"});
        }
    }

    // I2: the caller's authority, narrowed to the agent's declaration -- plus the ChatClient's own secrets.
    std::vector<Capability> held = narrow_to_ceiling(opts.grants, meta.capability_ceiling);
    held.insert(held.end(), opts.chat_client_grants.begin(), opts.chat_client_grants.end());
    auto capabilities = std::make_unique<CapabilitySet>(CapabilitySet::grant_root(std::move(held)));

    ChatClientCapabilities const client_caps = client.capabilities();

    auto session = std::make_unique<SessionT>();
    session->initialize(opts.session_id, opts.principal, token_budget, max_turns);
    session->emplace_chat_client(std::move(client));
    session->set_capabilities(capabilities.get());

    std::vector<ToolDescriptor> declared = declared_descriptors(meta);
    if (filter) filter(declared);
    if (auto bound = session->history_provider().bind(ToolTable::from_descriptors(std::move(declared)));
        !bound) {
        return std::unexpected(bound.error());
    }

    // The agent's instructions first, then the engine's own summary of what this session may do (the same
    // text every quickstart builder installs, OQ-16). Both host/engine-authored, never model output.
    std::string instructions = meta.agent_instructions;
    std::string const summary = trust::push_side_summary(*capabilities);
    if (!instructions.empty() && !summary.empty()) instructions += "\n\n";
    instructions += summary;
    session->set_static_instructions(std::move(instructions));

    if (meta.output_schema_json.has_value()) {
        session->set_output_schema(*meta.output_schema_json, select_output_schema_strategy(client_caps),
                                   std::move(validator));
    }

    quickstart::detail::install_deciders(*session, opts.approve_tools, opts.policy);

    // Recorded, not enforced -- ADR-226 §3 names why each has no session-level consumer.
    auto& md = session->metadata();
    md["agent.name"]           = meta.agent_name;
    md["agent.chat_client_id"] = meta.chat_client_id;
    md["agent.telemetry"]      = std::string(telemetry_name(meta.telemetry));
    md["agent.sandbox_profile"] = meta.sandbox_profile.is_strict ? "strict" : "declared_backend";
    if (meta.agent_version.has_value()) md["agent.version"] = *meta.agent_version;
    if (meta.stateless_pool_size.has_value()) {
        md["agent.stateless_pool_size"] = std::to_string(*meta.stateless_pool_size);
    }

    return BundleFactory::make<BundleT>(std::make_unique<InMemorySecretStore>(), std::move(capabilities),
                                        std::move(session));
}

// `OutputSchema<T>`'s typed validator, or an empty one when `A` declares no schema. Shared by
// `make_agent_session<A>()` and the harness's `make_harness_session<A>()` (ADR-234).
template <class A>
[[nodiscard]] OutputValidator output_validator_for() {
    using SchemaT = agent_output_schema_t<A>;
    if constexpr (std::is_void_v<SchemaT>) {
        return {};
    } else {
        return [](std::string_view text) -> result<void> {
            auto parsed = json::parse(text);
            if (!parsed) return std::unexpected(parsed.error());
            auto decoded = schema::from_json<SchemaT>(*parsed);
            if (!decoded) return std::unexpected(decoded.error());
            return {};
        };
    }
}

}  // namespace agent_bind_detail

// THE bridge (002 §2.1 as amended, I6): any compiled `AgentMetadata` -- from `register_agent<A>()` or from
// `compile_agent_document()` -- plus the caller's client and authority, to a running session. Validates
// the metadata with `validate_agent_metadata()` first, whichever surface produced it.
template <class ChatClientT>
    requires(ChatClient<ChatClientT> || ModelCallGatewayLike<ChatClientT>)
[[nodiscard]] result<AgentBundle<ChatClientT>> bind_agent_session(AgentMetadata const& meta, ChatClientT client,
                                                                  AgentSessionOptions opts = {},
                                                                  ChatClientRegistry const* registry = nullptr,
                                                                  SandboxBackendRegistry const* sandbox_registry =
                                                                      nullptr) {
    return agent_bind_detail::bind(meta, std::move(client), std::move(opts), {}, registry, sandbox_registry);
}

// The native-surface entry point: `register_agent<A>()` (every 002 §6 check, including the one only the C++
// type allows -- Stateless<N>), then the same bridge, plus `OutputSchema<T>`'s typed validator when `A`
// declares one.
template <class A, class ChatClientT>
    requires(ChatClient<ChatClientT> || ModelCallGatewayLike<ChatClientT>)
[[nodiscard]] result<AgentBundle<ChatClientT>> make_agent_session(ChatClientT client, AgentSessionOptions opts = {},
                                                                  ChatClientRegistry const* registry = nullptr,
                                                                  SandboxBackendRegistry const* sandbox_registry =
                                                                      nullptr) {
    result<AgentMetadata> meta = register_agent<A>(registry, sandbox_registry);
    if (!meta) return std::unexpected(meta.error());

    return agent_bind_detail::bind(*meta, std::move(client), std::move(opts),
                                   agent_bind_detail::output_validator_for<A>(), registry, sandbox_registry);
}

}  // namespace agentengine
