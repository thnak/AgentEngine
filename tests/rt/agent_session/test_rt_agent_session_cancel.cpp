// Proof for ADR-178 (decisions/ADR-178-session-cancellation.md): `AgentSession::cancel()` stops the run it
// is executing, cooperatively, at named checkpoints -- and a canceled run is a STATE (run_canceled, an
// attributable `run.canceled` result), never an abort.
//
// Deterministic and offline. Every claim carries a positive control -- the same shape WITHOUT the cancel,
// which must behave normally -- because "cancel stops it" proves nothing if the run would have stopped
// anyway (CLAUDE.md).
//
//   K1  round boundary: a cancel raised while a tool runs ends the run at the top of the NEXT round -- no
//       second model call -- and the tool itself saw the same signal as `EffectContext::cancellation`.
//   K2  mid-stream: a provider that goes silent is abandoned when the run is canceled (well inside the
//       time the silence would have lasted), and the model-call bracket still closes.
//   K3  a response that arrives after the cancel is dropped BEFORE its tool calls run, its usage still
//       charged (non-streaming path, so the response is really delivered).
//   K4  the source is per RUN: a cancel after a run finished cannot poison the next one.
//   K5  the retry predicate's stop clause (ADR-177), now reachable: a stream that dies while the run is
//       canceled is NOT retried.
//   K6  cancel() with no run in flight is harmless.
//   K7  a canceled run is not reported as a failure: run_canceled is emitted, run_failed is not.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        std::fprintf(stderr, "  FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
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
using agentengine::Message;
using agentengine::Principal;
using agentengine::RunEvent;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::error;
using agentengine::failure_class;
using agentengine::role;
using agentengine::run_event_kind;

// ---- A tool that cancels the run from inside itself, and reports what it saw --------------------------

std::atomic<int>  g_tool_runs{0};
std::atomic<bool> g_tool_saw_cancel{false};
std::function<void()> g_cancel_from_tool;  // set per case; empty == the tool does not cancel

struct ProbeArgs { int value = 0; };
AE_JSON_SCHEMA(ProbeArgs, value)
struct ProbeReply { int value = 0; };
AE_JSON_SCHEMA(ProbeReply, value)

struct ProbeTool : agentengine::Tool<ProbeTool, agentengine::Capabilities<>,
                                      agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "probe_tool";
    static constexpr std::string_view description = "Optionally cancels the run, then reports.";
    using Args = ProbeArgs;
    using Reply = ProbeReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext& ctx) {
        ++g_tool_runs;
        if (g_cancel_from_tool) g_cancel_from_tool();
        g_tool_saw_cancel = ctx.cancellation.stop_requested();
        return Reply{a.value};
    }
};

class ProbeHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<ProbeTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};

// ---- A scripted client: one Step per model call -------------------------------------------------------

constexpr Usage kUsage{5, 7, 0, 0, 0.0};

[[nodiscard]] error dead_stream() {
    return error{failure_class::transient, "TLS read failed: SSL - The operation timed out",
                  "net.connect_failed"};
}

[[nodiscard]] ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                             std::optional<Usage> usage = std::nullopt) {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

[[nodiscard]] ChatResponseUpdate probe_call() {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = ToolCall{"call-1", "probe_tool", R"({"value":7})", content_origin::assistant,
                                 call_provenance::vendor_structured};
    upd.is_final = true;
    upd.usage    = kUsage;
    return upd;
}

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

struct Step {
    std::vector<ChatResponseUpdate> updates;
    std::optional<error>            fail;               // nullopt == close cleanly (unless `hang`)
    bool                            hang = false;       // deliver `updates`, then say NOTHING
    std::function<void()>           on_call;            // runs when the client is asked, before it answers
};

class ScriptedClient {
public:
    std::vector<Step> steps;
    std::size_t       calls = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const {
        ChatClientCapabilities caps;
        caps.tool_calling = true;
        return caps;
    }

    // The non-streaming path: the response really is delivered, so a cancel that lands during the call is
    // seen only by the check AFTER it (K3).
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Step& step = steps.at(calls++);
        if (step.on_call) step.on_call();
        Message m;
        m.role = role::assistant;
        for (auto& u : step.updates) m.content.push_back(u.delta);
        Usage usage = kUsage;
        co_return ChatResponse{std::move(m), usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        Step& step = steps.at(calls++);
        if (step.on_call) step.on_call();
        agentengine::stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 64;
        auto pair = agentengine::make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        for (auto const& u : step.updates) (void)pair.producer.push(u);
        if (step.hang) {
            // Silent, not dead: it fails on its own after a few seconds so that a session which FAILS to
            // honour the cancel ends the test as a failure, not as a hung build. Ends early if the client
            // goes away.
            watchdogs_.emplace_back([p = std::move(pair.producer)](std::stop_token st) mutable {
                for (int i = 0; i < 200 && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!st.stop_requested()) p.fail(dead_stream());
            });
        } else if (step.fail.has_value()) {
            pair.producer.fail(*step.fail);
        } else {
            pair.producer.close();
        }
        return std::move(pair.consumer);
    }

private:
    std::vector<std::jthread> watchdogs_;
};

using Session = AgentSession<ScriptedClient, NoSessionState, ProbeHistoryProvider>;

struct Harness {
    Session session;
    CapabilitySet held = CapabilitySet::grant_root({});
    agentengine::stream<RunEvent> viewer;
    ScriptedClient* client = nullptr;

    explicit Harness(std::vector<Step> steps, std::uint32_t retries = 0, bool stream_live = true) {
        session.initialize("s", Principal{"p", ""});
        client = &session.emplace_chat_client();
        client->steps = std::move(steps);
        session.set_capabilities(&held);
        session.set_stream_model_calls(stream_live);
        session.set_stream_retries(retries);
        viewer = session.enable_event_stream(std::pmr::get_default_resource());
    }
};

struct Counts {
    std::size_t canceled = 0, failed = 0, discarded = 0, call_started = 0, call_finished = 0;
};
Counts tally(Harness& h) {
    Counts c;
    while (auto ev = h.viewer.next()) {
        switch (ev->kind) {
            case run_event_kind::run_canceled: ++c.canceled; break;
            case run_event_kind::run_failed: ++c.failed; break;
            case run_event_kind::model_output_discarded: ++c.discarded; break;
            case run_event_kind::model_call_started: ++c.call_started; break;
            case run_event_kind::model_call_finished: ++c.call_finished; break;
            default: break;
        }
    }
    return c;
}

}  // namespace

int main() {
    // ---- K1: a cancel raised inside a tool ends the run at the top of the next round -----------------
    {
        g_tool_runs = 0;
        g_tool_saw_cancel = false;
        std::vector<Step> steps(2);
        steps[0].updates = {probe_call()};
        steps[1].updates = {text_delta("never asked", /*is_final=*/true, kUsage)};
        Harness h(std::move(steps), 0);
        g_cancel_from_tool = [&h] { h.session.cancel(); };
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        g_cancel_from_tool = nullptr;
        check(!r.has_value() && r.error().code == "run.canceled", "K1: the run ends run.canceled");
        check(!r.has_value() && r.error().klass == failure_class::fatal, "K1: it is a fatal end, not a transient one a caller might retry");
        check(h.client->calls == 1, "K1: no SECOND model call was made after the cancel");
        check(g_tool_runs == 1, "K1: the tool that was already running finished (cooperative, not preemptive)");
        check(g_tool_saw_cancel, "K1: and it saw the cancel as EffectContext::cancellation");
        auto c = tally(h);
        check(c.canceled == 1 && c.failed == 0, "K7: run_canceled is emitted once, run_failed not at all");

        // K1 control: the same script with NO cancel runs to its answer.
        g_tool_runs = 0;
        g_tool_saw_cancel = false;
        std::vector<Step> steps2(2);
        steps2[0].updates = {probe_call()};
        steps2[1].updates = {text_delta("done", /*is_final=*/true, kUsage)};
        Harness h2(std::move(steps2), 0);
        auto r2 = drive(h2.session.start_run(StartRun{user_message("hi")}));
        check(r2.has_value() && h2.client->calls == 2 && g_tool_runs == 1 && !g_tool_saw_cancel,
              "K1 control: without a cancel the same run makes both calls and the tool sees no cancel");
        auto c2 = tally(h2);
        check(c2.canceled == 0, "K7 control: an uncanceled run emits no run_canceled");
    }

    // ---- K2: a silent provider is abandoned when the run is canceled -----------------------------------
    {
        std::vector<Step> steps(1);
        steps[0].updates = {text_delta("Let me think")};
        steps[0].hang = true;
        Harness h(std::move(steps), 0);
        std::jthread canceller([&h](std::stop_token) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            h.session.cancel();
        });
        auto const t0 = std::chrono::steady_clock::now();
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        canceller.join();
        check(!r.has_value() && r.error().code == "run.canceled",
              "K2: a run canceled mid-stream ends run.canceled, not run.stream_incomplete");
        check(ms < 2500, "K2: and it did so well before the provider's silence would have ended it");
        auto c = tally(h);
        check(c.call_started == 1 && c.call_finished == 1,
              "K2: the model-call bracket still closed (a consumer's per-call state is not left open)");
        check(c.canceled == 1 && c.failed == 0, "K7: mid-stream cancel emits run_canceled, not run_failed");
    }

    // ---- K3: a response delivered after the cancel is dropped before its tools run ----------------------
    {
        g_tool_runs = 0;
        std::vector<Step> steps(1);
        steps[0].updates = {probe_call()};
        Harness h(std::move(steps), 0, /*stream_live=*/false);
        h.client->steps[0].on_call = [&h] { h.session.cancel(); };
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(!r.has_value() && r.error().code == "run.canceled", "K3: the run ends run.canceled");
        check(g_tool_runs == 0, "K3: the tool the canceled response asked for NEVER ran");
        check(h.session.run_tokens_consumed() == kUsage.input_tokens + kUsage.output_tokens,
              "K3: the response's usage was still charged -- the model call really happened");
        check(h.session.history().size() == 1, "K3: nothing from the dropped response reached history");

        // K3 control: the same delivered response with no cancel DOES run its tool.
        g_tool_runs = 0;
        std::vector<Step> steps2(2);
        steps2[0].updates = {probe_call()};
        steps2[1].updates = {text_delta("done")};
        Harness h2(std::move(steps2), 0, /*stream_live=*/false);
        auto r2 = drive(h2.session.start_run(StartRun{user_message("hi")}));
        check(r2.has_value() && g_tool_runs == 1, "K3 control: uncanceled, the same response's tool runs");
    }

    // ---- K4: the cancel source is per run ---------------------------------------------------------------
    {
        std::vector<Step> steps(2);
        steps[0].updates = {text_delta("first", /*is_final=*/true, kUsage)};
        steps[1].updates = {text_delta("second", /*is_final=*/true, kUsage)};
        Harness h(std::move(steps), 0);
        auto r1 = drive(h.session.start_run(StartRun{user_message("one")}));
        check(r1.has_value(), "K4 setup: the first run completes");
        h.session.cancel();  // aimed at a run that is already over
        check(h.session.cancellation_token().stop_requested(),
              "K4 setup: the stale cancel is recorded against the finished run's source");
        auto r2 = drive(h.session.start_run(StartRun{user_message("two")}));
        check(r2.has_value() && agentengine::text_of(r2->message) == "second",
              "K4: a cancel after a run finished does NOT cancel the next run");
        check(!h.session.cancellation_token().stop_requested(), "K4: the next run began with a fresh source");
    }

    // ---- K5: the retry predicate's stop clause (ADR-177) is now reachable ---------------------------------
    // (a) the cancel lands BEFORE the stream is drained: the drain never consumes it.
    {
        std::vector<Step> steps(2);
        steps[0].updates = {text_delta("Let me check")};
        steps[0].fail = dead_stream();
        steps[1].updates = {text_delta("All done.", /*is_final=*/true, kUsage)};
        Harness h(std::move(steps), /*retries=*/2);
        h.client->steps[0].on_call = [&h] { h.session.cancel(); };
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(!r.has_value() && r.error().code == "run.canceled",
              "K5a: a cancel that lands before the stream is drained ends run.canceled");
        check(h.client->calls == 1 && h.session.stream_retries_used() == 0,
              "K5a: no retry was made, though retries were available");
    }
    // (b) the cancel lands WHILE the stream is drained -- from the run's own thread, in the event tap, the
    // one deterministic way to land it after the drain's last look at the token -- and the stream then
    // dies. This is the shape that reaches the predicate's stop clause and run_rounds' own re-check.
    {
        std::vector<Step> steps(2);
        steps[0].updates = {text_delta("Let me check")};
        steps[0].fail = dead_stream();
        steps[1].updates = {text_delta("All done.", /*is_final=*/true, kUsage)};
        Harness h(std::move(steps), /*retries=*/2);
        bool cancelled_once = false;
        h.session.set_run_event_tap([&h, &cancelled_once](RunEvent const& ev) {
            if (ev.kind == run_event_kind::model_delta && !cancelled_once) {
                cancelled_once = true;
                h.session.cancel();
            }
        });
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(!r.has_value() && r.error().code == "run.canceled",
              "K5b: a stream that dies after a cancel landed mid-drain ends run.canceled, not run.chat_failed");
        check(h.client->calls == 1 && h.session.stream_retries_used() == 0,
              "K5b: it was NOT retried, though retries were available and the failure is retryable");
        auto c = tally(h);
        check(c.discarded == 0 && c.canceled == 1 && c.failed == 0,
              "K5b: nothing was declared discarded, and the run ended as canceled, not failed");

        // K5 control: the same dead stream with no cancel IS retried and converges.
        std::vector<Step> steps2(2);
        steps2[0].updates = {text_delta("Let me check")};
        steps2[0].fail = dead_stream();
        steps2[1].updates = {text_delta("All done.", /*is_final=*/true, kUsage)};
        Harness h2(std::move(steps2), /*retries=*/2);
        auto r2 = drive(h2.session.start_run(StartRun{user_message("hi")}));
        check(r2.has_value() && h2.client->calls == 2 && h2.session.stream_retries_used() == 1,
              "K5 control: without the cancel the identical failure is retried and the run converges");
    }

    // ---- K6: cancel() with no run in flight is harmless ------------------------------------------------
    {
        std::vector<Step> steps(1);
        steps[0].updates = {text_delta("fine", /*is_final=*/true, kUsage)};
        Harness h(std::move(steps), 0);
        h.session.cancel();  // before any run has ever started
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "K6: a cancel before the first run does not affect it");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
