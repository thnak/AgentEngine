// Gap-audit finding 19, Phase 1 fix (2026-08-14): proves `validate_outbound_media_capabilities()`
// (core/chat_client.hpp) actually catches what every real backend's own outbound translation
// silently drops -- `Media` content the bound backend hasn't declared a matching `multimodal_in_*`
// capability for -- turning a silent content loss into a real, attributable `result<void>` failure
// BEFORE a request ever reaches a backend. Checked: each of the four categories (image/audio/video/
// file) independently gated; a positive control proving a declared capability lets the SAME content
// through; Media nested inside `ToolResult::content` (the gap's own named "unaddressed by either
// phase" residual, closed here as a direct consequence of the recursive check); a request with no
// Media at all is unaffected regardless of capabilities; and a Reasoning item never accidentally
// trips the Media-specific gate.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/chat_client.hpp"

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

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ContentItem;
using agentengine::Media;
using agentengine::Message;
using agentengine::Reasoning;
using agentengine::Text;
using agentengine::ToolResult;
using agentengine::content_origin;
using agentengine::role;
using agentengine::validate_outbound_media_capabilities;

Message media_message(std::string message_id, std::string media_type) {
    Message m;
    m.role       = role::user;
    m.message_id = std::move(message_id);
    ContentItem item;
    item.origin = content_origin::user;
    Media media;
    media.media_type = std::move(media_type);
    media.payload    = std::vector<std::byte>{};
    item.value       = std::move(media);
    m.content.push_back(std::move(item));
    return m;
}

Message tool_result_with_media(std::string message_id, std::string media_type) {
    Message m;
    m.role       = role::tool;
    m.message_id = std::move(message_id);
    ContentItem outer;
    outer.origin = content_origin::tool;
    ToolResult tr;
    tr.call_id = "call-1";
    ContentItem inner;
    inner.origin = content_origin::tool;
    Media media;
    media.media_type = std::move(media_type);
    media.payload    = std::vector<std::byte>{};
    inner.value       = std::move(media);
    tr.content.push_back(std::move(inner));
    outer.value = std::move(tr);
    m.content.push_back(std::move(outer));
    return m;
}

}  // namespace

int main() {
    // ---- Each of the four categories independently gated ------------------------------------------
    {
        struct Case {
            char const* media_type;
            char const* label;
        };
        Case const cases[] = {
            {"image/png", "image"},
            {"audio/mpeg", "audio"},
            {"video/mp4", "video"},
            {"application/pdf", "file"},
        };
        for (auto const& c : cases) {
            ChatRequest req{{media_message("m-1", c.media_type)}};
            ChatClientCapabilities const no_caps{};  // every multimodal_in_* bit false
            auto denied = validate_outbound_media_capabilities(req, no_caps);
            check(!denied.has_value(),
                  (std::string("category '") + c.label + "': no declared capability -> fails closed").c_str());
            check(!denied.has_value() && denied.error().code == "chat_client.multimodal_capability_missing",
                  (std::string("category '") + c.label + "': real, attributable error code").c_str());
        }
    }

    // ---- Positive control: the SAME content, WITH the matching capability declared, passes --------
    {
        ChatClientCapabilities caps{};
        caps.multimodal_in_image = true;
        ChatRequest req{{media_message("m-2", "image/png")}};
        auto ok = validate_outbound_media_capabilities(req, caps);
        check(ok.has_value(),
              "positive control: declaring multimodal_in_image lets an image/png Media item through "
              "-- the gate above is real capability checking, not an unconditional rejection");
    }

    // ---- A request with no Media at all is unaffected regardless of capabilities ------------------
    {
        Message text_only;
        text_only.role = role::user;
        ContentItem item;
        item.origin = content_origin::user;
        item.value  = Text{"hello"};
        text_only.content.push_back(std::move(item));

        ChatRequest req{{text_only}};
        ChatClientCapabilities const no_caps{};
        auto ok = validate_outbound_media_capabilities(req, no_caps);
        check(ok.has_value(),
              "a text-only request passes regardless of multimodal capabilities -- the gate only "
              "fires when Media content is actually present");
    }

    // ---- Media nested inside ToolResult::content is caught too (the gap's own named residual) -----
    {
        ChatRequest req{{tool_result_with_media("m-3", "image/jpeg")}};
        ChatClientCapabilities const no_caps{};
        auto denied = validate_outbound_media_capabilities(req, no_caps);
        check(!denied.has_value(),
              "Media nested inside a ToolResult's own content is caught by the same recursive check "
              "-- closing the gap-audit's own 'Media nested inside ToolResult::content remains "
              "unaddressed' residual as a direct consequence of walking tool-result content too");

        ChatClientCapabilities caps{};
        caps.multimodal_in_image = true;
        auto ok = validate_outbound_media_capabilities(req, caps);
        check(ok.has_value(),
              "positive control: the same nested Media passes once the matching capability is "
              "declared -- proving the nested check is real gating, not an unconditional reject");
    }

    // ---- A Reasoning item never trips the Media-specific gate --------------------------------------
    {
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        Reasoning r;
        r.text = "thinking...";
        item.value = std::move(r);
        m.content.push_back(std::move(item));

        ChatRequest req{{m}};
        ChatClientCapabilities const no_caps{};
        auto ok = validate_outbound_media_capabilities(req, no_caps);
        check(ok.has_value(),
              "a Reasoning content item never trips the Media capability gate -- this check is "
              "scoped strictly to Media, matching 004 §2's multimodal_in_* bits and nothing else");
    }

    std::printf(g_failures == 0 ? "test_outbound_media_capability_gate: OK\n"
                                 : "test_outbound_media_capability_gate: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
