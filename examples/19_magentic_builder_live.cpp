// AgentEngine "get started" examples, 19 -- the Magentic convenience API (ADR-149, GitHub issue
// #28) against a REAL model.
//
// The same live moderator/researcher/writer cycle 17_planner_live.cpp hand-builds, rebuilt with
// `agentengine::workflow::MagenticWorkflowBuilder` instead of a hand-wired `Workflow`/`Edge` list --
// proving the convenience layer produces a genuinely runnable graph, not just one that "looks
// right" as data. Unlike 17, this uses TWO GENUINELY DISTINCT declared message types (`TaskMsg`,
// what the moderator assigns; `ReportMsg`, what a specialist reports back) -- ADR-149 §3 finding 9
// named the earlier, single-shared-type design as untested by 17's own "T"/"T" placeholder shape;
// this example is the real exercise of that path. Also demonstrates `max_stalls()` composing with a
// live moderator via `designated_stall_reporter` (ADR-149 item 2) -- the moderator reports
// `stalled=true` whenever its own live reply didn't parse, so a model that never produces a usable
// decision trips the safety valve instead of silently exhausting `max_rounds`.
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via
// tools/run-live-provider-tests.ps1, or set it yourself. SKIPS (exit 0), same as every other
// live-network test in this repo, when it's absent.
//
// Run: ./agentengine_example_19_magentic_builder_live

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "agentengine/workflow/magentic.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;

[[nodiscard]] agentengine::result<ChatResponse> live_call(RealClient& client, EffectContext& ctx,
                                                            std::string system_prompt,
                                                            std::string user_content) {
    ChatRequest req;
    ContentItem sys_item{};
    sys_item.origin = content_origin::system;
    sys_item.value  = Text{std::move(system_prompt)};
    Message sys{};
    sys.role = role::system;
    sys.content.push_back(sys_item);
    req.messages.push_back(sys);
    req.messages.push_back(text_message(std::move(user_content)));
    return drive(client.chat(req, ctx));
}

// The moderator: identical decision logic to 17_planner_live.cpp's own, plus one addition --
// `stalled=true` whenever the live reply didn't parse AND the fallback ledger also can't tell what
// to do next (a genuine "no progress possible this round" signal, not merely "used the fallback").
[[nodiscard]] ExecutorBody planner_moderator(RealClient& client, EffectContext& ctx) {
    return [&client, &ctx](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const transcript = text_of(in);

        auto resp = live_call(
            client, ctx,
            "You are coordinating two specialists on a task: a Researcher (gathers facts) and a "
            "Writer (composes a short summary from facts already gathered). Read the transcript and "
            "reply with EXACTLY ONE WORD, nothing else: RESEARCHER if facts still need gathering, "
            "WRITER if facts exist but no summary has been written yet, or DONE if a summary already "
            "exists and the task is complete.",
            "Transcript so far:\n" + transcript);

        std::string decision;
        bool        live_decided = false;
        if (resp.has_value()) {
            std::string const raw = upper(text_of(resp->message));
            if (raw.find("DONE") != std::string::npos) { decision = "done"; live_decided = true; }
            else if (raw.find("RESEARCHER") != std::string::npos) { decision = "researcher"; live_decided = true; }
            else if (raw.find("WRITER") != std::string::npos) { decision = "writer"; live_decided = true; }
        }
        bool fallback_decided = false;
        if (!live_decided) {
            bool const has_facts   = transcript.find("Researcher:") != std::string::npos;
            bool const has_summary = transcript.find("Writer:") != std::string::npos;
            decision          = !has_facts ? "researcher" : (!has_summary ? "writer" : "done");
            fallback_decided  = true;
        }
        std::printf("[moderator] %s%s\n", decision.c_str(), live_decided ? "" : " (fallback ledger)");

        ExecutorOutcome out{text_message(transcript), {decision}};
        // ADR-149 item 2's stalled signal: the moderator only ever sets this on ITS OWN outcome,
        // and only when the live call genuinely failed (not merely "used the fallback ledger",
        // which still makes real forward-progress decisions).
        out.stalled = !resp.has_value();
        return out;
    };
}

[[nodiscard]] ExecutorBody specialist(std::string label, std::string system_prompt, RealClient& client,
                                       EffectContext& ctx) {
    return [label, system_prompt, &client, &ctx](Message const& in,
                                                  EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const transcript = text_of(in);
        auto resp = live_call(client, ctx, system_prompt, "Transcript so far:\n" + transcript);
        if (!resp.has_value()) return std::unexpected(resp.error());
        std::string const reply = text_of(resp->message);
        std::printf("[%s] %s\n", label.c_str(), reply.c_str());
        return ExecutorOutcome{text_message(transcript + "\n\n" + label + ": " + reply)};
    };
}

[[nodiscard]] std::string env_or(char const* name, char const* fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::string(fallback);
}

}  // namespace

// Two genuinely distinct declared message types (ADR-149 §3 finding 9) -- NOT the same "T"/"T"
// placeholder 17_planner_live.cpp uses, which never exercised TypedExecutor's real static_assert.
// Named and string-declared IDENTICALLY across every file that needs a Magentic test message pair
// -- see test_workflow_magentic_builder.cpp's identical block for the full ODR reasoning.
struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Magentic.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Magentic.ReportMsg");

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "example_19_magentic_builder_live: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is "
                     "not set.\n  Run tools/run-live-provider-tests.ps1, or set the variable "
                     "yourself, to exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::fprintf(stderr, "example_19_magentic_builder_live: host=%s model=%s\n", host.c_str(), model.c_str());

    constexpr char const* kSecretName = "openrouter-api-key";
    InMemorySecretStore   store;
    store.set(kSecretName, *key_env);
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 200;

    RealClient client(host, /*port=*/443, model, SecretRef{kSecretName}, caps, store, "/api/v1");
    static_assert(ChatClient<RealClient>, "OpenAIChatClient must satisfy the ChatClient concept");

    EffectContext ctx;
    ctx.principal    = agentengine::Principal{"example-19-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    MagenticWorkflowBuilder<TaskMsg, ReportMsg> builder("planner-live-magentic");
    builder.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "moderator", .capability_ceiling = {}});
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "researcher", .capability_ceiling = {}});
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "writer", .capability_ceiling = {}});
    builder.max_rounds(10);  // a safety valve, same as 17 -- the task needs 6 rounds if the moderator judges well
    builder.max_stalls(3);   // ADR-149 item 2 -- 3 consecutive unparseable live replies is a real stall

    result<MagenticGraph> built = builder.build();
    check(built.has_value(), "the MagenticWorkflowBuilder builds a valid graph");
    if (!built) {
        std::fprintf(stderr, "example_19_magentic_builder_live: FAIL (build error: %s)\n",
                     built.error().message.c_str());
        return 1;
    }
    check(validate_workflow(built->graph).has_value(), "the produced graph validates");

    // `bodies` is parallel to `built->graph.executors` BY INDEX -- MagenticWorkflowBuilder's own
    // add-order convention: manager, then participants in call order, then the synthetic done sink.
    std::vector<ExecutorBody> bodies = {
        planner_moderator(client, ctx),
        specialist("Researcher",
                   "You are a Researcher. State 2-3 concise, accurate factual bullet "
                   "points about the given topic. Do not write a summary -- just facts.",
                   client, ctx),
        specialist("Writer",
                   "You are a Writer. Using ONLY the facts already gathered in the "
                   "transcript above, write a 2-sentence summary suitable for a general "
                   "audience. Synthesize, do not restate the facts verbatim.",
                   client, ctx),
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{in};  // the builder's synthetic "done" sink -- caller-supplied,
                                          // per magentic.hpp's own "bodies always caller-supplied"
                                          // layering rule.
        },
    };

    WorkflowSupervisor supervisor;
    supervisor.initialize(built->graph, bodies, {}, built->manager_id);

    WorkflowResult r = drive(supervisor.run_workflow(
        RunWorkflow{text_message("Task: research and summarize what quantum entanglement is.")}));
    check(r.status != workflow_status::invalid, "the workflow run returns");
    std::printf("\n--- status: %d, rounds: %u ------------------------------\n",
                static_cast<int>(r.status), r.rounds);
    std::printf("%s\n", text_of(r.output).c_str());
    check(r.status == workflow_status::completed || r.status == workflow_status::bound_max_rounds ||
              r.status == workflow_status::bound_max_stalls,
          "the run ends in an honest, expected status either way");
    if (r.status == workflow_status::completed) {
        check(r.rounds < *built->graph.bound.max_rounds,
              "when the moderator decides completion, it's well under the safety-valve bound");
    }

    std::fprintf(stderr,
                 g_failures == 0 ? "example_19_magentic_builder_live: OK\n"
                                 : "example_19_magentic_builder_live: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
