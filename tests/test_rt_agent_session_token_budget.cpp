// Proof for ADR-037 Phase 2: porting the real behavioral claims from the OLD, Quark-actor-based
// test_agent_session_token_budget.cpp onto agentengine::rt::AgentSession (include/agentengine/rt/
// agent_session.hpp) directly -- deterministic, offline, no live model, no network, no
// quark::TestKit<A>/quark::Ask plumbing. 004 §5's per-run TokenBudget<N>, enforced at the turn
// boundary inside run_rounds() (`token_budget_.has_value() && run_tokens_consumed_ > *token_budget_`).
//
// Claim (2) of the old file -- a run whose total usage EXCEEDS the configured ceiling fails closed
// with run.token_budget_exceeded -- is already covered by test_rt_agent_session_streaming_and_events.
// cpp's own A4b (confirmed by reading that file: it initializes a token_budget of 1, scripts a reply
// carrying 1000 tokens, and asserts the RunFailed event's error_code is exactly
// "run.token_budget_exceeded") and by test_rt_agent_session.cpp's own S4 (same shape, via the return
// value rather than the event stream). NOT re-ported here.
//
// What IS genuinely uncovered by any existing rt:: test and is ported here:
//   B1 -- a run whose usage is UNDER the configured budget succeeds normally, and
//         run_tokens_consumed() reflects exactly that run's input+output tokens.
//   B2 -- no budget configured (the default std::nullopt) is unbounded: an arbitrarily large Usage
//         never fails the run.
//   B3 -- the accumulator resets across SEPARATE runs on the SAME session -- it is a PER-RUN budget,
//         not a per-session-lifetime one (two runs that are each individually under budget, but whose
//         SUMMED usage would exceed it, both succeed).

#include <cstdint>
#include <cstdio>
#include <string>

#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;
using agentengine::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- ScriptedUsageChatClient::chat() below co_returns
// immediately.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ContentItem;

// Ported verbatim (in spirit) from the old test_agent_session_token_budget.cpp's own
// ScriptedUsageChatClient: the returned Usage is driven entirely by the content of the request it
// receives, not by external mutable state reached into the session -- the last user message's text
// is "<input_tokens>,<output_tokens>"; chat() parses it and reports exactly that Usage back, so a
// test controls the returned token totals purely through what StartRun{...} it sends.
class ScriptedUsageChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        std::uint64_t in_tokens = 0;
        std::uint64_t out_tokens = 0;
        if (!request.messages.empty()) {
            ContentItem const& item = request.messages.back().content.front();
            if (auto const* t = std::get_if<Text>(&item.value)) {
                std::string const& text = t->text;
                auto const comma = text.find(',');
                if (comma != std::string::npos) {
                    in_tokens  = std::stoull(text.substr(0, comma));
                    out_tokens = std::stoull(text.substr(comma + 1));
                }
            }
        }

        Message reply;
        reply.role = role::assistant;
        reply.message_id = "m-reply";
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{"reply"};
        reply.content.push_back(item);

        co_return ChatResponse{reply, Usage{in_tokens, out_tokens, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused (stream_model_calls_ stays false throughout this file)
    }
};
static_assert(agentengine::ChatClient<ScriptedUsageChatClient>);

// `text` is "<input_tokens>,<output_tokens>" -- see ScriptedUsageChatClient::chat() above.
Message scripted_turn(std::uint64_t input_tokens, std::uint64_t output_tokens) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::to_string(input_tokens) + "," + std::to_string(output_tokens)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;

    // B1: a run whose usage is UNDER the configured budget succeeds normally.
    {
        AgentSession<ScriptedUsageChatClient> session;
        session.initialize("s-under", Principal{"p-a", ""}, /*token_budget=*/std::uint64_t{100});
        session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{scripted_turn(/*in=*/40, /*out=*/30)}));  // 70, under 100
        check(outcome.has_value(), "B1: a run under budget resolves with a real AgentResponse");
        check(session.history().size() == 2,
              "B1: an under-budget run appends both the input and the reply to history");
        check(session.run_tokens_consumed() == 70,
              "B1: run_tokens_consumed() reflects input_tokens + output_tokens for the run");
    }

    // B2: no budget configured (the default std::nullopt) is unbounded.
    {
        AgentSession<ScriptedUsageChatClient> session;
        session.initialize("s-unbounded", Principal{"p-c", ""});  // no token_budget argument -- default nullopt
        session.emplace_chat_client();

        std::uint64_t const huge = 1'000'000'000ULL;
        auto outcome = drive(session.start_run(StartRun{scripted_turn(huge, huge)}));
        check(outcome.has_value(),
              "B2: with no budget configured, an arbitrarily large Usage never fails the run");
        check(session.history().size() == 2, "B2: the unbounded run's reply is appended to history normally");
        check(session.run_tokens_consumed() == 2 * huge,
              "B2: the accumulator still tracks consumption even when unbounded -- it just never gates "
              "anything without a configured ceiling");
    }

    // B3: the accumulator resets across separate runs on the SAME session -- per-run, not
    // per-session-lifetime.
    {
        AgentSession<ScriptedUsageChatClient> session;
        // Budget is 100; each individual run's usage (60) is under budget on its own, but the two
        // runs' usages SUMMED (120) would exceed it -- if the accumulator were session-lifetime
        // rather than per-run, the second run would wrongly fail closed.
        session.initialize("s-reset", Principal{"p-d", ""}, /*token_budget=*/std::uint64_t{100});
        session.emplace_chat_client();

        auto r1 = drive(session.start_run(StartRun{scripted_turn(/*in=*/30, /*out=*/30)}));  // 60 total
        check(r1.has_value(), "B3: run 1 (60 tokens, under the 100 budget) succeeds");
        check(session.run_tokens_consumed() == 60, "B3: run 1's own accumulator total is exactly its own usage");

        auto r2 = drive(session.start_run(StartRun{scripted_turn(/*in=*/30, /*out=*/30)}));  // 60 total again
        check(r2.has_value(),
              "B3: run 2 (also 60 tokens) ALSO succeeds -- proving the accumulator reset at the top of "
              "start_run() rather than carrying run 1's 60 tokens forward (which would have made run "
              "2's total 120, over budget)");
        check(session.run_tokens_consumed() == 60,
              "B3: after run 2, the accumulator holds ONLY run 2's total, not the sum of both runs -- "
              "direct proof the reset is per-run, not per-session-lifetime");
        check(session.history().size() == 4, "B3: both runs' turns landed in history -- neither was rejected");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_token_budget: ALL PASS\n");
    return 0;
}
