// This file MUST NOT compile (decisions/ADR-187-evaluation-harness.md §3.8, round-1 red-team fix on
// the trial-running harness's first slice) -- see tests/CMakeLists.txt's try_compile() gate.
// `run_trial`'s own top comment claims wrapping `Inner` in `RecordingChatClient` inside its
// signature "enforces refuses to run a trial whose client is not wrapped... by type." A round-1
// reviewer proved that claim was type-true but not effect-true: passing an ALREADY-WRAPPED
// `RecordingChatClient<X>` as `Inner` compiled cleanly and chained the caller's own external sink
// onto every trial call, entirely outside the trial's own EvalStore/CapabilitySet confinement. This
// file is exactly that attempt, and it must now fail via the static_assert eval_trial.hpp added.

#include "agentengine/core/recording_chat_client.hpp"
#include "agentengine/eval/eval_trial.hpp"

using namespace agentengine;

class DummyInner {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return ChatResponse{};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
};

class DummySummarizer {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) { co_return ChatResponse{}; }
    stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
};

int main() {
    RecordingChatClient<DummyInner> already_wrapped(DummyInner{});
    eval::TrialSpec spec;
    spec.arm = eval::trial_arm::baseline;
    spec.trial_id = "compile-fail-probe";
    // must not compile: Inner == RecordingChatClient<DummyInner>, rejected by run_trial's own
    // static_assert(!detail::is_recording_chat_client_v<Inner>, ...).
    auto t = eval::run_trial(already_wrapped, DummySummarizer{}, spec);
    (void)t;
    return 0;
}
