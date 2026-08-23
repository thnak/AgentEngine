// Milestone 4 Phase B3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 005
// §3's rules ("every contributor declares a token budget... assembly is pure and replayable") had
// zero implementation anywhere before this task — `ContextContribution`/`ContextProvider` existed
// only as the single-contributor path Phase B2 wired into `AgentSession`. This proves the
// generalized, standalone N-contributor assembler directly: contributor order is preserved in the
// combined output, and assembly is byte-identical (both on success and on failure) across repeated
// runs given the same input (005 §3's purity rule).
//
// 2026-08-23 (docs/planning/2026-08-22-component-role-audit-tracker.md Finding E,
// decisions/ADR-075-context-budget-fail-closed.md): a contributor exceeding its own declared
// `ContextBudget` used to be proven here as a silent OLDEST-first trim that still returned success.
// That behavior was replaced with a hard failure (`assemble_context()` itself returns
// `std::unexpected`) -- this file's over-budget cases below prove the failure, not a trim, and
// `ContextAssemblyResult`'s own `drops` field (still real, `context_assembly.hpp`'s own comment)
// has no producer left to prove here.

#include <iostream>
#include <string>
#include <string_view>
#include <variant>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/trust/principal.hpp"
#include "support/run_task_sync.hpp"

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

ae::Message make_msg(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message m{};
    m.role       = ae::role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

// A minimal second ContextProvider conformer — distinct from HistoryProvider, needed to prove
// multi-contributor ordering/union (a single-provider setup can't distinguish "preserved order"
// from "there was only ever one thing to order").
struct FixedMessagesProvider {
    static constexpr std::string_view name = "fixed-messages";  // ADR-066 §3

    std::vector<ae::Message> to_return;

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages = to_return;
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<FixedMessagesProvider>,
              "FixedMessagesProvider must satisfy ContextProvider (005 §5)");

// Gap-16/21 fix (2026-08-14): a ContextProvider contributing `.instructions`, now `TaintedText` --
// constructing it explicitly IS the trust decision (context_provider.hpp's own comment).
struct FixedInstructionsProvider {
    static constexpr std::string_view name = "fixed-instructions";  // ADR-066 §3

    std::string text;

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        ae::ContextContribution c;
        c.instructions = ae::TaintedText{text};
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<FixedInstructionsProvider>,
              "FixedInstructionsProvider must satisfy ContextProvider (005 §5)");

} // namespace

int main() {
    ae::Principal principal{"p-assembly", ""};

    // Each text is 8 bytes -> approx_token_count == (8+3)/4 == 2 tokens/message (integer div).
    std::vector<ae::Message> history{
        make_msg("aaaaaaaa", "h-1"), make_msg("bbbbbbbb", "h-2"), make_msg("cccccccc", "h-3"),
        make_msg("dddddddd", "h-4"), make_msg("eeeeeeee", "h-5"),
    };
    // Total = 10 tokens. Budget = 5: exceeded -> assemble_context() itself must fail (2026-08-23,
    // Finding E / ADR-075), not silently trim h-1/h-2/h-3 and succeed.
    ae::EffectContext ctx{};

    auto build_contributors = [&]() {
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            ae::HistoryProvider<ae::Window<0>>{}, ae::ContextBudget{5}));
        contributors.push_back(ae::make_context_provider_descriptor(
            FixedMessagesProvider{{make_msg("keep-me", "f-1")}}, ae::ContextBudget{0}));
        return contributors;
    };

    ae::result<ae::ContextAssemblyResult> result1;
    {
        auto contributors = build_contributors();
        ae::SessionContext session_ctx{"s-assembly", principal, history};
        result1 = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
    }

    AE_CHECK(!result1.has_value(),
             "B3-R1: a contributor exceeding its own declared ContextBudget fails the whole "
             "assemble_context() call, rather than silently trimming and succeeding");
    AE_CHECK(!result1.has_value() && result1.error().klass == ae::failure_class::resource,
             "B3-R2: the failure is failure_class::resource -- the same class "
             "rt/agent_session.hpp's own token_budget_ check already uses for the identical shape");
    AE_CHECK(!result1.has_value() && result1.error().code == "context_assembly.contributor_budget_exceeded",
             "B3-R3: the failure carries a stable, catchable code");
    AE_CHECK(!result1.has_value() && result1.error().message.find("history") != std::string::npos &&
                 result1.error().message.find("index 0") != std::string::npos,
             "B3-R4: the failure names WHICH contributor (by declared name and index) exceeded its "
             "budget -- attributable (I4), not a bare 'something failed'");

    // --- Purity/replay (005 §3: "assembly is pure and replayable given {history, ... policies}") -
    // The failure itself is deterministic and byte-identical across repeated runs against the same
    // input, not just the old trim/success path.
    ae::result<ae::ContextAssemblyResult> result2;
    {
        auto contributors = build_contributors();
        ae::SessionContext session_ctx{"s-assembly", principal, history};
        result2 = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
    }
    AE_CHECK(!result2.has_value() && result2.error().code == result1.error().code &&
                 result2.error().message == result1.error().message,
             "B3-R5: re-running assembly against the SAME {history, contributors} produces the "
             "byte-identical failure -- deterministic, not incidental");

    // --- An under-budget contributor succeeds and drops nothing ---------------------------------
    {
        std::vector<ae::Message> small_history{make_msg("a", "s-1")};
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            ae::HistoryProvider<ae::Window<0>>{}, ae::ContextBudget{1000}));
        ae::SessionContext session_ctx{"s-small", principal, small_history};
        auto result3 = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
        AE_CHECK(result3.has_value() && result3->drops.empty() && result3->combined.messages.size() == 1,
                 "B3-R6: a contribution well under its declared budget succeeds and drops nothing");
    }

    // --- Exactly AT budget (not over) succeeds -- the check is `total > max_tokens`, not `>=` -----
    {
        // "a" -> approx_token_count == (1+3)/4 == 1 token.
        std::vector<ae::Message> exact_history{make_msg("a", "e-1")};
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            ae::HistoryProvider<ae::Window<0>>{}, ae::ContextBudget{1}));
        ae::SessionContext session_ctx{"s-exact", principal, exact_history};
        auto result3b = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
        AE_CHECK(result3b.has_value() && result3b->combined.messages.size() == 1,
                 "B3-R7: a contribution exactly AT its declared budget succeeds -- only STRICTLY "
                 "exceeding it is a failure");
    }

    // --- Gap-16/21 fix: multiple contributors' TaintedText instructions concatenate, in declared
    // order, without re-litigating either contributor's own already-made trust decision ------------
    {
        std::vector<ae::Message> no_history;
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            FixedInstructionsProvider{"first. "}, ae::ContextBudget{0}));
        contributors.push_back(ae::make_context_provider_descriptor(
            FixedInstructionsProvider{"second."}, ae::ContextBudget{0}));
        ae::SessionContext session_ctx{"s-instructions", principal, no_history};
        auto result4 = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
        AE_CHECK(result4.has_value() && result4->combined.instructions.has_value(),
                 "B3-I1: two instructions-contributing providers combine into a real value");
        AE_CHECK(result4.has_value() && result4->combined.instructions.has_value() &&
                     result4->combined.instructions->unsafe_view() == "first. second.",
                 "B3-I2: combined instructions concatenate in DECLARED contributor order, exactly");
    }
    {
        // A provider that contributes NOTHING to instructions (the dominant real-world shape today)
        // leaves the combined field unset, not a stray empty TaintedText.
        std::vector<ae::Message> no_history;
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            FixedMessagesProvider{{make_msg("hi", "m-1")}}, ae::ContextBudget{0}));
        ae::SessionContext session_ctx{"s-no-instructions", principal, no_history};
        auto result5 = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, session_ctx, ctx));
        AE_CHECK(result5.has_value() && !result5->combined.instructions.has_value(),
                 "B3-I3: a contributor that never sets .instructions leaves the combined field unset");
    }

    std::cout << (g_failures == 0 ? "test_context_assembly: OK\n" : "test_context_assembly: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
