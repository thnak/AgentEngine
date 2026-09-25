// Proof for ADR-183 (decisions/ADR-183-approval-resolved-before-dispatch.md): on every path that
// resolves an `interaction_reason::approval` interaction, `approval_resolved` is emitted
//   (a) for exactly the calls that got `approval_requested` for that interaction, in the same order,
//   (b) before any `tool_call_started` of the resumed round,
//   (c) with the operator's decision in `approved`.
//
//   O1 -- plain round, one gated call, approve: resolved(c1, true) precedes tool_call_started(c1).
//   O2 -- plain round, one gated call, deny: resolved(c1, false); no tool_call_started.
//   O3 -- plain mixed round (gated + ungated): only the gated call is asked (ADR-196 fixed BUG-1), it is
//         resolved, and both resolutions precede the first tool_call_started.
//   O4 -- hook-touched round that suspends directly for approval (the hook rewrites c1's arguments
//         and denies c2): approval_requested carries the REWRITTEN arguments (regression for a
//         read of a moved-from vector, ADR-183 §2), and on approve both asked calls are resolved
//         before c1 starts; c2 (hook-denied) never starts. Before ADR-183 this path emitted no
//         approval_resolved at all.
//   O5 -- same round, deny: both resolved with approved=false, nothing starts.
//   O6/O7 -- cascade: a hook_decision resume that still needs a human suspends for approval. Its
//         approval_requested names the pass-through calls only (not the hook-denied c3), and the
//         approval_resolved pairs with exactly those, on approve (O6) and deny (O7).
//
// Style follows tests/test_rt_agent_session_tool_call_hook.cpp: offline, hand-scripted
// ChatClientT, check()/drive<T>() copied rather than shared.

#include <algorithm>
#include <cstdio>
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
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

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
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::HookDispatchAnswer;
using agentengine::Message;
using agentengine::RunEvent;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolCallHookContext;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::interaction_reason;
using agentengine::role;
using agentengine::run_event_kind;
namespace payload = agentengine::run_event_payload;

int& gated_runs() {
    static int n = 0;
    return n;
}
int& plain_runs() {
    static int n = 0;
    return n;
}

struct ValueArgs { int value = 0; };
AE_JSON_SCHEMA(ValueArgs, value)
struct ValueReply { int value = 0; };
AE_JSON_SCHEMA(ValueReply, value)

// `pure` and capability-free on purpose: O4's hook rewrite downgrades c1 to text_derived, which is the
// shape ADR-183 §5 found skipping approval. Since ADR-184 it stays approval-gated.
struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<>,
                                       agentengine::EffectClass<agentengine::effect_class::pure>,
                                       agentengine::Approval<agentengine::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Needs approval before every call.";
    using Args = ValueArgs;
    using Reply = ValueReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        ++gated_runs();
        return Reply{a.value};
    }
};

struct PlainTool : agentengine::Tool<PlainTool, agentengine::Capabilities<>,
                                       agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "plain_tool";
    static constexpr std::string_view description = "Never needs approval.";
    using Args = ValueArgs;
    using Reply = ValueReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        ++plain_runs();
        return Reply{a.value};
    }
};

class Provider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<GatedTool, PlainTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<Provider>);

// Strict: a model call past the script fails the test loudly instead of repeating a reply.
class Client {
public:
    Client() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Message> script;
        std::size_t calls = 0;
    };
    void set_script(std::vector<Message> s) { state_->script = std::move(s); }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        if (state_->calls >= state_->script.size()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "script exhausted", "test.script_exhausted"});
        }
        co_return ChatResponse{state_->script[state_->calls++], Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<Client>);

struct Call {
    std::string id, tool, args;
};
Message calls_msg(std::vector<Call> calls) {
    Message m;
    m.role = role::assistant;
    for (Call& c : calls) {
        ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall tc;
        tc.call_id = std::move(c.id);
        tc.tool_name = std::move(c.tool);
        tc.arguments_json = std::move(c.args);
        tc.provenance = call_provenance::vendor_structured;
        item.value = tc;
        m.content.push_back(item);
    }
    return m;
}
Message text_msg(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}
Message user_msg(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

using Session = AgentSession<Client, NoSessionState, Provider>;
using Viewer = decltype(std::declval<Session&>().enable_event_stream(std::pmr::get_default_resource()));

std::vector<RunEvent> drain(Viewer& v) {
    std::vector<RunEvent> out;
    while (auto ev = v.next()) out.push_back(std::move(*ev));
    return out;
}

std::vector<std::string> requested_ids(std::vector<RunEvent> const& evs, std::string const& interaction_id) {
    std::vector<std::string> ids;
    for (RunEvent const& e : evs) {
        if (e.kind != run_event_kind::approval_requested) continue;
        auto const& p = std::get<payload::ApprovalRequested>(e.payload);
        if (p.interaction_id == interaction_id) ids.push_back(p.call_id);
    }
    return ids;
}

// The resume's events: every approval_resolved for `interaction_id` with its decision, whether each
// came after that interaction's input_resolved and before the first tool_call_started, and how many
// approval_resolved named some other interaction.
struct ResumeView {
    std::vector<std::string> resolved_ids;
    std::vector<bool> decisions;
    bool all_before_first_start = true;
    bool all_after_input_resolved = true;
    int foreign_resolved = 0;
    std::vector<std::string> started_ids;
};
ResumeView view_resume(std::vector<RunEvent> const& evs, std::string const& interaction_id) {
    ResumeView v;
    bool input_resolved_seen = false;
    for (RunEvent const& e : evs) {
        if (e.kind == run_event_kind::tool_call_started) {
            v.started_ids.push_back(std::get<payload::ToolCallStarted>(e.payload).call_id);
        } else if (e.kind == run_event_kind::input_resolved) {
            if (std::get<payload::InteractionRef>(e.payload).interaction_id == interaction_id) {
                input_resolved_seen = true;
            }
        } else if (e.kind == run_event_kind::approval_resolved) {
            auto const& p = std::get<payload::ApprovalResolved>(e.payload);
            if (p.interaction_id != interaction_id) {
                ++v.foreign_resolved;
                continue;
            }
            v.resolved_ids.push_back(p.call_id);
            v.decisions.push_back(p.approved);
            if (!v.started_ids.empty()) v.all_before_first_start = false;
            if (!input_resolved_seen) v.all_after_input_resolved = false;
        }
    }
    return v;
}

bool all_equal(std::vector<bool> const& v, bool value) {
    return std::all_of(v.begin(), v.end(), [&](bool b) { return b == value; });
}

std::string join(std::vector<std::string> const& v) {
    std::string s;
    for (std::string const& x : v) s += (s.empty() ? "" : ",") + x;
    return "[" + s + "]";
}

struct Suspended {
    std::string interaction_id;
    std::vector<RunEvent> events;
};

Suspended start_suspended(Session& s, Viewer& viewer, std::string const& tag) {
    auto r = drive(s.start_run(StartRun{user_msg("go")}));
    check(!r.has_value() && r.error().code == Session::kSuspendedForApproval,
          tag + ": suspends for approval (" + (r.has_value() ? std::string("ran to completion") : r.error().code) + ")");
    Suspended out;
    out.events = drain(viewer);
    if (s.open_interactions().size() == 1) out.interaction_id = s.open_interactions().front().interaction_id;
    return out;
}

void setup(Session& s, std::string const& id, CapabilitySet const& held, std::vector<Message> script) {
    s.initialize(id, agentengine::Principal{"p", ""});
    s.emplace_chat_client().set_script(std::move(script));
    s.set_capabilities(&held);
    s.set_suspend_for_approval(true);
}

void reset_counts() {
    gated_runs() = 0;
    plain_runs() = 0;
}

}  // namespace

int main() {
    CapabilitySet const held = CapabilitySet::grant_root({});

    // O1 / O2 -- plain round, one gated call.
    for (bool const approve : {true, false}) {
        std::string const tag = approve ? "O1" : "O2";
        reset_counts();
        Session s;
        setup(s, tag, held, {calls_msg({{"c1", "gated_tool", R"({"value":1})"}}), text_msg("done")});
        auto viewer = s.enable_event_stream(std::pmr::get_default_resource());
        Suspended sus = start_suspended(s, viewer, tag);
        auto asked = requested_ids(sus.events, sus.interaction_id);
        auto r = drive(s.resolve_interaction(ResolveInteraction{sus.interaction_id, approve, std::nullopt}));
        check(r.has_value(), tag + ": the run converges after resolve" + (r.has_value() ? std::string() : " (" + r.error().code + ": " + r.error().message + ")"));
        ResumeView v = view_resume(drain(viewer), sus.interaction_id);
        check(asked == std::vector<std::string>{"c1"} && v.resolved_ids == asked,
              tag + ": resolved " + join(v.resolved_ids) + " pairs with asked " + join(asked));
        check(all_equal(v.decisions, approve), tag + ": approved carries the decision");
        check(v.all_after_input_resolved && v.foreign_resolved == 0,
              tag + ": approval_resolved follows its input_resolved; none for another interaction");
        check(v.all_before_first_start, tag + ": approval_resolved precedes any tool_call_started");
        if (approve) {
            check(v.started_ids == std::vector<std::string>{"c1"} && gated_runs() == 1, "O1: c1 ran once, after");
        } else {
            check(v.started_ids.empty() && gated_runs() == 0, "O2: nothing started");
        }
    }

    // O3 -- plain mixed round: gated + ungated.
    {
        reset_counts();
        Session s;
        setup(s, "o3", held,
              {calls_msg({{"c1", "gated_tool", R"({"value":1})"}, {"c2", "plain_tool", R"({"value":2})"}}),
               text_msg("done")});
        auto viewer = s.enable_event_stream(std::pmr::get_default_resource());
        Suspended sus = start_suspended(s, viewer, "O3");
        auto asked = requested_ids(sus.events, sus.interaction_id);
        check(asked == std::vector<std::string>{"c1"}, "O3: only the gated call is asked (ADR-196, BUG-1 fixed)");
        auto r = drive(s.resolve_interaction(ResolveInteraction{sus.interaction_id, true, std::nullopt}));
        check(r.has_value(), "O3: the run converges");
        ResumeView v = view_resume(drain(viewer), sus.interaction_id);
        check(v.resolved_ids == asked, "O3: resolved " + join(v.resolved_ids) + " pairs with asked");
        check(v.all_before_first_start, "O3: every approval_resolved precedes the first tool_call_started");
        check(v.started_ids.size() == 2 && gated_runs() == 1 && plain_runs() == 1, "O3: both calls ran once");
    }

    // O4 / O5 -- hook-touched round suspending directly for approval: rewrite c1, deny c2.
    for (bool const approve : {true, false}) {
        std::string const tag = approve ? "O4" : "O5";
        reset_counts();
        Session s;
        setup(s, tag, held,
              {calls_msg({{"c1", "gated_tool", R"({"value":1})"}, {"c2", "plain_tool", R"({"value":2})"}}),
               text_msg("done")});
        s.set_tool_call_hook([](ToolCallHookContext& h) -> task<agentengine::result<std::monostate>> {
            if (h.call_id == "c1") h.rewritten_arguments = *agentengine::json::parse(R"({"value":77})");
            if (h.call_id == "c2") {
                h.denial = agentengine::error{agentengine::failure_class::contract, "no", "tool.hook_denied"};
            }
            co_return agentengine::result<std::monostate>{};
        });
        auto viewer = s.enable_event_stream(std::pmr::get_default_resource());
        Suspended sus = start_suspended(s, viewer, tag);
        auto asked = requested_ids(sus.events, sus.interaction_id);
        std::string c1_args;
        for (RunEvent const& e : sus.events) {
            if (e.kind != run_event_kind::approval_requested) continue;
            auto const& p = std::get<payload::ApprovalRequested>(e.payload);
            if (p.call_id == "c1") c1_args = p.arguments_json;
        }
        check(c1_args.find("77") != std::string::npos,
              tag + ": approval_requested shows the hook-rewritten arguments (" + c1_args + ")");
        auto r = drive(s.resolve_interaction(ResolveInteraction{sus.interaction_id, approve, std::nullopt}));
        check(r.has_value(), tag + ": the run converges");
        ResumeView v = view_resume(drain(viewer), sus.interaction_id);
        check(!asked.empty() && v.resolved_ids == asked,
              tag + ": resolved " + join(v.resolved_ids) + " pairs with asked " + join(asked));
        check(all_equal(v.decisions, approve), tag + ": approved carries the decision");
        check(v.all_before_first_start, tag + ": approval_resolved precedes any tool_call_started");
        check(plain_runs() == 0, tag + ": the hook-denied call never ran");
        if (approve) {
            check(v.started_ids == std::vector<std::string>{"c1"} && gated_runs() == 1, "O4: only c1 started, once");
        } else {
            check(v.started_ids.empty() && gated_runs() == 0, "O5: nothing started");
        }
    }

    // O6 / O7 -- cascade: a hook_decision resume that still needs a human. c1 is gated
    // (pass-through), c2 goes to external dispatch and is answered "allow", c3 is hook-denied. The
    // cascade asks about the pass-through calls only, so the resolution must name exactly c1 and c2,
    // never c3 -- on approve (O6) and deny (O7).
    for (bool const approve : {true, false}) {
        std::string const tag = approve ? "O6" : "O7";
        reset_counts();
        Session s;
        setup(s, tag, held,
              {calls_msg({{"c1", "gated_tool", R"({"value":1})"},
                          {"c2", "plain_tool", R"({"value":2})"},
                          {"c3", "plain_tool", R"({"value":3})"}}),
               text_msg("done")});
        s.set_tool_call_hook([](ToolCallHookContext& h) -> task<agentengine::result<std::monostate>> {
            if (h.call_id == "c2") h.needs_external_dispatch = true;
            if (h.call_id == "c3") {
                h.denial = agentengine::error{agentengine::failure_class::contract, "no", "tool.hook_denied"};
            }
            co_return agentengine::result<std::monostate>{};
        });
        auto viewer = s.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(s.start_run(StartRun{user_msg("go")}));
        check(!r1.has_value() && r1.error().code == Session::kSuspendedForHookDecision,
              tag + ": suspends for the hook decision first");
        std::string const hook_id = s.open_interactions().front().interaction_id;
        (void)drain(viewer);
        auto r2 = drive(s.resolve_interaction(ResolveInteraction{
            hook_id, false, std::nullopt, std::nullopt, std::nullopt,
            std::vector<HookDispatchAnswer>{HookDispatchAnswer{"c2", true, std::nullopt, std::nullopt}}}));
        check(!r2.has_value() && r2.error().code == Session::kSuspendedForApproval,
              tag + ": the hook resume cascades into an approval suspend");
        check(s.open_interactions().size() == 1 &&
                  s.open_interactions().front().reason == interaction_reason::approval,
              tag + ": one open approval interaction");
        std::string const approval_id = s.open_interactions().front().interaction_id;
        std::vector<RunEvent> const cascade_events = drain(viewer);
        auto asked = requested_ids(cascade_events, approval_id);
        check(asked == std::vector<std::string>({"c1"}),
              tag + ": the cascade asked about the gated pass-through call only (ADR-196): " + join(asked));
        check(view_resume(cascade_events, hook_id).resolved_ids.empty(),
              tag + ": the hook_decision interaction itself gets no approval_resolved");
        auto r3 = drive(s.resolve_interaction(ResolveInteraction{approval_id, approve, std::nullopt}));
        check(r3.has_value(), tag + ": the run converges");
        ResumeView v = view_resume(drain(viewer), approval_id);
        check(v.resolved_ids == asked,
              tag + ": resolved " + join(v.resolved_ids) + " pairs with asked " + join(asked));
        check(all_equal(v.decisions, approve), tag + ": approved carries the decision");
        check(v.all_after_input_resolved && v.foreign_resolved == 0,
              tag + ": approval_resolved follows its input_resolved; none for another interaction");
        check(v.all_before_first_start, tag + ": approval_resolved precedes any tool_call_started");
        if (approve) {
            check(gated_runs() == 1 && plain_runs() == 1, "O6: c1 and c2 ran once each, c3 never");
        } else {
            // ADR-196 (BUG-2 fixed): the deny covers the call it was asked about; c2 never needed approval and runs.
            check(v.started_ids.size() == 1 && gated_runs() == 0 && plain_runs() == 1,
                  "O7: the denied gated call never started; the ungated call ran once");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_approval_resolved_order: ALL PASS\n");
    return 0;
}
