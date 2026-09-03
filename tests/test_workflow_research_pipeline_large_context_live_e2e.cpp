// Large-context, GENUINELY MULTI-TURN live stress test. First version of this file (2026-09-03) fed
// each specialist ONE call with a large corpus stuffed into a single prompt -- real per-call context,
// but still just one LLM turn per node, not an actual multi-turn/complex workflow. Correctly called out
// by the project owner: "chỉ có 1 turn LLM nên không thể nói đó là 1 workflow lớn/phức tạp. chỉ có
// turn user đang dùng nhiều token làm đầu vào thôi" (only one LLM turn, so this can't be called a
// large/complex workflow -- only the ONE user turn is using a lot of input tokens).
//
// Rewritten to actually be multi-turn: `market` and `technical` are now CYCLIC self-loop nodes
// (014 §9 Q2's "cycles are allowed"; the same switch_case-self-edge shape
// test_rt_workflow_supervisor_patterns.cpp's CY-1/CY-2 already prove offline, and the SAME "one
// AgentSession reused across rounds accumulates real history" guarantee
// tests/test_rt_agent_workflow_executor.cpp's T5 already proves with a scripted backend) -- each one
// binds its OWN real `rt::AgentSession<RealClient>` and drives it through ~10 REAL, SEQUENTIAL live
// turns: turn i sends a fresh ~28K-char slice of a real reference document (sliced from this repo's
// own MIT-licensed design docs) and asks for a one-sentence note; the FINAL turn asks for the actual
// 150-250 word analysis. Because AgentSession resends the WHOLE accumulated conversation on every
// turn (a real stateless chat-completions client, not server-side session state), the LAST turn's own
// reported `Usage.input_tokens` is a genuine measurement of ~10 real turns' worth of accumulated
// conversation -- not one stuffed prompt. Every check below is against that REAL reported number.
//
// What this specifically hunts for that the (now superseded) single-shot version, the small-context
// test, and the offline suite all structurally cannot:
//   1. AgentSession's real conversation-history growth and resend behavior at genuine multi-turn
//      scale (~10 real turns, tens of thousands of tokens by the end) against a REAL backend --
//      test_rt_agent_session_real_backend.cpp's own multi-turn coverage stays much smaller.
//   2. A cyclic self-loop workflow node (014 §3's "Reflection"-shaped pattern) driving a REAL
//      AgentSession across real network latency/jitter per round, not a scripted stand-in.
//   3. Large REQUEST bodies over TLS repeated many times in one run (10 real POSTs per specialist,
//      each larger than the last).
//   4. Checkpoint serialization of a real run whose specialist outputs and final report are
//      meaningfully larger than a one-line tag, same as the previous version of this file.
//
// ENGINE GAP FOUND, THEN FIXED: THIS FILE'S FIRST MULTI-TURN LIVE RUN (2026-09-03) found that item 3
// above ("the fan_in merge only firing once BOTH cyclic specialists have finished") -- this file's
// ORIGINAL expectation -- was FALSE. An ordinary `fan_in` target did not wait for every declared
// source; it dispatched as soon as ANY source delivered THIS round. `competitive` (via GitHub issue
// #52's own already-fixed `fallback` path) resolves in ~2-3 rounds; `market`/`technical` take ~11
// rounds each. `aggregate` dispatched EARLY, once `competitive_fallback` delivered, with ONLY that
// content -- then `writer` -> the `publish_review` request_port opened -> `execute()`'s round loop
// (`if (!ports_.empty() || ...) {status = suspended; break;}`) ended the ENTIRE run immediately,
// abandoning `market`/`technical` mid-loop. Filed as GitHub issue #62, pinned offline by (the
// since-renamed) tests/test_workflow_fanin_uneven_round_sources_fix.cpp, and FIXED the same day:
// `seed_fan_in_holds()` (workflow_supervisor.hpp) now pre-registers, before round 1 ever runs, a
// join barrier for EVERY fan_in target with 2+ declared sources -- `aggregate` now genuinely waits
// for `market_done`, `technical_done`, AND `competitive`/`competitive_fallback` before it ever
// dispatches, no matter how many rounds apart they resolve. The assertions below reflect that FIXED
// behavior (both specialists complete their full ~11-turn loop, `aggregate` runs exactly once with
// every contribution merged, the final context genuinely exceeds 60K) -- the ORIGINAL, correct
// expectation this file always intended, restored once the fix landed. NOTE: this specific file has
// NOT itself been re-run live since the fix (a genuine multi-hour-of-turns, real-money live run) --
// the fix is verified by test_workflow_fanin_uneven_round_sources_fix.cpp's offline U1 and
// test_workflow_fanin_concurrent_failure_policy_fix.cpp's F1-F4 (all passing), not yet by re-executing
// THIS file end to end; run it (AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS=1) to close that gap.
//
// A SEPARATE, COMPOUNDING characteristic this fix does NOT address (still real, disclosed, not fixed
// here): `execute()`'s round loop still breaks the ENTIRE round loop the instant ANY request_port
// opens anywhere in the graph, abandoning every OTHER still-pending branch regardless of whether it
// has anything to do with that port. For THIS file's own shape (the port is strictly downstream of
// `aggregate`) that no longer matters in practice -- `market`/`technical` finish before the port can
// ever open -- but a graph where a port can open from an unrelated branch while a fan_in target is
// still genuinely waiting would still see that branch paused mid-loop until resume.
//
// Needs BOTH `AGENTENGINE_OPENROUTER_API_KEY` AND `AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS=1` -- see
// tests/CMakeLists.txt's own comment for why this carries its own "live-network-heavy" label instead
// of "live-network": ~10 real sequential turns per specialist, each resending a growing conversation,
// is well over 1M billed tokens combined for one run of this file -- meaningfully more than the
// previous single-shot version, by design (that's what genuine multi-turn accumulation costs). SKIPS
// (exit 0) if either is missing. Nondeterministic by nature: nothing below asserts on model CONTENT,
// only on structure, turn counts, and the real usage numbers the provider reports.

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
#include "agentengine/rt/agent_session.hpp"
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
using agentengine::rt::AgentResponse;
using agentengine::rt::AgentSession;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::FileSessionStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::StartRun;
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

using RealClient  = openai::OpenAIChatClient<InMemorySecretStore>;
using RealSession = AgentSession<RealClient>;

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

// Reads a real file from this repository and splits it into `count` roughly-equal, non-overlapping
// chunks of at most `chunk_chars` characters each -- the per-TURN payload a cyclic specialist below
// reads one at a time across real, sequential live calls. Real MIT-licensed project text, not
// synthetic filler: exercises the JSON request encoder against genuine markdown on every single turn.
[[nodiscard]] std::vector<std::string> read_repo_file_chunks(std::string const& relative_path,
                                                              std::size_t count, std::size_t chunk_chars) {
    std::filesystem::path const path =
        std::filesystem::path(AGENTENGINE_REPO_ROOT) / relative_path;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "FATAL: could not open fixture file: %s\n", path.string().c_str());
        return {};
    }
    std::vector<std::string> chunks;
    chunks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::string buf(chunk_chars, '\0');
        in.read(buf.data(), static_cast<std::streamsize>(chunk_chars));
        buf.resize(static_cast<std::size_t>(in.gcount()));
        if (buf.empty()) break;
        chunks.push_back(std::move(buf));
    }
    return chunks;
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

// A GENUINELY MULTI-TURN specialist: on turn i < chunks.size(), sends chunk i (fresh real document
// text) to `session` and asks for a one-sentence note; on the FINAL turn, asks for the real 150-250
// word analysis. Since `session` accumulates real history and resends the whole conversation on every
// `start_run()` call, turn count real turns genuinely happen -- this is a `function`-kind body (not
// `executor_kind::agent`/`agent_session_as_executor_body`, whose generic adapter has no routing
// concept -- see that adapter's own "ROUTING NOTE" comment) so it can propose its own switch_case
// route ("continue" to loop back onto itself, "finish" to exit toward `aggregate`) based on its own
// round counter, independent of anything the model says. `turns_out`/`final_usage_out` let main()
// assert on the REAL turn count and the REAL final-turn reported Usage.
// Returns `ExecutorOutcome` (NOT a bare `Message`) with an explicit route ("continue"/"finish") --
// this body's caller wires it onto TWO switch_case edges (self-loop and exit), and switch_case's own
// `edge_fires()` (workflow_supervisor.hpp) fires ONLY when `reply.routes` contains that edge's
// `case_label`; a body returning a bare `Message` (empty routes) would make BOTH edges dead and the
// whole run fail at round 1 with `routing_failed` -- caught in review, not empirically the hard way.
// `ctx` is the OUTER, real EffectContext (captured by reference) -- the SAME pattern
// planner_body()/specialist_body()/writer_body() above already use, and for the SAME reason: the
// per-call EffectContext argument WorkflowSupervisor::execute() passes into every dispatched body is
// `contexts_[idx]`, populated from `initialize()`'s own (here, never-supplied, defaulted-empty)
// `contexts` argument -- NOT this file's real, Secret-granted `ctx`. Using the per-call parameter for
// `set_capabilities()` instead of the captured outer `ctx` was a real bug an early version of this
// file had: AgentSession::start_run() failed immediately with "secret ... resolved without a granted
// Secret<name> capability", before any real network call, on turn 1.
[[nodiscard]] ExecutorBody cyclic_multiturn_specialist_body(
    std::string label, std::string role_description, std::vector<std::string> chunks,
    std::shared_ptr<RealSession> session, EffectContext& ctx,
    std::shared_ptr<std::atomic<std::uint32_t>> turns_out,
    std::shared_ptr<agentengine::Usage> final_usage_out) {
    // Real bug found live 2026-09-03 (user inspected OpenRouter's own dashboard/request-log view):
    // an earlier version of this function never called `AgentSession::set_static_instructions()` --
    // `role_description` was baked into the per-turn USER prompt text instead, so `market`/
    // `technical`'s real live requests carried NO `role: "system"` message at all, unlike
    // `planner`/`writer`/`competitive` (built via `live_call()`, which constructs its own `ChatRequest`
    // with an explicit system message directly). `set_static_instructions()` is AgentSession's own
    // real, documented mechanism for this (agent_session.hpp, ADR-116/OQ-16) -- a configuration-time
    // call, made once here per this comment's own "called once, before the first start_run()"
    // contract, not per turn. Fixed by calling it once, right at body-construction time.
    session->set_static_instructions("You are a " + role_description + ".");
    auto round       = std::make_shared<std::size_t>(0);
    auto saved_brief = std::make_shared<std::string>();
    return [label, chunks = std::move(chunks), session, &ctx, turns_out,
            final_usage_out, round, saved_brief](
               Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        session->set_capabilities(ctx.capabilities.get());
        std::size_t const i = (*round)++;
        if (i == 0) *saved_brief = text_of(in);

        std::string prompt;
        bool const  is_final = i >= chunks.size();
        if (!is_final) {
            prompt = "Reference material -- part " + std::to_string(i + 1) + " of " +
                     std::to_string(chunks.size()) + ":\n\n" + chunks[i] +
                     "\n\n---\nIn ONE short sentence, note the single most notable detail from THIS "
                     "part (it is unrelated engineering documentation, included only to build up "
                     "conversation length -- do not try to connect it to the business question). "
                     "Respond with ONLY that one sentence.";
        } else {
            prompt = "You have now reviewed " + std::to_string(chunks.size()) +
                     " parts of the reference material across our conversation above. Ignore that "
                     "material's actual content -- it was unrelated filler. Give your real, thorough "
                     "150-250 word analysis addressing this brief:\n\n" + *saved_brief;
        }

        auto const t0     = std::chrono::steady_clock::now();
        agentengine::result<AgentResponse> driven =
            drive(session->start_run(StartRun{text_message(prompt)}));
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!driven.has_value()) {
            std::fprintf(stderr, "[%s turn %zu] FAILED after %lldms: %s (%s)\n", label.c_str(), i + 1,
                         static_cast<long long>(ms), driven.error().message.c_str(),
                         driven.error().code.c_str());
            return std::unexpected(driven.error());
        }
        turns_out->fetch_add(1, std::memory_order_relaxed);
        *final_usage_out = driven->usage;
        std::string const reply = text_of(driven->message);
        std::printf("[%s turn %zu/%zu] (%lldms) this-turn-usage=%llu+%llu=%llu tokens, reply=%zu "
                     "chars: %.80s%s\n",
                     label.c_str(), i + 1, chunks.size() + 1, static_cast<long long>(ms),
                     static_cast<unsigned long long>(driven->usage.input_tokens),
                     static_cast<unsigned long long>(driven->usage.output_tokens),
                     static_cast<unsigned long long>(driven->usage.input_tokens +
                                                       driven->usage.output_tokens),
                     reply.size(), reply.c_str(), reply.size() > 80 ? "..." : "");
        ExecutorOutcome outcome{};
        outcome.usage = driven->usage;
        if (is_final) {
            outcome.payload = text_message(label + " findings: " + reply);
            outcome.routes  = {"finish"};
            return outcome;
        }
        // Looping: this turn's own reply becomes NEXT turn's `in` -- meaningless as content (the
        // model was told to summarize unrelated filler), but its role/content shape must stay a
        // plain Message for the self-loop edge's type to line up (014 §1). What actually matters for
        // "was the brief preserved" is `saved_brief`, read directly on turn 0, not threaded through.
        outcome.payload = text_message(reply);
        outcome.routes  = {"continue"};
        return outcome;
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

constexpr std::size_t kChunksPerSpecialist = 10;
constexpr std::size_t kChunkChars          = 28000;

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
                     "expensive (well over 1M billed tokens: ~10 real sequential turns per "
                     "specialist, each resending a growing conversation) live run.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest");
    std::string const host  = env_or("AGENTENGINE_OPENROUTER_HOST", "openrouter.ai");
    std::string const bad_model = "agentengine-test/definitely-not-a-real-model-id";
    std::fprintf(stderr,
                 "test_workflow_research_pipeline_large_context_live_e2e: host=%s model=%s "
                 "chunks_per_specialist=%zu chunk_chars=%zu\n",
                 host.c_str(), model.c_str(), kChunksPerSpecialist, kChunkChars);

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

    // Real, MIT-licensed project text -- TWO DIFFERENT large docs, chunked per-turn, so market and
    // technical are reading genuinely different material (not the same blob), and nothing downstream
    // could accidentally paper over a real per-turn bug via caching/dedup.
    std::vector<std::string> const market_chunks = read_repo_file_chunks(
        "docs/planning/identity-native-sandbox-worktree-design.md", kChunksPerSpecialist, kChunkChars);
    std::vector<std::string> const technical_chunks = read_repo_file_chunks(
        "docs/planning/office-document-extraction-design-draft.md", kChunksPerSpecialist, kChunkChars);
    check(market_chunks.size() == kChunksPerSpecialist && technical_chunks.size() == kChunksPerSpecialist,
          "setup: both real fixture documents chunked into the expected number of pieces");

    Workflow wf;
    wf.id        = "large-context-multiturn-research-go-no-go-pipeline";
    wf.executors = {
        node_desc("planner"), node_desc("market"), node_desc("market_done"), node_desc("technical"),
        node_desc("technical_done"), node_desc("competitive"), node_desc("competitive_fallback"),
        node_desc("aggregate"), node_desc("writer"),
        node_desc("publish_review", executor_kind::request_port), node_desc("publish_decision"),
        node_desc("publish"), node_desc("abandon"), node_desc("done"),
    };
    wf.edges = {
        Edge{"planner", "market", edge_kind::fan_out, {}},
        Edge{"planner", "technical", edge_kind::fan_out, {}},
        Edge{"planner", "competitive", edge_kind::fan_out, {}},
        // Cyclic self-loops (014 §9 Q2, same shape CY-1/CY-2 -- test_rt_workflow_supervisor_patterns.
        // cpp -- prove offline): each real turn's own body proposes "continue" (loop back for the
        // next chunk) or "finish" (exit) based on its OWN round counter, never on anything the model
        // said. The exit edge deliberately routes through a one-shot `_done` node rather than
        // straight into `aggregate`: `fan_in` (unlike switch_case) fires UNCONDITIONALLY and merges
        // by TARGET INDEX (deliver_to_fan_in(), GitHub issue #52's fix) -- a switch_case edge straight
        // to a SHARED target would instead push a second, unmerged Delivery for the same index the
        // same round market/technical both finish in, tripping the OQ-19 same-round-duplicate
        // quarantine instead of a real merge. `_done` is dispatched exactly once (switch_case can only
        // select it once), so its own single `fan_in` edge into `aggregate` merges cleanly.
        Edge{"market", "market", edge_kind::switch_case, "continue"},
        Edge{"market", "market_done", edge_kind::switch_case, "finish"},
        Edge{"market_done", "aggregate", edge_kind::fan_in, {}},
        Edge{"technical", "technical", edge_kind::switch_case, "continue"},
        Edge{"technical", "technical_done", edge_kind::switch_case, "finish"},
        Edge{"technical_done", "aggregate", edge_kind::fan_in, {}},
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
    // Comfortably above kChunksPerSpecialist+1 rounds for market/technical's own loop, plus the rest
    // of the pipeline.
    wf.bound.max_rounds = static_cast<std::uint32_t>(kChunksPerSpecialist) + 10;
    check(validate_workflow(wf).has_value(), "the large-context multi-turn pipeline graph validates");

    auto planner_calls     = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto market_turns      = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto technical_turns   = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto competitive_fallback_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto aggregate_calls   = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto writer_calls      = std::make_shared<std::atomic<std::uint32_t>>(0);

    auto market_final_usage    = std::make_shared<agentengine::Usage>();
    auto technical_final_usage = std::make_shared<agentengine::Usage>();

    auto market_session    = std::make_shared<RealSession>();
    auto technical_session = std::make_shared<RealSession>();
    market_session->initialize("large-ctx-market", Principal{"large-ctx-market-principal", ""});
    market_session->emplace_chat_client(host, static_cast<std::uint16_t>(443), model,
                                        SecretRef{kSecretName}, caps, store, "/api/v1");
    technical_session->initialize("large-ctx-technical", Principal{"large-ctx-technical-principal", ""});
    technical_session->emplace_chat_client(host, static_cast<std::uint16_t>(443), model,
                                           SecretRef{kSecretName}, caps, store, "/api/v1");

    auto market_done_calls    = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto technical_done_calls = std::make_shared<std::atomic<std::uint32_t>>(0);

    // Executor order must match wf.executors above EXACTLY (bodies_ binds to graph_.executors purely
    // by array index): planner, market, market_done, technical, technical_done, competitive,
    // competitive_fallback, aggregate, writer, publish_review, publish_decision, publish, abandon,
    // done.
    std::vector<ExecutorBody> bodies = {
        counted(planner_body(main_client, ctx), planner_calls),
        cyclic_multiturn_specialist_body("market", "Market specialist", market_chunks, market_session,
                                         ctx, market_turns, market_final_usage),
        counted(identity_sink, market_done_calls),
        cyclic_multiturn_specialist_body("technical", "Technical-feasibility specialist",
                                         technical_chunks, technical_session, ctx, technical_turns,
                                         technical_final_usage),
        counted(identity_sink, technical_done_calls),
        // Deliberately pointed at `bad_client` (a nonexistent model id) -- a REAL failure, routed by
        // the engine's own `EdgeFailurePolicy::fallback`. Single-shot: this call tests fast-failure,
        // not scale.
        counted(specialist_body("competitive", "You are a Competitive-landscape specialist. In 1-2 "
                                                "sentences, summarize the competitive landscape for "
                                                "the given brief.",
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

        // GitHub issue #62's fix (this file's own top comment): `aggregate` now genuinely holds until
        // `market_done`, `technical_done`, AND `competitive`/`competitive_fallback` have ALL delivered
        // -- so `market`/`technical` run their FULL ~11-turn loop before `writer`/`publish_review` are
        // ever reached, restoring this file's ORIGINAL intended expectation.
        check(r.status == workflow_status::suspended,
              "the run suspends at the human publish-review gate rather than auto-publishing");
        check(planner_calls->load(std::memory_order_relaxed) == 1, "planner ran exactly once");
        std::printf("market turns completed: %u/%zu; technical: %u/%zu (FIXED: both expected to "
                     "reach the full %zu before aggregate ever dispatches)\n",
                     market_turns->load(std::memory_order_relaxed), kChunksPerSpecialist + 1,
                     technical_turns->load(std::memory_order_relaxed), kChunksPerSpecialist + 1,
                     kChunksPerSpecialist + 1);
        check(market_turns->load(std::memory_order_relaxed) == kChunksPerSpecialist + 1 &&
                  technical_turns->load(std::memory_order_relaxed) == kChunksPerSpecialist + 1,
              "GitHub issue #62 FIXED: the market specialist genuinely ran ALL expected turns (chunks "
              "+ one final synthesis turn) on the SAME AgentSession, and so did technical -- real "
              "multi-turn work, run to genuine completion, not cut short by an early fan_in dispatch");
        check(competitive_calls->load(std::memory_order_relaxed) == 1,
              "competitive specialist was genuinely ATTEMPTED before its real failure routed through "
              "EdgeFailurePolicy::fallback");
        check(competitive_fallback_calls->load(std::memory_order_relaxed) == 1,
              "the fallback recovery executor ran exactly once");
        check(market_done_calls->load(std::memory_order_relaxed) == 1 &&
                  technical_done_calls->load(std::memory_order_relaxed) == 1,
              "GitHub issue #62 FIXED: market_done AND technical_done both ran exactly once -- the "
              "real signal each cyclic specialist's own loop genuinely finished, dispatched only once "
              "`aggregate`'s fan_in barrier saw both (this used to never happen before the fix, since "
              "the run suspended well before either specialist's loop could finish)");
        check(aggregate_calls->load(std::memory_order_relaxed) == 1,
              "aggregate ran exactly once -- and, unlike before the fix, this is now the CORRECT, "
              "COMPLETE merge: market_done, technical_done, AND competitive_fallback ALL delivered "
              "before it ever dispatched, exercised under REAL multi-turn concurrent load");
        check(writer_calls->load(std::memory_order_relaxed) == 1,
              "the writer ran exactly once, on aggregate's now-complete, fully-merged input");

        std::printf("REAL final-turn usage: market=%llu+%llu technical=%llu+%llu (input+output tokens, "
                     "after %zu real prior turns each)\n",
                     static_cast<unsigned long long>(market_final_usage->input_tokens),
                     static_cast<unsigned long long>(market_final_usage->output_tokens),
                     static_cast<unsigned long long>(technical_final_usage->input_tokens),
                     static_cast<unsigned long long>(technical_final_usage->output_tokens),
                     kChunksPerSpecialist);
        check(market_final_usage->input_tokens + market_final_usage->output_tokens > 60000,
              "the market specialist's FINAL turn's REAL reported context (input+output tokens) "
              "exceeds 60K -- reached through genuine multi-turn accumulation across "
              "kChunksPerSpecialist+1 real sequential calls, not one stuffed prompt, and no longer cut "
              "short by GitHub issue #62's fan_in gap");
        check(technical_final_usage->input_tokens + technical_final_usage->output_tokens > 60000,
              "the technical specialist's FINAL turn's REAL reported context exceeds 60K, same way");

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
              "the real checkpoint record is meaningfully larger than a tiny toy checkpoint -- the "
              "multi-turn specialists' own findings and the writer's fuller report actually flow "
              "through the engine's own state, not just the outbound wire");

        run_id         = sup.run_id();
        interaction_id = r.open_interactions.at(0).interaction_id;
        std::printf("[before restart] suspended run_id=%s awaiting publish decision (interaction=%s)\n",
                     run_id.c_str(), interaction_id.c_str());
    }

    // ---- "the process restarts" ----------------------------------------------------------------------
    {
        FileSessionStore reopened(root);

        auto planner_calls2     = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto market_turns2      = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto technical_turns2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto competitive_fallback_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto aggregate_calls2   = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto writer_calls2      = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto market_usage2      = std::make_shared<agentengine::Usage>();
        auto technical_usage2   = std::make_shared<agentengine::Usage>();

        // Fresh AgentSessions -- if resume secretly re-drove either cyclic loop, THESE (never the
        // ones above, already out of scope) would show real turns.
        auto market_session2    = std::make_shared<RealSession>();
        auto technical_session2 = std::make_shared<RealSession>();
        market_session2->initialize("large-ctx-market-2", Principal{"large-ctx-market-principal-2", ""});
        market_session2->emplace_chat_client(host, static_cast<std::uint16_t>(443), model,
                                             SecretRef{kSecretName}, caps, store, "/api/v1");
        technical_session2->initialize("large-ctx-technical-2",
                                       Principal{"large-ctx-technical-principal-2", ""});
        technical_session2->emplace_chat_client(host, static_cast<std::uint16_t>(443), model,
                                                SecretRef{kSecretName}, caps, store, "/api/v1");

        auto market_done_calls2    = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto technical_done_calls2 = std::make_shared<std::atomic<std::uint32_t>>(0);

        std::vector<ExecutorBody> bodies2 = {
            counted(planner_body(main_client, ctx), planner_calls2),
            cyclic_multiturn_specialist_body("market", "unused after resume", market_chunks,
                                             market_session2, ctx, market_turns2, market_usage2),
            counted(identity_sink, market_done_calls2),
            cyclic_multiturn_specialist_body("technical", "unused after resume", technical_chunks,
                                             technical_session2, ctx, technical_turns2,
                                             technical_usage2),
            counted(identity_sink, technical_done_calls2),
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
              "resume_or_start() finds the on-disk checkpoint and resumes instead of starting fresh");

        auto const resumed_asks = sup.open_interaction_asks();
        check(resumed_asks.size() == 1, "the resumed run still carries the original pending ask");
        std::string const resumed_report = resumed_asks.empty() ? "" : text_of(resumed_asks.at(0).ask);
        check(resumed_report.find("REPORT:") != std::string::npos,
              "the report recovered from the resumed checkpoint's own ask is real, not a stub");

        // GitHub issue #62's fix changes what "drive this resumed run to completion" costs: `market`/
        // `technical` are NOT in `state_.pending` at the checkpoint anymore -- their fan_in barrier
        // already released and `aggregate`/`writer` already ran BEFORE `publish_review` ever opened
        // (that is the whole point of the fix), so the only thing left pending across the restart is
        // the port answer itself. Resolving it drives straight through `publish_decision` -> `publish`
        // -> `done`, at ZERO additional live model cost -- unlike before the fix, where the abandoned
        // cyclic branches WOULD have still been mid-loop at checkpoint time, and (per
        // agent_workflow_executor.hpp's own already-documented, accepted checkpoint/resume limitation,
        // "conversation history does NOT survive a checkpoint/resume cycle") a driven-to-completion
        // resume back then would have redone BOTH specialists' ENTIRE ~11-turn loop from scratch on
        // fresh, history-less AgentSessions. `market_session2`/`technical_session2` above exist
        // precisely to prove that did NOT happen: their own turn counters must stay at zero.
        WorkflowResult const r2 =
            drive(sup.resume_workflow(ResumeWorkflow{interaction_id, text_message("approved"), {}}));
        check(r2.status == workflow_status::completed,
              "resuming with the human's approval drives the run through publish_decision to "
              "completion");
        check(market_turns2->load(std::memory_order_relaxed) == 0 &&
                  technical_turns2->load(std::memory_order_relaxed) == 0,
              "GitHub issue #62 FIXED (and a happy consequence of it): NEITHER fresh AgentSession took "
              "a single turn on resume -- market/technical had ALREADY fully finished (and fed "
              "aggregate/writer) before the checkpoint was ever taken, so completing the resumed run "
              "costs nothing beyond the port answer itself, unlike before the fix");
        check(aggregate_calls2->load(std::memory_order_relaxed) == 0 &&
                  writer_calls2->load(std::memory_order_relaxed) == 0,
              "aggregate/writer did not re-run either -- their real output already lived inside the "
              "checkpoint (the writer's report `open_interaction_asks()` recovered above), not "
              "recomputed after resume");
        std::string const published = text_of(r2.output);
        check(published.find("PUBLISHED:") != std::string::npos,
              "the completed run's output carries the real publish branch, not a stub");
        check(published.find("REPORT:") != std::string::npos,
              "the published output still carries the writer's real, earlier-produced (multi-turn-"
              "grounded) report -- proving it survived the checkpoint round-trip at this scale");
        std::printf("[after restart] completed (%zu chars)\n", published.size());
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_research_pipeline_large_context_live_e2e: OK\n"
                              : "test_workflow_research_pipeline_large_context_live_e2e: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
