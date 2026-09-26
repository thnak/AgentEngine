// Implements decisions/ADR-167-plan-execute-mode.md's proof obligations for `PlanExecuteMode`
// (core/plan_execute_mode.hpp), 002/014 gap #54. Mirrors test_todo_provider.cpp's structure: each
// block proves one claim from the ADR's per-claim verdict table.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/plan_execute_mode.hpp"
#include "agentengine/core/todo_provider.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

ae::Message make_msg(ae::role r, std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;

    ae::Message m{};
    m.role       = r;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    namespace json = ae::json;
    ae::Principal const principal{"p-plan", ""};
    std::vector<ae::Message> history{make_msg(ae::role::user, "ship the release", "m-1")};
    ae::EffectContext ctx{};
    ctx.principal = principal;
    ae::SessionContext session_ctx{"s-plan", principal, history};

    auto find_tool = [](ae::ContextContribution const& c, std::string_view name) {
        for (auto const& t : c.tools) {
            if (t.name == name) return &t;
        }
        return static_cast<ae::ToolDescriptor const*>(nullptr);
    };

    // R6-R8 need a gate opened FOR REAL (todos_add then plan_ready) -- gate_handle() is deliberately
    // read-only (shared_ptr<GateState const>), so there is no direct-mutation shortcut, by design.
    auto open_gate_for_real = [&](ae::TodoProvider& t, ae::PlanExecuteMode& m) {
        auto t_out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            t.on_context(session_ctx, ctx));
        auto const* add = find_tool(*t_out, "todos_add");
        auto add_args = json::parse(R"({"title":"plan it"})");
        auto add_result = add->invoke(*add_args, ctx);
        check(add_result.has_value(), "setup: todos_add succeeds");

        auto m_out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            m.on_context(session_ctx, ctx));
        auto const* ready = find_tool(*m_out, "plan_ready");
        auto no_args = json::parse(R"({})");
        auto ready_result = ready->invoke(*no_args, ctx);
        check(ready_result.has_value(), "setup: plan_ready succeeds");
    };

    // --- R1: before any planning, on_context() declares plan_ready and injects gating guidance -----
    ae::TodoProvider todo;
    ae::PlanExecuteMode mode{todo};
    auto gate = mode.gate_handle();
    check(gate != nullptr && !gate->executing, "setup: a fresh gate starts closed");

    auto out0 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
        mode.on_context(session_ctx, ctx));
    check(out0.has_value(), "R1: on_context() succeeds before any planning");
    auto const* ready_tool0 = find_tool(*out0, "plan_ready");
    check(ready_tool0 != nullptr, "R1: plan_ready is always declared");
    check(out0->instructions.has_value(),
          "R1: gating guidance is present while the gate is closed (unlike TodoProvider's adaptive "
          "suppression, this instruction must be visible BEFORE the model has planned, or the model "
          "never learns it needs to)");

    // --- R2: plan_ready before any todos_add fails closed, never silently "succeeds with nothing" --
    {
        auto no_args = json::parse(R"({})");
        check(no_args.has_value(), "setup: empty-object args parse");
        auto result = ready_tool0->invoke(*no_args, ctx);
        check(!result.has_value() && result.error().code == "plan_execute.no_plan",
              "R2 (I3): plan_ready refuses to open the gate with zero real planning evidence -- the "
              "structured call alone is not trusted as substance, only a prior real todos_add is");
        check(!gate->executing, "R2: the gate is still closed after a rejected plan_ready");
    }

    // --- R3: real planning (todos_add) then plan_ready opens the gate -------------------------------
    {
        auto todo_out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            todo.on_context(session_ctx, ctx));
        check(todo_out.has_value(), "setup: TodoProvider on_context succeeds");
        auto const* add_tool = find_tool(*todo_out, "todos_add");
        check(add_tool != nullptr, "setup: todos_add found");
        auto add_args = json::parse(R"({"title":"cut release notes"})");
        auto add_result = add_tool->invoke(*add_args, ctx);
        check(add_result.has_value(), "setup: todos_add succeeds");

        auto no_args = json::parse(R"({})");
        auto ready_result = ready_tool0->invoke(*no_args, ctx);
        check(ready_result.has_value(), "R3: plan_ready succeeds once a real todo exists");
        check(gate->executing, "R3: the gate is now open");
    }

    // --- R4: once executing, on_context() stops injecting the gating instruction --------------------
    {
        auto out1 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            mode.on_context(session_ctx, ctx));
        check(out1.has_value() && !out1->instructions.has_value(),
              "R4: gating guidance disappears once the gate is open -- no reason to keep nagging an "
              "agent that already passed the gate");
        check(find_tool(*out1, "plan_ready") != nullptr,
              "R4: plan_ready stays declared (idempotent re-call is harmless, not an error)");
    }

    // --- R5: the shared-state refactor's whole point -- a COPY of both providers (simulating
    // ComposedContextProvider's own internal copy-into-shared_ptr) still shares live state with the
    // ORIGINAL objects a host builds the PolicyDecider from ------------------------------------------
    {
        ae::TodoProvider fresh_todo;
        ae::PlanExecuteMode fresh_mode{fresh_todo};
        auto fresh_gate = fresh_mode.gate_handle();  // captured BEFORE either is copied anywhere

        // Simulate ComposedContextProvider: copy both provider VALUES into a fresh scope, exactly
        // the way make_context_provider_descriptor() copies its ProviderT argument into its own
        // shared_ptr<ProviderT>.
        ae::TodoProvider copied_todo = fresh_todo;
        ae::PlanExecuteMode copied_mode = fresh_mode;

        auto todo_ctx = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            copied_todo.on_context(session_ctx, ctx));
        auto const* copied_add = find_tool(*todo_ctx, "todos_add");
        check(copied_add != nullptr, "setup: copied TodoProvider's todos_add found");
        auto add_args = json::parse(R"({"title":"ship it"})");
        check(copied_add->invoke(*add_args, ctx).has_value(),
              "setup: todos_add on the COPY succeeds");

        auto plan_ctx = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            copied_mode.on_context(session_ctx, ctx));
        auto const* copied_ready = find_tool(*plan_ctx, "plan_ready");
        check(copied_ready != nullptr, "setup: copied PlanExecuteMode's plan_ready found");
        auto no_args = json::parse(R"({})");
        auto ready_result = copied_ready->invoke(*no_args, ctx);
        check(ready_result.has_value(),
              "R5a: plan_ready on the COPY sees the todo the COPY's own todos_add just added -- the "
              "copy shares TodoState with its own copy, not a stale snapshot");
        check(fresh_gate->executing,
              "R5b (the central claim): the handle captured from the ORIGINAL, pre-copy PlanExecuteMode "
              "observes the gate the COPY opened -- proving GateState survives "
              "ComposedContextProvider-style copying, which a bare captured reference/pointer would not");
    }

    // --- R6: the PolicyDecider itself -- auto_deny for non-planning-safe tools while closed ---------
    {
        ae::ToolDescriptor execute_tool;
        execute_tool.name          = "delete_file";
        execute_tool.approval       = ae::approval_mode::policy_driven;
        execute_tool.effect_class   = ae::effect_class::at_most_once;  // not pure -> not planning-safe

        ae::ToolDescriptor readonly_tool;
        readonly_tool.name         = "read_notes";
        readonly_tool.approval      = ae::approval_mode::policy_driven;
        readonly_tool.effect_class  = ae::effect_class::pure;  // pure + empty capability_ceiling

        ae::TodoProvider todo2;
        ae::PlanExecuteMode mode2{todo2};
        auto gate2 = mode2.gate_handle();
        check(!gate2->executing, "setup: gate2 starts closed");

        auto decider = ae::make_plan_execute_policy_decider(gate2);
        check(decider(principal, execute_tool, false) == ae::policy_decision::auto_deny,
              "R6a: a non-planning-safe, policy_driven tool is auto-denied while the gate is closed");
        check(decider(principal, readonly_tool, false) == ae::policy_decision::require_approval,
              "R6b: a planning-safe tool falls through to require_approval (== 'no opinion, use the "
              "normal path') while the gate is closed, never auto_deny -- read-only reconnaissance "
              "stays available during planning");

        open_gate_for_real(todo2, mode2);
        check(gate2->executing, "setup: gate2 opened for real via todos_add + plan_ready");
        check(decider(principal, execute_tool, false) == ae::policy_decision::require_approval,
              "R7: once the gate opens, the SAME tool that was auto-denied now falls through to "
              "require_approval (== behaves as if no PolicyDecider were wired at all) -- the gate "
              "never grants anything on its own, it only ever withholds");
    }

    // --- R8: composition with an inner PolicyDecider narrows, never widens, what inner would allow -
    {
        ae::ToolDescriptor execute_tool;
        execute_tool.name        = "delete_file";
        execute_tool.approval     = ae::approval_mode::policy_driven;
        execute_tool.effect_class = ae::effect_class::at_most_once;

        ae::TodoProvider todo3;
        ae::PlanExecuteMode mode3{todo3};
        auto gate3 = mode3.gate_handle();

        ae::PolicyDecider always_approve = [](ae::Principal const&, ae::ToolDescriptor const&,
                                               bool) { return ae::policy_decision::auto_approve; };
        auto decider = ae::make_plan_execute_policy_decider(gate3, always_approve);

        check(decider(principal, execute_tool, false) == ae::policy_decision::auto_deny,
              "R8a: the gate overrides even an inner policy that would auto_approve -- composition "
              "only ever narrows toward less authority, never widens toward more (002 §9 Q2's "
              "narrowing-only rule, applied to decider composition)");

        open_gate_for_real(todo3, mode3);
        check(gate3->executing, "setup: gate3 opened for real via todos_add + plan_ready");
        check(decider(principal, execute_tool, false) == ae::policy_decision::auto_approve,
              "R8b: once the gate opens, control genuinely passes through to inner -- composition "
              "isn't a permanent override, only a precondition");
    }

    // --- R9: a host-supplied is_planning_safe widens what's callable during planning, without
    // touching what the gate can ever GRANT (still narrowing-only, just a different floor) ----------
    {
        ae::ToolDescriptor search_tool;
        search_tool.name        = "web_search";
        search_tool.approval     = ae::approval_mode::policy_driven;
        search_tool.effect_class = ae::effect_class::idempotent;  // not pure by the DEFAULT bar

        ae::TodoProvider todo4;
        ae::PlanExecuteMode mode4{todo4};
        auto gate4 = mode4.gate_handle();

        auto default_decider = ae::make_plan_execute_policy_decider(gate4);
        check(default_decider(principal, search_tool, false) == ae::policy_decision::auto_deny,
              "R9a: the default is_planning_safe bar denies an idempotent (non-pure) tool while "
              "closed");

        auto widened_decider = ae::make_plan_execute_policy_decider(
            gate4, {}, [](ae::ToolDescriptor const& t) { return t.name == "web_search"; });
        check(widened_decider(principal, search_tool, false) == ae::policy_decision::require_approval,
              "R9b: a host-supplied is_planning_safe predicate can widen the planning-phase floor for "
              "a specific tool without waiting for the gate to open");
    }

    // --- R10 (independent review finding): count_gated_execute_tools() correctly flags whether a
    // host's own ToolTable has ANY tool the gate can actually reach -- a `never_require` tool is
    // structurally invisible to any PolicyDecider (resolve_approval_outcome() never consults one for
    // it), so a table built entirely of such tools leaves the gate with zero real protective effect
    // even though make_plan_execute_policy_decider() itself is correct.
    {
        ae::ToolDescriptor ungated_execute_tool;
        ungated_execute_tool.name          = "delete_file";
        ungated_execute_tool.approval       = ae::approval_mode::never_require;  // the coverage trap
        ungated_execute_tool.effect_class   = ae::effect_class::at_most_once;

        ae::ToolDescriptor gated_execute_tool;
        gated_execute_tool.name          = "wipe_disk";
        gated_execute_tool.approval       = ae::approval_mode::policy_driven;
        gated_execute_tool.effect_class   = ae::effect_class::at_most_once;

        ae::ToolDescriptor planning_safe_tool;
        planning_safe_tool.name          = "read_notes";
        planning_safe_tool.approval       = ae::approval_mode::policy_driven;
        planning_safe_tool.effect_class   = ae::effect_class::pure;  // planning-safe, not counted

        auto table_with_no_coverage = ae::ToolTable::from_descriptors({ungated_execute_tool, planning_safe_tool});
        check(ae::count_gated_execute_tools(table_with_no_coverage) == 0,
              "R10a: a table with only never_require/planning-safe tools reports zero real gate "
              "coverage -- the diagnostic catches the exact silent-no-protection mistake the "
              "independent review found");

        auto table_with_coverage =
            ae::ToolTable::from_descriptors({ungated_execute_tool, gated_execute_tool, planning_safe_tool});
        check(ae::count_gated_execute_tools(table_with_coverage) == 1,
              "R10b: adding one properly-marked policy_driven, non-planning-safe tool makes the "
              "diagnostic report real coverage, counting only that tool -- not the never_require one, "
              "not the planning-safe one");
    }

    std::printf("test_plan_execute_mode: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
