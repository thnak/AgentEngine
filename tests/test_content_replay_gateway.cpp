// Implements decisions/ADR-069-content-triggered-model-response-replay.md's own prove phase -- the
// first executed evidence against that ADR's §3 falsifiable claims (all previously INCONCLUSIVE, no
// code existed). See content_replay_gateway.hpp's own top comment for a real, mid-implementation
// finding this test file also covers: the amended retry request must never re-include the discarded
// response's own content (that would re-send whatever got it discarded back to the vendor a second
// time) -- checked explicitly below, not just asserted in the header comment.

#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/content_replay_gateway.hpp"
#include "agentengine/core/effect_context.hpp"
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

Message make_user_msg(std::string text) {
    ContentItem item{};
    item.value  = Text{std::move(text)};
    item.origin = content_origin::user;
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    if (m.content.empty()) return {};
    if (auto const* t = std::get_if<Text>(&m.content.front().value)) return t->text;
    return {};
}

// A scripted `ModelCallGatewayLike` conformer -- "ERROR" is a sentinel meaning "fail this attempt as
// a real backend failure," any other string becomes an assistant response carrying that text.
// Records every request it was actually invoked with, so a test can inspect exactly what the
// gateway sent on a retry.
struct ScriptedInner {
    std::vector<std::string> markers;
    std::vector<ChatRequest> received;
    std::size_t              next_index = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext&) {
        received.push_back(request);
        if (next_index >= markers.size()) {
            co_return std::unexpected(
                error{failure_class::fatal, "scripted inner exhausted", "test.exhausted"});
        }
        std::string const marker = markers[next_index++];
        if (marker == "ERROR") {
            co_return std::unexpected(
                error{failure_class::transient, "scripted backend failure", "test.scripted_failure"});
        }
        Message m{};
        m.role = role::assistant;
        ContentItem item{};
        item.value = Text{marker};
        m.content.push_back(std::move(item));
        Usage usage{};
        usage.output_tokens = 10;
        co_return ChatResponse{m, usage};
    }
};
static_assert(ModelCallGatewayLike<ScriptedInner>,
              "ScriptedInner must satisfy ModelCallGatewayLike (chat_client.hpp)");

// Triggers discard_and_retry whenever the response text equals "BAD" -- checks every response the
// gateway hands it exactly once per attempt (counted separately below to prove §3's bounded-
// recursion claim: a replay attempt's own response IS re-checked by the same predicate).
ContentReplayTrigger make_bad_text_trigger(std::shared_ptr<int> trigger_calls) {
    return [trigger_calls](ChatResponse const& response) -> ContentReplayDecision {
        ++*trigger_calls;
        if (text_of(response.message) == "BAD") {
            return ContentReplayDecision{true, "please do not say that -- try again without it"};
        }
        return ContentReplayDecision{};
    };
}

}  // namespace

int main() {
    EffectContext ctx{};

    // --- Baseline: a non-triggering response passes through untouched, exactly one inner call. -----
    {
        auto trigger_calls = std::make_shared<int>(0);
        ScriptedInner inner{{"GOOD"}, {}, 0};
        ContentReplayGateway<ScriptedInner> gateway(inner, make_bad_text_trigger(trigger_calls));
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(outcome.has_value(), "a non-triggering response is returned successfully");
        if (outcome.has_value()) {
            AE_CHECK(text_of(outcome->message) == "GOOD", "the response text is unchanged");
        }
        AE_CHECK(*trigger_calls == 1, "the trigger was consulted exactly once");
        AE_CHECK(gateway.session_replays_used() == 0, "no replay was used");
    }

    // --- §3 claim 4 (bounded recursion): a discard-and-retry response is re-checked, and the -------
    // corrective request never re-includes the discarded response's own content.
    {
        auto trigger_calls = std::make_shared<int>(0);
        ScriptedInner inner{{"BAD", "GOOD"}, {}, 0};
        ContentReplayGateway<ScriptedInner> gateway(inner, make_bad_text_trigger(trigger_calls));
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(outcome.has_value(), "the retried call ultimately succeeds");
        if (outcome.has_value()) {
            AE_CHECK(text_of(outcome->message) == "GOOD", "the FINAL kept response is the retry's, not the discarded one");
        }
        AE_CHECK(*trigger_calls == 2, "the retry's own response was re-checked by the same trigger");
        AE_CHECK(gateway.session_replays_used() == 1, "exactly one replay was consumed");
    }

    // --- Real finding this pass proves explicitly: the discarded "BAD" text never appears in the ---
    // amended retry request -- only the corrective instruction does.
    {
        auto trigger_calls = std::make_shared<int>(0);
        auto inner_ptr = std::make_shared<ScriptedInner>(ScriptedInner{{"BAD", "GOOD"}, {}, 0});
        // Wrap by value into the gateway but keep our own shared_ptr's `received` unreachable after
        // the move -- so instead, run the retry loop against a plain local and inspect it via a
        // reference captured BEFORE construction is not possible since the gateway takes Inner by
        // value. Re-run with a fresh inner and read back through the gateway's own copy is not
        // exposed either -- so this scenario constructs the gateway, then separately re-derives what
        // WOULD be sent by inspecting corrective_message() directly (the exact function call() uses)
        // and confirms IT alone, with no discarded text anywhere, matches what a real request would
        // carry.
        (void)inner_ptr;
        Message const corrective = corrective_message("please do not say that -- try again without it");
        AE_CHECK(text_of(corrective) == "please do not say that -- try again without it",
                 "corrective_message() carries exactly the corrective instruction text");
        AE_CHECK(corrective.role == role::system, "the corrective message is role::system");
        AE_CHECK(!corrective.content.empty() && corrective.content.front().origin == content_origin::system,
                 "the corrective message's content_origin is ::system -- engine-synthesized, not "
                 "user- or assistant-attributed");
    }

    // --- Same finding, proven end-to-end against the REAL request the gateway actually sends: ------
    // `ContentReplayGateway` owns its own copy of `Inner`, so observing what request the SECOND
    // attempt actually received needs an `Inner` that shares its own log via a pointer instead.
    {
        auto trigger_calls = std::make_shared<int>(0);
        struct SharedLogInner {
            std::shared_ptr<ScriptedInner> impl;
            [[nodiscard]] ChatClientCapabilities capabilities() const { return impl->capabilities(); }
            [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext& c) {
                return impl->call(std::move(request), c);
            }
        };
        auto shared_impl = std::make_shared<ScriptedInner>(ScriptedInner{{"BAD", "GOOD"}, {}, 0});
        ContentReplayGateway<SharedLogInner> gateway2(SharedLogInner{shared_impl},
                                                          make_bad_text_trigger(trigger_calls));
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway2.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(outcome.has_value(), "sanity: the shared-log variant still succeeds the same way");
        AE_CHECK(shared_impl->received.size() == 2, "exactly 2 real backend calls were made");
        if (shared_impl->received.size() == 2) {
            ChatRequest const& second_request = shared_impl->received[1];
            AE_CHECK(second_request.messages.size() == 2,
                     "the retry request is the original 1 message PLUS exactly 1 corrective message "
                     "-- nothing else appended");
            bool found_corrective = false;
            bool leaked_discarded_text = false;
            for (Message const& m : second_request.messages) {
                if (text_of(m) == "please do not say that -- try again without it") found_corrective = true;
                if (text_of(m) == "BAD") leaked_discarded_text = true;
            }
            AE_CHECK(found_corrective, "the corrective instruction IS present in the retry request");
            AE_CHECK(!leaked_discarded_text,
                     "the discarded response's own text ('BAD') is NEVER present in the retry "
                     "request -- re-including it would re-send the very thing that got it "
                     "discarded back to the vendor a second time");
        }
    }

    // --- §3 claim: replay is bounded per-trigger-site (max_replay_attempts). -----------------------
    {
        auto trigger_calls = std::make_shared<int>(0);
        auto shared_impl = std::make_shared<ScriptedInner>(ScriptedInner{{"BAD", "BAD", "BAD"}, {}, 0});
        struct SharedLogInner {
            std::shared_ptr<ScriptedInner> impl;
            [[nodiscard]] ChatClientCapabilities capabilities() const { return impl->capabilities(); }
            [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext& c) {
                return impl->call(std::move(request), c);
            }
        };
        ContentReplayGateway<SharedLogInner> gateway(SharedLogInner{shared_impl},
                                                        make_bad_text_trigger(trigger_calls),
                                                        /*max_replay_attempts=*/2,
                                                        /*session_lifetime_cap=*/100);
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(!outcome.has_value(), "an always-triggering response fails closed, never returns a bad response");
        if (!outcome.has_value()) {
            AE_CHECK(outcome.error().code == "content_replay.max_attempts_exhausted",
                     "fails with the per-trigger-site exhaustion code specifically");
        }
        AE_CHECK(shared_impl->received.size() == 2,
                 "exactly max_replay_attempts (2) real calls were made -- not before, not after "
                 "(§3's own 'not before, not after' wording)");
    }

    // --- §3 claim: replay is ALSO bounded across the SESSION's lifetime, independent of and in ------
    // addition to max_replay_attempts -- the must-fix finding's core claim.
    {
        auto trigger_calls = std::make_shared<int>(0);
        auto shared_impl =
            std::make_shared<ScriptedInner>(ScriptedInner{{"BAD", "GOOD", "BAD"}, {}, 0});
        struct SharedLogInner {
            std::shared_ptr<ScriptedInner> impl;
            [[nodiscard]] ChatClientCapabilities capabilities() const { return impl->capabilities(); }
            [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext& c) {
                return impl->call(std::move(request), c);
            }
        };
        // High per-trigger-site budget (5) -- ONLY the session cap (1) should be the thing that
        // stops the SECOND call() invocation from replaying, proving the two bounds are independent.
        ContentReplayGateway<SharedLogInner> gateway(SharedLogInner{shared_impl},
                                                        make_bad_text_trigger(trigger_calls),
                                                        /*max_replay_attempts=*/5,
                                                        /*session_lifetime_cap=*/1);

        auto outcome1 = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("round 1")}}, ctx));
        AE_CHECK(outcome1.has_value(), "the FIRST round's replay succeeds -- within budget");
        AE_CHECK(gateway.session_replays_used() == 1, "the session cap now shows 1 replay used");

        auto outcome2 = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("round 2")}}, ctx));
        AE_CHECK(!outcome2.has_value(),
                 "the SECOND round's replay attempt fails -- the session-lifetime cap is exhausted, "
                 "even though max_replay_attempts (5) was nowhere near hit for THIS trigger site");
        if (!outcome2.has_value()) {
            AE_CHECK(outcome2.error().code == "content_replay.session_cap_exhausted",
                     "fails with the session-lifetime exhaustion code specifically, distinguishing "
                     "it from the per-trigger-site one");
        }
        AE_CHECK(shared_impl->received.size() == 3,
                 "round 1 made 2 real calls (BAD then GOOD), round 2 made exactly 1 (BAD, then "
                 "immediately failed closed without a second attempt) -- 3 total, not 4");
        AE_CHECK(gateway.session_replays_used() == 1,
                 "the session counter never exceeds the cap even though it was consulted again");
    }

    // --- An unconfigured trigger (nullptr) fails open to no-replay, matching ADR-068's ---------------
    // SecretDetector precedent for "host wires nothing."
    {
        ScriptedInner inner{{"BAD"}, {}, 0};  // would trigger if any trigger were configured
        ContentReplayGateway<ScriptedInner> gateway(inner, ContentReplayTrigger{});
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(outcome.has_value(), "with no trigger configured, the response passes through unchanged");
        if (outcome.has_value()) {
            AE_CHECK(text_of(outcome->message) == "BAD",
                     "the UNCHECKED text survives -- an unconfigured trigger means no policy is "
                     "enforced at all, the same fail-open shape this codebase already accepts for "
                     "an unconfigured SecretDetector (ADR-068)");
        }
    }

    // --- A real backend failure propagates untouched and consumes no replay budget. -----------------
    {
        auto trigger_calls = std::make_shared<int>(0);
        ScriptedInner inner{{"ERROR"}, {}, 0};
        ContentReplayGateway<ScriptedInner> gateway(inner, make_bad_text_trigger(trigger_calls));
        auto outcome = test_support::run_task_sync<result<ChatResponse>>(
            gateway.call(ChatRequest{{make_user_msg("hi")}}, ctx));
        AE_CHECK(!outcome.has_value(), "a real backend failure is NOT converted into a replay attempt");
        if (!outcome.has_value()) {
            AE_CHECK(outcome.error().code == "test.scripted_failure",
                     "the ORIGINAL failure code propagates unchanged -- this gateway does not "
                     "relabel a real backend error as its own");
        }
        AE_CHECK(*trigger_calls == 0, "the trigger is never even consulted for a failed attempt");
        AE_CHECK(gateway.session_replays_used() == 0, "no replay budget was consumed");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) FAILED\n";
    return 1;
}
