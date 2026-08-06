// Milestone 4 Phase B3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 005
// §3's rules ("every contributor declares a token budget... drop order is declared, not
// incidental... drops are recorded in the trace... assembly is pure and replayable") had zero
// implementation anywhere before this task — `ContextContribution`/`ContextProvider` existed only
// as the single-contributor path Phase B2 wired into `AgentSession`. This proves the generalized,
// standalone N-contributor assembler directly: budget-triggered drops are the OLDEST messages
// within one contributor's own contribution, drop order/content is byte-identical across repeated
// runs (005 §3's purity rule), and contributor order is preserved in the combined output.

#include <algorithm>
#include <iostream>
#include <string>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/trust/principal.hpp"

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
    std::vector<ae::Message> to_return;

    [[nodiscard]] ae::result<ae::ContextContribution> on_context(ae::SessionContext&, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages = to_return;
        return c;
    }
    void on_turn_end(ae::EffectContext&) {}
};
static_assert(ae::ContextProvider<FixedMessagesProvider>,
              "FixedMessagesProvider must satisfy ContextProvider (005 §5)");

} // namespace

int main() {
    ae::Principal principal{"p-assembly", ""};

    // Each text is 8 bytes -> approx_token_count == (8+3)/4 == 2 tokens/message (integer div).
    std::vector<ae::Message> history{
        make_msg("aaaaaaaa", "h-1"), make_msg("bbbbbbbb", "h-2"), make_msg("cccccccc", "h-3"),
        make_msg("dddddddd", "h-4"), make_msg("eeeeeeee", "h-5"),
    };
    // Total = 10 tokens. Budget = 5: drop oldest until total <= 5 -> drops h-1 (10->8), h-2 (8->6),
    // h-3 (6->4, now <= 5, stop) -- keeps h-4, h-5 (4 tokens), drops h-1/h-2/h-3 in that order.
    ae::EffectContext ctx{};

    auto build_contributors = [&]() {
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            ae::HistoryProvider<ae::Window<0>>{}, ae::ContextBudget{5}));
        contributors.push_back(ae::make_context_provider_descriptor(
            FixedMessagesProvider{{make_msg("keep-me", "f-1")}}, ae::ContextBudget{0}));
        return contributors;
    };

    ae::ContextAssemblyResult result1;
    {
        auto contributors = build_contributors();
        ae::SessionContext session_ctx{"s-assembly", principal, history};
        result1 = ae::assemble_context(contributors, session_ctx, ctx);
    }

    AE_CHECK(result1.drops.size() == 3, "B3-R1: exactly 3 messages dropped from the over-budget contributor");
    AE_CHECK(result1.drops.size() == 3 && result1.drops[0].contributor_message_id == "h-1" &&
                 result1.drops[1].contributor_message_id == "h-2" &&
                 result1.drops[2].contributor_message_id == "h-3",
             "B3-R2: drop order is OLDEST-first within the contributor, and matches exactly (h-1, "
             "h-2, h-3) -- not incidental, per 005 §3");
    AE_CHECK(std::ranges::all_of(result1.drops, [](ae::ContextDrop const& d) { return d.contributor_index == 0; }),
             "B3-R3: every drop is attributed to contributor index 0 (HistoryProvider), never the "
             "second, under-budget contributor");

    AE_CHECK(result1.combined.messages.size() == 3,
             "B3-R4: combined output keeps 2 surviving history messages + 1 from the second "
             "contributor (3 total)");
    AE_CHECK(result1.combined.messages.size() == 3 && result1.combined.messages[0].message_id == "h-4" &&
                 result1.combined.messages[1].message_id == "h-5" &&
                 result1.combined.messages[2].message_id == "f-1",
             "B3-R5: contributor ORDER is preserved in the combined output (history's survivors "
             "first, then the second contributor's contribution) -- 005 §3's ordered ⊕");

    // --- Purity/replay (005 §3: "assembly is pure and replayable given {history, ... policies}") -
    ae::ContextAssemblyResult result2;
    {
        auto contributors = build_contributors();
        ae::SessionContext session_ctx{"s-assembly", principal, history};
        result2 = ae::assemble_context(contributors, session_ctx, ctx);
    }
    AE_CHECK(result2.drops.size() == result1.drops.size() &&
                 result2.combined.messages.size() == result1.combined.messages.size(),
             "B3-R6: re-running assembly against the SAME {history, contributors} produces the "
             "identical drop count and message count -- deterministic, not incidental");
    for (std::size_t i = 0; i < result1.drops.size(); ++i) {
        AE_CHECK(result2.drops[i].contributor_message_id == result1.drops[i].contributor_message_id,
                 "B3-R7: drop #" + std::to_string(i) + " is byte-identical across the two runs");
    }

    // --- An under-budget contributor never drops anything --------------------------------------
    {
        std::vector<ae::Message> small_history{make_msg("a", "s-1")};
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(
            ae::HistoryProvider<ae::Window<0>>{}, ae::ContextBudget{1000}));
        ae::SessionContext session_ctx{"s-small", principal, small_history};
        auto result3 = ae::assemble_context(contributors, session_ctx, ctx);
        AE_CHECK(result3.drops.empty() && result3.combined.messages.size() == 1,
                 "B3-R8: a contribution well under its declared budget drops nothing");
    }

    std::cout << (g_failures == 0 ? "test_context_assembly: OK\n" : "test_context_assembly: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
