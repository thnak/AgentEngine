// Prove phase for OQ-21 (OpenQuestions.md) / core/tool_call_hook.hpp -- the tool-call hook stage
// wired into a real agentengine::rt::AgentSession round (rt/agent_session.hpp's hook-stage block in
// run_rounds(), resolve_hook_decision(), finish_hook_processed_round()). Style/structure deliberately
// mirrors tests/test_rt_agent_session_suspend_approval.cpp (this project's own precedent for this
// class of test): deterministic, offline, a hand-scripted ChatClientT, check()/drive<T>() copied
// rather than shared (that file's own "no cross-test-file coupling" precedent).
//
//   H1 -- a denying hook stops the round BEFORE ApprovalDecider or the real tool's invoke() ever
//         runs: the denial is folded as an ordinary tool_results_message (no suspend -- a hook denial
//         is a decided outcome, not a pending question), the real tool body never executes, and an
//         ApprovalDecider tripwire wired into the SAME session is never even consulted.
//   H2 -- a rewriting hook's output actually reaches the real dispatched call (the tool's own
//         invoke() observes the REWRITTEN arguments, not the model's original ones), the rewrite
//         downgrades the call's provenance to call_provenance::text_derived
//         (enforce_hook_rewritten_tool_call_provenance -- observed indirectly, the same way
//         tests/test_tool_pipeline.cpp's own ADR-023 P2-T2 proves it: a capability-bearing tool that
//         ALSO declares Approval<never_require> would otherwise run with no decider at all, but here
//         it does NOT, proving the call's provenance flipped away from the tool's normal
//         vendor_structured/never_require fast path), and the call is refused with no approving
//         decider wired -- exactly ADR-023 P2-T2's own shape, now proven at this new hook call site.
//   H3 -- positive control / regression: with NO hook registered at all (tool_call_hook() default-
//         constructed, `set_tool_call_hook()` never called), behavior is byte-for-byte identical to
//         a session with no concept of a tool-call hook -- both the "never_require, no decider needed"
//         immediate-success path AND the full suspend-for-approval/resolve round trip, matching
//         test_rt_agent_session_suspend_approval.cpp's own SU1/SU3 assertions exactly.
//   H4 -- the interaction_reason::hook_decision suspend/resume path: a hook that sets
//         needs_external_dispatch suspends the round with the named sentinel
//         (Session::kSuspendedForHookDecision), opens a real Interaction tagged
//         interaction_reason::hook_decision (never a crash, never a silently-eaten exception), fires
//         hook_decision_requested naming the right call_id/interaction_id/tool_name, and
//         resolve_interaction() with hook_dispatch_answers correctly resumes and completes the call --
//         both the approve (real tool invoked, run converges) and deny (synthetic-style denial folded,
//         real tool never invoked, run still converges) outcomes.

#include <cstdio>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_hook.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
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

// Same "safe because nothing here genuinely suspends on an external wake" reasoning
// test_rt_agent_session_suspend_approval.cpp's own drive<T>() already documents.
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
using agentengine::EffectContext;
using agentengine::Interaction;
using agentengine::Message;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::ContentItem;
using agentengine::call_provenance;
using agentengine::interaction_reason;
using agentengine::RunEvent;
using agentengine::run_event_kind;
using agentengine::HookDispatchAnswer;
using agentengine::ToolCallHookContext;
using agentengine::error;

// -- H1: a plain, capability-free, never_require tool -- would run with no decider at all if the
// hook stage never touched it (the "no decider supplied, tool doesn't need one" shape) -----------

[[nodiscard]] bool& plain_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

struct PlainArgs { int value = 0; };
AE_JSON_SCHEMA(PlainArgs, value)
struct PlainReply { int value = 0; };
AE_JSON_SCHEMA(PlainReply, value)

struct PlainTool : agentengine::Tool<PlainTool, agentengine::Capabilities<>,
                                       agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "plain_tool";
    static constexpr std::string_view description = "A capability-free tool that never needs approval.";
    using Args = PlainArgs;
    using Reply = PlainReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        plain_tool_invoked_log() = true;
        return Reply{a.value};
    }
};

// -- H2: a capability-bearing tool that ALSO declares Approval<never_require> for its own
// vendor-structured calls -- mirrors test_tool_pipeline.cpp's own DangerousNeverRequireTool exactly,
// a fresh copy in this file per this project's own "no cross-test-file coupling" precedent. Records
// the exact account string its real invoke() observed, so H2 can prove a rewrite actually reached it.

[[nodiscard]] std::string& dangerous_tool_seen_account() {
    static std::string seen;
    return seen;
}
[[nodiscard]] int& dangerous_tool_invoked_count() {
    static int n = 0;
    return n;
}

struct DangerousArgs { std::string account; };
AE_JSON_SCHEMA(DangerousArgs, account)
struct DangerousReply { bool sent = false; };
AE_JSON_SCHEMA(DangerousReply, sent)

struct DangerousNeverRequireTool
    : agentengine::Tool<DangerousNeverRequireTool,
                          agentengine::Capabilities<agentengine::cap::decl::NetOut<"api.example.com">>,
                          agentengine::Approval<agentengine::approval_mode::never_require>> {
    static constexpr std::string_view name = "send_email";
    static constexpr std::string_view description = "Sends an email (real egress capability).";
    using Args = DangerousArgs;
    using Reply = DangerousReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        dangerous_tool_seen_account() = a.account;
        ++dangerous_tool_invoked_count();
        return Reply{true};
    }
};

// -- H3's suspend/approve regression fixture: byte-for-byte the same shape as
// test_rt_agent_session_suspend_approval.cpp's own GatedTool -- copied, not shared, matching that
// file's own precedent (empty ceiling, always_require, so it's a real approval decision under test).

[[nodiscard]] bool& gated_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

struct GateArgs { int value = 0; };
AE_JSON_SCHEMA(GateArgs, value)
struct GateReply { int value = 0; };
AE_JSON_SCHEMA(GateReply, value)

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<>,
                                       agentengine::EffectClass<agentengine::effect_class::pure>,
                                       agentengine::Approval<agentengine::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Needs approval before every call.";
    using Args = GateArgs;
    using Reply = GateReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        gated_tool_invoked_log() = true;
        return Reply{a.value};
    }
};

// -- H4c's regression fixture: decisions/ADR-070-host-configurable-responsibility-boundary.md's
// PolicyDecider seam, at the resolve_hook_decision() completion call site specifically -- the exact
// call site the implementing pass's own report says had a real bug (finish_hook_processed_round()'s
// invoke_tool() call silently dropping policy_decider_, so a PolicyDecider-auto_approved
// policy_driven call would be misclassified as needs_decider and spuriously denied). Byte-for-byte
// the same shape as test_rt_agent_session_suspend_approval.cpp's own SU7/SU8 PolicyGatedTool.

[[nodiscard]] bool& policy_gated_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

struct PolicyGateArgs { int value = 0; };
AE_JSON_SCHEMA(PolicyGateArgs, value)
struct PolicyGateReply { int value = 0; };
AE_JSON_SCHEMA(PolicyGateReply, value)

struct PolicyGatedTool : agentengine::Tool<PolicyGatedTool, agentengine::Capabilities<>,
                                             agentengine::EffectClass<agentengine::effect_class::pure>,
                                             agentengine::Approval<agentengine::approval_mode::policy_driven>> {
    static constexpr std::string_view name = "policy_gated_tool";
    static constexpr std::string_view description = "Needs policy_driven resolution before every call.";
    using Args = PolicyGateArgs;
    using Reply = PolicyGateReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        policy_gated_tool_invoked_log() = true;
        return Reply{a.value};
    }
};

// -- HistoryProviderT fixture: declares every tool this file's cases need ---------------------------

class HookHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<PlainTool, DangerousNeverRequireTool, GatedTool,
                                                        PolicyGatedTool>()
                      .descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<HookHistoryProvider>);

// -- The scripted backend, copied from test_rt_agent_session_suspend_approval.cpp's own conventions -

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
        return {};  // unused (stream_model_calls_ stays false throughout)
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

using Session = AgentSession<ScriptedChatClient, NoSessionState, HookHistoryProvider>;

}  // namespace

int main() {
    using agentengine::Principal;

    // ==============================================================================================
    // H1 -- a denying hook stops the round BEFORE ApprovalDecider or the real tool ever runs.
    // ==============================================================================================
    {
        plain_tool_invoked_log() = false;

        Session session;
        session.initialize("h1", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "plain_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("after-denial"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        bool decider_called = false;
        session.set_approval_decider(
            [&](Principal const&, std::string_view, std::string const&) {
                decider_called = true;  // must NEVER fire -- a hook denial is already-decided
                return true;
            });

        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "plain_tool") {
                hctx.denial = error{agentengine::failure_class::policy, "blocked by hook policy",
                                     "test.hook_blocked"};
            }
            co_return agentengine::result<std::monostate>{};
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "H1: a hook denial is a DECIDED outcome, not a pending question -- the round converges "
              "in one shot, it does not suspend");
        check(!plain_tool_invoked_log(),
              "H1: plain_tool's real invoke() was NEVER reached -- a hook-denied call never reaches "
              "invoke_tool() at all");
        check(!decider_called,
              "H1: the ApprovalDecider was NEVER consulted for this call -- the hook stage runs and "
              "decides strictly BEFORE the suspend-for-approval pre-check/ApprovalDecider seam");
        check(!session.has_open_interactions(),
              "H1: no Interaction ever opened -- a hook denial is not itself a suspend");

        if (outcome.has_value()) {
            std::size_t const denial_msg_index = session.history().size() - 2;  // tool_results, then text
            check(denial_msg_index < session.history().size(),
                  "H1: history has room for the folded denial message");
            Message const& folded = session.history()[denial_msg_index];
            check(folded.role == role::tool && folded.content.size() == 1,
                  "H1: exactly one ToolResult was folded for the single denied call");
            if (folded.role == role::tool && folded.content.size() == 1) {
                auto const* tr = std::get_if<ToolResult>(&folded.content.front().value);
                check(tr != nullptr && tr->is_error && tr->call_id == "c1",
                      "H1: the folded result is an error result for call_id c1");
                if (tr != nullptr && !tr->content.empty()) {
                    auto const* err = std::get_if<agentengine::Error>(&tr->content.front().value);
                    check(err != nullptr && err->message == "blocked by hook policy",
                          "H1: the denial carries the hook body's OWN message verbatim, not a generic "
                          "one");
                }
            }
        }
    }

    // ==============================================================================================
    // H2 -- a rewriting hook's output reaches the real dispatched call, downgrades provenance to
    // text_derived, and is refused without an approving decider (ADR-023 P2-T2's shape, at this seam).
    // ==============================================================================================

    // H2a: no ApprovalDecider at all -- the rewritten (now text_derived) call to a capability-bearing,
    // never_require tool is refused, and the real tool body never runs.
    {
        dangerous_tool_seen_account() = "";
        dangerous_tool_invoked_count() = 0;

        Session session;
        session.initialize("h2a", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "send_email", R"({"account":"attacker-controlled"})"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("after-refusal"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        session.set_capabilities(&held);
        // Deliberately no set_approval_decider() and no set_suspend_for_approval(true) -- exactly
        // ADR-023 P2-T2's own "no decider supplied" shape, proving invoke_tool()'s own step 5 (not a
        // suspend) is what refuses this call.

        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "send_email") {
                hctx.rewritten_arguments = *agentengine::json::parse(R"({"account":"rewritten-by-hook"})");
            }
            co_return agentengine::result<std::monostate>{};
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "H2a: the round converges (the refusal is an ordinary tool error folded into history, "
              "not a run-level failure or a suspend)");
        check(dangerous_tool_invoked_count() == 0,
              "H2a: ADR-023 P2-T2 (now via a hook rewrite): a rewritten call to a NetOut-capable tool "
              "is refused with no decider, EVEN THOUGH the tool itself declares "
              "Approval<never_require> -- the override holds at this seam too");
        check(!session.has_open_interactions(), "H2a: no suspend -- invoke_tool() itself denied it");
    }

    // H2b: an ApprovalDecider IS wired and denies -- proves it's genuinely consulted with the
    // REWRITTEN canonical arguments (not the model's original ones), and a "no" still blocks the call.
    {
        dangerous_tool_seen_account() = "";
        dangerous_tool_invoked_count() = 0;

        Session session;
        session.initialize("h2b", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "send_email", R"({"account":"attacker-controlled"})"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("after-deny"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        session.set_capabilities(&held);

        std::string seen_args;
        session.set_approval_decider(
            [&](Principal const&, std::string_view tool_name, std::string const& args_json) {
                check(tool_name == "send_email", "H2b: decider sees the correct tool name");
                seen_args = args_json;
                return false;
            });
        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "send_email") {
                hctx.rewritten_arguments = *agentengine::json::parse(R"({"account":"rewritten-by-hook"})");
            }
            co_return agentengine::result<std::monostate>{};
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "H2b: the round converges -- decider's 'no' is an ordinary denial");
        check(seen_args.find("rewritten-by-hook") != std::string::npos,
              "H2b: the ApprovalDecider observed the HOOK-REWRITTEN arguments, not the model's "
              "original ones -- the rewrite genuinely reached the real dispatched call's approval "
              "step");
        check(dangerous_tool_invoked_count() == 0,
              "H2b: the real tool body was never invoked -- an explicit 'no' still blocks the call");
    }

    // H2c: an ApprovalDecider approves -- the real tool body runs and observes the REWRITTEN args.
    {
        dangerous_tool_seen_account() = "";
        dangerous_tool_invoked_count() = 0;

        Session session;
        session.initialize("h2c", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "send_email", R"({"account":"attacker-controlled"})"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("after-approve"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        session.set_capabilities(&held);
        session.set_approval_decider(
            [](Principal const&, std::string_view, std::string const&) { return true; });
        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "send_email") {
                hctx.rewritten_arguments = *agentengine::json::parse(R"({"account":"rewritten-by-hook"})");
            }
            co_return agentengine::result<std::monostate>{};
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "H2c: an EXPLICIT human/policy approval still lets the rewritten call to a "
              "capability-bearing tool through -- this is a gate, not a permanent block");
        check(dangerous_tool_invoked_count() == 1,
              "H2c: the real tool body WAS invoked exactly once -- the hook's rewrite reached the "
              "real dispatched call, this is not a dry run");
        check(dangerous_tool_seen_account() == "rewritten-by-hook",
              "H2c: the real tool observed the HOOK-REWRITTEN argument value ('rewritten-by-hook'), "
              "never the model's original ('attacker-controlled') -- the rewrite is what actually "
              "dispatched");
    }

    // ==============================================================================================
    // H3 -- positive control: with NO hook registered, behavior is unchanged from before this
    // feature existed.
    // ==============================================================================================

    // H3a: never_require + vendor_structured (the model's normal call shape) succeeds with no
    // decider at all -- same as test_tool_pipeline.cpp's own "no decider needed" case, now proven end
    // to end through a real AgentSession round with tool_call_hook_ left completely unset.
    {
        dangerous_tool_seen_account() = "";
        dangerous_tool_invoked_count() = 0;

        Session session;
        session.initialize("h3a", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "send_email", R"({"account":"legit-user"})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        session.set_capabilities(&held);
        // No set_tool_call_hook() call at all -- session.tool_call_hook() stays default-constructed.

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "H3a: with no hook, a never_require capability-bearing tool still runs with no decider "
              "-- identical to this project's pre-hook-feature behavior");
        check(dangerous_tool_invoked_count() == 1 && dangerous_tool_seen_account() == "legit-user",
              "H3a: the real tool ran exactly once, observing the model's OWN unmodified arguments -- "
              "nothing rewrote them, because no hook is wired");
    }

    // H3b: the full suspend-for-approval/resolve round trip, no hook wired -- byte-for-byte
    // test_rt_agent_session_suspend_approval.cpp's own SU1+SU3 assertions, re-run here to prove the
    // hook-stage code added to run_rounds()/resolve_interaction() is a true no-op on this path
    // (every `hook_touched_round`-gated branch it added takes the exact original path).
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("h3b", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        // No set_tool_call_hook() call at all.

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "H3b: start_run() fails while suspended for approval, same as SU1");
        check(r1.has_value() || r1.error().code == Session::kSuspendedForApproval,
              "H3b: the error code is kSuspendedForApproval, same sentinel as before this feature");
        check(!gated_tool_invoked_log(), "H3b: gated_tool's invoke() was never reached before suspend");
        check(session.has_open_interactions() &&
                  session.open_interactions().size() == 1 &&
                  session.open_interactions().front().reason == interaction_reason::approval,
              "H3b: the open Interaction is tagged interaction_reason::approval, exactly as SU1 -- "
              "hook_decision never appears when no hook is wired");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        bool saw_hook_decision_requested = false;
        for (RunEvent const& ev : events) {
            if (ev.kind == run_event_kind::hook_decision_requested) saw_hook_decision_requested = true;
        }
        check(!saw_hook_decision_requested,
              "H3b: hook_decision_requested never fires when no hook is wired");

        std::string const interaction_id = session.open_interactions().front().interaction_id;
        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt}));
        check(r2.has_value(), "H3b: resolving approved=true lets the run converge, same as SU3");
        check(gated_tool_invoked_log(), "H3b: gated_tool's invoke() WAS called after approval");
        check(!session.has_open_interactions(), "H3b: the interaction closed once resolved");
        check(client.call_count() == 2, "H3b: the ChatClientT was called twice, same as SU3");
    }

    // ==============================================================================================
    // H4 -- the interaction_reason::hook_decision suspend/resume path.
    // ==============================================================================================

    // H4a: dispatch approves -- the round resumes and completes the real call.
    {
        plain_tool_invoked_log() = false;

        Session session;
        session.initialize("h4a", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "plain_tool", R"({"value":42})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done-after-dispatch"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "plain_tool") hctx.needs_external_dispatch = true;
            co_return agentengine::result<std::monostate>{};
        });

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(),
              "H4a: start_run() does NOT crash or throw -- it completes with an error result naming "
              "the suspend, exactly like every other suspend path in this codebase");
        check(r1.has_value() || r1.error().code == Session::kSuspendedForHookDecision,
              "H4a: the error code is the named kSuspendedForHookDecision sentinel");
        check(!plain_tool_invoked_log(),
              "H4a: plain_tool's real invoke() was never reached before the dispatch answer arrives");
        check(session.has_open_interactions() &&
                  session.open_interactions().size() == 1 &&
                  session.open_interactions().front().reason == interaction_reason::hook_decision,
              "H4a: a REAL Interaction opened, tagged interaction_reason::hook_decision");
        std::string const interaction_id = session.open_interactions().front().interaction_id;
        check(session.open_interactions().front().run_id == session.last_run_id(),
              "H4a: the Interaction names the exact run_id that suspended");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        bool saw_hook_event_with_right_fields = false;
        for (RunEvent const& ev : events) {
            if (ev.kind != run_event_kind::hook_decision_requested) continue;
            auto const* p = std::get_if<agentengine::run_event_payload::HookDecisionRequested>(&ev.payload);
            if (p != nullptr && p->call_id == "c1" && p->interaction_id == interaction_id &&
                p->tool_name == "plain_tool") {
                saw_hook_event_with_right_fields = true;
            }
        }
        check(saw_hook_event_with_right_fields,
              "H4a: a hook_decision_requested event fired naming the right call_id/interaction_id/"
              "tool_name");

        auto r2 = drive(session.resolve_interaction(ResolveInteraction{
            interaction_id, /*approved=*/false, std::nullopt, std::nullopt, std::nullopt,
            std::vector<HookDispatchAnswer>{HookDispatchAnswer{"c1", /*approved=*/true, std::nullopt,
                                                                  std::nullopt}}}));
        check(r2.has_value(),
              "H4a: resolve_interaction() with a matching hook_dispatch_answers correctly resumes and "
              "completes the round");
        check(plain_tool_invoked_log(),
              "H4a: the real tool WAS invoked after the external dispatch approved it");
        check(!session.has_open_interactions(), "H4a: the interaction closed once resolved");
        check(client.call_count() == 2, "H4a: the ChatClientT was called twice -- suspend then resume");
    }

    // H4b: dispatch denies -- the round resumes, folds a denial, never invokes the real tool, and
    // still converges.
    {
        plain_tool_invoked_log() = false;

        Session session;
        session.initialize("h4b", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "plain_tool", R"({"value":9})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done-after-external-denial"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "plain_tool") hctx.needs_external_dispatch = true;
            co_return agentengine::result<std::monostate>{};
        });

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "H4b setup: the round suspends for a hook decision");
        std::string const interaction_id = session.open_interactions().front().interaction_id;

        auto r2 = drive(session.resolve_interaction(ResolveInteraction{
            interaction_id, /*approved=*/false, std::nullopt, std::nullopt, std::nullopt,
            std::vector<HookDispatchAnswer>{
                HookDispatchAnswer{"c1", /*approved=*/false, std::nullopt,
                                    std::string("denied by the external process")}}}));
        check(r2.has_value(),
              "H4b: resolving with an external denial still lets the run converge -- an ordinary "
              "folded tool error, not a run-level failure");
        check(!plain_tool_invoked_log(),
              "H4b: the real tool was NEVER invoked -- an external dispatch denial never reaches it");
        check(!session.has_open_interactions(), "H4b: the interaction closed once resolved");
    }

    // H4c (regression, not one of the four required proofs but guards a real bug the implementing
    // pass's own report names): a hook-decision resume that completes into a policy_driven call must
    // still consult policy_decider_ -- an auto_approve verdict must let the call actually run, never
    // get spuriously denied for lack of an ApprovalDecider that was never supposed to be needed.
    {
        policy_gated_tool_invoked_log() = false;

        Session session;
        session.initialize("h4c", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "policy_gated_tool", R"({"value":5})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done-after-policy"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        // Deliberately no set_approval_decider() -- if policy_decider_ is NOT threaded through
        // finish_hook_processed_round()'s invoke_tool() call, this call is misclassified as
        // needs_decider and denied with no decider present. That IS the bug this test guards.
        session.set_policy_decider(
            [](Principal const&, agentengine::ToolDescriptor const&, bool) {
                return agentengine::policy_decision::auto_approve;
            });
        session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<agentengine::result<std::monostate>> {
            if (hctx.tool_name == "policy_gated_tool") hctx.needs_external_dispatch = true;
            co_return agentengine::result<std::monostate>{};
        });

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "H4c setup: the round suspends for a hook decision");
        std::string const interaction_id = session.open_interactions().front().interaction_id;

        auto r2 = drive(session.resolve_interaction(ResolveInteraction{
            interaction_id, /*approved=*/false, std::nullopt, std::nullopt, std::nullopt,
            std::vector<HookDispatchAnswer>{HookDispatchAnswer{"c1", /*approved=*/true, std::nullopt,
                                                                  std::nullopt}}}));
        check(r2.has_value(),
              "H4c: the round converges after the hook-decision resume folds a policy_driven call "
              "back in");
        check(policy_gated_tool_invoked_log(),
              "H4c: the PolicyDecider's auto_approve verdict WAS honored on the hook-decision resume "
              "path -- policy_decider_ reached invoke_tool() from finish_hook_processed_round(), the "
              "call was not spuriously denied for lack of an ApprovalDecider that was never needed");
        check(!session.has_open_interactions(), "H4c: the interaction closed once resolved");
        check(client.call_count() == 2, "H4c: the ChatClientT was called twice -- suspend then resume");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_tool_call_hook: ALL PASS\n");
    return 0;
}
