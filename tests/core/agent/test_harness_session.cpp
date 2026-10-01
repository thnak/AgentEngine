// Proves decisions/ADR-234-harness-composition.md (issue #60; 002 §2.2): core/harness.hpp's
// make_harness_session<A>() / bind_harness_session() turn a declared agent into an ADR-226 bridge session
// carrying the harness's default stack -- and each piece is shown ACTIVE in a real start_run() against the
// strict ScriptedChatClient (no network), with a positive control that disables it and shows it gone.
//
//   H1  default composition: the six default pieces are present (instructions, history, todo, plan_execute,
//       approval, telemetry); every disable flag removes its piece; all disabled == a plain bridge session.
//   H2  todo: todos_add works and the list reaches the next model request; disabled -> todos_add is unknown.
//   H3  plan_execute: a declared never_require tool is held back until a real todos_add + plan_ready (R1:
//       the gate COVERS default tools), plan_ready refuses with no plan, pure tools stay usable while
//       planning, planning itself is never gated (R2); disabled -> the tool runs at once.
//   H4  approval: an approval-needing call parks the run (run.suspended_for_approval); resolve() continues it;
//       disabled -> the call is denied and the run continues (the bridge's own behaviour).
//   H5  telemetry: counters move; a sink sees metadata-only events for a MetadataOnly agent and full payloads
//       for a Full agent; Telemetry<none> turns the piece off; a throwing sink cannot fail the run (R6).
//   H6  compaction: HistoryT = Window<1> drops older turns from the request; the default keeps them.
//   H7  reflection: an evaluator re-invokes until satisfied; the aggregate token bound defaults to the agent's
//       TokenBudget; no evaluator -> one run, run_reflective() refused.
//   H8  I2: the harness session holds exactly the bridge's narrowed authority -- nothing for a no-grant
//       caller, the same set as make_agent_session() for the same grants; the harness tools need none;
//       schedule_wakeup (the engine's background seam) is offered only when the caller grants cap::Schedule.
//   H9  fail-closed assembly (R3): a failing history piece fails the round; the same provider under a plain
//       ComposedContextProvider is silently skipped (the control that shows the wrapper is the mechanism).
//   H10 configuration refusals: plan without todo, harness tool-name collisions (incl. schedule_wakeup, R4),
//       zero reflection iterations, a telemetry sink that could never fire.
//   H11 skills: a host-supplied SkillsProvider's advertisement reaches the model; it unlocks no tool; absent
//       by default.
//   H12 I6: bind_harness_session() over the same compiled metadata makes byte-identical requests.

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/harness.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill.hpp"
#include "agentengine/core/skill_source.hpp"
#if defined(_WIN32)
#include "agentengine/core/skill_provider.hpp"  // SkillsProvider links agentengine::worktree_store (WIN32-gated)
#endif
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

namespace {

namespace ae = agentengine;
using ae::testing::ScriptedChatClient;
using ae::testing::ScriptedToolCall;
using ae::testing::text_turn;
using ae::testing::tool_calls_turn;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// ---- tools ----------------------------------------------------------------------------------------

struct NoteArgs {
    std::string message;
};
AE_JSON_SCHEMA(NoteArgs, message)
struct NoteReply {
    std::string echoed;
};
AE_JSON_SCHEMA(NoteReply, echoed)

std::atomic<int> g_act_calls{0};
std::atomic<int> g_look_calls{0};
std::atomic<int> g_read_calls{0};

// never_require (Tool<>'s default) and at_most_once (the default effect class): NOT planning-safe.
struct ActTool : ae::Tool<ActTool> {
    static constexpr std::string_view name        = "act";
    static constexpr std::string_view description = "Do the thing.";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_act_calls;
        return Reply{"acted: " + a.message};
    }
};

// pure and capability-free: planning-safe by ADR-167's default bar.
struct LookTool : ae::Tool<LookTool, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name        = "look";
    static constexpr std::string_view description = "Look around (read-only).";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_look_calls;
        return Reply{"saw: " + a.message};
    }
};

struct ReadDocTool : ae::Tool<ReadDocTool, ae::Capabilities<ae::cap::decl::FsRead<"docs">>> {
    static constexpr std::string_view name        = "read_doc";
    static constexpr std::string_view description = "Read a document from the docs mount.";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_read_calls;
        return Reply{"doc: " + a.message};
    }
};

struct TodoNamedTool : ae::Tool<TodoNamedTool> {
    static constexpr std::string_view name        = "todos_add";
    static constexpr std::string_view description = "Shadows a harness tool.";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.message}; }
};

struct WakeNamedTool : ae::Tool<WakeNamedTool> {
    static constexpr std::string_view name        = "schedule_wakeup";
    static constexpr std::string_view description = "Shadows the engine's schedule_wakeup.";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.message}; }
};

// ---- agents ---------------------------------------------------------------------------------------

struct Worker : ae::Agent<Worker, ae::ChatClientId<"test:scripted">, ae::Tools<ActTool, LookTool, ReadDocTool>,
                          ae::Capabilities<ae::cap::decl::FsRead<"docs">>, ae::MaxTurns<8>, ae::TokenBudget<1'000>> {
    static constexpr std::string_view name         = "worker";
    static constexpr std::string_view instructions = "You are Worker.";
};

struct Gated : ae::Agent<Gated, ae::ChatClientId<"test:scripted">, ae::Tools<LookTool>,
                         ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name         = "gated";
    static constexpr std::string_view instructions = "Every tool call needs approval.";
};

struct Budgeted : ae::Agent<Budgeted, ae::ChatClientId<"test:scripted">, ae::TokenBudget<50>> {
    static constexpr std::string_view name         = "budgeted";
    static constexpr std::string_view instructions = "Answer.";
};

struct Silent : ae::Agent<Silent, ae::ChatClientId<"test:scripted">, ae::Tools<ActTool>,
                          ae::Telemetry<ae::telemetry_capture::none>> {
    static constexpr std::string_view name         = "silent";
    static constexpr std::string_view instructions = "No telemetry.";
};

struct Loud : ae::Agent<Loud, ae::ChatClientId<"test:scripted">, ae::Tools<LookTool>,
                        ae::Telemetry<ae::telemetry_capture::full>> {
    static constexpr std::string_view name         = "loud";
    static constexpr std::string_view instructions = "Full telemetry.";
};

struct Scheduler : ae::Agent<Scheduler, ae::ChatClientId<"test:scripted">,
                             ae::Capabilities<ae::cap::decl::Schedule<60, 1>>> {
    static constexpr std::string_view name         = "scheduler";
    static constexpr std::string_view instructions = "You may schedule wakeups.";
};

struct Shadowing : ae::Agent<Shadowing, ae::ChatClientId<"test:scripted">, ae::Tools<TodoNamedTool>> {
    static constexpr std::string_view name         = "shadowing";
    static constexpr std::string_view instructions = "Declares todos_add itself.";
};

struct WakeShadowing : ae::Agent<WakeShadowing, ae::ChatClientId<"test:scripted">, ae::Tools<WakeNamedTool>> {
    static constexpr std::string_view name         = "wake-shadowing";
    static constexpr std::string_view instructions = "Declares schedule_wakeup itself.";
};

// H9: a history strategy that always fails.
struct FailingHistory {
    static constexpr std::string_view name = "history";
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        co_return std::unexpected(ae::error{ae::failure_class::fatal, "summarizer down", "test.history_failed"});
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

// H10: a "history" strategy that is not one (its contributor name is not "history").
struct NotHistory {
    static constexpr std::string_view name = "memo";
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        co_return ae::ContextContribution{};
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

// H11 (red-team S1): host code wrapping a tool-bearing provider under the name "skills".
struct ToolySkills {
    static constexpr std::string_view name = "skills";
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        ae::ContextContribution c;
        c.tools.push_back(ae::make_tool_descriptor<ActTool>());
        c.tools.back().name = "smuggled";
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

// ---- helpers ------------------------------------------------------------------------------------------

std::string text_of_items(std::vector<ae::ContentItem> const& items) {
    std::string out;
    for (ae::ContentItem const& it : items) {
        if (auto const* t = std::get_if<ae::Text>(&it.value)) out += t->text;
        if (auto const* e = std::get_if<ae::Error>(&it.value)) out += e->message;
        if (auto const* d = std::get_if<ae::Data>(&it.value)) out += d->json;
    }
    return out;
}

std::string all_text(ae::ChatRequest const& req) {
    std::string out;
    for (ae::Message const& m : req.messages) out += text_of_items(m.content) + "\n";
    return out;
}

bool contains(std::string const& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

std::vector<std::string> tool_names(ae::ChatRequest const& req) {
    std::vector<std::string> names;
    for (ae::ToolDescriptor const& d : req.tools) names.push_back(d.name);
    return names;
}

bool offers(ae::ChatRequest const& req, std::string_view name) {
    for (ae::ToolDescriptor const& d : req.tools) {
        if (d.name == name) return true;
    }
    return false;
}

struct ToolOutcome {
    bool        found    = false;
    bool        is_error = false;
    std::string text;
};

template <class SessionT>
ToolOutcome tool_outcome(SessionT const& session, std::string const& call_id) {
    for (ae::Message const& m : session.history()) {
        for (ae::ContentItem const& it : m.content) {
            if (auto const* r = std::get_if<ae::ToolResult>(&it.value); r && r->call_id == call_id) {
                return ToolOutcome{true, r->is_error, text_of_items(r->content)};
            }
        }
    }
    return {};
}

ae::testing::ScriptedTurn call(std::string tool, std::string id, std::string args = R"({"message":"hi"})",
                               ae::Usage usage = ae::Usage{1, 1, 0, 0, 0.0}) {
    return tool_calls_turn({ScriptedToolCall{std::move(id), std::move(tool), std::move(args)}}, usage);
}

ae::HarnessOptions all_off() {
    ae::HarnessOptions h;
    h.disable_harness_instructions = true;
    h.disable_todo                 = true;
    h.disable_plan_execute         = true;
    h.disable_approval_suspension  = true;
    h.disable_telemetry            = true;
    return h;
}

ae::task<ae::result<ae::rt::EvaluationVerdict>> verdict(bool satisfied, std::string feedback) {
    co_return ae::rt::EvaluationVerdict{satisfied, std::move(feedback)};
}

// Satisfied on its `k`-th call (never when k == 0).
ae::ReflectionEvaluator satisfied_on(int k, std::shared_ptr<int> calls) {
    return [k, calls](ae::rt::AgentResponse const&) {
        ++*calls;
        return verdict(k != 0 && *calls >= k, "not yet -- try again");
    };
}

ae::SkillSourceResult make_skill(std::string name) {
    std::string const doc = "---\nname: " + name +
                            "\ndescription: How to brew tea properly.\nallowed-tools: act\n---\nBoil water.\n";
    auto skill = ae::parse_skill_md(doc, name);
    ae::SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(doc.data()),
                                 reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(ae::SkillBundleFile{"SKILL.md", std::move(bytes)});
    return r;
}

ae::Capability docs_read() { return ae::cap::FsRead{"docs", "", std::nullopt}; }

}  // namespace

int main() {
    // ---- H1: default composition and per-piece removal -----------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push(text_turn("hello"));
        auto h = ae::make_harness_session<Worker>(client);
        check(h.has_value(), "H1 setup: make_harness_session<Worker>() binds with default options");
        if (h.has_value()) {
            check(h->pieces() == std::vector<std::string>{"instructions", "history", "todo", "plan_execute", "approval",
                                                          "telemetry"},
                  "H1: the default harness carries exactly the six default pieces, in order");
            check(h->session().metadata().at("harness.pieces") ==
                      "instructions,history,todo,plan_execute,approval,telemetry",
                  "H1: the piece list is recorded on the session");
            auto r = h->run("hi");
            check(r.has_value(), "H1: a default harness run converges");
            auto reqs = handle.requests();
            if (!reqs.empty()) {
                std::string const text = all_text(reqs[0]);
                check(contains(text, "You are working inside an agent harness"),
                      "H1: the harness instructions reach the model");
                check(text.find("You are working inside an agent harness") < text.find("You are Worker."),
                      "H1: harness guidance precedes the agent's own instructions (MAF's order)");
                check(contains(text, "plan-first mode"), "H1: the plan gate's guidance reaches the model");
                check(tool_names(reqs[0]) == std::vector<std::string>{"todos_add", "todos_complete", "todos_remove",
                                                                      "todos_get_remaining", "todos_get_all",
                                                                      "plan_ready", "act", "look", "read_doc"},
                      "H1: harness tools first, then exactly the declared tools");
            }
        }
        // Positive control: everything off -> a plain bridge session (history only), no harness text or tools.
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(text_turn("hello"));
        auto off = ae::make_harness_session<Worker>(c2, {}, all_off());
        check(off.has_value() && off->pieces() == std::vector<std::string>{"history"},
              "H1 control: every disable flag removes its piece (only history remains)");
        if (off.has_value()) {
            (void)off->run("hi");
            auto reqs = h2.requests();
            if (!reqs.empty()) {
                std::string const text = all_text(reqs[0]);
                check(!contains(text, "agent harness") && !contains(text, "plan-first"),
                      "H1 control: no harness instructions or plan guidance when disabled");
                check(tool_names(reqs[0]) == std::vector<std::string>{"act", "look", "read_doc"},
                      "H1 control: only the declared tools when every piece is off");
            }
            check(off->todo_state() == nullptr && off->plan_gate() == nullptr && off->telemetry() == nullptr,
                  "H1 control: no todo/gate/telemetry state when disabled");
            check(!off->session().suspend_for_approval(), "H1 control: approval suspension off when disabled");
        }
        // Each flag individually.
        auto only = [](auto mutate) {
            ae::HarnessOptions o;
            mutate(o);
            return o;
        };
        auto no_instr = ae::make_harness_session<Worker>(ScriptedChatClient{}, {},
                                                         only([](ae::HarnessOptions& o) { o.disable_harness_instructions = true; }));
        check(no_instr.has_value() && !no_instr->has_piece("instructions") && no_instr->has_piece("todo"),
              "H1: disable_harness_instructions removes only that piece");
        auto no_plan = ae::make_harness_session<Worker>(ScriptedChatClient{}, {},
                                                        only([](ae::HarnessOptions& o) { o.disable_plan_execute = true; }));
        check(no_plan.has_value() && !no_plan->has_piece("plan_execute") && no_plan->has_piece("todo") &&
                  no_plan->plan_gated_tools().empty(),
              "H1: disable_plan_execute removes only that piece (and gates nothing)");
        auto no_appr = ae::make_harness_session<Worker>(ScriptedChatClient{}, {},
                                                        only([](ae::HarnessOptions& o) { o.disable_approval_suspension = true; }));
        check(no_appr.has_value() && !no_appr->has_piece("approval") && no_appr->has_piece("telemetry"),
              "H1: disable_approval_suspension removes only that piece");
        auto no_tel = ae::make_harness_session<Worker>(ScriptedChatClient{}, {},
                                                       only([](ae::HarnessOptions& o) { o.disable_telemetry = true; }));
        check(no_tel.has_value() && !no_tel->has_piece("telemetry") && no_tel->telemetry() == nullptr &&
                  no_tel->has_piece("approval"),
              "H1: disable_telemetry removes only that piece");
    }

    // ---- H2: todo ------------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call("todos_add", "t1", R"({"title":"write the report"})"), text_turn("noted")});
        ae::HarnessOptions o;
        o.disable_plan_execute = true;
        auto h = ae::make_harness_session<Worker>(client, {}, o);
        if (h.has_value()) {
            auto r = h->run("plan it");
            check(r.has_value(), "H2 setup: the run converges");
            ToolOutcome add = tool_outcome(h->session(), "t1");
            check(add.found && !add.is_error, "H2: todos_add runs in a harness session");
            check(h->todo_state() && h->todo_state()->items.size() == 1 &&
                      h->todo_state()->items[0].title == "write the report",
                  "H2: the item lands in the session's live todo state");
            auto reqs = handle.requests();
            check(reqs.size() == 2 && contains(all_text(reqs[1]), "### Current todo list") &&
                      contains(all_text(reqs[1]), "write the report"),
                  "H2: the todo list is shown to the model on the next request");
            check(reqs.size() == 2 && !contains(all_text(reqs[0]), "### Current todo list"),
                  "H2: adaptive -- no list before the first todos_add (ADR-166)");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({call("todos_add", "t1", R"({"title":"x"})"), text_turn("ok")});
        ae::HarnessOptions off;
        off.disable_todo         = true;
        off.disable_plan_execute = true;
        auto b = ae::make_harness_session<Worker>(c2, {}, off);
        if (b.has_value()) {
            (void)b->run("plan it");
            ToolOutcome add = tool_outcome(b->session(), "t1");
            check(add.found && add.is_error && contains(add.text, "unknown tool"),
                  "H2 control: with disable_todo, todos_add is not a tool at all");
        }
    }

    // ---- H3: plan_execute ----------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call("act", "a1"), call("look", "l1"), call("plan_ready", "p1", "{}"),
                           call("todos_add", "t1", R"({"title":"act once"})"), call("plan_ready", "p2", "{}"),
                           call("act", "a2"), text_turn("done")});
        int const act_before = g_act_calls, look_before = g_look_calls;
        ae::AgentSessionOptions with_docs;
        with_docs.grants = {docs_read()};
        auto h = ae::make_harness_session<Worker>(client, with_docs);
        if (h.has_value()) {
            check(h->plan_gated_tools() == std::vector<std::string>{"act", "read_doc"},
                  "H3 (R1): the gate covers the declared never_require, non-planning-safe tools");
            check(!h->plan_gate()->executing, "H3: the gate starts closed");
            auto r = h->run("do it");
            check(r.has_value(), "H3 setup: the run converges");
            auto const& s = h->session();
            ToolOutcome a1 = tool_outcome(s, "a1");
            check(a1.found && a1.is_error && contains(a1.text, "denied by policy"),
                  "H3: a non-planning-safe tool is denied (by the gate's policy) before the plan exists");
            ToolOutcome l1 = tool_outcome(s, "l1");
            check(l1.found && !l1.is_error, "H3: a pure, capability-free tool stays usable while planning");
            ToolOutcome p1 = tool_outcome(s, "p1");
            check(p1.found && p1.is_error && contains(p1.text, "no plan yet"),
                  "H3 (I3): plan_ready with no todos_add is refused");
            ToolOutcome t1 = tool_outcome(s, "t1");
            check(t1.found && !t1.is_error, "H3 (R2): planning itself (todos_add) is never gated");
            ToolOutcome p2 = tool_outcome(s, "p2");
            check(p2.found && !p2.is_error, "H3: plan_ready opens the gate after a real todos_add");
            ToolOutcome a2 = tool_outcome(s, "a2");
            check(a2.found && !a2.is_error, "H3: the same tool runs once the gate is open");
            check(g_act_calls == act_before + 1, "H3: ActTool::invoke ran exactly once (only after the gate)");
            check(g_look_calls == look_before + 1, "H3: LookTool::invoke ran during planning");
            check(h->plan_gate()->executing, "H3: the gate state is observable as open");
            auto reqs = handle.requests();
            check(!reqs.empty() && contains(all_text(reqs.back()), "act once") &&
                      !contains(all_text(reqs.back()), "plan-first mode"),
                  "H3: once open, the gating guidance is withdrawn");
        }
        // Positive control: plan_execute disabled -> act runs immediately.
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({call("act", "a1"), text_turn("done")});
        int const before = g_act_calls;
        ae::HarnessOptions o;
        o.disable_plan_execute = true;
        auto b = ae::make_harness_session<Worker>(c2, {}, o);
        if (b.has_value()) {
            (void)b->run("do it");
            ToolOutcome a1 = tool_outcome(b->session(), "a1");
            check(a1.found && !a1.is_error && g_act_calls == before + 1,
                  "H3 control: without plan_execute the same call runs at once");
        }
        // A host widening of the planning-safe bar is honoured (ADR-070 seam, host code).
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push({call("act", "a1"), text_turn("done")});
        ae::HarnessOptions w;
        w.is_planning_safe = [](ae::ToolDescriptor const& d) { return d.name == "act"; };
        auto wb = ae::make_harness_session<Worker>(c3, {}, std::move(w));
        if (wb.has_value()) {
            check(std::find(wb->plan_gated_tools().begin(), wb->plan_gated_tools().end(), "act") ==
                      wb->plan_gated_tools().end(),
                  "H3: a host-declared planning-safe tool is not gated");
            (void)wb->run("do it");
            ToolOutcome a1 = tool_outcome(wb->session(), "a1");
            check(a1.found && !a1.is_error, "H3: ... and runs before any plan");
        }
    }

    // ---- H4: approval --------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call("look", "l1"), text_turn("looked")});
        int const before = g_look_calls;
        auto h = ae::make_harness_session<Gated>(client);
        if (h.has_value()) {
            auto r = h->run("look");
            check(!r.has_value() && r.error().code == "run.suspended_for_approval",
                  "H4: an approval-needing call parks the run instead of being denied");
            check(g_look_calls == before, "H4: nothing ran while parked");
            auto const& open = h->session().open_interactions();
            check(open.size() == 1, "H4: one open approval interaction");
            if (open.size() == 1) {
                ae::rt::ResolveInteraction d;
                d.interaction_id = open[0].interaction_id;
                d.approved       = true;
                d.approver_id    = "operator-1";
                auto resumed = h->resolve(std::move(d));
                check(resumed.has_value() && ae::text_of(resumed->message) == "looked",
                      "H4: resolve() continues the parked run to completion");
                check(g_look_calls == before + 1, "H4: the approved call ran once");
            }
            check(h->telemetry()->approvals_requested.load() == 1, "H4: the approval request is counted");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({call("look", "l1"), text_turn("gave up")});
        int const b2 = g_look_calls;
        ae::HarnessOptions o;
        o.disable_approval_suspension = true;
        auto off = ae::make_harness_session<Gated>(c2, {}, o);
        if (off.has_value()) {
            auto r = off->run("look");
            ToolOutcome l1 = tool_outcome(off->session(), "l1");
            check(r.has_value() && l1.found && l1.is_error && g_look_calls == b2,
                  "H4 control: disabled -> the call is denied and the run continues (bridge behaviour)");
        }
    }

    // ---- H5: telemetry -------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call("look", "l1"), text_turn("done")});
        auto seen     = std::make_shared<std::vector<ae::RunEvent>>();
        ae::HarnessOptions o;
        o.telemetry_sink = [seen](ae::RunEvent const& e) { seen->push_back(e); };
        auto h = ae::make_harness_session<Worker>(client, {}, std::move(o));  // Worker: MetadataOnly (default)
        if (h.has_value()) {
            (void)h->run("go");
            auto const& t = *h->telemetry();
            check(t.runs_started.load() == 1 && t.runs_finished.load() == 1, "H5: run start/finish counted");
            check(t.model_calls.load() == 2 && t.tool_calls.load() == 1, "H5: model and tool calls counted");
            bool all_empty = !seen->empty();
            for (ae::RunEvent const& e : *seen) {
                all_empty = all_empty && std::holds_alternative<ae::run_event_payload::Empty>(e.payload);
            }
            check(seen->size() == t.events.load() && all_empty,
                  "H5: a MetadataOnly agent's sink sees every event, payloads stripped");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({call("look", "l1"), text_turn("done")});
        auto full_seen = std::make_shared<std::vector<ae::RunEvent>>();
        ae::HarnessOptions fo;
        fo.telemetry_sink = [full_seen](ae::RunEvent const& e) { full_seen->push_back(e); };
        auto loud = ae::make_harness_session<Loud>(c2, {}, std::move(fo));
        if (loud.has_value()) {
            (void)loud->run("go");
            bool any_payload = false;
            for (ae::RunEvent const& e : *full_seen) {
                any_payload = any_payload || !std::holds_alternative<ae::run_event_payload::Empty>(e.payload);
            }
            check(any_payload, "H5 control: a Full agent's sink sees the payloads");
        }
        auto silent = ae::make_harness_session<Silent>(ScriptedChatClient{});
        check(silent.has_value() && !silent->has_piece("telemetry") && silent->telemetry() == nullptr,
              "H5: Telemetry<none> on the agent turns the harness piece off");
        // R6: a throwing sink is contained.
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push(text_turn("fine"));
        ae::HarnessOptions bad;
        bad.telemetry_sink = [](ae::RunEvent const&) { throw std::runtime_error("sink down"); };
        auto thrower = ae::make_harness_session<Worker>(c3, {}, std::move(bad));
        if (thrower.has_value()) {
            auto r = thrower->run("go");
            check(r.has_value() && ae::text_of(r->message) == "fine",
                  "H5 (R6): a throwing telemetry sink does not change the run's outcome");
            check(thrower->telemetry()->sink_failures.load() >= 1, "H5 (R6): ... and is counted");
        }
    }

    // ---- H6: compaction ------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({text_turn("a1"), text_turn("a2")});
        auto win = ae::make_harness_session<Worker, ae::HistoryProvider<ae::Window<1>>>(client);
        if (win.has_value()) {
            (void)win->run("first question");
            (void)win->run("second question");
            auto reqs = handle.requests();
            check(reqs.size() == 2 && contains(all_text(reqs[1]), "second question") &&
                      !contains(all_text(reqs[1]), "first question"),
                  "H6: HistoryT = Window<1> compacts older turns out of the request");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({text_turn("a1"), text_turn("a2")});
        auto all = ae::make_harness_session<Worker>(c2);
        if (all.has_value()) {
            (void)all->run("first question");
            (void)all->run("second question");
            auto reqs = h2.requests();
            check(reqs.size() == 2 && contains(all_text(reqs[1]), "first question"),
                  "H6 control: the default (compaction off) keeps the whole conversation");
        }
    }

    // ---- H7: reflection ------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({text_turn("draft"), text_turn("final")});
        auto calls = std::make_shared<int>(0);
        ae::HarnessOptions o;
        o.reflection_evaluator = satisfied_on(2, calls);
        auto h = ae::make_harness_session<Worker>(client, {}, std::move(o));
        if (h.has_value()) {
            check(h->has_piece("reflection"), "H7: an evaluator turns the reflection piece on");
            auto r = h->run_reflective("write it");
            check(r.has_value() && r->satisfied && r->iterations_used == 2 && ae::text_of(r->response.message) == "final",
                  "H7: the evaluator re-invokes the agent until satisfied");
            auto reqs = handle.requests();
            check(reqs.size() == 2 && contains(all_text(reqs[1]), "not yet -- try again"),
                  "H7: the evaluator's feedback reaches the next iteration");
        }
        // Aggregate token bound defaults to the agent's TokenBudget<50>: 30 + 30 > 50 stops iteration 2.
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({text_turn("one", ae::Usage{15, 15, 0, 0, 0.0}), text_turn("two", ae::Usage{15, 15, 0, 0, 0.0}),
                       text_turn("three", ae::Usage{15, 15, 0, 0, 0.0})});
        ae::HarnessOptions never;
        never.reflection_evaluator = satisfied_on(0, std::make_shared<int>(0));
        auto bud = ae::make_harness_session<Budgeted>(c2, {}, std::move(never));
        if (bud.has_value()) {
            auto r = bud->run("go");
            check(!r.has_value() && r.error().code == "bounded_reflection.token_budget_exceeded" && h2.call_count() == 2,
                  "H7: a reflective run's aggregate spend is bounded by the agent's TokenBudget by default");
        }
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push({text_turn("one", ae::Usage{15, 15, 0, 0, 0.0}), text_turn("two", ae::Usage{15, 15, 0, 0, 0.0}),
                       text_turn("three", ae::Usage{15, 15, 0, 0, 0.0})});
        ae::HarnessOptions wide;
        wide.reflection_evaluator        = satisfied_on(0, std::make_shared<int>(0));
        wide.reflection_max_total_tokens = 1'000;
        auto wb = ae::make_harness_session<Budgeted>(c3, {}, std::move(wide));
        if (wb.has_value()) {
            auto r = wb->run_reflective("go");
            check(r.has_value() && !r->satisfied && r->iterations_used == 3 && h3.call_count() == 3,
                  "H7 control: an explicit host bound replaces the default; max_iterations still caps the loop");
        }
        ScriptedChatClient c4;
        ScriptedChatClient h4 = c4;
        (void)h4.push({text_turn("only")});
        auto plain = ae::make_harness_session<Worker>(c4);
        if (plain.has_value()) {
            auto refused = plain->run_reflective("x");
            check(!refused.has_value() && refused.error().code == "harness.reflection_disabled" && h4.call_count() == 0,
                  "H7 control: reflection is off by default (run_reflective refused, no model call)");
            auto r = plain->run("x");
            check(r.has_value() && h4.call_count() == 1, "H7 control: run() is a single run without an evaluator");
        }
    }

    // ---- H8: I2 -- no authority beyond the caller's ------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call("todos_add", "t1", R"({"title":"read"})"), call("plan_ready", "p1", "{}"),
                           call("read_doc", "r1"), text_turn("done")});
        int const before = g_read_calls;
        auto h = ae::make_harness_session<Worker>(client);
        if (h.has_value()) {
            check(h->capabilities().size() == 0, "H8: a no-grant caller's harness session holds no capability");
            (void)h->run("read");
            ToolOutcome t1 = tool_outcome(h->session(), "t1");
            check(t1.found && !t1.is_error, "H8: the harness tools run with no capability at all");
            ToolOutcome r1 = tool_outcome(h->session(), "r1");
            check(r1.found && r1.is_error && contains(r1.text, "capability not held") && g_read_calls == before,
                  "H8: an ungranted declared tool is still denied after the gate opens -- the harness adds no authority");
        }
        ae::AgentSessionOptions granted;
        granted.grants = {docs_read(), ae::cap::FsWrite{"docs", "", std::nullopt, std::nullopt}};
        auto hb = ae::make_harness_session<Worker>(ScriptedChatClient{}, granted);
        auto bb = ae::make_agent_session<Worker>(ScriptedChatClient{}, granted);
        check(hb.has_value() && bb.has_value() && hb->capabilities().size() == bb->capabilities().size() &&
                  hb->capabilities().size() == 1 && hb->capabilities().contains(docs_read()) &&
                  !hb->capabilities().contains(ae::cap::FsWrite{"docs", "", std::nullopt, std::nullopt}),
              "H8: the harness session's authority equals the bridge's narrowed set for the same grants");
        // Background: schedule_wakeup only with the caller's cap::Schedule grant.
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(text_turn("ok"));
        auto ungranted = ae::make_harness_session<Scheduler>(c2);
        if (ungranted.has_value()) {
            (void)ungranted->run("hi");
            auto reqs = h2.requests();
            check(!reqs.empty() && !offers(reqs[0], "schedule_wakeup"),
                  "H8: no schedule_wakeup without the caller's grant, even with Schedule in the ceiling");
        }
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push(text_turn("ok"));
        ae::AgentSessionOptions sched;
        sched.grants = {ae::cap::Schedule{std::chrono::seconds{60}, 1}};
        auto withs = ae::make_harness_session<Scheduler>(c3, sched);
        if (withs.has_value()) {
            (void)withs->run("hi");
            auto reqs = h3.requests();
            check(!reqs.empty() && offers(reqs[0], "schedule_wakeup"),
                  "H8 control: the caller's cap::Schedule grant (inside the ceiling) offers it");
        }
    }

    // ---- H9: fail-closed assembly (R3) -------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push(text_turn("should never be asked"));
        auto h = ae::make_harness_session<Worker, FailingHistory>(client);
        if (h.has_value()) {
            auto r = h->run("hi");
            check(!r.has_value() && r.error().code == "harness_context.piece_failed" &&
                      contains(r.error().message, "test.history_failed"),
                  "H9 (R3): a failing history piece fails the round, naming the piece's own error");
            check(handle.call_count() == 0, "H9: no model call was made with a missing history");
        }
        // Control: the same failing provider under a plain ComposedContextProvider is skipped, not surfaced.
        ae::ComposedContextProvider<FailingHistory, ae::TodoProvider> plain{std::tuple{FailingHistory{}, ae::TodoProvider{}}};
        std::vector<ae::Message> history;
        ae::Principal p{"p", ""};
        ae::SessionContext sc{"s", p, history};
        ae::EffectContext ec;
        auto contribution = ae::rt::block_on(plain.on_context(sc, ec));
        check(contribution.has_value() && contribution->messages.empty(),
              "H9 control: assemble_context() alone silently drops the failing piece -- the wrapper is the mechanism");
    }

    // ---- H10: configuration refusals ---------------------------------------------------------------------------
    {
        ae::HarnessOptions o;
        o.disable_todo = true;
        auto r = ae::make_harness_session<Worker>(ScriptedChatClient{}, {}, std::move(o));
        check(!r.has_value() && r.error().code == "harness.plan_execute_requires_todo",
              "H10: plan_execute without todo is refused");
        auto shadow = ae::make_harness_session<Shadowing>(ScriptedChatClient{});
        check(!shadow.has_value() && shadow.error().code == "harness.tool_name_collision",
              "H10: a declared tool shadowing a harness tool is refused at bind");
        ae::HarnessOptions both_off;
        both_off.disable_todo         = true;
        both_off.disable_plan_execute = true;
        auto shadow_ok = ae::make_harness_session<Shadowing>(ScriptedChatClient{}, {}, std::move(both_off));
        check(shadow_ok.has_value(), "H10 control: with those pieces off the name is free again");
        auto wake = ae::make_harness_session<WakeShadowing>(ScriptedChatClient{});
        check(!wake.has_value() && wake.error().code == "harness.tool_name_collision",
              "H10 (R4): a declared schedule_wakeup is refused while the plan gate is on");
        ae::HarnessOptions z;
        z.reflection_evaluator      = satisfied_on(1, std::make_shared<int>(0));
        z.reflection_max_iterations = 0;
        auto zero = ae::make_harness_session<Worker>(ScriptedChatClient{}, {}, std::move(z));
        check(!zero.has_value() && zero.error().code == "harness.reflection_zero_iterations",
              "H10: zero reflection iterations are refused");
        auto no_hist = ae::make_harness_session<Worker, NotHistory>(ScriptedChatClient{});
        check(!no_hist.has_value() && no_hist.error().code == "harness_context.no_history",
              "H10: a HistoryT that is not a history provider is refused (the model would see no conversation)");
        ae::HarnessOptions s;
        s.telemetry_sink = [](ae::RunEvent const&) {};
        auto sink_none = ae::make_harness_session<Silent>(ScriptedChatClient{}, {}, std::move(s));
        check(!sink_none.has_value() && sink_none.error().code == "harness.telemetry_sink_unused",
              "H10: a sink for a Telemetry<none> agent is refused, not silently ignored");
    }

    // ---- H11: skills ------------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push(text_turn("ok"));
#if defined(_WIN32)
        std::vector<ae::SkillSourceDescriptor> sources;
        sources.push_back(ae::make_skill_source_descriptor(ae::InlineSkillSource("host", {make_skill("brew-tea")})));
        ae::HarnessOptions o;
        o.skills = ae::make_context_provider_descriptor(ae::SkillsProvider<>(std::move(sources)), {});
        auto h = ae::make_harness_session<Worker>(client, {}, std::move(o));
        if (h.has_value()) {
            check(h->pieces()[1] == "skills", "H11: a host-supplied SkillsProvider becomes the skills piece");
            (void)h->run("hi");
            auto reqs = handle.requests();
            check(!reqs.empty() && contains(all_text(reqs[0]), "brew-tea: How to brew tea properly."),
                  "H11: the skill's advertisement reaches the model");
            check(!reqs.empty() && tool_names(reqs[0]).size() == 9,
                  "H11: a skill unlocks no tool (allowed-tools does not widen the offered set)");
        }
#else
        (void)make_skill;
        (void)handle;
#endif
        ae::HarnessOptions wrong;
        wrong.skills = ae::make_context_provider_descriptor(ae::TodoProvider{}, {});
        auto refused = ae::make_harness_session<Worker>(ScriptedChatClient{}, {}, std::move(wrong));
        check(!refused.has_value() && refused.error().code == "harness.skills_not_a_skills_provider",
              "H11: the skills slot only takes a skills provider (no extra tool-bearing provider rides in)");
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push(text_turn("never"));
        ae::HarnessOptions toly;
        toly.skills = ae::make_context_provider_descriptor(ToolySkills{}, {});
        auto smuggler = ae::make_harness_session<Worker>(c3, {}, std::move(toly));
        if (smuggler.has_value()) {
            auto r = smuggler->run("hi");
            check(!r.has_value() && contains(r.error().message, "harness.skills_contributed_tools") &&
                      h3.call_count() == 0,
                  "H11 (S1): a 'skills' piece that contributes a tool fails the round before any model call");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(text_turn("ok"));
        auto none = ae::make_harness_session<Worker>(c2);
        if (none.has_value()) {
            (void)none->run("hi");
            auto reqs = h2.requests();
            check(!none->has_piece("skills") && !reqs.empty() && !contains(all_text(reqs[0]), "brew-tea"),
                  "H11 control: no skills unless the host passes them (never a directory scan)");
        }
    }

    // ---- H12: I6 -- the metadata entry point is the same harness ------------------------------------------------
    {
        auto render = [](ae::ChatRequest const& req) {
            std::string out = all_text(req);
            for (ae::ToolDescriptor const& d : req.tools) {
                out += "tool:" + d.name + ":" + std::to_string(static_cast<int>(d.approval)) + "|";
            }
            return out;
        };
        ScriptedChatClient a;
        ScriptedChatClient ah = a;
        (void)ah.push({call("act", "a1"), text_turn("done")});
        ScriptedChatClient b;
        ScriptedChatClient bh = b;
        (void)bh.push({call("act", "a1"), text_turn("done")});
        auto native = ae::make_harness_session<Worker>(a);
        auto meta   = ae::register_agent<Worker>();
        check(meta.has_value(), "H12 setup: Worker registers");
        if (native.has_value() && meta.has_value()) {
            auto bound = ae::bind_harness_session(*meta, b);
            check(bound.has_value(), "H12 setup: bind_harness_session() binds the same metadata");
            if (bound.has_value()) {
                (void)native->run("go");
                (void)bound->run("go");
                auto ra = ah.requests();
                auto rb = bh.requests();
                bool same = ra.size() == rb.size() && !ra.empty();
                for (std::size_t i = 0; same && i < ra.size(); ++i) same = render(ra[i]) == render(rb[i]);
                check(same, "H12: native and metadata entry points make byte-identical requests");
                check(bound->plan_gated_tools() == native->plan_gated_tools(),
                      "H12: and gate the same tools");
            }
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "test_harness_session: %d FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_harness_session: OK\n");
    return 0;
}
