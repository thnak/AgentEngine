// Proof for ADR-037 Phase 2, Slice 1: ADR-029's suspend-for-human-approval mechanism, ported to
// agentengine::rt::AgentSession (include/agentengine/rt/agent_session.hpp) -- the Quark-actor-free
// replacement for agentengine::AgentSession (core/agent_session.hpp). Same deterministic, offline,
// scripted-ChatClientT shape as test_rt_agent_session.cpp (S1-S5's own conventions, copied here
// rather than shared, matching that file's own "no cross-test-file coupling" precedent), and the
// same SU1-SU6 claims the OLD, Quark-actor-based test_agent_session_suspend_approval.cpp proved --
// re-expressed for `rt::AgentSession`'s own shape, which has NO Quark::Ask "never resolves" concept:
// every `task<result<T>>` this file drives DOES complete, either with a real answer or with the
// named sentinel error code `AgentSession<...>::kSuspendedForApproval` (see that constant's own
// comment in agent_session.hpp for why this is the deliberate replacement for "the ask never
// resolves").
//
//   SU1 -- suspend_for_approval_ == true, no approval_decider_ configured: a call to a tool that
//          requires approval makes start_run() return an error whose code is the named sentinel
//          (kSuspendedForApproval), never a generic failure. A real Interaction opens, tagged
//          interaction_reason::approval, naming the exact run_id that suspended. The gated tool's
//          invoke() is never reached. input_required/approval_requested events fire on the event
//          stream.
//   SU2 -- while that interaction is still open, a SECOND start_run() is rejected with
//          run.approval_pending (start_run()'s own has_open_approval guard) -- no new run_id is
//          minted, the one open interaction is untouched.
//   SU3 -- resolve_interaction({interaction_id, approved=true}) resumes the SAME run: the pending
//          call is invoked for real (through the ordinary invoke_tool pipeline), the interaction
//          closes, and the run converges to a final response with the SAME run_id (a resumption,
//          not a fresh run -- I4 attributability).
//   SU4 -- resolve_interaction({interaction_id, approved=false}) folds a SYNTHETIC DENIAL
//          (make_denial_result, "denied by operator" / "tool.approval_denied") into history as an
//          ordinary tool_results_message, WITHOUT ever invoking the gated tool's real body, and the
//          run continues (converges) from there.
//   SU5 -- resolve_interaction() naming an UNKNOWN interaction_id fails with
//          session.resolve_interaction.unknown_id and mutates nothing -- history and
//          open_interactions() are both unchanged.
//   SU6 -- resolve_interaction() whose caller identity does not admit against the session's own
//          principal is denied at admission (run.admission_denied, admission_denied_count()
//          observes it) -- the same admission check start_run() already has, applied to
//          resolve_interaction() too. The open interaction stays open; a denied resolver must not
//          consume it.

#include <cstdio>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
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

// Drives an agentengine::rt::task<T> to completion -- same "safe because nothing here genuinely
// suspends on an external wake" reasoning as test_rt_agent_session.cpp's own drive<T>().
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::CapabilitySet;
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

// -- The gated tool: needs approval before every call -----------------------------------------------

struct GateArgs { int value = 0; };
AE_JSON_SCHEMA(GateArgs, value)
struct GateReply { int value = 0; };
AE_JSON_SCHEMA(GateReply, value)

[[nodiscard]] bool& gated_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

// Empty capability ceiling (so a real approval decision, not a missing-capability one, is what's
// under test) but `always_require` -- exactly the shape the old, Quark-based
// test_agent_session_suspend_approval.cpp's own GatedTool used.
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

// decisions/ADR-070-host-configurable-responsibility-boundary.md: a second gated tool, this one
// `policy_driven` -- the mode the new PolicyDecider seam exists to resolve, distinct from
// GatedTool's `always_require` above.
struct PolicyGateArgs { int value = 0; };
AE_JSON_SCHEMA(PolicyGateArgs, value)
struct PolicyGateReply { int value = 0; };
AE_JSON_SCHEMA(PolicyGateReply, value)

[[nodiscard]] bool& policy_gated_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

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

// -- HistoryProviderT fixture: history passthrough + the gated tools' declaration --------------------

class GatedHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<GatedTool, PolicyGatedTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<GatedHistoryProvider>);

// -- The scripted backend, copied from test_rt_agent_session.cpp's own conventions ------------------

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

// SU4's own check: the message just pushed into history in place of a real tool invocation must be
// a role::tool message carrying exactly one ToolResult, is_error, call_id matching, and a nested
// Error content item whose message is the exact synthetic-denial text agent_session.hpp's own
// resolve_interaction() hard-codes via make_denial_result().
[[nodiscard]] bool is_synthetic_denial_for(Message const& m, std::string const& call_id) {
    if (m.role != role::tool) return false;
    if (m.content.size() != 1) return false;
    auto const* result = std::get_if<ToolResult>(&m.content.front().value);
    if (result == nullptr) return false;
    if (!result->is_error || result->call_id != call_id) return false;
    if (result->content.size() != 1) return false;
    auto const* err = std::get_if<agentengine::Error>(&result->content.front().value);
    return err != nullptr && err->message == "denied by operator";
}

using Session = AgentSession<ScriptedChatClient, NoSessionState, GatedHistoryProvider>;

}  // namespace

int main() {
    using agentengine::Principal;

    // ---- SU1: suspends with the named sentinel, mints a real Interaction, never invokes ---------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su1", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        // Deliberately no set_approval_decider() -- suspension only fires when neither a synchronous
        // decider nor an off suspend_for_approval_ would otherwise handle the call.

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(!outcome.has_value(), "SU1: start_run() fails while suspended for approval");
        check(outcome.has_value() || outcome.error().code == Session::kSuspendedForApproval,
              "SU1: the error code is the named kSuspendedForApproval sentinel, not a generic failure");
        check(!gated_tool_invoked_log(),
              "SU1: gated_tool's invoke() was never reached -- suspension happens BEFORE any call in "
              "the round is invoked");
        check(session.has_open_interactions(), "SU1: a real Interaction opened for this suspension");
        check(session.open_interactions().size() == 1 &&
                  session.open_interactions().front().reason == interaction_reason::approval,
              "SU1: the open Interaction is tagged interaction_reason::approval");
        check(session.open_interactions().front().run_id == session.last_run_id(),
              "SU1: the Interaction names the exact run_id that suspended");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        bool saw_input_required = false;
        bool saw_approval_requested = false;
        for (RunEvent const& ev : events) {
            if (ev.kind == run_event_kind::input_required) saw_input_required = true;
            if (ev.kind == run_event_kind::approval_requested) saw_approval_requested = true;
        }
        check(saw_input_required, "SU1: an input_required event fired");
        check(saw_approval_requested, "SU1: an approval_requested event fired for the pending call");
    }

    // ---- SU2: a fresh start_run() while suspended is rejected (run.approval_pending) --------------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su2", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU2 setup: the first start_run() suspends, same as SU1");
        std::string const run_id_at_suspend = session.last_run_id();

        auto r2 = drive(session.start_run(StartRun{user_message("second, while suspended")}));
        check(!r2.has_value(),
              "SU2: a second start_run() sent while an interaction is open is rejected -- fail-closed, "
              "not a second concurrent run");
        check(r2.has_value() || r2.error().code == "run.approval_pending",
              "SU2: the rejection is specifically run.approval_pending, start_run()'s own "
              "has_open_approval guard");
        check(session.last_run_id() == run_id_at_suspend,
              "SU2: no new run_id was minted -- the rejected start_run() never reached run_counter_ "
              "at all");
        check(session.open_interactions().size() == 1,
              "SU2: still exactly one open interaction -- the rejected start_run() didn't touch it");
    }

    // ---- SU3: resolve_interaction{approved=true} resumes the SAME run, invokes for real -----------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su3", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU3 setup: the run suspends");
        std::string const interaction_id = session.open_interactions().front().interaction_id;
        std::string const run_id_before_resume = session.last_run_id();

        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt}));
        check(r2.has_value(),
              "SU3: resolving with approved=true lets the run converge to a final response");
        if (r2.has_value()) {
            auto const* t = std::get_if<Text>(&r2->message.content.front().value);
            check(t != nullptr && t->text == "done",
                  "SU3: the converged response is the scripted post-resume text");
        }
        check(gated_tool_invoked_log(),
              "SU3: gated_tool's invoke() WAS called after approval -- the exact pending call, "
              "invoked for real through the ordinary pipeline");
        check(!session.has_open_interactions(), "SU3: the interaction closed once resolved");
        check(session.last_run_id() == run_id_before_resume,
              "SU3: the SAME run_id continued -- this was a resumption, not a fresh run (I4 "
              "attributability)");
        check(client.call_count() == 2,
              "SU3: the ChatClientT was called twice -- once before suspend, once after resume");
    }

    // ---- SU4: resolve_interaction{approved=false} folds a synthetic denial, never invokes ---------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su4", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("denied-path"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU4 setup: the run suspends");
        std::string const interaction_id = session.open_interactions().front().interaction_id;
        std::size_t const history_size_before = session.history().size();

        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt}));
        check(r2.has_value(),
              "SU4: resolving with approved=false still lets the run converge -- the denial is an "
              "ordinary tool error fed back, not a run-level failure");
        check(!gated_tool_invoked_log(),
              "SU4: gated_tool's invoke() was NEVER called -- a denial never reaches the real tool");
        check(!session.has_open_interactions(), "SU4: the interaction closed even on denial");
        check(session.history().size() == history_size_before + 2,
              "SU4: history grows by exactly the synthetic tool-results message plus the final "
              "response");
        check(history_size_before < session.history().size() &&
                  is_synthetic_denial_for(session.history()[history_size_before], "c1"),
              "SU4: the message folded in is the real make_denial_result() shape -- 'denied by "
              "operator' against call_id 'c1', not a fabricated success");
    }

    // ---- SU5: an unknown interaction_id fails closed, mutates nothing -----------------------------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su5", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU5 setup: the run suspends");
        std::size_t const history_size_before = session.history().size();

        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{"no-such-interaction", /*approved=*/true, std::nullopt}));
        check(!r2.has_value(), "SU5: resolving an unknown interaction_id fails");
        check(r2.has_value() || r2.error().code == "session.resolve_interaction.unknown_id",
              "SU5: the failure is specifically session.resolve_interaction.unknown_id");
        check(!gated_tool_invoked_log(), "SU5: gated_tool's invoke() was never reached");
        check(session.has_open_interactions() && session.open_interactions().size() == 1,
              "SU5: the REAL open interaction is untouched -- an unknown id must not resolve or "
              "otherwise mutate it");
        check(session.history().size() == history_size_before,
              "SU5: history is unchanged -- an unknown interaction_id mutates nothing");
    }

    // ---- SU6: a caller that doesn't admit against the owning principal is denied at admission -----
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su6", Principal{"owner", "tenant-a"});
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU6 setup: the run suspends");
        std::string const interaction_id = session.open_interactions().front().interaction_id;
        std::uint64_t const denied_before = session.admission_denied_count();

        ResolveInteraction req{interaction_id, /*approved=*/true,
                                 agentengine::rt::SessionCaller{"someone-else", "tenant-a"}};
        auto r2 = drive(session.resolve_interaction(req));
        check(!r2.has_value(),
              "SU6: a resolve_interaction() from an unrelated principal is denied -- admission runs "
              "before the interaction lookup ever executes");
        check(r2.has_value() || r2.error().code == "run.admission_denied",
              "SU6: the denial is specifically run.admission_denied, the same admission shape "
              "start_run()::caller already has");
        check(!gated_tool_invoked_log(), "SU6: gated_tool's invoke() was never reached");
        check(session.admission_denied_count() == denied_before + 1,
              "SU6: the denial is counted via admission_denied_count()");
        check(session.has_open_interactions(),
              "SU6: the interaction stays open -- a denied resolver must not consume it");
    }

    // ---- SU7 (ADR-070): a PolicyDecider that auto_approves a policy_driven call resolves it WITHOUT
    // ever suspending -- the suspend pre-check itself must recognize the PolicyDecider's verdict, not
    // just invoke_tool()'s own step 5. -------------------------------------------------------------
    {
        policy_gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su7", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "policy_gated_tool", R"({"value":9})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done-via-policy"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        // Deliberately no set_approval_decider() -- proving the PolicyDecider alone resolves this
        // policy_driven call without ever needing a human.
        session.set_policy_decider(
            [](Principal const&, agentengine::ToolDescriptor const&, bool) {
                return agentengine::policy_decision::auto_approve;
            });

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(r1.has_value(),
              "SU7 (ADR-070): a PolicyDecider that auto_approves a policy_driven call converges the "
              "run WITHOUT suspending, even with suspend_for_approval_ true and no ApprovalDecider");
        check(!session.has_open_interactions(),
              "SU7 (ADR-070): no Interaction ever opens -- the suspend pre-check itself recognizes "
              "the PolicyDecider's resolution and never treats this call as needing a human");
        check(policy_gated_tool_invoked_log(),
              "SU7 (ADR-070): the gated tool's invoke() WAS called -- PolicyDecider auto_approve is a "
              "real bypass of suspension, not merely of the ApprovalDecider");
        check(client.call_count() == 2,
              "SU7 (ADR-070): the ChatClientT was called twice, converging normally -- no suspend/"
              "resume round trip was needed at all");
    }

    // ---- SU8 (ADR-070): a PolicyDecider that auto_denies also never suspends -- the run converges
    // with the denial folded in like an ordinary tool error, exactly as if a human had denied it,
    // but no human was ever asked. -------------------------------------------------------------------
    {
        policy_gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su8", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "policy_gated_tool", R"({"value":9})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("denied-via-policy"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        session.set_policy_decider(
            [](Principal const&, agentengine::ToolDescriptor const&, bool) {
                return agentengine::policy_decision::auto_deny;
            });

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(r1.has_value(),
              "SU8 (ADR-070): a PolicyDecider that auto_denies still lets the run converge -- the "
              "denial is an ordinary tool error fed back, not a suspension and not a run-level "
              "failure");
        check(!session.has_open_interactions(),
              "SU8 (ADR-070): no Interaction ever opens for an auto_deny verdict either -- it never "
              "needed a human in the first place");
        check(!policy_gated_tool_invoked_log(),
              "SU8 (ADR-070): the gated tool's invoke() was NEVER called -- auto_deny blocks it "
              "exactly like a human denial would, without ever asking one");
    }

    // ---- SU9 (ADR-029 §6 / ADR-070): expired_interaction_ids()/set_interaction_expiry() -- the
    // host-driven query+setter pair that closes "nothing checks expires_at_ns", without this ADR
    // inventing a wall clock or a second resolution mechanism. ---------------------------------------
    {
        gated_tool_invoked_log() = false;

        Session session;
        session.initialize("su9", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "gated_tool", R"({"value":7})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("denied-by-timeout"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "SU9 setup: the run suspends");
        std::string const interaction_id = session.open_interactions().front().interaction_id;

        check(session.expired_interaction_ids(1'000).empty(),
              "SU9: a freshly-opened interaction has no expiry set (expires_at_ns == 0) -- "
              "expired_interaction_ids() finds nothing, unchanged default behavior");

        check(!session.set_interaction_expiry("no-such-id", 500),
              "SU9: set_interaction_expiry() on an unknown id returns false and touches nothing");

        check(session.set_interaction_expiry(interaction_id, /*expires_at_ns=*/1'000),
              "SU9: set_interaction_expiry() on the real open interaction returns true");

        check(session.expired_interaction_ids(500).empty(),
              "SU9: before the expiry, expired_interaction_ids() still finds nothing");
        auto const expired_at_deadline = session.expired_interaction_ids(1'000);
        check(expired_at_deadline.size() == 1 && expired_at_deadline.front() == interaction_id,
              "SU9: at/after the configured expires_at_ns, the interaction is reported expired "
              "(now_ns >= expires_at_ns, inclusive)");

        // The host's own timeout policy: deny it, via the ALREADY-EXISTING resolve_interaction() --
        // no new resolution mechanism, same synthetic-denial shape SU4 already proves.
        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt}));
        check(r2.has_value(),
              "SU9: a host-driven timeout denial resolves through the ordinary resolve_interaction() "
              "path and the run converges");
        check(!gated_tool_invoked_log(), "SU9: the gated tool's invoke() was never called on timeout");
        check(!session.has_open_interactions(), "SU9: the interaction closed once resolved");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_suspend_approval: ALL PASS\n");
    return 0;
}
