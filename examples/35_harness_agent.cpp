// AgentEngine example 35 -- the Harness: one call from a declared agent to a fully loaded session.
//
// Mirrors Microsoft Agent Framework's `chatClient.AsHarnessAgent()` / `create_harness_agent()`
// (docs/research/2026-10-01-agent-harness.md): `make_harness_session<Writer>()` (core/harness.hpp,
// decisions/ADR-234, 002 §2.2) runs the ADR-226 run bridge -- so the session enforces exactly what `Writer`
// declares and holds only the authority the caller passes -- and adds the harness's default pieces:
//   - harness operating instructions ahead of the agent's own;
//   - a todo list (todos_* tools, ADR-166);
//   - plan-first mode (ADR-167): tools that act are held back until the model has written a plan and called
//     plan_ready -- the harness makes the gate cover the agent's own tools;
//   - approval suspension: a call that needs approval parks the run for a human instead of being denied;
//   - telemetry counters over the run's event stream (capped by the agent's own Telemetry<...>).
// Opt-in pieces: compaction (a HistoryT strategy), skills (a host SkillsProvider), and bounded reflection
// (a host evaluator, ADR-168) -- this example turns reflection on.
//
// What the run shows: the (scripted) model tries to publish before planning and is refused; it plans with
// todos_add, opens the gate with plan_ready, publishes, and answers; the host's evaluator wants a line
// starting "Summary:", so it sends one round of feedback and the second answer satisfies it.
//
// Offline on purpose: testing::ScriptedChatClient plays the model, so this runs with no API key and no
// network. For a real model, pass a real ChatClient and its key in AgentSessionOptions::chat_client_grants
// (see examples/01_hello_agent.cpp's note).
//
// Run: ./agentengine_example_35_harness_agent

#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/harness.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct DraftArgs {
    std::string text;
};
AE_JSON_SCHEMA(DraftArgs, text)
struct DraftReply {
    std::string result;
};
AE_JSON_SCHEMA(DraftReply, result)

// Read-only and capability-free: usable while the agent is still planning.
struct ReadBrief : Tool<ReadBrief, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name        = "read_brief";
    static constexpr std::string_view description = "Read the assignment brief.";
    using Args  = DraftArgs;
    using Reply = DraftReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{"Write 3 lines on why tests need positive controls."}; }
};

// Has an effect: held back by the plan gate until the plan exists.
int g_published = 0;
struct Publish : Tool<Publish> {
    static constexpr std::string_view name        = "publish";
    static constexpr std::string_view description = "Publish the final text.";
    using Args  = DraftArgs;
    using Reply = DraftReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        ++g_published;
        return Reply{"published " + std::to_string(a.text.size()) + " chars"};
    }
};

struct Writer : Agent<Writer, ChatClientId<"example:scripted">, Tools<ReadBrief, Publish>, MaxTurns<8>,
                      TokenBudget<2'000>> {
    static constexpr std::string_view name         = "writer";
    static constexpr std::string_view instructions = "You are Writer. Read the brief, then publish a short piece.";
};

testing::ScriptedTurn call(std::string tool, std::string id, std::string args) {
    return testing::tool_calls_turn({testing::ScriptedToolCall{std::move(id), std::move(tool), std::move(args)}});
}

task<result<rt::EvaluationVerdict>> verdict(bool ok) {
    co_return rt::EvaluationVerdict{ok, "Please end with a line that starts with 'Summary:'."};
}

}  // namespace

int main() {
    testing::ScriptedChatClient model;
    testing::ScriptedChatClient script = model;  // copies share one script
    (void)script.push({
        call("publish", "c1", R"({"text":"too early"})"),        // refused: no plan yet
        call("read_brief", "c2", R"({"text":""})"),               // allowed while planning
        call("todos_add", "c3", R"({"title":"draft three lines"})"),
        call("plan_ready", "c4", "{}"),                           // opens the gate
        call("publish", "c5", R"({"text":"Tests need controls."})"),
        testing::text_turn("Published the piece."),               // iteration 1: evaluator unsatisfied
        testing::text_turn("Published the piece.\nSummary: one short piece, published."),  // iteration 2
    });

    HarnessOptions harness;
    harness.reflection_evaluator = [](rt::AgentResponse const& r) {
        return verdict(text_of(r.message).find("Summary:") != std::string::npos);
    };

    auto writer = make_harness_session<Writer>(model, AgentSessionOptions{}, std::move(harness));
    if (!writer) {
        std::fprintf(stderr, "bind failed: %s\n", writer.error().message.c_str());
        return 1;
    }

    std::printf("pieces:");
    for (std::string const& p : writer->pieces()) std::printf(" %s", p.c_str());
    std::printf("\nplan gate covers:");
    for (std::string const& t : writer->plan_gated_tools()) std::printf(" %s", t.c_str());
    std::printf("\n");

    auto outcome = writer->run_reflective("Do the assignment.");
    if (!outcome) {
        std::fprintf(stderr, "run failed: %s (%s)\n", outcome.error().message.c_str(), outcome.error().code.c_str());
        return 1;
    }
    std::printf("answer: %s\n", text_of(outcome->response.message).c_str());
    std::printf("reflection: satisfied=%s after %u iteration(s)\n", outcome->satisfied ? "yes" : "no",
                outcome->iterations_used);
    std::printf("todo list:\n");
    for (auto const& item : writer->todo_state()->items) {
        std::printf("  [%c] %s\n", item.completed ? 'x' : ' ', item.title.c_str());
    }
    auto const& t = *writer->telemetry();
    std::printf("telemetry: runs=%llu model_calls=%llu tool_calls=%llu\n",
                static_cast<unsigned long long>(t.runs_started.load()),
                static_cast<unsigned long long>(t.model_calls.load()),
                static_cast<unsigned long long>(t.tool_calls.load()));
    std::printf("capabilities held: %zu (the caller granted none; the harness adds none)\n",
                writer->capabilities().size());

    check(outcome->satisfied && outcome->iterations_used == 2, "the evaluator was satisfied on the second iteration");
    check(g_published == 1, "publish ran exactly once -- after the plan, never before it");
    check(writer->plan_gate()->executing, "the plan gate is open");
    check(writer->capabilities().size() == 0, "the harness session holds no capability the caller did not pass");
    check(script.pending() == 0, "the whole script was used");
    return g_failures == 0 ? 0 : 1;
}
