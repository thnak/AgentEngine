// Gate test for decisions/ADR-244-required-choices-step-1b.md §2.2, slice 4a (red-team D1): a tool's approval mode
// is a stated choice. `Tool<>` declares `Approval<M>` or `ApprovalChosenByHost` (the compile-time half is
// tests/compile_fail/tool_rejects_missing_approval.cpp and tool_rejects_two_approvals.cpp); a `ToolDescriptor`
// whose `approval` is unset is refused with `choice.missing.tool.approval` at every place a call can start:
//
//   T1 `declared_approval()` is the declared mode, and nullopt for an `ApprovalChosenByHost` tool.
//   T2 `admit_call` refuses an unset descriptor before the approval decider is asked; a stated one is admitted.
//   T3 `invoke_tool` on an unset descriptor never runs the tool's body; the stated twin runs it.
//   T4 `background_task` refuses an unset descriptor; the stated twin starts.
//   T5 a session turn offering an unset descriptor fails before the model is called, with a `run_failed` event
//      carrying the code; the same provider with the mode stated runs the turn and the tool.
//   T6 `ToolRegistry::register_tool` refuses an unset descriptor; the stated twin registers.
//
// Each refusal has its positive control on the same tool with the mode set, so a refusal that fired for some
// other reason (an unknown name, a missing capability) would fail the control.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "../../support/run_task_sync.hpp"
#include "../../support/session_choices.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/tool_registry.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/block_on.hpp"

namespace ae  = agentengine;
namespace art = agentengine::rt;

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
T drive(art::task<T> t) {
    return art::block_on(std::move(t));
}

int g_body_runs = 0;

struct NoteArgs {
    std::string note;
};
AE_JSON_SCHEMA(NoteArgs, note)
struct NoteReply {
    bool ok = true;
};
AE_JSON_SCHEMA(NoteReply, ok)

// The host states this tool's mode (ADR-244 slice 4a).
struct HostTool : ae::Tool<HostTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>, ae::Backgroundable,
                           ae::ApprovalChosenByHost> {
    static constexpr std::string_view name        = "host_tool";
    static constexpr std::string_view description = "a tool whose approval mode the host states";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext&) const {
        ++g_body_runs;
        co_return Reply{true};
    }
};

struct GatedTool : ae::Tool<GatedTool, ae::Capabilities<>, ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "declares always_require";
    using Args  = NoteArgs;
    using Reply = NoteReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext&) const { co_return Reply{true}; }
};

ae::ToolDescriptor host_descriptor(std::optional<ae::approval_mode> mode) {
    ae::ToolDescriptor d = ae::make_tool_descriptor(HostTool{});
    d.approval           = mode;
    return d;
}

ae::ToolCallRequest note_call(std::string id = "c1") {
    return ae::ToolCallRequest{std::move(id), "host_tool", *ae::json::parse(R"({"note":"x"})"), false};
}

ae::Principal const kCaller{"p-caller", "tenant-a"};

ae::EffectContext make_ctx() {
    ae::EffectContext ctx;
    ctx.principal = kCaller;
    return ctx;
}

std::string error_text(ae::ToolResult const& r) {
    if (r.content.empty()) return {};
    auto const* e = std::get_if<ae::Error>(&r.content[0].value);
    return e != nullptr ? e->message : std::string{};
}

ae::Message text_message(std::string text, ae::role r = ae::role::user) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// Calls host_tool once, then answers; counts model calls.
class CallingClient {
public:
    struct State {
        int calls = 0;
    };
    CallingClient() : state_(std::make_shared<State>()) {}
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const {
        ae::ChatClientCapabilities c{};
        c.tool_calling = true;
        return c;
    }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext&) {
        if (++state_->calls == 1) {
            ae::Message m;
            m.role = ae::role::assistant;
            ae::ContentItem it{};
            it.origin = ae::content_origin::assistant;
            ae::ToolCall c;
            c.call_id        = "c1";
            c.tool_name      = "host_tool";
            c.arguments_json = R"({"note":"x"})";
            c.provenance     = ae::call_provenance::vendor_structured;
            it.value         = c;
            m.content.push_back(it);
            co_return ae::ChatResponse{m, ae::Usage{2, 3, 0, 0, 0.0}};
        }
        co_return ae::ChatResponse{text_message("done", ae::role::assistant), ae::Usage{2, 3, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }
    [[nodiscard]] std::shared_ptr<State> const& state() const { return state_; }

private:
    std::shared_ptr<State> state_;
};
static_assert(ae::ChatClient<CallingClient>);

// Offers host_tool with whatever mode the host gave it -- none, unless `mode` is set.
class HostToolProvider {
public:
    std::optional<ae::approval_mode> mode;
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(host_descriptor(mode));
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

}  // namespace

int main() {
    // ---- T1: the declaration ---------------------------------------------------------------------------------------
    check(GatedTool::declared_approval() == ae::approval_mode::always_require, "T1: Approval<M> is declared as M");
    check(!HostTool::declared_approval().has_value(), "T1: ApprovalChosenByHost declares no mode");
    check(!ae::make_tool_descriptor(HostTool{}).approval.has_value(),
          "T1: the descriptor of an ApprovalChosenByHost tool carries no mode until the host sets one");
    check(ae::make_tool_descriptor(GatedTool{}).approval == ae::approval_mode::always_require,
          "T1: the descriptor carries the declared mode");

    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});

    // ---- T2: admit_call -------------------------------------------------------------------------------------------
    {
        int                  asked = 0;
        ae::ApprovalDecider  approve = [&asked](ae::Principal const&, std::string_view, std::string const&) -> ae::task<bool> {
            ++asked;
            co_return true;
        };
        ae::ToolTable const unset = ae::ToolTable::from_descriptors({host_descriptor(std::nullopt)});
        auto refused = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(unset, held, note_call(), kCaller, approve, nullptr));
        check(!refused.has_value() && refused.error().code == "choice.missing.tool.approval",
              "T2: admit_call refuses a descriptor with no approval mode (choice.missing.tool.approval)");
        check(!refused.has_value() && refused.error().message.find("host_tool") != std::string::npos,
              "T2: the refusal names the tool");
        check(asked == 0, "T2: the approval decider is never asked about a tool with no mode");

        ae::ToolTable const gated = ae::ToolTable::from_descriptors({host_descriptor(ae::approval_mode::always_require)});
        auto admitted = ae::test_support::run_task_sync<ae::result<ae::AdmittedCall>>(
            ae::admit_call(gated, held, note_call(), kCaller, approve, nullptr));
        check(admitted.has_value() && asked == 1,
              "T2 control: the same tool stated always_require is admitted after the decider is asked");
    }

    // ---- T3: invoke_tool ------------------------------------------------------------------------------------------
    {
        g_body_runs = 0;
        ae::ToolTable const unset = ae::ToolTable::from_descriptors({host_descriptor(std::nullopt)});
        auto ctx = make_ctx();
        auto r   = ae::test_support::run_task_sync<ae::ToolResult>(ae::invoke_tool(unset, held, note_call(), ctx, nullptr));
        check(r.is_error && error_text(r).find("tool.approval") != std::string::npos,
              "T3: invoke_tool on a descriptor with no mode is an error naming the missing choice");
        check(g_body_runs == 0, "T3: the tool's body never ran");

        ae::ToolTable const stated = ae::ToolTable::from_descriptors({host_descriptor(ae::approval_mode::never_require)});
        auto ctx2 = make_ctx();
        auto ok   = ae::test_support::run_task_sync<ae::ToolResult>(ae::invoke_tool(stated, held, note_call(), ctx2, nullptr));
        check(!ok.is_error && g_body_runs == 1, "T3 control: stated never_require, the tool runs");
    }

    // ---- T4: background_task --------------------------------------------------------------------------------------
    {
        // background_task also needs the caller's Background grant; with it held, the refusal is the approval's.
        ae::CapabilitySet const held_bg = ae::CapabilitySet::grant_root({ae::cap::Background{4}});
        ae::ToolTable const unset = ae::ToolTable::from_descriptors({host_descriptor(std::nullopt)});
        std::atomic<int>    completed{0};
        auto refused = ae::test_support::run_task_sync<ae::result<void>>(ae::background_task(
            unset, held_bg, note_call(), make_ctx(), nullptr, 0,
            [&completed](ae::ToolResult, ae::ToolInvocationAudit) { completed.fetch_add(1); }));
        check(!refused.has_value() && refused.error().code == "choice.missing.tool.approval",
              "T4: background_task refuses a descriptor with no mode");

        ae::ToolTable const stated = ae::ToolTable::from_descriptors({host_descriptor(ae::approval_mode::never_require)});
        auto started = ae::test_support::run_task_sync<ae::result<void>>(ae::background_task(
            stated, held_bg, note_call("c2"), make_ctx(), nullptr, 0,
            [&completed](ae::ToolResult, ae::ToolInvocationAudit) { completed.fetch_add(1); }));
        check(started.has_value(), "T4 control: stated never_require, the background task starts");
        for (int i = 0; i < 2000 && completed.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(completed.load() == 1, "T4 control: and completes (the refused one never did)");
    }

    // ---- T5: the session turn -------------------------------------------------------------------------------------
    using Session = art::AgentSession<CallingClient, art::NoSessionState, HostToolProvider>;
    {
        Session s;
        s.initialize("s-t5", kCaller, ae_test::trusted_unbounded());
        std::shared_ptr<CallingClient::State> const state = s.emplace_chat_client().state();
        std::vector<ae::RunEvent> events;
        s.set_run_event_tap([&events](ae::RunEvent const& ev) { events.push_back(ev); });
        g_body_runs = 0;
        auto r = drive(s.start_run(art::StartRun{text_message("q")}));
        check(!r.has_value() && r.error().code == "choice.missing.tool.approval",
              "T5: a turn offering a tool with no approval mode fails with choice.missing.tool.approval");
        check(state->calls == 0, "T5: the model was never called");
        check(g_body_runs == 0, "T5: the tool never ran");
        bool failed_event = false;
        for (ae::RunEvent const& ev : events) {
            auto const* p = std::get_if<ae::run_event_payload::RunFailed>(&ev.payload);
            if (ev.kind == ae::run_event_kind::run_failed && p != nullptr &&
                p->error_code == "choice.missing.tool.approval" && p->stage == "run.tool_approval_missing") {
                failed_event = true;
            }
        }
        check(failed_event, "T5: run_failed carries the code and the stage");
    }
    {
        Session s;
        s.initialize("s-t5c", kCaller, ae_test::trusted_unbounded());
        s.history_provider().mode = ae::approval_mode::never_require;
        std::shared_ptr<CallingClient::State> const state = s.emplace_chat_client().state();
        ae::CapabilitySet const none = ae::CapabilitySet::grant_root({});
        s.set_capabilities(&none);
        g_body_runs = 0;
        auto r = drive(s.start_run(art::StartRun{text_message("q")}));
        check(r.has_value() && state->calls == 2 && g_body_runs == 1,
              "T5 control: the same provider with the mode stated runs the turn and the tool");
    }

    // ---- T6: the registry -----------------------------------------------------------------------------------------
    {
        ae::ToolRegistry registry;
        auto refused = registry.register_tool("host_tool", host_descriptor(std::nullopt), ae::tool_provenance::native);
        check(!refused.has_value() && refused.error().code == "choice.missing.tool.approval",
              "T6: register_tool refuses a descriptor with no approval mode");
        auto ok = registry.register_tool("host_tool", host_descriptor(ae::approval_mode::policy_driven),
                                         ae::tool_provenance::native);
        check(ok.has_value(), "T6 control: the stated twin registers under the same name");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "ALL CHECKS PASSED\n");
    return 0;
}
