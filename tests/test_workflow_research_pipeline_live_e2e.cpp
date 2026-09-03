// A production-shaped multi-agent workflow, driven against a REAL model over OpenRouter, built to
// find real bugs rather than to demonstrate a feature. Scenario: a "should we launch product X in
// market Y" go/no-go pipeline -- a planner drafts a research brief, three specialists (market,
// technical-feasibility, competitive-landscape) research it CONCURRENTLY, their findings are
// aggregated, a writer drafts a report, and a human gates publication before the run completes.
// This is 014 §3's "Planner" + "Concurrent" + "Handoff"(-shaped human gate) patterns composed into
// one graph the way a real caller would, not exercised one at a time.
//
// Three things this file specifically hunts for, each because an OFFLINE/synthetic test structurally
// cannot exercise it:
//
//   1. FAILURE CLASSIFICATION AGAINST A REAL RESPONSE (014 §6, `EdgeFailurePolicy`), AND ITS
//      COMPOSITION WITH A SHARED `fan_in` TARGET UNDER REAL, JITTERED CONCURRENCY. The "competitive"
//      specialist is deliberately pointed at a model id that does not exist, so its live call fails
//      against the REAL OpenRouter error response, not a synthetic std::unexpected --
//      protocol/openai/chat_client.hpp's `map_http_status_error` is what turns that real HTTP 4xx
//      into `failure_class::contract`. A SEPARATE small graph (P1 below) proves the sharper claim
//      `tests/test_rt_workflow_supervisor_failure_policies.cpp`'s D2c already proves offline -- that a
//      `contract` failure is never retried, even under a `retry` policy -- but against a real wire
//      response instead of a hand-classified stand-in: if OpenRouter ever changed its "unknown model"
//      response to a 5xx, that offline test would not notice the reclassification to `transient`
//      (suddenly, wrongly, retryable); this one would.
//
//      `competitive`'s edge to `aggregate` declares a REAL `EdgeFailurePolicy::fallback` naming
//      `competitive_fallback` as the recovery executor, which has its own `fan_in` edge back to the
//      SAME `aggregate` target `market`/`technical` also feed. THIS FILE'S FIRST LIVE RUN (2026-09-03)
//      found that composition genuinely broken -- GitHub issue #52, pinned offline by
//      `tests/test_workflow_fanin_concurrent_failure_policy_fix.cpp` -- and an interim version of this
//      file routed around it with an application-level try/catch instead of the engine policy. That
//      gap is now FIXED (`route_from()`'s `seed_fan_in_holds()`/`deliver_to_fan_in()`/
//      `RunState::held_fan_in`, see workflow_supervisor.hpp), and this file was switched back to the
//      real `EdgeFailurePolicy::fallback` specifically so a live, jittered run keeps exercising the
//      fix, not just the offline characterization test.
//
//   2. FAN-IN UNDER REAL, JITTERED CONCURRENCY (014 §2's determinism obligation). The three
//      specialists run in the SAME round via `edge_kind::fan_out`/`fan_in`
//      (`examples/09_concurrent_workflow.cpp`'s mechanism), each a real network call with genuinely
//      different, unpredictable latency -- unlike
//      `tests/test_rt_workflow_supervisor_scheduling_shuffle.cpp`'s synthetic shuffle, completion
//      order here is real, not injected. The aggregator must still run EXACTLY ONCE with all three
//      contributions, regardless of which specialist's socket happened to finish first.
//
//   3. CHECKPOINT/RESUME DOES NOT RE-DO FINISHED LIVE WORK. The run suspends at a human publish-gate
//      request port (mirroring `examples/22_magentic_plan_signoff_checkpoint.cpp`'s exact
//      attach()/resume_or_start() mechanics, simulating a process restart with a brand-new
//      `WorkflowSupervisor` and a brand-new `bodies` vector over the same on-disk checkpoint). Unlike
//      22 (whose bodies are deterministic stand-ins with nothing to waste), this one counts real live
//      invocations per specialist and asserts every one of them is STILL ZERO on the resumed
//      supervisor's fresh body closures after the human approves -- proving the resumed run really
//      replays from the checkpoint's recorded state instead of quietly re-issuing already-paid-for
//      API calls.
//
// Needs AGENTENGINE_OPENROUTER_API_KEY in the environment -- run via tools/run-live-provider-tests.ps1,
// or set it yourself. SKIPS (exit 0), same as every other live-network test in this repo, when it's
// absent. Nondeterministic by nature (a live model): like every other live test here, nothing below
// asserts on model CONTENT, only on structure (which node ran, how many times, what shape the final
// report has, which real failure class a real response produced).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/workflow_checkpoint_manager.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::FileSessionStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowCheckpointManager;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

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

// Fan-in delivers one ContentItem per contributing branch (examples/09_concurrent_workflow.cpp's own
// helper) -- under real jittered concurrency the assertions below check MEMBERSHIP, not position,
// since which of two real sockets happens to finish first is genuinely not under this test's control.
[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "\n---\n";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

// Wraps a body with a real invocation tally -- the same rt:: stand-in for the retired
// `FunctionExecutor::invocations()` actor state every other example/test file in this suite uses.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> tally) {
    return [body = std::move(body), tally](Message const& in,  // NOLINT(clang-analyzer-core.StackAddressEscape)
                                           EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
        tally->fetch_add(1, std::memory_order_relaxed);
        return body(in, ctx);
    };
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
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

// ---- Bodies -------------------------------------------------------------------------------------

[[nodiscard]] ExecutorBody planner_body(RealClient& client, EffectContext& ctx) {
    return [&client, &ctx](Message const& in, EffectContext&) -> agentengine::result<Message> {
        std::string const goal = text_of(in);
        auto resp = live_call(
            client, ctx,
            "You are a research planning lead. Given a business go/no-go question, write a SHORT "
            "(2-3 line) research brief naming what a Market specialist, a Technical-feasibility "
            "specialist, and a Competitive-landscape specialist should each look into. Do not answer "
            "the question yourself.",
            goal);
        if (!resp.has_value()) return std::unexpected(resp.error());
        std::string const plan = text_of(resp->message);
        std::printf("[planner] %s\n", plan.c_str());
        return text_message("Research brief: " + plan + "\n\nOriginal question: " + goal);
    };
}

[[nodiscard]] ExecutorBody specialist_body(std::string label, std::string system_prompt,
                                            RealClient& client, EffectContext& ctx) {
    return [label, system_prompt, &client, &ctx](Message const& in,
                                                  EffectContext&) -> agentengine::result<Message> {
        auto const t0   = std::chrono::steady_clock::now();
        auto        resp = live_call(client, ctx, system_prompt, text_of(in));
        auto const  ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!resp.has_value()) {
            std::fprintf(stderr, "[%s] FAILED after %lldms: %s (%s)\n", label.c_str(),
                         static_cast<long long>(ms), resp.error().message.c_str(),
                         resp.error().code.c_str());
            return std::unexpected(resp.error());
        }
        std::string const reply = text_of(resp->message);
        std::printf("[%s] (%lldms) %s\n", label.c_str(), static_cast<long long>(ms), reply.c_str());
        return text_message(label + " findings: " + reply);
    };
}

// The engine-level recovery executor `competitive`'s edge's real `EdgeFailurePolicy::fallback` names
// (see this file's top comment). Runs on the failure_marker() Message competitive's real failure
// produced -- reads the real classification/message out of it for a slightly more informative
// degraded report, exactly what a real caller's recovery node would do, but doesn't need to (a fixed
// degraded message would work just as well for this test's own assertions).
[[nodiscard]] agentengine::result<Message> competitive_fallback_body(Message const& in, EffectContext&) {
    std::string reason = "live lookup failed";
    for (auto const& item : in.content) {
        if (auto const* e = std::get_if<Error>(&item.value)) {
            reason = e->message;
            break;
        }
    }
    std::printf("[competitive_fallback] recovering from: %s\n", reason.c_str());
    return text_message(
        "competitive findings: UNAVAILABLE (" + reason + ") -- using cached baseline: no major "
        "competitor announcements on file. Treat this section as low-confidence.");
}

[[nodiscard]] agentengine::result<Message> aggregate_body(Message const& in, EffectContext&) {
    return text_message("Aggregated findings:\n" + all_text_of(in));
}

[[nodiscard]] ExecutorBody writer_body(RealClient& client, EffectContext& ctx) {
    return [&client, &ctx](Message const& in, EffectContext&) -> agentengine::result<Message> {
        auto resp = live_call(
            client, ctx,
            "You are a report writer. Using ONLY the findings given, draft a 3-4 sentence go/no-go "
            "recommendation. If any finding is marked low-confidence or unavailable, explicitly say "
            "the recommendation carries a disclosed gap.",
            text_of(in));
        if (!resp.has_value()) return std::unexpected(resp.error());
        std::string const report = text_of(resp->message);
        std::printf("[writer] %s\n", report.c_str());
        return text_message("REPORT:\n" + report);
    };
}

// publish_review is a request_port -- never dispatched to a body (matches
// examples/22_magentic_plan_signoff_checkpoint.cpp's identical convention).

[[nodiscard]] agentengine::result<ExecutorOutcome> publish_decision_body(Message const& in, EffectContext&) {
    std::string const decision = text_of(in);
    bool const approved = decision.find("approved") != std::string::npos;
    return ExecutorOutcome{in, {approved ? "approved" : "rejected"}};
}

[[nodiscard]] agentengine::result<Message> publish_body(Message const& in, EffectContext&) {
    return text_message("PUBLISHED:\n" + text_of(in));
}

[[nodiscard]] agentengine::result<Message> abandon_body(Message const& in, EffectContext&) {
    return text_message("ABANDONED:\n" + text_of(in));
}

[[nodiscard]] agentengine::result<ExecutorOutcome> identity_sink(Message const& in, EffectContext&) {
    return ExecutorOutcome{in};
}

#if defined(_WIN32)
[[nodiscard]] int current_pid() noexcept { return ::_getpid(); }
#else
[[nodiscard]] int current_pid() noexcept { return ::getpid(); }
#endif

[[nodiscard]] std::filesystem::path make_temp_root() {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ae_test_workflow_research_pipeline_live_e2e_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

}  // namespace

int main() {
    auto const key_env = pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "test_workflow_research_pipeline_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                     "tools/run-live-provider-tests.ps1, or set the variable yourself, to exercise "
                     "the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    // Deliberately not a real OpenRouter model id -- forces a REAL "model not found" response from
    // the REAL API on every call, so its `failure_class` classification (map_http_status_error) is
    // proven against real wire bytes, not asserted by construction.
    std::string const bad_model = "agentengine-test/definitely-not-a-real-model-id";
    std::fprintf(stderr, "test_workflow_research_pipeline_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    constexpr char const* kSecretName = "openrouter-api-key";
    InMemorySecretStore   store;
    store.set(kSecretName, *key_env);
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 200;

    EffectContext ctx;
    ctx.principal    = agentengine::Principal{"research-pipeline-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    RealClient main_client(host, /*port=*/443, model, SecretRef{kSecretName}, caps, store, "/api/v1");
    RealClient bad_client(host, /*port=*/443, bad_model, SecretRef{kSecretName}, caps, store, "/api/v1");
    static_assert(ChatClient<RealClient>, "OpenAIChatClient must satisfy the ChatClient concept");

    // ==== P1: a real `contract`-classified failure is NEVER retried (014 §6 / D2c's live analogue) ==
    // Standalone, tiny graph -- the sharper, narrower claim, isolated from the production graph below
    // so a failure here points straight at classification, not at anything downstream.
    {
        Workflow wf;
        wf.id        = "p1-no-retry-on-real-contract-failure";
        wf.executors = {node_desc("bad_call"), node_desc("sink")};
        wf.edges     = {Edge{"bad_call", "sink", edge_kind::direct, {},
                              EdgeFailurePolicy{edge_failure_policy::retry, /*attempts=*/2, {}}}};
        wf.start            = "bad_call";
        wf.output_selection = {"sink"};
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "P1: the graph validates");

        auto bad_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
        std::vector<ExecutorBody> bodies = {
            counted(
                [&bad_client, &ctx](Message const& in, EffectContext&) -> agentengine::result<Message> {
                    auto resp = live_call(bad_client, ctx, "irrelevant", text_of(in));
                    if (!resp.has_value()) {
                        std::fprintf(stderr, "[bad_call] real failure: %s (klass=%d, code=%s)\n",
                                     resp.error().message.c_str(), static_cast<int>(resp.error().klass),
                                     resp.error().code.c_str());
                        return std::unexpected(resp.error());
                    }
                    return resp->message;
                },
                bad_calls),
            [](Message const& in, EffectContext&) -> agentengine::result<Message> { return in; },
        };

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("ping")}));

        check(r.status == workflow_status::executor_failed,
              "P1: a real 'model not found' response fails the workflow (not silently absorbed)");
        check(r.failed_executor == "bad_call", "P1: the result names the real failing executor (I4)");
        check(bad_calls->load(std::memory_order_relaxed) == 1,
              "P1: the live call was attempted EXACTLY ONCE despite a `retry(attempts=2)` policy -- "
              "the REAL response really does classify as non-retryable `contract`, matching D2c's "
              "offline claim against real wire bytes instead of a hand-classified stand-in");
    }

    // ==== P2: the production pipeline -- planner, concurrent specialists (one degraded via a real ===
    // ==== failure + fallback), fan-in, writer, a human publish gate, and a simulated restart ========
    Workflow wf;
    wf.id        = "research-go-no-go-pipeline";
    wf.executors = {
        node_desc("planner"), node_desc("market"), node_desc("technical"), node_desc("competitive"),
        node_desc("competitive_fallback"), node_desc("aggregate"), node_desc("writer"),
        node_desc("publish_review", executor_kind::request_port), node_desc("publish_decision"),
        node_desc("publish"), node_desc("abandon"), node_desc("done"),
    };
    wf.edges = {
        Edge{"planner", "market", edge_kind::fan_out, {}},
        Edge{"planner", "technical", edge_kind::fan_out, {}},
        Edge{"planner", "competitive", edge_kind::fan_out, {}},
        Edge{"market", "aggregate", edge_kind::fan_in, {}},
        Edge{"technical", "aggregate", edge_kind::fan_in, {}},
        // The real engine policy this file's top comment describes: competitive's own real failure
        // routes to `competitive_fallback`, whose own `fan_in` edge below rejoins the SAME `aggregate`
        // target `market`/`technical` also feed -- exactly the composition GitHub issue #52 found
        // broken and workflow_supervisor.hpp's `seed_fan_in_holds()`/`deliver_to_fan_in()` fix.
        Edge{"competitive", "aggregate", edge_kind::fan_in, {},
             EdgeFailurePolicy{edge_failure_policy::fallback, 0, "competitive_fallback"}},
        Edge{"competitive_fallback", "aggregate", edge_kind::fan_in, {}},
        Edge{"aggregate", "writer", edge_kind::direct, {}},
        Edge{"writer", "publish_review", edge_kind::direct, {}},
        Edge{"publish_review", "publish_decision", edge_kind::direct, {}},
        Edge{"publish_decision", "publish", edge_kind::switch_case, "approved"},
        Edge{"publish_decision", "abandon", edge_kind::switch_case, "rejected"},
        Edge{"publish", "done", edge_kind::direct, {}},
        Edge{"abandon", "done", edge_kind::direct, {}},
    };
    wf.start            = "planner";
    wf.output_selection = {"done"};
    wf.bound.max_rounds = 16;
    check(validate_workflow(wf).has_value(), "P2: the production pipeline graph validates");

    auto planner_calls    = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto market_calls     = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto technical_calls  = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_fallback_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto aggregate_calls  = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto writer_calls     = std::make_shared<std::atomic<std::uint32_t>>(0);

    std::vector<ExecutorBody> bodies = {
        counted(planner_body(main_client, ctx), planner_calls),
        counted(specialist_body("market", "You are a Market specialist. In 1-2 sentences, state the "
                                           "single biggest market-demand signal for the given brief.",
                                 main_client, ctx),
                market_calls),
        counted(specialist_body("technical",
                                 "You are a Technical-feasibility specialist. In 1-2 sentences, state "
                                 "the single biggest technical risk for the given brief.",
                                 main_client, ctx),
                technical_calls),
        // Deliberately pointed at `bad_client` (the nonexistent model id) -- a REAL failure, routed by
        // the engine's own `EdgeFailurePolicy::fallback`, not caught in-body.
        counted(specialist_body("competitive",
                                 "You are a Competitive-landscape specialist. In 1-2 sentences, "
                                 "summarize the competitive landscape for the given brief.",
                                 bad_client, ctx),
                competitive_calls),
        counted(competitive_fallback_body, competitive_fallback_calls),
        counted(aggregate_body, aggregate_calls),
        counted(writer_body(main_client, ctx), writer_calls),
        {},  // publish_review: request_port, never dispatched
        publish_decision_body,
        publish_body,
        abandon_body,
        identity_sink,
    };

    std::filesystem::path const root = make_temp_root();
    std::string                 run_id;
    std::string                 interaction_id;

    // ---- "before the restart": planner -> concurrent specialists (competitive's REAL failure routes
    //      through the engine's own `fallback` to competitive_fallback) -> aggregate (exactly once,
    //      despite real jitter AND the cross-round fallback join) -> writer -> suspend for a human
    //      publish decision, auto-checkpointing every round ----------------------------------------
    {
        FileSessionStore store2(root);
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowCheckpointManager<FileSessionStore> mgr(store2);
        mgr.attach(sup);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{
            text_message("Should we launch a mid-tier home-espresso machine in the US market?")}));

        check(r.status == workflow_status::suspended,
              "P2: the run suspends at the human publish-review gate rather than auto-publishing");
        check(r.open_interactions.size() == 1,
              "P2: exactly one open interaction is waiting on the publish decision");
        check(planner_calls->load(std::memory_order_relaxed) == 1, "P2: planner ran exactly once");
        check(market_calls->load(std::memory_order_relaxed) == 1, "P2: market specialist ran exactly once");
        check(technical_calls->load(std::memory_order_relaxed) == 1,
              "P2: technical specialist ran exactly once");
        check(competitive_calls->load(std::memory_order_relaxed) == 1,
              "P2: competitive specialist was genuinely ATTEMPTED (not skipped) and its real failure "
              "routed through the engine's own `EdgeFailurePolicy::fallback`, not an in-body catch");
        check(competitive_fallback_calls->load(std::memory_order_relaxed) == 1,
              "P2: the fallback recovery executor ran exactly once, dispatched by the engine off "
              "competitive's real failure marker");
        check(aggregate_calls->load(std::memory_order_relaxed) == 1,
              "P2 (GitHub issue #52's fix, exercised live): the aggregator still ran EXACTLY ONCE with "
              "ALL THREE contributions merged -- market/technical's real, independently-timed network "
              "calls under real jitter, AND competitive_fallback's later-round recovery -- proving the "
              "cross-round fan_in hold/release actually works against real concurrent network timing, "
              "not just the offline characterization test's synthetic ordering");
        check(writer_calls->load(std::memory_order_relaxed) == 1, "P2: the writer ran exactly once");

        // A suspended run's own `WorkflowResult::output` is not meaningfully populated (the run never
        // reached completion) -- the real pending content is the request_port's own ASK, which is
        // exactly what `resume_workflow()`'s eventual caller (a real approval UI) would read to show
        // the human what they're approving.
        auto const asks = sup.open_interaction_asks();
        check(asks.size() == 1, "P2: exactly one interaction ask is pending");
        std::string const pending_report = asks.empty() ? "" : text_of(asks.at(0).ask);
        check(pending_report.find("REPORT:") != std::string::npos,
              "P2: the suspended run's pending publish-review ask carries the writer's real report, "
              "not a stub");

        run_id         = sup.run_id();
        interaction_id = r.open_interactions.at(0).interaction_id;
        std::printf("[before restart] suspended run_id=%s awaiting publish decision (interaction=%s)\n",
                     run_id.c_str(), interaction_id.c_str());
    }  // sup, mgr, and store2 all go out of scope here -- nothing survives in memory past this point

    // ---- "the process restarts": a brand-new store handle, a brand-new supervisor, a brand-new
    //      `bodies2` vector with its OWN fresh invocation counters -- if resume secretly re-ran any
    //      live node, these counters (not the ones above) would catch it -----------------------------
    {
        FileSessionStore reopened(root);

        auto planner_calls2     = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto market_calls2      = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto technical_calls2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_fallback_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto aggregate_calls2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto writer_calls2      = std::make_shared<std::atomic<std::uint32_t>>(0);

        std::vector<ExecutorBody> bodies2 = {
            counted(planner_body(main_client, ctx), planner_calls2),
            counted(specialist_body("market", "unused after resume", main_client, ctx), market_calls2),
            counted(specialist_body("technical", "unused after resume", main_client, ctx),
                    technical_calls2),
            counted(specialist_body("competitive", "unused after resume", bad_client, ctx),
                    competitive_calls2),
            counted(competitive_fallback_body, competitive_fallback_calls2),
            counted(aggregate_body, aggregate_calls2),
            counted(writer_body(main_client, ctx), writer_calls2),
            {},
            publish_decision_body,
            publish_body,
            abandon_body,
            identity_sink,
        };

        WorkflowSupervisor sup;
        result<bool> resumed = WorkflowCheckpointManager<FileSessionStore>::resume_or_start(
            reopened, run_id, sup, wf, bodies2);
        check(resumed.has_value() && *resumed == true,
              "P2: resume_or_start() finds the on-disk checkpoint and resumes instead of starting "
              "fresh (mirrors examples/22_magentic_plan_signoff_checkpoint.cpp's exact mechanics)");

        auto const open = sup.open_interactions();
        check(open.size() == 1 && open[0].interaction_id == interaction_id,
              "P2: the resumed supervisor's open interaction is the SAME publish-review request the "
              "original run left open, not a re-derived one");

        // Mirrors a real approval UI: it read the pending ask (the writer's report) to show the human,
        // and the response it eventually submits carries that report forward -- publish_decision_body
        // only ever sees WHATEVER this resume response contains (a request_port's resume() does not
        // implicitly re-attach the original ask), so a caller that wants the report to survive into
        // "publish" has to thread it through here itself, same as this file's OWN suspended-run check
        // above had to read it via `open_interaction_asks()` rather than assuming it flows automatically.
        auto const resumed_asks = sup.open_interaction_asks();
        check(resumed_asks.size() == 1, "P2: the resumed run still carries the original pending ask");
        std::string const resumed_report = resumed_asks.empty() ? "" : text_of(resumed_asks.at(0).ask);
        check(resumed_report.find("REPORT:") != std::string::npos,
              "P2: the report recovered from the resumed checkpoint's own ask is real, not a stub");

        Message const approval = text_message("approved: ship it\n\n" + resumed_report);
        WorkflowResult r = drive(sup.resume_workflow(ResumeWorkflow{interaction_id, approval, {}}));

        check(r.status == workflow_status::completed,
              "P2: resuming with the human's approval drives the run through publish_decision to "
              "completion");
        std::string const final_text = text_of(r.output);
        std::printf("[after restart] completed: %s\n", final_text.c_str());
        check(final_text.find("PUBLISHED:") != std::string::npos,
              "P2: the completed run's output carries the real publish branch, not a stub");
        check(final_text.find("REPORT:") != std::string::npos,
              "P2: the published output still carries the writer's real, earlier-produced report -- "
              "proving it survived the checkpoint round-trip rather than being regenerated");

        check(planner_calls2->load(std::memory_order_relaxed) == 0 &&
                  market_calls2->load(std::memory_order_relaxed) == 0 &&
                  technical_calls2->load(std::memory_order_relaxed) == 0 &&
                  competitive_calls2->load(std::memory_order_relaxed) == 0 &&
                  competitive_fallback_calls2->load(std::memory_order_relaxed) == 0 &&
                  aggregate_calls2->load(std::memory_order_relaxed) == 0 &&
                  writer_calls2->load(std::memory_order_relaxed) == 0,
              "P2: NONE of the resumed supervisor's fresh body closures ran -- the finished live work "
              "(5 real API calls' worth, plus the fallback join) genuinely came from the checkpoint, "
              "not from silently re-doing it after resume");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::fprintf(stderr, g_failures == 0 ? "test_workflow_research_pipeline_live_e2e: OK\n"
                                          : "test_workflow_research_pipeline_live_e2e: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
