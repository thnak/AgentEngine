// Milestone 5 Phase F4 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, "Design
// decisions (2026-08-07)" block under Phase F): 004 §5's "per-run TokenBudget<N> -- exceeded ->
// Resource failure at the turn boundary" had no consumer anywhere in the codebase before this task
// (`AgentMetadata::token_budget`, agent_registry.hpp:60, was compiled but never read). Proves
// `AgentSession`'s new per-run accumulator (`run_tokens_consumed_`, agent_session.hpp) does exactly
// what the design doc specifies: (1) a run under budget succeeds normally; (2) a run whose total
// usage exceeds the configured ceiling fails closed -- the ask never resolves with a response, the
// SAME shape the pre-existing `!contribution`/`!response` branches already use, proven via
// `TestKit::ask`'s own documented "failed by reply-before-teardown if the handler never replied"
// path (quark/core/testkit.hpp); (3) no budget configured (the default `std::nullopt`) is
// unbounded -- an arbitrarily large `Usage` never fails the run; (4) the accumulator resets across
// separate runs on the same session -- it is a PER-RUN budget, not a per-session-lifetime one.

#include <cstdint>
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

// A `ChatClient` conformer whose returned `Usage` is scripted by the CALLER, driven entirely by the
// content of the request it receives -- the same "derive behavior from the request, not from
// external mutable state reached into the actor" shape `EchoChatClient`
// (test_agent_session_isolation.cpp) already establishes, since `AgentSession` default-constructs
// its `ChatClientT` member with no external wiring hook. The last user message's text is
// "<input_tokens>,<output_tokens>"; `chat()` parses it and reports exactly that `Usage` back, so a
// test controls the returned token totals purely through what `StartRun{...}` it sends.
class ScriptedUsageChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const& request, ae::EffectContext&) {
        std::uint64_t in_tokens = 0;
        std::uint64_t out_tokens = 0;
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (std::holds_alternative<ae::Text>(item.value)) {
                std::string const& text = std::get<ae::Text>(item.value).text;
                auto const comma = text.find(',');
                if (comma != std::string::npos) {
                    in_tokens  = std::stoull(text.substr(0, comma));
                    out_tokens = std::stoull(text.substr(comma + 1));
                }
            }
        }

        ae::ContentItem item{};
        item.value  = ae::Text{"reply"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);

        co_return ae::ChatResponse{reply, ae::Usage{in_tokens, out_tokens, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<ScriptedUsageChatClient>,
              "ScriptedUsageChatClient must satisfy the ChatClient concept (004 §1)");

// `text` is "<input_tokens>,<output_tokens>" -- see `ScriptedUsageChatClient::chat()` above.
ae::Message make_scripted_turn(std::uint64_t input_tokens, std::uint64_t output_tokens,
                                std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::to_string(input_tokens) + "," + std::to_string(output_tokens)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<ScriptedUsageChatClient>;

    // --- (1) A run whose usage is UNDER the configured budget succeeds normally --------------------
    {
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-under", ae::Principal{"p-a", ""}, /*token_budget=*/100);

        auto r = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_scripted_turn(/*in=*/40, /*out=*/30, "m-1")});  // 70 total, under 100
        AE_CHECK(r.has_value(), "F4-R1: a run under budget resolves with a real AgentResponse");
        AE_CHECK(kit.actor().history().size() == 2,
                 "F4-R2: an under-budget run appends both the input and the reply to history");
        AE_CHECK(kit.actor().run_tokens_consumed() == 70,
                 "F4-R3: run_tokens_consumed() reflects input_tokens + output_tokens for the run");
    }

    // --- (2) A run whose usage is OVER the configured budget fails closed --------------------------
    {
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-over", ae::Principal{"p-b", ""}, /*token_budget=*/100);

        auto r = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_scripted_turn(/*in=*/80, /*out=*/30, "m-1")});  // 110 total, over 100
        AE_CHECK(!r.has_value(),
                 "F4-R4: a run over budget never resolves with a response -- fail closed, the same "
                 "shape as the pre-existing !contribution/!response branches (never a hang, per "
                 "TestKit::ask's own 'failed by reply-before-teardown' documented path)");
        AE_CHECK(kit.actor().history().size() == 1,
                 "F4-R5: an over-budget run appends only the user's input, never the reply -- no "
                 "history push on the fail-closed path, matching the two branches above it");
        AE_CHECK(kit.actor().run_tokens_consumed() == 110,
                 "F4-R6: the accumulator still recorded the full over-budget total before failing "
                 "closed (observable via the accessor even though the ask itself never resolved)");
    }

    // --- (3) No budget configured (std::nullopt, the default) is unbounded --------------------------
    {
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-unbounded", ae::Principal{"p-c", ""});  // no third argument -- default nullopt

        std::uint64_t const huge = 1'000'000'000ULL;
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_scripted_turn(huge, huge, "m-1")});
        AE_CHECK(r.has_value(),
                 "F4-R7: with no budget configured, an arbitrarily large Usage never fails the run");
        AE_CHECK(kit.actor().history().size() == 2,
                 "F4-R8: the unbounded run's reply is appended to history normally");
        AE_CHECK(kit.actor().run_tokens_consumed() == 2 * huge,
                 "F4-R9: the accumulator still tracks consumption even when unbounded -- it just "
                 "never gates anything without a configured ceiling");
    }

    // --- (4) The accumulator resets across separate runs on the same session ------------------------
    {
        quark::TestKit<Session> kit;
        // Budget is 100; each individual run's usage (60) is under budget on its own, but the two
        // runs' usages SUMMED (120) would exceed it -- if the accumulator were session-lifetime
        // rather than per-run, the second run would wrongly fail closed.
        kit.actor().initialize("s-reset", ae::Principal{"p-d", ""}, /*token_budget=*/100);

        auto r1 = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_scripted_turn(/*in=*/30, /*out=*/30, "m-1")});  // 60 total
        AE_CHECK(r1.has_value(), "F4-R10: run 1 (60 tokens, under the 100 budget) succeeds");
        AE_CHECK(kit.actor().run_tokens_consumed() == 60,
                 "F4-R11: run 1's own accumulator total is exactly its own usage");

        auto r2 = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_scripted_turn(/*in=*/30, /*out=*/30, "m-2")});  // 60 total again
        AE_CHECK(r2.has_value(),
                 "F4-R12: run 2 (also 60 tokens) ALSO succeeds -- proving the accumulator reset at "
                 "the top of handle() rather than carrying run 1's 60 tokens forward (which would "
                 "have made run 2's total 120, over budget)");
        AE_CHECK(kit.actor().run_tokens_consumed() == 60,
                 "F4-R13: after run 2, the accumulator holds ONLY run 2's total, not the sum of both "
                 "runs -- direct proof the reset is per-run, not per-session-lifetime");
        AE_CHECK(kit.actor().history().size() == 4,
                 "F4-R14: both runs' turns landed in history -- neither was rejected");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_token_budget: OK\n"
                                   : "test_agent_session_token_budget: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
