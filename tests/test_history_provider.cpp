// Milestone 4 Phase B2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// `HistoryProvider<Window<N>>` is the first real `ContextProvider` conformer -- before this task,
// `context_provider.hpp`'s `ContextProvider` concept had exactly one conforming type anywhere in
// this codebase (`smoke_vocabulary.cpp`'s `DummyContextProvider`, a trivial stub that returns an
// empty `ContextContribution` and proves nothing about windowing). This proves the real windowing
// logic directly against a hand-built `SessionContext`, independent of `AgentSession`'s own wiring
// (which `test_agent_session_history_window.cpp` proves separately, end to end).

#include <iostream>
#include <string>

#include "agentengine/core/content.hpp"
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

ae::Message make_turn(std::string text, std::string message_id) {
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
    ae::Principal principal{"p-window", ""};
    std::vector<ae::Message> history{
        make_turn("one", "m-1"), make_turn("two", "m-2"), make_turn("three", "m-3"),
        make_turn("four", "m-4"), make_turn("five", "m-5"),
    };
    ae::EffectContext ctx{};

    // --- Window<0> (unbounded) keeps everything, in order -----------------------------------------
    {
        ae::HistoryProvider<ae::Window<0>> provider;
        ae::SessionContext session_ctx{"s-1", principal, history};
        auto out = provider.on_context(session_ctx, ctx);
        AE_CHECK(out.has_value() && out->messages.size() == 5,
                 "B2-C1: Window<0> keeps the full history (unbounded default)");
        AE_CHECK(out.has_value() && !out->messages.empty() && text_of(out->messages.front()) == "one" &&
                     text_of(out->messages.back()) == "five",
                 "B2-C2: Window<0> preserves original order");
    }

    // --- Window<N> keeps exactly the last N messages, dropping older ones -------------------------
    {
        ae::HistoryProvider<ae::Window<2>> provider;
        ae::SessionContext session_ctx{"s-1", principal, history};
        auto out = provider.on_context(session_ctx, ctx);
        AE_CHECK(out.has_value() && out->messages.size() == 2,
                 "B2-R1: Window<2> keeps exactly 2 messages out of 5");
        AE_CHECK(out.has_value() && out->messages.size() == 2 &&
                     text_of(out->messages[0]) == "four" && text_of(out->messages[1]) == "five",
                 "B2-R2: Window<2> keeps the LAST 2 messages verbatim, in order (005 §4)");
    }

    // --- Window<N> larger than the history keeps everything, no padding/fabrication ---------------
    {
        ae::HistoryProvider<ae::Window<50>> provider;
        ae::SessionContext session_ctx{"s-1", principal, history};
        auto out = provider.on_context(session_ctx, ctx);
        AE_CHECK(out.has_value() && out->messages.size() == 5,
                 "B2-R3: a window wider than the history keeps all of it, fabricates nothing");
    }

    // --- Empty history is not an error --------------------------------------------------------------
    {
        std::vector<ae::Message> empty_history;
        ae::HistoryProvider<ae::Window<3>> provider;
        ae::SessionContext session_ctx{"s-empty", principal, empty_history};
        auto out = provider.on_context(session_ctx, ctx);
        AE_CHECK(out.has_value() && out->messages.empty(),
                 "B2-R4: an empty history windows to an empty (not erroring) contribution");
    }

    std::cout << (g_failures == 0 ? "test_history_provider: OK\n" : "test_history_provider: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
