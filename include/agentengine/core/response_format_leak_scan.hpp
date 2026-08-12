#pragma once
// ADR-023 §6 point 4 (Phase 2) glue, and ADR-035 Phase 1's backend-agnostic generalization of it.
// `response_format_codec.hpp` deliberately has no dependency on `tool_pipeline.hpp` (its own top
// comment names this explicitly -- it produces plain-data `DetectedToolCallCandidate`s, never a
// `ToolCallRequest`, never anything that itself decides trust). This file is the one place that
// depends on BOTH: it matches a candidate's recipient against a real, live `ToolDescriptor` list and
// promotes matched candidates to real `ToolCall` content items tagged
// `provenance = call_provenance::text_derived` -- still not a trust decision itself; that happens
// entirely in `core/tool_pipeline.hpp`'s `invoke_tool` step 5 (the capability-scoped declassifier,
// the ONE place `provenance` is ever read for an approval decision).
//
// Originally lived inside `protocol/openai/chat_client.hpp` (`OpenAIChatClient`'s own private
// `detail::apply_response_format_scan`), reachable only from that backend's non-streaming `chat()`
// path. Relocated here (design -> red-team -> prove) so `AgentSession::run_model_call()` can apply it
// uniformly to the reconstructed `Message` regardless of which `ChatClient` backend or path
// (streaming or `chat()`) produced it -- closing Anthropic's total gap (it had no scan at all, in
// either path), not merely porting OpenAI's existing coverage to its own streaming path.
// `OpenAIChatClient` itself now just calls this relocated function instead of its own copy -- one
// implementation, not two that could drift (this project's own established "one decoder" precedent).

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/response_format_codec.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// Runs `response_format_codec::decode_response_format` over every plain, UNTAINTED `Text` item in
// `message` and splices in whatever it returns (unchanged verbatim text on the overwhelmingly common
// clean-content path; split `Reasoning`/`Text`/inert-diagnostic items when a serving layer leaked raw
// Harmony/DeepSeek/Hermes/`<think>` tokens). For each detected candidate whose recipient matches a
// KNOWN name in `tools` (the SAME list declared to the model this round), the corresponding inert
// diagnostic `Text` item is replaced with a real `ToolCall` tagged `text_derived`. An unrecognized
// recipient keeps the diagnostic `Text` unchanged -- promoting an unknown name would only reach
// `invoke_tool` step 1's "unknown tool" rejection anyway, so this is a hygiene choice, not a safety
// one. Never touches `ToolCall`/`Reasoning` items already present (vendor-structured or previously
// scanned) -- this function only ever looks at plain `Text`.
//
// TAINTED `Text` items (`item.tainted == true`) are skipped, not re-scanned. This is load-bearing,
// not incidental (red-team finding, ADR-035 Phase 1): a tainted diagnostic Text's own body is model-
// supplied text re-embedded verbatim (`"[unrecognized tool-call attempt, not executed: " + recipient
// + "(" + arguments + ")]"`), so if `arguments` itself happened to contain a DIFFERENT format's
// literal markers, re-running `decode_response_format` on that diagnostic could sniff the embedded
// markers and promote a second, different candidate the FIRST pass deliberately declined to promote
// -- reachable whenever this function runs twice over the same message (e.g. a caller arms both
// `OpenAIChatClient::scan_response_format_leaks` and `AgentSession`'s own flag simultaneously on the
// `chat()` path). `tainted == true` is exactly the predicate for "scan-produced diagnostic, not fresh
// model text" -- skipping it makes a second pass a true no-op by construction, not by convention.
[[nodiscard]] inline Message apply_response_format_scan(Message message,
                                                           std::vector<ToolDescriptor> const& tools) {
    std::vector<ContentItem> rewritten;
    rewritten.reserve(message.content.size());
    std::uint64_t promoted_count = 0;
    for (ContentItem& item : message.content) {
        if (item.tainted) {
            rewritten.push_back(std::move(item));
            continue;
        }
        if (auto const* text = std::get_if<Text>(&item.value)) {
            auto decoded = response_format_codec::decode_response_format(text->text);
            for (auto const& candidate : decoded.candidates) {
                auto const tool_it = std::find_if(
                    tools.begin(), tools.end(),
                    [&candidate](ToolDescriptor const& t) { return t.name == candidate.recipient; });
                if (tool_it == tools.end()) continue;  // unrecognized name: keep the diagnostic Text
                ContentItem promoted;
                promoted.value = ToolCall{"text_derived_" + std::to_string(promoted_count++),
                                            candidate.recipient, candidate.arguments_json,
                                            content_origin::assistant, call_provenance::text_derived};
                promoted.origin = content_origin::assistant;
                promoted.tainted = true;  // 003 §2 / 007 §4: model-originated, policy-relevant-looking text
                decoded.items[candidate.diagnostic_item_index] = std::move(promoted);
            }
            for (ContentItem& decoded_item : decoded.items) rewritten.push_back(std::move(decoded_item));
        } else {
            rewritten.push_back(std::move(item));
        }
    }
    message.content = std::move(rewritten);
    return message;
}

}  // namespace agentengine
