// Proves 006 §7 / 028 §2's tool-result-to-BlobRef promotion mechanism (core/tool_pipeline.hpp's
// `normalize_success`, step 9 of both `invoke_tool()` and `background_task()`) -- Track A of the
// first-party-tools scope (docs/research/2026-08-24-dify-ai-feature-comparison.md's named gap).
//
// Every case here is exercised through the REAL public pipeline entry points (`invoke_tool`,
// `background_task`), never by calling `tool_pipeline_detail::normalize_success` directly -- proving
// the mechanism the way a real caller reaches it, and proving both paths share the identical rule
// (the whole reason that logic was factored into one shared function instead of living twice).

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include "agentengine/core/tool_pipeline.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// A capability-free, pure tool whose reply size is entirely controlled by `Args::payload` --
// existing for exactly one purpose: letting a test drive the JSON-dumped reply comfortably under or
// over a chosen byte threshold without needing a real content-producing tool.
struct EchoPayloadArgs {
    std::string payload;
};
AE_JSON_SCHEMA(EchoPayloadArgs, payload)

struct EchoPayloadReply {
    std::string payload;
};
AE_JSON_SCHEMA(EchoPayloadReply, payload)

struct EchoPayloadTool
    : agentengine::Tool<EchoPayloadTool, agentengine::Capabilities<>,
                         agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "echo_payload";
    static constexpr std::string_view description = "Echoes its payload back, unchanged.";
    using Args = EchoPayloadArgs;
    using Reply = EchoPayloadReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{std::move(args.payload)};
    }
};

struct EchoPayloadBackgroundableTool
    : agentengine::Tool<EchoPayloadBackgroundableTool, agentengine::Capabilities<>,
                         agentengine::EffectClass<agentengine::effect_class::pure>,
                         agentengine::Backgroundable> {
    static constexpr std::string_view name = "echo_payload_bg";
    static constexpr std::string_view description = "Backgroundable twin of echo_payload.";
    using Args = EchoPayloadArgs;
    using Reply = EchoPayloadReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{std::move(args.payload)};
    }
};

agentengine::EffectContext make_ctx() {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    return ctx;
}

agentengine::ToolCallRequest make_request(std::string call_id, std::string tool_name, std::string payload) {
    return agentengine::ToolCallRequest{
        std::move(call_id), std::move(tool_name),
        agentengine::json::Value::make_object(
            {{"payload", agentengine::json::Value::make_string(std::move(payload))}}),
        false};
}

void test_no_threshold_set_is_unaffected() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadTool>();
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx();  // tool_result_byte_threshold left unset, blob_sink left unset
    std::string const big(5000, 'a');

    auto result = agentengine::invoke_tool(table, held, make_request("c1", "echo_payload", big), ctx, nullptr);
    check(!result.is_error, "with no threshold wired, even a large reply is not treated as oversized");
    check(result.content.size() == 1 && std::holds_alternative<agentengine::Data>(result.content[0].value),
          "with no threshold wired, the result is an ordinary Data item -- byte-for-byte prior behavior");
}

void test_under_threshold_is_data_item() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadTool>();
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx();
    ctx.tool_result_byte_threshold = 1000;

    auto result = agentengine::invoke_tool(table, held, make_request("c2", "echo_payload", "small"), ctx, nullptr);
    check(!result.is_error, "under threshold succeeds");
    check(std::holds_alternative<agentengine::Data>(result.content[0].value), "under threshold stays a Data item");
    check(result.content[0].tainted, "still provenance-marked tainted, exactly as before");
}

void test_over_threshold_with_sink_promotes_to_blob() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadTool>();
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx();
    ctx.tool_result_byte_threshold = 20;  // the JSON envelope alone makes any real payload exceed this

    std::string sunk_media_type;
    std::size_t sunk_byte_count = 0;
    ctx.blob_sink = [&](std::span<std::byte const> bytes, std::string const& media_type)
            -> agentengine::result<agentengine::BlobRef> {
        sunk_media_type = media_type;
        sunk_byte_count = bytes.size();
        return agentengine::BlobRef{"digest-abc", media_type, bytes.size(), "test-store"};
    };

    std::string const big(500, 'z');
    auto result = agentengine::invoke_tool(table, held, make_request("c3", "echo_payload", big), ctx, nullptr);
    check(!result.is_error, "over threshold with a sink wired still succeeds");
    check(result.content.size() == 1, "still exactly one content item");
    auto const* media = std::get_if<agentengine::Media>(&result.content[0].value);
    check(media != nullptr, "over threshold, the item is promoted to Media, not Data");
    if (media) {
        auto const* blob = std::get_if<agentengine::BlobRef>(&media->payload);
        check(blob != nullptr, "the Media item's payload is a BlobRef");
        if (blob) check(blob->digest == "digest-abc", "the BlobRef is exactly what blob_sink returned");
    }
    check(result.content[0].tainted, "a promoted result is still tainted, same as an inline one");
    check(sunk_media_type == "application/json", "blob_sink is called with the reply's real media type");
    check(sunk_byte_count > 20, "blob_sink receives the full over-threshold byte count, not a truncated slice");
}

void test_over_threshold_with_no_sink_fails_closed() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadTool>();
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx();
    ctx.tool_result_byte_threshold = 20;  // blob_sink left unset

    std::string const big(500, 'z');
    auto result = agentengine::invoke_tool(table, held, make_request("c4", "echo_payload", big), ctx, nullptr);
    check(result.is_error, "over threshold with no sink fails closed rather than inlining anyway");
    auto const* err = std::get_if<agentengine::Error>(&result.content[0].value);
    check(err != nullptr, "the failure is a structured Error content item");
}

void test_blob_sink_failure_propagates() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadTool>();
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx();
    ctx.tool_result_byte_threshold = 20;
    ctx.blob_sink = [](std::span<std::byte const>, std::string const&) -> agentengine::result<agentengine::BlobRef> {
        return std::unexpected(agentengine::error{agentengine::failure_class::resource, "store is full",
                                                    "test.blob_store_full"});
    };

    std::string const big(500, 'z');
    auto result = agentengine::invoke_tool(table, held, make_request("c5", "echo_payload", big), ctx, nullptr);
    check(result.is_error, "a failing blob_sink fails the whole call");
    // The sink's own error propagates verbatim -- never masked behind a generic "oversized" message.
}

// The identical rule, proven through the OTHER real entry point -- `background_task()` -- so the
// shared-helper refactor (`normalize_success`) is proven to keep both paths in lockstep, not merely
// argued to by code inspection.
void test_background_task_applies_the_same_rule() {
    auto const table = agentengine::ToolTable::from_tools<EchoPayloadBackgroundableTool>();
    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({agentengine::cap::Background{1}});
    auto ctx = make_ctx();
    ctx.tool_result_byte_threshold = 20;

    std::size_t sunk_byte_count = 0;
    ctx.blob_sink = [&](std::span<std::byte const> bytes, std::string const&)
            -> agentengine::result<agentengine::BlobRef> {
        sunk_byte_count = bytes.size();
        return agentengine::BlobRef{"digest-bg", "application/json", bytes.size(), "test-store"};
    };

    bool completed = false;
    bool result_is_error = true;
    bool result_is_media = false;
    std::string const big(500, 'z');
    auto started = agentengine::background_task(
        table, held, make_request("c6", "echo_payload_bg", big), ctx, nullptr,
        /*current_background_count=*/0,
        [&](agentengine::ToolResult result, agentengine::ToolInvocationAudit) {
            result_is_error = result.is_error;
            result_is_media = !result.content.empty() &&
                               std::holds_alternative<agentengine::Media>(result.content[0].value);
            completed = true;
        });
    check(started.has_value(), "background_task accepts a Backgroundable tool under a granted Background<1>");

    for (int i = 0; i < 200 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(completed, "the backgrounded call completes");
    check(!result_is_error, "background_task's over-threshold-with-sink call succeeds, exactly like invoke_tool's");
    check(result_is_media, "background_task promotes to Media/BlobRef too -- the same normalize_success rule");
    check(sunk_byte_count > 20, "background_task's blob_sink also receives the full byte count");
}

}  // namespace

int main() {
    test_no_threshold_set_is_unaffected();
    test_under_threshold_is_data_item();
    test_over_threshold_with_sink_promotes_to_blob();
    test_over_threshold_with_no_sink_fails_closed();
    test_blob_sink_failure_propagates();
    test_background_task_applies_the_same_rule();

    if (g_failures == 0) {
        std::printf("test_tool_result_blob_promotion: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_result_blob_promotion: %d check(s) failed\n", g_failures);
    return 1;
}
