// decisions/ADR-235-batch-inference-coalescing.md §3.2 -- the OpenRouter `BatchBackend`'s non-template
// wire code. Wire facts and the deliberately narrow admission rule are documented at the declarations in
// include/agentengine/protocol/openai/openrouter_batch_backend.hpp.

#include "agentengine/protocol/openai/openrouter_batch_backend.hpp"

#include <string_view>
#include <variant>

namespace agentengine::openai::detail {

namespace {

// The longest custom id the engine mints for one job (`"i" + index`, index < kOpenRouterBatchMaxItems).
constexpr std::string_view kWidestCustomId = "i49999";

[[nodiscard]] error refuse(std::string message) {
    return error{failure_class::contract, std::move(message), "openrouter_batch.not_batchable"};
}

[[nodiscard]] std::string string_field(json::Value const& v, std::string_view key) {
    auto const* f = v.find(key);
    return (f && f->is_string()) ? f->as_string() : std::string{};
}

[[nodiscard]] failure_class class_of_status_code(double code) {
    if (code == 429 || code >= 500) return failure_class::transient;
    if (code == 401 || code == 403) return failure_class::policy;
    return failure_class::contract;
}

}  // namespace

result<std::size_t> openrouter_batch_admit(ChatRequest const& request, std::string const& model) {
    if (request.messages.empty()) return std::unexpected(refuse("a batched request needs at least one message"));
    if (!request.tools.empty()) {
        return std::unexpected(refuse("tools are not batched on OpenRouter (unknown parameters are silently dropped)"));
    }
    if (request.output_schema_json) return std::unexpected(refuse("an output schema is not batched on OpenRouter"));
    if (request.reasoning_effort) return std::unexpected(refuse("a reasoning effort is not batched on OpenRouter"));
    if (!request.client_interaction_answers.empty()) {
        return std::unexpected(refuse("client interaction answers are not batchable"));
    }
    for (Message const& m : request.messages) {
        for (ContentItem const& item : m.content) {
            if (!std::holds_alternative<Text>(item.value)) {
                return std::unexpected(refuse("only text content is batched on OpenRouter (images/files must be "
                                              "public URLs there, and tool turns imply a loop)"));
            }
        }
    }
    auto body = build_request_body(request, model, /*stream=*/false);
    if (!body) return std::unexpected(body.error());
    std::vector<std::pair<std::string, json::Value>> item{
        {"custom_id", json::Value::make_string(std::string(kWidestCustomId))},
        {"body", std::move(*body)},
    };
    return json::dump(json::Value::make_object(std::move(item))).size() + 1;  // + the array separator
}

result<json::Value> build_openrouter_batch_body(std::string const& model, std::vector<BatchItemRequest> const& items) {
    if (items.empty()) {
        return std::unexpected(error{failure_class::contract, "a batch needs at least one request",
                                     "openrouter_batch.empty"});
    }
    std::vector<json::Value> requests;
    requests.reserve(items.size());
    for (BatchItemRequest const& it : items) {
        auto body = build_request_body(it.request, model, /*stream=*/false);
        if (!body) return std::unexpected(body.error());
        std::vector<std::pair<std::string, json::Value>> item{
            {"custom_id", json::Value::make_string(it.custom_id)},
            {"body", std::move(*body)},
        };
        requests.push_back(json::Value::make_object(std::move(item)));
    }
    // `endpoint` and `model` MUST precede `requests` (OpenRouter stream-parses the body; 400 otherwise).
    std::vector<std::pair<std::string, json::Value>> top{
        {"endpoint", json::Value::make_string("/v1/chat/completions")},
        {"model", json::Value::make_string(model)},
        {"requests", json::Value::make_array(std::move(requests))},
    };
    return json::Value::make_object(std::move(top));
}

result<std::string> parse_openrouter_batch_id(json::Value const& body) {
    std::string id = string_field(body, "id");
    if (id.empty()) {
        return std::unexpected(error{failure_class::contract, "batch create response carries no string id",
                                     "openrouter_batch.no_id"});
    }
    return id;
}

result<BatchPoll> parse_openrouter_batch_object(json::Value const& body, std::string const& producer_chat_client_id) {
    std::string const status = string_field(body, "status");
    if (status.empty()) {
        return std::unexpected(error{failure_class::contract, "batch object carries no status",
                                     "openrouter_batch.no_status"});
    }
    BatchPoll poll;
    poll.ended = status == "completed" || status == "failed" || status == "expired" || status == "cancelled";
    if (auto const* err = body.find("error"); err && err->is_object()) poll.detail = string_field(*err, "message");
    if (poll.detail.empty() && poll.ended && status != "completed") poll.detail = "batch " + status;

    auto const* results = body.find("results");
    if (results == nullptr || !results->is_array()) return poll;  // null unless `completed`
    for (json::Value const& r : results->as_array()) {
        BatchItemResult item;
        item.custom_id = string_field(r, "custom_id");
        if (item.custom_id.empty()) continue;  // unmatchable; the engine counts what it cannot place
        if (auto const* err = r.find("error"); err && !err->is_null()) {
            item.status = batch_item_status::errored;
            item.detail = err->is_object() ? string_field(*err, "message") : json::dump(*err);
            double code = 0;
            if (err->is_object()) {
                if (auto const* c = err->find("code"); c && c->is_number()) code = c->as_number();
            }
            item.klass = code > 0 ? class_of_status_code(code) : failure_class::transient;
            poll.items.push_back(std::move(item));
            continue;
        }
        auto const* response = r.find("response");
        auto const* status_code = response ? response->find("status_code") : nullptr;
        auto const* resp_body = response ? response->find("body") : nullptr;
        double const code = (status_code && status_code->is_number()) ? status_code->as_number() : 200;
        if (resp_body == nullptr || code < 200 || code >= 300) {
            item.status = batch_item_status::errored;
            item.klass  = class_of_status_code(code);
            item.detail = "result item has no response body (status_code " + std::to_string(static_cast<int>(code)) + ")";
            poll.items.push_back(std::move(item));
            continue;
        }
        auto parsed = parse_chat_completion_response(*resp_body, producer_chat_client_id);
        if (!parsed) {
            item.status = batch_item_status::errored;
            item.klass  = failure_class::contract;
            item.detail = parsed.error().message;
        } else {
            item.status   = batch_item_status::succeeded;
            item.response = std::move(*parsed);
        }
        poll.items.push_back(std::move(item));
    }
    return poll;
}

sandbox::NetEgressRequest openrouter_batch_request(std::string method, std::string path, std::string const& api_key,
                                                   std::string body) {
    sandbox::NetEgressRequest req;
    req.method = std::move(method);
    req.path   = std::move(path);
    if (!body.empty()) req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    req.body = std::move(body);
    return req;
}

}  // namespace agentengine::openai::detail
