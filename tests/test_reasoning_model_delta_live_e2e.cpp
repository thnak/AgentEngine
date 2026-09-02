// Issue #49's own follow-up comment (IC_kwDOTqIUBs8AAAABSE2VAA): "A real, checked-in live test
// proving this behavior ... drive a reasoning-capable model (e.g. via OpenRouter) through
// chat_stream() directly, assert that (a) the final assembled ChatResponse contains a real
// Reasoning-kind ContentItem when the backend supports it, and (b) [live model_delta events fire for
// it during streaming] ... something a future backend-specific fix (OpenAI/OpenRouter
// reasoning_content parsing) can be checked against too."
//
// That comment was written BEFORE the fix (it describes the bug: zero model_delta events ever fired
// for a Reasoning delta, and the OpenAI/OpenRouter backend never produced a genuine Reasoning content
// item at all) -- this file proves the FIXED behavior instead, now that both halves of issue #49 are
// closed:
//   1. `run_event.hpp`'s `ModelDelta::value` gained a `ModelReasoningDelta` variant, and
//      `rt/agent_session_trust.hpp`'s `drain_streaming_response()` (the shared function every
//      streaming turn drains through, gateway-routed or not) now forwards a `Reasoning`-kind
//      `ContentItem` delta through it, mirroring the pre-existing `Text` branch exactly.
//   2. `protocol/openai/chat_client.hpp` (OpenAI direct, and OpenRouter's OpenAI-compatible surface --
//      also reused by the Ollama/llama.cpp local-server backends) now parses OpenRouter's own
//      (non-vanilla-OpenAI) `reasoning`/`reasoning_content` streaming extension field into a genuine
//      `Reasoning` content item, both non-streaming (`parse_chat_completion_response`) and streaming
//      (`StreamingUpdateAccumulator::items_from_block`).
// `AnthropicChatClient` already produced genuine `Reasoning` items (Phase E, 004 §3) before this
// issue -- its own gap was ONLY the shared `drain_streaming_response()` forwarding, fixed by (1) alone
// -- so this file exercises the OpenAI-compatible surface specifically, the one that needed BOTH
// halves of the fix to have anything to forward at all.
//
// Drives `openai::OpenAIChatClient<>::chat_stream()` DIRECTLY (comment (4)'s own words) -- not through
// a full `rt::AgentSession` turn -- then feeds the raw `stream<ChatResponseUpdate>` through
// `rt::detail::drain_streaming_response()` itself, the exact function issue #49 named, with a capturing
// `EmitFn` collecting every `RunEvent` it would have emitted. This is the narrowest real proof of the
// whole pipeline: backend wire parsing -> ChatResponseUpdate -> drain_streaming_response -> RunEvent.
//
// Confirmed live, 2026-09-02, directly against OpenRouter's raw wire (not merely inferred from specs)
// before this fix was written: the `reasoning` delta field is not DeepSeek-specific -- the identical
// shape (`delta.reasoning`, separate from and interleaved ahead of `delta.content`) was observed
// against `deepseek/deepseek-v3.2-exp`, `qwen/qwen3-235b-a22b-thinking-2507` (Alibaba), and
// `z-ai/glm-4.6` (Zhipu AI) -- three independent model vendors through the same OpenRouter surface --
// so `reasoning_field_of()`'s parsing is a genuine cross-vendor wire convention, not a single-vendor
// special case. This file still exercises only the suite's existing default model (below) to keep one
// live credential's cost/flakiness surface no larger than every sibling file in this suite already
// accepts; the cross-vendor claim above is a repro finding, not re-asserted per-model here.
//
// Mirrors tests/test_openrouter_live_e2e.cpp's / test_openai_chat_client_openrouter_live_e2e.cpp's
// EXACT pattern (env-var-gated credential, SKIP not FAIL when absent, structural-only assertions) --
// see either file's own top comment for the full rationale, not repeated here.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_OPENROUTER_API_KEY             required -- unset means SKIP (exit 0), never a failure.
//     (Reuses the SAME key every other live test in this suite uses.)
//   AGENTENGINE_OPENROUTER_REASONING_MODEL     optional -- default below, confirmed live to work.
//   AGENTENGINE_OPENROUTER_HOST                optional -- default `openrouter.ai`.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session_trust.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;

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

void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Same routing alias test_openrouter_live_e2e.cpp already defaults to -- confirmed live (this
// session, before this file was written) to emit a real, separate `reasoning`/`reasoning_content`
// streaming delta when `reasoning_effort` is set, exactly the shape `build_request_body` sends below.
constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr char const* kXTitle = "AgentEngine: issue-49-reasoning-model-delta-live-e2e";

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "test_reasoning_model_delta_live_e2e: SKIPPED -- "
                      "AGENTENGINE_OPENROUTER_API_KEY is not set.\n  Run "
                      "tools/run-live-provider-tests.ps1, or set the variable yourself, to exercise "
                      "the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_REASONING_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_reasoning_model_delta_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"live-e2e-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.reasoning = true;
    caps.max_output_tokens = 512;

    openai::OpenAIChatClient<InMemorySecretStore> oai(
        host, kHttpsPort, model, SecretRef{kSecretName}, caps, store, kPathPrefix, sandbox::resolve_host,
        /*ca=*/{}, /*http_referer=*/{}, /*x_title=*/kXTitle,
        /*end_user_id=*/"test-reasoning-model-delta-live-e2e", /*seed=*/std::nullopt,
        /*transport=*/sandbox::ProviderTransport::tls, /*scan_response_format_leaks=*/false,
        /*session_id=*/"test-reasoning-model-delta-live-e2e");

    static_assert(ChatClient<decltype(oai)>, "OpenAIChatClient must satisfy the ChatClient concept");

    // ---- RM-1: chat_stream() directly, drained through drain_streaming_response() (comment (4)'s own
    // named function), a real reasoning-capable model, `reasoning_effort` set so the provider actually
    // emits a trace (confirmed live: this model emits NO reasoning field at all when unset) ----------
    {
        ChatRequest req;
        req.messages.push_back(user_message("What is 12*13? Think it through, then answer."));
        req.reasoning_effort = reasoning_effort::medium;

        stream<ChatResponseUpdate> s = oai.chat_stream(req, ctx);

        std::vector<std::pair<run_event_kind, RunEventPayload>> events;
        rt::detail::EmitFn emit = [&](run_event_kind kind, RunEventPayload payload) {
            events.emplace_back(kind, std::move(payload));
        };
        result<ChatResponse> resp =
            rt::detail::drain_streaming_response(std::move(s), /*stream_model_calls=*/true, emit);

        check(resp.has_value(),
              "RM-1: drain_streaming_response() reaches a clean terminal with real reported usage "
              "over a real OpenRouter reasoning-model stream");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
            std::fprintf(stderr, "test_reasoning_model_delta_live_e2e: %d FAILURE(S)\n", g_failures + 1);
            return 1;
        }

        // ---- (a) the final assembled ChatResponse contains a real Reasoning-kind ContentItem -------
        std::string final_reasoning_text;
        std::string final_answer_text;
        std::string final_producer_id;
        for (ContentItem const& item : resp->message.content) {
            if (auto const* r = std::get_if<Reasoning>(&item.value)) {
                final_reasoning_text += r->text;
                final_producer_id = r->producer_chat_client_id;
            } else if (auto const* t = std::get_if<Text>(&item.value)) {
                final_answer_text += t->text;
            }
        }
        check(!final_reasoning_text.empty(),
              "RM-1a: the final assembled ChatResponse contains a real Reasoning-kind ContentItem "
              "(issue #49's gap 3: OpenAIChatClient/OpenRouter previously never produced one at all)");
        check(final_producer_id == oai.producer_chat_client_id(),
              "RM-1a: the Reasoning item is stamped with THIS backend's own producer_chat_client_id "
              "(gap-audit finding 20 / 003 §8 Q2), not left empty/unknown-provenance");
        check(!final_answer_text.empty(), "RM-1: the same turn also produced a real Text answer");
        note("final reasoning text", final_reasoning_text);
        note("final answer text", final_answer_text);
        note("producer_chat_client_id", final_producer_id);

        // ---- (b) real model_delta events fired for the reasoning trace WHILE STREAMING, not zero ----
        // (issue #49's core finding: before this fix, EVERY branch in drain_streaming_response()
        // failed for a Reasoning delta, so NO event fired for it at all -- this is the fix, proven.)
        std::string live_reasoning_text;
        std::string live_answer_text;
        std::size_t reasoning_delta_events = 0;
        std::size_t text_delta_events = 0;
        for (auto const& [kind, payload] : events) {
            if (kind != run_event_kind::model_delta) continue;
            auto const& d = std::get<run_event_payload::ModelDelta>(payload);
            if (auto const* r = std::get_if<run_event_payload::ModelReasoningDelta>(&d.value)) {
                live_reasoning_text += r->text;
                ++reasoning_delta_events;
            } else if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d.value)) {
                live_answer_text += t->text;
                ++text_delta_events;
            }
        }
        check(reasoning_delta_events > 0,
              "RM-1b: at least one live model_delta event carries a ModelReasoningDelta -- BEFORE "
              "this fix, this count was unconditionally zero for every backend, every turn");
        check(text_delta_events > 0,
              "RM-1b: Text-kind model_delta events ALSO fire for the same turn's answer -- proving "
              "reasoning forwarding is additive, not a replacement for the pre-existing Text path");
        note("reasoning_delta_events", std::to_string(reasoning_delta_events));
        note("text_delta_events", std::to_string(text_delta_events));

        // The live events and the final assembled message are sourced from the exact same sequence of
        // `ChatResponseUpdate`s (drain_streaming_response() both emits AND appends each one) -- so
        // their concatenated text must agree exactly, not merely both-be-non-empty.
        check(live_reasoning_text == final_reasoning_text,
              "RM-1: the concatenated live ModelReasoningDelta text equals the final Reasoning "
              "ContentItem(s)' concatenated text -- the same delta, not a divergent second copy");
        check(live_answer_text == final_answer_text,
              "RM-1: the concatenated live ModelTextDelta text equals the final Text ContentItem(s)' "
              "concatenated text (the same invariant already held before this fix; unaffected by it)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_reasoning_model_delta_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_reasoning_model_delta_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
