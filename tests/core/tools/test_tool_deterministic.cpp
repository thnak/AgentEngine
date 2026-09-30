// Implements decisions/ADR-212-deterministic-tool-output-declaration.md (GitHub issue #81): the
// `Deterministic` tag, `Tool<>::declared_deterministic()`, `ToolDescriptor::deterministic` from both
// factories, `rerun_comparable()`, and the declarative path inheriting the flag by name (§3.4). The
// §3.3 static_assert is proven separately by the compile-fail gates in tests/compile_fail/; this
// binary is their positive control (it compiles `Deterministic` with `pure`, in either tag order).

#include <cstdio>
#include <string>

#include "agentengine/core/json_schema_validator.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/tool_registry.hpp"

namespace {

namespace ae = agentengine;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct SumArgs { int a = 0; int b = 0; };
AE_JSON_SCHEMA(SumArgs, a, b)
struct SumReply { int sum = 0; std::string note; };
AE_JSON_SCHEMA(SumReply, sum, note)

// Deterministic + pure: the canonical case.
struct SumTool : ae::Tool<SumTool, ae::EffectClass<ae::effect_class::pure>, ae::Deterministic> {
    static constexpr std::string_view name = "sum";
    static constexpr std::string_view description = "Adds two integers.";
    using Args = SumArgs;
    using Reply = SumReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.a + a.b, "sum"}; }
};

// Same claim, tag order reversed, plus a later EffectClass overriding an earlier one: the fold must
// not depend on position, and last-declared wins exactly as `declared_effect_class()` does.
struct EchoTool : ae::Tool<EchoTool, ae::Deterministic, ae::EffectClass<ae::effect_class::at_most_once>,
                           ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Returns its first argument.";
    using Args = SumArgs;
    using Reply = SumReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.a, "echo"}; }
};

// Pure but NOT deterministic: safe to repeat, different answer each time. The case issue #81 names.
int g_counter = 0;
struct CounterTool : ae::Tool<CounterTool, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "counter";
    static constexpr std::string_view description = "Returns a fresh number on every call.";
    using Args = SumArgs;
    using Reply = SumReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{++g_counter, "counter"}; }
};

// No policies at all -- the empty-pack instantiation the MSVC C3520 note in tool.hpp is about.
struct BareTool : ae::Tool<BareTool> {
    static constexpr std::string_view name = "bare";
    static constexpr std::string_view description = "Declares nothing.";
    using Args = SumArgs;
    using Reply = SumReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{}; }
};

ae::EffectContext make_ctx() {
    ae::EffectContext ctx;
    ctx.principal = ae::Principal{"test-principal", ""};
    ctx.run_id = "adr-212:run:1";
    return ctx;
}

// ADR-212 §3.2's comparison, as a host would run it: two successful replies, json_value_equal.
bool rerun_matches(ae::ToolDescriptor const& d, ae::json::Value const& args) {
    auto ctx1 = make_ctx();
    auto ctx2 = make_ctx();
    auto first = d.invoke(args, ctx1);
    auto second = d.invoke(args, ctx2);
    return first && second && ae::schema::json_value_equal(*first, *second);
}

}  // namespace

int main() {
    // D1: the compile-time accessor.
    static_assert(SumTool::declared_deterministic());
    static_assert(EchoTool::declared_deterministic());
    static_assert(!CounterTool::declared_deterministic());
    static_assert(!BareTool::declared_deterministic());
    static_assert(BareTool::kEffectClass == ae::effect_class::at_most_once);
    static_assert(EchoTool::kEffectClass == ae::effect_class::pure);

    // D2: make_tool_descriptor copies it; undeclared stays false.
    auto sum = ae::make_tool_descriptor<SumTool>();
    auto echo = ae::make_tool_descriptor<EchoTool>();
    auto counter = ae::make_tool_descriptor<CounterTool>();
    auto bare = ae::make_tool_descriptor<BareTool>();
    check(sum.deterministic, "D2: make_tool_descriptor copies Deterministic");
    check(echo.deterministic && echo.effect_class == ae::effect_class::pure,
          "D2: tag order and a later EffectClass do not change the result");
    check(!counter.deterministic, "D2: an undeclared tool is not deterministic");
    check(!bare.deterministic, "D2: an empty-policy tool is not deterministic");
    auto with_invoke = ae::make_tool_descriptor_with_invoke<SumTool>(
        [](SumArgs a, ae::EffectContext&) -> ae::result<SumReply> { return SumReply{a.a + a.b, "w"}; });
    // ...but not through the session-state factory: its closure is not the code the tag describes.
    check(!with_invoke.deterministic && with_invoke.captures_session_state,
          "D2: make_tool_descriptor_with_invoke drops Deterministic");

    // D3: a hand-built descriptor defaults to false (MCP bridge, WASM, providers -- ADR-212 §3.4).
    ae::ToolDescriptor hand_built;
    check(!hand_built.deterministic, "D3: a default-constructed descriptor is not deterministic");

    // D4: rerun_comparable() -- including the hand-built combination Tool<> would have rejected.
    check(ae::rerun_comparable(sum), "D4: deterministic + pure is comparable");
    check(ae::rerun_comparable(echo), "D4: echo is comparable");
    ae::ToolDescriptor idem = sum;
    idem.effect_class = ae::effect_class::idempotent;
    check(!ae::rerun_comparable(idem), "D4: deterministic + idempotent is not comparable");
    ae::ToolDescriptor stateful = sum;
    stateful.captures_session_state = true;
    check(!ae::rerun_comparable(stateful), "D4: a session-state-capturing tool is not comparable");
    check(!ae::rerun_comparable(counter), "D4: pure but not deterministic is not comparable");
    ae::ToolDescriptor forged = sum;
    forged.effect_class = ae::effect_class::at_most_once;
    check(!ae::rerun_comparable(forged), "D4: deterministic + at_most_once is never comparable");
    hand_built.effect_class = ae::effect_class::pure;
    check(!ae::rerun_comparable(hand_built), "D4: pure without the claim is not comparable");

    // D5: the declarative path (015) resolves tools by name, so it carries whatever the registered
    // descriptor declared -- no separate YAML field that could disagree with the C++ declaration.
    {
        ae::ToolRegistry registry;
        check(registry.register_tool("sum", sum, ae::tool_provenance::native).has_value(),
              "D5: register sum");
        check(registry.register_tool("counter", counter, ae::tool_provenance::native).has_value(),
              "D5: register counter");
        auto table = ae::ToolTable::from_names({"sum", "counter"}, registry);
        check(table.has_value(), "D5: from_names resolves both");
        if (table) {
            auto const* s = table->find("sum");
            auto const* c = table->find("counter");
            check(s && s->deterministic, "D5: sum stays deterministic through from_names");
            check(c && !c->deterministic, "D5: counter stays non-deterministic through from_names");
        }
    }

    // D6: the comparison the claim licenses. Positive control: a pure but non-deterministic tool must
    // MISMATCH under the same procedure, or the procedure proves nothing about the deterministic one.
    auto args = ae::schema::to_json(SumArgs{2, 3});
    check(rerun_matches(sum, args), "D6: a deterministic tool re-runs to an equal reply");
    check(!rerun_matches(counter, args), "D6 control: a non-deterministic pure tool mismatches");

    if (g_failures == 0) std::puts("test_tool_deterministic: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
