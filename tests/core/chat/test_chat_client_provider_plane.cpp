// Milestone 5 Phase B1/B2/B5 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md):
// ChatRequest.tools/output_schema_json (un-eliding 004 §1's own vocabulary-only note), the
// completed ChatClientCapabilities bitset (004 §2), and select_output_schema_strategy (004 §2's
// degradation rule / 003 §4's three enforcement strategies) — all in core/chat_client.hpp.

#include <cstdio>
#include <string>

#include "agentengine/core/chat_client.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace agentengine;

    // ---- B1: ChatRequest carries real tool declarations + an optional structured-output schema --
    {
        ChatRequest req;
        check(req.tools.empty(), "ChatRequest.tools defaults empty, not a null/unset distinction");
        check(!req.output_schema_json.has_value(), "ChatRequest.output_schema_json defaults nullopt");

        ToolDescriptor tool;
        tool.name = "search";
        tool.description = "Search the web.";
        req.tools.push_back(tool);
        req.output_schema_json = R"({"type":"object"})";

        check(req.tools.size() == 1 && req.tools[0].name == "search",
              "ChatRequest.tools carries a real ToolDescriptor, not a placeholder");
        check(req.output_schema_json.value() == R"({"type":"object"})",
              "ChatRequest.output_schema_json carries real schema text");
    }

    // ---- B2: ChatClientCapabilities carries 004 §2's full declared bitset -------------------------
    {
        ChatClientCapabilities caps;
        caps.stop_sequences = true;
        caps.seed = true;
        caps.token_counting = true;
        caps.batch = true;
        check(caps.stop_sequences && caps.seed && caps.token_counting && caps.batch,
              "the four bits Phase B2 added (stop_sequences/seed/token_counting/batch) are real "
              "fields, not still-missing");
    }

    // ---- B5: select_output_schema_strategy prefers native > tool_shaped > parse_and_repair -------
    {
        ChatClientCapabilities native_capable;
        native_capable.structured_output_native = true;
        native_capable.tool_calling = true;  // both true -- native must still win
        check(select_output_schema_strategy(native_capable) == output_schema_strategy::native,
              "native is preferred over tool_shaped when both are available");

        ChatClientCapabilities tool_only;
        tool_only.tool_calling = true;
        check(select_output_schema_strategy(tool_only) == output_schema_strategy::tool_shaped,
              "tool_shaped is chosen when native is unavailable but tool_calling is");

        ChatClientCapabilities neither;
        check(select_output_schema_strategy(neither) == output_schema_strategy::parse_and_repair,
              "parse_and_repair is the universal last resort -- always a defined strategy, never "
              "an unenforceable dead end, under today's three-tier design (004 §2/003 §4)");
    }

    // ---- B6: ChatClientRegistry is a real capability-lookup table ---------------------------------
    {
        ChatClientRegistry registry;
        ChatClientCapabilities caps;
        caps.structured_output_native = true;
        registry.register_client("anthropic:claude-opus-5", caps);

        auto found = registry.find("anthropic:claude-opus-5");
        check(found.has_value() && found->structured_output_native,
              "a registered ChatClientId resolves to its declared capabilities");

        auto missing = registry.find("openai:gpt-5");
        check(!missing.has_value(), "an unregistered ChatClientId resolves to nothing, fails closed");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_provider_plane: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_provider_plane: %d FAILURE(S)\n", g_failures);
    return 1;
}
