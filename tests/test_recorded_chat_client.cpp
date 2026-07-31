// Test-only: proves the ChatClient concept and ChatRequest/ChatResponse vocabulary
// (agentengine/core/chat_client.hpp, agentengine/core/content.hpp) are usable end-to-end without a
// real network call or API key, via the hand-authored JSON fixture player RecordedChatClient
// (tests/support/recorded_chat_client.hpp — a test-scoped stand-in for the recording/replay concept
// in 004-Model-Provider-Plane.md §6, not an implementation of it). Adding a scenario means adding a
// fixture under tests/fixtures/chat_client/, not writing more C++ (CLAUDE.md task brief).

#include <cassert>
#include <filesystem>
#include <string>
#include <variant>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/principal.hpp"
#include "support/recorded_chat_client.hpp"

#ifndef AE_TEST_FIXTURE_DIR
#error "AE_TEST_FIXTURE_DIR must be defined by CMake to tests/fixtures/chat_client"
#endif

static_assert(ae::ChatClient<ae::test_support::RecordedChatClient>,
              "RecordedChatClient must satisfy the ChatClient concept (004 §1)");

namespace {

std::filesystem::path fixture(std::string const& name) {
    return std::filesystem::path(AE_TEST_FIXTURE_DIR) / name;
}

ae::EffectContext make_ctx() {
    ae::EffectContext ctx{};
    ctx.principal = ae::Principal{"p-1", "tenant-1"};
    ctx.trace_id = "trace-1";
    ctx.span_id = "span-1";
    return ctx;
}

void test_simple_reply() {
    ae::test_support::RecordedChatClient client{fixture("simple_reply.json")};
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = client.chat(request, ctx);
    assert(result.has_value());

    ae::ChatResponse const& response = *result;
    assert(response.message.role == ae::role::assistant);
    assert(response.message.message_id == "m-1");
    assert(response.message.content.size() == 1);

    auto const* text = std::get_if<ae::Text>(&response.message.content.front().value);
    assert(text != nullptr);
    assert(text->text == "hello from a fixture");
    assert(response.message.content.front().origin == ae::content_origin::assistant);

    assert(response.usage.input_tokens == 10);
    assert(response.usage.output_tokens == 5);
}

void test_tool_call() {
    ae::ChatClientCapabilities caps{};
    caps.tool_calling = true;
    ae::test_support::RecordedChatClient client{fixture("tool_call.json"), caps};
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = client.chat(request, ctx);
    assert(result.has_value());

    ae::ChatResponse const& response = *result;
    assert(response.message.content.size() == 1);

    auto const* call = std::get_if<ae::ToolCall>(&response.message.content.front().value);
    assert(call != nullptr);
    assert(call->call_id == "call-1");
    assert(call->tool_name == "get_weather");
    assert(call->arguments_json.find("Seattle") != std::string::npos);

    assert(response.usage.input_tokens == 20);
    assert(response.usage.output_tokens == 8);
    assert(client.capabilities().tool_calling);
}

void test_reasoning_and_text() {
    ae::test_support::RecordedChatClient client{fixture("reasoning_and_text.json")};
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = client.chat(request, ctx);
    assert(result.has_value());

    ae::ChatResponse const& response = *result;
    assert(response.message.content.size() == 2);

    auto const* reasoning = std::get_if<ae::Reasoning>(&response.message.content.at(0).value);
    assert(reasoning != nullptr);
    assert(!reasoning->encrypted);
    assert(reasoning->text == "The user wants the weather; I should call the tool.");

    auto const* text = std::get_if<ae::Text>(&response.message.content.at(1).value);
    assert(text != nullptr);
    assert(text->text == "Let me check that for you.");

    assert(response.usage.input_tokens == 42);
    assert(response.usage.output_tokens == 17);
    assert(response.usage.reasoning_tokens == 12);
}

void test_missing_fixture_returns_error() {
    ae::test_support::RecordedChatClient client{fixture("does_not_exist.json")};
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = client.chat(request, ctx);
    assert(!result.has_value());
    assert(result.error().klass == ae::failure_class::fatal);
    assert(result.error().code == "E_FIXTURE_NOT_FOUND");
}

} // namespace

int main() {
    test_simple_reply();
    test_tool_call();
    test_reasoning_and_text();
    test_missing_fixture_returns_error();
    return 0;
}
