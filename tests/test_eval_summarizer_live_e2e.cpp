// Live check for decisions/ADR-181-evaluation-harness.md's summarizer accounting (PR #100: §3.8's
// "recorded (E19), budgeted (§3.9)", and the R3-GH-3 fix): runs real `run_trial` calls in which BOTH the
// agent's model and the memory SUMMARIZER are a real OpenAI-compatible endpoint. Every earlier test drove
// the summarizer with a scripted mock, so none could show what the budget actually depends on:
//   * that a real provider reports token usage on a summarizer stream's FINAL chunk (the budget reads
//     only that chunk -- a provider that reports none is charged an estimate instead, visible here as
//     `summarizer_usage_estimated`);
//   * that every real summarizer call is recorded, one per agent turn;
//   * that a tight `summarizer_token_budget` really refuses further calls without breaking the trial.
//
// A live model is nondeterministic, so the assertions are structural (counts, flags, convergence) and the
// per-call token numbers are PRINTED for a human. Unset key => SKIP, exit 0.
//   AGENTENGINE_OPENROUTER_API_KEY, _MODEL, _HOST, _PATH_PREFIX (same names as the other live tests;
//   defaults point at DeepSeek direct: api.deepseek.com, deepseek-flash, /v1)

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/eval/eval_trial.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;

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

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

template <class T>
T drive(task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

constexpr char const* kSecretName = "eval-summarizer-live-api-key";

Message user_message(std::string text) {
    Message m{};
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// The usage the summarizer's own final chunk reported, if any -- the one number the budget charges.
std::optional<Usage> final_chunk_usage(ChatCallRecording const& rec) {
    std::optional<Usage> usage;
    for (RecordedChunk const& chunk : rec.chunks) {
        if (chunk.update.is_final && chunk.update.usage.has_value()) usage = chunk.update.usage;
    }
    return usage;
}

void print_summarizer(char const* label, eval::TrialResult const& t) {
    std::fprintf(stderr,
                 "  [%s] outcome=%s agent_calls=%zu summarizer_calls=%zu summarizer_tokens=%llu estimated=%s "
                 "budget_exhausted=%s\n",
                 label, t.outcome.has_value() ? "ok" : t.outcome.error().code.c_str(), t.recordings.size(),
                 t.summarizer_recordings.size(), static_cast<unsigned long long>(t.summarizer_tokens),
                 t.summarizer_usage_estimated ? "true" : "false", t.summarizer_budget_exhausted ? "true" : "false");
    for (std::size_t i = 0; i < t.summarizer_recordings.size(); ++i) {
        ChatCallRecording const& rec = t.summarizer_recordings[i];
        auto const usage = final_chunk_usage(rec);
        std::string text;
        for (RecordedChunk const& chunk : rec.chunks) {
            if (auto const* piece = std::get_if<Text>(&chunk.update.delta.value)) text += piece->text;
        }
        if (text.size() > 160) text = text.substr(0, 160) + "...";
        std::fprintf(stderr, "    summarizer call %zu: terminal=%s chunks=%zu usage=%s  \"%s\"\n", i,
                     rec.stream_terminal.c_str(), rec.chunks.size(),
                     usage ? (std::to_string(usage->input_tokens) + " in / " + std::to_string(usage->output_tokens) + " out").c_str()
                           : "none reported",
                     text.c_str());
    }
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr, "test_eval_summarizer_live_e2e: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is not set.\n");
        return 0;
    }
    std::string const model  = env_or("AGENTENGINE_OPENROUTER_MODEL", "deepseek-flash");
    std::string const host   = env_or("AGENTENGINE_OPENROUTER_HOST", "api.deepseek.com");
    std::string const prefix = env_or("AGENTENGINE_OPENROUTER_PATH_PREFIX", "/v1");
    std::fprintf(stderr, "test_eval_summarizer_live_e2e: host=%s model=%s\n", host.c_str(), model.c_str());

    InMemorySecretStore secret_store;
    secret_store.set(kSecretName, *key_env);

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 200;

    auto make_client = [&] {
        return openai::OpenAIChatClient(host, 443, model, SecretRef{kSecretName}, caps, secret_store, prefix,
                                        sandbox::resolve_host, /*ca=*/{}, /*http_referer=*/{},
                                        /*x_title=*/"AgentEngine Eval Summarizer Live Check",
                                        /*end_user_id=*/"test-eval-summarizer-live");
    };

    eval::StubToolFixture set_deploy_region;
    set_deploy_region.name              = "set_deploy_region";
    set_deploy_region.description       = "Sets the deployment region for a new service.";
    set_deploy_region.args_schema_json  = R"({"type":"object","properties":{"region":{"type":"string"}}})";
    set_deploy_region.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    set_deploy_region.canned_reply = json::Value::make_object({{"ok", json::Value::make_bool(true)}});

    auto base_spec = [&](std::string trial_id) {
        eval::TrialSpec spec;
        spec.arm         = eval::trial_arm::baseline;
        spec.task_prompt = user_message("I'm about to deploy a new service to eu-west-1. Please set the deploy "
                                        "region for it using the available tool, then confirm briefly.");
        spec.stub_tools  = {set_deploy_region};
        spec.trial_id    = std::move(trial_id);
        spec.max_turns   = 4;
        spec.extra_capabilities = {Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}}};
        return spec;
    };

    // ---- 1. A real summarizer, unbudgeted: every call recorded, usage reported ------------------------
    {
        auto result = drive(eval::run_trial(make_client(), make_client(), base_spec("live-summ-1")));
        print_summarizer("unbudgeted", result);
        check(!result.setup_error.has_value() && result.outcome.has_value(), "1: the trial converges");
        check(!result.summarizer_recordings.empty(), "1: the real summarizer was called and recorded");
        check(result.summarizer_recordings.size() == result.recordings.size(),
              "1: one summarizer call per agent call (the 2-calls-per-turn budget rule)");
        bool all_closed = true;
        for (auto const& rec : result.summarizer_recordings) all_closed = all_closed && rec.stream_terminal == "closed";
        check(all_closed, "1: every summarizer stream ended cleanly");
        check(!result.summarizer_usage_estimated,
              "1: the provider reported usage on every summarizer stream's final chunk (no estimate needed)");
        std::uint64_t reported = 0;
        for (auto const& rec : result.summarizer_recordings) {
            if (auto u = final_chunk_usage(rec)) reported += u->input_tokens + u->output_tokens;
        }
        check(result.summarizer_tokens > 0 && result.summarizer_tokens == reported,
              "1: summarizer_tokens is exactly the sum of the final-chunk usage the provider reported");
        check(!result.summarizer_budget_exhausted, "1: no budget, nothing refused");
    }

    // ---- 2. A 1-token summarizer budget: the first call runs, the rest are refused ---------------------
    {
        auto spec = base_spec("live-summ-2");
        spec.summarizer_token_budget = 1;
        auto result = drive(eval::run_trial(make_client(), make_client(), spec));
        print_summarizer("budget=1", result);
        check(!result.setup_error.has_value() && result.outcome.has_value(),
              "2: the trial still converges with its summarizer cut off");
        check(result.summarizer_recordings.size() == 1u,
              "2: exactly one real summarizer call was made (0 < 1 admits it; its spend closes the budget)");
        check(result.recordings.size() < 2u || result.summarizer_budget_exhausted,
              "2: every later turn's summarizer call was refused before reaching the model");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_eval_summarizer_live_e2e: OK\n" : "test_eval_summarizer_live_e2e: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
