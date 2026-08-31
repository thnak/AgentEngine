// AgentEngine "get started" examples, 17 -- Planner (Magentic) against a REAL model.
//
// The live counterpart to 15_group_chat_and_planner.cpp's offline Planner half. There, the
// moderator's "ledger" was a hardcoded turn counter. Here it's a real model call: the moderator
// reads the transcript so far and decides, live, which specialist should go next -- or whether the
// goal is met -- by replying with exactly one word (RESEARCHER / WRITER / DONE), parsed into
// `ExecutorOutcome::routes` exactly the way 10_conditional_routing.cpp's offline classifier already
// does. `bound.max_rounds` stays a safety valve, same as the offline example, in case a live
// model's own judgment ever loops -- and a deterministic fallback (scan the transcript for what's
// already been contributed) keeps the demo from stalling if a reply is ever unparseable, without
// ever pretending the fallback IS the live decision.
//
// Task: research two or three facts about a topic, then write a short summary from them, then stop
// -- exactly the "moderator owns completion, round bound is a safety valve it should never reach"
// shape 014 §3 draws between Planner and Group Chat.
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via
// `tools/run-live-provider-tests.ps1`, or set it yourself. SKIPS (exit 0), same as every other
// live-network test in this repo, when it's absent.
//
// Run: ./agentengine_example_17_planner_live

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
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

// Drives an `agentengine::rt::task<T>` to completion from a plain, non-coroutine `main()` -- the same
// helper `tests/test_rt_workflow_supervisor.cpp` establishes: safe here because neither the
// workflow's own superstep loop nor a specialist's `client.chat()` call genuinely parks (the real
// HTTPS client blocks under the hood instead of suspending a coroutine), so one external `resume()`
// chain runs the whole thing inline.
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

// The moderator: asks the real model which specialist should go next, or whether the goal is met.
// Falls back to a deterministic ledger (scan the transcript for what each specialist already
// contributed) if the live reply doesn't parse -- a safety net, never the primary decision.
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
        if (!live_decided) {
            // Deterministic fallback ledger -- never the primary decision, only a safety net.
            bool const has_facts   = transcript.find("Researcher:") != std::string::npos;
            bool const has_summary = transcript.find("Writer:") != std::string::npos;
            decision = !has_facts ? "researcher" : (!has_summary ? "writer" : "done");
        }
        std::printf("[moderator] %s%s\n", decision.c_str(), live_decided ? "" : " (fallback ledger)");
        return ExecutorOutcome{text_message(transcript), {decision}};
    };
}

[[nodiscard]] ExecutorBody specialist(std::string label, std::string system_prompt, RealClient& client,
                                       EffectContext& ctx) {
    return [label, system_prompt, &client, &ctx](Message const& in,
                                                  EffectContext&) -> agentengine::result<Message> {
        std::string const transcript = text_of(in);
        auto resp = live_call(client, ctx, system_prompt, "Transcript so far:\n" + transcript);
        if (!resp.has_value()) return std::unexpected(resp.error());
        std::string const reply = text_of(resp->message);
        std::printf("[%s] %s\n", label.c_str(), reply.c_str());
        return text_message(transcript + "\n\n" + label + ": " + reply);
    };
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] std::string env_or(char const* name, char const* fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::string(fallback);
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "example_17_planner_live: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is not "
                     "set.\n  Run tools/run-live-provider-tests.ps1, or set the variable yourself, "
                     "to exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::fprintf(stderr, "example_17_planner_live: host=%s model=%s\n", host.c_str(), model.c_str());

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
    ctx.principal    = agentengine::Principal{"example-17-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    Workflow wf;
    wf.id        = "planner-live";
    wf.executors = {node_desc("moderator"), node_desc("researcher"), node_desc("writer"),
                     node_desc("done")};
    wf.edges.push_back(Edge{"moderator", "researcher", edge_kind::switch_case, "researcher"});
    wf.edges.push_back(Edge{"moderator", "writer", edge_kind::switch_case, "writer"});
    wf.edges.push_back(Edge{"moderator", "done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"researcher", "moderator", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"writer", "moderator", edge_kind::direct, {}});
    wf.start = "moderator";
    wf.output_selection.push_back("done");
    wf.bound.max_rounds = 10;  // a safety valve -- the task needs 6 rounds if the moderator judges well
    check(validate_workflow(wf).has_value(), "the graph validates");

    // `bodies` is parallel to `wf.executors` BY INDEX (WorkflowSupervisor::initialize()'s own
    // convention) -- so the order here must match {"moderator", "researcher", "writer", "done"} above.
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
        [](Message const& in, EffectContext&) -> agentengine::result<Message> {
            return text_message(text_of(in));
        },
    };

    WorkflowSupervisor supervisor;
    supervisor.initialize(wf, bodies);

    WorkflowResult r = drive(supervisor.run_workflow(
        RunWorkflow{text_message("Task: research and summarize what quantum entanglement is.")}));
    check(r.status != workflow_status::invalid, "the workflow run returns");
    std::printf("\n--- status: %s, rounds: %u ------------------------------\n",
                r.status == workflow_status::completed ? "completed" : "bound_max_rounds", r.rounds);
    std::printf("%s\n", text_of(r.output).c_str());
    check(r.status == workflow_status::completed || r.status == workflow_status::bound_max_rounds,
          "the run ends in an honest, expected status either way");
    if (r.status == workflow_status::completed) {
        check(r.rounds < wf.bound.max_rounds.value(),
              "when the moderator decides completion, it's well under the safety-valve bound");
    }

    std::fprintf(stderr,
                 g_failures == 0 ? "example_17_planner_live: OK\n" : "example_17_planner_live: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
