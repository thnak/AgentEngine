// Implements decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md's own prove phase --
// the first executed evidence against that ADR's §3 falsifiable claims (all previously
// INCONCLUSIVE, no code existed). See turn_middleware.hpp's own top comment for two real,
// mid-implementation refinements this test file also covers: the chain is a single forward pass
// (not the full ADR-033 before/after onion, which has no real inner action to sandwich at this
// point), and `ToolSurfaceView`'s guarantee holds against a middleware using ONLY its public API,
// not against one that bypasses it and reaches into `TurnContext::assembled` directly.

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/turn_middleware.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

Message make_msg(role r, std::string text, std::string message_id) {
    ContentItem item{};
    item.value = Text{std::move(text)};
    Message m{};
    m.role       = r;
    m.message_id = std::move(message_id);
    m.content.push_back(std::move(item));
    return m;
}

Message make_tool_call_msg(std::string call_id, std::string message_id) {
    ContentItem item{};
    item.value = ToolCall{call_id, "some_tool", "{}"};
    Message m{};
    m.role       = role::assistant;
    m.message_id = std::move(message_id);
    m.content.push_back(std::move(item));
    return m;
}

Message make_tool_result_msg(std::string call_id, std::string message_id) {
    ContentItem item{};
    ToolResult tr{};
    tr.call_id = std::move(call_id);
    item.value = std::move(tr);
    Message m{};
    m.role       = role::tool;
    m.message_id = std::move(message_id);
    m.content.push_back(std::move(item));
    return m;
}

ToolDescriptor make_marker_tool(std::string name, std::string marker) {
    ToolDescriptor d;
    d.name        = name;
    d.description = "original description";
    d.invoke      = [marker](json::Value const&, EffectContext&) -> result<json::Value> {
        return json::Value::make_string(marker);
    };
    return d;
}

// A "hostile" turn middleware -- uses ONLY ToolSurfaceView's public API (never reaches around it
// into TurnContext::assembled.combined.tools directly), attempting redact/reorder/annotate on
// every tool it sees. Proves ADR-067 §3's structural claim: no combination of these calls can
// change what a SURVIVING tool's invoke() actually does.
struct HostileToolMiddleware {
    static constexpr std::string_view name = "hostile-tool";

    [[nodiscard]] task<result<std::monostate>> on_turn(TurnContext& ctx) {
        ctx.tool_surface.annotate_description(0, "hostile-rewritten description");
        ctx.tool_surface.redact(1);
        co_return result<std::monostate>{};
    }
};

struct DenyingMiddleware {
    static constexpr std::string_view name = "denying";
    std::shared_ptr<int> calls = std::make_shared<int>(0);

    [[nodiscard]] task<result<std::monostate>> on_turn(TurnContext&) {
        ++*calls;
        co_return std::unexpected(error{failure_class::policy, "denied for test", "test.denied"});
    }
};

struct CountingMiddleware {
    static constexpr std::string_view name = "counting";
    std::shared_ptr<int> calls = std::make_shared<int>(0);

    [[nodiscard]] task<result<std::monostate>> on_turn(TurnContext&) {
        ++*calls;
        co_return result<std::monostate>{};
    }
};

}  // namespace

int main() {
    EffectContext ctx{};

    // --- §3 claim: a turn middleware using only ToolSurfaceView's public API cannot cause a ------
    // surviving tool's actually-dispatched invoke() to differ from what fan-out produced.
    {
        std::vector<ToolDescriptor> tools{
            make_marker_tool("tool_a", "marker-a"),
            make_marker_tool("tool_b", "marker-b"),
            make_marker_tool("tool_c", "marker-c"),
        };
        ContextAssemblyResult assembled;
        assembled.combined.tools = tools;
        TurnContext turn_ctx{assembled};

        std::tuple<HostileToolMiddleware> chain{};
        result<std::monostate> outcome =
            test_support::run_task_sync<result<std::monostate>>(run_turn_middleware_chain(chain, turn_ctx));
        AE_CHECK(outcome.has_value(), "the hostile-but-API-only middleware's chain still succeeds (allow)");

        AE_CHECK(assembled.combined.tools.size() == 2,
                 "redact(1) removed exactly the middle tool, applied at finalize()");
        bool found_a = false, found_c = false;
        for (ToolDescriptor const& t : assembled.combined.tools) {
            auto reply = t.invoke(json::Value::make_null(), ctx);
            AE_CHECK(reply.has_value(), "a surviving tool's invoke() still succeeds when called");
            if (reply.has_value()) {
                std::string const marker = reply->as_string();
                if (marker == "marker-a") found_a = true;
                if (marker == "marker-c") found_c = true;
                AE_CHECK(marker != "marker-b",
                         "the redacted tool's marker never appears -- it's gone, not merely hidden");
            }
        }
        AE_CHECK(found_a && found_c,
                 "both surviving tools' invoke() closures still return their ORIGINAL markers -- no "
                 "substitution occurred through the sanctioned ToolSurfaceView API");
        AE_CHECK(assembled.combined.tools[0].description == "hostile-rewritten description",
                 "annotate_description() DID take effect -- description is the one field this API "
                 "intentionally allows a turn middleware to change");
    }

    // --- §3 claim: the chain short-circuits on first deny; no later middleware runs. --------------
    {
        auto deny_calls  = std::make_shared<int>(0);
        auto count_calls = std::make_shared<int>(0);
        DenyingMiddleware deny{deny_calls};
        CountingMiddleware count{count_calls};

        std::vector<Message> msgs{make_msg(role::user, "hi", "m1")};
        ContextAssemblyResult assembled;
        assembled.combined.messages = msgs;
        TurnContext turn_ctx{assembled};

        std::tuple<DenyingMiddleware, CountingMiddleware> chain{deny, count};
        result<std::monostate> outcome =
            test_support::run_task_sync<result<std::monostate>>(run_turn_middleware_chain(chain, turn_ctx));
        AE_CHECK(!outcome.has_value(), "a denying middleware's chain returns an error (017 §4 'deny')");
        AE_CHECK(*deny_calls == 1, "the denying middleware itself ran exactly once");
        AE_CHECK(*count_calls == 0,
                 "the SECOND middleware never ran at all -- first denial stops the chain, matching "
                 "017 §4's verdict semantics");
    }

    // --- §3 claim (I5): chaining is deterministic given {Ms..., assembled}. -----------------------
    {
        auto make_input = [] {
            ContextAssemblyResult a;
            a.combined.messages = {make_msg(role::user, "aaa", "m1"), make_msg(role::user, "bbb", "m2"),
                                     make_msg(role::user, "ccc", "m3")};
            return a;
        };
        ContextAssemblyResult a1 = make_input();
        ContextAssemblyResult a2 = make_input();
        TurnContext ctx1{a1};
        TurnContext ctx2{a2};
        std::tuple<Compactor<2>> chain1{};
        std::tuple<Compactor<2>> chain2{};
        (void)test_support::run_task_sync<result<std::monostate>>(run_turn_middleware_chain(chain1, ctx1));
        (void)test_support::run_task_sync<result<std::monostate>>(run_turn_middleware_chain(chain2, ctx2));
        AE_CHECK(a1.combined.messages == a2.combined.messages,
                 "running the identical chain against identical input twice produces byte-identical "
                 "output -- deterministic, not incidental (I5)");
    }

    // --- §3 claim: a turn-level Compactor never rewrites history[] -- PROVABLE BY THE TYPE ---------
    // SIGNATURE: TurnContext carries no reference to any history vector at all, so there is no
    // expression anywhere in Compactor<N>::on_turn by which it COULD touch one. Demonstrated here by
    // keeping a real, untouched `history` vector alongside the assembled view and asserting it is
    // byte-identical before/after -- not because the mechanism might reach it (it structurally
    // cannot), but as a concrete, running proof a reader can see rather than take on faith.
    {
        std::vector<Message> const history{make_msg(role::user, "real history, never touched", "h-1")};
        std::vector<Message> const history_snapshot = history;

        ContextAssemblyResult assembled;
        assembled.combined.messages = {
            make_msg(role::user, "m0", "m0"), make_tool_call_msg("c1", "m1"),
            make_tool_result_msg("c1", "m2"), make_msg(role::user, "m3", "m3"),
            make_msg(role::assistant, "m4", "m4"),
        };
        TurnContext turn_ctx{assembled};
        // N=3: naive last-3 windowing starts at index 2 (m2, m3, m4) -- landing exactly ON m2 (the
        // ToolResult), splitting it from its matching ToolCall at m1 (index 1, outside the naive
        // window). This is the scenario that actually exercises the atomic-pair extension; N=2 (naive
        // start=3) does NOT split anything here, since m1 and m2 both fall on the SAME side of that
        // cut -- found the hard way, this test's own first version asserted the wrong thing against
        // N=2, where the "no split" check trivially passed by both being correctly dropped together.
        std::tuple<Compactor<3>> chain{};
        (void)test_support::run_task_sync<result<std::monostate>>(run_turn_middleware_chain(chain, turn_ctx));

        AE_CHECK(history == history_snapshot,
                 "the real, durable history vector is untouched by the compactor -- structurally "
                 "impossible for it to be otherwise, TurnContext holds no reference to it");

        // Naive last-3 windowing would start at index 2 (m2, m3, m4), splitting the c1 ToolCall(m1)/
        // ToolResult(m2) pair (m2 would survive without m1). The atomic-pair fix must extend the
        // window backward to include m1 too.
        std::vector<std::string> ids;
        for (auto const& m : assembled.combined.messages) ids.push_back(m.message_id);
        bool has_m1 = false, has_m2 = false;
        for (auto const& id : ids) {
            if (id == "m1") has_m1 = true;
            if (id == "m2") has_m2 = true;
        }
        AE_CHECK(has_m1 == has_m2,
                 "the ToolCall/ToolResult pair (m1, m2) is never split -- both present or both absent");
        AE_CHECK(has_m1 && has_m2,
                 "in THIS scenario, the pair-preserving extension keeps both m1 and m2, wider than a "
                 "naive last-3 window would have (which would have kept only m2, m3, m4 -- splitting "
                 "the pair)");
    }

    // --- §3 claim: redact_subspan() output is always a subsequence of the input's bytes, for -------
    // arbitrary offset/length including adversarial ones (property check over interesting cases).
    {
        TaintedText const original{"0123456789"};
        struct Case { std::size_t offset, length; std::string expected; };
        std::vector<Case> const cases{
            {0, 0, "0123456789"},           // no-op removal
            {0, 3, "3456789"},              // remove prefix
            {7, 3, "0123456"},              // remove suffix
            {3, 4, "012789"},               // remove middle: "3456" removed from "0123456789"
            {10, 5, "0123456789"},          // offset == size: no-op
            {100, 5, "0123456789"},         // offset > size: no-op
            {5, 1000, "01234"},             // length clamped to remainder
            {0, 1000, ""},                  // whole string removed, length clamped
        };
        for (Case const& c : cases) {
            TaintedText const out = redact_subspan(original, c.offset, c.length);
            AE_CHECK(out.unsafe_view() == c.expected,
                     "redact_subspan(offset=" + std::to_string(c.offset) +
                         ", length=" + std::to_string(c.length) + ") matches the expected subsequence");
        }
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) FAILED\n";
    return 1;
}
