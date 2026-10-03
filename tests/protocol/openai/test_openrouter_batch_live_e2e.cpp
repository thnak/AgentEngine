// decisions/ADR-235-batch-inference-coalescing.md claim C15, live: a real two-node fan-out round sent to
// OpenRouter's Batch API as ONE job, polled to completion through WorkflowSupervisor::poll_batches(), and folded
// into a completed run -- the whole engine path against the real vendor, nothing scripted.
//
// Environment:
//   AGENTENGINE_OPENROUTER_BATCH_API_KEY   required -- unset means SKIP (exit 0). A dedicated variable, not the
//                                          live-network family's AGENTENGINE_OPENROUTER_API_KEY, which may point
//                                          at another OpenAI-compatible endpoint with no batch API at all.
//   AGENTENGINE_OPENROUTER_BATCH_MODEL     optional -- default `google/gemini-2.5-flash-lite` (accepted by the GA
//                                          `/api/v1/batches` on 2026-10-02). Batch support is per model AND per
//                                          account: `openai/gpt-4o-mini` is listed with a `:batch` variant yet the
//                                          same submit was refused 400 "does not have a :batch endpoint" for this
//                                          key on that date -- the engine fell back to synchronous calls, as built.
//   AGENTENGINE_OPENROUTER_BATCH_WAIT_S    optional -- poll budget in seconds, default 1200.
//
// Verdicts: PASS when the run completes with both nodes' outputs from the vendor. A batch still pending when the
// budget runs out is INCONCLUSIVE (exit 0 with that word printed), never a pass -- the vendor's window is 24h.
// Asserts on structure only, never on model content.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/protocol/openai/openrouter_batch_backend.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::rt::BatchableModelCall;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::PollBatches;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

[[nodiscard]] Message user_text(std::string text) {
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m;
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += " | ";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor fn(char const* id, bool batch) {
    Executor e{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
               .worktree_mode = sharing_mode::shared, .capability_ceiling = {}};
    e.batch = batch;
    return e;
}

// Forwards to the real backend and prints every vendor error, so a live failure names its cause.
class LoggingBackend final : public BatchBackend {
public:
    explicit LoggingBackend(std::shared_ptr<BatchBackend> inner) : inner_(std::move(inner)) {}
    [[nodiscard]] std::string group_key() const override { return inner_->group_key(); }
    [[nodiscard]] BatchLimits limits() const override { return inner_->limits(); }
    [[nodiscard]] result<std::size_t> admit(ChatRequest const& r) const override { return log("admit", inner_->admit(r)); }
    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& ctx) override {
        return log("submit", inner_->submit(items, ctx));
    }
    [[nodiscard]] result<BatchPoll> poll(std::string const& job, EffectContext& ctx) override {
        auto p = log("poll", inner_->poll(job, ctx));
        if (p) {
            for (auto const& item : p->items) {
                std::fprintf(stderr, "  item %s: %s %s\n", item.custom_id.c_str(),
                             batch_item_status_tag(item.status), item.detail.c_str());
            }
        }
        return p;
    }
    [[nodiscard]] result<void> cancel(std::string const& job, EffectContext& ctx) override {
        return log("cancel", inner_->cancel(job, ctx));
    }
    [[nodiscard]] result<void> release(std::string const& job, EffectContext& ctx) override {
        return log("release", inner_->release(job, ctx));
    }

private:
    template <class R>
    static R log(char const* what, R r) {
        if (!r) std::fprintf(stderr, "  backend %s failed: %s (%s)\n", what, r.error().message.c_str(), r.error().code.c_str());
        return r;
    }
    std::shared_ptr<BatchBackend> inner_;
};

}  // namespace

int main() {
    auto const key = pal::env_var("AGENTENGINE_OPENROUTER_BATCH_API_KEY");
    if (!key || key->empty()) {
        std::fprintf(stderr, "test_openrouter_batch_live_e2e: SKIPPED -- AGENTENGINE_OPENROUTER_BATCH_API_KEY is not set.\n");
        return 0;
    }
    std::string const model = env_or("AGENTENGINE_OPENROUTER_BATCH_MODEL", "google/gemini-2.5-flash-lite");
    int const wait_s        = std::stoi(env_or("AGENTENGINE_OPENROUTER_BATCH_WAIT_S", "1200"));
    std::fprintf(stderr, "test_openrouter_batch_live_e2e: model=%s budget=%ds\n", model.c_str(), wait_s);

    InMemorySecretStore store;
    store.set("openrouter", *key);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"openrouter", std::chrono::seconds{0}}});
    EffectContext node_ctx;
    node_ctx.principal    = Principal{"batch-live", ""};
    node_ctx.capabilities = borrow_capabilities(held);

    auto backend = std::make_shared<LoggingBackend>(std::make_shared<openai::OpenRouterBatchBackend<InMemorySecretStore>>(
        "openrouter.ai", std::uint16_t{443}, model, SecretRef{"openrouter"}, store));
    auto sync_client = std::make_shared<openai::OpenAIChatClient<InMemorySecretStore>>(
        "openrouter.ai", std::uint16_t{443}, model, SecretRef{"openrouter"}, ChatClientCapabilities{}, store, "/api/v1");
    auto sync_calls = std::make_shared<int>(0);

    auto node = [&](std::string question) {
        return BatchableModelCall{
            [question](Message const&, EffectContext&) -> result<ChatRequest> {
                ChatRequest r;
                r.messages.push_back(user_text(question + " Answer in at most five words."));
                return r;
            },
            [sync_client, sync_calls](ChatRequest const& req, EffectContext& ctx) -> result<ChatResponse> {
                ++*sync_calls;
                auto t = sync_client->chat(req, ctx);
                while (!t.done()) t.resume();
                auto resp = t.take_value();
                if (!resp) std::fprintf(stderr, "  sync call failed: %s\n", resp.error().message.c_str());
                return resp;
            },
            [](Message const&, ChatResponse const& resp) -> result<ExecutorOutcome> {
                return ExecutorOutcome{resp.message};
            },
            backend};
    };

    Workflow wf;
    wf.id        = "batch-live";
    wf.executors = {fn("start", false), fn("capital", true), fn("color", true), fn("join", false)};
    wf.edges.push_back(Edge{"start", "capital", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"start", "color", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"capital", "join", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"color", "join", edge_kind::fan_in, {}});
    wf.start = "start";
    wf.output_selection.push_back("join");
    wf.bound.max_rounds = 8;

    ExecutorBody passthrough = [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{in};
    };
    WorkflowSupervisor sup;
    sup.initialize(wf,
                   {passthrough, node("What is the capital of France?"), node("What color is a clear daytime sky?"),
                    passthrough},
                   {node_ctx, node_ctx, node_ctx, node_ctx});
    check(sup.enable_batch_coalescing().has_value(), "C15 setup: opt-in accepted");

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{user_text("go")}));
    check(r.status == workflow_status::suspended, "C15: the round suspends on a real OpenRouter batch job");
    check(r.pending_batches.size() == 1 && r.pending_batches[0].item_count == 2,
          "C15: one job carrying both nodes");
    if (r.status != workflow_status::suspended) {
        std::fprintf(stderr, "  status=%s failed_executor=%s sync_calls=%d\n", rt::workflow_status_tag(r.status),
                     r.failed_executor.c_str(), *sync_calls);
        return 1;
    }
    std::fprintf(stderr, "  submitted job %s\n", r.pending_batches[0].job_id.c_str());

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    std::chrono::seconds backoff{5};
    while (r.status == workflow_status::suspended && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::seconds{30});
        r = drive(sup.poll_batches(PollBatches{}));
        std::fprintf(stderr, "  poll: status=%s poll_errors=%u\n", rt::workflow_status_tag(r.status),
                     r.batch_poll_errors);
    }
    if (r.status == workflow_status::suspended) {
        std::fprintf(stderr, "test_openrouter_batch_live_e2e: INCONCLUSIVE -- the job was still pending after %ds "
                             "(vendor window is 24h)\n", wait_s);
        return g_failures == 0 ? 0 : 1;
    }
    check(r.status == workflow_status::completed, std::string("C15: the run completes (status=") +
                                                     rt::workflow_status_tag(r.status) + ")");
    check(*sync_calls == 0, "C15: no synchronous call was made -- both answers came from the batch");
    std::string const out = text_of(r.output);
    std::fprintf(stderr, "  output: %s\n", out.c_str());
    check(r.output.content.size() == 2, "C15: the join received both nodes' answers");
    check(sup.usage().output_tokens > 0, "C15: the vendor's usage was counted");

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}
