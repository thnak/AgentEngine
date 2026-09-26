// Milestone 4 Phase F1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 019
// §3's "Every effect carries an idempotency key derived deterministically from {run_id,
// turn_index, call_index, argument_digest}" had no implementation anywhere before this task.
// Proves: the SAME four inputs always yield the SAME key (the "deterministic derivation is what
// makes the key survive a restart" claim, proven by construction: recomputing from identical
// inputs after a simulated "restart" — a fresh EffectContext/request built from scratch — gives an
// identical key, not merely a comment asserting it), and changing ANY ONE of the four inputs
// changes the key (so two genuinely different calls never collide by construction). Also proves
// `invoke_tool()` (M2's real pipeline) actually computes and surfaces the key through
// `ToolInvocationAudit`, not just that a standalone function can compute one in isolation.

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

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)

struct EchoReply {
    std::string echoed;
};
AE_JSON_SCHEMA(EchoReply, echoed)

struct EchoTool : agentengine::Tool<EchoTool> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo the input message back.";
    using Args = EchoArgs;
    using Reply = EchoReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{"echo: " + args.message};
    }
};

agentengine::EffectContext make_ctx(std::string run_id, std::uint64_t turn_index) {
    agentengine::EffectContext ctx;
    ctx.principal   = agentengine::Principal{"test-principal", ""};
    ctx.run_id      = std::move(run_id);
    ctx.turn_index  = turn_index;
    return ctx;
}

} // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::derive_idempotency_key;
    using agentengine::IdempotencyKey;

    // --- F1-C1: determinism -- the SAME 4 inputs always yield the SAME key, "restart" or not ----
    auto ctx1 = make_ctx("s-1:run:1", 0);
    auto args1 = *json::parse(R"({"message":"hi"})");
    IdempotencyKey k1 = derive_idempotency_key(ctx1, /*call_index=*/0, args1);

    // Simulate "after a restart": a completely fresh EffectContext/args built from scratch, not the
    // same objects reused.
    auto ctx1_restarted = make_ctx("s-1:run:1", 0);
    auto args1_restarted = *json::parse(R"({"message":"hi"})");
    IdempotencyKey k1_restarted = derive_idempotency_key(ctx1_restarted, 0, args1_restarted);
    check(k1 == k1_restarted, "F1-C1: identical {run_id,turn_index,call_index,args} yields an identical key");

    // --- F1-R1: changing run_id alone changes the key -------------------------------------------
    auto ctx2 = make_ctx("s-2:run:1", 0);
    IdempotencyKey k2 = derive_idempotency_key(ctx2, 0, args1);
    check(!(k1 == k2), "F1-R1: a different run_id produces a different key");

    // --- F1-R2: changing turn_index alone changes the key -----------------------------------------
    auto ctx3 = make_ctx("s-1:run:1", 1);
    IdempotencyKey k3 = derive_idempotency_key(ctx3, 0, args1);
    check(!(k1 == k3), "F1-R2: a different turn_index produces a different key");

    // --- F1-R3: changing call_index alone changes the key -----------------------------------------
    IdempotencyKey k4 = derive_idempotency_key(ctx1, 1, args1);
    check(!(k1 == k4), "F1-R3: a different call_index produces a different key (two calls in the "
                       "same turn never collide)");

    // --- F1-R4: changing the arguments alone changes the key --------------------------------------
    auto args2 = *json::parse(R"({"message":"bye"})");
    IdempotencyKey k5 = derive_idempotency_key(ctx1, 0, args2);
    check(!(k1 == k5), "F1-R4: different arguments produce a different key (the digest is over the "
                       "actual call content, not a placeholder)");

    // --- F1-R5: to_string() is a real, distinct dedup key per input, usable as a journal key ------
    check(k1.to_string() != k2.to_string() && k1.to_string() != k3.to_string() &&
              k1.to_string() != k4.to_string() && k1.to_string() != k5.to_string(),
          "F1-R5: to_string() preserves the distinctness proven above -- safe as a flat dedup key");

    // --- F1-R6: invoke_tool() (M2's real pipeline) actually computes and surfaces the key --------
    {
        auto const table = agentengine::ToolTable::from_tools<EchoTool>();
        agentengine::CapabilitySet held;
        agentengine::ToolCallRequest req{"call-1", "echo", args1, false, /*call_index=*/7};
        agentengine::ToolInvocationAudit audit;
        auto ctx = make_ctx("s-pipeline:run:3", 2);
        auto result = agentengine::invoke_tool(table, held, req, ctx, nullptr, &audit);
        check(result.is_error == false, "F1-R6 setup: the pipeline call itself succeeds");
        IdempotencyKey expected = derive_idempotency_key(ctx, 7, args1);
        check(audit.idempotency_key == expected,
              "F1-R6: invoke_tool()'s own audit carries the SAME key derive_idempotency_key() "
              "would compute from the same {ctx, call_index, arguments} -- the pipeline is "
              "actually wired to F1, not a parallel, unused function");
    }

    std::printf("test_idempotency_key: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
