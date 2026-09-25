// Live sanity check for decisions/ADR-195-evaluation-harness.md's trial-running harness (§3.0
// items 2-4, §3.2): runs one real B (baseline) trial and one real T (treatment) trial through
// `run_trial` against a real model, rather than a scripted client. This is the end-to-end
// verification test_eval_trial_driver.cpp cannot provide on its own -- a scripted client proves
// the DRIVER's own plumbing works; this proves the whole thing still works against a real model's
// actual tool-calling behaviour.
//
// OBSERVATION, NOT A GATE (I5): a live model is nondeterministic, so assertions here are
// structural only (both trials converge without error). Delivery/tool-call results are PRINTED for
// a human, matching this repo's other live-network tests' own convention
// (test_memory_lesson_effectiveness_live_e2e.cpp). Unset key => SKIP, exit 0.
//   AGENTENGINE_OPENROUTER_API_KEY, _MODEL, _HOST, _PATH_PREFIX (same names as the other live tests;
//   defaults below point at DeepSeek direct: api.deepseek.com, deepseek-flash, /v1)

#include <cstdio>
#include <string>

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

constexpr char const* kSecretName = "eval-trial-live-api-key";

class NoopSummarizerClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"no summary in this sanity check"};
        item.origin = content_origin::assistant;
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-noop-summary";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = Text{"no summary in this sanity check"};
        upd.is_final     = true;
        upd.usage        = Usage{1, 1, 0, 0, 0.0};
        (void)pair.producer.push(upd);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<NoopSummarizerClient>);

Message user_message(std::string text) {
    Message m{};
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

void print_result(char const* label, eval::TrialResult const& result) {
    std::fprintf(stderr, "  [%s] outcome=%s delivered=%s delivered_via_recall=%s recall_invoked=%s tool_calls=%zu\n",
                 label, result.outcome.has_value() ? "ok" : "error", result.delivered ? "true" : "false",
                 result.delivered_via_recall ? "true" : "false", result.recall_invoked ? "true" : "false",
                 result.tool_calls.size());
    for (auto const& call : result.tool_calls) {
        std::fprintf(stderr, "    tool_call: %s(%s)\n", call.tool_name.c_str(), json::dump(call.arguments).c_str());
    }
}

char const* role_name(role r) {
    switch (r) {
        case role::system:    return "system";
        case role::user:      return "user";
        case role::assistant: return "assistant";
        case role::tool:      return "tool";
    }
    return "?";
}

// Recurses into a ToolResult's own content (a tool reply is itself a small list of content items) --
// this is the SAME shape eval_trial.hpp's own detail::content_item_contains walks for delivery
// detection, printed here instead of searched.
std::string describe_content_item(ContentItem const& item) {
    return std::visit(
        [&](auto const& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Text>) {
                return "Text: " + v.text;
            } else if constexpr (std::is_same_v<T, Reasoning>) {
                return "Reasoning: " + v.text;
            } else if constexpr (std::is_same_v<T, ToolCall>) {
                return "ToolCall[" + v.call_id + "] " + v.tool_name + "(" + v.arguments_json + ")";
            } else if constexpr (std::is_same_v<T, ToolResult>) {
                std::string out = "ToolResult[" + v.call_id + "]" + (v.is_error ? " ERROR" : "") + " { ";
                for (auto const& nested : v.content) out += describe_content_item(nested) + " ";
                out += "}";
                return out;
            } else if constexpr (std::is_same_v<T, Data>) {
                return "Data: " + v.json;
            } else if constexpr (std::is_same_v<T, Citation>) {
                return "Citation: " + v.source;
            } else if constexpr (std::is_same_v<T, agentengine::Error>) {
                return "Error: " + v.message;
            } else if constexpr (std::is_same_v<T, Custom>) {
                return "Custom(" + v.type_id + "): " + v.payload_json;
            } else {
                return "Media(...)";
            }
        },
        item.value);
}

void print_message(Message const& msg, char const* prefix) {
    std::string attribution = "none";
    if (msg.attribution.has_value()) {
        attribution = msg.attribution->contributor_type + "#" + std::to_string(msg.attribution->contributor_index);
    }
    std::fprintf(stderr, "%s[%s attribution=%s]\n", prefix, role_name(msg.role), attribution.c_str());
    for (ContentItem const& item : msg.content) {
        std::fprintf(stderr, "%s  %s%s\n", prefix, describe_content_item(item).c_str(),
                     item.tainted ? "  (tainted)" : "");
    }
}

// The full conversation dump the user asked for: every recorded round's outbound request (every
// message actually sent to the model, in order) and the model's own response, for a whole trial --
// not just the summary booleans/tool-call list print_result() already gives.
void print_transcript(char const* label, eval::TrialResult const& result) {
    std::fprintf(stderr, "  ---- [%s] full transcript (%zu round(s)) ----\n", label, result.recordings.size());
    for (std::size_t round = 0; round < result.recordings.size(); ++round) {
        ChatCallRecording const& rec = result.recordings[round];
        std::fprintf(stderr, "  -- round %zu: %zu message(s) sent, %zu tool(s) offered --\n", round,
                     rec.request.messages.size(), rec.request.tools.size());
        for (Message const& msg : rec.request.messages) {
            print_message(msg, "    > ");
        }
        if (rec.response.has_value()) {
            print_message(rec.response->message, "    < ");
        } else if (rec.chat_error.has_value()) {
            std::fprintf(stderr, "    < [chat_error] %s\n", rec.chat_error->message.c_str());
        }
    }
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "test_eval_trial_driver_live_e2e: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is not set.\n");
        return 0;
    }
    std::string const model  = env_or("AGENTENGINE_OPENROUTER_MODEL", "deepseek-flash");
    std::string const host   = env_or("AGENTENGINE_OPENROUTER_HOST", "api.deepseek.com");
    std::string const prefix = env_or("AGENTENGINE_OPENROUTER_PATH_PREFIX", "/v1");
    std::fprintf(stderr, "test_eval_trial_driver_live_e2e: host=%s model=%s\n", host.c_str(), model.c_str());

    InMemorySecretStore secret_store;
    secret_store.set(kSecretName, *key_env);

    ChatClientCapabilities caps;
    caps.streaming         = true;
    caps.max_output_tokens = 200;

    // Chosen to clear lesson_value_passes_validator's denylist (no URL/path/shell/imperative shape,
    // >= 6 characters, not a common whole-value token) -- checked by eyeball against
    // lesson_candidate.hpp's own rules, and render_lesson would refuse it at trial setup otherwise.
    eval::LessonCandidate const candidate{"deploy-region", "default",
                                           "the default region for new deployments is eu-west-1",
                                           "run-0/turn-0"};

    eval::StubToolFixture set_deploy_region;
    set_deploy_region.name              = "set_deploy_region";
    set_deploy_region.description       = "Sets the deployment region for a new service.";
    set_deploy_region.args_schema_json  = R"({"type":"object","properties":{"region":{"type":"string"}}})";
    set_deploy_region.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    set_deploy_region.canned_reply = json::Value::make_object({{"ok", json::Value::make_bool(true)}});

    Message const prompt =
        user_message("I'm about to deploy a new service and need to pick a region. Please set the "
                      "deploy region for it using the available tool.");

    std::vector<Capability> const secret_cap = {Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}}};

    // ---- baseline (B): no candidate lesson ----------------------------------------------------------
    {
        eval::TrialSpec spec;
        spec.arm               = eval::trial_arm::baseline;
        spec.task_prompt       = prompt;
        spec.stub_tools        = {set_deploy_region};
        spec.trial_id          = "live-b1";
        spec.max_turns         = 4;
        spec.extra_capabilities = secret_cap;

        openai::OpenAIChatClient client(host, 443, model, SecretRef{kSecretName}, caps, secret_store, prefix,
                                         sandbox::resolve_host, /*ca=*/{}, /*http_referer=*/{},
                                         /*x_title=*/"AgentEngine Eval Trial Live Sanity Check",
                                         /*end_user_id=*/"test-eval-trial-driver-live");
        auto result = drive(eval::run_trial(std::move(client), NoopSummarizerClient{}, spec));
        check(!result.setup_error.has_value(), "B: no setup error");
        check(result.outcome.has_value(), "B: the baseline trial converges");
        print_result("B", result);
        print_transcript("B", result);
    }

    // ---- treatment (T): the candidate lesson, seeded through the real MemoryProvider path ------------
    {
        eval::TrialSpec spec;
        spec.arm                = eval::trial_arm::treatment;
        spec.candidate          = candidate;
        spec.template_version   = "v1";
        spec.lesson_salience    = 0.3f;
        spec.task_prompt        = prompt;
        spec.stub_tools         = {set_deploy_region};
        spec.trial_id           = "live-t1";
        spec.max_turns          = 4;
        spec.extra_capabilities = secret_cap;

        openai::OpenAIChatClient client(host, 443, model, SecretRef{kSecretName}, caps, secret_store, prefix,
                                         sandbox::resolve_host, /*ca=*/{}, /*http_referer=*/{},
                                         /*x_title=*/"AgentEngine Eval Trial Live Sanity Check",
                                         /*end_user_id=*/"test-eval-trial-driver-live");
        auto result = drive(eval::run_trial(std::move(client), NoopSummarizerClient{}, spec));
        check(!result.setup_error.has_value(), "T: no setup error");
        check(result.outcome.has_value(), "T: the treatment trial converges");
        print_result("T", result);
        print_transcript("T", result);
    }

    std::fprintf(stderr, g_failures == 0 ? "test_eval_trial_driver_live_e2e: OK\n"
                                          : "test_eval_trial_driver_live_e2e: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
