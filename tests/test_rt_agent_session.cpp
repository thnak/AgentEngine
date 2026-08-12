// Proof for ADR-037 Phase 2, Slice 1: agentengine::rt::AgentSession (include/agentengine/rt/
// agent_session.hpp), the Quark-actor-free replacement for agentengine::AgentSession -- driven
// through agentengine::rt::task<T>/agentengine::rt::AsyncMutex instead of quark::Actor<Sequential>/
// quark::Ask<>. Deterministic, offline, no live model, no network. Covers:
//   S1 -- a no-tool-call round converges: start_run() returns the model's own response directly.
//   S2 -- a tool-call round runs the round loop's tool-invocation step (this test's minimal
//         HistoryProviderT declares no tools, so tool_pipeline.hpp's own already-proven-elsewhere
//         fail-closed-unknown-tool path fires -- what's under test here is that AgentSession folds
//         SOME result and continues, not tool_pipeline.hpp's own correctness) and feeds the result
//         back; the SECOND round converges -- proving the round loop itself (not just a single call)
//         works.
//   S3 -- admission: a caller identity that does not match the session's own principal is denied,
//         WITHOUT ever reaching the ChatClientT (admission_denied_count() observes it).
//   S4 -- token budget: a run whose accumulated usage exceeds the configured ceiling fails closed
//         with run.token_budget_exceeded, without ever completing.
//   S5 -- max_turns: a tool call that never stops being requested fails closed with
//         run.max_turns_exceeded once the configured bound is reached.
//
// NOT covered by this file (named, not silently skipped): a genuine cross-thread I1 proof (two
// concurrent start_run() calls into the SAME session instance actually contending on
// AsyncMutex/session_mutex_) -- every ChatClientT fixture below is fully synchronous under the hood
// (chat() never suspends on anything external), so nothing in this file exercises AsyncMutex's own
// contended path; that primitive already has its own dedicated, real cross-thread proof
// (test_rt_async_mutex.cpp's M3). A future slice's own test, once a genuinely async ChatClientT
// fixture exists for this file to use, is where session-level concurrent-call serialization would be
// proven end to end -- not claimed here.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::task;

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

// Drives an agentengine::rt::task<T> to completion. Slice 1's ChatClientT fixtures below never
// genuinely suspend on anything external (their own chat()/chat_stream() bodies co_return
// immediately) -- so a plain "resume until done" loop is safe here, unlike test_rt_async_mutex.cpp's
// own driving-pattern warning (which is specifically about coroutines that suspend on an EXTERNAL
// wake, which nothing in this test does). See that file's own comment for the fuller explanation of
// why the distinction matters.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ToolCall;
using agentengine::ContentItem;
using agentengine::call_provenance;

// -- The scripted backend --------------------------------------------------------------------------
// Each chat() call consumes the NEXT scripted outcome, in order (repeats the last once exhausted).
// State lives behind a shared_ptr since AgentSession takes ChatClientT by value (via emplace_chat_
// client's forwarding constructor) and this fixture is also constructed directly in some tests.
struct ScriptedOutcome {
    Message message;
    Usage usage;
};

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused by these tests (stream_model_calls_ stays false throughout)
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall call;
    call.call_id = std::move(call_id);
    call.tool_name = std::move(tool_name);
    call.arguments_json = std::move(args);
    call.provenance = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;

    // S1: a no-tool-call round converges immediately.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("s1", Principal{"p1", ""});
        session.emplace_chat_client().set_script({{text_response("hello"), Usage{2, 3, 0, 0, 0.0}}});

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "S1: a no-tool-call round converges and returns a real response");
        if (outcome.has_value()) {
            auto const* t = std::get_if<Text>(&outcome->message.content.front().value);
            check(t != nullptr && t->text == "hello", "S1: the response carries the scripted text");
            check(outcome->usage.input_tokens == 2 && outcome->usage.output_tokens == 3,
                  "S1: the response carries the scripted usage");
        }
        check(session.run_tokens_consumed() == 5, "S1: run_tokens_consumed() reflects the one call");
        check(session.history().size() == 2, "S1: history holds the input + the response, nothing else");
    }

    // S2: a tool-call round invokes the tool for real, then a SECOND round converges.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("s2", Principal{"p2", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("call-1", "get_weather", "{}"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("it's sunny"), Usage{1, 1, 0, 0, 0.0}},
        });

        auto outcome = drive(session.start_run(StartRun{user_message("what's the weather")}));
        check(outcome.has_value(), "S2: the round loop converges after a tool-call round");
        if (outcome.has_value()) {
            auto const* t = std::get_if<Text>(&outcome->message.content.front().value);
            check(t != nullptr && t->text == "it's sunny",
                  "S2: the SECOND round's response is what start_run() ultimately returns");
        }
        check(client.call_count() == 2,
              "S2: the ChatClientT was genuinely called twice -- once per round, not memoized/skipped");
        check(session.history().size() == 4,
              "S2: history holds input + tool-call response + tool-results + final response");
    }

    // S3: admission denies a caller identity that does not match the session's own principal,
    // WITHOUT ever reaching the ChatClientT.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("s3", Principal{"owner", "tenant-a"});
        session.emplace_chat_client().set_script({{text_response("should never be seen"), Usage{1, 1, 0, 0, 0.0}}});

        StartRun req{user_message("hi")};
        req.caller = agentengine::rt::SessionCaller{"someone-else", "tenant-a"};
        auto outcome = drive(session.start_run(req));
        check(!outcome.has_value(), "S3: a mismatched caller is denied");
        check(outcome.has_value() || outcome.error().code == "run.admission_denied",
              "S3: the denial is specifically an admission denial, not some other failure");
        check(session.admission_denied_count() == 1, "S3: admission_denied_count() observes the denial");
        check(session.history().empty(), "S3: nothing was mutated -- history stays empty");
    }

    // S4: token budget -- a run whose usage exceeds the configured ceiling fails closed.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("s4", Principal{"p4", ""}, /*token_budget=*/10);
        session.emplace_chat_client().set_script({{text_response("expensive"), Usage{8, 8, 0, 0, 0.0}}});

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(!outcome.has_value(), "S4: exceeding the token budget fails the run");
        check(outcome.has_value() || outcome.error().code == "run.token_budget_exceeded",
              "S4: the failure is specifically a token-budget failure");
    }

    // S5: max_turns -- a tool call that never stops being requested fails closed once the bound is
    // reached, rather than looping forever.
    {
        AgentSession<ScriptedChatClient> session;
        session.initialize("s5", Principal{"p5", ""}, std::nullopt, /*max_turns=*/3);
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "get_weather", "{}"), Usage{1, 1, 0, 0, 0.0}},
        });  // repeats forever -- always a tool call, never converges

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(!outcome.has_value(), "S5: a non-converging round loop fails closed, does not hang forever");
        check(outcome.has_value() || outcome.error().code == "run.max_turns_exceeded",
              "S5: the failure is specifically a max_turns failure");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session: ALL PASS\n");
    return 0;
}
