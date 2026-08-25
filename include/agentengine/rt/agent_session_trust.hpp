#pragma once
// Implements docs/planning/agent-session-decomposition-design-draft.md §2b -- I3 trust-enforcement
// helpers and the streaming-response drain loop, factored out of rt::AgentSession
// (agent_session.hpp) as free functions. Each takes an `EmitFn` callback instead of calling
// `AgentSession::emit_run_event()` directly -- `AgentSession`'s own emit_run_event()/
// emit_run_event_for() and their backing state (run_event_producer_/run_event_seq_by_run_) are
// NOT moving; these functions just take a thin closure over them at each call site. See the design
// draft for the full rationale.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/stream.hpp"

namespace agentengine::rt::detail {

// ae-naming-lint: allow EmitFn — ADR-097's own new vocabulary (agent-session-decomposition-design-draft.md §2b), 027 not yet updated
using EmitFn = std::function<void(agentengine::run_event_kind, agentengine::RunEventPayload)>;

// unified-streaming-design-draft.md §5 (Piece E). A tool-pushed `ContentItem` (via
// `EffectContext::report_progress`) gets `tainted = true` unconditionally, same convention
// `tool_pipeline.hpp`'s own `invoke_tool()` already follows for its return-value content -- a tool
// author does not get to unilaterally mark its own mid-call content trusted. Recursive (not a flat
// assignment): a pushed `ContentItem` can itself hold a `ToolResult`, which nests its own
// `std::vector<ContentItem>`, each independently serialized with its own `tainted` flag downstream
// (`rt/message_codec.hpp`) -- a non-recursive fix would leave a nested item's own `tainted = false`
// surviving verbatim into AG-UI/MCP output. Inherits the same unbounded-recursion-depth risk
// `message_codec.hpp`/`chat_recording.hpp`'s own content-item codecs already accept for this
// identical structure -- not a new risk class.
inline void force_tainted(agentengine::ContentItem& item) {
    item.tainted = true;
    if (auto* tr = std::get_if<agentengine::ToolResult>(&item.value)) {
        for (agentengine::ContentItem& child : tr->content) force_tainted(child);
    }
}

// Gap-audit finding 20 / 003 §8 Q2. Drops every `Reasoning` content item whose
// `producer_chat_client_id` does not exactly match `current_chat_client_id` -- including an EMPTY
// stamp (a record written before this field existed, or a `text_derived` leak-scan extraction,
// core/response_format_codec.hpp, whose provenance is already untrustworthy by construction): Q2's
// own rule is an allowlist ("included only when it originated from..."), not a denylist, so unknown
// provenance is excluded, never assumed safe. A message that becomes empty SOLELY because of this
// filter is dropped entirely (never sent as an empty-content message); a message that was already
// empty for an unrelated reason is left alone. The excluded item is not deleted from anything
// durable -- `contribution` is this turn's own transient `ContextContribution`, not `history_`, so
// the original item stays intact there for audit/replay (Q2's own "excluded... not deleted"
// wording), and each exclusion fires a real `run_event_kind::policy_decision` (013 §1's own
// vocabulary; this was its first real producer) via `emit`.
inline void filter_cross_provider_reasoning(agentengine::ContextContribution& contribution,
                                             std::string const& current_chat_client_id,
                                             EmitFn const& emit) {
    std::vector<agentengine::Message> filtered;
    filtered.reserve(contribution.messages.size());
    for (agentengine::Message& m : contribution.messages) {
        bool const originally_empty = m.content.empty();
        std::vector<agentengine::ContentItem> kept;
        kept.reserve(m.content.size());
        for (agentengine::ContentItem& item : m.content) {
            auto const* r = std::get_if<agentengine::Reasoning>(&item.value);
            if (r != nullptr && r->producer_chat_client_id != current_chat_client_id) {
                emit(agentengine::run_event_kind::policy_decision,
                     agentengine::run_event_payload::PolicyDecision{
                         "excluded a Reasoning content item from message '" + m.message_id + "' -- produced by '" +
                         (r->producer_chat_client_id.empty() ? "(unknown)" : r->producer_chat_client_id) +
                         "', currently bound backend is '" + current_chat_client_id +
                         "' (003 §8 Q2: reasoning is vendor-specific, never translated across providers)"});
                continue;
            }
            kept.push_back(std::move(item));
        }
        m.content = std::move(kept);
        if (!m.content.empty() || originally_empty) filtered.push_back(std::move(m));
    }
    contribution.messages = std::move(filtered);
}

// unified-streaming-design-draft.md §3 (Piece A), Rev 7. Drains ANY `stream<ChatResponseUpdate>` --
// whether produced by a plain `ChatClientT::chat_stream()` or a gateway's `call_stream()` -- into a
// `result<ChatResponse>`, firing live `model_delta` events along the way via `emit` exactly as
// `AgentSession`'s own drain loop always did. Fail-closed-on-missing-usage (004 §5's
// TokenBudget<N>) is preserved exactly.
[[nodiscard]] inline agentengine::result<agentengine::ChatResponse> drain_streaming_response(
    agentengine::stream<agentengine::ChatResponseUpdate> s, bool stream_model_calls, EmitFn const& emit) {
    agentengine::Message accumulated;
    accumulated.role = agentengine::role::assistant;
    std::optional<agentengine::Usage> usage;
    while (!s.done()) {
        while (std::optional<agentengine::ChatResponseUpdate> upd = s.next()) {
            if (stream_model_calls) {
                if (auto const* t = std::get_if<agentengine::Text>(&upd->delta.value);
                    t != nullptr && !t->text.empty()) {
                    emit(agentengine::run_event_kind::model_delta,
                         agentengine::run_event_payload::ModelDelta{
                             agentengine::run_event_payload::ModelTextDelta{t->text}});
                } else if (upd->tool_call_argument_chunk.has_value()) {
                    auto const& chunk = *upd->tool_call_argument_chunk;
                    emit(agentengine::run_event_kind::model_delta,
                         agentengine::run_event_payload::ModelDelta{
                             agentengine::run_event_payload::ModelToolCallArgumentDelta{
                                 chunk.call_id, chunk.tool_name, chunk.arguments_fragment, chunk.is_final}});
                }
            }
            // A pure argument-chunk update carries no real content in `delta` (it's left at its
            // default) -- appending it would push a spurious placeholder ContentItem into the
            // accumulated message. unified-streaming-design-draft.md §1, Finding 14.
            if (!upd->tool_call_argument_chunk.has_value()) {
                accumulated.content.push_back(upd->delta);
            }
            if (upd->is_final && upd->usage.has_value()) usage = upd->usage;
        }
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (s.terminal() != agentengine::stream_terminal::closed) {
        return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                    "chat_stream() did not reach a clean terminal",
                                                    "run.stream_incomplete"});
    }
    if (!usage.has_value()) {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::contract,
                                "streaming chat call completed with no reported token usage — refusing "
                                "to treat it as zero-cost against the per-run token budget (004 §5)",
                                "run.usage_unavailable"});
    }
    return agentengine::ChatResponse{std::move(accumulated), *usage};
}

}  // namespace agentengine::rt::detail
