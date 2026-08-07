// Milestone 4 Phase E4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 019
// §4's "a run that fails repeatedly on resume is quarantined after a bounded number of attempts,
// with its state preserved for inspection — not retried forever, and not discarded" had no
// implementation before this task. Proves `PoisonRunPolicy<N>` fires quarantine at EXACTLY the
// bound (never early, never late) against a `ChatClient` that fails every single call, and — the
// "state preserved... not discarded" half — that the session's own history is fully intact and
// inspectable afterward: quarantining is a HOST-side decision to stop retrying, never something
// that reaches into `AgentSession` and deletes/clears anything.

#include <iostream>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

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

// Simulates "a run that fails repeatedly on resume" (019 §4) -- every single call fails, the
// worst case the quarantine bound exists to catch.
class AlwaysFailingChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        co_return std::unexpected(
            ae::error{ae::failure_class::transient, "simulated provider outage", "chat.always_fails"});
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<AlwaysFailingChatClient>,
              "AlwaysFailingChatClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<AlwaysFailingChatClient>;
    using Policy   = ae::PoisonRunPolicy<3>;

    static_assert(!Policy::is_quarantined(0), "0 consecutive failures is never quarantined");
    static_assert(!Policy::is_quarantined(2), "one attempt short of the bound is never quarantined");
    static_assert(Policy::is_quarantined(3), "exactly MaxAttempts consecutive failures IS quarantined");
    static_assert(Policy::is_quarantined(4), "past the bound stays quarantined, never un-quarantines");

    quark::TestKit<Session> kit;
    kit.actor().initialize("s-poison", ae::Principal{"p-remy", ""});

    std::uint32_t consecutive_failures = 0;
    int attempts_made = 0;
    bool quarantined = false;
    for (int attempt = 1; attempt <= 10; ++attempt) {
        auto r = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_user_turn("t" + std::to_string(attempt), "m-" + std::to_string(attempt))});
        attempts_made = attempt;
        if (r.has_value()) {
            consecutive_failures = 0;  // never reached against AlwaysFailingChatClient
        } else {
            ++consecutive_failures;
        }
        if (Policy::is_quarantined(consecutive_failures)) {
            quarantined = true;
            break;
        }
    }

    AE_CHECK(quarantined, "E4-R1: the poison run is eventually quarantined, not retried forever");
    AE_CHECK(attempts_made == 3,
             "E4-R2: quarantine triggers at EXACTLY the bound (3 attempts) -- never early (would "
             "quarantine a run that might have succeeded), never late (would retry past the "
             "declared bound)");

    // --- "State preserved for inspection... not discarded" -- every failed attempt still
    // appended its user turn to history_ (handle()'s own push_back happens BEFORE the chat() call
    // that then fails), and nothing about quarantining ever calls
    // clear_in_process_state()/delete_session() ------------------------------------------------
    AE_CHECK(kit.actor().history().size() == 3,
             "E4-R3: the session's history has exactly 3 entries (one per failed attempt) -- fully "
             "intact and inspectable, never cleared just because the run was quarantined");
    AE_CHECK(kit.actor().session_id() == "s-poison" && kit.actor().principal().id == "p-remy",
             "E4-R4: session identity is untouched -- quarantine is a HOST-side bookkeeping "
             "decision, never a call into AgentSession's own delete/clear paths");

    std::cout << (g_failures == 0 ? "test_agent_session_poison_run: OK\n"
                                   : "test_agent_session_poison_run: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
