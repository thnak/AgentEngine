// Positive control for eval_run_trial_rejects_prewrapped_inner.cpp -- proves the static_assert
// rejects a PRE-WRAPPED Inner specifically, not `run_trial` generally: the identical setup, minus
// the pre-wrapping, must still compile.

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
    eval::TrialSpec spec;
    spec.arm = eval::trial_arm::baseline;
    spec.trial_id = "compile-fail-probe-control";
    auto t = eval::run_trial(DummyInner{}, DummySummarizer{}, spec);  // plain Inner: must compile
    (void)t;
    return 0;
}
