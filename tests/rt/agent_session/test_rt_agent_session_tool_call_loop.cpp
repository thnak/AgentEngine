// Proof for ADR-037 Phase 2: porting the real behavioral claims from the OLD, Quark-actor-based
// test_agent_session_tool_call_loop.cpp (ADR-027's internal tool-call loop, agent_session.hpp's
// handle()/run_rounds()) onto agentengine::rt::AgentSession (include/agentengine/rt/agent_session.hpp)
// directly. Confirmed before porting: no test_rt_agent_session_tool_call_loop.cpp existed yet (this
// IS that file). Deterministic, offline (no live model, no network) -- a scripted ChatClientT test
// double drives every scenario, same as the old file.
//
// Covers, one case per block in main() -- same R-numbering as the old file, for traceability:
//   R1 -- a multi-round tool conversation converges to a final Text answer within one start_run() call.
//   R2 -- the must-fix #1 regression: a text_derived ToolCall targeting a tool with
//         approval_mode::never_require but a non-inert capability ceiling is still DENIED (proving
//         the declassification gate governs, not the tool's own approval mode) -- exactly the case
//         that silently passed through before ToolCall::provenance was threaded into
//         ToolCallRequest::provenance (tool_call_extraction.hpp).
//   R3 -- a failing tool call does not abort the run; the error is fed back and the loop continues.
//   R4 -- exhausting max_turns_ without convergence fails the run closed (no response), never a hang.
//   R5 -- a session with no granted capabilities (capabilities_ == nullptr) denies every tool call
//         per-call rather than crashing, and still terminates via R4's same fail-closed path.
//   R6 -- tool_call_started observably precedes tool_call_finished for a given call_id in the emitted
//         run_event stream.

#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- every ChatClientT fixture below co_returns immediately.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::CapabilitySet;
using agentengine::Capability;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::RunEvent;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;
using agentengine::run_event_kind;

// ---- Test tools --------------------------------------------------------------------------------

struct EchoArgs { int value = 0; };
AE_JSON_SCHEMA(EchoArgs, value)
struct EchoReply { int value = 0; };
AE_JSON_SCHEMA(EchoReply, value)

// Empty capability ceiling, never_require approval (both defaults) -- always succeeds. Used for R1
// (convergence), R3 (failure-doesn't-abort, alongside FailingTool), R4/R5 (never-converges).
struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<>,
                                     agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its integer argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) { return Reply{a.value}; }
};

struct FailingArgs { bool unused = false; };  // AE_JSON_SCHEMA needs >=1 field; args are ignored
AE_JSON_SCHEMA(FailingArgs, unused)
struct FailingReply { bool unused = false; };
AE_JSON_SCHEMA(FailingReply, unused)

// Always fails at step 8 (invoke) -- R3 proves this doesn't abort the run.
struct FailingTool : agentengine::Tool<FailingTool, agentengine::Capabilities<>,
                                        agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "failing_tool";
    static constexpr std::string_view description = "Always fails.";
    using Args = FailingArgs;
    using Reply = FailingReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::contract, "scripted failure", "test.scripted_fail"});
    }
};

struct WriteArgs { bool unused = false; };  // AE_JSON_SCHEMA needs >=1 field; args are ignored
AE_JSON_SCHEMA(WriteArgs, unused)
struct WriteReply { bool unused = false; };
AE_JSON_SCHEMA(WriteReply, unused)

// never_require approval (undeclared, default) but a NON-inert capability ceiling (FsWrite,
// is_inert_for_text_derived_declassification returns false for fs_write, trust/capability.hpp) --
// exactly the shape ADR-023 §4b Finding 1's confused-deputy scenario needs: a tool a caller could
// wrongly treat as "no approval needed" the moment its call is reconstructed from model TEXT rather
// than a real vendor tool-call field.
[[nodiscard]] bool& write_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}
struct WriteTool : agentengine::Tool<WriteTool, agentengine::Capabilities<agentengine::cap::decl::FsWrite<"work">>,
                                      agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "write_tool";
    static constexpr std::string_view description = "Writes to the work mount.";
    using Args = WriteArgs;
    using Reply = WriteReply;
    static agentengine::result<Reply> invoke(WriteArgs, EffectContext&) {
        write_tool_invoked_log() = true;  // must never fire in R2 -- see that block's assertion
        return Reply{};
    }
};

// ---- HistoryProviderT fixture: history passthrough + the fixed three-tool declaration -----------

class ToolLoopHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc,
                                                                              EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<EchoTool, FailingTool, WriteTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<ToolLoopHistoryProvider>);

// ---- Scripted ChatClientT: a queue of pre-built responses, or "always emit a ToolCall" ----------
// State lives behind a shared_ptr since AgentSession takes ChatClientT by value (via emplace_chat_
// client's forwarding constructor) -- same convention every other rt:: fixture in this test suite
// already uses.

class ScriptedToolLoopChatClient {
public:
    ScriptedToolLoopChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<Message> scripted_responses;  // consumed in order, one per chat() call
        bool always_tool_call = false;             // if set, ignore the script; never converges
        std::string always_tool_call_name = "echo_tool";
        std::size_t call_count = 0;
    };

    void set_scripted_responses(std::vector<Message> responses) {
        state_->scripted_responses = std::move(responses);
    }
    void set_always_tool_call(std::string tool_name) {
        state_->always_tool_call = true;
        state_->always_tool_call_name = std::move(tool_name);
    }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message reply = next_reply();
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout this file
    }

    [[nodiscard]] static Message make_text_message(std::string text) {
        Message m;
        m.role = role::assistant;
        m.message_id = "m-text";
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = Text{std::move(text)};
        m.content.push_back(std::move(item));
        return m;
    }

    [[nodiscard]] static Message make_tool_call_message(
        std::string call_id, std::string tool_name, std::string args_json,
        call_provenance provenance = call_provenance::vendor_structured) {
        Message m;
        m.role = role::assistant;
        m.message_id = "m-" + call_id;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                               content_origin::assistant, provenance};
        m.content.push_back(std::move(item));
        return m;
    }

private:
    Message next_reply() {
        Message reply;
        if (state_->always_tool_call) {
            reply = make_tool_call_message("c-" + std::to_string(state_->call_count),
                                            state_->always_tool_call_name, R"({"value":1})");
        } else if (state_->call_count < state_->scripted_responses.size()) {
            reply = state_->scripted_responses[state_->call_count];
        } else {
            reply = make_text_message("done");
        }
        ++state_->call_count;
        return reply;
    }

    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedToolLoopChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] bool has_text(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Text>(item.value)) return true;
    }
    return false;
}

using Session = AgentSession<ScriptedToolLoopChatClient, NoSessionState, ToolLoopHistoryProvider>;

}  // namespace

int main() {
    // ---- R1: multi-round convergence -------------------------------------------------------------
    {
        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_scripted_responses({
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "echo_tool", R"({"value":1})"),
            ScriptedToolLoopChatClient::make_tool_call_message("c2", "echo_tool", R"({"value":2})"),
            // 3rd chat() call falls through to the script's exhaustion default: plain text.
        });
        session.initialize("s-r1", Principal{"p", ""});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "R1: one start_run() call resolves the whole multi-round tool "
                              "conversation internally");
        if (r.has_value()) {
            check(has_text(r->message), "R1: the converged response carries a final Text answer");
        }
        check(client.call_count() == 3,
              "R1: exactly 3 model calls happened (2 tool rounds + the converging round) within the "
              "ONE start_run() call");
    }

    // ---- R2: must-fix #1 regression -- text_derived overrides never_require ----------------------
    {
        write_tool_invoked_log() = false;

        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_scripted_responses({
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "write_tool", R"({"unused":false})",
                                                                 call_provenance::text_derived),
            // falls through to plain text next round once the denial is fed back
        });
        session.initialize("s-r2", Principal{"p", ""});
        // Grant the capability WriteTool needs -- the denial under test must come from step 5
        // (approval), not from step 4 (missing capability), or this wouldn't isolate must-fix #1.
        CapabilitySet const held = CapabilitySet::grant_root(
            {Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "R2: the run still converges (the denial is fed back as an error, not a "
                              "run-level failure)");
        check(!write_tool_invoked_log(),
              "R2: write_tool's invoke() was NEVER called -- a text_derived call against a non-inert "
              "(FsWrite) capability ceiling is denied by the declassification gate REGARDLESS of the "
              "tool's own never_require approval_mode. Without provenance correctly threaded "
              "(ToolCallRequest::provenance defaulting to vendor_structured), this call would have "
              "been silently approved and invoke() WOULD have run -- this assertion is the "
              "regression test for that exact bug.");
    }

    // ---- R3: a failing tool call does not abort the run --------------------------------------------
    {
        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_scripted_responses({
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "failing_tool", "{}"),
            // falls through to plain text next round -- proving the loop continued past the failure
        });
        session.initialize("s-r3", Principal{"p", ""});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "R3: the run converges even though failing_tool's call errored -- the "
                              "error was folded into the tool-results message and fed back, not "
                              "treated as a run-level failure");
        check(client.call_count() == 2,
              "R3: a 2nd model call happened after the failed tool call -- the loop genuinely "
              "continued, it didn't abort on the first error");
    }

    // ---- R4: max_turns exhausted fails the run closed, never a hang -------------------------------
    {
        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_always_tool_call("echo_tool");  // always succeeds -- never converges on its own
        session.initialize("s-r4", Principal{"p", ""}, /*token_budget=*/std::nullopt,
                            /*max_turns=*/3);
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(!r.has_value(),
              "R4: a chat client that never stops requesting tool calls fails the run closed once "
              "max_turns is exhausted -- start_run() never returns a real AgentResponse (never a "
              "hang, the same fail-closed shape every other branch in run_rounds() already uses)");
        check(!r.has_value() && r.error().code == "run.max_turns_exceeded",
              "R4: the failure is specifically run.max_turns_exceeded");
        check(client.call_count() == 3, "R4: exactly max_turns (3) model calls happened, not more");
    }

    // ---- R5: null capabilities_ denies every call per-call, still terminates ----------------------
    {
        write_tool_invoked_log() = false;  // reset -- R2 above may have already set this to true

        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_always_tool_call("write_tool");  // needs FsWrite -- denied at step 4 with no grant
        session.initialize("s-r5", Principal{"p", ""}, /*token_budget=*/std::nullopt,
                            /*max_turns=*/3);
        // Deliberately no set_capabilities() call -- capabilities_ stays nullptr.

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(!r.has_value(),
              "R5: with no granted capabilities at all, every write_tool call is denied per-call (the "
              "empty-CapabilitySet fallback) rather than crashing, and the run still terminates via "
              "the same max_turns-exceeded fail-closed path as R4");
        check(!r.has_value() && r.error().code == "run.max_turns_exceeded",
              "R5: the failure is still specifically run.max_turns_exceeded -- capability denial "
              "alone does not abort the round loop early, it just prevents convergence");
        check(!write_tool_invoked_log(),
              "R5: write_tool's invoke() was never reached -- denied at step 4 (capability), before "
              "step 8");
        check(client.call_count() == 3, "R5: exactly max_turns (3) model calls happened");
    }

    // ---- R6: tool_call_started observably precedes tool_call_finished -----------------------------
    {
        Session session;
        ScriptedToolLoopChatClient& client = session.emplace_chat_client();
        client.set_scripted_responses({
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "echo_tool", R"({"value":1})"),
        });
        session.initialize("s-r6", Principal{"p", ""});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "R6: the scripted run converges");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));

        std::size_t started_idx = events.size();
        std::size_t finished_idx = events.size();
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].kind == run_event_kind::tool_call_started) started_idx = i;
            if (events[i].kind == run_event_kind::tool_call_finished) finished_idx = i;
        }
        check(started_idx < events.size() && finished_idx < events.size(),
              "R6: both tool_call_started and tool_call_finished fired");
        check(started_idx < finished_idx,
              "R6: tool_call_started fires strictly before tool_call_finished for the same call -- a "
              "live consumer sees an 'in progress' state, not two events that both land after "
              "invoke_tool already returned");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_tool_call_loop: ALL PASS\n");
    return 0;
}
