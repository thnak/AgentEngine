// Proves decisions/ADR-197-reject-malformed-tool-arguments.md (GitHub issue #105, 006 §3 step 2:
// "reject; do not coerce"). A model tool call whose argument text is not valid JSON never reaches
// the tool: `tool_call_request_of()` (core/tool_call_extraction.hpp) records the parse error, and
// the pipeline (core/tool_pipeline.hpp: `admit_call()`, so `invoke_tool()` and the parallel-batch
// path, plus `background_task()`) refuses it as `tool.malformed_arguments`. The probe tool's
// arguments are ALL optional, so under the old `{}` coercion it would have run -- that is the bug.
//
// Positive controls, run 2026-09-25, each restored after:
//   1. `tool_call_request_of()` back to the old coercion (parse error dropped, arguments `{}`):
//      T1, T2, T5 (admit_call and background_task) and T6 fail -- the probe runs on '{not json'.
//   2. only `admit_call()`'s step-2 check disabled: T1, T2, T5 (admit_call) and T6 fail.
//   3. only `background_task()`'s step-2 check disabled: T5 (background_task) fails.
// T3/T4 (the deliberate "" rule and valid-JSON controls) pass under every control.

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

namespace {

namespace ae = agentengine;

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

// Every argument optional: `{}` is a VALID call, so only a real step-2 refusal keeps it from running.
struct ProbeArgs {
    std::optional<std::string> note;
};
AE_JSON_SCHEMA(ProbeArgs, note)
struct ProbeReply {
    std::string seen;
};
AE_JSON_SCHEMA(ProbeReply, seen)

int g_probe_runs = 0;

struct ProbeTool : ae::Tool<ProbeTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "probe";
    static constexpr std::string_view description = "Counts its runs. Every argument is optional.";
    using Args = ProbeArgs;
    using Reply = ProbeReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_probe_runs;
        return Reply{a.note.value_or("<none>")};
    }
};

struct BackgroundProbeTool
    : ae::Tool<BackgroundProbeTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>,
               ae::Backgroundable> {
    static constexpr std::string_view name = "background_probe";
    static constexpr std::string_view description = "Backgroundable probe. Every argument is optional.";
    using Args = ProbeArgs;
    using Reply = ProbeReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_probe_runs;
        return Reply{a.note.value_or("<none>")};
    }
};

ae::ToolCall model_call(std::string call_id, std::string tool, std::string arguments_json) {
    ae::ToolCall c;
    c.call_id = std::move(call_id);
    c.tool_name = std::move(tool);
    c.arguments_json = std::move(arguments_json);
    c.provenance = ae::call_provenance::vendor_structured;
    return c;
}

std::string error_text(ae::ToolResult const& r) {
    for (ae::ContentItem const& item : r.content) {
        if (auto const* e = std::get_if<ae::Error>(&item.value)) return e->message;
    }
    return {};
}

// A pipeline-level call through `invoke_tool()`, returning (result, audit).
struct Invoked {
    ae::ToolResult result;
    ae::ToolInvocationAudit audit;
};
Invoked invoke(ae::ToolTable const& table, std::string arguments_json) {
    ae::CapabilitySet const held;
    ae::EffectContext ctx;
    Invoked out;
    out.result = ae::invoke_tool(table, held, ae::tool_call_request_of(model_call("c", "probe", arguments_json), 0),
                                 ctx, ae::ApprovalDecider{}, &out.audit);
    return out;
}

// The session's context: history plus the probe tool.
class ProbeProvider {
public:
    ae::rt::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<ProbeTool>().descriptors();
        co_return c;
    }
    ae::rt::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
    ae::ToolTable const table = ae::ToolTable::from_tools<ProbeTool, BackgroundProbeTool>();

    // --- T1: malformed text never reaches the tool, and folds as tool.malformed_arguments ----------
    {
        g_probe_runs = 0;
        Invoked r = invoke(table, "{not json");
        check(g_probe_runs == 0, "T1: '{not json' -- the all-optional probe tool did NOT run");
        check(r.result.is_error, "T1: the call folds as a tool error (a value, not an abort)");
        check(r.audit.error_code == "tool.malformed_arguments", "T1: its audit code is tool.malformed_arguments");
        check(error_text(r.result).find("not valid JSON") != std::string::npos,
              "T1: the model-visible message says the arguments were not valid JSON, so it can retry");
    }

    // --- T2: other malformed shapes (trailing garbage, truncated object) are refused too ------------
    {
        g_probe_runs = 0;
        Invoked a = invoke(table, R"({"note":"x"} trailing)");
        Invoked b = invoke(table, R"({"note":)");
        check(g_probe_runs == 0 && a.audit.error_code == "tool.malformed_arguments" &&
                  b.audit.error_code == "tool.malformed_arguments",
              "T2: trailing garbage and a truncated object are both refused, the tool never runs");
    }

    // --- T3: empty / whitespace-only text is a no-argument call, deliberately (ADR-197 §2) ---------
    {
        g_probe_runs = 0;
        Invoked e = invoke(table, "");
        Invoked w = invoke(table, "  \n\t");
        check(!e.result.is_error && !w.result.is_error && g_probe_runs == 2,
              "T3: '' and whitespace-only arguments mean {} (providers send '' for no-arg calls) -- the tool runs");
    }

    // --- T4: control -- valid JSON still runs, with its real arguments -----------------------------
    {
        g_probe_runs = 0;
        Invoked v = invoke(table, R"({"note":"hello"})");
        Invoked empty_obj = invoke(table, "{}");
        check(!v.result.is_error && !empty_obj.result.is_error && g_probe_runs == 2,
              "T4 control: '{\"note\":\"hello\"}' and '{}' both run -- the refusal is not a blanket one");
        check(ae::tool_call_request_of(model_call("c", "probe", R"({"note":"hello"})"), 0).arguments_parse_error.empty(),
              "T4 control: valid text leaves arguments_parse_error empty");
    }

    // --- T5: admit_call() (the parallel-batch path) and background_task() refuse it as well -------
    {
        g_probe_runs = 0;
        ae::CapabilitySet const held;
        auto admitted = ae::admit_call(table, held, ae::tool_call_request_of(model_call("c", "probe", "{not json"), 0),
                                       ae::Principal{}, ae::ApprovalDecider{}, ae::PolicyDecider{});
        check(!admitted && admitted.error().code == "tool.malformed_arguments",
              "T5: admit_call() refuses the malformed call before binding anything");

        ae::CapabilitySet const held_bg = ae::CapabilitySet::grant_root({ae::cap::Background{4}});
        bool completed = false;
        auto started = ae::background_task(
            table, held_bg, ae::tool_call_request_of(model_call("c", "background_probe", "{not json"), 0),
            ae::EffectContext{}, ae::ApprovalDecider{}, 0,
            [&](ae::ToolResult, ae::ToolInvocationAudit) { completed = true; });
        check(!started && started.error().code == "tool.malformed_arguments",
              "T5: background_task() refuses it too, before spawning anything");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!completed && g_probe_runs == 0, "T5: no background worker ran the tool");

        // control: the same backgroundable tool with valid arguments does start
        started = ae::background_task(table, held_bg,
                                      ae::tool_call_request_of(model_call("c2", "background_probe", "{}"), 0),
                                      ae::EffectContext{}, ae::ApprovalDecider{}, 0,
                                      [&](ae::ToolResult, ae::ToolInvocationAudit) { completed = true; });
        for (int i = 0; i < 200 && !completed; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(started.has_value() && completed, "T5 control: valid arguments background and complete");
    }

    // --- T6: end to end through a real AgentSession -- the model gets the error and retries ---------
    {
        g_probe_runs = 0;
        ae::testing::ScriptedChatClient script;
        (void)script.push(ae::testing::tool_calls_turn({{"call-bad", "probe", "{not json"}}));
        (void)script.push(ae::testing::tool_calls_turn({{"call-good", "probe", R"({"note":"retry"})"}}));
        (void)script.push(ae::testing::text_turn("done"));

        ae::CapabilitySet held = ae::CapabilitySet::grant_root({});
        ae::rt::AgentSession<ae::testing::ScriptedChatClient, ae::rt::NoSessionState, ProbeProvider> session;
        session.initialize("malformed-args", ae::Principal{"p1", ""}, std::nullopt, /*max_turns=*/6);
        session.emplace_chat_client(script);
        session.set_capabilities(&held);

        auto outcome = drive(session.start_run(ae::rt::StartRun{user_message("call probe")}));
        check(outcome.has_value(), "T6: the run completes (a malformed call is a tool error, not a run abort)");
        check(g_probe_runs == 1, "T6: the probe ran exactly once -- for the valid retry, never for '{not json'");

        bool bad_is_error = false;
        bool good_ok = false;
        for (ae::Message const& m : session.history()) {
            for (ae::ContentItem const& item : m.content) {
                if (auto const* tr = std::get_if<ae::ToolResult>(&item.value)) {
                    if (tr->call_id == "call-bad")
                        bad_is_error = tr->is_error && error_text(*tr).find("not valid JSON") != std::string::npos;
                    if (tr->call_id == "call-good") good_ok = !tr->is_error;
                }
            }
        }
        check(bad_is_error, "T6: history holds call-bad's result as an error naming the invalid JSON");
        check(good_ok, "T6: the model's retry with valid arguments succeeded");
        check(script.call_count() == 3, "T6: the model was called again after the error, so it could retry");
    }

    std::printf(g_failures == 0 ? "test_tool_call_malformed_arguments: OK\n"
                                : "test_tool_call_malformed_arguments: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
