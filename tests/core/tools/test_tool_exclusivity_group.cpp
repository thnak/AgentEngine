// Implements decisions/ADR-158-tool-concurrency-exclusivity-policy.md §4: `ExclusivityGroup<Name>`
// is a declared policy tag with zero pipeline logic yet (matching `Parallelizable`'s own existing
// state -- 006 §8 G4's parallel-batch scheduler is still deferred). This proves the declaration
// surface itself: `Tool<Derived, Policies...>::declared_exclusivity_group()` and
// `ToolDescriptor::exclusivity_group` (`make_tool_descriptor<ToolT>()`), and the two compile-time
// static_asserts MUST-FIX 1/§5 added (never both `Parallelizable` and `ExclusivityGroup<Name>`; at
// most one `ExclusivityGroup<Name>`) -- those two are proven separately, as compile-fail gates,
// under tests/compile_fail/ (see tests/CMakeLists.txt), since a real compile failure cannot be
// asserted from inside a test binary that must itself compile.

#include <cstdio>
#include <optional>
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

struct NoopArgs { bool ignored = false; };
AE_JSON_SCHEMA(NoopArgs, ignored)
struct NoopReply { bool ok; };
AE_JSON_SCHEMA(NoopReply, ok)

// -- Baseline: declares neither Parallelizable nor ExclusivityGroup<...> ------------------------
struct UndeclaredTool : agentengine::Tool<UndeclaredTool> {
    static constexpr std::string_view name = "undeclared";
    static constexpr std::string_view description = "Declares no concurrency policy at all.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

// -- Bare Parallelizable, unchanged from before this ADR -- must NOT populate exclusivity_group --
struct ParallelizableOnlyTool : agentengine::Tool<ParallelizableOnlyTool, agentengine::Parallelizable> {
    static constexpr std::string_view name = "parallelizable_only";
    static constexpr std::string_view description = "Declares bare Parallelizable, no group.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

// -- Two INDEPENDENT tool types sharing one named group -- the real point of a fixed_string NTTP
// over a type-tag: unrelated declaration sites correlate by NAME, not by C++ type identity. --------
struct DbWriteTool
    : agentengine::Tool<DbWriteTool, agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "db_write";
    static constexpr std::string_view description = "Writes a row; must not race another db-write.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};
struct DbCompactTool
    : agentengine::Tool<DbCompactTool, agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "db_compact";
    static constexpr std::string_view description = "Compacts storage; same exclusivity as db_write.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

// -- A distinct group name must not collide with "db-write" above -------------------------------
struct CacheFlushTool
    : agentengine::Tool<CacheFlushTool, agentengine::ExclusivityGroup<"cache-flush">> {
    static constexpr std::string_view name = "cache_flush";
    static constexpr std::string_view description = "Flushes a cache; its own, separate group.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

}  // namespace

int main() {
    // -- declared_exclusivity_group(): the compile-time accessor -----------------------------------
    check(!UndeclaredTool::declared_exclusivity_group().has_value(),
          "a tool declaring no concurrency policy has no exclusivity group");
    check(!ParallelizableOnlyTool::declared_exclusivity_group().has_value(),
          "ADR-158 MUST-FIX 1: bare Parallelizable must NOT populate exclusivity_group -- the two "
          "are distinct, mutually exclusive claims, not layered on top of each other");
    check(DbWriteTool::declared_exclusivity_group() == std::optional<std::string>{"db-write"},
          "a tool declaring ExclusivityGroup<\"db-write\"> reports that exact group name");
    check(DbCompactTool::declared_exclusivity_group() == std::optional<std::string>{"db-write"},
          "a second, independently-declared tool in the SAME named group reports the identical "
          "group name -- correlation by name (fixed_string NTTP), not by C++ type identity");
    check(CacheFlushTool::declared_exclusivity_group() == std::optional<std::string>{"cache-flush"},
          "a different group name is reported distinctly, no collision with \"db-write\"");

    // -- ToolDescriptor::exclusivity_group: the runtime, type-erased surface make_tool_descriptor<>()
    // populates -- what a (future) batch-formation step would actually read. ----------------------
    auto const undeclared_d = agentengine::make_tool_descriptor<UndeclaredTool>();
    check(!undeclared_d.exclusivity_group.has_value(),
          "ToolDescriptor for an undeclared tool carries no exclusivity_group");

    auto const parallel_d = agentengine::make_tool_descriptor<ParallelizableOnlyTool>();
    check(!parallel_d.exclusivity_group.has_value(),
          "ToolDescriptor for a bare-Parallelizable tool carries no exclusivity_group either");

    auto const db_write_d = agentengine::make_tool_descriptor<DbWriteTool>();
    auto const db_compact_d = agentengine::make_tool_descriptor<DbCompactTool>();
    check(db_write_d.exclusivity_group.has_value() && *db_write_d.exclusivity_group == "db-write",
          "db_write's real ToolDescriptor carries the declared group name");
    check(db_compact_d.exclusivity_group == db_write_d.exclusivity_group,
          "two real, independently-built ToolDescriptors for the same declared group name compare "
          "equal on exclusivity_group -- this is the runtime shape a batch-formation step would "
          "partition concurrency classes by (006 §8 G4, still deferred; not exercised here)");

    auto const cache_flush_d = agentengine::make_tool_descriptor<CacheFlushTool>();
    check(cache_flush_d.exclusivity_group.has_value() &&
              *cache_flush_d.exclusivity_group != *db_write_d.exclusivity_group,
          "a distinct declared group produces a distinct runtime exclusivity_group value");

    if (g_failures == 0) {
        std::fprintf(stderr, "ADR-158 tool exclusivity group: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "ADR-158 tool exclusivity group: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
