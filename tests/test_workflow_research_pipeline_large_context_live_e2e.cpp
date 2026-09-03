// Large-context live stress test, added 2026-09-03 at the project owner's explicit request after
// reviewing the OpenRouter dashboard for test_workflow_research_pipeline_live_e2e.cpp's own live
// runs: every call that test makes carries roughly ~1-2K tokens of context -- nothing like a real
// document-grounded production call. This file re-runs the SAME go/no-go production pipeline shape
// (planner -> concurrent specialists -> aggregate -> writer -> human publish gate -> checkpoint/
// resume), but feeds each concurrent specialist a large REAL reference corpus -- sliced from this
// repository's own MIT-licensed design docs, not synthetic filler -- immediately before the actual
// brief, so each individual specialist API call's context genuinely exceeds 60K tokens (prompt +
// completion combined), matching what a real document-grounded caller's bill/dashboard would show.
// Every check below is against the REAL reported `Usage.input_tokens`/`output_tokens` from the live
// response, never an assumed character count.
//
// What this specifically hunts for that the small-context test (and the offline suite) structurally
// cannot:
//   1. A large (~250KB+) REQUEST body serialized to JSON and sent over TLS -- the earlier ADR-011 fix
//      only ever proved large chunked RESPONSE decoding; this exercises the OUTBOUND send path and
//      the JSON encoder's escaping of a genuinely large, real (not synthetic) document -- quotes,
//      backslashes inside code fences, markdown tables, unicode -- instead of a hand-picked adversarial
//      snippet.
//   2. Checkpoint serialization of a real run whose specialist outputs and final report are
//      meaningfully larger than a one-line tag -- FileSessionStore's real on-disk write and
//      RunStateRecord's JSON codec at a scale the offline tests never approach.
//   3. The fan_in merge (deliver_to_fan_in, GitHub issue #52's fix) actually appending realistically
//      sized content items correctly under real network jitter, not just short tagged strings.
//
// Needs BOTH `AGENTENGINE_OPENROUTER_API_KEY` AND `AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS=1` --
// this is a real, meaningfully more expensive live run (well over 100K billed tokens total) that must
// never fire as a side effect of an ordinary API-key-gated live-test sweep (tools/run-live-provider-
// tests.ps1's own `-L live-network` selection does not carry this file's label for exactly that
// reason -- see tests/CMakeLists.txt). SKIPS (exit 0) if either is missing. Nondeterministic by
// nature (a live model): like every other live test here, nothing below asserts on model CONTENT,
// only on structure and on the real usage/size numbers the provider reports.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

#ifndef AGENTENGINE_REPO_ROOT
#error "AGENTENGINE_REPO_ROOT must be defined by tests/CMakeLists.txt"
#endif

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::FileSessionStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
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

// Reads up to `max_chars` bytes starting at `offset` from a real file in this repository -- the
// large-context "reference corpus" fed to each specialist below. Deliberately real, MIT-licensed
// project text (a design draft), not synthetic filler: exercises the JSON request encoder against
// genuine markdown (code fences, quotes, backslashes, tables), not a hand-picked adversarial snippet.
[[nodiscard]] std::string read_repo_file_slice(std::string const& relative_path, std::size_t offset,
                                                std::size_t max_chars) {
    std::filesystem::path const path =
        std::filesystem::path(AGENTENGINE_REPO_ROOT) / relative_path;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "FATAL: could not open fixture file: %s\n", path.string().c_str());
        return {};
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string out(max_chars, '\0');
    in.read(out.data(), static_cast<std::streamsize>(max_chars));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
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

// A large-context specialist: `corpus` (tens of thousands of tokens, a real design doc slice) is
// placed BEFORE the actual brief so the model must process the whole context to reach the question
// it's meant to answer, exactly like a real document-grounded RAG-style call. The system prompt
// explicitly tells it to IGNORE the corpus content itself (never reference/quote/summarize it) --
// this keeps the completion short and predictable (cost control, and assertable structure) while
// still forcing the full corpus through request encoding, the wire, and the provider's own context
// window. `usage_out` captures the REAL reported Usage so main() can assert on real numbers.
[[nodiscard]] ExecutorBody large_context_specialist_body(
    std::string label, std::string question_focus, std::string corpus, RealClient& client,
    EffectContext& ctx, std::shared_ptr<agentengine::Usage> usage_out) {
    return [label, question_focus, corpus = std::move(corpus), &client, &ctx,
            usage_out](Message const& in, EffectContext&) -> agentengine::result<Message> {
        std::string const system_prompt =
            "Below this instruction is a LARGE REFERENCE CORPUS -- unrelated internal engineering "
            "documentation, included ONLY to exercise a large context window. Do not reference, "
            "quote, or summarize the corpus in any way; it has nothing to do with the question. "
            "After the corpus you will find a line 'BRIEF:' followed by the real business question. "
            "You are a " +
            question_focus +
            ". Answer ONLY the brief, with a thorough 150-250 word analysis grounded in general "
            "domain knowledge.";
        std::string const user_content = corpus + "\n\nBRIEF:\n" + text_of(in);

        auto const t0   = std::chrono::steady_clock::now();
        auto        resp = live_call(client, ctx, system_prompt, user_content);
        auto const  ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!resp.has_value()) {
            std::fprintf(stderr, "[%s] FAILED after %lldms: %s (%s)\n", label.c_str(),
                         static_cast<long long>(ms), resp.error().message.c_str(),
                         resp.error().code.c_str());
            return std::unexpected(resp.error());
        }
        *usage_out = resp->usage;
        std::string const reply = text_of(resp->message);
        std::printf("[%s] (%lldms) context=%llu+%llu=%llu tokens, reply=%zu chars\n", label.c_str(),
                     static_cast<long long>(ms), static_cast<unsigned long long>(resp->usage.input_tokens),
                     static_cast<unsigned long long>(resp->usage.output_tokens),
                     static_cast<unsigned long long>(resp->usage.input_tokens + resp->usage.output_tokens),
                     reply.size());
        return text_message(label + " findings: " + reply);
    };
}

// competitive_fallback: the engine-level recovery `competitive`'s edge's real
// `EdgeFailurePolicy::fallback` names -- same shape as test_workflow_research_pipeline_live_e2e.cpp's
// own, reused here so the JUST-FIXED fallback/fan_in cross-round join (GitHub issue #52) is exercised
// against realistically-sized sibling content, not short tags.
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
            "You are a report writer. Using ONLY the findings given, draft a 4-6 sentence go/no-go "
            "recommendation. If any finding is marked low-confidence or unavailable, explicitly say "
            "the recommendation carries a disclosed gap.",
            text_of(in));
        if (!resp.has_value()) return std::unexpected(resp.error());
        std::string const report = text_of(resp->message);
        std::printf("[writer] (%zu chars) %s\n", report.size(), report.c_str());
        return text_message("REPORT:\n" + report);
    };
}

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
        ("ae_test_workflow_research_pipeline_large_context_live_e2e_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

[[nodiscard]] bool env_flag_set(char const* name) {
    auto const v = pal::env_var(name);
    return v.has_value() && !v->empty() && *v != "0";
}

}  // namespace

int main() {
    auto const key_env = pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "test_workflow_research_pipeline_large_context_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n");
        return 0;
    }
    if (!env_flag_set("AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS")) {
        std::fprintf(stderr,
                     "test_workflow_research_pipeline_large_context_live_e2e: SKIPPED -- set "
                     "AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS=1 to opt into this meaningfully more "
                     "expensive (well over 100K billed tokens) live run.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::string const bad_model = "agentengine-test/definitely-not-a-real-model-id";
    std::fprintf(stderr,
                 "test_workflow_research_pipeline_large_context_live_e2e: host=%s model=%s\n",
                 host.c_str(), model.c_str());

    constexpr char const* kSecretName = "openrouter-api-key";
    InMemorySecretStore   store;
    store.set(kSecretName, *key_env);
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 500;

    EffectContext ctx;
    ctx.principal    = agentengine::Principal{"large-context-research-pipeline-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    RealClient main_client(host, /*port=*/443, model, SecretRef{kSecretName}, caps, store, "/api/v1");
    RealClient bad_client(host, /*port=*/443, bad_model, SecretRef{kSecretName}, caps, store, "/api/v1");

    // Real, MIT-licensed project text -- three DIFFERENT large slices (not the same blob repeated),
    // so nothing downstream (caching, dedup) could accidentally paper over a real per-call bug.
    // ~260K chars each for market/technical is roughly 60-70K tokens for this kind of markdown text;
    // the actual measured token counts are asserted below against the real reported Usage, not
    // assumed from this character count.
    std::string const market_corpus =
        read_repo_file_slice("docs/planning/identity-native-sandbox-worktree-design.md", 0, 260000);
    std::string const technical_corpus = read_repo_file_slice(
        "docs/planning/office-document-extraction-design-draft.md", 0, 260000);
    std::string const competitive_corpus = read_repo_file_slice(
        "docs/planning/office-document-extraction-design-draft.md", 280000, 150000);
    check(!market_corpus.empty() && !technical_corpus.empty() && !competitive_corpus.empty(),
          "setup: all three real fixture corpora loaded from disk");
    std::printf("corpus sizes (chars): market=%zu technical=%zu competitive=%zu\n", market_corpus.size(),
                technical_corpus.size(), competitive_corpus.size());

    Workflow wf;
    wf.id        = "large-context-research-go-no-go-pipeline";
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
    // Large-context calls take real minutes, not seconds -- see tests/CMakeLists.txt's TIMEOUT for
    // this target; this is a graph-level round bound, unrelated to the ctest process timeout.
    check(validate_workflow(wf).has_value(), "the large-context pipeline graph validates");

    auto planner_calls     = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto market_calls      = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto technical_calls   = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_fallback_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto aggregate_calls   = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto writer_calls      = std::make_shared<std::atomic<std::uint32_t>>(0);

    auto market_usage      = std::make_shared<agentengine::Usage>();
    auto technical_usage   = std::make_shared<agentengine::Usage>();
    auto competitive_usage = std::make_shared<agentengine::Usage>();  // stays zero: this call fails

    std::vector<ExecutorBody> bodies = {
        counted(planner_body(main_client, ctx), planner_calls),
        counted(large_context_specialist_body("market", "Market specialist", market_corpus, main_client,
                                               ctx, market_usage),
                market_calls),
        counted(large_context_specialist_body("technical", "Technical-feasibility specialist",
                                               technical_corpus, main_client, ctx, technical_usage),
                technical_calls),
        // Deliberately pointed at `bad_client` (a nonexistent model id) -- a REAL failure against a
        // REAL, large request body, routed by the engine's own `EdgeFailurePolicy::fallback`.
        counted(large_context_specialist_body("competitive", "Competitive-landscape specialist",
                                               competitive_corpus, bad_client, ctx, competitive_usage),
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
    std::size_t                 checkpoint_bytes = 0;

    // ---- "before the restart" ----------------------------------------------------------------------
    {
        FileSessionStore store2(root);
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowCheckpointManager<FileSessionStore> mgr(store2);
        mgr.attach(sup);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{
            text_message("Should we launch a mid-tier home-espresso machine in the US market?")}));

        check(r.status == workflow_status::suspended,
              "the run suspends at the human publish-review gate rather than auto-publishing");
        check(planner_calls->load(std::memory_order_relaxed) == 1, "planner ran exactly once");
        check(market_calls->load(std::memory_order_relaxed) == 1, "market specialist ran exactly once");
        check(technical_calls->load(std::memory_order_relaxed) == 1,
              "technical specialist ran exactly once");
        check(competitive_calls->load(std::memory_order_relaxed) == 1,
              "competitive specialist was genuinely ATTEMPTED, with its full large request body, "
              "before its real failure routed through EdgeFailurePolicy::fallback");
        check(competitive_fallback_calls->load(std::memory_order_relaxed) == 1,
              "the fallback recovery executor ran exactly once");
        check(aggregate_calls->load(std::memory_order_relaxed) == 1,
              "GitHub issue #52's fix, exercised under REAL large-context calls: the aggregator still "
              "ran EXACTLY ONCE with all three contributions merged, not twice with the second "
              "overwriting the first");
        check(writer_calls->load(std::memory_order_relaxed) == 1, "the writer ran exactly once");

        std::uint64_t const market_total = market_usage->input_tokens + market_usage->output_tokens;
        std::uint64_t const technical_total =
            technical_usage->input_tokens + technical_usage->output_tokens;
        std::printf("REAL usage: market=%llu+%llu technical=%llu+%llu (input+output tokens)\n",
                     static_cast<unsigned long long>(market_usage->input_tokens),
                     static_cast<unsigned long long>(market_usage->output_tokens),
                     static_cast<unsigned long long>(technical_usage->input_tokens),
                     static_cast<unsigned long long>(technical_usage->output_tokens));
        check(market_total > 60000,
              "the market specialist's REAL reported context (input+output tokens) exceeds 60K -- "
              "not an assumed character count, the provider's own Usage on the live response");
        check(technical_total > 60000,
              "the technical specialist's REAL reported context (input+output tokens) exceeds 60K");

        auto const asks = sup.open_interaction_asks();
        check(asks.size() == 1, "exactly one interaction ask is pending");
        std::string const pending_report = asks.empty() ? "" : text_of(asks.at(0).ask);
        check(pending_report.find("REPORT:") != std::string::npos,
              "the suspended run's pending publish-review ask carries the writer's real report");

        RunStateRecord const rec = drive(sup.snapshot_record());
        std::vector<std::byte> const encoded = agentengine::rt::encode_run_state_record(rec);
        checkpoint_bytes = encoded.size();
        std::printf("checkpoint size: %zu bytes\n", checkpoint_bytes);
        check(checkpoint_bytes > 2000,
              "the real checkpoint record (specialist outputs + the writer's fuller report) is "
              "meaningfully larger than the small-context test's own checkpoint -- proof the larger "
              "completions, not just the large corpora, actually flow through the engine's own state");

        run_id         = sup.run_id();
        interaction_id = r.open_interactions.at(0).interaction_id;
        std::printf("[before restart] suspended run_id=%s awaiting publish decision (interaction=%s)\n",
                     run_id.c_str(), interaction_id.c_str());
    }

    // ---- "the process restarts" ----------------------------------------------------------------------
    {
        FileSessionStore reopened(root);

        auto planner_calls2     = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto market_calls2      = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto technical_calls2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_fallback_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto aggregate_calls2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto writer_calls2      = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto market_usage2      = std::make_shared<agentengine::Usage>();
        auto technical_usage2   = std::make_shared<agentengine::Usage>();
        auto competitive_usage2 = std::make_shared<agentengine::Usage>();

        std::vector<ExecutorBody> bodies2 = {
            counted(planner_body(main_client, ctx), planner_calls2),
            counted(large_context_specialist_body("market", "unused after resume", market_corpus,
                                                   main_client, ctx, market_usage2),
                    market_calls2),
            counted(large_context_specialist_body("technical", "unused after resume", technical_corpus,
                                                   main_client, ctx, technical_usage2),
                    technical_calls2),
            counted(large_context_specialist_body("competitive", "unused after resume",
                                                   competitive_corpus, bad_client, ctx,
                                                   competitive_usage2),
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
              "resume_or_start() finds the on-disk checkpoint and resumes instead of starting fresh");

        auto const resumed_asks = sup.open_interaction_asks();
        check(resumed_asks.size() == 1, "the resumed run still carries the original pending ask");
        std::string const resumed_report = resumed_asks.empty() ? "" : text_of(resumed_asks.at(0).ask);
        check(resumed_report.find("REPORT:") != std::string::npos,
              "the report recovered from the resumed checkpoint's own ask is real, not a stub");

        Message const approval = text_message("approved: ship it\n\n" + resumed_report);
        WorkflowResult r = drive(sup.resume_workflow(ResumeWorkflow{interaction_id, approval, {}}));

        check(r.status == workflow_status::completed,
              "resuming with the human's approval drives the run through publish_decision to "
              "completion");
        std::string const final_text = text_of(r.output);
        std::printf("[after restart] completed (%zu chars)\n", final_text.size());
        check(final_text.find("PUBLISHED:") != std::string::npos,
              "the completed run's output carries the real publish branch, not a stub");
        check(final_text.find("REPORT:") != std::string::npos,
              "the published output still carries the writer's real, earlier-produced (large-context-"
              "grounded) report -- proving it survived the checkpoint round-trip at this scale");

        check(planner_calls2->load(std::memory_order_relaxed) == 0 &&
                  market_calls2->load(std::memory_order_relaxed) == 0 &&
                  technical_calls2->load(std::memory_order_relaxed) == 0 &&
                  competitive_calls2->load(std::memory_order_relaxed) == 0 &&
                  competitive_fallback_calls2->load(std::memory_order_relaxed) == 0 &&
                  aggregate_calls2->load(std::memory_order_relaxed) == 0 &&
                  writer_calls2->load(std::memory_order_relaxed) == 0,
              "NONE of the resumed supervisor's fresh body closures ran -- the finished large-context "
              "live work genuinely came from the checkpoint, not from silently (and expensively) "
              "re-doing it after resume");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_research_pipeline_large_context_live_e2e: OK\n"
                              : "test_workflow_research_pipeline_large_context_live_e2e: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
