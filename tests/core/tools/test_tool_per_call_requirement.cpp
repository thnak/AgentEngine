// Gate test for decisions/archive/ADR-237-async-extension-points-and-io-reactor.md §9 D3 (#176), slice 1: a tool may
// declare a PER-CALL capability requirement -- `PerCallRequirement<Cs...>` plus `required_capabilities(Args const&)` --
// parsed once, checked against its declared bound, bound like a ceiling entry, and fail closed.
//
//   R1 a call whose requirement a held grant covers is admitted and runs; the arguments are parsed once (the
//      requirement and the body saw the same program), and the requirement was bound (a bound capability exists).
//   R2 a requirement no held grant covers is denied `tool.capability_not_held` and the body never runs.
//   R3 a requirement outside the declared bound (a program the tool's bound does not name) is refused
//      `tool.requirement_out_of_bounds` even though a held grant covers it; the body never runs.
//   R4 an EMPTY requirement is refused `tool.requirement_empty` -- never "nothing to bind, proceed".
//   R5 the descriptor's plain `invoke` refuses (`tool.requirement_unbound`) without the parse admission made: reaching
//      for it cannot skip binding. The parse is single use, and a parse stamped for another tool is refused.
//   R9 a wrapper that replaces `invoke` stays in the path: the pipeline calls `invoke`, never around it. A forwarding
//      wrapper sees the call and the body runs once; a stub that does not forward runs INSTEAD of the body.
//   R6 `background_task` refuses such a tool (`tool.requirement_not_backgroundable`); the control is the same tool
//      without the hook, which starts.
//   R7 the approver is shown the requirement beside the arguments (rule 5), and the call is refused when it says no.
//   R8 `check_capability_ceiling` refuses an agent whose ceiling holds no grant of the declared kind.
//
// Each refusal has its positive control on the same tool with a covered call, so a refusal that fired for some other
// reason would fail the control.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../support/run_task_sync.hpp"
#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/block_on.hpp"

namespace ae = agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

int         g_body_runs    = 0;
int         g_hook_calls   = 0;
std::string g_body_program;

struct RunArgs {
    std::string program;
};
AE_JSON_SCHEMA(RunArgs, program)
struct RunReply {
    bool ok = true;
};
AE_JSON_SCHEMA(RunReply, ok)

constexpr auto kU = ae::unbounded();

// The bound names `python*` on mount `wt`; the hook proposes the concrete program the call asked for.
struct RunProgramTool
    : ae::Tool<RunProgramTool, ae::Capabilities<>,
               ae::PerCallRequirement<ae::cap::decl::NativeExec<"python*", "wt", kU, kU, kU>>,
               ae::EffectClass<ae::effect_class::pure>, ae::Backgroundable,
               ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name        = "run_program";
    static constexpr std::string_view description = "runs a program the held NativeExec grant covers";
    using Args  = RunArgs;
    using Reply = RunReply;

    std::vector<ae::Capability> required_capabilities(Args const& a) const {
        ++g_hook_calls;
        if (a.program == "none") return {};
        return {ae::Capability{ae::cap::NativeExec{a.program, "wt", ae::cap::Limit::unbounded(),
                                                   ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded(), false,
                                                   ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded()}}};
    }
    ae::task<ae::result<Reply>> invoke(Args a, ae::EffectContext&) const {
        ++g_body_runs;
        g_body_program = a.program;
        co_return Reply{true};
    }
};

// The same tool WITHOUT the hook: the control for R6.
struct PlainTool : ae::Tool<PlainTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>, ae::Backgroundable,
                            ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name        = "run_program";
    static constexpr std::string_view description = "the same tool with no per-call requirement";
    using Args  = RunArgs;
    using Reply = RunReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext&) const { co_return Reply{true}; }
};

struct GatedRunTool
    : ae::Tool<GatedRunTool, ae::Capabilities<>,
               ae::PerCallRequirement<ae::cap::decl::NativeExec<"python*", "wt", kU, kU, kU>>,
               ae::EffectClass<ae::effect_class::pure>, ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name        = "run_program";
    static constexpr std::string_view description = "as RunProgramTool, but a human approves each call";
    using Args  = RunArgs;
    using Reply = RunReply;
    std::vector<ae::Capability> required_capabilities(Args const& a) const {
        return {ae::Capability{ae::cap::NativeExec{a.program, "wt", ae::cap::Limit::unbounded(),
                                                   ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded(), false,
                                                   ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded()}}};
    }
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext&) const {
        ++g_body_runs;
        co_return Reply{true};
    }
};

ae::Capability native_grant(std::string program) {
    return ae::Capability{ae::cap::NativeExec{std::move(program), "wt", ae::cap::Limit::unbounded(),
                                              ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded(), false,
                                              ae::cap::Limit::unbounded(), ae::cap::Limit::unbounded()}};
}

ae::ToolCallRequest call_of(std::string const& program, std::string id = "c1") {
    return ae::ToolCallRequest{std::move(id), "run_program", *ae::json::parse(R"({"program":")" + program + R"("})"), false};
}

ae::Principal const kCaller{"p-caller", "tenant-a"};

ae::EffectContext make_ctx() {
    ae::EffectContext ctx{.deadline = agentengine::Deadline::none()};
    ctx.principal = kCaller;
    return ctx;
}

void reset_counters() {
    g_body_runs  = 0;
    g_hook_calls = 0;
    g_body_program.clear();
}

}  // namespace

int main() {
    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({native_grant("python3"), native_grant("node")});
    ae::ToolTable const    table = ae::ToolTable::from(RunProgramTool{});

    check(RunProgramTool::kHasPerCallRequirement && !PlainTool::kHasPerCallRequirement,
          "R0: kHasPerCallRequirement is true exactly for the tool that declared the tag");
    ae::ToolDescriptor const d = ae::make_tool_descriptor(RunProgramTool{});
    check(static_cast<bool>(d.prepare) && d.requirement_bounds.size() == 1,
          "R0: the descriptor carries the prepare hook and the declared bound");
    check(!static_cast<bool>(ae::make_tool_descriptor(PlainTool{}).prepare),
          "R0 control: a tool without the tag has no prepare hook");

    // ---- R1: covered call -- admitted, parsed once, bound, runs -----------------------------------------------------
    {
        reset_counters();
        ae::EffectContext ctx = make_ctx();
        auto admitted = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(table, held, call_of("python3"), kCaller, nullptr, nullptr));
        check(admitted.has_value(), "R1: a requirement a held grant covers is admitted");
        check(g_hook_calls == 1, "R1: the hook ran once -- the arguments were parsed once");
        check(admitted.has_value() && admitted->bound.size() == 1, "R1: the requirement was bound");
        check(admitted.has_value() && admitted->prepared != nullptr, "R1: admission produced the prepared call");
        check(g_body_runs == 0, "R1: admission alone does not run the body");

        ae::ToolInvocationAudit audit;
        auto result = ae::test_support::run_task_sync<ae::ToolResult>(
            ae::invoke_tool(table, held, call_of("python3", "c2"), ctx, nullptr, &audit, nullptr));
        check(audit.ok, "R1: invoke_tool runs the call");
        check(g_body_runs == 1 && g_body_program == "python3",
              "R1: the body ran once on the program the requirement named");
        check(g_hook_calls == 2, "R1: one hook call per call -- no second parse for the body");
        (void)result;
    }

    // ---- R2: not held -------------------------------------------------------------------------------------------------
    {
        reset_counters();
        auto refused = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(table, held, call_of("python3.99"), kCaller, nullptr, nullptr));
        check(!refused.has_value() && refused.error().code == "tool.capability_not_held",
              "R2: a requirement no held grant covers is denied (tool.capability_not_held)");
        check(refused.has_value() == false && refused.error().message.find("python3.99") == std::string::npos,
              "R2: the refusal does not echo the model-supplied program");
        ae::EffectContext ctx = make_ctx();
        ae::ToolInvocationAudit audit;
        (void)ae::test_support::run_task_sync<ae::ToolResult>(
            ae::invoke_tool(table, held, call_of("python3.99"), ctx, nullptr, &audit, nullptr));
        check(!audit.ok && g_body_runs == 0, "R2: invoke_tool never runs the body");
    }

    // ---- R3: outside the declared bound -----------------------------------------------------------------------------
    {
        reset_counters();
        auto refused = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(table, held, call_of("node"), kCaller, nullptr, nullptr));
        check(!refused.has_value() && refused.error().code == "tool.requirement_out_of_bounds",
              "R3: a program outside the declared bound is refused (tool.requirement_out_of_bounds)");
        check(!refused.has_value() && refused.error().code != "tool.capability_not_held",
              "R3: and it was the bound that refused it -- `node` IS held, so the grant would have covered it");
        check(g_body_runs == 0, "R3: the body never ran");
    }

    // ---- R4: empty requirement fails closed -------------------------------------------------------------------------
    {
        reset_counters();
        auto refused = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(table, held, call_of("none"), kCaller, nullptr, nullptr));
        check(!refused.has_value() && refused.error().code == "tool.requirement_empty",
              "R4: an empty requirement is refused (tool.requirement_empty), not 'nothing to bind, proceed'");
        check(g_body_runs == 0, "R4: the body never ran");
    }

    // ---- R5: the plain invoke refuses -------------------------------------------------------------------------------
    {
        reset_counters();
        ae::EffectContext ctx = make_ctx();
        auto direct = ae::rt::block_on(d.invoke(*ae::json::parse(R"({"program":"python3"})"), ctx));
        check(!direct.has_value() && direct.error().code == "tool.requirement_unbound",
              "R5: descriptor.invoke refuses -- running it that way would skip binding the requirement");
        check(g_body_runs == 0, "R5: the body never ran");
    }

    // ---- R5b: the parse is single use and stamped ----------------------------------------------------------------
    {
        reset_counters();
        auto admitted = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(table, held, call_of("python3"), kCaller, nullptr, nullptr));
        ae::EffectContext ctx = make_ctx();
        ctx.prepared_arguments = admitted->prepared->arguments;
        auto first = ae::rt::block_on(d.invoke(*ae::json::parse(R"({"program":"ignored"})"), ctx));
        check(first.has_value() && g_body_runs == 1 && g_body_program == "python3",
              "R5b: with the admission's parse the body runs, on the parsed program and not the json it was handed");
        check(!ctx.prepared_arguments, "R5b: the parse was taken out of the context");
        auto second = ae::rt::block_on(d.invoke(*ae::json::parse(R"({"program":"python3"})"), ctx));
        check(!second.has_value() && second.error().code == "tool.requirement_unbound" && g_body_runs == 1,
              "R5b: a second invoke finds no parse and refuses (single use)");

        ctx.prepared_arguments = std::make_shared<ae::PreparedArguments const>(
            ae::PreparedArguments{"some_other_tool", admitted->prepared->arguments->type,
                                  admitted->prepared->arguments->arguments});
        auto foreign = ae::rt::block_on(d.invoke(*ae::json::parse(R"({"program":"python3"})"), ctx));
        check(!foreign.has_value() && foreign.error().code == "tool.requirement_unbound" && g_body_runs == 1,
              "R5b: a parse stamped for another tool is refused");
    }

    // ---- R9: a wrapper around invoke stays in the path ----------------------------------------------------------------
    {
        reset_counters();
        int wrapped = 0;
        ae::ToolDescriptor forwarding = d;
        forwarding.invoke = [inner = d.invoke, &wrapped](ae::json::Value const& args, ae::EffectContext& ctx) {
            ++wrapped;
            return inner(args, ctx);
        };
        ae::ToolTable const fwd_table = ae::ToolTable::from_descriptors({forwarding});
        ae::EffectContext ctx = make_ctx();
        ae::ToolInvocationAudit audit;
        (void)ae::test_support::run_task_sync<ae::ToolResult>(
            ae::invoke_tool(fwd_table, held, call_of("python3"), ctx, nullptr, &audit, nullptr));
        check(audit.ok && wrapped == 1 && g_body_runs == 1,
              "R9: a forwarding wrapper saw the call and the body ran once");

        reset_counters();
        wrapped = 0;
        ae::ToolDescriptor stub = d;
        stub.invoke = [&wrapped](ae::json::Value const&, ae::EffectContext&) -> ae::task<ae::result<ae::json::Value>> {
            ++wrapped;
            co_return ae::json::Value::make_object({});
        };
        ae::ToolTable const stub_table = ae::ToolTable::from_descriptors({stub});
        ae::ToolInvocationAudit stub_audit;
        (void)ae::test_support::run_task_sync<ae::ToolResult>(
            ae::invoke_tool(stub_table, held, call_of("python3", "c2"), ctx, nullptr, &stub_audit, nullptr));
        check(stub_audit.ok && wrapped == 1 && g_body_runs == 0,
              "R9: a stub that does not forward ran INSTEAD of the body -- a replay double cannot be bypassed");
    }

    // ---- R6: background ---------------------------------------------------------------------------------------------
    {
        reset_counters();
        ae::CapabilitySet const held_bg =
            ae::CapabilitySet::grant_root({ae::cap::Background{4}, native_grant("python3")});
        std::atomic<int> completed{0};
        auto refused = ae::test_support::run_task_sync<ae::result<void>>(ae::background_task(
            table, held_bg, call_of("python3"), make_ctx(), nullptr, 0,
            [&completed](ae::ToolResult, ae::ToolInvocationAudit) { completed.fetch_add(1); }));
        check(!refused.has_value() && refused.error().code == "tool.requirement_not_backgroundable",
              "R6: background_task refuses a tool with a per-call requirement");

        ae::ToolTable const plain = ae::ToolTable::from(PlainTool{});
        auto started = ae::test_support::run_task_sync<ae::result<void>>(ae::background_task(
            plain, held_bg, call_of("python3", "c2"), make_ctx(), nullptr, 0,
            [&completed](ae::ToolResult, ae::ToolInvocationAudit) { completed.fetch_add(1); }));
        check(started.has_value(), "R6 control: the same tool without the hook starts");
        for (int i = 0; i < 2000 && completed.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(completed.load() == 1, "R6 control: and completes (the refused one never did)");
    }

    // ---- R7: the approver sees the requirement ---------------------------------------------------------------------
    {
        reset_counters();
        ae::ToolTable const gated = ae::ToolTable::from(GatedRunTool{});
        std::string         seen;
        bool                answer = false;
        ae::ApprovalDecider approve = [&](ae::Principal const&, std::string_view, std::string const& text) -> ae::task<bool> {
            seen = text;
            co_return answer;
        };
        auto refused = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(gated, held, call_of("python3"), kCaller, approve, nullptr));
        check(!refused.has_value() && refused.error().code == "tool.approval_denied",
              "R7: a 'no' from the approver refuses the call");
        check(seen.find("\"arguments\"") != std::string::npos && seen.find("python3") != std::string::npos,
              "R7: the approver sees the arguments");
        check(seen.find("native_exec program=python3 mount=wt") != std::string::npos,
              "R7: and exactly what will be bound for them (rule 5)");

        answer = true;
        auto granted = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(gated, held, call_of("python3", "c2"), kCaller, approve, nullptr));
        check(granted.has_value(), "R7 control: the same call approved is admitted");
    }

    // ---- R8: registration checks the ceiling holds the declared kind ------------------------------------------------
    {
        std::vector<ae::ToolDescriptor> const tools{d};
        auto none = ae::agent_detail::check_capability_ceiling(ae::CapabilitySet::grant_root({}), tools);
        check(!none.has_value() && none.error().code == "agent.capability_ceiling_exceeded",
              "R8: a ceiling with no NativeExec grant is refused at registration");
        auto some = ae::agent_detail::check_capability_ceiling(ae::CapabilitySet::grant_root({native_grant("python3")}), tools);
        check(some.has_value(), "R8 control: a ceiling holding a NativeExec grant is accepted");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_tool_per_call_requirement: ALL PASS\n"
                                         : "test_tool_per_call_requirement: %d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
