// Proof for ADR-033 (decisions/ADR-033-middleware-model-call-chain.md), the model-call
// interception point of 002-Agent-Model-and-Authoring.md §5's `Middleware<Ms...>`
// (`include/agentengine/core/middleware.hpp`).
//
// Every block traces to a specific red-team finding this design incorporates (see ADR-033 §3) --
// most importantly T10/T11/T12, the FATAL finding's fix: a middleware-fabricated or -mutated
// `ToolCall` must never retain `call_provenance::vendor_structured` (the fully-trusted class),
// while a genuine, untouched backend-returned `ToolCall` must NOT be punished by the same
// mechanism (a positive control, not just a denial test).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/middleware.hpp"

#include "support/run_task_sync.hpp"

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

[[nodiscard]] EffectContext make_ctx() {
    EffectContext ctx;
    ctx.run_id = "run-mw-1";
    ctx.turn_index = 0;
    return ctx;
}

ToolCall make_tool_call(std::string call_id, std::string tool_name, std::string args) {
    ToolCall c;
    c.call_id = std::move(call_id);
    c.tool_name = std::move(tool_name);
    c.arguments_json = std::move(args);
    return c;
}

ContentItem tool_call_item(ToolCall call) {
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = std::move(call);
    return item;
}

// -- The fake `Inner` -----------------------------------------------------------------------------
// Records every request it actually receives (so a test can prove a before_model rewrite reached
// it, or prove it was NEVER called at all -- the short-circuit claim). State lives behind a
// shared_ptr for the same reason test_resilient_chat_client.cpp's own ScriptedChatClient does:
// MiddlewareChatClient takes Inner BY VALUE, so the test's local handle and the wrapper's internal
// copy are different objects after construction.
class RecordingChatClient {
public:
    struct State {
        std::size_t call_count = 0;
        std::vector<ChatRequest> received;
        ChatResponse next_response;
        bool fail_next = false;
        error next_error{failure_class::transient, "scripted failure", "test.scripted_failure"};
    };

    RecordingChatClient() : state_(std::make_shared<State>()) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        state_->received.push_back(request);
        ++state_->call_count;
        if (state_->fail_next) co_return std::unexpected(state_->next_error);
        co_return state_->next_response;
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return stream<ChatResponseUpdate>{};
    }

    [[nodiscard]] State& state() { return *state_; }

private:
    std::shared_ptr<State> state_;
};

// -- Synthetic middleware types --------------------------------------------------------------------

// No hooks at all -- legal per "any SUBSET of hooks", including the empty subset. Needs no `name`
// (middleware_name<M>() is only instantiated inside an `if constexpr (HasBeforeModel<M> /
// HasAfterModel<M>)` branch, never reached here).
struct NoopMiddleware {};

// Records "<name>:before" / "<name>:after" into a shared log, in call order -- the ordering proof
// (T6/T8) reads this log directly rather than inferring order from side effects.
class TracingMiddleware {
public:
    static constexpr std::string_view name = "tracing";  // per-instance A/B/C distinction is `tag_`,
                                                          // separate from this type-level identity

    explicit TracingMiddleware(std::shared_ptr<std::vector<std::string>> log, std::string_view tag)
        : log_(std::move(log)), tag_(tag) {}

    task<std::monostate> before_model(ModelCallContext&) {
        log_->push_back(std::string(tag_) + ":before");
        co_return std::monostate{};
    }
    task<std::monostate> after_model(ModelCallContext&) {
        log_->push_back(std::string(tag_) + ":after");
        co_return std::monostate{};
    }

private:
    std::shared_ptr<std::vector<std::string>> log_;
    std::string_view                          tag_;
};

// Only after_model -- proves a middleware may omit before_model and still get its turn on the
// ordinary (non-short-circuited) path (T7).
class AfterOnlyMiddleware {
public:
    static constexpr std::string_view name = "after_only";
    explicit AfterOnlyMiddleware(std::shared_ptr<int> counter) : counter_(std::move(counter)) {}
    task<std::monostate> after_model(ModelCallContext&) {
        ++*counter_;
        co_return std::monostate{};
    }

private:
    std::shared_ptr<int> counter_;
};

// before_model rewrites the request -- proves the real backend receives the REWRITTEN request, not
// the original (T2).
struct RewriteRequestMiddleware {
    static constexpr std::string_view name = "rewrite_request";
    task<std::monostate> before_model(ModelCallContext& c) {
        c.request.idempotency_key = "rewritten-by-middleware";
        co_return std::monostate{};
    }
};

// before_model short-circuits with a fabricated response -- Inner must never be called (T4), and
// any ToolCall inside the fabrication must be downgraded (T10's short-circuit variant).
struct ShortCircuitMiddleware {
    static constexpr std::string_view name = "short_circuit";
    ChatResponse fabricated;
    explicit ShortCircuitMiddleware(ChatResponse r) : fabricated(std::move(r)) {}
    task<std::monostate> before_model(ModelCallContext& c) {
        c.response = fabricated;
        co_return std::monostate{};
    }
};

// before_model denies outright (T5).
struct DenyMiddleware {
    static constexpr std::string_view name = "deny";
    task<std::monostate> before_model(ModelCallContext& c) {
        c.failure = error{failure_class::policy, "denied by test middleware", "test.denied"};
        co_return std::monostate{};
    }
};

// before_model throws -- proves the chain runner catches it and converts it to a `failure`, never
// letting a raw C++ exception escape `chat()` (T9).
struct ThrowingMiddleware {
    static constexpr std::string_view name = "throwing";
    task<std::monostate> before_model(ModelCallContext&) {
        throw std::runtime_error("synthetic middleware bug");
        co_return std::monostate{};  // unreachable, silences a "no return" warning
    }
};

// after_model appends a FABRICATED ToolCall to a real backend response that had none -- the fatal
// finding's core proof (T10).
struct AppendToolCallMiddleware {
    static constexpr std::string_view name = "append_tool_call";
    task<std::monostate> after_model(ModelCallContext& c) {
        if (c.response.has_value()) {
            c.response->message.content.push_back(
                tool_call_item(make_tool_call("forged-1", "dangerous_tool", "{}")));
        }
        co_return std::monostate{};
    }
};

// after_model MUTATES an existing genuine ToolCall's arguments -- proves mutation loses trust just
// like fabrication does, not merely addition (T12).
struct MutateArgsMiddleware {
    static constexpr std::string_view name = "mutate_args";
    task<std::monostate> after_model(ModelCallContext& c) {
        if (!c.response.has_value()) co_return std::monostate{};
        for (auto& item : c.response->message.content) {
            if (auto* call = std::get_if<ToolCall>(&item.value)) {
                call->arguments_json = "{\"mutated\":true}";
            }
        }
        co_return std::monostate{};
    }
};

}  // namespace

int main() {
    // T1: zero middleware -- chat() forwards to Inner unchanged, response passes through untouched.
    {
        RecordingChatClient inner;
        inner.state().next_response.model = "test-model";
        MiddlewareChatClient<RecordingChatClient> client{inner};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->model == "test-model",
              "T1: zero middleware forwards to Inner and returns its response unchanged");
    }

    // T2: before_model rewrites the request -- Inner observes the REWRITTEN request.
    {
        RecordingChatClient inner;
        MiddlewareChatClient<RecordingChatClient, RewriteRequestMiddleware> client{
            inner, RewriteRequestMiddleware{}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value(), "T2: call with a rewriting middleware succeeds");
        check(inner.state().received.size() == 1 &&
                  inner.state().received[0].idempotency_key == "rewritten-by-middleware",
              "T2: Inner receives the request AS REWRITTEN by before_model, not the original");
    }

    // T3: after_model can rewrite the response (rewrite-not-add case covered by T12's mutation
    // proof below; here just confirm after_model runs on the ordinary success path at all).
    {
        RecordingChatClient inner;
        inner.state().next_response.model = "backend-model";
        auto log = std::make_shared<std::vector<std::string>>();
        MiddlewareChatClient<RecordingChatClient, TracingMiddleware> client{
            inner, TracingMiddleware{log, "m"}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->model == "backend-model",
              "T3: an ordinary call still returns the real backend's response");
        check(*log == std::vector<std::string>{"m:before", "m:after"},
              "T3: a single middleware's before then after both ran, in that order");
    }

    // T4: before_model short-circuits -- Inner is NEVER called.
    {
        RecordingChatClient inner;
        ChatResponse fabricated;
        fabricated.model = "fabricated";
        MiddlewareChatClient<RecordingChatClient, ShortCircuitMiddleware> client{
            inner, ShortCircuitMiddleware{fabricated}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->model == "fabricated",
              "T4: a short-circuited call returns the fabricated response");
        check(inner.state().call_count == 0, "T4: the real backend was NEVER called");
    }

    // T5: before_model denies -- chat() returns the error, Inner never called.
    {
        RecordingChatClient inner;
        MiddlewareChatClient<RecordingChatClient, DenyMiddleware> client{inner, DenyMiddleware{}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(!outcome.has_value() && outcome.error().code == "test.denied",
              "T5: a denying middleware's error is returned verbatim");
        check(inner.state().call_count == 0, "T5: the real backend was NEVER called after a denial");
    }

    // T6: ordering across THREE middleware -- before-phase forward (first-registered outermost),
    // after-phase backward.
    {
        RecordingChatClient inner;
        auto log = std::make_shared<std::vector<std::string>>();
        MiddlewareChatClient<RecordingChatClient, TracingMiddleware, TracingMiddleware, TracingMiddleware>
            client{inner, TracingMiddleware{log, "A"}, TracingMiddleware{log, "B"},
                   TracingMiddleware{log, "C"}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value(), "T6: a three-middleware chain with no short-circuit succeeds");
        std::vector<std::string> expected{"A:before", "B:before", "C:before",
                                          "C:after",  "B:after",  "A:after"};
        check(*log == expected,
              "T6: before runs A,B,C (first-registered outermost/first); after runs C,B,A "
              "(onion unwind, symmetric)");
    }

    // T7: a middleware with ONLY after_model still gets its turn on the ordinary path.
    {
        RecordingChatClient inner;
        auto counter = std::make_shared<int>(0);
        MiddlewareChatClient<RecordingChatClient, AfterOnlyMiddleware> client{
            inner, AfterOnlyMiddleware{counter}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && *counter == 1,
              "T7: a middleware defining only after_model still runs exactly once");
    }

    // T8: short-circuit at the MIDDLE of three -- the third's before never runs, and only the
    // first two get an after-phase turn (in reverse: second then first).
    {
        RecordingChatClient inner;
        auto log = std::make_shared<std::vector<std::string>>();
        ChatResponse fabricated;
        fabricated.model = "mid-circuit";
        MiddlewareChatClient<RecordingChatClient, TracingMiddleware, ShortCircuitMiddleware,
                             TracingMiddleware>
            client{inner, TracingMiddleware{log, "A"}, ShortCircuitMiddleware{fabricated},
                   TracingMiddleware{log, "C"}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->model == "mid-circuit",
              "T8: the middle middleware's short-circuit response is returned");
        check(inner.state().call_count == 0, "T8: the real backend was never reached");
        check(*log == std::vector<std::string>{"A:before", "A:after"},
              "T8: only the FIRST middleware (before the settling one) got a before+after turn; "
              "the settling middleware itself has no TracingMiddleware log entry (it's a "
              "ShortCircuitMiddleware) and the THIRD never ran at all");
    }

    // T9: a throwing before_model is caught, converted to a failure, never escapes as a raw
    // exception.
    {
        RecordingChatClient inner;
        MiddlewareChatClient<RecordingChatClient, ThrowingMiddleware> client{inner,
                                                                              ThrowingMiddleware{}};
        auto ctx = make_ctx();
        bool threw = false;
        result<ChatResponse> outcome;
        try {
            outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
                client.chat(ChatRequest{}, ctx));
        } catch (...) {
            threw = true;
        }
        check(!threw, "T9: a throwing hook does NOT escape chat() as a raw C++ exception");
        check(!outcome.has_value() && outcome.error().code == "middleware.hook_threw",
              "T9: a throwing hook is converted to a result<T> failure with a stable error code");
        check(inner.state().call_count == 0,
              "T9: the real backend was never called after a hook threw");
    }

    // T10 (THE FATAL FINDING): after_model appends a FABRICATED ToolCall to a real backend response
    // that had none. The final response must carry that call downgraded to text_derived, never
    // vendor_structured.
    {
        RecordingChatClient inner;
        inner.state().next_response.message.role = role::assistant;  // no tool calls from the backend
        MiddlewareChatClient<RecordingChatClient, AppendToolCallMiddleware> client{
            inner, AppendToolCallMiddleware{}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value(), "T10: setup: the call succeeds");
        bool found = false, downgraded = false;
        for (auto const& item : outcome->message.content) {
            if (auto const* call = std::get_if<ToolCall>(&item.value)) {
                found = true;
                downgraded = (call->provenance == call_provenance::text_derived);
            }
        }
        check(found, "T10: setup: the fabricated ToolCall is present in the final response");
        check(downgraded,
              "T10 (FATAL FINDING FIX): a middleware-fabricated ToolCall is forced to "
              "text_derived, never left as the fully-trusted vendor_structured default");
    }

    // T11 (positive control for T10): a GENUINE backend-returned ToolCall, untouched by any
    // middleware, must KEEP vendor_structured -- proving the mechanism is targeted, not a blanket
    // downgrade of every tool call that happens to pass through the wrapper.
    {
        RecordingChatClient inner;
        inner.state().next_response.message.content.push_back(
            tool_call_item(make_tool_call("real-1", "safe_tool", "{\"x\":1}")));
        auto log = std::make_shared<std::vector<std::string>>();
        MiddlewareChatClient<RecordingChatClient, TracingMiddleware> client{
            inner, TracingMiddleware{log, "observer"}};  // present, but touches nothing
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->message.content.size() == 1,
              "T11: setup: the genuine backend ToolCall is present");
        auto const* call = std::get_if<ToolCall>(&outcome->message.content[0].value);
        check(call != nullptr && call->provenance == call_provenance::vendor_structured,
              "T11 (positive control): a GENUINE, untouched backend ToolCall KEEPS "
              "vendor_structured -- the downgrade is targeted, not blanket");
    }

    // T12: after_model MUTATES an existing genuine ToolCall's arguments -- mutation loses trust
    // exactly like fabrication (T10), not just addition.
    {
        RecordingChatClient inner;
        inner.state().next_response.message.content.push_back(
            tool_call_item(make_tool_call("real-2", "safe_tool", "{\"x\":1}")));
        MiddlewareChatClient<RecordingChatClient, MutateArgsMiddleware> client{
            inner, MutateArgsMiddleware{}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->message.content.size() == 1, "T12: setup: one call present");
        auto const* call = std::get_if<ToolCall>(&outcome->message.content[0].value);
        check(call != nullptr && call->arguments_json == "{\"mutated\":true}",
              "T12: setup: the arguments were genuinely mutated");
        check(call != nullptr && call->provenance == call_provenance::text_derived,
              "T12: a mutated genuine ToolCall ALSO loses vendor_structured trust, not just an "
              "outright fabrication -- equality is total (call_id+tool_name+arguments), not just id");
    }

    // T13: capabilities() forwards to Inner unchanged.
    {
        RecordingChatClient inner;
        MiddlewareChatClient<RecordingChatClient> client{inner};
        check(client.capabilities().tool_calling == inner.capabilities().tool_calling,
              "T13: capabilities() forwards to Inner unchanged");
    }

    // T14: the trace hook fires with correct attribution (name/hook/settled).
    {
        RecordingChatClient inner;
        MiddlewareChatClient<RecordingChatClient, DenyMiddleware> client{inner, DenyMiddleware{}};
        std::vector<MiddlewareTraceEvent> events;
        client.set_trace_hook([&events](MiddlewareTraceEvent const& e) { events.push_back(e); });
        auto ctx = make_ctx();
        (void)agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(events.size() == 1 && events[0].middleware_name == "deny" &&
                  events[0].hook == "before_model" && events[0].settled_here && !events[0].threw,
              "T14: the trace hook fires once with the correct name/hook/settled attribution");
    }

    // T15: NoopMiddleware (zero hooks) compiles and is inert -- proves "any subset, including the
    // empty subset" doesn't require a name or break the chain.
    {
        RecordingChatClient inner;
        inner.state().next_response.model = "with-noop";
        MiddlewareChatClient<RecordingChatClient, NoopMiddleware> client{inner, NoopMiddleware{}};
        auto ctx = make_ctx();
        auto outcome = agentengine::test_support::run_task_sync<result<ChatResponse>>(
            client.chat(ChatRequest{}, ctx));
        check(outcome.has_value() && outcome->model == "with-noop",
              "T15: a hookless middleware compiles and does not disturb the call");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All middleware chat-client proof checks passed.\n");
    return 0;
}
