#pragma once
// ADR-237 section 9 D3 (#176): a tool that declares a per-call requirement runs only with what admission produced -- the
// bound capability and the parsed arguments -- and refuses (`tool.requirement_unbound`) when its closure is called
// directly. `invoke_admitted` does what the pipeline does around `invoke`: admission (bind + approve), the call, then the
// revoke, and returns the raw reply JSON so a test can read it against the declared schema.

#include <string_view>
#include <utility>

#include "run_task_sync.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace ae_test {

[[nodiscard]] inline agentengine::result<agentengine::json::Value> invoke_admitted(
    agentengine::ToolDescriptor const& tool, agentengine::json::Value const& args, agentengine::EffectContext& ctx) {
    using namespace agentengine;
    CapabilitySet const held = ctx.capabilities ? *ctx.capabilities : CapabilitySet{};
    ToolTable const table = ToolTable::from_descriptors({tool});
    ToolCallRequest const request{"c1", tool.name, args, false};
    ApprovalDecider const approve = [](Principal const&, std::string_view, std::string const&) -> task<bool> {
        co_return true;
    };
    auto admitted = test_support::run_task_sync<result<AdmittedCall>>(
        admit_call(table, held, request, ctx.principal, approve, nullptr));
    if (!admitted.has_value()) return std::unexpected(admitted.error());
    ctx.bound_capabilities = &admitted->bound;
    ctx.prepared_arguments = admitted->prepared ? admitted->prepared->arguments : nullptr;
    auto reply = test_support::run_task_sync<result<json::Value>>(tool.invoke(args, ctx));
    ctx.bound_capabilities = nullptr;
    ctx.prepared_arguments.reset();
    for (auto const& b : admitted->bound) b.revoke();
    return reply;
}

}  // namespace ae_test
