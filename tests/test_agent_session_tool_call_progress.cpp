// Proof for ADR-060 -- giving EffectContext a real, call-scoped report_progress() channel that
// pushes a run_event_kind::tool_call_delta (run_event.hpp) through the session's own already-real
// emit_run_event()/enable_event_stream() machinery. Modeled directly on
// test_rt_agent_session_suspend_approval.cpp's deterministic, offline, scripted-ChatClient shape (P1-
// P3) plus test_rt_agent_session_background_task.cpp's direct-tool_pipeline-call shape (D), rather
// than inventing a third style.
//
//   P1 -- a tool that calls ctx.report_progress(...) during invoke() causes a real tool_call_delta
//         event, carrying the exact call_id of the in-flight call and the exact text reported, to
//         appear on enable_event_stream()'s output.
//   P2 (positive control) -- a tool that never calls it produces no tool_call_delta event at all --
//         the ordinary path is unaffected by this field's mere existence.
//   P3 -- two sequential tool calls in the SAME round each get their own correctly-tagged call_id on
//         the progress they report -- proves the per-call bracket (set immediately before, reset
//         immediately after, at each of the three real invoke_tool() call sites in
//         rt/agent_session.hpp) never leaks a stale binding from one call into the next.
//   D  -- ADR-060 §4's own must-fix red-team finding, now closed structurally in
//         tool_pipeline.hpp::background_task(): a caller's EffectContext carrying a LIVE, bound
//         report_progress closure (standing in for whatever a genuinely racing bracket window could
//         copy into start_background_task()'s "PLAIN, UNLOCKED" call, per rt/agent_session.hpp's own
//         file-banner disclaimer) must never reach a Backgroundable tool's invoke() running on
//         background_task()'s own detached std::thread -- proven directly against
//         agentengine::background_task() itself, the one function that ever detaches that thread,
//         rather than trying to reproduce a genuine data race through AgentSession's own public
//         surface (which has no way to observe or force the exact interleaving from a test).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"

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

// Same "safe because nothing here genuinely suspends on an external wake" drive<T>(), copied per
// test_rt_agent_session_suspend_approval.cpp's own "no cross-test-file coupling" precedent.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return p();
}

using agentengine::CapabilitySet;
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
using agentengine::call_provenance;
using agentengine::RunEvent;
using agentengine::run_event_kind;
using agentengine::Principal;

// -- Tool fixtures ------------------------------------------------------------------------------

struct ProgressArgs { bool noop = false; };
AE_JSON_SCHEMA(ProgressArgs, noop)
struct ProgressReply { bool ok = false; };
AE_JSON_SCHEMA(ProgressReply, ok)

// Reports progress exactly once, then succeeds. Not Backgroundable -- exercises the ordinary
// invoke_tool() bracket at the three real rt/agent_session.hpp call sites.
struct ProgressTool : agentengine::Tool<ProgressTool> {
    static constexpr std::string_view name        = "progress_tool";
    static constexpr std::string_view description = "Reports progress once during invoke().";
    using Args  = ProgressArgs;
    using Reply = ProgressReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        ctx.report_progress(agentengine::ContentItem{agentengine::Text{"half done"}});
        return Reply{true};
    }
};

// Never reports progress -- the positive control (P2).
struct SilentTool : agentengine::Tool<SilentTool> {
    static constexpr std::string_view name        = "silent_tool";
    static constexpr std::string_view description = "Never calls report_progress().";
    using Args  = ProgressArgs;
    using Reply = ProgressReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

// Backgroundable AND reports progress from inside invoke() -- the tool D's own regression check
// runs on tool_pipeline.hpp::background_task()'s genuinely detached std::thread.
struct BackgroundableProgressTool : agentengine::Tool<BackgroundableProgressTool, agentengine::Backgroundable> {
    static constexpr std::string_view name        = "bg_progress_tool";
    static constexpr std::string_view description = "Backgroundable; also calls report_progress().";
    using Args  = ProgressArgs;
    using Reply = ProgressReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        ctx.report_progress(agentengine::ContentItem{
            agentengine::Text{"should never be observed by the caller's own closure"}});
        return Reply{true};
    }
};

// -- HistoryProviderT fixture: history passthrough + the two P1-P3 tools --------------------------

class ProgressHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<ProgressTool, SilentTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<ProgressHistoryProvider>);

// -- The scripted backend, copied from test_rt_agent_session_suspend_approval.cpp's own conventions --

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
        return {};  // unused
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    agentengine::ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    agentengine::ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall call;
    call.call_id       = std::move(call_id);
    call.tool_name      = std::move(tool_name);
    call.arguments_json = std::move(args);
    call.provenance      = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

// Two tool calls inside ONE round -- P3's own fixture, distinct from tool_call_response() above.
Message two_tool_call_response(std::string call_id_1, std::string call_id_2, std::string tool_name,
                                std::string args) {
    Message m;
    m.role = role::assistant;
    for (std::string const& call_id : {call_id_1, call_id_2}) {
        agentengine::ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall call;
        call.call_id        = call_id;
        call.tool_name       = tool_name;
        call.arguments_json  = args;
        call.provenance       = call_provenance::vendor_structured;
        item.value = call;
        m.content.push_back(item);
    }
    return m;
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    agentengine::ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

using Session = AgentSession<ScriptedChatClient, NoSessionState, ProgressHistoryProvider>;

}  // namespace

int main() {
    namespace json = agentengine::json;

    // ---- P1: a real tool_call_delta event, correct call_id and text ------------------------------
    {
        Session session;
        session.initialize("p1", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "progress_tool", R"({"noop":true})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "P1: the run converges normally");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));

        int         delta_count = 0;
        std::string delta_call_id;
        std::string delta_text;
        for (RunEvent const& ev : events) {
            if (ev.kind == run_event_kind::tool_call_delta) {
                ++delta_count;
                if (auto const* p = std::get_if<agentengine::run_event_payload::ToolCallDelta>(&ev.payload)) {
                    delta_call_id = p->call_id;
                    if (auto const* t = std::get_if<Text>(&p->content.value)) delta_text = t->text;
                }
            }
        }
        check(delta_count == 1,
              "P1: exactly one tool_call_delta event fired for the one call that reported progress");
        check(delta_call_id == "c1", "P1: the event carries the real call_id of the in-flight call");
        check(delta_text == "half done", "P1: the event carries the exact text the tool reported");
    }

    // ---- P2 (positive control): a silent tool produces no tool_call_delta event ------------------
    {
        Session session;
        session.initialize("p2", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response("c1", "silent_tool", R"({"noop":true})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "P2: the run converges normally");

        bool saw_delta = false;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::tool_call_delta) saw_delta = true;
        }
        check(!saw_delta,
              "P2 (positive control): a tool that never calls report_progress() produces no "
              "tool_call_delta event -- the ordinary path is unaffected by this field's existence");
    }

    // ---- P3: two sequential calls in ONE round each get their OWN correctly-tagged call_id --------
    {
        Session session;
        session.initialize("p3", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {two_tool_call_response("c1", "c2", "progress_tool", R"({"noop":true})"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "P3: the run converges normally");

        std::vector<std::string> delta_call_ids;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::tool_call_delta) {
                if (auto const* p = std::get_if<agentengine::run_event_payload::ToolCallDelta>(&ev->payload)) {
                    delta_call_ids.push_back(p->call_id);
                }
            }
        }
        check(delta_call_ids.size() == 2,
              "P3: both sequential calls in the same round each reported progress -- two distinct "
              "events, no dropped or merged binding");
        check(std::count(delta_call_ids.begin(), delta_call_ids.end(), std::string("c1")) == 1,
              "P3: the first call's event is tagged with its OWN call_id (c1)");
        check(std::count(delta_call_ids.begin(), delta_call_ids.end(), std::string("c2")) == 1,
              "P3: the second call's event is tagged with its OWN call_id (c2), not a stale binding "
              "leaked from the first call's own bracket");
    }

    // ---- D: ADR-060 §4's must-fix finding, closed structurally in background_task() --------------
    // Direct call against agentengine::background_task() (tool_pipeline.hpp) -- the one function that
    // ever detaches the std::thread a Backgroundable tool's invoke() runs on. `ctx` below carries a
    // LIVE, bound report_progress closure, standing in for whatever a genuinely racing bracket window
    // (opened by one of the three real invoke_tool() call sites, on a different thread of control,
    // while AgentSession::start_background_task()'s own "PLAIN, UNLOCKED" entry point races it -- see
    // that method's file-banner comment in rt/agent_session.hpp) could copy into the by-value
    // EffectContext this function receives. If the backgrounded tool's call to ctx.report_progress()
    // (an ordinary, intended use of the very feature ADR-060 adds -- no tool misuse needed) ever
    // reached THIS closure from the detached thread, it would call back into whatever `this` it
    // captured and mutate agent_session.hpp's own unlocked run_event_seq_by_run_ map from off-thread.
    {
        std::atomic<int> fired{0};
        EffectContext ctx;
        ctx.principal    = Principal{"p", ""};
        ctx.run_id       = "bg-race-run";
        ctx.report_progress = [&fired](agentengine::ContentItem) { fired.fetch_add(1); };

        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::Background{1}});
        ctx.capabilities = agentengine::borrow_capabilities(held);

        auto const table = agentengine::ToolTable::from_tools<BackgroundableProgressTool>();
        agentengine::ToolCallRequest const req{"call-bg", "bg_progress_tool",
                                                *json::parse(R"({"noop":true})"), false};

        std::atomic<bool> completed{false};
        auto submitted = agentengine::background_task(
            table, held, req, ctx, agentengine::ApprovalDecider{}, /*current_background_count=*/0,
            [&completed](agentengine::ToolResult, agentengine::ToolInvocationAudit) {
                completed.store(true);
            });
        check(submitted.has_value(), "D setup: the Backgroundable tool call is accepted");

        check(wait_until([&] { return completed.load(); }, std::chrono::seconds(2)),
              "D setup: the detached thread actually finishes");

        check(fired.load() == 0,
              "D: the backgrounded tool DID call ctx.report_progress() from its own detached thread, "
              "but background_task()'s own structural reset means the CALLER's closure -- captured "
              "into this function's by-value EffectContext copy -- never fires; the cross-thread "
              "hazard ADR-060 par.4 found is closed at its one reachable choke point, not merely "
              "documented");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_agent_session_tool_call_progress: ALL PASS\n");
    return 0;
}
