#pragma once
// Test-only fixture player for the ChatClient seam (004-Model-Provider-Plane.md §6, "Recording and
// replay"). 004 §6 describes a *general* recording/replay feature: capturing a live request,
// response (or full ordered chunk sequence), timing, and usage from a real ChatClient call, then
// serving it back with identical chunk boundaries. That feature does not exist yet — there is no
// real vendor integration to record from (see CLAUDE.md task context).
//
// RecordedChatClient is NOT that feature. It is a small, hand-authored, test-scoped stand-in: it
// reads one JSON fixture file (authored by a human, not captured from a live call) and plays it
// back as a `ChatResponse`. It exists so tests that exercise ChatClient-consuming code can add a
// new scenario by adding a JSON file under tests/fixtures/chat_client/, not by writing more C++.
//
// Test-only: depends on nlohmann/json (tests/CMakeLists.txt FetchContent, linked to test targets
// only) and lives under tests/, never under include/agentengine/ (CONVENTIONS.md dependency tiers —
// core takes no third-party dependency, ever).
//
// Streaming fixture playback (a sequence of ChatResponseUpdate) is NOT implemented here —
// `chat_stream` is a stub matching the DummyChatClient pattern in tests/smoke_vocabulary.cpp,
// because `chat_stream` is unconstrained in chat_client.hpp until real streaming vocabulary
// (`ae::stream<T>`) lands. See tests/fixtures/chat_client/README.md for what a streaming fixture
// schema would need once that lands.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine::test_support {

namespace detail {

inline role parse_role(std::string const& s) {
    if (s == "system") return role::system;
    if (s == "user") return role::user;
    if (s == "assistant") return role::assistant;
    if (s == "tool") return role::tool;
    throw std::runtime_error("unknown role: " + s);
}

inline content_origin parse_origin(std::string const& s) {
    if (s == "user") return content_origin::user;
    if (s == "assistant") return content_origin::assistant;
    if (s == "tool") return content_origin::tool;
    if (s == "system") return content_origin::system;
    if (s == "external") return content_origin::external;
    throw std::runtime_error("unknown content_origin: " + s);
}

// Kind dispatch for ContentItem's variant. Today: text, reasoning, tool_call. Extending to another
// ContentItem alternative (Media, Data, ToolResult, Citation, Error, Custom) is "add a branch here
// and document it in tests/fixtures/chat_client/README.md," not a redesign of this loader.
inline ContentItem parse_content_item(nlohmann::json const& j) {
    ContentItem item{};
    std::string const kind = j.at("kind").get<std::string>();

    if (kind == "text") {
        item.value = Text{j.at("text").get<std::string>()};
    } else if (kind == "reasoning") {
        Reasoning r{};
        r.text = j.at("text").get<std::string>();
        r.encrypted = j.value("encrypted", false);
        item.value = r;
    } else if (kind == "tool_call") {
        ToolCall tc{};
        tc.call_id = j.at("call_id").get<std::string>();
        tc.tool_name = j.at("tool_name").get<std::string>();
        tc.arguments_json = j.at("arguments_json").get<std::string>();
        item.value = tc;
    } else {
        throw std::runtime_error("unsupported fixture content kind: " + kind);
    }

    if (auto it = j.find("origin"); it != j.end()) {
        item.origin = parse_origin(it->get<std::string>());
    }
    item.tainted = j.value("tainted", false);
    return item;
}

inline Message parse_message(nlohmann::json const& j) {
    Message message{};
    message.role = parse_role(j.at("role").get<std::string>());
    message.message_id = j.value("message_id", std::string{});
    for (auto const& item_json : j.at("content")) {
        message.content.push_back(parse_content_item(item_json));
    }
    return message;
}

inline Usage parse_usage(nlohmann::json const& j) {
    Usage usage{};
    usage.input_tokens = j.value("input_tokens", std::uint64_t{0});
    usage.output_tokens = j.value("output_tokens", std::uint64_t{0});
    usage.cached_input_tokens = j.value("cached_input_tokens", std::uint64_t{0});
    usage.reasoning_tokens = j.value("reasoning_tokens", std::uint64_t{0});
    usage.cost_estimate = j.value("cost_estimate", 0.0);
    return usage;
}

} // namespace detail

// Constructed with the exact fixture file it will always return — one instance per scenario, per
// CLAUDE.md's task guidance ("keep this simple... add another JSON file, not more C++"). No
// request-based fixture selection.
class RecordedChatClient {
public:
    explicit RecordedChatClient(std::filesystem::path fixture_path,
                                 ChatClientCapabilities caps = {})
        : fixture_path_(std::move(fixture_path)), capabilities_(caps) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    // Satisfies ChatClient::chat's `task<result<ChatResponse>>` return-type constraint
    // (chat_client.hpp, Milestone 5 Phase B4). A missing or malformed fixture is
    // `failure_class::fatal` — never a fabricated empty success. Pure synchronous file I/O inside
    // the coroutine body -- never genuinely parks, so this is safe to drive from
    // `test_support::run_task_sync` outside any actor, or `co_await`ed from a real one.
    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) const {
        std::ifstream in(fixture_path_, std::ios::binary);
        if (!in) {
            co_return std::unexpected(error{failure_class::fatal,
                                          "fixture not found: " + fixture_path_.string(),
                                          "E_FIXTURE_NOT_FOUND"});
        }
        try {
            nlohmann::json doc;
            in >> doc;
            ChatResponse response{};
            response.message = detail::parse_message(doc.at("message"));
            if (auto it = doc.find("usage"); it != doc.end()) {
                response.usage = detail::parse_usage(*it);
            }
            co_return response;
        } catch (std::exception const& ex) {
            co_return std::unexpected(
                error{failure_class::fatal,
                      "malformed fixture " + fixture_path_.string() + ": " + ex.what(),
                      "E_FIXTURE_MALFORMED"});
        }
    }

    // Streaming fixture playback not implemented (see top comment). Sentinel stub, matching the
    // DummyChatClient pattern in tests/smoke_vocabulary.cpp — chat_stream is unconstrained beyond
    // "callable" until real streaming vocabulary exists (chat_client.hpp).
    int chat_stream(ChatRequest const&, EffectContext&) const { return 0; }

private:
    std::filesystem::path fixture_path_;
    ChatClientCapabilities capabilities_;
};

} // namespace agentengine::test_support
