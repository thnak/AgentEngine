// Positive control for tool_requirement_without_hook.cpp (decisions/archive/ADR-237-async-extension-points-and-io-reactor.md
// §9 D3): the same tool declaring the bound AND the hook compiles, and so does a tool declaring neither, so the negative
// fails for the pairing rule and not for some unrelated reason.

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

struct PairedTool : Tool<PairedTool, Capabilities<>, EffectClass<effect_class::pure>,
                         Approval<approval_mode::never_require>,
                         PerCallRequirement<cap::decl::NativeExec<"python*", "wt", unbounded(), unbounded(), unbounded()>>> {
    static constexpr std::string_view name        = "paired";
    static constexpr std::string_view description = "declares a bound and the hook";
    using Args  = ::Args;
    using Reply = ::Reply;
    std::vector<Capability> required_capabilities(Args const&) const { return {}; }
    task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};

struct NeitherTool : Tool<NeitherTool, Capabilities<>, EffectClass<effect_class::pure>, Approval<approval_mode::never_require>> {
    static constexpr std::string_view name        = "neither";
    static constexpr std::string_view description = "declares neither";
    using Args  = ::Args;
    using Reply = ::Reply;
    task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};

int main() {
    return make_tool_descriptor(PairedTool{}).prepare && !make_tool_descriptor(NeitherTool{}).prepare ? 0 : 1;
}
