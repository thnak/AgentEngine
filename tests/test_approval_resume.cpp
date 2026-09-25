// Implements the proofs for decisions/ADR-196-per-call-approval-decisions.md (issues #104, #107, #108), ADR-198
// (issue #106), and the round-3 red-team fixes to ADR-191/192 (decisions/ADR-191-approved-lesson-delivery.md §8,
// decisions/ADR-192-unattended-mode.md §9) that live in the session's resume paths and the lesson registry.
//
//   U1   A tool the host's turn middleware hid from a round is never dispatched when that round resumes after a
//        tool-call hook decision -- with unattended approvals on (round 3 MAJOR 1). Control: the same hidden tool,
//        NOT hidden, does run on that resume path (the path really dispatches it).
//   U2   Clearing unattended mode while a CodeAct script waits on agent.ask() stops the replay (round 3 MAJOR 2).
//        Control: with unattended mode still on, the replay runs.
//   A1   An approval covers only the calls it was asked about: a policy_driven call the host's policy denies does
//        not run on the strength of another call's approval (ADR-196; this was an always-yes decider with no policy).
//        Control: the same round with a policy that allows it runs it.
//   A2   Per-call decisions: a call_id the interaction did not ask about, or one decided twice, is refused and the
//        interaction stays open; a retry then succeeds (issue #104).
//   A3   approval_resolved names the host-supplied approver; a blank or control-character approver is refused and
//        the interaction stays open; an unnamed resolve is recorded with an empty approver (issue #108).
//   S1   fork_from() drops a pending hook round: a later interaction that reuses its id never runs the old round's
//        stored requests (issue #107).
//   S2   restore_from_record() never hands out an interaction id the restored set already uses, and an `input`
//        interaction from a record is refused, not taken for an approval (issue #107).
//   F1   A model failure reports ONE code: run_failed.error_code is the result's own code, with the stage beside it
//        (issue #106, ADR-198).
//   L1   Lesson approvals are scoped by tenant (round 3 MAJOR: an approval for "alice" in one tenant reached "alice"
//        in another).
//   L2   The registry key is structured: a scope containing the old separator cannot reach another scope.
//   L3   An automatic (or simulated) approval never replaces a human's approval of the same text.
//   L4   Ids: blank / control-character approver, reviewer and operator ids are refused; the reserved automatic:/
//        simulated: prefix is refused anywhere in a human approver id.
//   L5   Knob 1: the instructions level names its operator (refused without one) and the delivery event says who.
//
// Positive controls: each MAJOR fix was reverted by hand and the matching check was seen to fail (recorded in
// ADR-196 §6 / ADR-192 §9).
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_hook.hpp"
#include "agentengine/core/turn_middleware.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/capability.hpp"

namespace ae = agentengine;
using ae::rt::AgentSession;
using ae::rt::AgentSessionRecord;
using ae::rt::ApprovalCallDecision;
using ae::rt::NoSessionState;
using ae::rt::ResolveInteraction;
using ae::rt::StartRun;

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

template <class T>
T drive(ae::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

ae::Message text_msg(ae::role r, std::string text) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

struct ValArgs {
    int value = 0;
};
AE_JSON_SCHEMA(ValArgs, value)
struct ValReply {
    int value = 0;
};
AE_JSON_SCHEMA(ValReply, value)

// Invocation counters, by tool name.
std::map<std::string, int>& runs() {
    static std::map<std::string, int> m;
    return m;
}

template <class Derived, ae::approval_mode Mode>
struct CountingTool : ae::Tool<Derived, ae::Capabilities<>, ae::EffectClass<ae::effect_class::at_most_once>,
                               ae::Approval<Mode>> {
    using Args  = ValArgs;
    using Reply = ValReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++runs()[std::string(Derived::name)];
        return Reply{a.value};
    }
};
struct HiddenTool : CountingTool<HiddenTool, ae::approval_mode::always_require> {
    static constexpr std::string_view name        = "hidden_tool";
    static constexpr std::string_view description = "Hidden from the round by the host's turn middleware.";
};
struct OtherTool : CountingTool<OtherTool, ae::approval_mode::never_require> {
    static constexpr std::string_view name        = "other_tool";
    static constexpr std::string_view description = "Answered by an external process through the hook.";
};
struct GatedTool : CountingTool<GatedTool, ae::approval_mode::always_require> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Always needs approval.";
};
struct PolicyTool : CountingTool<PolicyTool, ae::approval_mode::policy_driven> {
    static constexpr std::string_view name        = "policy_tool";
    static constexpr std::string_view description = "Decided by the host's policy.";
};
struct OldTool : CountingTool<OldTool, ae::approval_mode::never_require> {
    static constexpr std::string_view name        = "old_tool";
    static constexpr std::string_view description = "Called in a round that a fork later discards.";
};

class Provider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                            ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<HiddenTool, OtherTool, GatedTool, PolicyTool, OldTool>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

class ScriptedClient {
public:
    ScriptedClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<ae::result<ae::Message>> script;
        std::size_t calls = 0;
    };
    void set_script(std::vector<ae::result<ae::Message>> s) { state_->script = std::move(s); }
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext&) {
        std::size_t const i = state_->calls < state_->script.size() ? state_->calls : state_->script.size() - 1;
        ++state_->calls;
        if (!state_->script[i]) co_return std::unexpected(state_->script[i].error());
        co_return ae::ChatResponse{*state_->script[i], ae::Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }
    std::shared_ptr<State> state_;
};

ae::Message calls_msg(std::vector<std::pair<std::string, std::string>> const& id_tool, int value = 7) {
    ae::Message m;
    m.role = ae::role::assistant;
    for (auto const& [id, tool] : id_tool) {
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        ae::ToolCall call;
        call.call_id        = id;
        call.tool_name      = tool;
        call.arguments_json = "{\"value\":" + std::to_string(value) + "}";
        call.provenance     = ae::call_provenance::vendor_structured;
        item.value          = call;
        m.content.push_back(item);
    }
    return m;
}

using Session = AgentSession<ScriptedClient, NoSessionState, Provider>;

struct Events {
    std::vector<ae::run_event_payload::ApprovalResolved> resolved;
    std::vector<ae::run_event_payload::RunFailed>        failed;
    std::vector<std::string>                             decisions;
};
void tap(Session& s, Events& ev) {
    s.set_run_event_tap([&ev](ae::RunEvent const& e) {
        if (auto const* p = std::get_if<ae::run_event_payload::ApprovalResolved>(&e.payload)) ev.resolved.push_back(*p);
        if (auto const* p = std::get_if<ae::run_event_payload::RunFailed>(&e.payload)) ev.failed.push_back(*p);
        if (auto const* p = std::get_if<ae::run_event_payload::PolicyDecision>(&e.payload)) {
            ev.decisions.push_back(p->description);
        }
    });
}

ae::CapabilitySet const& no_caps() {
    static ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
    return held;
}

// ---- U1 ---------------------------------------------------------------------------------------------------------
int u1_hidden_runs(bool hide) {
    runs().clear();
    Session s;
    s.initialize("s-u1", ae::Principal{"p1", ""});
    s.emplace_chat_client().set_script({calls_msg({{"c1", "other_tool"}, {"c2", "hidden_tool"}}),
                                        text_msg(ae::role::assistant, "done")});
    s.set_capabilities(&no_caps());
    if (hide) {
        s.set_turn_middleware_hook([](ae::TurnContext& ctx) -> ae::task<ae::result<std::monostate>> {
            std::erase_if(ctx.assembled.combined.tools,
                          [](ae::ToolDescriptor const& t) { return t.name == "hidden_tool"; });
            co_return std::monostate{};
        });
    }
    s.set_tool_call_hook([](ae::ToolCallHookContext& h) -> ae::task<ae::result<std::monostate>> {
        if (h.tool_name == "other_tool") h.needs_external_dispatch = true;
        co_return std::monostate{};
    });
    (void)s.set_unattended_approvals("ops");
    (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
    if (s.has_open_interactions()) {
        ResolveInteraction r{s.open_interactions().front().interaction_id, false};
        r.hook_dispatch_answers =
            std::vector<ae::HookDispatchAnswer>{ae::HookDispatchAnswer{"c1", true, std::nullopt, std::nullopt}};
        (void)drive(s.resolve_interaction(r));
    }
    return runs()["hidden_tool"];
}

// ---- U2 ---------------------------------------------------------------------------------------------------------
struct AskArgs {
    std::string code;
    std::string language;
};
AE_JSON_SCHEMA(AskArgs, code, language)
struct AskReply {
    bool ok = false;
};
AE_JSON_SCHEMA(AskReply, ok)
struct AskTool : ae::Tool<AskTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::at_most_once>,
                          ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name        = "execute_code";
    static constexpr std::string_view description = "Asks once, then completes.";
    using Args  = AskArgs;
    using Reply = AskReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::fatal, "dead path", "test.dead_static_invoke_path"});
    }
};
int& ask_invocations() {
    static int n = 0;
    return n;
}
class AskProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                            ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = {ae::make_tool_descriptor_with_invoke<AskTool>(
            [](AskArgs, ae::EffectContext& ctx) -> ae::result<AskReply> {
                ++ask_invocations();
                if (ctx.codeact_preseeded_answers.empty()) {
                    return std::unexpected(
                        ae::error{ae::failure_class::contract, "proceed?", "codeact.ask_pending"});
                }
                return AskReply{true};
            })};
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
using AskSession = AgentSession<ScriptedClient, NoSessionState, AskProvider>;

int u2_replays(bool clear_before_answer) {
    ask_invocations() = 0;
    AskSession s;
    s.initialize("s-u2", ae::Principal{"p1", ""});
    ae::Message call_msg;
    call_msg.role = ae::role::assistant;
    ae::ContentItem item;
    item.origin = ae::content_origin::assistant;
    ae::ToolCall ask_call;
    ask_call.call_id        = "c1";
    ask_call.tool_name      = "execute_code";
    ask_call.arguments_json = R"json({"code":"agent.ask('proceed?')","language":"python"})json";
    ask_call.provenance     = ae::call_provenance::vendor_structured;
    item.value              = ask_call;
    call_msg.content.push_back(item);
    s.emplace_chat_client().set_script({call_msg, text_msg(ae::role::assistant, "done")});
    s.set_capabilities(&no_caps());
    (void)s.set_unattended_approvals("ops");
    auto first = drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
    if (first.has_value() || first.error().code != AskSession::kSuspendedForCodeActAsk) return -1;
    if (clear_before_answer) s.clear_unattended_approvals();
    ResolveInteraction r{s.open_interactions().front().interaction_id, false};
    r.answer = "yes";
    (void)drive(s.resolve_interaction(r));
    return ask_invocations();
}

// ---- A1 ---------------------------------------------------------------------------------------------------------
int a1_policy_tool_runs(ae::policy_decision verdict) {
    runs().clear();
    Session s;
    s.initialize("s-a1", ae::Principal{"p1", ""});
    s.emplace_chat_client().set_script({calls_msg({{"c1", "gated_tool"}, {"c2", "policy_tool"}}),
                                        text_msg(ae::role::assistant, "done")});
    s.set_capabilities(&no_caps());
    s.set_suspend_for_approval(true);
    s.set_policy_decider([verdict](ae::Principal const&, ae::ToolDescriptor const& td, bool) {
        return td.name == "policy_tool" ? verdict : ae::policy_decision::require_approval;
    });
    (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
    if (!s.has_open_interactions()) return -1;
    (void)drive(s.resolve_interaction(ResolveInteraction{s.open_interactions().front().interaction_id, true}));
    return runs()["gated_tool"] == 1 ? runs()["policy_tool"] : -2;
}

}  // namespace

int main() {
    // ---- U1 --------------------------------------------------------------------------------------------------
    check(u1_hidden_runs(/*hide=*/true) == 0,
          "U1: a tool the turn middleware hid from the round is not dispatched when the round resumes after a hook "
          "decision, even with unattended approvals on");
    check(u1_hidden_runs(/*hide=*/false) == 1,
          "U1 control: the same tool, not hidden, IS dispatched on that resume path");

    // ---- U2 --------------------------------------------------------------------------------------------------
    check(u2_replays(/*clear_before_answer=*/true) == 1,
          "U2: clearing unattended mode while the script waits on agent.ask() stops the replay (no second run)");
    check(u2_replays(/*clear_before_answer=*/false) == 2,
          "U2 control: with unattended mode still on, the replay runs the script again");

    // ---- A1 --------------------------------------------------------------------------------------------------
    check(a1_policy_tool_runs(ae::policy_decision::auto_deny) == 0,
          "A1: approving a round does not run a call the host's policy denies (it was never asked about)");
    check(a1_policy_tool_runs(ae::policy_decision::auto_approve) == 1,
          "A1 control: the same call, allowed by the policy, runs");

    // ---- A2 / A3 ---------------------------------------------------------------------------------------------
    {
        runs().clear();
        Session s;
        Events ev;
        s.initialize("s-a2", ae::Principal{"p1", ""});
        s.emplace_chat_client().set_script({calls_msg({{"c1", "gated_tool"}, {"c2", "other_tool"}}),
                                            text_msg(ae::role::assistant, "done")});
        s.set_capabilities(&no_caps());
        s.set_suspend_for_approval(true);
        tap(s, ev);
        (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
        std::string const ix = s.has_open_interactions() ? s.open_interactions().front().interaction_id : "";

        ResolveInteraction not_asked{ix, true};
        not_asked.call_decisions = std::vector<ApprovalCallDecision>{{"c2", true}};
        auto r1 = drive(s.resolve_interaction(not_asked));
        check(!r1 && r1.error().code == "session.resolve_interaction.call_not_pending" && s.has_open_interactions(),
              "A2: deciding a call the interaction never asked about is refused; the interaction stays open");

        ResolveInteraction twice{ix, true};
        twice.call_decisions = std::vector<ApprovalCallDecision>{{"c1", true}, {"c1", false}};
        auto r2 = drive(s.resolve_interaction(twice));
        check(!r2 && r2.error().code == "session.resolve_interaction.duplicate_call_decision" &&
                  s.has_open_interactions(),
              "A2: deciding one call twice is refused; the interaction stays open");

        ResolveInteraction blank{ix, true};
        blank.approver_id = "  ";
        auto r3 = drive(s.resolve_interaction(blank));
        ResolveInteraction newline{ix, true};
        newline.approver_id = "alice\nbob";
        auto r4 = drive(s.resolve_interaction(newline));
        check(!r3 && !r4 && r3.error().code == "session.resolve_interaction.bad_approver" &&
                  s.has_open_interactions() && ev.resolved.empty(),
              "A3: a blank or multi-line approver id is refused before anything is announced; still open");

        ResolveInteraction good{ix, false};
        good.call_decisions = std::vector<ApprovalCallDecision>{{"c1", true}};
        good.approver_id    = "alice";
        auto r5 = drive(s.resolve_interaction(good));
        check(r5.has_value() && ev.resolved.size() == 1 && ev.resolved[0].call_id == "c1" &&
                  ev.resolved[0].approved && ev.resolved[0].approver_id == "alice",
              "A2/A3: the retry succeeds; one approval_resolved, for the one asked call, naming alice");
        check(runs()["gated_tool"] == 1 && runs()["other_tool"] == 1,
              "A2: the approved call and the call that never needed approval both ran (round-level deny ignored "
              "for the call that was decided per call, and never applied to the unasked one)");
    }
    {
        runs().clear();
        Session s;
        Events ev;
        s.initialize("s-a3", ae::Principal{"p1", ""});
        s.emplace_chat_client().set_script({calls_msg({{"c1", "gated_tool"}}), text_msg(ae::role::assistant, "ok")});
        s.set_capabilities(&no_caps());
        s.set_suspend_for_approval(true);
        tap(s, ev);
        (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
        (void)drive(s.resolve_interaction(ResolveInteraction{s.open_interactions().front().interaction_id, true}));
        check(ev.resolved.size() == 1 && ev.resolved[0].approver_id.empty() && runs()["gated_tool"] == 1,
              "A3: a resolve that names nobody is recorded as anonymous (empty approver), not guessed");
    }

    // ---- S1 --------------------------------------------------------------------------------------------------
    {
        runs().clear();
        Session s;
        s.initialize("s-s1", ae::Principal{"p1", ""});
        // Round 1 (discarded by the fork): old_tool, held for an external hook decision.
        s.emplace_chat_client().set_script({calls_msg({{"c-old", "old_tool"}}), calls_msg({{"c-new", "gated_tool"}}),
                                            text_msg(ae::role::assistant, "done")});
        s.set_capabilities(&no_caps());
        s.set_suspend_for_approval(true);
        s.set_tool_call_hook([](ae::ToolCallHookContext& h) -> ae::task<ae::result<std::monostate>> {
            if (h.tool_name == "old_tool") h.needs_external_dispatch = true;
            co_return std::monostate{};
        });
        (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "first")}));
        std::string const old_id = s.has_open_interactions() ? s.open_interactions().front().interaction_id : "";
        Session fresh;
        fresh.initialize("s-s1", ae::Principal{"p1", ""});
        s.fork_from(fresh, "s-s1");
        s.set_capabilities(&no_caps());
        // Round 2 is not hook-touched, so nothing overwrites the discarded round's stored state under the reused id.
        s.set_tool_call_hook({});
        // Round 2: a plain approval that reuses the interaction id the discarded round had.
        (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "second")}));
        std::string const new_id = s.has_open_interactions() ? s.open_interactions().front().interaction_id : "";
        (void)drive(s.resolve_interaction(ResolveInteraction{new_id, true}));
        check(!old_id.empty() && old_id == new_id, "S1 setup: the new interaction reuses the discarded round's id");
        check(runs()["old_tool"] == 0 && runs()["gated_tool"] == 1,
              "S1: after fork_from(), resolving the reused id runs the new round, never the old round's stored calls");
    }

    // ---- S2 --------------------------------------------------------------------------------------------------
    {
        Session s;
        s.initialize("s-s2", ae::Principal{"p1", ""});
        s.emplace_chat_client().set_script({calls_msg({{"c1", "gated_tool"}}), text_msg(ae::role::assistant, "ok")});
        s.set_capabilities(&no_caps());
        s.set_suspend_for_approval(true);
        AgentSessionRecord rec;
        rec.session_id   = "s-s2";
        rec.principal_id = "p1";
        ae::Interaction restored{};
        restored.interaction_id = "s-s2:interaction:7";
        restored.reason         = ae::interaction_reason::input;
        rec.open_interactions.push_back(restored);
        s.restore_from_record(rec);
        s.set_capabilities(&no_caps());
        auto refused = drive(s.resolve_interaction(ResolveInteraction{"s-s2:interaction:7", true}));
        check(!refused && refused.error().code == "session.resolve_interaction.unsupported_reason",
              "S2: an `input` interaction from a record is refused, not taken for an approval");
        (void)drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
        bool fresh_id = false;
        for (ae::Interaction const& i : s.open_interactions()) {
            if (i.reason == ae::interaction_reason::approval) fresh_id = i.interaction_id == "s-s2:interaction:8";
        }
        check(fresh_id, "S2: the next interaction continues past the restored ids (8, not a reused 1..7)");
    }

    // ---- F1 --------------------------------------------------------------------------------------------------
    {
        Session s;
        Events ev;
        s.initialize("s-f1", ae::Principal{"p1", ""});
        s.emplace_chat_client().set_script(
            {std::unexpected(ae::error{ae::failure_class::transient, "overloaded", "provider.overloaded"})});
        s.set_capabilities(&no_caps());
        tap(s, ev);
        auto r = drive(s.start_run(StartRun{text_msg(ae::role::user, "go")}));
        check(!r && ev.failed.size() == 1 && ev.failed[0].error_code == r.error().code &&
                  ev.failed[0].error_code == "provider.overloaded" && ev.failed[0].stage == "run.chat_failed",
              "F1: run_failed carries the result's own code (provider.overloaded), stage run.chat_failed");
    }

    // ---- L1-L4 -------------------------------------------------------------------------------------------------
    {
        ae::ApprovedLessonRegistry reg;
        std::string const lesson = "Prefer the staging bucket for exports.";
        check(reg.approve(ae::LessonScope{"tenant-a", "alice"}, lesson, {"admin-a", "t", ""}).has_value(),
              "L1 setup: tenant-a's admin approves a lesson for alice");
        check(reg.find(ae::LessonScope{"tenant-a", "alice"}, lesson).has_value() &&
                  !reg.find(ae::LessonScope{"tenant-b", "alice"}, lesson).has_value() &&
                  !reg.find(ae::LessonScope{"", "alice"}, lesson).has_value() &&
                  reg.texts(ae::LessonScope{"tenant-b", "alice"}).empty(),
              "L1: the approval reaches alice in tenant-a only -- never alice in tenant-b, never a tenant-less alice");

        ae::ApprovedLessonRegistry keys;
        (void)keys.approve_automatic(std::string("victim\x1f" "X"), "C", "bot");
        check(!keys.find("victim", "X\x1f" "C").has_value() && keys.texts("victim").empty(),
              "L2: an approval for scope \"victim<US>X\" of \"C\" is not victim's approval of \"X<US>C\"");

        ae::ApprovedLessonRegistry human;
        (void)human.approve("p1", lesson, {"alice", "t", "ack-alice"});
        auto const automatic = human.approve_automatic("p1", lesson, "bot");
        auto const simulated = human.approve_simulated("p1", lesson, "trial-1");
        auto const kept = human.find("p1", lesson);
        check(!automatic && automatic.error().code == "memory.approval_would_replace_human" && !simulated && kept &&
                  kept->approval.approver_id == "alice" && !kept->approval.automatic,
              "L3: an automatic or simulated approval never replaces alice's human approval of the same text");
        ae::ApprovedLessonRegistry upgrade;
        (void)upgrade.approve_automatic("p1", lesson, "bot");
        check(upgrade.approve("p1", lesson, {"alice", "t", ""}).has_value() &&
                  !upgrade.find("p1", lesson)->approval.automatic,
              "L3: a human approval may replace an automatic one");

        ae::ApprovedLessonRegistry ids;
        bool const refused_all =
            !ids.approve("p1", "a", {" ", "t", ""}) && !ids.approve("p1", "a", {"alice\nautomatic:x", "t", ""}) &&
            !ids.approve("p1", "a", {"\xC2\xA0" "automatic:bot", "t", ""}) &&
            !ids.approve("p1", "a", {"\xE2\x80\x8B" "Simulated:t", "t", ""}) &&
            !ids.approve("p1", "a", {"alice", "t", "ack\n2"}) && !ids.approve_automatic("p1", "a", "\t") &&
            !ids.approve_simulated("p1", "a", " ") && ids.size() == 0;
        check(refused_all,
              "L4: blank / control-character ids and the reserved prefix anywhere in a human approver id are refused");

        Session s;
        s.initialize("s-l4", ae::Principal{"p1", ""});
        check(!s.set_unattended_approvals(" ") && !s.disable_system_channel_fence("ops\nroot") &&
                  !s.enable_unattended_mode("\t") && !s.unattended_approvals() && s.system_channel_fence_enabled(),
              "L4: blank / multi-line operator ids are refused by every unattended opt-in");
        check(!s.set_approved_lessons(&reg, ae::approved_lesson_level::instructions, "") &&
                  s.set_approved_lessons(&reg, ae::approved_lesson_level::instructions, "ops").has_value(),
              "L5: the instructions level is refused without an operator id and accepted with one");
    }

    std::fprintf(stderr, "%s (%d failure(s))\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
