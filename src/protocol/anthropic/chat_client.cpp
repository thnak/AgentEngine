// Implements 004-Model-Provider-Plane.md §3 (the Anthropic Messages ChatClient backend); ADR-206 (#120 S6).
// The non-template bodies of include/agentengine/protocol/anthropic/chat_client.hpp, moved verbatim out of the
// header so they are compiled once here instead of in every file that includes it. The header keeps every
// declaration and comment, the small helpers, and the AnthropicChatClient<Store> template; the design and the
// rules each body implements are documented at its declaration there.

#include "agentengine/protocol/anthropic/chat_client.hpp"

#include "../sse_stream_pump.hpp"

namespace agentengine::anthropic::detail {

json::Value translate_tool_use_input(std::string const& arguments_json) {
    auto parsed = json::parse(arguments_json);
    // Malformed input -- send an empty object. So is a non-object (issue #112 B2): the wire requires an object, and
    // such a call was refused at step 2 (its result in the same history says so).
    if (!parsed || !parsed->is_object()) return json::Value::make_object({});
    // ADR-191: parsing turns a JSON escape of a reserved bracket glyph into the real glyph, which this block then sends
    // raw. Only then is the parsed value re-dumped (json::dump writes non-ASCII raw), cleaned and parsed again -- an
    // input without the glyphs is sent exactly as parsed (round 3: rewriting the escapes in the text corrupted it).
    std::string const dumped = json::dump(*parsed);
    if (dumped.find("\xE2\x9F\xA6") == std::string::npos && dumped.find("\xE2\x9F\xA7") == std::string::npos) {
        return std::move(*parsed);
    }
    auto cleaned = json::parse(neutralize_outbound_text(dumped));
    if (!cleaned) return json::Value::make_object({});
    return std::move(*cleaned);
}

SplitMessages split_system_messages(std::vector<Message> const& messages,
                                    std::string request_code) {
    if (request_code.empty() && has_fenced_approved_lesson(messages)) request_code = new_request_approval_code();
    SplitMessages out;
    auto append_fragment = [&out](std::string const& text) {
        if (text.empty()) return;
        if (!out.system_text.empty()) out.system_text += "\n\n";
        out.system_text += text;
    };
    for (Message const& m : messages) {
        if (m.role == role::system) {
            for (ContentItem const& item : m.content) {
                auto const* t = std::get_if<Text>(&item.value);
                if (t == nullptr) continue;
                // ADR-173 (issue #61): a tainted item is fenced before it joins the blob. Emptiness
                // is checked by `needs_system_channel_fence` itself, so an empty tainted item still
                // contributes nothing at all rather than a content-free marker pair.
                if (needs_system_channel_fence(m.role, item)) {
                    append_fragment(fence_untrusted_text(t->text, item.origin, request_code, !item.approval.empty()));
                } else {
                    append_fragment(neutralize_outbound_text(t->text));  // ADR-191: no marker outside a real fence
                }
            }
        } else {
            out.rest.push_back(&m);
        }
    }
    // ADR-173: the reading rule goes FIRST, ahead of the agent's own instructions and everything
    // else — it explains markers that appear later, and nothing tainted can get above it (fenced
    // content is, by construction, inside a fence emitted after this point). Only emitted when
    // there is something to explain, so a request with no tainted system content gets no preamble and
    // no fence (its text still loses the raw bracket glyphs, ADR-191 §3.5).
    if (!out.system_text.empty() && has_fenced_system_content(messages)) {
        std::string prefixed = untrusted_fence_preamble_for(messages, request_code);  // ADR-191: + its sentence
        prefixed += "\n\n";
        prefixed += out.system_text;
        out.system_text = std::move(prefixed);
    }
    return out;
}

json::Value translate_message(Message const& m) {
    std::vector<json::Value> blocks;

    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("text")},
                {"text", json::Value::make_string(neutralize_outbound_text(t->text))},
            };
            blocks.push_back(json::Value::make_object(std::move(block)));
        } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("tool_use")},
                {"id", json::Value::make_string(tc->call_id)},
                {"name", json::Value::make_string(tc->tool_name)},
                {"input", translate_tool_use_input(tc->arguments_json)},  // ADR-191: cleaned after parsing
            };
            blocks.push_back(json::Value::make_object(std::move(block)));
        } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
            std::string content_text;
            for (ContentItem const& inner : tr->content) {
                if (auto const* it = std::get_if<Text>(&inner.value)) {
                    content_text += it->text;
                } else if (auto const* d = std::get_if<Data>(&inner.value)) {
                    content_text += d->json;
                } else if (auto const* e = std::get_if<Error>(&inner.value)) {
                    content_text += e->message;
                }
            }
            content_text = neutralize_outbound_text(content_text);  // ADR-191: after the parts are joined
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("tool_result")},
                {"tool_use_id", json::Value::make_string(tr->call_id)},
                {"content", json::Value::make_string(content_text)},
            };
            if (tr->is_error) block.emplace_back("is_error", json::Value::make_bool(true));
            blocks.push_back(json::Value::make_object(std::move(block)));
        }
        // Reasoning/Media/Citation/Custom: not translated outbound (Reasoning per file banner (3);
        // the rest is the same "narrower than the full content model" gap Phase D's OpenAI backend
        // already names for its own translate_message).
    }

    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(role_to_wire(m.role))));
    obj.emplace_back("content", json::Value::make_array(std::move(blocks)));
    return json::Value::make_object(std::move(obj));
}

json::Value make_cache_control(std::string const& cache_ttl) {
    std::vector<std::pair<std::string, json::Value>> cc{
        {"type", json::Value::make_string("ephemeral")},
    };
    if (!cache_ttl.empty()) cc.emplace_back("ttl", json::Value::make_string(cache_ttl));
    return json::Value::make_object(std::move(cc));
}

result<json::Value> translate_tool(ToolDescriptor const& t, bool cache_this_one,
                                   std::string const& cache_ttl) {
    json::Value schema;
    if (t.args_schema_value_cached) {
        schema = t.args_schema_value;
    } else {
        auto parsed_schema = json::parse(t.args_schema_json);
        if (!parsed_schema) return std::unexpected(parsed_schema.error());
        schema = std::move(*parsed_schema);
    }
    std::vector<std::pair<std::string, json::Value>> obj{
        {"name", json::Value::make_string(t.name)},
        {"description", json::Value::make_string(neutralize_outbound_text(t.description))},  // ADR-191
        {"input_schema", std::move(schema)},
    };
    if (cache_this_one) {
        obj.emplace_back("cache_control", make_cache_control(cache_ttl));
    }
    return json::Value::make_object(std::move(obj));
}

result<json::Value> translate_output_config(std::string const& schema_json) {
    auto parsed = json::parse(schema_json);
    if (!parsed) return std::unexpected(parsed.error());
    std::vector<std::pair<std::string, json::Value>> format{
        {"type", json::Value::make_string("json_schema")},
        {"schema", std::move(*parsed)},
    };
    std::vector<std::pair<std::string, json::Value>> output_config{
        {"format", json::Value::make_object(std::move(format))},
    };
    return json::Value::make_object(std::move(output_config));
}

std::size_t count_cache_control_blocks(json::Value const& v) {
    std::size_t count = 0;
    if (v.is_object()) {
        for (auto const& [key, member] : v.as_object()) {
            if (key == "cache_control") ++count;
            count += count_cache_control_blocks(member);
        }
    } else if (v.is_array()) {
        for (auto const& item : v.as_array()) count += count_cache_control_blocks(item);
    }
    return count;
}

std::uint64_t thinking_budget_for(reasoning_effort effort,
                                  std::uint64_t max_tokens) noexcept {
    std::uint64_t pct = 50;
    switch (effort) {
        case reasoning_effort::low:    pct = 25; break;
        case reasoning_effort::medium: pct = 50; break;
        case reasoning_effort::high:   pct = 75; break;
        case reasoning_effort::off:    return 0;  // caller emits `{"type":"disabled"}`, no budget
    }
    std::uint64_t const scaled = (max_tokens / 100) * pct;
    return scaled < kMinThinkingBudgetTokens ? kMinThinkingBudgetTokens : scaled;
}

result<json::Value> translate_reasoning_effort(reasoning_effort effort,
                                               std::uint64_t max_tokens) {
    if (effort == reasoning_effort::off) {
        std::vector<std::pair<std::string, json::Value>> disabled{
            {"type", json::Value::make_string("disabled")},
        };
        return json::Value::make_object(std::move(disabled));
    }
    std::uint64_t const budget = thinking_budget_for(effort, max_tokens);
    if (budget >= max_tokens) {
        // Names the actual numbers and the remedy. This branch is NOT a contrived boundary -- it fired
        // on the first real configuration it met (the live suite declares `max_output_tokens = 1024` to
        // bound cost, and Anthropic's thinking floor is exactly 1024), so an operator reading this needs
        // to know it is a vendor floor they must raise past, not a bug to report.
        return std::unexpected(error{
            failure_class::contract,
            "reasoning effort is unsatisfiable: Anthropic requires budget_tokens >= " +
                std::to_string(kMinThinkingBudgetTokens) + " and budget_tokens < max_tokens, but this "
                "backend declares max_output_tokens = " + std::to_string(max_tokens) +
                " -- raise it above " + std::to_string(kMinThinkingBudgetTokens) +
                " to use any reasoning effort level above `off`",
            "anthropic.thinking_budget_unsatisfiable"});
    }
    std::vector<std::pair<std::string, json::Value>> enabled{
        {"type", json::Value::make_string("enabled")},
        {"budget_tokens", json::Value::make_number(static_cast<double>(budget))},
    };
    return json::Value::make_object(std::move(enabled));
}

result<json::Value> build_request_body(ChatRequest const& request,
                                       std::string const& model,
                                       ChatClientCapabilities const& caps,
                                       bool stream,
                                       std::string const& end_user_id,
                                       std::string const& cache_ttl) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("model", json::Value::make_string(model));
    std::uint64_t const max_tokens =
        caps.max_output_tokens != 0 ? caps.max_output_tokens : kDefaultMaxTokens;
    obj.emplace_back("max_tokens", json::Value::make_number(static_cast<double>(max_tokens)));

    SplitMessages split = split_system_messages(request.messages);
    if (!split.system_text.empty()) {
        if (caps.prompt_caching) {
            std::vector<std::pair<std::string, json::Value>> block{
                {"type", json::Value::make_string("text")},
                {"text", json::Value::make_string(split.system_text)},
                {"cache_control", make_cache_control(cache_ttl)},
            };
            std::vector<json::Value> system_blocks;
            system_blocks.push_back(json::Value::make_object(std::move(block)));
            obj.emplace_back("system", json::Value::make_array(std::move(system_blocks)));
        } else {
            obj.emplace_back("system", json::Value::make_string(split.system_text));
        }
    }

    std::vector<json::Value> messages;
    messages.reserve(split.rest.size());
    for (Message const* m : split.rest) messages.push_back(translate_message(*m));
    obj.emplace_back("messages", json::Value::make_array(std::move(messages)));

    if (!request.tools.empty()) {
        std::vector<json::Value> tools;
        tools.reserve(request.tools.size());
        for (std::size_t i = 0; i < request.tools.size(); ++i) {
            bool const is_last = (i + 1 == request.tools.size());
            auto tool_json = translate_tool(request.tools[i], caps.prompt_caching && is_last, cache_ttl);
            if (!tool_json) return std::unexpected(tool_json.error());
            tools.push_back(std::move(*tool_json));
        }
        obj.emplace_back("tools", json::Value::make_array(std::move(tools)));
    }

    if (request.output_schema_json) {
        auto output_config = translate_output_config(*request.output_schema_json);
        if (!output_config) return std::unexpected(output_config.error());
        obj.emplace_back("output_config", std::move(*output_config));
    }

    if (!end_user_id.empty()) {
        std::vector<std::pair<std::string, json::Value>> metadata{
            {"user_id", json::Value::make_string(end_user_id)},
        };
        obj.emplace_back("metadata", json::Value::make_object(std::move(metadata)));
    }

    // ADR-020. `nullopt` emits nothing at all -- the vendor default, and today's exact behaviour.
    if (request.reasoning_effort.has_value()) {
        // 004 §2's degradation rule, identically to the OpenAI backend: no DECLARED fallback for
        // reasoning effort exists, so a backend that cannot reason refuses rather than dropping the
        // field. `off` is exempt -- a backend without the bit satisfies "do not reason" already.
        if (*request.reasoning_effort != reasoning_effort::off && !caps.reasoning) {
            return std::unexpected(error{
                failure_class::contract,
                "request asks for a reasoning effort level but this backend does not declare the "
                "`reasoning` capability (004 §2: no declared fallback exists)",
                "anthropic.reasoning_not_supported"});
        }
        auto thinking = translate_reasoning_effort(*request.reasoning_effort, max_tokens);
        if (!thinking) return std::unexpected(thinking.error());
        obj.emplace_back("thinking", std::move(*thinking));
    }

    if (stream) obj.emplace_back("stream", json::Value::make_bool(true));

    json::Value body = json::Value::make_object(std::move(obj));
    if (count_cache_control_blocks(body) > kMaxCacheControlBlocks) {
        return std::unexpected(error{failure_class::contract,
                                      "request would carry more than 4 cache_control blocks combined "
                                      "(system+tools+messages) -- Anthropic rejects this with HTTP 400",
                                      "anthropic.cache_control_limit_exceeded"});
    }
    return body;
}

sandbox::NetEgressRequest build_http_request(std::string const& path,
                                             std::string const& api_key,
                                             std::string const& api_version,
                                             std::string body,
                                             std::string const& http_referer,
                                             std::string const& x_title,
                                             std::string const& session_id) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path = path;
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("x-api-key", api_key);
    req.headers.emplace_back("anthropic-version", api_version);
    if (!http_referer.empty()) req.headers.emplace_back("HTTP-Referer", http_referer);
    if (!x_title.empty()) req.headers.emplace_back("X-Title", x_title);
    if (!session_id.empty()) req.headers.emplace_back("x-session-id", session_id);
    req.body = std::move(body);
    return req;
}

std::optional<ContentItem> translate_response_block(
    json::Value const& block, std::string const& producer_chat_client_id) {
    auto const* type = block.find("type");
    if (!type || !type->is_string()) return std::nullopt;
    std::string const& kind = type->as_string();

    ContentItem item;
    item.origin = content_origin::assistant;

    if (kind == "text") {
        auto const* text = block.find("text");
        item.value = Text{(text && text->is_string()) ? text->as_string() : std::string{}};
        return item;
    }
    if (kind == "tool_use") {
        auto const* id = block.find("id");
        auto const* name = block.find("name");
        auto const* input = block.find("input");
        ToolCall call;
        call.call_id = (id && id->is_string()) ? id->as_string() : std::string{};
        call.tool_name = (name && name->is_string()) ? name->as_string() : std::string{};
        // Issue #112 B2 (ADR-197 §5): a missing `input` is no arguments; any present value is kept as its JSON text,
        // so a non-object `input` is refused at step 2 as `tool.malformed_arguments` (`tool_call_request_of`).
        call.arguments_json = input ? json::dump(*input) : std::string{"{}"};
        item.value = std::move(call);
        return item;
    }
    if (kind == "thinking") {
        // (3): signature is intentionally not preserved -- this content item is response-parsing-only.
        auto const* thinking = block.find("thinking");
        Reasoning r;
        r.text = (thinking && thinking->is_string()) ? thinking->as_string() : std::string{};
        r.encrypted = false;
        r.producer_chat_client_id = producer_chat_client_id;
        item.value = std::move(r);
        return item;
    }
    if (kind == "redacted_thinking") {
        Reasoning r;
        r.text.clear();  // opaque `data` intentionally dropped, see file banner (3)
        r.encrypted = true;
        r.producer_chat_client_id = producer_chat_client_id;
        item.value = std::move(r);
        return item;
    }
    return std::nullopt;  // server tool / container / other block kinds -- not translated
}

result<ChatResponse> parse_message_response(
    json::Value const& body, std::string const& producer_chat_client_id) {
    if (auto const* err = body.find("error")) {
        std::string msg = "unknown error";
        if (auto const* m = err->find("message"); m && m->is_string()) msg = m->as_string();
        return std::unexpected(error{failure_class::contract, "anthropic error: " + msg, "anthropic.error"});
    }
    json::Value const* content = body.find("content");
    if (!content || !content->is_array()) {
        return std::unexpected(
            error{failure_class::contract, "response has no content array", "anthropic.no_content"});
    }

    ChatResponse resp;
    resp.message.role = role::assistant;
    for (json::Value const& block : content->as_array()) {
        if (auto item = translate_response_block(block, producer_chat_client_id)) {
            resp.message.content.push_back(std::move(*item));
        }
    }

    // M5 research follow-up item 4 (docs/research/2026-08-07-provider-metadata-and-sampling-params-
    // survey.md Finding 4): the model that ACTUALLY answered -- a sibling of "content" at the top level,
    // empty (never fabricated from the request's own model_ field) when the response doesn't report one.
    if (auto const* model_field = body.find("model"); model_field && model_field->is_string()) {
        resp.model = model_field->as_string();
    }

    if (auto const* usage = body.find("usage")) {
        if (auto const* it = usage->find("input_tokens"); it && it->is_number()) {
            resp.usage.input_tokens = static_cast<std::uint64_t>(it->as_number());
        }
        if (auto const* ot = usage->find("output_tokens"); ot && ot->is_number()) {
            resp.usage.output_tokens = static_cast<std::uint64_t>(ot->as_number());
        }
        if (auto const* crt = usage->find("cache_read_input_tokens"); crt && crt->is_number()) {
            resp.usage.cached_input_tokens = static_cast<std::uint64_t>(crt->as_number());
        }
        // M5 research follow-up item 5 (Finding 5): cache_creation_input_tokens -- tokens spent
        // ESTABLISHING a new cache entry, the symmetric counterpart to cache_read_input_tokens above --
        // now mapped to Usage::cache_write_tokens (previously a named, unmapped gap in this same
        // comment).
        if (auto const* cct = usage->find("cache_creation_input_tokens"); cct && cct->is_number()) {
            resp.usage.cache_write_tokens = static_cast<std::uint64_t>(cct->as_number());
        }
    }

    return resp;
}

error map_http_status_error(std::uint16_t status, std::string const& body) {
    return agentengine::detail::provider_wire::http_status_error("anthropic", status, body);
}

void seed_usage_from_message_start(AnthropicUsageSnapshot& snapshot, json::Value const& usage) {
    if (auto const* it = usage.find("input_tokens"); it && it->is_number()) {
        snapshot.input_tokens = static_cast<std::uint64_t>(it->as_number());
    }
    if (auto const* ot = usage.find("output_tokens"); ot && ot->is_number()) {
        snapshot.output_tokens = static_cast<std::uint64_t>(ot->as_number());
    }
    if (auto const* crt = usage.find("cache_read_input_tokens"); crt && crt->is_number()) {
        snapshot.cache_read_input_tokens = static_cast<std::uint64_t>(crt->as_number());
    }
}

void accumulate_message_delta_usage(AnthropicUsageSnapshot& snapshot, json::Value const& usage) {
    if (auto const* ot = usage.find("output_tokens"); ot && ot->is_number()) {
        snapshot.output_tokens = static_cast<std::uint64_t>(ot->as_number());  // overwrite, never add
    }
    if (auto const* it = usage.find("input_tokens"); it && it->is_number()) {
        snapshot.input_tokens = static_cast<std::uint64_t>(it->as_number());
    }
    if (auto const* crt = usage.find("cache_read_input_tokens"); crt && crt->is_number()) {
        snapshot.cache_read_input_tokens = static_cast<std::uint64_t>(crt->as_number());
    }
}

std::vector<SseEvent> split_sse_named_events(std::string_view body) {
    std::vector<SseEvent> out;
    std::string_view pending_type;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        auto const nl = body.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("event:")) {
            std::string_view t = line.substr(6);
            while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
            pending_type = t;
        } else if (line.starts_with("data:")) {
            std::string_view d = line.substr(5);
            while (!d.empty() && d.front() == ' ') d.remove_prefix(1);
            out.push_back(SseEvent{pending_type, d});
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

result<std::vector<ChatResponseUpdate>> parse_streaming_response_into_updates(
    std::string_view raw_body, bool is_chunked, std::string const& producer_chat_client_id) {
    StreamingUpdateAccumulator acc(is_chunked, producer_chat_client_id);
    auto fed = acc.feed(raw_body);
    if (!fed) return std::unexpected(fed.error());
    std::vector<ChatResponseUpdate> updates = std::move(*fed);
    for (auto& update : acc.finish()) updates.push_back(std::move(update));
    return updates;
}

void run_stream_worker(std::string host, std::uint16_t port, std::string path, std::string api_key,
                       std::string api_version, std::string model, ChatClientCapabilities caps,
                       ChatRequest request, stream_producer<ChatResponseUpdate> producer,
                       Resolver resolver, std::string ca_bundle_pem_override,
                       std::string http_referer, std::string x_title, std::string end_user_id,
                       std::string cache_ttl, sandbox::ProviderTransport transport,
                       std::stop_token stop, std::string session_id) {
    auto body = build_request_body(request, model, caps, /*stream=*/true, end_user_id, cache_ttl);
    if (!body) {
        // Forward the real error, the same shape `chat()`'s own non-streaming path returns.
        producer.fail(body.error());
        return;
    }
    auto req = build_http_request(path, api_key, api_version, json::dump(*body), http_referer, x_title,
                                   session_id);
    agentengine::detail::provider_wire::pump_sse_stream<StreamingUpdateAccumulator>(
        "anthropic", model, host, port, req, producer, resolver, ca_bundle_pem_override, transport,
        std::move(stop));
}

result<std::vector<ChatResponseUpdate>> StreamingUpdateAccumulator::feed(std::string_view bytes) {
    std::string decoded;
    if (chunked_) {
        auto d = chunked_decoder_.feed(bytes);
        if (!d) return std::unexpected(d.error());
        decoded = std::move(*d);
    } else {
        decoded.assign(bytes);
    }
    std::vector<ChatResponseUpdate> out;
    for (std::string const& block : framer_.feed(decoded)) {
        for (BlockItem& bi : items_from_block(block, &out)) release(&out, std::move(bi.item), bi.text_block);
    }
    return out;
}

std::vector<ChatResponseUpdate> StreamingUpdateAccumulator::finish() {
    std::vector<ChatResponseUpdate> out;
    if (std::string tail = framer_.take_remainder(); !tail.empty()) {
        for (BlockItem& bi : items_from_block(tail, &out)) release(&out, std::move(bi.item), bi.text_block);
    }
    for (auto const& b : pending_by_index_) {
        if (!b.seen) continue;
        if (b.kind == "tool_use") {
            ToolCall call;
            call.call_id = b.tool_id;
            call.tool_name = b.tool_name;
            call.arguments_json = b.tool_input_json.empty() ? "{}" : b.tool_input_json;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(call);
            release(&out, std::move(item));
        } else if (b.kind == "thinking") {
            Reasoning r;
            r.text = b.thinking_text;
            r.encrypted = false;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(r);
            release(&out, std::move(item));
        } else if (b.kind == "redacted_thinking") {
            Reasoning r;
            r.encrypted = true;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value = std::move(r);
            release(&out, std::move(item));
        }
    }
    if (held_) {
        ChatResponseUpdate last;
        last.delta = std::move(*held_);
        last.continues_previous = held_continues_previous_;
        last.is_final = true;
        last.usage = captured_usage();
        held_.reset();
        out.push_back(std::move(last));
    } else if (auto usage = captured_usage(); usage.has_value()) {
        // A genuinely empty completion (no text, no tool call, no thinking block) still carries
        // real usage -- never drop it just because there was no content item to hang it off of
        // (same reasoning as the OpenAI-side accumulator's own handling of this case).
        ChatResponseUpdate last;
        last.delta.value  = Text{};
        last.delta.origin = content_origin::assistant;
        last.is_final     = true;
        last.usage        = usage;
        out.push_back(std::move(last));
    }
    return out;
}

void StreamingUpdateAccumulator::release(std::vector<ChatResponseUpdate>* out, ContentItem item,
                                         std::optional<std::size_t> text_block) {
    if (auto* r = std::get_if<Reasoning>(&item.value)) {
        r->producer_chat_client_id = producer_chat_client_id_;
    }
    bool const continues = text_block.has_value() && last_released_text_block_ == text_block;
    last_released_text_block_ = text_block;
    if (held_) {
        ChatResponseUpdate update;
        update.delta = std::move(*held_);
        update.continues_previous = held_continues_previous_;
        update.is_final = false;
        out->push_back(std::move(update));
    }
    held_ = std::move(item);
    held_continues_previous_ = continues;
}

std::vector<StreamingUpdateAccumulator::BlockItem> StreamingUpdateAccumulator::items_from_block(std::string const& block,
                                                        std::vector<ChatResponseUpdate>* chunk_out) {
    std::vector<BlockItem> out;
    for (SseEvent const& ev : split_sse_named_events(block)) {
        if (ev.type == "message_stop") message_stop_seen_ = true;
        if (ev.type == "content_block_start") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            auto const* idx = parsed->find("index");
            auto const* cb = parsed->find("content_block");
            // Issue #72: bounded, not coerced. See core/chat_client.hpp's kMaxStreamBlockIndex.
            auto const bounded =
                idx != nullptr ? json::as_bounded_integer(*idx, kMaxStreamBlockIndex)
                               : std::nullopt;
            if (!bounded.has_value() || !cb) continue;
            PendingBlock& b = ensure_index(static_cast<std::size_t>(*bounded));
            b.seen = true;
            if (auto const* type = cb->find("type"); type && type->is_string()) b.kind = type->as_string();
            if (b.kind == "tool_use") {
                if (auto const* id = cb->find("id"); id && id->is_string()) b.tool_id = id->as_string();
                if (auto const* name = cb->find("name"); name && name->is_string()) {
                    b.tool_name = name->as_string();
                }
                // Issue #112 B2: Anthropic's own stream opens a tool_use block with the placeholder
                // `"input": {}` and sends the arguments as `partial_json` deltas. A start that already carries
                // a real input (a gateway replaying a whole message as SSE) is not dropped -- that ran the call
                // with `{}`. Its JSON text starts the buffer; deltas after it make the text unparseable, so an
                // input given both ways is refused at step 2, never resolved.
                if (auto const* input = cb->find("input");
                    input != nullptr && !input->is_null() && !(input->is_object() && input->as_object().empty())) {
                    b.tool_input_json = json::dump(*input);
                }
            } else if (b.kind == "redacted_thinking") {
                b.redacted = true;
            }
        } else if (ev.type == "content_block_delta") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            auto const* idx = parsed->find("index");
            auto const* delta = parsed->find("delta");
            // Issue #72: bounded, not coerced. See core/chat_client.hpp's kMaxStreamBlockIndex.
            auto const bounded =
                idx != nullptr ? json::as_bounded_integer(*idx, kMaxStreamBlockIndex)
                               : std::nullopt;
            if (!bounded.has_value() || !delta) continue;
            PendingBlock& b = ensure_index(static_cast<std::size_t>(*bounded));
            b.seen = true;
            auto const* dtype = delta->find("type");
            std::string const dkind = (dtype && dtype->is_string()) ? dtype->as_string() : std::string{};
            // ADR-035 Phase 1 hardening: only surface a `text_delta` as a `Text` content item
            // when its own index was actually started as a `text` block (or never explicitly
            // typed at all). A spec-compliant Anthropic server never sends `text_delta` for an
            // index `content_block_start` declared `tool_use`/`thinking` -- but a malformed or
            // buggy Anthropic-wire-compatible gateway (Bedrock/Vertex/self-hosted proxies are a
            // real deployment shape here, not hypothetical) sending one anyway must not inject a
            // stray `Text` item into `apply_response_format_scan`'s surface by misrouting through
            // a mismatched `b.kind`.
            if (dkind == "text_delta" && (b.kind.empty() || b.kind == "text")) {
                auto const* text = delta->find("text");
                if (text && text->is_string() && !text->as_string().empty()) {
                    ContentItem item;
                    item.origin = content_origin::assistant;
                    item.value = Text{text->as_string()};
                    out.push_back(BlockItem{std::move(item), static_cast<std::size_t>(*bounded)});
                }
            } else if (dkind == "input_json_delta") {
                // Issue #112 B2: a non-string `partial_json` is appended as its JSON text, not dropped (the
                // OpenAI stream's rule); `null` is no fragment.
                auto const* pj = delta->find("partial_json");
                std::string fragment;
                if (pj != nullptr && pj->is_string()) {
                    fragment = pj->as_string();
                } else if (pj != nullptr && !pj->is_null()) {
                    fragment = json::dump(*pj);
                }
                if (!fragment.empty()) {
                    b.tool_input_json += fragment;
                    ChatResponseUpdate chunk_update;
                    chunk_update.tool_call_argument_chunk = ToolCallArgumentChunk{
                        b.tool_id, b.tool_name, fragment, /*is_final=*/false};
                    chunk_out->push_back(std::move(chunk_update));
                }
            } else if (dkind == "thinking_delta") {
                if (auto const* th = delta->find("thinking"); th && th->is_string()) {
                    b.thinking_text += th->as_string();
                }
            }
            // signature_delta: signature intentionally dropped (file banner (3)).
        }
        // content_block_stop carries no CONTENT item of its own (block completion is settled from
        // `pending_by_index_` at `finish()`) but IS a real per-index completion boundary --
        // unified-streaming-design-draft.md §1, Findings 26/27: for a `tool_use` block, this is
        // the fragment-level "this tool call's arguments are now complete" signal, a genuine
        // resolved answer this backend gives that OpenAI's stream does not.
        else if (ev.type == "content_block_stop") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            auto const* idx = parsed->find("index");
            // Issue #72. This site already bounds-checked before indexing, so it was never the
            // out-of-bounds write the other two were -- but the cast itself is undefined for a
            // negative or huge double, so it goes through the same guard rather than relying on
            // SIZE_MAX happening to fail the check below on this compiler.
            auto const bounded =
                idx != nullptr ? json::as_bounded_integer(*idx, kMaxStreamBlockIndex)
                               : std::nullopt;
            if (!bounded.has_value()) continue;
            std::size_t const index = static_cast<std::size_t>(*bounded);
            if (index >= pending_by_index_.size()) continue;
            PendingBlock const& b = pending_by_index_[index];
            if (b.kind == "tool_use") {
                ChatResponseUpdate chunk_update;
                chunk_update.tool_call_argument_chunk =
                    ToolCallArgumentChunk{b.tool_id, b.tool_name, /*arguments_fragment=*/{},
                                           /*is_final=*/true};
                chunk_out->push_back(std::move(chunk_update));
            }
        }
        // message_stop carries no content item either. message_start/message_delta carry no
        // content item either, but DO carry real usage (ADR-034/035) -- captured below via
        // the file banner's own (6) E2 reduce, previously computed but never attached to a
        // `ChatResponseUpdate` before `ChatResponseUpdate::usage` existed to hold it.
        else if (ev.type == "message_start") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            if (auto const* message = parsed->find("message")) {
                if (auto const* usage = message->find("usage")) {
                    seed_usage_from_message_start(usage_snapshot_, *usage);
                }
            }
        } else if (ev.type == "message_delta") {
            auto parsed = json::parse(ev.data);
            if (!parsed) continue;
            if (auto const* usage = parsed->find("usage")) {
                accumulate_message_delta_usage(usage_snapshot_, *usage);
            }
        }
    }
    return out;
}

std::optional<Usage> StreamingUpdateAccumulator::captured_usage() const {
    if (!usage_snapshot_.input_tokens.has_value() && usage_snapshot_.output_tokens == 0) {
        return std::nullopt;  // message_start never arrived (or carried nothing) -- report absence
    }
    Usage u;
    u.input_tokens        = usage_snapshot_.input_tokens.value_or(0);
    u.output_tokens       = usage_snapshot_.output_tokens;
    u.cached_input_tokens = usage_snapshot_.cache_read_input_tokens.value_or(0);
    return u;
}

}  // namespace agentengine::anthropic::detail
