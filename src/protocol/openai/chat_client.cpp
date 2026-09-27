// Implements 004-Model-Provider-Plane.md §3 (the OpenAI-compatible ChatClient backend); ADR-206 (#120 S6).
// The non-template bodies of include/agentengine/protocol/openai/chat_client.hpp, moved verbatim out of the
// header so they are compiled once here instead of in every file that includes it. The header keeps every
// declaration and comment, the small helpers, and the OpenAIChatClient<Store> template; the design and the rules
// each body implements are documented at its declaration there.

#include "agentengine/protocol/openai/chat_client.hpp"

#include "../sse_stream_pump.hpp"

namespace agentengine::openai::detail {

std::string_view role_to_wire(role r) noexcept {
    switch (r) {
        case role::system: return "system";
        case role::user: return "user";
        case role::assistant: return "assistant";
        case role::tool: return "tool";
    }
    return "user";
}

json::Value translate_message(Message const& m, std::string_view request_code) {
    std::string text;
    std::string unfenced;  // a run of consecutive unfenced system text, cleaned as one (ADR-191 round 3)
    std::vector<json::Value> tool_calls;
    std::optional<std::string> tool_call_id;

    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            // ADR-173 (issue #61): OpenAI keeps `system` as its own wire role rather than
            // concatenating into one blob, so it never had Anthropic's fragment-bleed problem --
            // but it dropped `tainted`/`origin` at exactly the same point, leaving a
            // `{"role":"system"}` message carrying tool/document/model-derived text
            // indistinguishable from a host-authored one. Same fence, same bytes, same predicate.
            // ADR-191: the bracket glyphs appear on the wire only where this serializer wrote a real fence. Text is
            // cleaned after its adjacent parts are joined (a glyph split across parts reassembled otherwise): in a
            // system message each run of unfenced text between two fences, in every other message the whole text.
            if (needs_system_channel_fence(m.role, item)) {
                text += neutralize_outbound_text(unfenced);
                unfenced.clear();
                text += fence_untrusted_text(t->text, item.origin, request_code, !item.approval.empty());
            } else if (m.role == role::system) {
                unfenced += t->text;
            } else {
                text += t->text;
            }
        } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
            std::vector<std::pair<std::string, json::Value>> fn{
                {"name", json::Value::make_string(tc->tool_name)},
                {"arguments", json::Value::make_string(neutralize_outbound_text(tc->arguments_json))},
            };
            std::vector<std::pair<std::string, json::Value>> call{
                {"id", json::Value::make_string(tc->call_id)},
                {"type", json::Value::make_string("function")},
                {"function", json::Value::make_object(std::move(fn))},
            };
            tool_calls.push_back(json::Value::make_object(std::move(call)));
        } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
            tool_call_id = tr->call_id;
            for (ContentItem const& inner : tr->content) {
                if (auto const* it = std::get_if<Text>(&inner.value)) {
                    text += it->text;
                } else if (auto const* d = std::get_if<Data>(&inner.value)) {
                    text += d->json;
                } else if (auto const* e = std::get_if<Error>(&inner.value)) {
                    text += e->message;
                }
            }
        }
    }
    if (m.role == role::system) {
        text += neutralize_outbound_text(unfenced);
    } else {
        text = neutralize_outbound_text(text);  // ADR-191: after the parts are joined
    }

    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(role_to_wire(m.role))));
    if (tool_call_id) {
        obj.emplace_back("tool_call_id", json::Value::make_string(*tool_call_id));
        obj.emplace_back("content", json::Value::make_string(text));
    } else if (!tool_calls.empty()) {
        // A message with tool_calls carries `content: null` when there is no accompanying text --
        // never a fabricated empty string (the SDK's own collapsing rule: absent text -> JSON null).
        obj.emplace_back("content", text.empty() ? json::Value::make_null() : json::Value::make_string(text));
        obj.emplace_back("tool_calls", json::Value::make_array(std::move(tool_calls)));
    } else {
        obj.emplace_back("content", json::Value::make_string(text));
    }
    return json::Value::make_object(std::move(obj));
}

std::vector<json::Value> translate_message_to_wire(Message const& m,
                                                   std::string_view request_code) {
    if (m.role == role::tool) {
        std::size_t tool_result_count = 0;
        for (ContentItem const& item : m.content) {
            if (std::holds_alternative<ToolResult>(item.value)) ++tool_result_count;
        }
        if (tool_result_count > 1) {
            std::vector<json::Value> out;
            out.reserve(tool_result_count);
            for (ContentItem const& item : m.content) {
                if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
                    Message single;
                    single.role = role::tool;
                    ContentItem wrapped;
                    wrapped.origin = item.origin;
                    wrapped.value = *tr;
                    single.content.push_back(std::move(wrapped));
                    out.push_back(translate_message(single, request_code));
                }
            }
            return out;
        }
    }
    std::vector<json::Value> out;
    out.push_back(translate_message(m, request_code));
    return out;
}

result<json::Value> translate_tool(ToolDescriptor const& t) {
    json::Value params;
    if (t.args_schema_value_cached) {
        params = t.args_schema_value;
    } else {
        auto parsed_params = json::parse(t.args_schema_json);
        if (!parsed_params) return std::unexpected(parsed_params.error());
        params = std::move(*parsed_params);
    }
    std::vector<std::pair<std::string, json::Value>> fn{
        {"name", json::Value::make_string(t.name)},
        {"description", json::Value::make_string(neutralize_outbound_text(t.description))},  // ADR-191
        {"parameters", std::move(params)},
    };
    std::vector<std::pair<std::string, json::Value>> tool{
        {"type", json::Value::make_string("function")},
        {"function", json::Value::make_object(std::move(fn))},
    };
    return json::Value::make_object(std::move(tool));
}

json::Value make_nullable(json::Value fragment) {
    std::vector<json::Value> variants;
    variants.push_back(std::move(fragment));
    variants.push_back(json::Value::make_object(
        {{"type", json::Value::make_string("null")}}));
    return json::Value::make_object({{"anyOf", json::Value::make_array(std::move(variants))}});
}

json::Value make_strict_schema(json::Value const& node) {
    if (node.is_array()) {
        std::vector<json::Value> items;
        items.reserve(node.as_array().size());
        for (auto const& item : node.as_array()) items.push_back(make_strict_schema(item));
        return json::Value::make_array(std::move(items));
    }
    if (!node.is_object()) return node;

    json::Value const* properties = node.find("properties");
    json::Value const* required = node.find("required");
    std::vector<std::string> already_required;
    if (required != nullptr && required->is_array()) {
        for (auto const& r : required->as_array())
            if (r.is_string()) already_required.push_back(r.as_string());
    }

    std::vector<std::pair<std::string, json::Value>> members;
    for (auto const& [key, value] : node.as_object()) {
        if (key == "required") continue;  // rebuilt below from `properties`
        if (key == "properties" && value.is_object()) {
            std::vector<std::pair<std::string, json::Value>> new_props;
            new_props.reserve(value.as_object().size());
            for (auto const& [prop_name, prop_schema] : value.as_object()) {
                json::Value reshaped = make_strict_schema(prop_schema);
                bool was_required =
                    std::find(already_required.begin(), already_required.end(), prop_name) !=
                    already_required.end();
                if (!was_required) reshaped = make_nullable(std::move(reshaped));
                new_props.emplace_back(prop_name, std::move(reshaped));
            }
            members.emplace_back(key, json::Value::make_object(std::move(new_props)));
        } else {
            members.emplace_back(key, make_strict_schema(value));
        }
    }

    if (properties != nullptr && properties->is_object()) {
        std::vector<json::Value> all_required;
        all_required.reserve(properties->as_object().size());
        for (auto const& [prop_name, prop_schema] : properties->as_object())
            all_required.push_back(json::Value::make_string(prop_name));
        members.emplace_back("required", json::Value::make_array(std::move(all_required)));
        if (node.find("additionalProperties") == nullptr) {
            members.emplace_back("additionalProperties", json::Value::make_bool(false));
        }
    }

    return json::Value::make_object(std::move(members));
}

result<json::Value> translate_output_schema(std::string const& schema_json) {
    auto parsed = json::parse(schema_json);
    if (!parsed) return std::unexpected(parsed.error());
    json::Value schema = make_strict_schema(*parsed);
    std::vector<std::pair<std::string, json::Value>> json_schema_obj{
        {"name", json::Value::make_string("response")},
        {"schema", std::move(schema)},
        {"strict", json::Value::make_bool(true)},
    };
    std::vector<std::pair<std::string, json::Value>> response_format{
        {"type", json::Value::make_string("json_schema")},
        {"json_schema", json::Value::make_object(std::move(json_schema_obj))},
    };
    return json::Value::make_object(std::move(response_format));
}

std::string_view translate_reasoning_effort(reasoning_effort effort) noexcept {
    switch (effort) {
        case reasoning_effort::off:    return "none";
        case reasoning_effort::low:    return "low";
        case reasoning_effort::medium: return "medium";
        case reasoning_effort::high:   return "high";
    }
    return "medium";  // unreachable for a valid enumerator; no fabricated level, just a safe default
}

result<json::Value> build_request_body(
    ChatRequest const& request, std::string const& model, bool stream,
    std::string const& end_user_id, std::optional<std::int64_t> seed,
    ChatClientCapabilities const& caps) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("model", json::Value::make_string(model));

    std::vector<json::Value> messages;
    messages.reserve(request.messages.size() + 1);
    // ADR-173 (issue #61): the fence's reading rule, as its own leading host-authored system
    // message -- the once-per-request point this backend has, since it emits one wire object per
    // AE Message and has no single concatenated system blob to prepend to. Same predicate as the
    // fences themselves (`has_fenced_system_content`), so preamble and fences cannot disagree; a
    // request with no tainted system content gets no preamble and no fence (its text still loses the
    // raw bracket glyphs, ADR-191 §3.5).
    // ADR-191: a request that carries an approved lesson gets one fresh code, shared by the preamble and that block.
    std::string const request_code =
        has_fenced_approved_lesson(request.messages) ? new_request_approval_code() : std::string{};
    if (has_fenced_system_content(request.messages)) {
        std::vector<std::pair<std::string, json::Value>> preamble;
        preamble.emplace_back("role", json::Value::make_string("system"));
        preamble.emplace_back("content",
                              json::Value::make_string(untrusted_fence_preamble_for(request.messages, request_code)));
        messages.push_back(json::Value::make_object(std::move(preamble)));
    }
    for (auto const& m : request.messages) {
        for (auto& wire : translate_message_to_wire(m, request_code)) messages.push_back(std::move(wire));
    }
    obj.emplace_back("messages", json::Value::make_array(std::move(messages)));

    if (!request.tools.empty()) {
        std::vector<json::Value> tools;
        tools.reserve(request.tools.size());
        for (auto const& t : request.tools) {
            auto tool_json = translate_tool(t);
            if (!tool_json) return std::unexpected(tool_json.error());
            tools.push_back(std::move(*tool_json));
        }
        obj.emplace_back("tools", json::Value::make_array(std::move(tools)));
    }

    if (request.output_schema_json) {
        auto response_format = translate_output_schema(*request.output_schema_json);
        if (!response_format) return std::unexpected(response_format.error());
        obj.emplace_back("response_format", std::move(*response_format));
    }

    if (stream) {
        obj.emplace_back("stream", json::Value::make_bool(true));
        // AgentSession's opt-in streaming turn loop (ADR-034) needs real per-call token usage to
        // keep 004 §5's TokenBudget<N> enforced -- without this, the vendor's SSE stream never
        // includes a usage object at all. A trailing chunk with `choices: []` and a top-level
        // `usage` arrives just before `[DONE]` once this is set (confirmed against OpenRouter's own
        // OpenAI-compatible streaming docs); StreamingUpdateAccumulator::items_from_block() below is
        // what captures it.
        std::vector<std::pair<std::string, json::Value>> stream_options;
        stream_options.emplace_back("include_usage", json::Value::make_bool(true));
        obj.emplace_back("stream_options", json::Value::make_object(std::move(stream_options)));
    }

    if (!end_user_id.empty()) obj.emplace_back("user", json::Value::make_string(end_user_id));
    if (seed.has_value()) obj.emplace_back("seed", json::Value::make_number(static_cast<double>(*seed)));

    // ADR-020. `nullopt` emits nothing at all -- the vendor default, and today's exact behaviour.
    if (request.reasoning_effort.has_value()) {
        // 004 §2's degradation rule: no DECLARED fallback for reasoning effort exists, so a backend
        // that cannot reason must refuse the request rather than quietly drop the field -- dropping
        // it is the "silently ignores the request" the rule forbids. `off` is exempt: a backend
        // without the bit satisfies "do not reason" by construction.
        if (*request.reasoning_effort != reasoning_effort::off && !caps.reasoning) {
            return std::unexpected(error{
                failure_class::contract,
                "request asks for a reasoning effort level but this backend does not declare the "
                "`reasoning` capability (004 §2: no declared fallback exists)",
                "openai.reasoning_not_supported"});
        }
        obj.emplace_back("reasoning_effort", json::Value::make_string(std::string(
                                                  translate_reasoning_effort(*request.reasoning_effort))));
    }

    return json::Value::make_object(std::move(obj));
}

sandbox::NetEgressRequest build_http_request(std::string const& path,
                                             std::string const& api_key,
                                             std::string body,
                                             std::string const& http_referer,
                                             std::string const& x_title,
                                             std::string const& session_id) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path = path;
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    if (!http_referer.empty()) req.headers.emplace_back("HTTP-Referer", http_referer);
    if (!x_title.empty()) req.headers.emplace_back("X-Title", x_title);
    if (!session_id.empty()) req.headers.emplace_back("x-session-id", session_id);
    req.body = std::move(body);
    return req;
}

error map_http_status_error(std::uint16_t status, std::string const& body) {
    return agentengine::detail::provider_wire::http_status_error("openai", status, body);
}

Usage parse_usage_object(json::Value const& usage) {
    Usage out;
    if (auto const* pt = usage.find("prompt_tokens"); pt && pt->is_number()) {
        out.input_tokens = static_cast<std::uint64_t>(pt->as_number());
    }
    if (auto const* ct = usage.find("completion_tokens"); ct && ct->is_number()) {
        out.output_tokens = static_cast<std::uint64_t>(ct->as_number());
    }
    if (auto const* ptd = usage.find("prompt_tokens_details")) {
        if (auto const* cached = ptd->find("cached_tokens"); cached && cached->is_number()) {
            out.cached_input_tokens = static_cast<std::uint64_t>(cached->as_number());
        }
        if (auto const* cwt = ptd->find("cache_write_tokens"); cwt && cwt->is_number()) {
            out.cache_write_tokens = static_cast<std::uint64_t>(cwt->as_number());
        }
    }
    if (auto const* ctd = usage.find("completion_tokens_details")) {
        if (auto const* rt = ctd->find("reasoning_tokens"); rt && rt->is_number()) {
            out.reasoning_tokens = static_cast<std::uint64_t>(rt->as_number());
        }
    }
    return out;
}

std::string_view reasoning_field_of(json::Value const& v) {
    if (auto const* r = v.find("reasoning"); r && r->is_string() && !r->as_string().empty()) {
        return r->as_string();
    }
    // Some OpenAI-compatible gateways (e.g. a self-hosted DeepSeek-native proxy) use this name instead
    // -- never observed together with `reasoning` on the same message/delta, so checking both, in this
    // order, never silently prefers a stale value over a fresher one.
    if (auto const* rc = v.find("reasoning_content"); rc && rc->is_string() && !rc->as_string().empty()) {
        return rc->as_string();
    }
    return {};
}

result<ChatResponse> parse_chat_completion_response(
    json::Value const& body, std::string const& producer_chat_client_id) {
    if (auto const* err = body.find("error")) {
        std::string msg = "unknown error";
        if (auto const* m = err->find("message"); m && m->is_string()) msg = m->as_string();
        return std::unexpected(error{failure_class::contract, "openai error: " + msg, "openai.error"});
    }
    json::Value const* choices = body.find("choices");
    if (!choices || !choices->is_array() || choices->as_array().empty()) {
        return std::unexpected(
            error{failure_class::contract, "response has no choices", "openai.no_choices"});
    }
    json::Value const& choice0 = choices->as_array().front();
    json::Value const* message = choice0.find("message");
    if (!message) {
        return std::unexpected(error{failure_class::contract, "choice has no message", "openai.no_message"});
    }

    ChatResponse resp;
    resp.message.role = role::assistant;

    // Finding 4: the model that ACTUALLY answered -- a sibling of `choices`/`usage` at the top level,
    // not a request-echo (never read from `request`/the caller's own `model` field). Left empty, not
    // fabricated, when the backend doesn't report one.
    if (auto const* model = body.find("model"); model && model->is_string()) {
        resp.model = model->as_string();
    }

    // Issue #49: ahead of the answer's own `content` -- OpenRouter's reasoning trace always precedes
    // the final answer server-side (confirmed live), the same ordering `AnthropicChatClient`'s own
    // `thinking`-block-before-`text`-block convention already has.
    if (std::string_view const reasoning = reasoning_field_of(*message); !reasoning.empty()) {
        ContentItem item;
        Reasoning r;
        r.text = std::string(reasoning);
        r.producer_chat_client_id = producer_chat_client_id;
        item.value = std::move(r);
        item.origin = content_origin::assistant;
        resp.message.content.push_back(std::move(item));
    }

    if (auto const* content = message->find("content");
        content && content->is_string() && !content->as_string().empty()) {
        ContentItem item;
        item.value = Text{content->as_string()};
        item.origin = content_origin::assistant;
        resp.message.content.push_back(std::move(item));
    }

    if (auto const* tool_calls = message->find("tool_calls"); tool_calls && tool_calls->is_array()) {
        for (auto const& tc : tool_calls->as_array()) {
            auto const* id = tc.find("id");
            auto const* fn = tc.find("function");
            if (!fn) continue;
            auto const* name = fn->find("name");
            auto const* args = fn->find("arguments");
            ToolCall call;
            call.call_id = (id && id->is_string()) ? id->as_string() : std::string{};
            call.tool_name = (name && name->is_string()) ? name->as_string() : std::string{};
            // Issue #112 B2 (ADR-197 §5): only a MISSING `arguments` means "no arguments" (`{}`). A present value
            // that is not the string the wire specifies is kept as its own JSON text, never replaced: an object is
            // the arguments themselves (some OpenAI-compatible servers send one) and parses as such; a number,
            // `null`, boolean or array is refused at step 2 as `tool.malformed_arguments` (`tool_call_request_of`).
            // It used to become `{}`, so a tool whose arguments are all optional ran with its defaults.
            if (args == nullptr) {
                call.arguments_json = "{}";
            } else if (args->is_string()) {
                call.arguments_json = args->as_string();
            } else {
                call.arguments_json = json::dump(*args);
            }
            ContentItem item;
            item.value = std::move(call);
            item.origin = content_origin::assistant;
            resp.message.content.push_back(std::move(item));
        }
    }

    if (auto const* usage = body.find("usage")) {
        resp.usage = parse_usage_object(*usage);
    }

    return resp;
}

std::vector<std::string_view> split_sse_data_events(std::string_view body) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        auto const nl = body.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("data:")) {
            std::string_view payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
            out.push_back(payload);
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
                       std::string model, ChatRequest request,
                       stream_producer<ChatResponseUpdate> producer, Resolver resolver,
                       std::string ca_bundle_pem_override, std::string http_referer,
                       std::string x_title, std::string end_user_id,
                       std::optional<std::int64_t> seed, sandbox::ProviderTransport transport,
                       std::stop_token stop, ChatClientCapabilities caps, std::string session_id) {
    // ADR-020: `caps` reaches here for one reason -- so `chat_stream()` enforces the SAME reasoning-
    // effort gate `chat()` does. A capability check that held on one of the two entry points would be
    // no check at all.
    auto body = build_request_body(request, model, /*stream=*/true, end_user_id, seed, caps);
    if (!body) {
        // Forward the REAL error `build_request_body` already computed, the same shape `chat()`'s own
        // non-streaming path returns.
        producer.fail(body.error());
        return;
    }
    auto req = build_http_request(path, api_key, json::dump(*body), http_referer, x_title, session_id);
    agentengine::detail::provider_wire::pump_sse_stream<StreamingUpdateAccumulator>(
        "openai", model, host, port, req, producer, resolver, ca_bundle_pem_override, transport, std::move(stop));
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
        for (ContentItem& item : items_from_block(block, &out)) {
            release(&out, std::move(item));
        }
    }
    return out;
}

std::vector<ChatResponseUpdate> StreamingUpdateAccumulator::finish() {
    std::vector<ChatResponseUpdate> out;
    // A truncated final event still carries a real item -- do not silently drop it.
    if (std::string tail = framer_.take_remainder(); !tail.empty()) {
        for (ContentItem& item : items_from_block(tail, &out)) release(&out, std::move(item));
    }
    for (auto const& acc : pending_by_index_) {
        if (!acc.seen) continue;
        ToolCall call;
        call.call_id = acc.id;
        call.tool_name = acc.name;
        call.arguments_json = acc.arguments.empty() ? "{}" : acc.arguments;
        ContentItem item;
        item.value = std::move(call);
        item.origin = content_origin::assistant;
        release(&out, std::move(item));
    }
    if (held_) {
        ChatResponseUpdate last;
        last.delta = std::move(*held_);
        last.continues_previous = held_continues_previous_;
        last.is_final = true;
        last.usage = captured_usage_;  // nullopt if the vendor never sent stream_options.include_usage
        held_.reset();
        out.push_back(std::move(last));
    } else if (captured_usage_.has_value()) {
        // A genuinely empty completion (no text, no tool call) still carries real usage -- never
        // drop it just because there was no content item to hang it off of. An empty Text delta
        // is a legitimate final update on its own (ADR-034's caller only reads `.usage`/
        // `.is_final` off it, never assumes non-empty text).
        ChatResponseUpdate last;
        last.delta.value  = Text{};
        last.delta.origin = content_origin::assistant;
        last.is_final     = true;
        last.usage        = captured_usage_;
        out.push_back(std::move(last));
    }
    return out;
}

void StreamingUpdateAccumulator::release(std::vector<ChatResponseUpdate>* out, ContentItem item) {
    if (auto* r = std::get_if<Reasoning>(&item.value)) {
        r->producer_chat_client_id = producer_chat_client_id_;
    }
    bool const continues =
        held_.has_value() &&
        ((std::holds_alternative<Text>(held_->value) && std::holds_alternative<Text>(item.value)) ||
         (std::holds_alternative<Reasoning>(held_->value) &&
          std::holds_alternative<Reasoning>(item.value)));
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

std::vector<ContentItem> StreamingUpdateAccumulator::items_from_block(std::string const& block,
                                                          std::vector<ChatResponseUpdate>* chunk_out) {
    std::vector<ContentItem> out;
    for (std::string_view payload : split_sse_data_events(block)) {
        if (payload == "[DONE]") {
            done_seen_ = true;
            continue;
        }
        auto parsed = json::parse(payload);
        if (!parsed) continue;  // one malformed chunk is skipped, not fatal to the whole stream
        // ADR-034: the `stream_options.include_usage` trailing chunk carries a top-level `usage`
        // object and an EMPTY (or absent) `choices` array -- captured here, BEFORE the
        // choices-empty check below would otherwise skip this exact chunk entirely.
        if (auto const* usage = parsed->find("usage")) {
            captured_usage_ = parse_usage_object(*usage);
        }
        json::Value const* choices = parsed->find("choices");
        if (!choices || !choices->is_array() || choices->as_array().empty()) continue;
        json::Value const& choice0 = choices->as_array().front();
        json::Value const* delta = choice0.find("delta");
        if (!delta) continue;

        // Issue #49: OpenRouter's own (non-vanilla-OpenAI) streaming extension -- a `reasoning`/
        // `reasoning_content` field arrives on its OWN delta events, ahead of and separate from
        // `content` deltas (confirmed live: 190 `delta.reasoning` chunks vs. 133 `delta.content`
        // chunks for one `deepseek/deepseek-v3.2-exp` turn) -- so this is a genuinely independent
        // per-chunk fragment, not a splice of the `content` branch below. Streamed immediately, one
        // `Reasoning` item per chunk, exactly like the `Text` branch immediately below it -- unlike
        // `AnthropicChatClient`'s own `thinking_delta` handling, which accumulates a whole block
        // before emitting one item at `content_block_stop`, this vendor gives no equivalent
        // block-boundary signal to accumulate against.
        if (std::string_view const reasoning = reasoning_field_of(*delta); !reasoning.empty()) {
            ContentItem item;
            Reasoning r;
            r.text = std::string(reasoning);
            item.value = std::move(r);
            item.origin = content_origin::assistant;
            out.push_back(std::move(item));
        }

        if (auto const* content = delta->find("content");
            content && content->is_string() && !content->as_string().empty()) {
            ContentItem item;
            item.value = Text{content->as_string()};
            item.origin = content_origin::assistant;
            out.push_back(std::move(item));
        }

        if (auto const* tool_calls = delta->find("tool_calls"); tool_calls && tool_calls->is_array()) {
            for (auto const& tc : tool_calls->as_array()) {
                auto const* idx = tc.find("index");
                // Issue #72. An ABSENT index still means 0 -- a single-tool-call delta legally
                // omits it. A PRESENT one that is negative, fractional, non-finite or absurd is
                // skipped, not coerced: `static_cast<std::size_t>(-1.0)` is SIZE_MAX here, so
                // `resize(index + 1)` wrapped to `resize(0)` and the line below then wrote far
                // out of bounds -- a segfault in Release, from one SSE field. Coercing a hostile
                // index to 0 instead would be its own bug, quietly corrupting the real call at
                // index 0, so a bad fragment is dropped exactly like every other malformed one.
                std::size_t index = 0;
                if (idx != nullptr) {
                    auto const bounded = json::as_bounded_integer(*idx, kMaxStreamBlockIndex);
                    if (!bounded.has_value()) continue;
                    index = static_cast<std::size_t>(*bounded);
                }
                if (index >= pending_by_index_.size()) pending_by_index_.resize(index + 1);
                PendingToolCall& acc = pending_by_index_[index];
                acc.seen = true;
                if (auto const* id = tc.find("id"); id && id->is_string()) acc.id = id->as_string();
                if (auto const* fn = tc.find("function")) {
                    if (auto const* name = fn->find("name"); name && name->is_string()) {
                        acc.name += name->as_string();
                    }
                    auto const* args = fn->find("arguments");
                    // Issue #112 B2: a streamed fragment that is not a string is not dropped (that turned an
                    // object sent in one delta into `{}`). It is appended as its JSON text, the same rule as
                    // the non-streamed path: a whole object alone parses as the arguments; anything else, or an
                    // object mixed with string fragments, fails to parse and is refused at step 2. `null` is
                    // "no fragment in this delta", like an absent field -- deltas omit fields routinely.
                    std::string fragment;
                    if (args != nullptr && args->is_string()) {
                        fragment = args->as_string();
                    } else if (args != nullptr && !args->is_null()) {
                        fragment = json::dump(*args);
                    }
                    if (!fragment.empty()) {
                        acc.arguments += fragment;
                        // OpenAI's stream has no explicit per-tool-call completion boundary event
                        // (only this unparsed `index`-transition convention or a trailing
                        // `finish_reason`) -- `is_final` stays false here; open question, not
                        // guessed at (unified-streaming-design-draft.md open question 6, OpenAI
                        // side).
                        ChatResponseUpdate chunk_update;
                        chunk_update.tool_call_argument_chunk = ToolCallArgumentChunk{
                            acc.id, acc.name, fragment, /*is_final=*/false};
                        chunk_out->push_back(std::move(chunk_update));
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace agentengine::openai::detail
