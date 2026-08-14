// Proves backends/wasm/wasm_tool_bridge.hpp -- decisions/ADR-040-wasm-tool-pipeline-bridge.md.
// Reuses the same real ae:tool component tests/test_wasm_backend.cpp already proves
// (tests/fixtures/wasm_ae_tool_fixture/) but does NOT re-prove WasmBackend's own internals (import
// verification, capability-kind confusion, wall_ms kill -- all covered there already). This file
// proves the NEW seam only: a wasm-hosted tool reachable through the REAL 006 §3
// `agentengine::invoke_tool()` pipeline, not `WasmBackend::invoke_tool` directly.
//
//   1. wasm_tools_as_descriptors_from() adapts the real fixture's tools into agentengine::
//      ToolDescriptor, each gated behind exactly one cap::ToolCall{plugin_id + "::" + name} and
//      captures_session_state = true.
//   2. End-to-end reachability: the adapted "echo" tool, unioned via union_codeact_tools() into a
//      real ToolTable, is actually callable through agentengine::invoke_tool() -- the real reply
//      round-trips through the real wasm guest.
//   3. I2 gate is real and independent: a `held` CapabilitySet missing the specific cap::ToolCall
//      denies with tool.capability_not_held, regardless of what the wasm instance's own
//      operator_grant holds (which is fully granted in this test).
//   4. The adapted "now" tool exercises the Data-content mapping path (distinct from echo's
//      Text-content path).
//   5. Delimiter-collision guard: a plugin_id containing "::" is rejected with
//      wasm.tool_name_ambiguous before the backend is ever touched (proven with a handle that was
//      never created/loaded -- the call must never reach it).
//   6. union_codeact_tools()'s cross-source collision check fires against a wasm-sourced name
//      clash too, not just the agent/skill/MCP triples test_codeact_tool_union.cpp already covers.
//   7. wasm_tool_result_to_json() unit tests against hand-built ToolResult values: the is_error and
//      Media-content-fails-closed paths, neither of which the real fixture's tools can exercise
//      end-to-end (it has no tool returning is_error:true or a Media item) -- proven directly
//      against the pure mapping function instead, the single source of truth the real bridge also
//      calls.
//
// SKIPs (CTest SKIP_RETURN_CODE 77), same posture as test_wasm_backend.cpp, if the cargo-component
// fixture wasn't built.

#include "backends/wasm/wasm_tool_bridge.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include "agentengine/core/codeact_tool_union.hpp"

using namespace agentengine;
using namespace agentengine::wasm;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

std::vector<std::uint8_t> read_fixture() {
    std::ifstream f(AE_WASM_FIXTURE_PATH, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "bridge-test-trace";
    ctx.span_id = "bridge-test-span";
    return ctx;
}

std::vector<Capability> const kAllRequiredCapabilities = {
    cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{},
};

}  // namespace

int main() {
    // -- 7. wasm_tool_result_to_json() pure-logic unit tests (no fixture needed) --------------------
    {
        ToolResult is_error_result;
        is_error_result.is_error = true;
        auto mapped = wasm_tool_result_to_json("some-tool", is_error_result);
        AE_CHECK(!mapped.has_value(), "unit: is_error:true maps to a real pipeline error");
        if (!mapped) {
            AE_CHECK(mapped.error().code == "wasm.tool_call_error",
                      "unit: specific wasm.tool_call_error code");
        }
    }
    {
        ToolResult media_result;
        ContentItem item;
        item.value = Media{std::vector<std::byte>{}, "application/octet-stream"};
        item.origin = content_origin::tool;
        item.tainted = true;
        media_result.content.push_back(item);
        auto mapped = wasm_tool_result_to_json("some-tool", media_result);
        AE_CHECK(!mapped.has_value(), "unit: a Media content item fails closed, not silently dropped");
        if (!mapped) {
            AE_CHECK(mapped.error().code == "wasm.tool_result_unsupported_content",
                      "unit: specific wasm.tool_result_unsupported_content code");
        }
    }
    {
        ToolResult data_result;
        ContentItem item;
        item.value = Data{"42", std::nullopt};
        item.origin = content_origin::tool;
        item.tainted = true;
        data_result.content.push_back(item);
        auto mapped = wasm_tool_result_to_json("some-tool", data_result);
        AE_CHECK(mapped.has_value() && mapped->is_number() && mapped->as_number() == 42.0,
                  "unit: a Data content item's json round-trips through json::parse");
    }
    {
        ToolResult text_result;
        ContentItem item;
        item.value = Text{"hello"};
        item.origin = content_origin::tool;
        item.tainted = true;
        text_result.content.push_back(item);
        auto mapped = wasm_tool_result_to_json("some-tool", text_result);
        AE_CHECK(mapped.has_value() && mapped->is_object(), "unit: a Text content item wraps as an object");
        if (mapped && mapped->is_object()) {
            auto const* text_field = mapped->find("text");
            AE_CHECK(text_field && text_field->is_string() && text_field->as_string() == "hello",
                      "unit: the wrapped object's text field is the real text");
        }
    }
    {
        ToolResult empty_result;
        auto mapped = wasm_tool_result_to_json("some-tool", empty_result);
        AE_CHECK(mapped.has_value() && mapped->is_null(), "unit: empty content maps to null");
    }

    // -- 5. Delimiter-collision guard: never reaches the backend -------------------------------------
    {
        auto never_created_backend = std::make_shared<WasmBackend>();
        SandboxHandle const never_loaded{"nonexistent-handle"};
        EffectContext ctx = make_ctx();
        auto descriptors =
            wasm_tools_as_descriptors_from(never_created_backend, never_loaded, "bad::id", ctx);
        AE_CHECK(!descriptors.has_value(), "delimiter guard: a plugin_id containing '::' is rejected");
        if (!descriptors) {
            AE_CHECK(descriptors.error().code == "wasm.tool_name_ambiguous",
                      "delimiter guard: specific wasm.tool_name_ambiguous code");
        }
    }

    // -- Fixture-dependent tests (real component) -----------------------------------------------------
    std::vector<std::uint8_t> const bytes = read_fixture();
    if (bytes.empty()) {
        std::cerr << "SKIP: " << AE_WASM_FIXTURE_PATH
                  << " not found -- cargo-component toolchain unavailable, see "
                     "tests/fixtures/wasm_ae_tool_fixture/README.md\n";
        return g_failures == 0 ? 77 : 1;
    }

    auto backend = std::make_shared<WasmBackend>();
    SandboxSpec spec;
    spec.capabilities = CapabilitySet::grant_root(kAllRequiredCapabilities);
    spec.limits.memory_bytes = 64ull * 1024 * 1024;
    EffectContext load_ctx = make_ctx();
    auto handle = backend->create(spec, load_ctx);
    AE_CHECK(handle.has_value(), "setup: create() succeeds");
    if (!handle) return 1;

    PluginManifest manifest;
    manifest.id = "bridge-test";
    manifest.version = "0.1.0";
    manifest.world = plugin_world::tool;
    manifest.requested_capabilities = kAllRequiredCapabilities;
    manifest.memory_bytes_limit = spec.limits.memory_bytes;

    auto loaded = backend->load_component(*handle, manifest, bytes, load_ctx);
    AE_CHECK(loaded.has_value(), "setup: load_component() succeeds");
    if (!loaded) {
        backend->destroy(*handle);
        return 1;
    }

    // -- 1. Adaptation shape ---------------------------------------------------------------------
    auto descriptors = wasm_tools_as_descriptors_from(backend, *handle, "bridge-plugin", load_ctx);
    AE_CHECK(descriptors.has_value() && descriptors->size() == 7,
              "adapt: all 7 real fixture tools are adapted into agentengine::ToolDescriptor");

    agentengine::ToolDescriptor const* echo_descriptor = nullptr;
    agentengine::ToolDescriptor const* now_descriptor = nullptr;
    if (descriptors) {
        for (auto const& d : *descriptors) {
            if (d.name == "bridge-plugin::echo") echo_descriptor = &d;
            if (d.name == "bridge-plugin::now") now_descriptor = &d;
        }
    }
    AE_CHECK(echo_descriptor != nullptr && now_descriptor != nullptr,
              "adapt: plugin-qualified names are individually findable");
    if (echo_descriptor) {
        AE_CHECK(echo_descriptor->capability_ceiling.size() == 1 &&
                      std::holds_alternative<cap::ToolCall>(echo_descriptor->capability_ceiling[0]) &&
                      std::get<cap::ToolCall>(echo_descriptor->capability_ceiling[0]).tool_name ==
                          "bridge-plugin::echo",
                  "adapt: echo is gated behind cap::ToolCall for its own exact plugin-qualified name (I2)");
        AE_CHECK(echo_descriptor->captures_session_state,
                  "adapt: captures_session_state is set (holds a live WasmBackend reference)");
    }

    // -- Build the real, unioned ToolTable (matches the real CodeAct integration point) -----------
    auto const agent_tools = ToolTable::from_tools<>();
    auto const skill_tools = ToolTable::from_tools<>();
    std::vector<agentengine::ToolDescriptor> const empty_mcp_tools;
    auto unioned = descriptors ? union_codeact_tools(agent_tools, skill_tools, empty_mcp_tools, *descriptors)
                                : result<ToolTable>(std::unexpected(error{failure_class::fatal, "", ""}));
    AE_CHECK(unioned.has_value() && unioned->descriptors().size() == 7,
              "union: wasm-sourced descriptors merge into one bridge-ready ToolTable");

    if (unioned) {
        // -- 2. End-to-end reachability through the REAL 006 §3 pipeline ----------------------------
        {
            CapabilitySet const held = CapabilitySet::grant_root({cap::ToolCall{"bridge-plugin::echo"}});
            EffectContext ctx = make_ctx();
            ToolCallRequest req;
            req.call_id = "call-1";
            req.tool_name = "bridge-plugin::echo";
            req.arguments = json::Value::make_object({{"msg", json::Value::make_string("hello")}});
            ApprovalDecider const approve;
            ToolInvocationAudit audit;

            ToolResult result = invoke_tool(*unioned, held, req, ctx, approve, &audit);
            AE_CHECK(!result.is_error, "e2e: echo call succeeds through the real invoke_tool() pipeline");
            AE_CHECK(audit.ok, "e2e: audit records success");
            if (!result.is_error && result.content.size() == 1) {
                auto const* data = std::get_if<Data>(&result.content[0].value);
                AE_CHECK(data != nullptr, "e2e: the pipeline's own step 9 wraps the reply as one Data item");
                if (data) {
                    auto parsed = json::parse(data->json);
                    AE_CHECK(parsed.has_value() && parsed->is_object(), "e2e: the wrapped reply parses back");
                    if (parsed && parsed->is_object()) {
                        auto const* text_field = parsed->find("text");
                        std::string const expected_echo = json::dump(req.arguments);
                        AE_CHECK(text_field && text_field->is_string() &&
                                      text_field->as_string() == expected_echo,
                                  "e2e: the echoed text is the REAL guest's own output (real "
                                  "computation, not a stub) -- matches exactly what was sent as args_json");
                    }
                }
                AE_CHECK(result.content[0].tainted,
                          "e2e: a wasm tool result is unconditionally tainted (006 §7), inherited for "
                          "free from the outer pipeline's own step 9");
            }
        }

        // -- 3. I2 gate: missing cap::ToolCall denies, independent of the wasm operator_grant --------
        {
            CapabilitySet const held_without_grant = CapabilitySet::grant_root({});
            EffectContext ctx = make_ctx();
            ToolCallRequest req;
            req.call_id = "call-2";
            req.tool_name = "bridge-plugin::echo";
            req.arguments = json::Value::make_object({{"msg", json::Value::make_string("hello")}});
            ApprovalDecider const approve;

            ToolResult result = invoke_tool(*unioned, held_without_grant, req, ctx, approve, nullptr);
            AE_CHECK(result.is_error,
                      "i2: missing cap::ToolCall denies the call, even though the wasm instance's own "
                      "operator_grant is fully granted");
            if (result.is_error && result.content.size() == 1) {
                auto const* err = std::get_if<Error>(&result.content[0].value);
                AE_CHECK(err != nullptr, "i2: denial surfaces as a real structured error");
            }
        }

        // -- 4. The "now" tool exercises the Data-content mapping path (distinct from echo's Text) ---
        {
            CapabilitySet const held = CapabilitySet::grant_root({cap::ToolCall{"bridge-plugin::now"}});
            EffectContext ctx = make_ctx();
            ToolCallRequest req;
            req.call_id = "call-3";
            req.tool_name = "bridge-plugin::now";
            req.arguments = json::Value::make_null();
            ApprovalDecider const approve;

            ToolResult result = invoke_tool(*unioned, held, req, ctx, approve, nullptr);
            AE_CHECK(!result.is_error, "now: call succeeds through the real pipeline (Data-content path)");
            if (!result.is_error && result.content.size() == 1) {
                auto const* data = std::get_if<Data>(&result.content[0].value);
                AE_CHECK(data != nullptr && !data->json.empty() &&
                              std::atoll(data->json.c_str()) > 0,
                          "now: the wrapped reply is a real, plausible unix-millis timestamp");
            }
        }
    }

    // -- 6. union_codeact_tools() collision detection fires against a wasm-sourced clash too --------
    if (descriptors) {
        std::vector<agentengine::ToolDescriptor> colliding_mcp_tools;
        {
            agentengine::ToolDescriptor d;
            d.name = "bridge-plugin::echo";  // deliberately collides with the wasm-sourced descriptor
            d.description = "an MCP descriptor colliding with a wasm-plugin tool's own name";
            colliding_mcp_tools.push_back(std::move(d));
        }
        auto collided = union_codeact_tools(agent_tools, skill_tools, colliding_mcp_tools, *descriptors);
        AE_CHECK(!collided.has_value() &&
                      collided.error().code == "codeact.tool_name_collision_across_sources",
                  "union collision: a wasm-plugin tool colliding with an MCP-sourced tool of the same "
                  "name is rejected, not silently resolved by precedence");
    }

    backend->destroy(*handle);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
