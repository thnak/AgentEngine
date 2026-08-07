// Milestone 4 Phase B4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): the
// `Summarize<N>` compaction strategy (005 §4's table, second row) had no implementation anywhere
// before this task -- `Window<N>` (Phase B2) was the only strategy `HistoryProvider` supported.
// This proves `HistoryProvider<Summarize<N, SummarizerT>>` against a mock summarizer `ChatClient`
// (decision 8: mock extraction/summarization is the established precedent every turn-loop test in
// this project already uses, not a new exception): the verbatim tail is exactly the last N
// messages, everything older is folded into one `system`-role summary message, and repeating the
// same input produces a byte-identical summary -- 005 §7 G3's "bounded divergence for Summarize"
// gate, proven by construction rather than asserted.

#include <iostream>
#include <string>

#include "agentengine/core/content.hpp"
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

// Deterministic given its input: the "summary" is just a count + concatenation of what it was
// asked to summarize, proving the REAL older-message content reached the summarizer (never a
// placeholder call) while staying trivially reproducible.
class MockSummarizerClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const& request, ae::EffectContext&) {
        std::string joined;
        for (auto const& m : request.messages) {
            if (!m.content.empty()) joined += std::get<ae::Text>(m.content.front().value).text + ";";
        }

        ae::ContentItem item{};
        item.value  = ae::Text{"SUMMARY(" + std::to_string(request.messages.size()) + "):" + joined};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;  // deliberately NOT system -- proves HistoryProvider re-labels it
        reply.message_id = "m-summary";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<MockSummarizerClient>,
              "MockSummarizerClient must satisfy the ChatClient concept (004 §1)");

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

[[nodiscard]] std::string text_of(ae::Message const& m) {
    return std::get<ae::Text>(m.content.front().value).text;
}

} // namespace

int main() {
    using Provider = ae::HistoryProvider<ae::Summarize<2, MockSummarizerClient>>;
    static_assert(ae::ContextProvider<Provider>,
                  "HistoryProvider<Summarize<N, SummarizerT>> must satisfy ContextProvider (005 §5)");

    ae::Principal principal{"p-summarize", ""};
    std::vector<ae::Message> history{
        make_msg("one", "h-1"), make_msg("two", "h-2"), make_msg("three", "h-3"),
        make_msg("four", "h-4"), make_msg("five", "h-5"),
    };
    ae::EffectContext ctx{};

    // --- History shorter than the verbatim window: no summary, nothing fabricated -----------------
    {
        Provider provider;
        std::vector<ae::Message> short_history{make_msg("only", "s-1")};
        ae::SessionContext session_ctx{"s-short", principal, short_history};
        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value() && out->messages.size() == 1 && text_of(out->messages[0]) == "only",
                 "B4-C1: history at or under the window keeps everything verbatim, no summary "
                 "message fabricated (matches Window<N>'s own rule for the same case)");
    }

    // --- History longer than the window: exactly 1 summary + the last N verbatim ------------------
    ae::result<ae::ContextContribution> out1;
    {
        Provider provider;
        ae::SessionContext session_ctx{"s-long", principal, history};
        out1 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
    }
    AE_CHECK(out1.has_value() && out1->messages.size() == 3,
             "B4-R1: 5 messages, Summarize<2> -> 1 summary + 2 verbatim = 3 total");
    AE_CHECK(out1.has_value() && out1->messages.size() == 3 &&
                 out1->messages[0].role == ae::role::system,
             "B4-R2: the summary message is re-labeled `system` regardless of what role the "
             "summarizer's own reply carried (005 §4)");
    AE_CHECK(out1.has_value() && out1->messages.size() == 3 &&
                 text_of(out1->messages[0]) == "SUMMARY(3):one;two;three;",
             "B4-R3: the summary was produced from exactly the OLDER 3 messages (one/two/three), "
             "proving the real content reached the summarizer, not a placeholder call");
    AE_CHECK(out1.has_value() && out1->messages.size() == 3 && text_of(out1->messages[1]) == "four" &&
                 text_of(out1->messages[2]) == "five",
             "B4-R4: the last 2 messages (four, five) survive verbatim, unmodified by the summarizer");

    // --- Bounded-divergence gate (005 §7 G3): the SAME history summarized twice produces a
    // byte-identical result -- deterministic given a deterministic summarizer, never re-derived
    // from wall-clock or randomness ------------------------------------------------------------------
    ae::result<ae::ContextContribution> out2;
    {
        Provider provider;
        ae::SessionContext session_ctx{"s-long", principal, history};
        out2 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
    }
    AE_CHECK(out2.has_value() && out1.has_value() && out2->messages.size() == out1->messages.size() &&
                 text_of(out2->messages[0]) == text_of(out1->messages[0]) &&
                 text_of(out2->messages[1]) == text_of(out1->messages[1]) &&
                 text_of(out2->messages[2]) == text_of(out1->messages[2]),
             "B4-R5: replaying against the identical history produces a byte-identical compacted "
             "result (005 §7 G3's bounded-divergence gate, proven by construction)");

    std::cout << (g_failures == 0 ? "test_history_provider_summarize: OK\n"
                                   : "test_history_provider_summarize: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
