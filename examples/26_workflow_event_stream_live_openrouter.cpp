// AgentEngine "get started" examples, 26 -- ADR-152's `agent_turn_event` bridge, against a REAL
// model (OpenRouter), proving real per-token `model_delta` events reach the workflow-level event
// stream live, not just the offline scripted proof in test_rt_workflow_event_stream.cpp's W8/W9 or
// 25_workflow_event_stream_live.cpp's own fully-offline demonstration.
//
// Unlike 19_magentic_builder_live.cpp (which calls `client.chat()` directly from a plain function
// body), this wraps a real, streaming `rt::AgentSession` via `agent_session_as_executor_body()` as
// an `agent`-kind workflow node -- the actual bridge path ADR-152 built
// (EffectContext::agent_turn_sink -> AgentSession::set_run_event_tap() -> emit_run_event_for()).
// `session.set_stream_model_calls(true)` is what makes the session's own model call go through
// `ModelCallGateway::call_stream()`/`chat_stream()` instead of a single buffered `chat()` -- without
// it, per test_session_builder_openrouter_live_e2e.cpp's own documented finding, no `model_delta`
// ever fires at all.
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via
// tools/run-live-provider-tests.ps1, or set it yourself. SKIPS (exit 0), same as every other
// live-network test in this repo, when it's absent.
//
// Run: ./agentengine_example_26_workflow_event_stream_live_openrouter

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::AgentSession;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::NoSessionState;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::agent_session_as_executor_body;
using agentengine::rt::workflow_status;
namespace payload = agentengine::workflow::workflow_event_payload;

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

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;

// A minimal, no-tool history provider -- self-contained per this codebase's own "no shared test
// helper" convention, matching test_rt_workflow_event_stream.cpp's identical W8 fixture.
class NoToolsHistoryProvider {
public:
    agentengine::rt::task<result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    agentengine::rt::task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<NoToolsHistoryProvider>);

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "example_26_workflow_event_stream_live_openrouter: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                     "tools/run-live-provider-tests.ps1, or set the variable yourself, to exercise a "
                     "real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_OPENAI_MODEL", "openai/gpt-4o-mini");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::fprintf(stderr, "example_26_workflow_event_stream_live_openrouter: host=%s model=%s\n",
                 host.c_str(), model.c_str());

    constexpr char const* kSecretName = "openrouter-api-key";
    InMemorySecretStore   store;
    store.set(kSecretName, *key_env);
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 200;

    static_assert(ChatClient<RealClient>, "OpenAIChatClient must satisfy the ChatClient concept");

    AgentSession<RealClient, NoSessionState, NoToolsHistoryProvider> session;
    session.emplace_chat_client(host, /*port=*/static_cast<std::uint16_t>(443), model,
                                 SecretRef{kSecretName}, caps, store, std::string("/api/v1"));
    session.set_stream_model_calls(true);  // required -- see this file's own top comment

    Workflow wf;
    wf.id        = "event-stream-live-openrouter-demo";
    wf.executors = {node_desc("agent1", executor_kind::agent)};
    wf.start = "agent1";
    wf.output_selection.push_back("agent1");
    wf.bound.max_rounds = 4;
    check(validate_workflow(wf).has_value(), "the single-agent-node graph validates");

    std::vector<ExecutorBody> bodies = {agent_session_as_executor_body(session)};

    // GitHub issue #67: GCC applies `-Wmissing-field-initializers` to a DESIGNATED initializer that
    // names only some members; clang does not, which is why this built on Windows and broke the
    // Linux build. Every other `EffectContext` member wants exactly its default here, so the two
    // this example actually cares about are set by assignment -- no member list to fall out of date
    // as the struct grows, which is the same reason nothing else in the tree designated-initializes
    // this type.
    EffectContext wf_ctx{};
    wf_ctx.principal    = Principal{"example-26-principal", ""};
    wf_ctx.capabilities = borrow_capabilities(held);

    std::vector<EffectContext> contexts;
    contexts.push_back(std::move(wf_ctx));

    WorkflowSupervisor sup;
    sup.initialize(wf, bodies, std::move(contexts));
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    agentengine::rt::task<WorkflowResult> run = sup.run_workflow(
        RunWorkflow{text_message("In one short sentence, what is quantum entanglement?")});

    // Polled between resumes, same discipline 25_workflow_event_stream_live.cpp's own comment
    // explains -- the events themselves are pushed the instant the session's own run_model_call()
    // emits them, from whatever thread is running this ThreadPool job.
    std::size_t model_deltas_seen_live = 0;
    std::string reconstructed_text;
    while (!run.done()) {
        run.resume();
        while (std::optional<WorkflowEvent> ev = stream.next()) {
            if (ev->kind != workflow_event_kind::agent_turn_event) continue;
            auto const* p = std::get_if<payload::AgentTurn>(&ev->payload);
            if (p == nullptr) continue;
            std::printf("[agent_turn_event] inner_kind=%d\n", static_cast<int>(p->inner.kind));
            if (p->inner.kind != run_event_kind::model_delta) continue;
            ++model_deltas_seen_live;
            if (auto const* d = std::get_if<run_event_payload::ModelDelta>(&p->inner.payload)) {
                if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d->value)) {
                    std::printf("  delta: %s\n", t->text.c_str());
                    reconstructed_text += t->text;
                }
            }
        }
    }
    WorkflowResult r = run.take_value();
    while (std::optional<WorkflowEvent> ev = stream.next()) {
        (void)ev;  // any events queued between the last poll above and run completing
    }

    check(r.status != workflow_status::invalid, "the workflow run returns");
    std::printf("\n--- status: %d, rounds: %u ------------------------------\n",
                static_cast<int>(r.status), r.rounds);
    std::printf("%s\n", text_of(r.output).c_str());

    // Structural-only assertions on a live model's nondeterministic output (matching this repo's own
    // live-test convention, e.g. test_session_builder_openrouter_live_e2e.cpp) -- a positive control
    // proving the credential and the bridge are genuinely load-bearing, not asserting on content.
    check(r.status == workflow_status::completed, "the live run completes");
    check(model_deltas_seen_live > 0,
          "at least one REAL model_delta reached the workflow-level agent_turn_event bridge, live, "
          "during the run -- not reconstructed after the fact");
    check(!reconstructed_text.empty(),
          "the streamed deltas reassemble into real, non-empty text -- the bridge carries the "
          "model's actual output, not an empty/stub event");
    check(!text_of(r.output).empty(), "the workflow's own final output is also non-empty");

    std::fprintf(stderr,
                 g_failures == 0 ? "example_26_workflow_event_stream_live_openrouter: OK\n"
                                 : "example_26_workflow_event_stream_live_openrouter: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
