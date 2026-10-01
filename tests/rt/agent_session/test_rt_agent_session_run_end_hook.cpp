// ADR-209 build step 3 (GitHub issue #146, claim C11): the optional, RELEASE-ONLY `on_run_end` hook
// (core/context_provider.hpp `HasOnRunEnd`, `AgentSessionCore::bound_on_run_end`,
// `ContextProviderDescriptor::on_run_end`, `ComposedContextProvider::on_run_end`).
//
// It fires once per completed run that emitted run_started -- final / max_turns / failed / canceled /
// exception -- never on `suspended`, never on a refusal (admission, run.approval_pending, an unknown
// interaction id). Offline, scripted model.
//
//   E1  a run that answers fires once with final_answer.
//   E2  a run suspended for approval fires NOTHING; neither do run.approval_pending, an unknown interaction id
//       or an admission-denied resolve while it is suspended; the approved resolve that completes it fires
//       exactly once.
//   E3  a tool loop cut off by max_turns fires max_turns.
//   E4  a failing model call fires failed.
//   E5  a run canceled mid-run (between turns) fires canceled.
//   E6  a model call that THROWS fires exception, and the exception still reaches the caller.
//   E7  composed: the hook reaches a provider wrapped in ComposedContextProvider (the only way the host can
//       release a composed provider), once per run.
//   E8  a provider without the hook compiles and runs unchanged (the hook is optional).
//   E9  the hook's `sandbox_exec_sink` is bound (step 5): a release failure it reports is a run event of the run
//       that completed, not dropped by the default no-op sink.
//   E10 (§15.5) the same for on_context: a sandbox event a provider reports there (a release at a turn start) is
//       a run event, not dropped.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): firing on "admission passed" instead of on
// run_started (fire in the wrapper whenever the body returned) fails E2; firing when the run suspended fails
// E2; deleting ComposedContextProvider::on_run_end's forwarding fails E7.

#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::task;
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

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

std::vector<run_end_reason>& fired() {
    static std::vector<run_end_reason> v;
    return v;
}

struct GateArgs { int value = 0; };
AE_JSON_SCHEMA(GateArgs, value)
struct GateReply { int value = 0; };
AE_JSON_SCHEMA(GateReply, value)

struct GatedTool : Tool<GatedTool, Capabilities<>, EffectClass<effect_class::pure>,
                        Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Needs approval.";
    using Args = GateArgs;
    using Reply = GateReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{a.value}; }
};

// Cancels the session it is told about, from inside a tool body -- a cancel BETWEEN turns.
rt::AgentSessionCore* g_cancel_target = nullptr;
struct CancelTool : Tool<CancelTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "cancel_tool";
    static constexpr std::string_view description = "Cancels the run.";
    using Args = GateArgs;
    using Reply = GateReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        if (g_cancel_target != nullptr) g_cancel_target->cancel();
        return Reply{a.value};
    }
};

bool g_context_reports = false;  // E10

// History passthrough + the tools; with the hook.
class HookedProvider {
public:
    static constexpr std::string_view name = "hooked";
    task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ctx) {
        if (g_context_reports) {
            ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished,
                                  run_event_payload::SandboxExec{"ctx-rel", "live-shell", "release", true, ""});
        }
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<GatedTool, CancelTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
    task<std::monostate> on_run_end(RunEndView view, EffectContext& ctx) {
        fired().push_back(view.reason);
        // E9: what a live shell does when its release fails.
        ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished,
                              run_event_payload::SandboxExec{"rel-1", "live-shell", "release", false, "test.rm_failed"});
        co_return std::monostate{};
    }
};
static_assert(ContextProvider<HookedProvider> && HasOnRunEnd<HookedProvider>);

// The same, without the hook (E8) -- and as the composed sibling in E7.
class PlainProvider {
public:
    static constexpr std::string_view name = "plain";
    task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        co_return ContextContribution{};
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<PlainProvider> && !HasOnRunEnd<PlainProvider>);

struct Step {
    std::optional<Message> message;
    std::optional<error> failure;
    bool throws = false;
};

class Client {
public:
    Client() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Step> script;
        std::size_t calls = 0;
    };
    void set_script(std::vector<Step> s) { state_->script = std::move(s); }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->calls < state_->script.size() ? state_->calls : state_->script.size() - 1;
        ++state_->calls;
        Step const& s = state_->script[idx];
        if (s.throws) throw std::runtime_error("model client threw");
        if (s.failure) co_return std::unexpected(*s.failure);
        co_return ChatResponse{*s.message, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};
static_assert(ChatClient<Client>);

Message text(std::string t) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(t)};
    m.content.push_back(item);
    return m;
}
Message call(std::string id, std::string tool, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall c;
    c.call_id = std::move(id);
    c.tool_name = std::move(tool);
    c.arguments_json = std::move(args);
    c.provenance = call_provenance::vendor_structured;
    item.value = c;
    m.content.push_back(item);
    return m;
}
Message user(std::string t) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(t)};
    m.content.push_back(item);
    return m;
}

using Session = AgentSession<Client, NoSessionState, HookedProvider>;
using Composed = ComposedContextProvider<HookedProvider, PlainProvider>;
using ComposedSession = AgentSession<Client, NoSessionState, Composed>;
using PlainSession = AgentSession<Client, NoSessionState, PlainProvider>;

}  // namespace

int main() {
    CapabilitySet const held = CapabilitySet::grant_root({});

    // ---- E1
    {
        fired().clear();
        Session s;
        s.initialize("e1", Principal{"p", ""});
        s.set_capabilities(&held);
        s.emplace_chat_client().set_script({Step{text("done"), {}, false}});
        auto r = drive(s.start_run(StartRun{user("hi")}));
        check(r.has_value(), "E1 setup: the run answers");
        check(fired().size() == 1 && fired()[0] == run_end_reason::final_answer,
              "E1: a completed run fires on_run_end once with final_answer");
    }

    // ---- E2
    {
        fired().clear();
        Session s;
        s.initialize("e2", Principal{"p", ""});
        s.set_capabilities(&held);
        s.set_suspend_for_approval(true);
        s.emplace_chat_client().set_script({Step{call("c1", "gated_tool", R"({"value":1})"), {}, false},
                                            Step{text("after approval"), {}, false}});
        auto r = drive(s.start_run(StartRun{user("go")}));
        check(!r.has_value() && r.error().code == Session::kSuspendedForApproval, "E2 setup: the run suspends");
        check(fired().empty(), "E2: a SUSPENDED run does not fire on_run_end");

        auto again = drive(s.start_run(StartRun{user("second")}));
        check(!again.has_value() && again.error().code == "run.approval_pending" && fired().empty(),
              "E2: run.approval_pending (a refusal) does not fire");

        ResolveInteraction bogus;
        bogus.interaction_id = "no-such-interaction";
        bogus.approved = true;
        auto unknown = drive(s.resolve_interaction(bogus));
        check(!unknown.has_value() && unknown.error().code == "session.resolve_interaction.unknown_id" &&
                  fired().empty(),
              "E2: a resolve naming an unknown interaction id does not fire (admission passed, nothing ran)");

        ResolveInteraction intruder;
        intruder.interaction_id = s.open_interactions().front().interaction_id;
        intruder.approved = true;
        intruder.caller = rt::SessionCaller{"someone-else", ""};
        auto denied = drive(s.resolve_interaction(intruder));
        check(!denied.has_value() && denied.error().code == "run.admission_denied" && fired().empty(),
              "E2: an admission-denied resolve does not fire");

        ResolveInteraction ok;
        ok.interaction_id = s.open_interactions().front().interaction_id;
        ok.approved = true;
        auto done = drive(s.resolve_interaction(ok));
        check(done.has_value(), "E2 setup: the approved resolve completes the run");
        check(fired().size() == 1 && fired()[0] == run_end_reason::final_answer,
              "E2: the resolve that completes the run fires exactly once");
    }

    // ---- E3
    {
        fired().clear();
        Session s;
        s.initialize("e3", Principal{"p", ""});
        s.set_capabilities(&held);
        s.set_max_turns(2);
        s.emplace_chat_client().set_script({Step{call("c1", "cancel_tool", R"({"value":1})"), {}, false}});
        g_cancel_target = nullptr;  // the tool does nothing here: a loop that never converges
        auto r = drive(s.start_run(StartRun{user("loop")}));
        check(!r.has_value() && r.error().code == "run.max_turns_exceeded", "E3 setup: max_turns cuts the loop off");
        check(fired().size() == 1 && fired()[0] == run_end_reason::max_turns, "E3: fires max_turns");
    }

    // ---- E4
    {
        fired().clear();
        Session s;
        s.initialize("e4", Principal{"p", ""});
        s.set_capabilities(&held);
        s.emplace_chat_client().set_script(
            {Step{std::nullopt, error{failure_class::transient, "backend down", "test.backend_down"}, false}});
        auto r = drive(s.start_run(StartRun{user("hi")}));
        check(!r.has_value(), "E4 setup: the model call fails the run");
        check(fired().size() == 1 && fired()[0] == run_end_reason::failed, "E4: fires failed");
    }

    // ---- E5
    {
        fired().clear();
        Session s;
        s.initialize("e5", Principal{"p", ""});
        s.set_capabilities(&held);
        s.emplace_chat_client().set_script({Step{call("c1", "cancel_tool", R"({"value":1})"), {}, false},
                                            Step{text("never"), {}, false}});
        g_cancel_target = &s;
        auto r = drive(s.start_run(StartRun{user("cancel me")}));
        g_cancel_target = nullptr;
        check(!r.has_value(), "E5 setup: the canceled run returns an error");
        check(fired().size() == 1 && fired()[0] == run_end_reason::canceled, "E5: fires canceled");
    }

    // ---- E6
    {
        fired().clear();
        Session s;
        s.initialize("e6", Principal{"p", ""});
        s.set_capabilities(&held);
        s.emplace_chat_client().set_script({Step{std::nullopt, std::nullopt, true}});
        bool caught = false;
        try {
            (void)drive(s.start_run(StartRun{user("throw")}));
        } catch (std::runtime_error const&) {
            caught = true;
        }
        check(caught, "E6: the exception still reaches the caller");
        check(fired().size() == 1 && fired()[0] == run_end_reason::exception, "E6: fires exception");
    }

    // ---- E7
    {
        fired().clear();
        ComposedSession s;
        s.initialize("e7", Principal{"p", ""});
        s.set_capabilities(&held);
        check(s.history_provider().engage(std::tuple<HookedProvider, PlainProvider>{}).has_value(),
              "E7 setup: engage the composed providers");
        s.emplace_chat_client().set_script({Step{text("one"), {}, false}});
        auto r1 = drive(s.start_run(StartRun{user("a")}));
        auto r2 = drive(s.start_run(StartRun{user("b")}));
        check(r1.has_value() && r2.has_value(), "E7 setup: two runs answer");
        check(fired().size() == 2, "E7: the hook reaches a COMPOSED provider, once per run");
    }

    // ---- E8
    {
        fired().clear();
        PlainSession s;
        s.initialize("e8", Principal{"p", ""});
        s.set_capabilities(&held);
        s.emplace_chat_client().set_script({Step{text("plain"), {}, false}});
        auto r = drive(s.start_run(StartRun{user("hi")}));
        check(r.has_value() && fired().empty(), "E8: a provider without the hook runs unchanged");
    }

    // ---- E9
    {
        fired().clear();
        Session s;
        s.initialize("e9", Principal{"p", ""});
        s.set_capabilities(&held);
        std::vector<RunEvent> events;
        s.set_run_event_tap([&](RunEvent const& e) { events.push_back(e); });
        s.emplace_chat_client().set_script({Step{text("done"), {}, false}});
        auto r = drive(s.start_run(StartRun{user("hi")}));
        std::string run_id;
        bool release_reported = false;
        for (auto const& e : events) {
            if (e.kind == run_event_kind::run_started) run_id = e.run_id;
            if (e.kind == run_event_kind::sandbox_exec_finished) {
                auto const* p = std::get_if<run_event_payload::SandboxExec>(&e.payload);
                release_reported = p != nullptr && p->stage == "release" && !p->ok && e.run_id == run_id;
            }
        }
        check(r.has_value() && release_reported,
              "E9: a sandbox event the hook reports becomes a run event of the run that completed");
    }

    // ---- E10 (ADR-209 §15.5): a release reported from on_context is a run event too (bracketed like E9).
    {
        Session s;
        s.initialize("e10", Principal{"p", ""});
        s.set_capabilities(&held);
        g_context_reports = true;
        std::vector<RunEvent> events;
        s.set_run_event_tap([&](RunEvent const& e) { events.push_back(e); });
        s.emplace_chat_client().set_script({Step{text("done"), {}, false}});
        auto r = drive(s.start_run(StartRun{user("hi")}));
        g_context_reports = false;
        bool reported = false;
        for (auto const& e : events) {
            if (e.kind != run_event_kind::sandbox_exec_finished) continue;
            auto const* p = std::get_if<run_event_payload::SandboxExec>(&e.payload);
            reported = reported || (p != nullptr && p->exec_id == "ctx-rel");
        }
        check(r.has_value() && reported, "E10: a sandbox event reported from on_context becomes a run event");
    }

    std::fprintf(stderr, g_failures == 0 ? "ALL PASS\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
