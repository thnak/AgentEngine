// Milestone 5 Phase F1+F2 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md,
// Phase F's "Design decisions (2026-08-07..." block): proves `ResilientChatClient<Inner>`
// (core/resilient_chat_client.hpp) end to end against a hand-rolled, scripted `ChatClient`
// conformer -- no real backend, no network.
//
// Covers:
//  (1) a transient failure retries and eventually succeeds within budget.
//  (2) retries stop and return the error once the deadline is exhausted, without sleeping past it.
//  (3) a non-transient error (failure_class::contract) does NOT retry.
//  (4) the circuit breaker trips after N consecutive failures and starts shedding immediately (no
//      more calls reach Inner) until its cooldown elapses, then allows exactly one half-open probe.
//  (5) the idempotency key is identical across every retry attempt of the same logical call.
//
// NOT built into tests/CMakeLists.txt yet (per the task's own instruction, to avoid a merge
// conflict with other agents editing that file concurrently) -- wiring is pending.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/resilient_chat_client.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // IdempotencyKey
#include "agentengine/trust/principal.hpp"

#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// What ScriptedChatClient::chat() should do on a given call, in order. Exhausting the script
// repeats the LAST entry (mirrors tests/support/recorded_chat_client.hpp's own "one scenario, kept
// simple" spirit -- a test just needs enough scripted entries to prove its own point, not an
// infinite generator).
enum class ScriptedOutcome { Success, TransientError, ContractError };

// A minimal, hand-rolled ChatClient conformer (chat_client.hpp) that plays back a fixed script of
// outcomes and records every request it actually receives -- in particular, `idempotency_key`, so
// test (5) can assert it never changes across retries of the SAME logical call.
//
// State lives behind a `shared_ptr` deliberately: `ResilientChatClient<Inner>` takes `Inner` BY
// VALUE and stores its own copy (core/resilient_chat_client.hpp's constructor), so the local
// `ScriptedChatClient` a test constructs and the one actually invoked from inside `client` are two
// distinct objects after construction. Sharing the counters/log through a `shared_ptr` (copied,
// not deep-copied, by ScriptedChatClient's implicit copy/move members) is what lets the test's own
// local handle observe calls made through the wrapper's internal copy.
class ScriptedChatClient {
public:
    struct State {
        std::size_t call_count = 0;
        std::vector<std::string> observed_idempotency_keys;
        std::vector<ScriptedOutcome> script;
    };

    explicit ScriptedChatClient(std::vector<ScriptedOutcome> script)
        : state_(std::make_shared<State>(State{0, {}, std::move(script)})) {}

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }

    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(
        agentengine::ChatRequest request, agentengine::EffectContext&) {
        state_->observed_idempotency_keys.push_back(request.idempotency_key.value_or(std::string{}));
        std::vector<ScriptedOutcome> const& script = state_->script;
        std::size_t const idx = state_->call_count < script.size() ? state_->call_count : script.size() - 1;
        ScriptedOutcome const outcome = script[idx];
        ++state_->call_count;
        switch (outcome) {
            case ScriptedOutcome::Success:
                co_return agentengine::ChatResponse{};
            case ScriptedOutcome::TransientError:
                co_return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                               "scripted transient failure",
                                                               "test.scripted_transient"});
            case ScriptedOutcome::ContractError:
                co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                               "scripted contract failure",
                                                               "test.scripted_contract"});
        }
        co_return std::unexpected(
            agentengine::error{agentengine::failure_class::fatal, "unreachable", "test.unreachable"});
    }

    // Unused by these tests (F1 retry / F2 breaker admission on chat() only) -- present only to
    // satisfy the ChatClient concept.
    agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest,
                                                                       agentengine::EffectContext&) {
        return {};
    }

    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] std::vector<std::string> const& observed_idempotency_keys() const {
        return state_->observed_idempotency_keys;
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>,
              "ScriptedChatClient must satisfy the real ChatClient concept");
static_assert(agentengine::ChatClient<agentengine::ResilientChatClient<ScriptedChatClient>>,
              "ResilientChatClient<Inner> must itself satisfy the ChatClient concept (chat_client.hpp)");

[[nodiscard]] agentengine::EffectContext make_ctx(std::string run_id, std::uint64_t turn_index) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.run_id = std::move(run_id);
    ctx.turn_index = turn_index;
    return ctx;
}

// A jitter source that always returns 0.0 -- removes randomness from the retry loop so tests can
// assert exact attempt counts / timing without flakiness (the same reasoning
// sandbox/provider_http_client.hpp's own injectable `resolver` comment gives for its seam).
[[nodiscard]] double zero_jitter() { return 0.0; }

}  // namespace

int main() {
    using namespace agentengine;
    using agentengine::test_support::run_task_sync;

    // ---- (1) a transient failure retries and eventually succeeds within budget -------------------
    {
        ScriptedChatClient inner({ScriptedOutcome::TransientError, ScriptedOutcome::TransientError,
                                   ScriptedOutcome::Success});
        RetryPolicy retry;
        retry.max_attempts = 5;
        retry.base_delay = std::chrono::milliseconds(1);
        retry.max_delay = std::chrono::milliseconds(5);
        retry.jitter_fraction = 0.0;
        BreakerConfig breaker;
        breaker.fail_threshold = 10;  // well above the 2 failures this scenario feeds it
        ResilientChatClient<ScriptedChatClient> client(inner, retry, breaker, &zero_jitter);

        EffectContext ctx = make_ctx("run-1", 0);
        auto result = run_task_sync<agentengine::result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));

        check(result.has_value(), "(1) eventually succeeds within the retry budget");
    }

    // ---- (2) retries stop once the deadline is exhausted, without sleeping past it -----------------
    {
        // Always transient -- the ONLY reason retrying stops here must be the deadline, not the
        // script running out or the breaker tripping.
        ScriptedChatClient inner({ScriptedOutcome::TransientError});
        RetryPolicy retry;
        retry.max_attempts = 10;
        retry.base_delay = std::chrono::milliseconds(200);  // clearly bigger than the deadline below
        retry.max_delay = std::chrono::milliseconds(1000);
        retry.jitter_fraction = 0.0;
        BreakerConfig breaker;
        breaker.fail_threshold = 100;  // never trips within this scenario's attempt count
        ResilientChatClient<ScriptedChatClient> client(inner, retry, breaker, &zero_jitter);

        EffectContext ctx = make_ctx("run-2", 0);
        ctx.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);

        auto const started = std::chrono::steady_clock::now();
        auto result = run_task_sync<agentengine::result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        auto const elapsed = std::chrono::steady_clock::now() - started;

        check(!result.has_value(), "(2) returns an error once the deadline can't fit another backoff");
        if (!result.has_value()) {
            check(result.error().klass == failure_class::transient,
                  "(2) the returned error is the last real transient error, not a fabricated one");
        }
        // base_delay (200ms) cannot fit inside the 5ms deadline after the first attempt -- so the
        // wrapper must give up after exactly ONE real attempt, never sleeping the full 200ms.
        check(inner.call_count() == 1,
              "(2) exactly one real attempt is made -- the 200ms backoff can't fit the 5ms deadline");
        check(elapsed < std::chrono::milliseconds(100),
              "(2) no retry sleep pushed wall-clock time anywhere near the 200ms backoff (proves the "
              "deadline clamp actually prevented the sleep, not just the retry-loop exit)");
    }

    // ---- (3) a non-transient error does NOT retry ---------------------------------------------------
    {
        ScriptedChatClient inner({ScriptedOutcome::ContractError, ScriptedOutcome::Success});
        RetryPolicy retry;  // defaults (max_attempts=3) -- irrelevant here, contract errors never retry
        BreakerConfig breaker;
        ResilientChatClient<ScriptedChatClient> client(inner, retry, breaker, &zero_jitter);

        EffectContext ctx = make_ctx("run-3", 0);
        auto result = run_task_sync<agentengine::result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));

        check(!result.has_value(), "(3) a contract failure is returned as an error");
        if (!result.has_value()) {
            check(result.error().klass == failure_class::contract,
                  "(3) the error class is preserved verbatim (contract, not transient)");
        }
        check(inner.call_count() == 1,
              "(3) exactly one attempt is made -- a non-transient failure never retries, even though "
              "the script's 2nd entry would have succeeded");
    }

    // ---- (4) the breaker trips after N consecutive failures, sheds, then allows one half-open probe
    {
        // Real attempts (Inner-reaching calls), in order: fail, fail, fail, [shed, no call], probe
        // succeeds. max_attempts=1 so EACH top-level chat() call makes at most one real attempt --
        // isolates breaker behavior from F1's own retry loop.
        ScriptedChatClient inner({ScriptedOutcome::TransientError, ScriptedOutcome::TransientError,
                                   ScriptedOutcome::TransientError, ScriptedOutcome::Success});
        RetryPolicy retry;
        retry.max_attempts = 1;
        BreakerConfig breaker;
        breaker.fail_threshold = 3;
        breaker.open_duration = std::chrono::milliseconds(60);
        ResilientChatClient<ScriptedChatClient> client(inner, retry, breaker, &zero_jitter);

        // 3 consecutive real failures -> trips the breaker.
        for (int i = 0; i < 3; ++i) {
            EffectContext ctx = make_ctx("run-4", static_cast<std::uint64_t>(i));
            auto result = run_task_sync<agentengine::result<ChatResponse>>(
                client.chat(ChatRequest{}, ctx));
            check(!result.has_value(), "(4) each of the first 3 calls fails (scripted transient)");
            if (!result.has_value()) {
                check(result.error().code == "test.scripted_transient",
                      "(4) the first 3 failures are the SCRIPT's own error, not the breaker's -- "
                      "proves these 3 really reached Inner");
            }
        }
        check(inner.call_count() == 3, "(4) 3 real attempts reached Inner to trip the breaker");

        // Immediately after tripping: shed, no call reaches Inner.
        {
            EffectContext ctx = make_ctx("run-4", 3);
            auto result = run_task_sync<agentengine::result<ChatResponse>>(
                client.chat(ChatRequest{}, ctx));
            check(!result.has_value(), "(4) a shed call is still reported as a failure");
            if (!result.has_value()) {
                check(result.error().code == "chat_client.circuit_open",
                      "(4) a shed call is reported with the breaker's OWN error code, not the "
                      "inner client's");
            }
            check(inner.call_count() == 3,
                  "(4) shedding does NOT reach Inner -- call_count stays at 3, not 4");
        }

        // Wait out the cooldown, then the NEXT call must be admitted as the single half-open probe.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            EffectContext ctx = make_ctx("run-4", 4);
            auto result = run_task_sync<agentengine::result<ChatResponse>>(
                client.chat(ChatRequest{}, ctx));
            check(result.has_value(),
                  "(4) after the cooldown elapses, exactly one half-open probe is admitted and, "
                  "since the script's 4th entry is Success, the breaker closes again");
            check(inner.call_count() == 4,
                  "(4) the half-open probe DID reach Inner (call_count advances to 4)");
        }

        // Breaker is Closed again -- a further call behaves normally (reaches Inner, not shed).
        {
            EffectContext ctx = make_ctx("run-4", 5);
            auto result = run_task_sync<agentengine::result<ChatResponse>>(
                client.chat(ChatRequest{}, ctx));
            check(result.has_value(), "(4) after closing, calls are admitted normally again");
            check(inner.call_count() == 5, "(4) call_count advances again -- no longer shedding");
        }
    }

    // ---- (5) the idempotency key is identical across every retry attempt --------------------------
    {
        ScriptedChatClient inner({ScriptedOutcome::TransientError, ScriptedOutcome::TransientError,
                                   ScriptedOutcome::TransientError, ScriptedOutcome::Success});
        RetryPolicy retry;
        retry.max_attempts = 4;
        retry.base_delay = std::chrono::milliseconds(1);
        retry.max_delay = std::chrono::milliseconds(2);
        retry.jitter_fraction = 0.0;
        BreakerConfig breaker;
        breaker.fail_threshold = 10;
        ResilientChatClient<ScriptedChatClient> client(inner, retry, breaker, &zero_jitter);

        EffectContext ctx = make_ctx("run-5", 42);
        auto result = run_task_sync<agentengine::result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));

        check(result.has_value(), "(5) succeeds on the 4th attempt");
        check(inner.call_count() == 4, "(5) exactly 4 real attempts were made");
        check(inner.observed_idempotency_keys().size() == 4,
              "(5) every one of the 4 attempts recorded an idempotency key");

        std::string const expected = IdempotencyKey{"run-5", 42, 0, 0}.to_string();
        for (std::size_t i = 0; i < inner.observed_idempotency_keys().size(); ++i) {
            check(inner.observed_idempotency_keys()[i] == expected,
                  "(5) attempt N's idempotency key matches the expected stable value");
        }
        if (inner.observed_idempotency_keys().size() == 4) {
            check(inner.observed_idempotency_keys()[0] == inner.observed_idempotency_keys()[1] &&
                      inner.observed_idempotency_keys()[1] == inner.observed_idempotency_keys()[2] &&
                      inner.observed_idempotency_keys()[2] == inner.observed_idempotency_keys()[3],
                  "(5) all 4 attempts observed the exact SAME idempotency key -- never regenerated "
                  "per attempt");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_resilient_chat_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_resilient_chat_client: %d FAILURE(S)\n", g_failures);
    return 1;
}
