// Prove phase for decisions/ADR-057-agent-ask-suspend-without-deadlock.md's Design B
// (abort-and-replay): `agent.ask()` (026-Agent-Facing-Runtime-Surface.md §5) suspends the round it
// runs inside, and a host-driven resolve/replay cycle
// (`rt::AgentSession::resolve_interaction()`'s new `codeact_ask` branch) resumes it against a real
// embedded CPython interpreter -- not a stand-in. Style/structure deliberately mirrors
// tests/test_rt_agent_session_suspend_approval.cpp (this project's own precedent for this class of
// test): deterministic, offline, a hand-scripted `ChatClientT`, `check()`/`drive<T>()` copied rather
// than shared (that file's own "no cross-test-file coupling" precedent). Unlike that file, this one
// needs a REAL `MediatedPythonRunner` -- `agent.ask()`'s suspend/replay mechanism lives inside
// `MediatedPythonRunner::run()` (mediated_python_runner.cpp), not in anything this test could stub
// out and still be proving ADR-057 §9's actual mechanism. Only built when
// AGENTENGINE_BUILD_PYTHON_RUNNER is ON (see tests/CMakeLists.txt).
//
//   B1 -- a script calling `agent.ask()` suspends the round: `start_run()` completes with the named
//         sentinel `kSuspendedForCodeActAsk`, a real `Interaction{reason=codeact_ask}` opens, and
//         nothing folds into `tool_results_message` yet (history stays exactly as it was right after
//         the assistant's tool-call message).
//   B2 -- a fresh `StartRun` sent while a `codeact_ask` interaction is open is rejected
//         (`run.approval_pending`, `start_run()`'s widened admission check -- ADR-057 §4 Finding A2,
//         the SU2 mirror this ADR's own text names as the test that would have caught A2 if it were
//         missing).
//   B3 -- resolving with an answer re-runs the script and completes correctly for a script with a
//         deterministic, side-effect-free prefix (`x = 1` before `agent.ask()`, `x` used after) --
//         proves replay of that narrow case behaves indistinguishably from real continuation.
//   B4 -- two sequential `agent.ask()` calls in ONE script: the first resolve produces a SECOND
//         ask-pending suspension under the SAME `interaction_id`, with an updated prompt; the second
//         resolve completes.
//   B5 -- positive control: a script with no `agent.ask()` call completes in one round exactly as
//         before this change (no regression to the ordinary path).
//   B6 -- a round with `execute_code` plus another tool call, where the script ask-pends, fails
//         closed with `run.codeact_ask_in_multi_call_round_unsupported` -- the named scoping
//         boundary is real and tested, not merely asserted in prose.
//   B7 -- residual, demonstrated: a script with a mediated file write BEFORE its `agent.ask()` call
//         is shown, on replay, to repeat that write -- turning ADR-057 §4's side-effect-repetition
//         finding into a permanent regression/documentation test.

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::task;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;

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
using agentengine::run_event_payload::CodeActAskRequested;
using agentengine::ExecRequest;
using agentengine::ExecState;
using agentengine::exec_outcome_class;
using agentengine::error;
using agentengine::failure_class;
using agentengine::result;

// -- The real execute_code tool -- same shape as tools/cli_chat.cpp's own ExecuteCodeTool ----------

struct ExecuteCodeArgs {
    std::string code;
    std::string language;
};
AE_JSON_SCHEMA(ExecuteCodeArgs, code, language)

struct ExecuteCodeReply {
    bool ok = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string result_repr;
};
AE_JSON_SCHEMA(ExecuteCodeReply, ok, stdout_text, stderr_text, result_repr)

struct ExecuteCodeTool : agentengine::Tool<ExecuteCodeTool,
                                             agentengine::Capabilities<agentengine::cap::decl::FsRead<"work">,
                                                                        agentengine::cap::decl::FsWrite<"work">>,
                                             agentengine::EffectClass<agentengine::effect_class::at_most_once>> {
    static constexpr std::string_view name = "execute_code";
    static constexpr std::string_view description = "Execute Python code in this session's real interpreter.";
    using Args = ExecuteCodeArgs;
    using Reply = ExecuteCodeReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "ExecuteCodeTool::invoke() must never run directly in this test -- only through "
            "CodeActHistoryProvider::real_execute_code()", "test.dead_static_invoke_path"});
    }
};

// -- A trivial second tool for B6's multi-call round ------------------------------------------------

struct NoopArgs { std::string label; };
AE_JSON_SCHEMA(NoopArgs, label)
struct NoopReply { std::string label; };
AE_JSON_SCHEMA(NoopReply, label)

[[nodiscard]] bool& noop_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

struct NoopTool : agentengine::Tool<NoopTool, agentengine::Capabilities<>,
                                      agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "noop_tool";
    static constexpr std::string_view description = "Does nothing; a second tool for B6's multi-call round.";
    using Args = NoopArgs;
    using Reply = NoopReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        noop_tool_invoked_log() = true;
        return Reply{a.label};
    }
};

// -- HistoryProviderT fixture: declares execute_code (+ optionally noop_tool for B6), backed by a
// REAL MediatedPythonRunner the test owns and hands in via configure(). -----------------------------

class CodeActHistoryProvider {
public:
    void configure(MediatedPythonRunner* runner, bool include_noop_tool) {
        runner_ = runner;
        include_noop_tool_ = include_noop_tool;
    }

    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        std::vector<agentengine::ToolDescriptor> descriptors = {
            agentengine::make_tool_descriptor_with_invoke<ExecuteCodeTool>(
                [this](ExecuteCodeArgs a, EffectContext& ctx) { return real_execute_code(std::move(a), ctx); }),
        };
        if (include_noop_tool_) descriptors.push_back(agentengine::make_tool_descriptor<NoopTool>());
        c.tools = std::move(descriptors);
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    // Byte-for-byte the same mapping ADR-057 §9 specifies for `real_execute_code()`
    // (tools/cli_chat.cpp) -- copied here rather than shared, matching this test file's own
    // "no cross-file coupling" precedent for the fixture code around it.
    [[nodiscard]] result<ExecuteCodeReply> real_execute_code(ExecuteCodeArgs a, EffectContext& ctx) {
        ExecRequest req{a.language.empty() ? "python" : a.language, a.code, ctx.codeact_preseeded_answers};
        auto outcome = runner_->run(req, exec_state_, ctx);
        if (!outcome) return std::unexpected(outcome.error());
        if (outcome->klass == exec_outcome_class::ask_pending) {
            return std::unexpected(error{failure_class::contract, outcome->ask_prompt, "codeact.ask_pending"});
        }
        ExecuteCodeReply reply;
        reply.ok = (outcome->klass == exec_outcome_class::ok);
        reply.stdout_text = outcome->stdout_text;
        reply.stderr_text = outcome->stderr_text;
        reply.result_repr = outcome->result_repr;
        return reply;
    }

    MediatedPythonRunner* runner_ = nullptr;
    bool include_noop_tool_ = false;
    ExecState exec_state_;
};
static_assert(agentengine::ContextProvider<CodeActHistoryProvider>);

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

// B6's own fixture: TWO tool calls in one assistant message.
Message two_tool_call_response(std::string call_id1, std::string tool1, std::string args1,
                                std::string call_id2, std::string tool2, std::string args2) {
    Message m;
    m.role = role::assistant;
    ToolCall c1; c1.call_id = std::move(call_id1); c1.tool_name = std::move(tool1);
    c1.arguments_json = std::move(args1); c1.provenance = call_provenance::vendor_structured;
    ContentItem i1; i1.origin = content_origin::assistant; i1.value = c1;
    ToolCall c2; c2.call_id = std::move(call_id2); c2.tool_name = std::move(tool2);
    c2.arguments_json = std::move(args2); c2.provenance = call_provenance::vendor_structured;
    ContentItem i2; i2.origin = content_origin::assistant; i2.value = c2;
    m.content.push_back(i1);
    m.content.push_back(i2);
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

// A code string, JSON-escaped inline (these test scripts have no characters needing real JSON
// escaping beyond quotes/newlines, handled by hand here -- deliberately not pulling in a JSON
// encoder just to build a literal test fixture).
std::string execute_code_args(std::string const& code) {
    std::string escaped;
    for (char c : code) {
        if (c == '"' || c == '\\') escaped += '\\';
        if (c == '\n') { escaped += "\\n"; continue; }
        escaped += c;
    }
    return R"({"code":")" + escaped + R"(","language":"python"})";
}

using Session = AgentSession<ScriptedChatClient, NoSessionState, CodeActHistoryProvider>;

[[nodiscard]] CapabilitySet make_held_caps() {
    return CapabilitySet::grant_root({
        Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
        Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
}

[[nodiscard]] MediatedPythonConfig make_cfg(std::wstring const& work_dir_w = L"") {
    MediatedPythonConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.expose_agent_ask = true;
    if (!work_dir_w.empty()) cfg.mount_roots["work"] = work_dir_w;
    return cfg;
}

}  // namespace

int main() {
    using agentengine::Principal;

    // ---- B1: agent.ask() suspends the round with the named sentinel, mints a real Interaction,
    // nothing folds into history yet. ----------------------------------------------------------------
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B1-setup: runner initializes cleanly");

        Session session;
        session.initialize("b1", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "execute_code",
                                   execute_code_args("import agent\nagent.ask('what is your name?')")),
              Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        std::size_t const history_size_before = session.history().size();  // 0 before start_run pushes input
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));

        check(!outcome.has_value(), "B1: start_run() fails while suspended for the ask");
        check(outcome.has_value() || outcome.error().code == Session::kSuspendedForCodeActAsk,
              "B1: the error code is the named kSuspendedForCodeActAsk sentinel");
        check(session.has_open_interactions(), "B1: a real Interaction opened for this suspension");
        check(session.open_interactions().size() == 1 &&
                  session.open_interactions().front().reason == interaction_reason::codeact_ask,
              "B1: the open Interaction is tagged interaction_reason::codeact_ask");
        // history: [user_message, assistant tool-call message] -- the tool_results_message must NOT
        // have been pushed.
        check(session.history().size() == history_size_before + 2,
              "B1: history holds exactly the user input and the assistant's tool-call message -- "
              "nothing folded into tool_results_message yet");
        check(session.history().back().role == role::assistant,
              "B1: history's tail is still the assistant tool-call message, not a tool result");

        std::vector<RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        bool saw_input_required = false;
        bool saw_ask_requested = false;
        std::string seen_prompt;
        for (RunEvent const& ev : events) {
            if (ev.kind == run_event_kind::input_required) saw_input_required = true;
            if (ev.kind == run_event_kind::codeact_ask_requested) {
                saw_ask_requested = true;
                seen_prompt = std::get<CodeActAskRequested>(ev.payload).prompt;
            }
        }
        check(saw_input_required, "B1: an input_required event fired");
        check(saw_ask_requested && seen_prompt == "what is your name?",
              "B1: a codeact_ask_requested event fired carrying the real prompt text");
    }

    // ---- B2: a fresh StartRun while a codeact_ask interaction is open is rejected -------------------
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B2-setup: runner initializes cleanly");

        Session session;
        session.initialize("b2", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        session.emplace_chat_client().set_script(
            {{tool_call_response("c1", "execute_code", execute_code_args("import agent\nagent.ask('q')")),
              Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "B2 setup: the first start_run() suspends, same as B1");
        std::string const run_id_at_suspend = session.last_run_id();

        auto r2 = drive(session.start_run(StartRun{user_message("second, while suspended")}));
        check(!r2.has_value(),
              "B2: a second start_run() sent while a codeact_ask interaction is open is rejected");
        check(r2.has_value() || r2.error().code == "run.approval_pending",
              "B2: the rejection is specifically run.approval_pending, start_run()'s widened "
              "admission check (ADR-057 Finding A2)");
        check(session.last_run_id() == run_id_at_suspend,
              "B2: no new run_id was minted -- the rejected start_run() never reached run_counter_");
        check(session.open_interactions().size() == 1,
              "B2: still exactly one open interaction -- the rejected start_run() didn't touch it");
    }

    // ---- B3: resolving with an answer re-runs the script; a deterministic prefix survives replay ---
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B3-setup: runner initializes cleanly");

        Session session;
        session.initialize("b3", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "execute_code",
                                  execute_code_args("import agent\nx = 1\ny = agent.ask('what is y?')\nprint(x, y)")),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "B3 setup: the run suspends on agent.ask()");
        std::string const interaction_id = session.open_interactions().front().interaction_id;
        std::string const run_id_before_resume = session.last_run_id();

        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt, std::string("42")}));
        check(r2.has_value(), "B3: resolving with an answer lets the run converge");
        if (r2.has_value()) {
            auto const* t = std::get_if<Text>(&r2->message.content.front().value);
            check(t != nullptr && t->text == "done", "B3: the converged response is the scripted post-resume text");
        }
        check(!session.has_open_interactions(), "B3: the interaction closed once resolved");
        check(session.last_run_id() == run_id_before_resume,
              "B3: the SAME run_id continued -- a resumption, not a fresh run");
        check(client.call_count() == 2, "B3: the ChatClientT was called twice -- once before, once after");

        // The real proof: the replayed script's deterministic prefix (x = 1) survived the replay, and
        // the preseeded answer (42) landed correctly -- read straight from the tool result folded
        // into history.
        Message const& results_msg = session.history()[session.history().size() - 2];
        check(results_msg.role == role::tool, "B3: a tool-results message was folded into history");
        bool found_correct_output = false;
        if (!results_msg.content.empty()) {
            if (auto const* tr = std::get_if<ToolResult>(&results_msg.content.front().value)) {
                if (!tr->is_error && !tr->content.empty()) {
                    if (auto const* d = std::get_if<agentengine::Data>(&tr->content.front().value)) {
                        found_correct_output = d->json.find("\"1 42\"") != std::string::npos ||
                                                d->json.find("1 42") != std::string::npos;
                    }
                }
            }
        }
        check(found_correct_output,
              "B3: the folded execute_code reply shows stdout '1 42' -- x=1 recomputed fresh on "
              "replay, y=42 the preseeded answer, combined correctly");
    }

    // ---- B4: two sequential agent.ask() calls in one script -- same interaction_id, updated prompt -
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B4-setup: runner initializes cleanly");

        Session session;
        session.initialize("b4", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "execute_code",
                                  execute_code_args(
                                      "import agent\na = agent.ask('q1')\nb = agent.ask('q2')\nprint(a, b)")),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "B4 setup: the run suspends on the FIRST agent.ask()");
        check(r1.has_value() || r1.error().code == Session::kSuspendedForCodeActAsk,
              "B4: first suspension is the codeact-ask sentinel");
        std::string const interaction_id_1 = session.open_interactions().front().interaction_id;

        std::string first_prompt_seen;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::codeact_ask_requested) {
                first_prompt_seen = std::get<CodeActAskRequested>(ev->payload).prompt;
            }
        }
        check(first_prompt_seen == "q1", "B4: the first codeact_ask_requested event carries prompt 'q1'");

        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id_1, /*approved=*/false, std::nullopt, std::string("A1")}));
        check(!r2.has_value(), "B4: resolving the first ask does NOT complete the run -- a SECOND ask-pends");
        check(r2.has_value() || r2.error().code == Session::kSuspendedForCodeActAsk,
              "B4: the second suspension is ALSO the codeact-ask sentinel");
        check(session.has_open_interactions() && session.open_interactions().size() == 1,
              "B4: still exactly one open interaction");
        std::string const interaction_id_2 = session.open_interactions().front().interaction_id;
        check(interaction_id_1 == interaction_id_2,
              "B4: the SAME interaction_id is reused for the second ask -- no new one minted "
              "(ADR-057 §9: chaining through as many questions as one script asks)");

        std::string second_prompt_seen;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::codeact_ask_requested) {
                second_prompt_seen = std::get<CodeActAskRequested>(ev->payload).prompt;
            }
        }
        check(second_prompt_seen == "q2",
              "B4: the SECOND codeact_ask_requested event carries an UPDATED prompt ('q2', not 'q1')");

        auto r3 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id_2, /*approved=*/false, std::nullopt, std::string("A2")}));
        check(r3.has_value(), "B4: resolving the second ask completes the run");
        check(!session.has_open_interactions(), "B4: the interaction closed once both asks were resolved");
        if (r3.has_value()) {
            auto const* t = std::get_if<Text>(&r3->message.content.front().value);
            check(t != nullptr && t->text == "done", "B4: the converged response is the scripted post-resume text");
        }
    }

    // ---- B5 (positive control): no agent.ask() call -- one round, no regression --------------------
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B5-setup: runner initializes cleanly");

        Session session;
        session.initialize("b5", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "execute_code", execute_code_args("print(6 * 7)")), Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(r1.has_value(), "B5: a script with no agent.ask() call completes in one round, no suspension");
        check(!session.has_open_interactions(), "B5: no Interaction ever opened");
        check(client.call_count() == 2, "B5: the ChatClientT was called exactly twice, the ordinary shape");
        if (r1.has_value()) {
            auto const* t = std::get_if<Text>(&r1->message.content.front().value);
            check(t != nullptr && t->text == "done", "B5: converges to the scripted response, unchanged shape");
        }
    }

    // ---- B6: execute_code + another tool call in one round, execute_code ask-pends -- fails closed -
    {
        MediatedPythonRunner runner(make_cfg());
        auto init = runner.initialize();
        check(init.has_value(), "B6-setup: runner initializes cleanly");

        noop_tool_invoked_log() = false;

        Session session;
        session.initialize("b6", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/true);
        session.emplace_chat_client().set_script(
            {{two_tool_call_response("c1", "execute_code", execute_code_args("import agent\nagent.ask('q')"),
                                       "c2", "noop_tool", R"({"label":"x"})"),
              Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        std::size_t const history_size_before = session.history().size();
        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(!outcome.has_value(), "B6: the round fails -- ask-pending inside a multi-call round is refused");
        check(outcome.has_value() ||
                  outcome.error().code == "run.codeact_ask_in_multi_call_round_unsupported",
              "B6: the failure is specifically run.codeact_ask_in_multi_call_round_unsupported");
        check(!session.has_open_interactions(),
              "B6: no Interaction opened -- a fail-closed multi-call round never suspends, it fails");
        check(session.history().size() == history_size_before + 2,
              "B6: history holds only the user input and the assistant's tool-call message -- no "
              "tool_results_message was folded, per ADR-057 §9's fail-closed shape");
    }

    // ---- B7 (residual, demonstrated): a mediated write before agent.ask() repeats on replay --------
    {
        std::filesystem::path const work_dir =
            std::filesystem::temp_directory_path() / "ae_adr040_b7_work";
        std::filesystem::remove_all(work_dir);
        std::filesystem::create_directories(work_dir);
        std::filesystem::path const marker = work_dir / "marker.txt";

        MediatedPythonRunner runner(make_cfg(work_dir.wstring()));
        auto init = runner.initialize();
        check(init.has_value(), "B7-setup: runner initializes cleanly with a real 'work' mount");

        Session session;
        session.initialize("b7", Principal{"p", ""});
        session.history_provider().configure(&runner, /*include_noop_tool=*/false);
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("c1", "execute_code",
                                  execute_code_args(
                                      "import agent\n"
                                      "with open('/work/marker.txt', 'a') as f:\n"
                                      "    f.write('X')\n"
                                      "agent.ask('q')")),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });
        CapabilitySet const held = make_held_caps();
        session.set_capabilities(&held);

        auto r1 = drive(session.start_run(StartRun{user_message("go")}));
        check(!r1.has_value(), "B7 setup: the run suspends on agent.ask(), AFTER the write already ran once");

        std::string after_first_run;
        {
            std::ifstream f(marker, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            after_first_run = ss.str();
        }
        check(after_first_run == "X", "B7 setup: the mediated write ran exactly once before the ask suspended");

        std::string const interaction_id = session.open_interactions().front().interaction_id;
        auto r2 = drive(session.resolve_interaction(
            ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt, std::string("42")}));
        check(r2.has_value(), "B7: resolving completes the run");

        std::string after_replay;
        {
            std::ifstream f(marker, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            after_replay = ss.str();
        }
        // THE RESIDUAL, DEMONSTRATED: replay re-ran the WHOLE script from the top, including the
        // write that already committed once before the first ask-pend -- 'X' was written a SECOND
        // time. This is not a bug in this pass's own code; it is ADR-057 §4's named cost of Design B
        // ("re-executes every side-effecting statement before the agent.ask() call a second time"),
        // proven real here rather than merely asserted in the ADR's prose.
        check(after_replay == "XX",
              "B7: THE RESIDUAL IS REAL -- the mediated write before agent.ask() ran TWICE total "
              "('XX', not 'X') because replay re-executes the script's deterministic prefix "
              "unconditionally, side effects included (ADR-057 §4's named, not fixed, cost of Design B)");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_agent_session_suspend_codeact_ask: ALL PASS\n");
    return 0;
}
