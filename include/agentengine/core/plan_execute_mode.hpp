#pragma once
// Implements decisions/ADR-167-plan-execute-mode.md, closing issue #54 ("No single-agent
// Plan/Execute mode -- only exists baked into the full Planner/Magentic workflow pattern, 014
// §3"). 014 §9 Q1 already rejected routing the dominant single-agent turn loop through
// supervising-actor/typed-edge machinery it cannot exercise -- the identical cost this issue's own
// body flags for routing single-agent plan/execute through 014's Planner pattern. This is the
// lighter mechanism 002 §3 (the policy table) has no entry for today.
//
// Two real, already-wired seams compose to give this real teeth, not a new inert `Agent<>` policy
// tag: `agent.hpp`'s own comment on `Concurrency`/`Memory`/`Middleware`/`Telemetry`/`Stateless` is
// explicit that those tags are "API surface only, no interpretation by register_agent<A>() or any
// pipeline yet" -- adding a `Mode<PlanExecute>` tag in that same inert style would be exactly that
// kind of scaffolding again, not a real behavior. Instead:
//   1. `ContextProvider` (005 §5, real, wired via `ComposedContextProvider`/`assemble_context()`) --
//      `PlanExecuteMode::on_context()` injects a "you must plan before you may execute" instruction
//      and a `plan_ready` tool while the gate is closed.
//   2. `PolicyDecider` (ADR-070, real, wired via `AgentSession::set_policy_decider()` /
//      `resolve_approval_outcome()`) -- `make_plan_execute_policy_decider()` returns a decider that
//      auto-denies any non-"planning-safe" `policy_driven` tool call until the gate opens.
// Both read the SAME shared, mutable gate state (`GateState`) and the SAME shared `TodoState` (via
// `TodoProvider::plan_state_handle()`, ADR-167's other half) -- see `PlanExecuteMode`'s own
// constructor comment for why a bare reference/pointer captured at construction would NOT survive
// `ComposedContextProvider` copying its `ProviderT` value into its own `shared_ptr`.
//
// I3 ("model output is data, never authority"): the phase transition is never inferred from what
// the model SAYS. It requires a real, structured, host-mediated action -- calling the `plan_ready`
// tool -- and that tool's own invoke closure independently re-checks `TodoProvider::has_planned()`
// (at least one real `todos_add` call already succeeded) before honoring it, so the model cannot
// talk its way into "execute" phase with zero actual planning by simply asserting it is done.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/todo_provider.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

struct PlanReadyArgs {
    std::optional<bool> ignored;  // json_schema.hpp's zero-real-argument idiom, matching TodoNoArgs
};
AE_JSON_SCHEMA(PlanReadyArgs, ignored)

struct PlanReadyReply {
    bool ok = false;
};
AE_JSON_SCHEMA(PlanReadyReply, ok)

// The one piece of mutable state the gate itself owns (as opposed to `TodoState`, which
// `TodoProvider` owns and this only reads). Shared the same way `TodoState` is: both
// `PlanExecuteMode` (living inside a session's `ComposedContextProvider`) and the standalone
// `PolicyDecider` closure `make_plan_execute_policy_decider()` returns (living in
// `AgentSession::policy_decider_`, a completely separate slot) hold their own `shared_ptr` to the
// SAME `GateState`, obtained from the SAME `PlanExecuteMode::gate_handle()` call before either was
// copied anywhere.
struct GateState {
    bool executing = false;
};

namespace plan_execute_detail {

// The default "safe to call before the plan gate opens" bar: no capability reach at all, and an
// effect class that's safe to attempt without having committed to a plan first. This is the same
// notion of "harmless enough" `tool_pipeline.hpp`'s `is_auto_declassifiable_text_derived_call` uses
// for a different question (declassifying text-derived calls) -- reused here as precedent for what
// "harmless" already means in this codebase, not by calling that function itself (its own semantics
// are specifically about call provenance, not about this gate). A host that wants planning-phase
// reconnaissance tools with real capability reach (e.g. a read-only search tool) can widen this via
// the `is_planning_safe` constructor parameter below -- this default is deliberately the
// conservative floor, not the only option.
[[nodiscard]] inline bool default_is_planning_safe(ToolDescriptor const& tool) noexcept {
    return tool.effect_class == agentengine::effect_class::pure && tool.capability_ceiling.empty();
}

}  // namespace plan_execute_detail

// A `ContextProvider` conformer (005 §5) that gates a session into "plan first, then execute."
// Requires a `TodoProvider` (#53) to already exist for this session -- the plan the gate demands
// evidence of IS the todo list; there is no separate planning representation invented here. Compose
// alongside `TodoProvider` in the same `ComposedContextProvider` tuple; use
// `make_plan_execute_policy_decider(mode.gate_handle())` to build the `PolicyDecider` that actually
// enforces the gate on tool calls (`AgentSession::set_policy_decider()`).
class PlanExecuteMode {
public:
    static constexpr std::string_view name = "plan_execute";  // ae-naming-lint: allow name — ADR-066 §3, same convention as "todo"/"memory"

    // `todo` must outlive this call, but NOT outlive `*this` -- only `todo.plan_state_handle()`'s
    // returned `shared_ptr` is retained, which keeps the underlying `TodoState` alive independently
    // of whichever `TodoProvider` value happens to hold a live reference to it at any given moment
    // (see todo_provider.hpp's own comment on `TodoState` for why that indirection exists).
    explicit PlanExecuteMode(TodoProvider const& todo) : plan_(todo.plan_state_handle()) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        c.tools.push_back(make_plan_ready_descriptor());
        if (!gate_->executing) {
            c.instructions = TaintedText{std::string(kGatingGuidance)};
        }
        co_return c;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    // A handle to the gate's own mutable state, for `make_plan_execute_policy_decider()` (usually
    // called once, right after constructing this, before this is copied into a session's own
    // provider composition).
    [[nodiscard]] std::shared_ptr<GateState const> gate_handle() const noexcept { return gate_; }

    // Read-only convenience for a host that wants to inspect gate state directly (tests, telemetry)
    // without going through the `PolicyDecider` machinery.
    [[nodiscard]] bool executing() const noexcept { return gate_->executing; }

private:
    static constexpr std::string_view kGatingGuidance =
        "You are in plan-first mode. Before you may call most tools, add at least one item to the "
        "todo list (todos_add) describing your plan, then call plan_ready to begin executing. "
        "Read-only, capability-free tools remain available while you plan.";

    [[nodiscard]] ToolDescriptor make_plan_ready_descriptor() {
        ToolDescriptor d;
        d.name                  = "plan_ready";
        d.description           = "Declare planning complete and begin executing. Requires at least "
                                   "one todo item to already exist.";
        d.approval               = approval_mode::never_require;
        d.args_schema_json      = schema::json_schema_of<PlanReadyArgs>();
        d.reply_schema_json     = schema::json_schema_of<PlanReadyReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<PlanReadyArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            // I3: re-checked here, not trusted from the fact the model chose to call this tool at
            // all -- a structured call is still not evidence of SUBSTANCE on its own. At least one
            // real todos_add must have already succeeded.
            if (!plan_->ever_used) {
                return std::unexpected(error{failure_class::contract,
                                              "no plan yet -- call todos_add at least once first",
                                              "plan_execute.no_plan"});
            }
            gate_->executing = true;
            return schema::to_json(PlanReadyReply{true});
        };
        return d;
    }

    std::shared_ptr<TodoState const> plan_;
    std::shared_ptr<GateState>       gate_ = std::make_shared<GateState>();
};

static_assert(ContextProvider<PlanExecuteMode>,
              "PlanExecuteMode must satisfy the ContextProvider concept (005 §5)");

// ADR-070's Delegated Decision Seam, reused verbatim: host-supplied, fails closed/safe when unset
// (an unset `inner` makes an already-executing gate behave EXACTLY as if no `PolicyDecider` had
// been wired at all -- `require_approval` falls through to the ordinary `ApprovalDecider` path,
// `resolve_approval_outcome`'s own documented behavior for that return value), narrows already-
// possessed authority only (never mints or widens a `Capability`; a call this denies was already
// `policy_driven` and reachable, this only makes it wait), host code decides `is_planning_safe`
// (never model output). `inner` lets a host compose this with its own broader policy -- this gate
// only ever narrows what `inner` would have allowed, never widens it (mirrors 002 §9 Q2's
// narrowing-only rule for per-run policy overrides, applied here to decider composition instead).
[[nodiscard]] inline PolicyDecider make_plan_execute_policy_decider(
    std::shared_ptr<GateState const> gate, PolicyDecider inner = {},
    std::function<bool(ToolDescriptor const&)> is_planning_safe =
        plan_execute_detail::default_is_planning_safe) {
    return [gate = std::move(gate), inner = std::move(inner),
            is_planning_safe = std::move(is_planning_safe)](
               Principal const& caller, ToolDescriptor const& tool,
               bool arguments_tainted) -> policy_decision {
        if (!gate->executing && !is_planning_safe(tool)) {
            return policy_decision::auto_deny;
        }
        if (inner) return inner(caller, tool, arguments_tainted);
        return policy_decision::require_approval;
    };
}

}  // namespace agentengine
