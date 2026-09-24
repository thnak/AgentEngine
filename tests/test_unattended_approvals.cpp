// Proof for decisions/ADR-184-unattended-mode.md, the approval half (no HTTPS needed, so it runs in every build):
//   A1-A7    Unattended approvals: off by default (an `always_require` call and a `text_derived` call are denied, a
//            suspend-for-approval session suspends); on, both run with no human, each approval audited exactly once;
//            a `PolicyDecider`'s auto_deny still denies -- for a `text_derived` call too (A5b, red team MAJOR);
//            unattended overrides the host's decider; clearing it restores the default.
//   A8-A11   A veto decider narrows it; an empty operator id is refused; `enable_unattended_mode` sets everything; a
//            stateful host decider keeps its state across rounds in default mode (red team MAJOR).
//   S1       A streaming join never merges items whose delivery marks differ.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/chat_stream_drain.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

namespace ae = agentengine;
using ae::rt::AgentSession;
using ae::rt::NoSessionState;
using ae::rt::StartRun;

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool cond, char const* what) {
    ++g_checks;
    if (!cond) ++g_failures;
    std::fprintf(stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what);
}

template <class T>
T drive(ae::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

ae::Message text_msg(ae::role r, std::string text, ae::content_origin origin, bool tainted) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = origin;
    item.tainted = tainted;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

struct GateArgs {
    int value = 0;
};
AE_JSON_SCHEMA(GateArgs, value)
struct GateReply {
    int value = 0;
};
AE_JSON_SCHEMA(GateReply, value)

int& always_calls() {
    static int n = 0;
    return n;
}
int& effectful_calls() {
    static int n = 0;
    return n;
}
int& policy_calls() {
    static int n = 0;
    return n;
}
int& policy_effectful_calls() {
    static int n = 0;
    return n;
}

// Always needs a decider.
struct AlwaysGated : ae::Tool<AlwaysGated, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>,
                              ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name = "always_gated";
    static constexpr std::string_view description = "Needs approval before every call.";
    using Args = GateArgs;
    using Reply = GateReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++always_calls();
        return Reply{a.value};
    }
};

// No approval for a structured call -- but a text_derived call to it (not pure) needs one (ADR-023).
struct Effectful : ae::Tool<Effectful, ae::Capabilities<>, ae::EffectClass<ae::effect_class::at_most_once>,
                            ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name = "effectful";
    static constexpr std::string_view description = "Has an effect; no approval for structured calls.";
    using Args = GateArgs;
    using Reply = GateReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++effectful_calls();
        return Reply{a.value};
    }
};

struct PolicyGated : ae::Tool<PolicyGated, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>,
                              ae::Approval<ae::approval_mode::policy_driven>> {
    static constexpr std::string_view name = "policy_gated";
    static constexpr std::string_view description = "Resolved by the host's policy.";
    using Args = GateArgs;
    using Reply = GateReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++policy_calls();
        return Reply{a.value};
    }
};

// Resolved by policy and has an effect: the plan-mode gate's shape (red team probe P1b).
struct PolicyEffectful : ae::Tool<PolicyEffectful, ae::Capabilities<>, ae::EffectClass<ae::effect_class::at_most_once>,
                                  ae::Approval<ae::approval_mode::policy_driven>> {
    static constexpr std::string_view name = "policy_effectful";
    static constexpr std::string_view description = "Resolved by the host's policy; has an effect.";
    using Args = GateArgs;
    using Reply = GateReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++policy_effectful_calls();
        return Reply{a.value};
    }
};

class ToolsProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<AlwaysGated, Effectful, PolicyGated, PolicyEffectful>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<ToolsProvider>);

class ScriptedClient {
public:
    ScriptedClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<ae::Message> script;
        std::size_t calls = 0;
    };
    void set_script(std::vector<ae::Message> s) { state_->script = std::move(s); }
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext&) {
        std::size_t const i = state_->calls < state_->script.size() ? state_->calls : state_->script.size() - 1;
        ++state_->calls;
        co_return ae::ChatResponse{state_->script[i], ae::Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};

ae::Message call_msg(std::string tool, ae::call_provenance provenance, std::string call_id = "c1") {
    ae::Message m;
    m.role = ae::role::assistant;
    ae::ContentItem item;
    item.origin = ae::content_origin::assistant;
    ae::ToolCall call;
    call.call_id = std::move(call_id);
    call.tool_name = std::move(tool);
    call.arguments_json = R"({"value":7})";
    call.provenance = provenance;
    item.value = call;
    m.content.push_back(item);
    return m;
}

ae::Message done() { return text_msg(ae::role::assistant, "done", ae::content_origin::assistant, false); }

using ToolSession = AgentSession<ScriptedClient, NoSessionState, ToolsProvider>;

struct RunOutcome {
    bool converged = false;
    std::vector<std::string> decisions;
    bool suspended = false;
};

template <class Configure>
RunOutcome run_script(std::vector<ae::Message> script, Configure configure) {
    ToolSession session;
    session.initialize("s-a", ae::Principal{"p1", ""});
    session.emplace_chat_client().set_script(std::move(script));
    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
    session.set_capabilities(&held);
    configure(session);
    RunOutcome out;
    session.set_run_event_tap([&out](ae::RunEvent const& e) {
        if (e.kind != ae::run_event_kind::policy_decision) return;
        if (auto const* p = std::get_if<ae::run_event_payload::PolicyDecision>(&e.payload)) {
            out.decisions.push_back(p->description);
        }
    });
    auto r = drive(session.start_run(StartRun{text_msg(ae::role::user, "go", ae::content_origin::user, false)}));
    out.converged = r.has_value();
    out.suspended = session.has_open_interactions();
    return out;
}

template <class Configure>
RunOutcome run_call(std::string tool, ae::call_provenance provenance, Configure configure) {
    return run_script({call_msg(std::move(tool), provenance), done()}, configure);
}

std::size_t count_containing(std::vector<std::string> const& v, std::string const& needle) {
    std::size_t n = 0;
    for (auto const& s : v) {
        if (s.find(needle) != std::string::npos) ++n;
    }
    return n;
}

void unattended(ToolSession& s) { (void)s.set_unattended_approvals("ops-automation"); }

}  // namespace

int main() {
    auto none = [](ToolSession&) {};
    {
        always_calls() = 0;
        RunOutcome const o = run_call("always_gated", ae::call_provenance::vendor_structured, none);
        check(always_calls() == 0 && o.decisions.empty(),
              "A1: default -- an always_require call with no decider is not run, and nothing is audited as approved");
    }
    {
        always_calls() = 0;
        RunOutcome const o = run_call("always_gated", ae::call_provenance::vendor_structured, unattended);
        check(always_calls() == 1 && o.converged, "A2: unattended -- the always_require call runs with no human");
        check(o.decisions.size() == 1 && count_containing(o.decisions, "unattended approval") == 1 &&
                  count_containing(o.decisions, "always_gated") == 1 &&
                  count_containing(o.decisions, "ops-automation") == 1 &&
                  count_containing(o.decisions, "caller p1") == 1 && count_containing(o.decisions, "arguments #") == 1,
              "A2: exactly one audit event, naming the tool, the operator, the caller and the arguments (I4)");
    }
    {
        always_calls() = 0;
        RunOutcome const o = run_call("always_gated", ae::call_provenance::vendor_structured,
                                      [](ToolSession& s) { s.set_suspend_for_approval(true); });
        check(o.suspended && always_calls() == 0, "A3: default with suspend-for-approval -- the session suspends");
        always_calls() = 0;
        RunOutcome const u = run_call("always_gated", ae::call_provenance::vendor_structured, [](ToolSession& s) {
            s.set_suspend_for_approval(true);
            unattended(s);
        });
        check(!u.suspended && always_calls() == 1 && u.converged, "A3: unattended -- it never suspends; the call runs");
    }
    {
        effectful_calls() = 0;
        (void)run_call("effectful", ae::call_provenance::vendor_structured, none);
        check(effectful_calls() == 1, "A4 control: a structured call to a never_require tool runs, as before");
        effectful_calls() = 0;
        (void)run_call("effectful", ae::call_provenance::text_derived, none);
        check(effectful_calls() == 0,
              "A4: default -- a text_derived call to a tool with an effect is not run (ADR-023's gate)");
        effectful_calls() = 0;
        RunOutcome const u = run_call("effectful", ae::call_provenance::text_derived, unattended);
        check(effectful_calls() == 1 && count_containing(u.decisions, "effectful") == 1,
              "A4: unattended -- the text_derived call runs, and its approval is audited");
    }
    auto deny_policy = [](ToolSession& s) {
        unattended(s);
        s.set_policy_decider(
            [](ae::Principal const&, ae::ToolDescriptor const&, bool) { return ae::policy_decision::auto_deny; });
    };
    {
        policy_calls() = 0;
        RunOutcome const o = run_call("policy_gated", ae::call_provenance::vendor_structured, deny_policy);
        check(policy_calls() == 0 && count_containing(o.decisions, "unattended approval") == 0,
              "A5: a PolicyDecider's auto_deny still denies a structured call in unattended mode");
        policy_effectful_calls() = 0;
        RunOutcome const t = run_call("policy_effectful", ae::call_provenance::text_derived, deny_policy);
        check(policy_effectful_calls() == 0 && count_containing(t.decisions, "unattended approval") == 0,
              "A5b: ... and a text_derived call too (red team MAJOR: it used to skip the policy and reach the "
              "always-yes decider)");
        policy_effectful_calls() = 0;
        bool asked = false;
        (void)run_call("policy_effectful", ae::call_provenance::text_derived, [&](ToolSession& s) {
            s.set_policy_decider(
                [](ae::Principal const&, ae::ToolDescriptor const&, bool) { return ae::policy_decision::auto_approve; });
            s.set_approval_decider([&](ae::Principal const&, std::string_view, std::string const&) {
                asked = true;
                return false;
            });
        });
        check(policy_effectful_calls() == 0 && asked,
              "A5c: a policy's auto_approve is still never an approval for a text_derived call (ADR-070 §4a) -- it "
              "goes to the decider");
    }
    {
        always_calls() = 0;
        bool host_decider_called = false;
        (void)run_call("always_gated", ae::call_provenance::vendor_structured, [&](ToolSession& s) {
            s.set_approval_decider([&](ae::Principal const&, std::string_view, std::string const&) {
                host_decider_called = true;
                return false;
            });
            unattended(s);
        });
        check(always_calls() == 1 && !host_decider_called,
              "A6: unattended overrides the host's own decider while set (documented)");
    }
    {
        always_calls() = 0;
        RunOutcome const o = run_call("always_gated", ae::call_provenance::vendor_structured, [](ToolSession& s) {
            unattended(s);
            s.clear_unattended_approvals();
        });
        check(always_calls() == 0 && o.decisions.empty(), "A7: cleared, the default is back -- the call is denied");
    }
    {
        always_calls() = 0;
        effectful_calls() = 0;
        auto veto = [](ae::Principal const&, std::string_view tool, std::string const&) { return tool != "always_gated"; };
        RunOutcome const o = run_script({call_msg("always_gated", ae::call_provenance::vendor_structured),
                                         call_msg("effectful", ae::call_provenance::text_derived, "c2"), done()},
                                        [&](ToolSession& s) { (void)s.set_unattended_approvals("ops-automation", veto); });
        check(always_calls() == 0 && effectful_calls() == 1 && count_containing(o.decisions, "vetoed") == 1 &&
                  count_containing(o.decisions, "unattended approval") == 1,
              "A8: a veto decider denies what it names (audited as vetoed) and the rest is approved unattended");
    }
    {
        ToolSession s;
        auto a = s.set_unattended_approvals("");
        auto b = s.disable_system_channel_fence("");
        auto c = s.enable_unattended_mode("");
        check(!a && !b && !c && !s.unattended_approvals() && s.system_channel_fence_enabled() &&
                  a.error().code == "session.unattended_operator_missing",
              "A9: an empty operator id is refused and changes nothing (I4: the audit must name someone)");
    }
    {
        ToolSession s;
        ae::ApprovedLessonRegistry reg;
        auto r = s.enable_unattended_mode("ops-automation", &reg);
        check(r.has_value() && s.unattended_approvals() && !s.system_channel_fence_enabled(),
              "A10: enable_unattended_mode turns on unattended approvals and turns off the fence in one call");
    }
    {
        always_calls() = 0;
        (void)run_script({call_msg("always_gated", ae::call_provenance::vendor_structured, "c1"),
                          call_msg("always_gated", ae::call_provenance::vendor_structured, "c2"),
                          call_msg("always_gated", ae::call_provenance::vendor_structured, "c3"), done()},
                         [&](ToolSession& s) {
                             s.set_approval_decider(
                                 [n = 0](ae::Principal const&, std::string_view, std::string const&) mutable {
                                     return n++ < 1;
                                 });
                         });
        check(always_calls() == 1,
              "A11: default mode -- a stateful host decider (approve at most once) keeps its state across rounds (red "
              "team MAJOR: the decider was copied per round and reset)");
    }
    {
        ae::Message acc;
        acc.role = ae::role::system;
        ae::ContentItem a;
        a.origin = ae::content_origin::external;
        a.tainted = true;
        a.value = ae::Text{"first "};
        ae::append_stream_delta(acc, a, false);
        ae::ContentItem b = a;
        b.value = ae::Text{"second"};
        b.deliver_as_instructions = true;
        ae::append_stream_delta(acc, b, true);
        check(acc.content.size() == 2, "S1: a streamed delta with a different delivery mark is never joined");
    }

    std::fprintf(stderr, "test_unattended_approvals: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
