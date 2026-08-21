// Standalone, non-interactive batch-inference CLI: submits N independent single-shot prompts to
// OpenRouter's real Batch API (beta) and polls to completion. docs/research/2026-08-21-openrouter-
// batch-api.md is the fetched-and-cited source for the wire shape this file speaks.
//
// SCOPE, DELIBERATELY NARROW: this tool has NO AgentSession/StandingEffect/workflow integration at
// all -- it never touches `ChatClientCapabilities::batch`, `start_background_task()`, or any of
// AgentSession's run loop. docs/planning/batch-inference-coalescing-gap.md is an explicit, standing
// project-owner directive to NOT wire vendor batch mode into AgentEngine's own agentic loop without a
// future ADR (six real open design questions listed there: coalescing-coordinator ownership, result
// fan-out, cross-tenant custom_id attribution, durable polling, and more). A tool that just submits a
// batch of independent prompts and prints the results sits outside that gate entirely -- it is direct
// vendor batch usage, not agentic-loop coalescing, and needs no ADR to exist.
//
// "ALL PROVIDERS": OpenRouter's batch endpoint is pinned to exactly one wire shape per job
// (`endpoint` is a batch-level field, confirmed against the real docs page). This tool submits TWO
// separate batch jobs -- one at `/v1/chat/completions` (OpenAI wire shape) and one at `/v1/messages`
// (Anthropic wire shape) -- each built with the SAME per-item request-body translation this project's
// own `OpenAIChatClient`/`AnthropicChatClient` already use for their synchronous calls
// (`openai::detail::build_request_body` / `anthropic::detail::build_request_body`), proving that
// translation is valid for the batch endpoint's `body` field too, not just the synchronous one.
//
// `kDefaultModel` below is NOT the alias tests/test_openrouter_live_e2e.cpp uses for its synchronous
// calls (`~deepseek/deepseek-v4-flash-latest`) -- confirmed live, 2026-08-21, that alias 400s on
// submit with "does not have a :batch endpoint." Batch-endpoint support is a real, separate,
// per-model capability the synchronous path says nothing about. `openai/gpt-4o-mini` is confirmed
// live, same date, to accept :batch on BOTH `/v1/chat/completions` and `/v1/messages`.
//
// Real OpenAI's/Anthropic's OWN direct batch endpoints (Files-API upload, Message Batches) are NOT
// implemented here: this workstation has no real api.openai.com/api.anthropic.com credential to
// verify them against (only AGENTENGINE_OPENROUTER_API_KEY / api-test.txt), and CLAUDE.md's "a test
// that cannot fail proves nothing" discipline argues against shipping code no live run can check.
// Left as documented future work.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4): AGENTENGINE_OPENROUTER_API_KEY, read at run time.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"

using namespace agentengine;

namespace {

// Confirmed live (2026-08-21) to accept OpenRouter's :batch endpoint on BOTH wire shapes this tool
// submits -- see this file's own top comment for why this differs from the synchronous-path default.
constexpr char const* kDefaultModel = "openai/gpt-4o-mini";
constexpr char const* kHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kBatchesPath = "/api/beta/batches";

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

[[nodiscard]] Message user_message(std::string text, std::string message_id) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role       = role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string extract_reply_text(Message const& m) {
    std::string out;
    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

struct BatchJob {
    std::string endpoint;   // "/v1/chat/completions" or "/v1/messages"
    std::string label;      // "openai-shape" / "anthropic-shape" -- for progress output only
    json::Value body;       // the full POST /api/beta/batches request body
};

// Builds one prompt's `{"custom_id","body"}` entry for the OpenAI wire shape, reusing the exact
// translation `OpenAIChatClient::chat()` itself calls.
[[nodiscard]] result<json::Value> openai_shaped_item(std::string const& custom_id,
                                                       std::string const& prompt_text,
                                                       std::string const& model) {
    ChatRequest req;
    req.messages.push_back(user_message(prompt_text, custom_id));
    auto body = openai::detail::build_request_body(req, model, /*stream=*/false);
    if (!body) return std::unexpected(body.error());
    std::vector<std::pair<std::string, json::Value>> item{
        {"custom_id", json::Value::make_string(custom_id)},
        {"body", std::move(*body)},
    };
    return json::Value::make_object(std::move(item));
}

// Same, for the Anthropic wire shape -- `max_tokens` must be nonzero or the vendor rejects the
// request, hence the explicit capability below (mirrors AnthropicChatClient's own default handling).
[[nodiscard]] result<json::Value> anthropic_shaped_item(std::string const& custom_id,
                                                          std::string const& prompt_text,
                                                          std::string const& model) {
    ChatRequest req;
    req.messages.push_back(user_message(prompt_text, custom_id));
    ChatClientCapabilities caps;
    caps.max_output_tokens = 512;
    auto body = anthropic::detail::build_request_body(req, model, caps, /*stream=*/false);
    if (!body) return std::unexpected(body.error());
    std::vector<std::pair<std::string, json::Value>> item{
        {"custom_id", json::Value::make_string(custom_id)},
        {"body", std::move(*body)},
    };
    return json::Value::make_object(std::move(item));
}

[[nodiscard]] result<BatchJob> build_job(std::string endpoint, std::string label, std::string model,
                                          std::vector<std::string> const& prompts,
                                          std::string const& custom_id_prefix) {
    std::vector<json::Value> requests;
    requests.reserve(prompts.size());
    for (std::size_t i = 0; i < prompts.size(); ++i) {
        std::string const custom_id = custom_id_prefix + "-" + std::to_string(i);
        auto item = (endpoint == "/v1/messages") ? anthropic_shaped_item(custom_id, prompts[i], model)
                                                   : openai_shaped_item(custom_id, prompts[i], model);
        if (!item) return std::unexpected(item.error());
        requests.push_back(std::move(*item));
    }
    std::vector<std::pair<std::string, json::Value>> top{
        {"endpoint", json::Value::make_string(endpoint)},
        {"model", json::Value::make_string(model)},
        {"requests", json::Value::make_array(std::move(requests))},
    };
    BatchJob job;
    job.endpoint = std::move(endpoint);
    job.label    = std::move(label);
    job.body     = json::Value::make_object(std::move(top));
    return job;
}

[[nodiscard]] sandbox::NetEgressRequest post_request(std::string path, std::string const& api_key,
                                                       std::string body) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path   = std::move(path);
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    req.body = std::move(body);
    return req;
}

[[nodiscard]] sandbox::NetEgressRequest get_request(std::string path, std::string const& api_key) {
    sandbox::NetEgressRequest req;
    req.method = "GET";
    req.path   = std::move(path);
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    return req;
}

// Polls GET /api/beta/batches/:id until a terminal status, printing progress. Caps total wait at
// ~10 minutes with backoff -- generous for a handful of prompts, far short of the vendor's 24h
// completion-window ceiling, and this is a manually-invoked tool, not a ctest fixture that must
// finish in seconds.
[[nodiscard]] result<json::Value> poll_until_done(std::string const& api_key, std::string const& id,
                                                    char const* label) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    // A batch job just submitted is occasionally not yet visible to GET for the first second or two
    // (observed live, 2026-08-21: the create call itself returns 200 immediately, but an immediate
    // follow-up poll can 404 "Batch job ... not found" before the backing store catches up) --
    // eventual-consistency lag on a beta API, not a real error. Give that its own short, separate
    // grace window rather than treating any 404 as terminal.
    auto const not_found_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::chrono::seconds backoff{5};
    for (;;) {
        auto req  = get_request(std::string(kBatchesPath) + "/" + id, api_key);
        auto resp = sandbox::perform_provider_https_exchange(kHost, kHttpsPort, req);
        if (!resp) return std::unexpected(resp.error());
        if (resp->status == 404 && std::chrono::steady_clock::now() < not_found_deadline) {
            std::fprintf(stderr, "  [%s] batch %s: not yet visible (404), retrying...\n", label,
                          id.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (resp->status < 200 || resp->status >= 300) {
            return std::unexpected(error{failure_class::fatal,
                                          "[" + std::string(label) + "] poll http status " +
                                              std::to_string(resp->status) + ": " + resp->body,
                                          "batch_infer.poll_http_error"});
        }
        auto parsed = json::parse(resp->body);
        if (!parsed) return std::unexpected(parsed.error());
        auto const* status = parsed->find("status");
        std::string const status_str = (status && status->is_string()) ? status->as_string() : "unknown";
        std::fprintf(stderr, "  [%s] batch %s: status=%s\n", label, id.c_str(), status_str.c_str());
        if (status_str == "completed") return parsed;
        if (status_str == "failed" || status_str == "expired" || status_str == "cancelled") {
            return std::unexpected(error{failure_class::fatal,
                                          "[" + std::string(label) + "] batch ended in status '" +
                                              status_str + "'",
                                          "batch_infer.batch_terminal_failure"});
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(
                error{failure_class::transient,
                      "[" + std::string(label) +
                          "] batch did not complete within this tool's 10-minute poll budget "
                          "(vendor completion window is 24h -- this is a wait-time cap, not a "
                          "failure verdict)",
                      "batch_infer.poll_budget_exceeded"});
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::seconds(30));
    }
}

void print_results(json::Value const& completed_batch, char const* label, bool anthropic_shape) {
    auto const* results = completed_batch.find("results");
    if (!results || !results->is_array()) {
        std::fprintf(stderr, "  [%s] no results array in the completed batch response\n", label);
        return;
    }
    for (json::Value const& item : results->as_array()) {
        auto const* custom_id = item.find("custom_id");
        std::string const id = (custom_id && custom_id->is_string()) ? custom_id->as_string() : "?";
        auto const* err = item.find("error");
        if (err && !err->is_null()) {
            std::fprintf(stderr, "  [%s] %s -> ERROR: %s\n", label, id.c_str(), json::dump(*err).c_str());
            continue;
        }
        auto const* response = item.find("response");
        auto const* resp_body = response ? response->find("body") : nullptr;
        if (!resp_body) {
            std::fprintf(stderr, "  [%s] %s -> no response body in result item\n", label, id.c_str());
            continue;
        }
        auto parsed_response = anthropic_shape ? anthropic::detail::parse_message_response(*resp_body)
                                                : openai::detail::parse_chat_completion_response(*resp_body);
        if (!parsed_response) {
            std::fprintf(stderr, "  [%s] %s -> failed to parse response: %s\n", label, id.c_str(),
                          parsed_response.error().message.c_str());
            continue;
        }
        std::fprintf(stderr, "  [%s] %s -> \"%s\" (in=%llu out=%llu)\n", label, id.c_str(),
                      extract_reply_text(parsed_response->message).c_str(),
                      static_cast<unsigned long long>(parsed_response->usage.input_tokens),
                      static_cast<unsigned long long>(parsed_response->usage.output_tokens));
    }
}

[[nodiscard]] int run_job(std::string const& api_key, BatchJob const& job) {
    std::fprintf(stderr, "\n== %s (%s) ==\n", job.label.c_str(), job.endpoint.c_str());
    auto req  = post_request(kBatchesPath, api_key, json::dump(job.body));
    auto resp = sandbox::perform_provider_https_exchange(kHost, kHttpsPort, req);
    if (!resp) {
        std::fprintf(stderr, "  [%s] submit failed: %s\n", job.label.c_str(), resp.error().message.c_str());
        return 1;
    }
    if (resp->status < 200 || resp->status >= 300) {
        std::fprintf(stderr, "  [%s] submit http status %u: %s\n", job.label.c_str(), resp->status,
                      resp->body.c_str());
        return 1;
    }
    auto parsed = json::parse(resp->body);
    if (!parsed) {
        std::fprintf(stderr, "  [%s] submit response did not parse as JSON: %s\n", job.label.c_str(),
                      parsed.error().message.c_str());
        return 1;
    }
    auto const* id = parsed->find("id");
    if (!id || !id->is_string()) {
        std::fprintf(stderr, "  [%s] submit response carries no string 'id': %s\n", job.label.c_str(),
                      resp->body.c_str());
        return 1;
    }
    std::fprintf(stderr, "  [%s] submitted: id=%s\n", job.label.c_str(), id->as_string().c_str());

    auto completed = poll_until_done(api_key, id->as_string(), job.label.c_str());
    if (!completed) {
        std::fprintf(stderr, "  [%s] %s\n", job.label.c_str(), completed.error().message.c_str());
        return 1;
    }
    print_results(*completed, job.label.c_str(), job.endpoint == "/v1/messages");
    return 0;
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "AGENTENGINE_OPENROUTER_API_KEY is not set. Export a real OpenRouter API key "
                      "and re-run (e.g. via tools/run-live-provider-tests.ps1's own api-test.txt "
                      "loading convention).\n");
        return 1;
    }
    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);

    std::vector<std::string> const prompts = {
        "Name the capital of France in one word.",
        "What is 2 + 2? Answer with just the number.",
        "Name one primary color.",
    };

    auto openai_job = build_job("/v1/chat/completions", "openai-shape", model, prompts, "batch-oai");
    if (!openai_job) {
        std::fprintf(stderr, "failed to build the openai-shape job: %s\n", openai_job.error().message.c_str());
        return 1;
    }
    auto anthropic_job = build_job("/v1/messages", "anthropic-shape", model, prompts, "batch-ant");
    if (!anthropic_job) {
        std::fprintf(stderr, "failed to build the anthropic-shape job: %s\n",
                      anthropic_job.error().message.c_str());
        return 1;
    }

    std::fprintf(stderr, "AgentEngine batch inference -- host=%s model=%s prompts=%zu\n", kHost,
                  model.c_str(), prompts.size());

    int failures = 0;
    failures += run_job(*key_env, *openai_job);
    failures += run_job(*key_env, *anthropic_job);

    std::fprintf(stderr, "\nbatch_infer: %s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
