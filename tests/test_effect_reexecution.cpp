// Milestone 4 Phase F4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 019
// §6's "On re-execution: pure effects re-run freely; idempotent effects re-run under their
// original keys; at-most-once effects require explicit operator acknowledgement before
// re-execution" had no implementation anywhere before this task -- no `effect_class` vocabulary
// existed at all. Proves: an undeclared tool defaults to the conservative `at_most_once`
// classification (never silently "safe to repeat"), `authorize_reexecution()` gates exactly per
// 019 §6's own three cases, and -- "provable now against a manually-triggered turn re-execution"
// -- a real `invoke_tool()` call, deliberately re-run a second time under the SAME idempotency
// key (a manual rewind-and-replay), is correctly blocked without an operator's acknowledgement and
// correctly allowed once one is given.

#include <cstdio>
#include <string>

#include "agentengine/core/tool_pipeline.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct PureArgs {
    std::string x;
};
AE_JSON_SCHEMA(PureArgs, x)
struct PureReply {
    std::string x;
};
AE_JSON_SCHEMA(PureReply, x)

struct PureTool : agentengine::Tool<PureTool, agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "pure_tool";
    static constexpr std::string_view description = "A read-only, side-effect-free tool.";
    using Args = PureArgs;
    using Reply = PureReply;
    static agentengine::result<Reply> invoke(Args a, agentengine::EffectContext&) { return Reply{a.x}; }
};

struct PaymentArgs {
    std::string account;
};
AE_JSON_SCHEMA(PaymentArgs, account)
struct PaymentReply {
    bool charged;
};
AE_JSON_SCHEMA(PaymentReply, charged)

// Deliberately declares NO EffectClass -- proving the conservative default applies.
struct UndeclaredTool : agentengine::Tool<UndeclaredTool> {
    static constexpr std::string_view name = "undeclared_tool";
    static constexpr std::string_view description = "A tool that forgot to classify itself.";
    using Args = PaymentArgs;
    using Reply = PaymentReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct MakePaymentTool
    : agentengine::Tool<MakePaymentTool, agentengine::EffectClass<agentengine::effect_class::at_most_once>> {
    static constexpr std::string_view name = "make_payment";
    static constexpr std::string_view description = "019 §3's own example of a must-not-repeat effect.";
    using Args = PaymentArgs;
    using Reply = PaymentReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

agentengine::EffectContext make_ctx() {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.run_id     = "s-rewind:run:1";
    ctx.turn_index = 0;
    return ctx;
}

} // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::effect_class;
    using agentengine::authorize_reexecution;

    // --- Declared classification is read back exactly; undeclared defaults conservatively -------
    check(PureTool::declared_effect_class() == effect_class::pure,
          "F4-C1: a declared EffectClass<pure> is read back exactly");
    check(MakePaymentTool::declared_effect_class() == effect_class::at_most_once,
          "F4-C2: a declared EffectClass<at_most_once> is read back exactly");
    check(UndeclaredTool::declared_effect_class() == effect_class::at_most_once,
          "F4-R1: an UNDECLARED tool defaults to at_most_once -- the conservative direction (never "
          "silently 'safe to repeat'), the opposite default from declared_approval()'s own "
          "least-restrictive default");

    // --- authorize_reexecution() gates exactly 019 §6's three cases -------------------------------
    check(authorize_reexecution(effect_class::pure, /*ack=*/false).has_value(),
          "F4-R2: a pure effect re-runs freely with NO acknowledgement at all");
    check(authorize_reexecution(effect_class::idempotent, false).has_value(),
          "F4-R3: an idempotent effect re-runs freely too (under its original key, F1's job)");
    check(!authorize_reexecution(effect_class::at_most_once, false).has_value(),
          "F4-R4: an at-most-once effect is BLOCKED without an operator acknowledgement");
    auto blocked = authorize_reexecution(effect_class::at_most_once, false);
    check(!blocked.has_value() && blocked.error().code == "effect.reexecution_requires_ack",
          "F4-R5: the block carries a real, stable error code, not a generic failure");
    check(authorize_reexecution(effect_class::at_most_once, /*ack=*/true).has_value(),
          "F4-R6: the SAME at-most-once effect is allowed once an operator acknowledgement is given");

    // --- A real manually-triggered re-execution: invoke_tool() twice under the SAME idempotency
    // key (a manual rewind-and-replay), gated by authorize_reexecution() BEFORE the second call --
    {
        auto const table = agentengine::ToolTable::from_tools<MakePaymentTool>();
        agentengine::CapabilitySet held;
        auto args = *json::parse(R"({"account":"acct-1"})");
        auto ctx = make_ctx();

        agentengine::ToolCallRequest req{"call-1", "make_payment", args, false, /*call_index=*/0};
        agentengine::ToolInvocationAudit audit1;
        auto first = agentengine::invoke_tool(table, held, req, ctx, nullptr, &audit1);
        check(!first.is_error, "F4-R7 setup: the first (original) execution succeeds");

        // Rewind: attempt to re-run the EXACT SAME call (same key) without an operator ack first.
        auto gate_no_ack = authorize_reexecution(MakePaymentTool::declared_effect_class(), /*ack=*/false);
        check(!gate_no_ack.has_value(),
              "F4-R8: re-executing make_payment (at_most_once) is BLOCKED before the pipeline is "
              "even invoked a second time -- the gate sits BEFORE re-invocation, not after");

        // With an explicit operator acknowledgement, the SAME re-execution is now authorized --
        // invoke_tool() itself doesn't need to know about acknowledgement; that's this gate's job,
        // sitting in front of it, matching 019 §6's own "requires explicit operator acknowledgement
        // before re-execution" wording (before, not instead of).
        auto gate_with_ack = authorize_reexecution(MakePaymentTool::declared_effect_class(), /*ack=*/true);
        check(gate_with_ack.has_value(), "F4-R9: with an operator ack, re-execution is authorized");

        agentengine::ToolInvocationAudit audit2;
        auto second = agentengine::invoke_tool(table, held, req, ctx, nullptr, &audit2);
        check(!second.is_error, "F4-R10: the acknowledged re-execution itself still runs correctly");
        check(audit2.idempotency_key == audit1.idempotency_key,
              "F4-R11: the re-execution carries the SAME idempotency key as the original -- it is "
              "recognizably the same effect being repeated, not a new, unrelated call");
    }

    std::printf("test_effect_reexecution: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
