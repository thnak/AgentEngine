// Implements decisions/ADR-160-parallel-tool-batch-scheduler.md §5 "Batch partitioning". Proves
// `partition_batch()` (core/tool_pipeline.hpp) in complete isolation from AgentSession -- no I/O, no
// admission, no invocation, just the pure eligibility-gate + concurrency-class logic against real
// `ToolDescriptor`s built through the real `Tool<>`/`make_tool_descriptor<>()` path (never hand-rolled),
// including the three tags this scheduler reads together for the first time: `Parallelizable`
// (ADR-160 §5's own new `ToolDescriptor::parallelizable` field), `ExclusivityGroup<Name>` (ADR-158),
// and `captures_session_state` (ADR-028).

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

struct NoopArgs { bool ignored = false; };
AE_JSON_SCHEMA(NoopArgs, ignored)
struct NoopReply { bool ok; };
AE_JSON_SCHEMA(NoopReply, ok)

struct PlainTool : agentengine::Tool<PlainTool> {
    static constexpr std::string_view name = "plain";
    static constexpr std::string_view description = "Declares no concurrency policy at all.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct ParallelTool : agentengine::Tool<ParallelTool, agentengine::Parallelizable> {
    static constexpr std::string_view name = "parallel_a";
    static constexpr std::string_view description = "Bare Parallelizable.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct ParallelToolB : agentengine::Tool<ParallelToolB, agentengine::Parallelizable> {
    static constexpr std::string_view name = "parallel_b";
    static constexpr std::string_view description = "Bare Parallelizable, second independent tool.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct DbWriteTool : agentengine::Tool<DbWriteTool, agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "db_write";
    static constexpr std::string_view description = "ExclusivityGroup<db-write>, member 1.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct DbCompactTool : agentengine::Tool<DbCompactTool, agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "db_compact";
    static constexpr std::string_view description = "ExclusivityGroup<db-write>, member 2.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct CacheFlushTool : agentengine::Tool<CacheFlushTool, agentengine::ExclusivityGroup<"cache-flush">> {
    static constexpr std::string_view name = "cache_flush";
    static constexpr std::string_view description = "A DIFFERENT ExclusivityGroup, its own class.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

// ADR-160 §5 MUST-FIX 1: a session-state-capturing descriptor built via
// make_tool_descriptor_with_invoke<T>(), like the real, shipped ScheduleWakeupTool -- if it were
// EVER declared Parallelizable, partitioning must still force it sequential. `StatefulTool` itself
// declares Parallelizable (a tool author's mistake this scheduler must guard against structurally,
// not merely by convention) -- the ONLY way to reach `captures_session_state == true` combined with
// `parallelizable == true` on a real descriptor, since `make_tool_descriptor_with_invoke<T>()`
// (unlike the plain factory) sets `captures_session_state = true` unconditionally.
struct StatefulTool : agentengine::Tool<StatefulTool, agentengine::Parallelizable> {
    static constexpr std::string_view name = "stateful";
    static constexpr std::string_view description = "Session-state-capturing AND (mistakenly) Parallelizable.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

agentengine::ToolCallRequest req(std::string call_id, std::string tool_name) {
    agentengine::ToolCallRequest r;
    r.call_id = std::move(call_id);
    r.tool_name = std::move(tool_name);
    return r;
}

}  // namespace

int main() {
    agentengine::ToolTable table = agentengine::ToolTable::from_descriptors({
        agentengine::make_tool_descriptor<PlainTool>(),
        agentengine::make_tool_descriptor<ParallelTool>(),
        agentengine::make_tool_descriptor<ParallelToolB>(),
        agentengine::make_tool_descriptor<DbWriteTool>(),
        agentengine::make_tool_descriptor<DbCompactTool>(),
        agentengine::make_tool_descriptor<CacheFlushTool>(),
        agentengine::make_tool_descriptor_with_invoke<StatefulTool>(
            [](NoopArgs, agentengine::EffectContext&) -> agentengine::result<NoopReply> {
                return NoopReply{true};
            }),
    });

    // -- ToolDescriptor::parallelizable itself (ADR-160 §5's new field) ---------------------------
    check(table.find("plain")->parallelizable == false, "an undeclared tool is not parallelizable");
    check(table.find("parallel_a")->parallelizable == true,
          "a bare-Parallelizable tool's descriptor reports parallelizable=true");
    check(table.find("db_write")->parallelizable == false,
          "ADR-158: ExclusivityGroup<Name> must NOT also set parallelizable -- the two are distinct,\n"
          "mutually exclusive claims (Tool<>'s own static_assert already proves no type can declare both)");

    // -- Not-eligible: any call whose tool declares NEITHER tag demotes the WHOLE batch to sequential
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "parallel_a"), req("c2", "plain")}, table);
        check(classes.size() == 2, "a non-eligible batch has one singleton class per call");
        for (auto const& cls : classes) {
            check(cls.kind == agentengine::concurrency_class_kind::sequential,
                  "a non-eligible batch's classes are all `sequential`, even the Parallelizable one");
        }
    }

    // -- Eligible: every call declares Parallelizable or ExclusivityGroup<Name> --------------------
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "parallel_a"), req("c2", "parallel_b")}, table);
        check(classes.size() == 2, "two independent Parallelizable calls -> two singleton classes");
        for (auto const& cls : classes) {
            check(cls.kind == agentengine::concurrency_class_kind::parallel,
                  "each bare-Parallelizable call gets its own `parallel` class");
            check(cls.call_indices.size() == 1, "a `parallel` class always has exactly one member");
        }
    }

    // -- ExclusivityGroup<Name> members sharing a name -> ONE class, not one per member (MUST-FIX 5)
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "db_write"), req("c2", "db_compact"), req("c3", "cache_flush")}, table);
        check(classes.size() == 2, "two group NAMES -> two classes, regardless of member count");
        bool found_db_write_group = false, found_cache_flush_group = false;
        for (auto const& cls : classes) {
            check(cls.kind == agentengine::concurrency_class_kind::exclusivity_group,
                  "every class in this all-grouped batch is `exclusivity_group`");
            if (cls.group_name == "db-write") {
                found_db_write_group = true;
                check(cls.call_indices.size() == 2,
                      "db-write's class holds BOTH members sharing that name, in emitted order");
                check(cls.call_indices[0] == 0 && cls.call_indices[1] == 1,
                      "db-write group members appear in emitted order: db_write (0) before db_compact (1)");
            } else if (cls.group_name == "cache-flush") {
                found_cache_flush_group = true;
                check(cls.call_indices.size() == 1, "cache-flush's class holds its own single member");
                check(cls.call_indices[0] == 2, "cache_flush is index 2 in the emitted batch");
            }
        }
        check(found_db_write_group, "a db-write class was actually produced");
        check(found_cache_flush_group, "a cache-flush class was actually produced");
    }

    // -- Mixed batch, all classes eligible: singleton + group classes coexist -----------------------
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "parallel_a"), req("c2", "db_write"), req("c3", "db_compact")}, table);
        check(classes.size() == 2, "one `parallel` singleton + one 2-member `exclusivity_group` class");
    }

    // -- MUST-FIX 1: captures_session_state forces sequential, unconditionally, even though it also
    // counts as "declares a tag" for the eligibility gate itself (006 §5's literal text is about the
    // declared TAG, not what actually runs concurrently) ------------------------------------------
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "parallel_a"), req("c2", "stateful")}, table);
        // `stateful` declares Parallelizable, so the batch-eligibility gate (every call declares a
        // tag) is satisfied -- this is NOT the "non-eligible, whole batch sequential" case.
        bool saw_parallel_class = false, saw_forced_sequential = false;
        for (auto const& cls : classes) {
            if (cls.kind == agentengine::concurrency_class_kind::parallel) saw_parallel_class = true;
            if (cls.kind == agentengine::concurrency_class_kind::sequential) {
                saw_forced_sequential = true;
                check(cls.call_indices.size() == 1 && cls.call_indices[0] == 1,
                      "the captures_session_state call (index 1) is forced into its own sequential "
                      "singleton class, despite the batch itself being fan-out-eligible");
            }
        }
        check(saw_parallel_class,
              "the OTHER (non-stateful) Parallelizable call still gets a real `parallel` class -- "
              "MUST-FIX 1 forces only the stateful call sequential, not the whole batch");
        check(saw_forced_sequential, "a sequential class for the stateful call was actually produced");
    }

    // -- Unresolved tool name counts as NOT eligible -- resolution stays admission's job -----------
    {
        auto classes = agentengine::partition_batch(
            {req("c1", "parallel_a"), req("c2", "does_not_exist")}, table);
        check(classes.size() == 2, "an unresolved tool name demotes the whole batch to sequential");
        for (auto const& cls : classes) {
            check(cls.kind == agentengine::concurrency_class_kind::sequential,
                  "including the call that WOULD have been eligible on its own");
        }
    }

    // -- Empty batch --------------------------------------------------------------------------------
    check(agentengine::partition_batch({}, table).empty(), "an empty batch partitions to zero classes");

    if (g_failures == 0) {
        std::fprintf(stderr, "ADR-160 partition_batch: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "ADR-160 partition_batch: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
