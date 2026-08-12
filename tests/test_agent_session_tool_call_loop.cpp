// Proves `AgentSession::handle()`'s real, internal tool-call loop (agent_session.hpp) — see
// ADR-027-agent-session-tool-call-loop.md for the design/red-team record this test suite is the
// "prove" half of. Deterministic, offline (no live model, no network) — a scripted `ChatClientT`
// test double drives every scenario.
//
// Covers, one case per block in `main()`:
//   R1 — a multi-round tool conversation converges to a final Text answer within one `StartRun` ask.
//   R2 — the must-fix #1 regression: a `text_derived` `ToolCall` targeting a tool with
//        `approval_mode::never_require` but a non-inert capability ceiling is still DENIED (proving
//        the declassification gate governs, not the tool's own approval mode) — this is exactly the
//        case that silently passed through before `ToolCall::provenance` was threaded into
//        `ToolCallRequest::provenance` (tool_call_extraction.hpp).
//   R3 — a failing tool call does not abort the run; the error is fed back and the loop continues.
//   R4 — exhausting `max_turns_` without convergence fails the run closed (no response), never a hang.
//   R5 — a session with no granted capabilities (`capabilities_ == nullptr`) denies every tool call
//        per-call rather than crashing, and still terminates via R4's same fail-closed path.
//   R6 — `tool_call_started` observably precedes `tool_call_finished` for a given call_id in the
//        emitted `run_event` stream.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// ---- Test tools ------------------------------------------------------------------------------

struct EchoArgs { int value = 0; };
AE_JSON_SCHEMA(EchoArgs, value)
struct EchoReply { int value = 0; };
AE_JSON_SCHEMA(EchoReply, value)

// Empty capability ceiling, never_require approval (both defaults) -- always succeeds. Used for
// R1 (convergence), R3 (failure-doesn't-abort, alongside FailingTool), R4/R5 (never-converges).
struct EchoTool : ae::Tool<EchoTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its integer argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.value}; }
};

struct FailingArgs { bool unused = false; };  // AE_JSON_SCHEMA needs >=1 field; args are ignored
AE_JSON_SCHEMA(FailingArgs, unused)
struct FailingReply { bool unused = false; };
AE_JSON_SCHEMA(FailingReply, unused)

// Always fails at step 8 (invoke) -- R3 proves this doesn't abort the run.
struct FailingTool : ae::Tool<FailingTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "failing_tool";
    static constexpr std::string_view description = "Always fails.";
    using Args = FailingArgs;
    using Reply = FailingReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::contract, "scripted failure", "test.scripted_fail"});
    }
};

struct WriteArgs { bool unused = false; };  // AE_JSON_SCHEMA needs >=1 field; args are ignored
AE_JSON_SCHEMA(WriteArgs, unused)
struct WriteReply { bool unused = false; };
AE_JSON_SCHEMA(WriteReply, unused)

// `never_require` approval (undeclared, default) but a NON-inert capability ceiling (FsWrite,
// `is_inert_for_text_derived_declassification` returns false for fs_write, trust/capability.hpp) --
// exactly the shape ADR-023 §4b Finding 1's confused-deputy scenario needs: a tool a caller could
// wrongly treat as "no approval needed" the moment its call is reconstructed from model TEXT rather
// than a real vendor tool-call field.
[[nodiscard]] bool& write_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}
struct WriteTool : ae::Tool<WriteTool, ae::Capabilities<ae::cap::decl::FsWrite<"work">>,
                             ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "write_tool";
    static constexpr std::string_view description = "Writes to the work mount.";
    using Args = WriteArgs;
    using Reply = WriteReply;
    static ae::result<Reply> invoke(WriteArgs, ae::EffectContext&) {
        write_tool_invoked_log() = true;  // must never fire in R2 -- see that block's assertion
        return Reply{};
    }
};

// ---- ContextProvider fixture: history + the fixed three-tool declaration ----------------------

class ToolLoopHistoryProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<EchoTool, FailingTool, WriteTool>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<ToolLoopHistoryProvider>);

// ---- Scripted ChatClientT: a queue of pre-built responses, or "always emit a ToolCall" ---------

class ScriptedToolLoopChatClient {
public:
    std::vector<ae::Message> scripted_responses;  // consumed in order, one per chat() call
    bool always_tool_call = false;                // if set, ignore the script; never converges
    std::string always_tool_call_name = "echo_tool";
    std::size_t call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::Message reply;
        if (always_tool_call) {
            reply = make_tool_call_message("c-" + std::to_string(call_count), always_tool_call_name,
                                            R"({"value":1})");
        } else if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ++call_count;
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;  // generous enough that a small scripted response never blocks on credit
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::Message reply;
        if (always_tool_call) {
            reply = make_tool_call_message("c-" + std::to_string(call_count), always_tool_call_name,
                                            R"({"value":1})");
        } else if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ++call_count;
        ae::ChatResponseUpdate upd;
        if (!reply.content.empty()) upd.delta = reply.content.front();
        upd.is_final = true;
        upd.usage    = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }

    [[nodiscard]] static ae::Message make_text_message(std::string text) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-text";
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::Text{std::move(text)};
        m.content.push_back(std::move(item));
        return m;
    }

    [[nodiscard]] static ae::Message make_tool_call_message(
        std::string call_id, std::string tool_name, std::string args_json,
        ae::call_provenance provenance = ae::call_provenance::vendor_structured) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-" + call_id;
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                                   ae::content_origin::assistant, provenance};
        m.content.push_back(std::move(item));
        return m;
    }
};
static_assert(ae::ChatClient<ScriptedToolLoopChatClient>);

[[nodiscard]] ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] bool has_text(ae::Message const& m) {
    for (ae::ContentItem const& item : m.content) {
        if (std::holds_alternative<ae::Text>(item.value)) return true;
    }
    return false;
}

using Session = ae::AgentSession<ScriptedToolLoopChatClient, ae::NoSessionState, ToolLoopHistoryProvider>;

}  // namespace

int main() {
    // ---- R1: multi-round convergence ---------------------------------------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "echo_tool", R"({"value":1})"),
            ScriptedToolLoopChatClient::make_tool_call_message("c2", "echo_tool", R"({"value":2})"),
            // 3rd chat() call falls through to the script's exhaustion default: plain text.
        };
        kit.actor().initialize("s-r1", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "R1: one StartRun ask resolves the whole multi-round tool "
                                 "conversation internally");
        if (r.has_value()) {
            AE_CHECK(has_text(r->message), "R1: the converged response carries a final Text answer");
        }
        AE_CHECK(client.call_count == 3,
                 "R1: exactly 3 model calls happened (2 tool rounds + the converging round) within "
                 "the ONE StartRun ask");
    }

    // ---- R2: must-fix #1 regression -- text_derived overrides never_require -------------------
    {
        write_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "write_tool", R"({"unused":false})",
                                                                 ae::call_provenance::text_derived),
            // falls through to plain text next round once the denial is fed back
        };
        kit.actor().initialize("s-r2", ae::Principal{"p", ""});
        // Grant the capability WriteTool needs -- the denial under test must come from step 5
        // (approval), not from step 4 (missing capability), or this wouldn't isolate must-fix #1.
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root(
            {ae::Capability{ae::cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "R2: the run still converges (the denial is fed back as an error, "
                                 "not a run-level failure)");
        AE_CHECK(!write_tool_invoked_log(),
                 "R2: write_tool's invoke() was NEVER called -- a text_derived call against a "
                 "non-inert (FsWrite) capability ceiling is denied by the declassification gate "
                 "REGARDLESS of the tool's own never_require approval_mode. Without provenance "
                 "correctly threaded (ToolCallRequest::provenance defaulting to vendor_structured), "
                 "this call would have been silently approved and invoke() WOULD have run --- this "
                 "assertion is the regression test for that exact bug.");
    }

    // ---- R3: a failing tool call does not abort the run ----------------------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "failing_tool", "{}"),
            // falls through to plain text next round -- proving the loop continued past the failure
        };
        kit.actor().initialize("s-r3", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "R3: the run converges even though failing_tool's call errored -- "
                                 "the error was folded into the tool-results message and fed back, "
                                 "not treated as a run-level failure");
        AE_CHECK(client.call_count == 2,
                 "R3: a 2nd model call happened after the failed tool call -- the loop genuinely "
                 "continued, it didn't abort on the first error");
    }

    // ---- R4: max_turns exhausted fails the run closed, never a hang ---------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.always_tool_call = true;
        client.always_tool_call_name = "echo_tool";  // always succeeds -- never converges on its own
        kit.actor().initialize("s-r4", ae::Principal{"p", ""}, /*token_budget=*/std::nullopt,
                                /*max_turns=*/3);
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r.has_value(),
                 "R4: a chat client that never stops requesting tool calls fails the run closed once "
                 "max_turns is exhausted -- the ask never resolves with a response (never a hang, "
                 "the same 'failed by reply-before-teardown' shape every other fail-closed branch "
                 "in AgentSession::handle() already uses)");
        AE_CHECK(client.call_count == 3, "R4: exactly max_turns (3) model calls happened, not more");
    }

    // ---- R5: null capabilities_ denies every call per-call, still terminates -------------------
    {
        write_tool_invoked_log() = false;  // reset -- R2 above may have already set this to true

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.always_tool_call = true;
        client.always_tool_call_name = "write_tool";  // needs FsWrite -- denied at step 4 with no grant
        kit.actor().initialize("s-r5", ae::Principal{"p", ""}, /*token_budget=*/std::nullopt,
                                /*max_turns=*/3);
        // Deliberately no set_capabilities() call -- capabilities_ stays nullptr.

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r.has_value(),
                 "R5: with no granted capabilities at all, every write_tool call is denied per-call "
                 "(the empty-CapabilitySet fallback) rather than crashing, and the run still "
                 "terminates via the same max_turns-exceeded fail-closed path as R4");
        AE_CHECK(!write_tool_invoked_log(),
                 "R5: write_tool's invoke() was never reached -- denied at step 4 (capability), "
                 "before step 8");
    }

    // ---- R6: tool_call_started observably precedes tool_call_finished --------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedToolLoopChatClient::make_tool_call_message("c1", "echo_tool", R"({"value":1})"),
        };
        kit.actor().initialize("s-r6", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);

        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "R6: the scripted run converges");

        std::vector<ae::RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));

        std::size_t started_idx = events.size();
        std::size_t finished_idx = events.size();
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].kind == ae::run_event_kind::tool_call_started) started_idx = i;
            if (events[i].kind == ae::run_event_kind::tool_call_finished) finished_idx = i;
        }
        AE_CHECK(started_idx < events.size() && finished_idx < events.size(),
                 "R6: both tool_call_started and tool_call_finished fired");
        AE_CHECK(started_idx < finished_idx,
                 "R6: tool_call_started fires strictly before tool_call_finished for the same call "
                 "-- a live consumer sees an 'in progress' state, not two events that both land "
                 "after invoke_tool already returned");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_tool_call_loop: OK\n"
                                   : "test_agent_session_tool_call_loop: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
