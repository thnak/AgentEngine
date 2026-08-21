#pragma once
// Implements decisions/ADR-069-content-triggered-model-response-replay.md: a settled model response
// has already committed content that violates policy (a secret, a toxicity hit, anything a pluggable
// trigger flags) -- this file discards it and re-invokes the underlying model call with a corrective
// instruction, rather than letting it commit to durable history/checkpoint/transcript at all.
// Distinct from `002-Agent-Model-and-Authoring.md` §3's `Retry<Policy>` (a TRANSIENT-FAILURE retry
// shape -- retrying because a call ERRORED) -- this is a CONTENT-triggered retry: the call succeeded,
// but what it produced must never be kept.
//
// SCOPE, same "prove the mechanism against one real consumer, name the rest" precedent
// `middleware.hpp`/`turn_middleware.hpp` already establish for their own points: `ContentReplayGateway`
// wraps any `ModelCallGatewayLike` (`chat_client.hpp`) -- typically a `ModelCallGateway<...>` or a
// `MiddlewareModelCallGateway<...>` (`model_call_gateway.hpp`, ADR-036/033), composed the SAME way
// those two types already compose over each other, not folded into either. This proves the mechanism
// as its own standalone, tested wrapper; wiring an instance of it into `rt::AgentSession`'s own
// chat-client template slot for a real end-to-end run is separate, unscoped work (a real deployment
// decision -- does a session opt into this at all -- not a small follow-up), matching this codebase's
// standing precedent for every other interception point proven this same way.
//
// HONEST SCOPE LIMIT (design draft §2, carried into the real code): replaying CANNOT un-send what the
// model provider already received in THIS round's ORIGINAL request -- if a secret arrived via a tool
// result the model then read, that transmission already happened. What this file actually achieves:
// preventing the tainted RESPONSE from committing to durable history, and giving the model a
// corrected instruction to continue from. A DELIBERATE, related choice found while implementing this:
// the amended retry request appends ONLY `corrective_instruction`, NEVER the discarded response
// itself -- re-including a response that was discarded specifically because it leaked something would
// SEND THAT SAME CONTENT TO THE VENDOR A SECOND TIME, inside the very call meant to correct it. The
// design draft did not spell this out explicitly; it follows directly from §2's own "containment of
// what AgentEngine persists" framing once actually building the retry request forced the question.
//
// STREAMING IS STRUCTURALLY EXCLUDED, not merely documented as excluded: `call()` below only ever
// invokes `Inner::call()` (`ModelCallGatewayLike`'s coroutine method) -- there is no `chat_stream()`
// method on this type at all for a caller to reach, the same "proof by absence" `turn_middleware.hpp`'s
// `Compactor<N>`/`history[]` claim already uses, not a runtime check that could be bypassed.
//
// TokenBudget ACCOUNTING, real finding from implementation: `call()`'s own return type is ONE
// `task<result<ChatResponse>>` -- by construction, a caller outside this file can only ever observe
// the FINALLY-KEPT (or finally-failed) attempt's `Usage`. The design draft's own §3 flagged
// `TokenBudget` accounting every replay attempt's real usage as "must be proven, not assumed"; what
// THIS file actually closes is narrower and real: `ContentReplayTraceHook` (below) fires once per
// attempt, discarded or not, carrying that attempt's real `Usage` when it succeeded -- a host that
// wires this hook can account every attempt's true cost for ITS OWN budget bookkeeping. Whether
// `rt::AgentSession`'s OWN existing `TokenBudget<N>` (002 §3) is ever wired to consume this hook, so
// the ENGINE's own enforcement (not just an opt-in host observer) sees every attempt, remains a real,
// separate, unclosed integration question -- named here, not silently claimed solved.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

// 017 §4's `pre_model`/`post_model` verdict vocabulary, narrowed to this mechanism's own binary
// need: either the settled response stands (`discard_and_retry == false`, the common case), or it
// must be discarded and the underlying call re-invoked with `corrective_instruction` appended as
// additional context.
struct ContentReplayDecision {
    bool        discard_and_retry = false;
    std::string corrective_instruction;
};

// The pluggable trigger (design draft §4: "the SAME replay machinery serves any 'discard this
// settled response, retry with a correction' need... secrets are one caller, not the reason the
// mechanism exists"). A host-injected callback, matching this codebase's own established idiom
// (`ApprovalDecider`, `MiddlewareTraceHook`, `SecretDetector`) rather than a new interception-point
// vocabulary. `nullptr` is a legal, meaningful value -- fails open to no-replay (§ below).
using ContentReplayTrigger = std::function<ContentReplayDecision(ChatResponse const&)>;

// Fires once per attempt inside ONE `call()` invocation's own retry loop -- including every
// DISCARDED attempt, not just the one finally returned. `usage` is the attempt's real backend cost
// when it succeeded (every discard-and-retry attempt DID reach a real backend and DID cost real
// tokens -- only a genuine backend failure, distinct from a content-triggered discard, leaves this
// `nullopt`). See this file's own top comment for what wiring this hook does and does not close.
struct ContentReplayAttemptEvent {
    std::uint32_t         attempt_index = 0;  // 0-based, within this call()'s own retry loop
    bool                  discarded     = false;
    std::optional<Usage>  usage;
    std::string           reason;  // the corrective_instruction that triggered the NEXT attempt; empty on the final attempt
};
using ContentReplayTraceHook = std::function<void(ContentReplayAttemptEvent const&)>;

// Wraps a `Message{role::system, [Text{corrective_instruction}]}`, `content_origin::system` --
// engine-synthesized, host-configured-trigger-derived text, the SAME origin class
// `rt::AgentSession::run_rounds()` already uses for `ContextContribution.instructions` (ADR-042),
// not `::user` (this text was never typed by the human) and not `::assistant` (the model didn't say
// this either -- it is corrective guidance FROM the mechanism).
[[nodiscard]] inline Message corrective_message(std::string const& corrective_instruction) {
    ContentItem item{};
    item.origin = content_origin::system;
    item.value  = Text{corrective_instruction};
    Message m{};
    m.role = role::system;
    m.content.push_back(std::move(item));
    return m;
}

// decisions/ADR-069 §3's mechanism. Bounded TWO independent ways, both must pass before a replay is
// allowed to proceed: `max_replay_attempts` (per-`call()`-invocation, i.e. per trigger site) AND
// `session_lifetime_cap` (a counter that persists across every `call()` this ONE gateway instance
// ever serves -- meaningful only if this gateway instance itself lives as long as the session that
// owns it, which is the caller's responsibility, not enforced here). Once `session_lifetime_cap` is
// exhausted, EVERY subsequent `call()` that would want to replay fails immediately, on its very
// first attempt -- "fails closed for the rest of the session," matching the design draft's own
// must-fix fix, not merely the per-trigger-site bound resetting every round.
template <class Inner>
class ContentReplayGateway {
    static_assert(ModelCallGatewayLike<Inner>,
                  "ContentReplayGateway's Inner must satisfy ModelCallGatewayLike -- wrap a "
                  "ModelCallGateway<...> or MiddlewareModelCallGateway<...>, the same composition "
                  "shape those two types already use over each other");

public:
    explicit ContentReplayGateway(Inner inner, ContentReplayTrigger trigger,
                                    std::uint32_t max_replay_attempts = 2,
                                    std::uint32_t session_lifetime_cap = 8)
        : inner_(std::move(inner)),
          trigger_(std::move(trigger)),
          max_replay_attempts_(max_replay_attempts == 0 ? 1 : max_replay_attempts),
          session_lifetime_cap_(session_lifetime_cap) {}

    ContentReplayGateway& set_trace_hook(ContentReplayTraceHook hook) {
        trace_hook_ = std::move(hook);
        return *this;
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return inner_.capabilities(); }

    // The session-lifetime counter, for a caller/test that wants to observe it directly rather than
    // only through `ContentReplayTraceHook`.
    [[nodiscard]] std::uint32_t session_replays_used() const noexcept { return session_replays_used_; }

    task<result<ChatResponse>> call(ChatRequest request, EffectContext& ctx) {
        ChatRequest current = std::move(request);

        for (std::uint32_t attempt = 0;; ++attempt) {
            result<ChatResponse> outcome = co_await inner_.call(current, ctx);

            if (!outcome.has_value()) {
                // A real backend/middleware failure is not this mechanism's problem -- propagate as-is,
                // no attempt event fired (nothing succeeded to report a Usage for, and this isn't a
                // content-triggered discard).
                co_return outcome;
            }

            ContentReplayDecision decision{};
            if (trigger_) decision = trigger_(*outcome);

            if (!decision.discard_and_retry) {
                if (trace_hook_) {
                    trace_hook_(ContentReplayAttemptEvent{attempt, /*discarded=*/false, outcome->usage, ""});
                }
                co_return outcome;
            }

            // Bounded both ways -- checked BEFORE committing to another attempt, in either order,
            // both must pass.
            if (attempt + 1 >= max_replay_attempts_) {
                if (trace_hook_) {
                    trace_hook_(ContentReplayAttemptEvent{attempt, /*discarded=*/true, outcome->usage,
                                                            decision.corrective_instruction});
                }
                co_return std::unexpected(
                    error{failure_class::policy,
                          "content-triggered replay: per-trigger-site max_replay_attempts exhausted",
                          "content_replay.max_attempts_exhausted"});
            }
            if (session_replays_used_ >= session_lifetime_cap_) {
                if (trace_hook_) {
                    trace_hook_(ContentReplayAttemptEvent{attempt, /*discarded=*/true, outcome->usage,
                                                            decision.corrective_instruction});
                }
                co_return std::unexpected(error{
                    failure_class::policy,
                    "content-triggered replay: session-lifetime replay cap exhausted -- no further "
                    "replay for the rest of this session",
                    "content_replay.session_cap_exhausted"});
            }

            ++session_replays_used_;
            if (trace_hook_) {
                trace_hook_(ContentReplayAttemptEvent{attempt, /*discarded=*/true, outcome->usage,
                                                        decision.corrective_instruction});
            }

            // See this file's own top comment: the discarded response's OWN content is deliberately
            // never appended here -- only the corrective instruction. Re-including it would re-send
            // whatever got this attempt discarded back to the vendor a second time.
            current.messages.push_back(corrective_message(decision.corrective_instruction));
        }
    }

private:
    Inner                  inner_;
    ContentReplayTrigger   trigger_;
    std::uint32_t          max_replay_attempts_;
    std::uint32_t          session_lifetime_cap_;
    std::uint32_t          session_replays_used_ = 0;
    ContentReplayTraceHook trace_hook_;
};

}  // namespace agentengine
