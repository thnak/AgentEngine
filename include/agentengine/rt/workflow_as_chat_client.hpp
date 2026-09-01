#pragma once
// GitHub issue #35: `WorkflowChatClient` -- lets a whole, already-initialized rt::WorkflowSupervisor
// satisfy this codebase's `ChatClient` concept (core/chat_client.hpp), so a built Workflow can be
// reused anywhere a model-backed agent backend is expected -- a direct caller, or (with the real,
// disclosed limitation this file documents) an outer `AgentSession`'s bound backend -- mirroring MAF's
// `workflow.as_agent()` in spirit. NOT issue #33's `executor_kind::sub_workflow` mechanism, and NOT
// `workflow_as_executor.hpp`'s `workflow_as_executor_body()` (ADR-150, issue #36) -- that adapter
// embeds a Workflow as an `ExecutorBody` participant INSIDE another workflow's graph; this one makes a
// Workflow satisfy the `ChatClient` calling convention instead. Full design history, ten independent
// red-team rounds, and every rejected alternative: docs/planning/workflow-as-chatclient-adapter-
// design-draft.md.
//
// DELIBERATELY NO `chat()` -- `ChatClient` (core/chat_client.hpp) only requires `capabilities()` +
// `chat_stream()`; this type conforms via `chat_stream()` alone. Round 8 of the design's own red-team
// tried adding a fail-closed `chat()` (at the time, this adapter could never report honest,
// non-fabricated token `Usage` -- `WorkflowSupervisor` had no usage-tracking mechanism at all) and
// found it broke the headline "direct caller" use case: `chat()`'s signature has no way to tell an
// `AgentSession` binding apart from a caller who never needed a usage guarantee. Round 9 removed
// `chat()` entirely instead of patching it a fourth time.
//
// GitHub issue #35 follow-up (ADR-163) CLOSED THE ROOT CAUSE round 8/9 worked around: `usage` on the
// terminal push is now REAL, not always `nullopt` -- sourced from `WorkflowSupervisor::usage()` (a
// before/after delta around this one call, see `run_worker()`'s own comment), which itself sums every
// TRACKED dispatch's real cost (every `agent`-kind node via `AgentSession::run_usage()`, plus any
// resolved nested `sub_workflow`'s own recursively-summed total -- `WorkflowSupervisor::usage()`'s own
// comment has the full contract and its one honestly-disclosed residual: an ordinary `function`-kind
// node's `ExecutorBody` is arbitrary C++, free to hold and call a real `ChatClient` directly without
// ever reporting through this tracked path -- this adapter therefore reports "how much every TRACKED
// dispatch cost," which can UNDER-report for a graph containing such a node, never over-report/
// fabricate). **A direct caller drains `chat_stream()` and gets honest usage, never a failure. An
// `AgentSession` bound to this adapter can now genuinely COMPLETE a call**, through that session's own
// pre-existing `chat()`-absent fallback (`rt/agent_session.hpp`'s `detail::drain_streaming_response()`),
// provided the wrapped workflow's real cost is fully covered by the tracked path above -- §5/§9's own
// "cannot complete ANY call" limitation (ADR-162) is LIFTED for that common case, not merely narrowed.
//
// `chat()` STAYS ABSENT even so -- this fix did not need it back, and reopening that question is real,
// separate, deliberately NOT done here: `AgentSession`'s dispatch is a pure TYPE-LEVEL `if constexpr` on
// whether `chat()` exists, never on what it does, so re-adding one (even a now-honest one) changes WHICH
// path `AgentSession` prefers, a different question from whether either path can report usage honestly --
// worth its own future design pass, not bundled into this one. One disclosed consequence, unchanged by
// this fix: `RecordingChatClient<Inner>` (core/recording_chat_client.hpp) gates on the stricter
// `LegacyChatClient` concept (needs `chat()`), so `RecordingChatClient<WorkflowChatClient>` still does
// not compile -- a real, accepted trade, not an oversight.
//
// ONE CONVERSATION PER INSTANCE, FOR ITS WHOLE LIFETIME -- `WorkflowSupervisor::run_workflow()`
// unconditionally clears `ports_`/`pending_sub_workflows_` at its own top on every fresh call, and this
// adapter's own fail-closed contract-mismatch check (below) refuses a second, unrelated fresh
// conversation while an earlier one on the same instance is still paused. Unlike an ordinary, freely-
// shared `ChatClient` (`OpenAIChatClient`, etc.), one `WorkflowChatClient` instance serves exactly one
// caller-visible conversation for as long as that instance lives -- document this on every call site,
// the same way `AgentSession` documents I1 ("one session, one executor").
//
// MESSAGE HISTORY -- NOT PHYSICALLY FLATTENED. `run_workflow()` takes exactly one `agentengine::Message`;
// `request.messages` is a whole history. An earlier design (physically concatenating every message's
// `content` items into one `Message`, relying on per-item `ContentItem::origin` for turn attribution)
// was found actively WRONG: neither real backend (`OpenAIChatClient`/`AnthropicChatClient`) reads
// `origin` at all when translating to wire format -- both key entirely on the outer `Message::role` --
// so a flattened message reaching either one, deep inside the wrapped workflow, produces a garbled or
// wire-protocol-invalid request. Instead, the WHOLE ordered `request.messages` list is encoded via this
// codebase's own EXISTING `rt::message_to_json()`/`message_from_json()` codec (message_codec.hpp) into
// one JSON array, wrapped as a single opaque `ContentItem{Custom{type_id=
// "agentengine.workflow_chat_client_history", payload_json=<dumped array>}}`. A wrapped Workflow's
// `start` executor MUST explicitly decode this envelope (via `message_from_json()` in a loop) to recover
// the real message list before doing anything with it -- there is no "just forward the Message as-is"
// path that could silently corrupt a real backend's wire call, because nothing about this envelope
// type-checks as an ordinary single-turn message. Oversized histories (`payload_json` at or above
// `ctx.tool_result_byte_threshold`) promote to `Media{BlobRef}` via `ctx.blob_sink`, mirroring
// `tool_pipeline.hpp::normalize_success()`'s own established fail-closed-if-no-sink rule -- never
// silently inlined regardless of size.
//
// SUSPENDED INTERACTIONS -- `Custom`, NOT `ToolCall`/`ToolResult`. A first design encoded a paused
// `request_port` interaction as `ContentItem{ToolCall{tool_name="workflow_request_port", ...}}`,
// mirroring MAF's `function_call` envelope literally -- traced end to end against `AgentSession`'s real
// turn loop, this DETERMINISTICALLY AND SILENTLY corrupts the paused interaction the moment this
// adapter is bound as an ordinary agent's ChatClientId backend: `tool_calls_of()` extracts every
// `ToolCall` with no name filtering, an unrecognized tool name still reaches `invoke_tool()`, which
// returns a fabricated `ToolResult` carrying the SAME call_id (the real interaction_id), and
// `AgentSession` folds that back into history and calls the model again automatically -- answering the
// human-in-the-loop question with the absence of anyone actually being asked. `Custom` items are
// invisible to `tool_calls_of()`, closing this structurally. Ask:
// `ContentItem{Custom{type_id="agentengine.workflow_request_port",
// payload_json={"interaction_id":...,"ask":<message_to_json(ask)>}}}`, one per currently-open
// interaction (`WorkflowSupervisor::open_interaction_asks()`, workflow_supervisor.hpp -- an `OpenPort`-
// only accessor; a `PendingSubWorkflow` entry's own nested ask is real, unbuilt follow-on work and gets
// an honestly-empty `Message{}` placeholder, never a fabricated one). Answer:
// `ContentItem{Custom{type_id="agentengine.workflow_request_port_response",
// payload_json={"interaction_id":...,"response":<message_to_json(response)>}}}`, matched against the
// CURRENT `open_interactions()` set on every call (so a stale answer to an already-resolved interaction
// is silently skipped, not a special case -- it simply never matches).
//
// EffectContext -- SANITIZED WHOLE-STRUCT COPY, crossing the thread boundary exactly once, before
// detaching. Mirrors `tool_pipeline.hpp::background_task()`'s own real, established pattern (copy-THEN-
// SANITIZE, not copy-and-trust) rather than a hand-picked field list -- `bound_capabilities`/
// `capabilities`/`sandbox_fs` are nulled (borrowed, never owned; also matches this adapter's own I2
// stance below: zero implicit capability flow across this boundary either way), `report_progress`/
// `agent_turn_sink`/`moderator_delta_sink` reset to their no-op defaults (call-scoped reverse channels,
// ADR-060 §4/ADR-152 -- reaching back into a caller's own frame from a detached thread after that frame
// has returned is exactly the hazard those resets close), `cancellation`/`deadline`/
// `tool_result_byte_threshold`/`blob_sink` kept (this adapter's own design genuinely needs them on the
// worker thread). `chat_stream()` NEVER reads the caller's own `EffectContext&` from the detached
// worker -- everything crosses as an owned value, copied synchronously before the thread starts.
//
// CAPABILITY SOURCING (I2) -- same answer `workflow_as_executor.hpp`'s sibling adapter already
// established: the caller's own `EffectContext` is UNUSED for capability purposes. The wrapped
// workflow's own executors run under whatever `EffectContext`s were already supplied to
// `inner->initialize(..., contexts, ...)` -- entirely decoupled from whatever the outer caller holds.
//
// CONCURRENCY -- `call_mutex_` (an adapter-owned `std::mutex`, independent of `WorkflowSupervisor`'s own
// `AsyncMutex`-based `run_mutex_`) is acquired ON THE DETACHED WORKER THREAD, for the whole read-then-act
// body -- never synchronously inside `chat_stream()` before detaching, which would block the calling
// thread and reintroduce the exact regression the detached-worker design exists to close.
//
// `inner` must already be `initialize()`d before being wrapped.

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

namespace workflow_chat_client_detail {

// Same hand-rolled "resume until done" loop every rt:: file driving a task<T> from a plain,
// non-coroutine call site duplicates (workflow_as_executor.hpp's own copy, this codebase's own
// established convention -- not deduplicated elsewhere, not deduplicated here).
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// File banner's "EffectContext" paragraph, extracted as its own directly-testable function rather than
// left inline in chat_stream() -- mirrors tool_pipeline.hpp::background_task()'s real pattern (copy-
// THEN-sanitize, not copy-and-trust): nulls every borrowed/non-owning pointer field and resets every
// call-scoped reverse-channel callback to its no-op default, keeping only what THIS adapter's own
// design genuinely needs on the detached worker thread (cancellation/deadline/tool_result_byte_threshold/
// blob_sink). `ctx` is taken BY VALUE -- the caller already made the one real copy (`ctx_copy = ctx` at
// the call site); this function only ever mutates that owned copy, never the caller's own reference.
[[nodiscard]] inline agentengine::EffectContext sanitize_for_detached_worker(
    agentengine::EffectContext ctx) {
    ctx.bound_capabilities = nullptr;
    ctx.capabilities = nullptr;
    ctx.sandbox_fs = nullptr;
    ctx.report_progress = [](agentengine::ContentItem) {};
    ctx.agent_turn_sink = [](agentengine::RunEvent const&) {};
    ctx.moderator_delta_sink = [](std::string const&) {};
    return ctx;
}

// Type-id constants -- the wire vocabulary this adapter and a purpose-built wrapped `start` executor
// share. See file banner's "MESSAGE HISTORY"/"SUSPENDED INTERACTIONS" paragraphs.
inline constexpr char const* kHistoryTypeId = "agentengine.workflow_chat_client_history";
inline constexpr char const* kAskTypeId = "agentengine.workflow_request_port";
inline constexpr char const* kResponseTypeId = "agentengine.workflow_request_port_response";

// Builds the history envelope for a FRESH call (§4a's "Empty" branch) -- the whole `messages` list,
// JSON-encoded via this codebase's own existing Message<->JSON codec, wrapped as one opaque `Custom`
// item, promoted to `Media{BlobRef}` when oversized. See file banner's "MESSAGE HISTORY" paragraph for
// the full rationale (this mirrors tool_pipeline.hpp::normalize_success()'s own fail-closed rule).
[[nodiscard]] inline agentengine::result<agentengine::Message> build_history_envelope(
    std::vector<agentengine::Message> const& messages, agentengine::EffectContext const& ctx) {
    std::vector<agentengine::json::Value> arr;
    arr.reserve(messages.size());
    bool any_tainted = false;
    for (agentengine::Message const& m : messages) {
        arr.push_back(agentengine::rt::message_to_json(m));
        for (agentengine::ContentItem const& item : m.content) {
            if (item.tainted) any_tainted = true;
        }
    }
    std::string payload_json =
        agentengine::json::dump(agentengine::json::Value::make_array(std::move(arr)));

    agentengine::Message out{};
    out.role = agentengine::role::user;

    if (ctx.tool_result_byte_threshold.has_value() &&
        payload_json.size() > *ctx.tool_result_byte_threshold) {
        if (!ctx.blob_sink) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::resource,
                "workflow chat history (" + std::to_string(payload_json.size()) +
                    " bytes) exceeds the run's byte threshold (" +
                    std::to_string(*ctx.tool_result_byte_threshold) +
                    ") and no blob sink is configured to promote it",
                "chat_client.workflow_chat_client.history_oversized_no_sink"});
        }
        std::span<std::byte const> bytes{reinterpret_cast<std::byte const*>(payload_json.data()),
                                          payload_json.size()};
        auto blob = ctx.blob_sink(bytes, "application/json");
        if (!blob) return std::unexpected(blob.error());
        agentengine::ContentItem item{};
        item.origin = agentengine::content_origin::external;
        item.tainted = any_tainted;
        item.value = agentengine::Media{*blob, "application/json"};
        out.content.push_back(std::move(item));
        return out;
    }

    agentengine::ContentItem item{};
    item.origin = agentengine::content_origin::external;
    item.tainted = any_tainted;
    item.value = agentengine::Custom{kHistoryTypeId, std::move(payload_json)};
    out.content.push_back(std::move(item));
    return out;
}

// Builds one ask-signal ContentItem for one currently-open interaction. See file banner's "SUSPENDED
// INTERACTIONS" paragraph.
[[nodiscard]] inline agentengine::ContentItem build_ask_item(
    agentengine::rt::WorkflowSupervisor::InteractionAsk const& a) {
    std::vector<std::pair<std::string, agentengine::json::Value>> obj;
    obj.emplace_back("interaction_id",
                      agentengine::json::Value::make_string(a.interaction.interaction_id));
    obj.emplace_back("ask", agentengine::rt::message_to_json(a.ask));
    std::string payload_json =
        agentengine::json::dump(agentengine::json::Value::make_object(std::move(obj)));

    agentengine::ContentItem item{};
    item.origin = agentengine::content_origin::assistant;
    item.tainted = false;
    item.value = agentengine::Custom{kAskTypeId, std::move(payload_json)};
    return item;
}

// One matched resume signal found in the caller's own request.messages.
struct ResumeSignal {
    std::string          interaction_id;
    agentengine::Message response;
};

// Scans `messages` for a Custom response-signal item naming an interaction_id currently in `open` --
// a stale answer to an already-resolved interaction (or a malformed payload) is silently skipped, not a
// special case: it simply never matches. See file banner's "SUSPENDED INTERACTIONS" paragraph.
[[nodiscard]] inline std::vector<ResumeSignal> find_resume_signals(
    std::vector<agentengine::Message> const& messages,
    std::vector<agentengine::Interaction> const& open) {
    std::vector<ResumeSignal> out;
    for (agentengine::Message const& m : messages) {
        for (agentengine::ContentItem const& item : m.content) {
            auto const* custom = std::get_if<agentengine::Custom>(&item.value);
            if (custom == nullptr || custom->type_id != kResponseTypeId) continue;
            auto parsed = agentengine::json::parse(custom->payload_json);
            if (!parsed) continue;
            agentengine::json::Value const* interaction_id_v = parsed->find("interaction_id");
            agentengine::json::Value const* response_v = parsed->find("response");
            if (interaction_id_v == nullptr || !interaction_id_v->is_string() || response_v == nullptr) {
                continue;
            }
            std::string const interaction_id = interaction_id_v->as_string();
            bool is_open = false;
            for (agentengine::Interaction const& oi : open) {
                if (oi.interaction_id == interaction_id) {
                    is_open = true;
                    break;
                }
            }
            if (!is_open) continue;
            auto response_msg = agentengine::rt::message_from_json(*response_v);
            if (!response_msg) continue;
            out.push_back(ResumeSignal{interaction_id, std::move(*response_msg)});
        }
    }
    return out;
}

// The whole read-then-act algorithm PLUS the push/fold step -- runs entirely on the detached worker
// thread `chat_stream()` spawns. `ctx` is the SANITIZED owned copy `chat_stream()` built synchronously
// before detaching (file banner's "EffectContext" paragraph); `request` is likewise an owned copy.
inline void run_worker(std::shared_ptr<agentengine::rt::WorkflowSupervisor> inner,
                        std::shared_ptr<std::mutex> call_mutex, agentengine::ChatRequest request,
                        agentengine::EffectContext ctx,
                        agentengine::stream_producer<agentengine::ChatResponseUpdate> producer) {
    std::lock_guard<std::mutex> guard(*call_mutex);

    // File banner's "CONCURRENCY"/round-8 §8 cancellation bridge: registering a stop_callback on an
    // ALREADY-stopped token invokes the callback immediately and synchronously (the standard's own
    // guarantee), so a caller whose EffectContext was already cancelled before this call even started
    // correctly cancels the inner run at the first opportunity. cancel()'s own body takes no lock.
    std::stop_callback bridge(ctx.cancellation, [inner] { inner->cancel(); });

    if (ctx.deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= ctx.deadline) {
        producer.fail(agentengine::error{
            agentengine::failure_class::resource,
            "workflow chat call's deadline had already passed before the call could start",
            "chat_client.workflow_chat_client.deadline_exceeded"});
        return;
    }

    // GitHub issue #35 follow-up (ADR-163): `WorkflowSupervisor::usage()` is CUMULATIVE across this
    // instance's whole suspend/resume lifecycle (never reset by resume_workflow(), only by a fresh
    // run_workflow() -- see that accessor's own comment), but this adapter must report per-CALL usage,
    // matching every other real ChatClient conformer's own "usage for just this exchange" contract (an
    // outer AgentSession sums per-call deltas into ITS OWN running total; reporting the cumulative
    // total on every call would badly over-count there). Snapshotting before/after this one call's own
    // dispatch and reporting the delta is correct for BOTH the fresh-call case (before is always zero,
    // just reset) and the resume case (before is whatever accumulated across earlier calls so far).
    agentengine::Usage const usage_before = inner->usage();

    std::vector<agentengine::Interaction> open = inner->open_interactions();
    agentengine::rt::WorkflowResult r;

    if (open.empty()) {
        auto envelope = build_history_envelope(request.messages, ctx);
        if (!envelope) {
            producer.fail(envelope.error());
            return;
        }
        r = drive(inner->run_workflow(agentengine::rt::RunWorkflow{*envelope}));
    } else {
        std::vector<ResumeSignal> signals = find_resume_signals(request.messages, open);
        if (signals.empty()) {
            producer.fail(agentengine::error{
                agentengine::failure_class::contract,
                "workflow chat call: a prior turn is still paused waiting for an answer, but this "
                "request carries no matching resume signal for any currently-open interaction -- "
                "never silently discarding the paused run or starting a second concurrent one",
                "chat_client.workflow_chat_client.no_matching_resume_signal"});
            return;
        }
        // Loop over every matched signal, re-fetching open_interactions() before each next
        // resume_workflow() call -- resolving one interaction never mutates any OTHER currently-open
        // interaction's id (proven against the real resume_workflow() body during design), so this
        // correctly handles a caller answering M of N open interactions in one call. Only the LAST
        // iteration's WorkflowResult is what gets folded below -- every earlier one is necessarily an
        // intermediate `suspended` result, superseded by construction.
        for (ResumeSignal const& sig : signals) {
            std::vector<agentengine::Interaction> const still_open = inner->open_interactions();
            bool still_present = false;
            for (agentengine::Interaction const& oi : still_open) {
                if (oi.interaction_id == sig.interaction_id) {
                    still_present = true;
                    break;
                }
            }
            if (!still_present) continue;  // resolved by an earlier signal in this same loop
            r = drive(inner->resume_workflow(
                agentengine::rt::ResumeWorkflow{sig.interaction_id, sig.response, {}}));
        }
    }

    // GitHub issue #35 follow-up (ADR-163): the real per-call delta, read ONCE here (not per-push) --
    // `inner->usage()` only advances at well-defined fold points inside `execute()`, never mid-push, so
    // one read after `r` is already final is exactly this call's own total. Applied to the TERMINAL
    // push only (`ChatResponseUpdate::usage`'s own doc comment: "populated only on the terminal
    // update"), never fabricated on an intermediate one.
    agentengine::Usage const usage_after = inner->usage();
    agentengine::Usage usage_delta{};
    usage_delta.input_tokens = usage_after.input_tokens - usage_before.input_tokens;
    usage_delta.output_tokens = usage_after.output_tokens - usage_before.output_tokens;
    usage_delta.cached_input_tokens = usage_after.cached_input_tokens - usage_before.cached_input_tokens;
    usage_delta.reasoning_tokens = usage_after.reasoning_tokens - usage_before.reasoning_tokens;
    usage_delta.cost_estimate = usage_after.cost_estimate - usage_before.cost_estimate;
    usage_delta.cache_write_tokens = usage_after.cache_write_tokens - usage_before.cache_write_tokens;

    switch (r.status) {
        case agentengine::rt::workflow_status::completed: {
            if (r.output.content.empty()) {
                agentengine::ChatResponseUpdate upd{};
                upd.is_final = true;
                upd.usage = usage_delta;
                if (producer.push(std::move(upd)) == agentengine::stream_push::ok) {
                    producer.close();
                }
                return;
            }
            for (std::size_t i = 0; i < r.output.content.size(); ++i) {
                agentengine::ChatResponseUpdate upd{};
                upd.delta = r.output.content[i];
                upd.is_final = (i + 1 == r.output.content.size());
                if (upd.is_final) upd.usage = usage_delta;
                if (producer.push(std::move(upd)) != agentengine::stream_push::ok) return;
            }
            producer.close();
            return;
        }
        case agentengine::rt::workflow_status::suspended: {
            std::vector<agentengine::rt::WorkflowSupervisor::InteractionAsk> const asks =
                inner->open_interaction_asks();
            if (asks.empty()) {
                // Structurally shouldn't happen (suspended implies open_interactions() non-empty), but
                // fail closed rather than push zero updates and close as if nothing were pending.
                producer.fail(agentengine::error{
                    agentengine::failure_class::fatal,
                    "workflow chat call: inner run reports suspended with no open interaction asks",
                    "chat_client.workflow_chat_client.suspended_with_no_asks"});
                return;
            }
            for (std::size_t i = 0; i < asks.size(); ++i) {
                agentengine::ChatResponseUpdate upd{};
                upd.delta = build_ask_item(asks[i]);
                upd.is_final = (i + 1 == asks.size());
                if (upd.is_final) upd.usage = usage_delta;
                if (producer.push(std::move(upd)) != agentengine::stream_push::ok) return;
            }
            producer.close();
            return;
        }
        default: {
            char const* const tag = agentengine::rt::workflow_status_tag(r.status);
            producer.fail(agentengine::error{
                agentengine::failure_class::contract,
                std::string("workflow chat call: the wrapped workflow did not complete (status=") +
                    tag + ")",
                std::string("chat_client.workflow_chat_client.inner_run_not_completed.") + tag});
            return;
        }
    }
}

}  // namespace workflow_chat_client_detail

// See file banner for the full design and every rejected alternative.
class WorkflowChatClient {
public:
    explicit WorkflowChatClient(std::shared_ptr<agentengine::rt::WorkflowSupervisor> inner)
        : inner_(std::move(inner)), call_mutex_(std::make_shared<std::mutex>()) {}

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const {
        return agentengine::ChatClientCapabilities{.streaming = false, .tool_calling = false};
    }

    // ONLY method -- see file banner's "DELIBERATELY NO chat()" paragraph. Returns synchronously,
    // exactly like every other real chat_stream() conformer in this codebase; the whole read-then-act
    // algorithm runs on a detached worker thread this call spawns.
    [[nodiscard]] agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(
        agentengine::ChatRequest const& request, agentengine::EffectContext& ctx) const {
        auto pair = agentengine::make_stream<agentengine::ChatResponseUpdate>(
            std::pmr::get_default_resource());

        // File banner's "EffectContext" paragraph: sanitized whole-struct copy, made synchronously,
        // BEFORE detaching. ctx itself (the caller's own reference) is never captured or read from the
        // detached thread.
        agentengine::EffectContext ctx_copy =
            workflow_chat_client_detail::sanitize_for_detached_worker(ctx);

        std::thread(&workflow_chat_client_detail::run_worker, inner_, call_mutex_, request,
                    std::move(ctx_copy), std::move(pair.producer))
            .detach();
        return std::move(pair.consumer);
    }

private:
    std::shared_ptr<agentengine::rt::WorkflowSupervisor> inner_;
    std::shared_ptr<std::mutex> call_mutex_;
};

static_assert(agentengine::ChatClient<WorkflowChatClient>,
              "WorkflowChatClient must satisfy the real ChatClient concept (004 §1) via chat_stream() "
              "alone -- checked directly here, not deferred to per-instantiation tests.");

}  // namespace agentengine::rt
