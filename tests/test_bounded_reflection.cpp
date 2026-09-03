// Implements decisions/ADR-168-bounded-reflection-loop.md's proof obligations for
// `run_with_bounded_reflection()` (rt/bounded_reflection.hpp), issue #55. Mirrors
// examples/03_multi_turn.cpp's fake-ChatClient/drive() pattern -- this loop's whole point is
// repeated `start_run()` calls on one session, so the test needs a ChatClient that answers
// differently per call, exactly like that example already established.

#include <cstdio>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/bounded_reflection.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::AgentResponse;
using agentengine::rt::EvaluationVerdict;
using agentengine::rt::ReflectionOutcome;
using agentengine::rt::run_with_bounded_reflection;
using agentengine::rt::StartRun;

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

// Answers with "draft N" each call -- deterministic, distinguishable per iteration, no real
// suspension (never awaits anything external), matching JokerChatClient's role in
// examples/03_multi_turn.cpp.
class DraftingChatClient {
public:
    std::size_t call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        std::string const text = "draft " + std::to_string(call_count);
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{text};
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-" + std::to_string(call_count);
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<DraftingChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] task<result<EvaluationVerdict>> satisfied_evaluator(AgentResponse const&) {
    co_return EvaluationVerdict{true, ""};
}

}  // namespace

int main() {
    Principal principal{"p-reflect", ""};

    // --- R1: satisfied on the first try -- exactly one start_run(), iterations_used == 1 --------
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r1", principal);
        session.emplace_chat_client();

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(run_with_bounded_reflection(
            session, user_message("write it"), 5, satisfied_evaluator));
        check(outcome.has_value(), "R1: loop succeeds");
        if (outcome) {
            check(outcome->satisfied, "R1: satisfied on first attempt");
            check(outcome->iterations_used == 1, "R1: exactly one iteration used");
            check(text_of(outcome->response.message) == "draft 0", "R1: response is the first draft");
        }
        check(session.history().size() == 2, "R1: session history holds exactly one user+assistant pair");
    }

    // --- R2: unsatisfied twice, satisfied on the third -- iterations_used == 3, three real turns -
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r2", principal);
        session.emplace_chat_client();

        int calls = 0;
        auto evaluator = [&calls](AgentResponse const&) -> task<result<EvaluationVerdict>> {
            ++calls;
            co_return EvaluationVerdict{calls >= 3, "not good enough yet, try again"};
        };

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(run_with_bounded_reflection(
            session, user_message("write it"), 5, evaluator));
        check(outcome.has_value(), "R2: loop succeeds");
        if (outcome) {
            check(outcome->satisfied, "R2: eventually satisfied");
            check(outcome->iterations_used == 3, "R2: took exactly three iterations");
            check(text_of(outcome->response.message) == "draft 2", "R2: response is the third draft");
        }
        // 3 iterations * (user/feedback + assistant) == 6 messages.
        check(session.history().size() == 6, "R2: three real start_run() turns recorded in history");

        // I3 provenance: the SECOND and THIRD turns' inputs are evaluator feedback, not the literal
        // user -- role::system, content_origin::external, tainted -- never masquerading as the user.
        if (session.history().size() == 6) {
            Message const& second_input = session.history()[2];
            check(second_input.role == role::system, "R2: feedback turn is role::system, not role::user");
            check(!second_input.content.empty() && second_input.content.front().origin == content_origin::external,
                  "R2: feedback content_origin is external");
            check(!second_input.content.empty() && second_input.content.front().tainted,
                  "R2: feedback content is tainted");
        }
    }

    // --- R3: bound reached without satisfaction -- an OK outcome, not an error --------------------
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r3", principal);
        session.emplace_chat_client();

        auto never_satisfied = [](AgentResponse const&) -> task<result<EvaluationVerdict>> {
            co_return EvaluationVerdict{false, "still not good enough"};
        };

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(run_with_bounded_reflection(
            session, user_message("write it"), 3, never_satisfied));
        check(outcome.has_value(), "R3: reaching the bound is NOT an error");
        if (outcome) {
            check(!outcome->satisfied, "R3: not satisfied when the bound is hit");
            check(outcome->iterations_used == 3, "R3: used exactly max_iterations, never more");
        }
        check(session.history().size() == 6, "R3: exactly 3 real turns happened, no extra iteration beyond the bound");
    }

    // --- R4: evaluator failure aborts the loop immediately, never treated as satisfied/retry -----
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r4", principal);
        session.emplace_chat_client();

        auto failing_evaluator = [](AgentResponse const&) -> task<result<EvaluationVerdict>> {
            co_return std::unexpected(
                error{failure_class::fatal, "judge model unreachable", "test.evaluator_broke"});
        };

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(run_with_bounded_reflection(
            session, user_message("write it"), 5, failing_evaluator));
        check(!outcome.has_value(), "R4: evaluator failure propagates as a real error");
        if (!outcome) {
            check(outcome.error().code == "test.evaluator_broke",
                  "R4: the real evaluator error is propagated verbatim, not replaced/laundered");
        }
        check(session.history().size() == 2,
              "R4: exactly one start_run() happened before the evaluator failure aborted the loop");
    }

    // --- R5: max_iterations == 0 is rejected before any start_run() at all ------------------------
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r5", principal);
        session.emplace_chat_client();

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(
            run_with_bounded_reflection(session, user_message("write it"), 0, satisfied_evaluator));
        check(!outcome.has_value(), "R5: max_iterations == 0 is rejected");
        if (!outcome) {
            check(outcome.error().code == "bounded_reflection.zero_iterations",
                  "R5: rejected with the specific zero-iterations diagnostic");
        }
        check(session.history().empty(), "R5: no start_run() call was made at all");
    }

    // --- R6: bound is independent of the inner run's MaxTurns -- each start_run() gets a FRESH ----
    // per-run turn budget (agent_session.hpp:953-960: run_tokens_consumed_/turn_index reset every
    // start_run()), so a session capped at MaxTurns==1 still completes a 3-iteration reflection loop
    // without ever hitting run.max_turns_exceeded -- the positive control for this file's own
    // top-comment claim that the two bounds are genuinely separate axes.
    {
        AgentSession<DraftingChatClient> session;
        session.initialize("s-r6", principal);
        session.emplace_chat_client();
        session.set_max_turns(1);

        int calls = 0;
        auto evaluator = [&calls](AgentResponse const&) -> task<result<EvaluationVerdict>> {
            ++calls;
            co_return EvaluationVerdict{calls >= 3, ""};
        };

        auto outcome = test_support::run_task_sync<result<ReflectionOutcome>>(run_with_bounded_reflection(
            session, user_message("write it"), 5, evaluator));
        check(outcome.has_value(), "R6: MaxTurns<1> does not block a 3-iteration reflection loop");
        if (outcome) {
            check(outcome->satisfied, "R6: still reaches satisfaction");
            check(outcome->iterations_used == 3, "R6: took three iterations, each within its own MaxTurns<1>");
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "test_bounded_reflection: all checks passed\n"
                                          : "test_bounded_reflection: FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
