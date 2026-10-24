// This file MUST NOT compile (decisions/archive/ADR-237-async-extension-points-and-io-reactor.md §9 D3). A tool that
// declares PerCallRequirement<...> but provides no required_capabilities(Args const&) would be bound to a bound it never
// narrows; a tool that provides the hook without the declared bound would propose requirements nothing checks.
// Declaring one without the other is refused when the descriptor is made.

#include "agentengine/core/tool_descriptor.hpp"

using namespace agentengine;

struct Args {
    int x = 0;
};
AE_JSON_SCHEMA(Args, x)
struct Reply {
    bool ok = true;
};
AE_JSON_SCHEMA(Reply, ok)

struct BoundOnlyTool : Tool<BoundOnlyTool, Capabilities<>, EffectClass<effect_class::pure>,
                            Approval<approval_mode::never_require>,
                            PerCallRequirement<cap::decl::NativeExec<"python*", "wt", unbounded(), unbounded(), unbounded()>>> {
    static constexpr std::string_view name        = "bound_only";
    static constexpr std::string_view description = "declares a bound and no hook";
    using Args  = ::Args;
    using Reply = ::Reply;
    task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};

int main() { return make_tool_descriptor(BoundOnlyTool{}).name.empty() ? 1 : 0; }
