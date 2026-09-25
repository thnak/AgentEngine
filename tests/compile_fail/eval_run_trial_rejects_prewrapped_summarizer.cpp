// This file MUST NOT compile (decisions/ADR-195-evaluation-harness.md §3.8, round-2 red-team fix on
// the trial-running harness's round-1 fixes) -- see tests/CMakeLists.txt's try_compile() gate.
// Round 1 added a static_assert rejecting an already-wrapped RecordingChatClient<X> as `Inner`, but
// left `SummarizerT` completely unchecked. A round-2 reviewer proved that compiled cleanly and let a
// pre-wrapped summarizer's own external sink observe MemoryProvider::on_turn_end's per-turn content
// (including a recall reply's lesson text) outside the trial's own EvalStore/CapabilitySet
// confinement -- the identical escape round 1 fixed for `Inner`, just on the untouched parameter.
// This file is exactly that attempt, and it must now fail via the static_assert eval_trial.hpp added.

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
    RecordingChatClient<DummySummarizer> already_wrapped_summarizer(DummySummarizer{});
    eval::TrialSpec spec;
    spec.arm = eval::trial_arm::baseline;
    spec.trial_id = "compile-fail-probe-summarizer";
    // must not compile: SummarizerT == RecordingChatClient<DummySummarizer>, rejected by run_trial's
    // static_assert(!detail::is_recording_chat_client_after_decay_v<SummarizerT>, ...).
    auto t = eval::run_trial(DummyInner{}, already_wrapped_summarizer, spec);
    (void)t;
    return 0;
}
