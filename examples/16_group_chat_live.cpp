// AgentEngine "get started" examples, 16 -- Group Chat against a REAL model.
//
// The live counterpart to 15_group_chat_and_planner.cpp's offline version. There, the moderator and
// both participants were plain functions -- the point was the GRAPH shape (a moderator cycling
// among participants, bounded by the caller's own round count). Here the two participants are real
// live `OpenAIChatClient` calls against OpenRouter, each with its own persona, actually debating a
// question -- the same `FunctionExecutor`/`Workflow`/`WorkflowSupervisor` machinery, just with a
// body that makes a real network call instead of appending a string. `agent`-kind executors aren't
// runnable yet (see the Workflow & Orchestration API page's "not yet built" note) -- this is what a
// model-backed node looks like TODAY, as an ordinary `function`-kind executor whose body happens to
// call a real `ChatClient`.
//
// `ChatClient::chat()` is a coroutine (`ae::task<result<ChatResponse>>`, which under ADR-037 resolves
// to `agentengine::rt::task<result<ChatResponse>>` for any non-void `T` -- core/task.hpp), but
// `rt::ExecutorBody` is a plain synchronous `std::function`. `rt::task<T>` was built to support being
// driven directly (`.resume()`/`.done()`/`.take_value()`), not just `co_await`ed, so the bridge here
// is the same plain `drive<T>()` loop every migrated rt:: file uses -- safe because the real HTTPS
// client's I/O is genuinely blocking under the hood, never something that needs a live reactor to
// progress.
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via
// `tools/run-live-provider-tests.ps1`, or set it yourself. SKIPS (exit 0), same as every other
// live-network test in this repo, when it's absent -- this example builds and the rest of the suite
// stays green with no credential and no network.
//
// Run: ./agentengine_example_16_group_chat_live

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
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

// Drives an `agentengine::rt::task<T>` to completion from a plain, non-coroutine `main()` -- the same
// helper `tests/test_rt_workflow_supervisor.cpp` establishes: safe here because neither the
// workflow's own superstep loop nor a debater's `client.chat()` call genuinely parks (the real HTTPS
// client blocks under the hood instead of suspending a coroutine), so one external `resume()` chain
// runs the whole thing inline.
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

using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;

// A participant executor: asks the real model to continue the debate as `persona`, given the
// transcript so far, and appends "\n\n<persona>: <reply>" to it. `client`/`ctx` are captured by
// reference -- this example never backgrounds a workflow node, so their lifetime (the enclosing
// `main()`'s stack) safely outlives every call.
[[nodiscard]] ExecutorBody debater(std::string persona, std::string stance, RealClient& client,
                                    EffectContext& ctx) {
    return [persona, stance, &client, &ctx](Message const& in,
                                             EffectContext&) -> agentengine::result<Message> {
        std::string const transcript = text_of(in);

        ChatRequest req;
        ContentItem sys_item{};
        sys_item.origin = content_origin::system;
        sys_item.value  = Text{"You are " + persona + " in a short debate. " + stance +
                                " Reply in 2-3 sentences, staying in character. Do not repeat the "
                                "transcript back, just give your next contribution."};
        Message sys{};
        sys.role = role::system;
        sys.content.push_back(sys_item);
        req.messages.push_back(sys);
        req.messages.push_back(text_message("Transcript so far:\n" + transcript));

        auto resp = drive(client.chat(req, ctx));
        if (!resp.has_value()) {
            return std::unexpected(resp.error());
        }
        std::string const reply = text_of(resp->message);
        std::printf("[%s] %s\n", persona.c_str(), reply.c_str());
        return text_message(transcript + "\n\n" + persona + ": " + reply);
    };
}

[[nodiscard]] Executor node_desc(char const* id) { return Executor{id, executor_kind::function, "T", "T"}; }

[[nodiscard]] std::string env_or(char const* name, char const* fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::string(fallback);
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "example_16_group_chat_live: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is not "
                     "set.\n  Run tools/run-live-provider-tests.ps1, or set the variable yourself, "
                     "to exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::fprintf(stderr, "example_16_group_chat_live: host=%s model=%s\n", host.c_str(), model.c_str());

    constexpr char const* kSecretName = "openrouter-api-key";
    InMemorySecretStore   store;
    store.set(kSecretName, *key_env);
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming        = true;
    caps.tool_calling     = true;
    caps.max_output_tokens = 200;  // keep each reply short -- bounds cost and wall-clock

    RealClient client(host, /*port=*/443, model, SecretRef{kSecretName}, caps, store, "/api/v1");
    static_assert(ChatClient<RealClient>, "OpenAIChatClient must satisfy the ChatClient concept");

    EffectContext ctx;
    ctx.principal     = agentengine::Principal{"example-16-principal", ""};
    ctx.capabilities  = agentengine::borrow_capabilities(held);

    // The SAME "moderator cycles among participants, bounded by the caller's round count" shape
    // 15_group_chat_and_planner.cpp uses -- just two live nodes instead of two plain functions.
    Workflow wf;
    wf.id        = "group-chat-live";
    wf.executors = {node_desc("optimist"), node_desc("skeptic")};
    wf.edges.push_back(Edge{"optimist", "skeptic", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"skeptic", "optimist", edge_kind::direct, {}});
    wf.start = "optimist";
    wf.output_selection.push_back("skeptic");
    wf.bound.max_rounds = 4;  // two turns each -- short and cheap
    check(validate_workflow(wf).has_value(), "the graph validates");

    // `bodies` is parallel to `wf.executors` BY INDEX (WorkflowSupervisor::initialize()'s own
    // convention) -- so the order here must match {"optimist", "skeptic"} above exactly.
    std::vector<ExecutorBody> bodies = {
        debater("the Optimist", "Argue FOR using microservices for a new project.", client, ctx),
        debater("the Skeptic", "Argue AGAINST using microservices for a new project.", client, ctx),
    };

    WorkflowSupervisor supervisor;
    supervisor.initialize(wf, bodies);

    WorkflowResult r = drive(supervisor.run_workflow(
        RunWorkflow{text_message("Topic: should we use microservices for a new project?")}));
    check(r.status != workflow_status::invalid, "the workflow run completes");
    check(r.status == workflow_status::bound_max_rounds,
          "the caller's round bound is the termination contract, same as the offline example");
    std::printf("\n--- transcript --------------------------------------------\n%s\n",
                text_of(r.output).c_str());

    std::fprintf(stderr, g_failures == 0 ? "example_16_group_chat_live: OK\n"
                                          : "example_16_group_chat_live: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
