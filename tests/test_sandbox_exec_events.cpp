// Proof for decisions/ADR-170-sandbox-exec-events.md (GitHub issue #64): `run_event_kind::
// sandbox_exec_started`/`sandbox_exec_finished` -- declared in `core/run_event.hpp`, listed
// normatively in 013 §1, and already projected to AG-UI `ActivitySnapshot`
// (`protocol/agui/projection.hpp`) -- had **no producer anywhere in the tree**. The only construction
// site was a synthetic event in `tests/test_rt_agui_projection.cpp`, built purely to exercise the
// projection.
//
// Issue #64 proposed emitting them inside `AgentSession`, around `SandboxBackend::create()/exec()`.
// That call site does not exist: `AgentSession` never touches `SandboxBackend`. Every real sandbox
// execution happens BENEATH an opaque `Tool<>::invoke()`. What those call sites do have is an
// `EffectContext&`, so ADR-170 adds `EffectContext::sandbox_exec_sink` plus the `SandboxExecScope`
// bracket, wired at the same three `invoke_tool()` sites `report_progress` (ADR-060) already uses.
//
//   S1 -- SandboxExecScope emits a started event at construction and a finished event at scope exit,
//         carrying the exec_id/backend/stage it was given.
//   S2 -- FAIL-CLOSED DEFAULT: a scope destroyed without succeeded()/failed() -- an early return
//         between provisioning and completion, which the real PDF producer has four of -- reports
//         `ok == false`, `error_code == "sandbox.exec.abandoned"`, never a success that never
//         happened and never a start with no end.
//   S3 -- failed(code) carries that exact code; succeeded() clears it.
//   S4 -- END TO END through a real `AgentSession` + `enable_event_stream()`: a tool that opens a
//         scope during invoke() produces real events on the session's own stream. This is the claim
//         issue #64's step 3 asks for -- the events fire through the real emission machinery, not
//         only in a projection unit test.
//   S5 (positive control) -- a tool that never touches the sink produces NO sandbox events; the
//         ordinary path is unaffected by the field's mere existence.
//   S6 -- the per-call bracket: two tool calls in ONE round each get their own correctly-tagged
//         events, and a LATER round whose tool is silent produces none -- no stale binding leaks.
//   S7 -- the detached-thread hazard `tool_pipeline.hpp::background_task()` already closes for
//         `report_progress`/`sandbox_fs`, now closed for this field too: a Backgroundable tool that
//         opens a scope on the detached thread must never reach the caller's live sink.
//   S8 -- the AG-UI projection carries the new fields, and omits ok/error_code on a started event
//         (where they are meaningless) while including them on a finished one.
//   S9 -- A REAL, SHIPPED PRODUCER: `SessionShellSandbox`'s `run_shell`, driven through the real
//         006 §3 ten-step pipeline (`invoke_tool`), emits a genuine `sandbox_exec_started`/`finished`
//         pair tagged `backend == "mediated-shell"`, `stage == "exec"`. Nothing scripted or
//         synthetic: this is the production call path.
//
// MACHINE SAFETY (CLAUDE.md): one detached thread bounded by a 2s wait_until, one real shell command
// (`pwd`) against a temp directory, no subprocesses, no unbounded loops.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/protocol/agui/projection.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "backends/native_jail/session_shell_wiring.hpp"

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
using agentengine::Principal;
using agentengine::RunEvent;
using agentengine::SandboxExecScope;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;
using agentengine::run_event_kind;
namespace payload = agentengine::run_event_payload;

// A recording sink, standing in for whatever AgentSession binds.
struct Recorder {
    std::vector<std::pair<run_event_kind, payload::SandboxExec>> events;

    [[nodiscard]] std::function<void(run_event_kind, payload::SandboxExec)> sink() {
        return [this](run_event_kind k, payload::SandboxExec p) {
            events.emplace_back(k, std::move(p));
        };
    }
};

// -- Tool fixtures --------------------------------------------------------------------------------

struct ExecArgs { bool noop = false; };
AE_JSON_SCHEMA(ExecArgs, noop)
struct ExecReply { bool ok = false; };
AE_JSON_SCHEMA(ExecReply, ok)

// Stands in for any tool whose invoke() reaches a sandbox: it brackets a (fictional) exec exactly the
// way the real PDF-worker and mediated-shell producers do.
struct SandboxingTool : agentengine::Tool<SandboxingTool> {
    static constexpr std::string_view name        = "sandboxing_tool";
    static constexpr std::string_view description = "Brackets a sandbox exec during invoke().";
    using Args  = ExecArgs;
    using Reply = ExecReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        SandboxExecScope scope(ctx, "exec-1", "test-backend", "exec");
        scope.succeeded();
        return Reply{true};
    }
};

// The positive control (S5): never touches the sink.
struct SilentTool : agentengine::Tool<SilentTool> {
    static constexpr std::string_view name        = "silent_tool";
    static constexpr std::string_view description = "Never opens a SandboxExecScope.";
    using Args  = ExecArgs;
    using Reply = ExecReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

// S7's fixture: Backgroundable AND brackets an exec, so the scope runs on background_task()'s own
// detached std::thread.
struct BackgroundableSandboxingTool
    : agentengine::Tool<BackgroundableSandboxingTool, agentengine::Backgroundable> {
    static constexpr std::string_view name        = "bg_sandboxing_tool";
    static constexpr std::string_view description = "Backgroundable; also opens a SandboxExecScope.";
    using Args  = ExecArgs;
    using Reply = ExecReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        SandboxExecScope scope(ctx, "exec-bg", "test-backend", "exec");
        scope.succeeded();
        return Reply{true};
    }
};

class ToolHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<SandboxingTool, SilentTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<ToolHistoryProvider>);

struct ScriptedOutcome {
    Message message;
    Usage   usage;
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
        std::size_t const idx =
            state_->call_count < state_->script.size() ? state_->call_count : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
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

Message tool_call_response(std::vector<std::pair<std::string, std::string>> calls) {
    Message m;
    m.role = role::assistant;
    for (auto const& [call_id, tool_name] : calls) {
        agentengine::ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall call;
        call.call_id        = call_id;
        call.tool_name       = tool_name;
        call.arguments_json  = R"({"noop":true})";
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

using Session = AgentSession<ScriptedChatClient, NoSessionState, ToolHistoryProvider>;

[[nodiscard]] std::vector<RunEvent> sandbox_events_of(std::vector<RunEvent> const& evs) {
    std::vector<RunEvent> out;
    for (auto const& e : evs) {
        if (e.kind == run_event_kind::sandbox_exec_started ||
            e.kind == run_event_kind::sandbox_exec_finished) {
            out.push_back(e);
        }
    }
    return out;
}

}  // namespace

int main() {
    namespace json = agentengine::json;

    // ---- S1/S2/S3: the SandboxExecScope bracket itself ------------------------------------------
    {
        Recorder rec;
        {
            EffectContext ctx;
            ctx.sandbox_exec_sink = rec.sink();
            {
                SandboxExecScope scope(ctx, "e1", "native-jail", "create");
                check(rec.events.size() == 1,
                      "S1: the started event fires at CONSTRUCTION -- a UI learns provisioning began "
                      "before the slow part runs, which is the entire point of the pair");
                check(rec.events[0].first == run_event_kind::sandbox_exec_started &&
                          rec.events[0].second.exec_id == "e1" &&
                          rec.events[0].second.backend == "native-jail" &&
                          rec.events[0].second.stage == "create",
                      "S1: it carries the exec_id/backend/stage it was given");
                scope.succeeded();
            }
            check(rec.events.size() == 2, "S1: the finished event fires at scope exit");
            check(rec.events[1].first == run_event_kind::sandbox_exec_finished &&
                      rec.events[1].second.exec_id == "e1" && rec.events[1].second.ok &&
                      rec.events[1].second.error_code.empty(),
                  "S1: succeeded() reports ok with no error code, correlated by the same exec_id");
        }

        rec.events.clear();
        {
            EffectContext ctx;
            ctx.sandbox_exec_sink = rec.sink();
            {
                SandboxExecScope scope(ctx, "e2", "native-jail", "create");
                // Deliberately no succeeded()/failed() -- stands in for an early return between
                // provisioning and completion.
            }
            check(rec.events.size() == 2, "S2: a start and an end still both fire");
            check(!rec.events[1].second.ok &&
                      rec.events[1].second.error_code == "sandbox.exec.abandoned",
                  "S2: FAIL-CLOSED -- an abandoned scope reports ok=false with a real code, never a "
                  "success that never happened and never a dangling start a UI must time out on");
        }

        rec.events.clear();
        {
            EffectContext ctx;
            ctx.sandbox_exec_sink = rec.sink();
            {
                SandboxExecScope scope(ctx, "e3", "docker", "exec");
                scope.failed("docker.create_failed");
            }
            check(!rec.events[1].second.ok &&
                      rec.events[1].second.error_code == "docker.create_failed",
                  "S3: failed(code) carries that exact code through");
        }
    }

    // ---- S4: end to end through a real AgentSession and its own event stream ---------------------
    {
        Session session;
        session.initialize("s4", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response({{"c1", "sandboxing_tool"}}), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "S4: the run converges normally");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        std::vector<RunEvent> const sandbox = sandbox_events_of(events);

        check(sandbox.size() == 2,
              "S4: exactly one started/finished pair reached the session's REAL event stream -- the "
              "producer side issue #64 found missing now exists");
        if (sandbox.size() == 2) {
            check(sandbox[0].kind == run_event_kind::sandbox_exec_started &&
                      sandbox[1].kind == run_event_kind::sandbox_exec_finished,
                  "S4: in that order");
            auto const& p = std::get<payload::SandboxExec>(sandbox[1].payload);
            check(p.exec_id == "exec-1" && p.backend == "test-backend" && p.stage == "exec" && p.ok,
                  "S4: the payload survives the whole emit path intact, new fields included");
            check(sandbox[0].seq < sandbox[1].seq,
                  "S4: both carry real, monotonically-sequenced run-event identity -- they went "
                  "through emit_run_event(), not a side channel");
            check(!sandbox[0].run_id.empty() && sandbox[0].run_id == sandbox[1].run_id,
                  "S4: and both are attributed to this run (I4)");
        }
    }

    // ---- S5: positive control -- a silent tool emits nothing -------------------------------------
    {
        Session session;
        session.initialize("s5", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            {tool_call_response({{"c1", "silent_tool"}}), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "S5: the run converges normally");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        check(sandbox_events_of(events).empty(),
              "S5: a tool that never opens a scope produces NO sandbox events -- the ordinary path is "
              "unchanged by this field existing");
        bool saw_tool_call = false;
        for (auto const& e : events) {
            if (e.kind == run_event_kind::tool_call_finished) saw_tool_call = true;
        }
        check(saw_tool_call, "S5: and the tool genuinely ran -- this is not an empty-run false pass");
    }

    // ---- S6: the per-call bracket leaks nothing across calls or rounds ---------------------------
    {
        Session session;
        session.initialize("s6", Principal{"p", ""});
        session.emplace_chat_client().set_script({
            // Round 1: two calls in ONE round, both sandboxing.
            {tool_call_response({{"c1", "sandboxing_tool"}, {"c2", "sandboxing_tool"}}),
             Usage{1, 1, 0, 0, 0.0}},
            // Round 2: a silent tool -- if the bracket leaked, this round would still emit.
            {tool_call_response({{"c3", "silent_tool"}}), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto viewer  = session.enable_event_stream(std::pmr::get_default_resource());
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "S6: the run converges normally");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        std::vector<RunEvent> const sandbox = sandbox_events_of(events);
        check(sandbox.size() == 4,
              "S6: exactly two pairs -- one per sandboxing call, none from the silent third call in a "
              "later round; the per-call bracket never leaves a stale binding behind");
    }

    // ---- S7: background_task()'s detached thread never reaches the caller's sink ------------------
    // Same hazard, same choke point, and same proof shape as D/E in
    // test_agent_session_tool_call_progress.cpp: `ctx` carries a LIVE sink, standing in for whatever a
    // racing bracket window could copy into start_background_task()'s "PLAIN, UNLOCKED" call. ADR-160
    // gave emit_run_event_for() a mutex, which fixes the map race but NOT the object-lifetime half --
    // a detached thread outliving a forked/cleared/destroyed session is what this reset closes.
    {
        std::atomic<int> fired{0};
        EffectContext ctx;
        ctx.principal         = Principal{"p", ""};
        ctx.run_id            = "bg-race-run";
        ctx.sandbox_exec_sink = [&fired](run_event_kind, payload::SandboxExec) {
            fired.fetch_add(1);
        };
        CapabilitySet const held = CapabilitySet::grant_root({agentengine::cap::Background{1}});
        ctx.capabilities = agentengine::borrow_capabilities(held);

        auto const table = agentengine::ToolTable::from_tools<BackgroundableSandboxingTool>();
        agentengine::ToolCallRequest const req{"call-bg", "bg_sandboxing_tool",
                                                *json::parse(R"({"noop":true})"), false};

        std::atomic<bool> completed{false};
        auto submitted = agentengine::background_task(
            table, held, req, ctx, agentengine::ApprovalDecider{}, /*current_background_count=*/0,
            [&completed](agentengine::ToolResult, agentengine::ToolInvocationAudit) {
                completed.store(true);
            });
        check(submitted.has_value(), "S7 setup: the Backgroundable tool call is accepted");
        check(wait_until([&] { return completed.load(); }, std::chrono::seconds(2)),
              "S7 setup: the detached thread actually finishes");
        check(fired.load() == 0,
              "S7: the backgrounded tool DID open a scope on its own detached thread, but "
              "background_task()'s structural reset means the CALLER's sink never fires -- the same "
              "cross-thread hazard already closed for report_progress/sandbox_fs, closed for this "
              "field too rather than left to be rediscovered");
    }

    // ---- S8: the AG-UI projection carries the new fields ------------------------------------------
    {
        namespace agui = agentengine::agui;
        agui::RunEventProjector projector("thread-1");

        RunEvent started;
        started.run_id  = "run-p";
        started.seq     = 1;
        started.kind    = run_event_kind::sandbox_exec_started;
        started.payload = payload::SandboxExec{"e9", "docker", "create", true, {}};
        auto const started_out = projector.project(started);
        check(started_out.size() == 1, "S8: a started event projects to exactly one AG-UI event");
        if (started_out.size() == 1) {
            auto const* snap = std::get_if<agui::ActivitySnapshot>(&started_out[0]);
            check(snap != nullptr, "S8: it is an ActivitySnapshot (013 §2.1)");
            if (snap != nullptr) {
                json::Value const* backend = snap->content.find("backend");
                json::Value const* stage   = snap->content.find("stage");
                check(backend != nullptr && backend->is_string() && backend->as_string() == "docker",
                      "S8: backend reaches the wire");
                check(stage != nullptr && stage->is_string() && stage->as_string() == "create",
                      "S8: stage reaches the wire -- a UI can now say WHICH half it is waiting on");
                check(snap->content.find("ok") == nullptr &&
                          snap->content.find("error_code") == nullptr,
                      "S8: ok/error_code are ABSENT on a started event -- they are meaningless there, "
                      "and a default 'ok: true' next to 'status: started' would read as a result that "
                      "has not happened yet");
            }
        }

        RunEvent finished;
        finished.run_id  = "run-p";
        finished.seq     = 2;
        finished.kind    = run_event_kind::sandbox_exec_finished;
        finished.payload = payload::SandboxExec{"e9", "docker", "create", false, "docker.create_failed"};
        auto const finished_out = projector.project(finished);
        check(finished_out.size() == 1, "S8: a finished event projects to exactly one AG-UI event");
        if (finished_out.size() == 1) {
            auto const* snap = std::get_if<agui::ActivitySnapshot>(&finished_out[0]);
            check(snap != nullptr, "S8: also an ActivitySnapshot");
            if (snap != nullptr) {
                json::Value const* ok   = snap->content.find("ok");
                json::Value const* code = snap->content.find("error_code");
                check(ok != nullptr && ok->is_bool() && !ok->as_bool(),
                      "S8: ok IS present on a finished event, and carries the real failure");
                check(code != nullptr && code->is_string() &&
                          code->as_string() == "docker.create_failed",
                      "S8: with the producer's own error code, so a UI can show why");
            }
        }
    }

    // ---- S9: a REAL, shipped producer -- SessionShellSandbox's run_shell -------------------------
    // Nothing scripted here: the real `run_shell` ToolDescriptor, resolved through the real 006 §3
    // ten-step pipeline (`invoke_tool`), against a real host directory. If ADR-170's wiring in
    // src/backends/native_jail/session_shell_wiring.hpp regressed, this is what fails.
    {
        using agentengine::SessionShellSandbox;
        using agentengine::ToolCallRequest;
        using agentengine::ToolTable;
        using agentengine::invoke_tool;

        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "ae_sandbox_exec_events_test";
        std::filesystem::remove_all(scratch);
        std::filesystem::create_directories(scratch);

        auto sandbox = SessionShellSandbox::create(scratch);
        check(sandbox.has_value(), "S9 setup: SessionShellSandbox::create succeeds");
        if (sandbox.has_value()) {
            auto const table = ToolTable::from_descriptors({(*sandbox)->tool_descriptor()});
            // The same real grants test_session_shell_wiring.cpp's own positive case uses -- run_shell
            // is genuinely capability-gated (I2), so an empty set makes this a policy refusal that
            // never reaches the producer at all, which would prove nothing about the events.
            CapabilitySet const held = CapabilitySet::grant_root({
                agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
                agentengine::Capability{
                    agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
            });

            Recorder rec;
            EffectContext ctx;
            ctx.principal         = Principal{"test-principal", ""};
            ctx.capabilities      = agentengine::borrow_capabilities(held);
            ctx.sandbox_exec_sink = rec.sink();

            ToolCallRequest const req{"call-shell", "run_shell", *json::parse(R"({"source":"pwd"})"),
                                       false};
            agentengine::ToolInvocationAudit audit;
            agentengine::ToolResult const result = invoke_tool(table, held, req, ctx, nullptr, &audit);
            check(!result.is_error, "S9 setup: the real run_shell call succeeds");

            check(rec.events.size() == 2,
                  "S9: a REAL, shipped production tool emitted exactly one started/finished pair -- "
                  "the events fire on the real path, not only in a projection unit test");
            if (rec.events.size() == 2) {
                check(rec.events[0].first == run_event_kind::sandbox_exec_started &&
                          rec.events[1].first == run_event_kind::sandbox_exec_finished,
                      "S9: in order");
                check(rec.events[1].second.backend == "mediated-shell" &&
                          rec.events[1].second.stage == "exec",
                      "S9: tagged with the real backend and the ONE stage this producer honestly has "
                      "-- its sandbox is provisioned per SESSION, so there is no per-call create to "
                      "report and none is fabricated");
                check(rec.events[0].second.exec_id == rec.events[1].second.exec_id &&
                          !rec.events[0].second.exec_id.empty(),
                      "S9: correlated by a real, non-empty exec_id");
                check(rec.events[1].second.ok, "S9: and reported as a completed exec");
            }

            // A second call must mint a DISTINCT exec_id -- otherwise a consumer correlating by it
            // would splice two unrelated shell runs into one activity.
            rec.events.clear();
            ToolCallRequest const req2{"call-shell-2", "run_shell",
                                        *json::parse(R"({"source":"pwd"})"), false};
            agentengine::ToolInvocationAudit audit2;
            (void)invoke_tool(table, held, req2, ctx, nullptr, &audit2);
            check(rec.events.size() == 2 && rec.events[0].second.exec_id != "shell_1",
                  "S9: a second call mints its own exec_id -- two shell runs never splice into one "
                  "activity in a consumer that correlates by it");
        }
        std::filesystem::remove_all(scratch);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall ADR-170 sandbox-exec-event checks passed\n");
    return 0;
}
