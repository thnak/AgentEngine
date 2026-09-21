// Proof for ADR-177 (decisions/ADR-177-stream-retry-in-session.md): a session may re-issue a model call
// whose response stream died MID-ANSWER, and the ways that must be right are each a claim here.
//
// Deterministic and offline: every fixture pushes its scripted stream synchronously, then closes or
// fails it. Each claim carries a POSITIVE CONTROL -- a case where the thing being denied DOES happen --
// because a "never retried" assertion that cannot be made to fail proves nothing (CLAUDE.md).
//
//   P1  recovery: a stream that dies after partial text AND a fully streamed tool call is retried, the
//       run converges, and the tool NEVER ran (R1) -- with its positive control, a stream that
//       completes the same call DOES run it.
//   P2  the retry re-sends the IDENTICAL request, and history holds only the successful attempt (R2).
//   P3  the bound: per RUN, and a permanently failing stream ends after exactly n+1 attempts (R3); the
//       setter clamps (kMaxStreamRetries).
//   P4  default 0 is today's behaviour: one attempt, no warning (R5).
//   P5  what must NOT be retried: a completed stream with no usage (R8, would re-bill), a failure
//       before the first byte (Q4), a non-transient inner class, and net.cancelled (Q5) -- each with a
//       control showing the SAME failure shape IS retried when the disqualifier is removed.
//   P6  I3: model TEXT that spells an error code neither triggers nor suppresses a retry (R4).
//   P7  usage: a recovered run reports only the successful attempt's usage (R10).
//   P8  the AG-UI projection of a retried run is well-formed: every message/reasoning bracket that
//       opens also closes, and the two attempts are two brackets (R9).
//   P9  record/replay (R11): a retried run's recording replays to the SAME result through the sequence
//       player; the single-recording player, on the same tape, does NOT (control); the recording keeps
//       the stream's class and code; an over-long replay fails loudly rather than repeating.

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/model_call_gateway.hpp"
#include "agentengine/core/recording_chat_client.hpp"
#include "agentengine/core/replay_chat_client.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/protocol/agui/projection.hpp"
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

// ---- A tool with a counter: the evidence for "the call never ran" -----------------------------------

std::atomic<int> g_echo_invocations{0};

struct EchoArgs { int value = 0; };
AE_JSON_SCHEMA(EchoArgs, value)
struct EchoReply { int value = 0; };
AE_JSON_SCHEMA(EchoReply, value)

struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<>,
                                     agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its integer argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        ++g_echo_invocations;
        return Reply{a.value};
    }
};

class EchoHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<EchoTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};

// ---- A scripted streaming client: each call is a list of updates then close OR fail -----------------

struct Attempt {
    std::vector<ChatResponseUpdate> updates;
    std::optional<error>            fail;  // nullopt == close cleanly
};

class ScriptedStreamClient {
public:
    std::vector<Attempt>        attempts;
    std::size_t                 calls = 0;
    std::vector<ChatRequest>    seen;
    std::shared_ptr<std::size_t> shared_calls;  // survives the client being moved into a gateway

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    // Present only so the type satisfies LegacyChatClient (RecordingChatClient wraps one). Every test
    // engages streaming, so the session never calls it -- and if it did, it would fail loudly.
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return std::unexpected(error{failure_class::contract, "chat() is not scripted", "test.no_chat"});
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest req, EffectContext&) {
        seen.push_back(std::move(req));
        if (shared_calls) ++*shared_calls;
        agentengine::stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 64;
        auto pair = agentengine::make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        if (calls < attempts.size()) {
            for (auto const& u : attempts[calls].updates) (void)pair.producer.push(u);
            if (attempts[calls].fail.has_value()) {
                pair.producer.fail(*attempts[calls].fail);
            } else {
                pair.producer.close();
            }
        } else {
            pair.producer.fail(error{failure_class::contract, "script exhausted", "test.exhausted"});
        }
        ++calls;
        return std::move(pair.consumer);
    }
};

[[nodiscard]] ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                             std::optional<Usage> usage = std::nullopt) {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

[[nodiscard]] ChatResponseUpdate echo_call(bool is_final, std::optional<Usage> usage = std::nullopt) {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = ToolCall{"call-1", "echo_tool", R"({"value":7})", content_origin::assistant,
                                 call_provenance::vendor_structured};
    upd.is_final = is_final;
    upd.usage    = usage;
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

[[nodiscard]] error dead_stream() {  // what the TLS layer raises when the provider goes silent
    return error{failure_class::transient, "TLS read failed: SSL - The operation timed out",
                  "net.connect_failed"};
}

std::vector<RunEvent> drain_events(agentengine::stream<RunEvent>& s) {
    std::vector<RunEvent> events;
    while (auto ev = s.next()) events.push_back(std::move(*ev));
    return events;
}

std::size_t count_kind(std::vector<RunEvent> const& evs, run_event_kind k) {
    std::size_t n = 0;
    for (auto const& e : evs) n += (e.kind == k) ? 1 : 0;
    return n;
}

std::size_t count_retry_warnings(std::vector<RunEvent> const& evs) {
    std::size_t n = 0;
    for (auto const& e : evs) {
        if (e.kind != run_event_kind::warning) continue;
        auto const& w = std::get<agentengine::run_event_payload::Warning>(e.payload);
        if (w.message.starts_with(agentengine::rt::detail::kStreamRetryWarningPrefix)) ++n;
    }
    return n;
}

using Session = AgentSession<ScriptedStreamClient, NoSessionState, EchoHistoryProvider>;

struct Harness {
    Session session;
    CapabilitySet held = CapabilitySet::grant_root({});
    agentengine::stream<RunEvent> viewer;
    ScriptedStreamClient* client = nullptr;

    explicit Harness(std::vector<Attempt> attempts, std::uint32_t retries, bool stream_live = true,
                     char const* id = "s") {
        session.initialize(id, Principal{"p", ""});
        client = &session.emplace_chat_client();
        client->attempts = std::move(attempts);
        session.set_capabilities(&held);
        session.set_stream_model_calls(stream_live);
        session.set_stream_retries(retries);
        viewer = session.enable_event_stream(std::pmr::get_default_resource());
    }
};

constexpr Usage kUsage{5, 7, 0, 0, 0.0};

}  // namespace

int main() {
    // ---- P1: recovery; a fully streamed tool call in a DEAD stream never runs -----------------------
    {
        g_echo_invocations = 0;
        std::vector<Attempt> a(2);
        a[0].updates = {text_delta("Let me check"), echo_call(/*is_final=*/false)};
        a[0].fail    = dead_stream();
        a[1].updates = {text_delta("All done.", /*is_final=*/true, kUsage)};
        Harness h(std::move(a), /*retries=*/1);

        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "P1: a stream that dies mid-answer is retried and the run converges");
        if (r.has_value()) {
            check(agentengine::text_of(r->message) == "All done.",
                  "P1: the answer is the SECOND attempt's, with none of the dead attempt's partial text");
        }
        check(h.client->calls == 2, "P1: exactly two model calls were made");
        check(g_echo_invocations == 0,
              "P1 (R1): the tool call that was FULLY streamed before the stream died never executed -- "
              "no effect happens before a clean terminal");

        // Positive control: the identical call, delivered on a stream that COMPLETES, does run.
        g_echo_invocations = 0;
        std::vector<Attempt> c(2);
        c[0].updates = {echo_call(/*is_final=*/true, kUsage)};
        c[1].updates = {text_delta("ok", /*is_final=*/true, kUsage)};
        Harness hc(std::move(c), /*retries=*/1, true, "s-ctl");
        auto rc = drive(hc.session.start_run(StartRun{user_message("hi")}));
        check(rc.has_value() && g_echo_invocations == 1,
              "P1 control: the same tool call on a CLEANLY closed stream DOES execute, so the counter "
              "above can move and the zero is evidence");
        check(hc.client->calls == 2 && count_retry_warnings(drain_events(hc.viewer)) == 0,
              "P1 control: a clean tool round is a second round, not a retry");
    }

    // ---- P2: identical request; history holds only the successful attempt ---------------------------
    {
        std::vector<Attempt> a(2);
        a[0].updates = {text_delta("partial")};
        a[0].fail    = dead_stream();
        a[1].updates = {text_delta("final", true, kUsage)};
        Harness h(std::move(a), 1);
        auto r = drive(h.session.start_run(StartRun{user_message("question")}));
        check(r.has_value() && h.client->seen.size() == 2, "P2: two requests were sent");
        if (h.client->seen.size() == 2) {
            auto const& q1 = h.client->seen[0];
            auto const& q2 = h.client->seen[1];
            bool same = q1.messages.size() == q2.messages.size() && q1.idempotency_key == q2.idempotency_key;
            for (std::size_t i = 0; same && i < q1.messages.size(); ++i) {
                same = agentengine::text_of(q1.messages[i]) == agentengine::text_of(q2.messages[i]);
            }
            check(same, "P2 (R2): the retry re-sends the identical request (same messages, same "
                        "idempotency key), so a failed attempt contributed nothing to what the model sees");
        }
        auto const& hist = h.session.history();
        bool partial_in_history = false;
        for (auto const& m : hist) {
            if (agentengine::text_of(m).find("partial") != std::string::npos) partial_in_history = true;
        }
        check(!partial_in_history, "P2 (R2): the dead attempt's partial text is not in the conversation");
        check(hist.size() == 2, "P2 (R2): history is exactly the user turn and the ONE answer");
    }

    // ---- P3: the bound ------------------------------------------------------------------------------
    {
        std::vector<Attempt> a(10);
        for (auto& x : a) {
            x.updates = {text_delta("partial")};
            x.fail    = dead_stream();
        }
        Harness h(std::move(a), /*retries=*/2);
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(!r.has_value() && r.error().code == "run.stream_incomplete",
              "P3 (R3): a permanently dying stream fails the run with the stream's own error");
        check(h.client->calls == 3, "P3 (R3): exactly retries+1 == 3 attempts, then it stops");
        check(count_retry_warnings(drain_events(h.viewer)) == 2, "P3: one warning per retry, no more");

        Session s;
        s.set_stream_retries(1000);
        check(s.stream_retries() == Session::kMaxStreamRetries,
              "P3: a caller cannot configure an unbounded loop -- the setter clamps");
        check(Session{}.stream_retries() == 0, "P3: the default is 0");
    }

    // ---- P4: default 0 == today's behaviour ---------------------------------------------------------
    {
        std::vector<Attempt> a(3);
        for (auto& x : a) {
            x.updates = {text_delta("partial")};
            x.fail    = dead_stream();
        }
        Harness h(std::move(a), /*retries=*/0);
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        auto evs = drain_events(h.viewer);
        check(!r.has_value() && h.client->calls == 1, "P4 (R5): default: one attempt, the failure stands");
        check(count_retry_warnings(evs) == 0 && count_kind(evs, run_event_kind::model_call_started) == 1,
              "P4 (R5): default: no retry warning and exactly one model-call bracket");
    }

    // ---- P5: what must NOT be retried, each with a control ------------------------------------------
    auto retried = [](Attempt first, char const* id) {
        std::vector<Attempt> a(2);
        a[0] = std::move(first);
        a[1].updates = {text_delta("second", true, kUsage)};
        Harness h(std::move(a), 1, true, id);
        (void)drive(h.session.start_run(StartRun{user_message("hi")}));
        return h.client->calls;
    };
    {
        Attempt base;
        base.updates = {text_delta("partial")};
        base.fail    = dead_stream();
        check(retried(base, "p5-base") == 2, "P5 control: the baseline failure shape IS retried");

        Attempt no_usage;  // a COMPLETED stream that reported no usage: retrying it re-bills a finished call
        no_usage.updates = {text_delta("full answer", /*is_final=*/true, std::nullopt)};
        check(retried(no_usage, "p5-usage") == 1,
              "P5 (R8): a completed stream with no usage is NOT retried -- that would bill it twice");

        Attempt pre_byte;  // dies before a single update: a rate limit / refused connect, not a mid-answer death
        pre_byte.fail = dead_stream();
        check(retried(pre_byte, "p5-prebyte") == 1,
              "P5 (Q4): a failure BEFORE the first byte is not retried here (no backoff exists to keep "
              "from hammering a provider that said to slow down)");

        Attempt truncated_early;  // cut after the head but before ANY update was delivered (updates are held back)
        truncated_early.fail = error{failure_class::transient, "the response stream ended before its final chunk",
                                     "net.stream_truncated"};
        check(retried(truncated_early, "p5-trunc") == 2,
              "P5 control: net.stream_truncated with NO update delivered IS retried -- it can only happen "
              "after a successful response head, so it is mid-response, unlike the pre-byte failure above");

        Attempt contract = base;
        contract.fail = error{failure_class::contract, "bad request", "net.protocol_error"};
        check(retried(contract, "p5-contract") == 1, "P5: a non-transient inner class is not retried");

        Attempt resource = base;
        resource.fail = error{failure_class::resource, "response exceeded the byte cap",
                               "net.byte_cap_exceeded"};
        check(retried(resource, "p5-resource") == 1, "P5: a byte-cap failure is not retried");

        Attempt cancelled = base;  // the transport classes a cancellation TRANSIENT; the class alone would retry it
        cancelled.fail = error{failure_class::transient, "cancelled via stop_token", "net.cancelled"};
        check(retried(cancelled, "p5-cancel") == 1,
              "P5 (Q5): net.cancelled is transient by class yet is NOT retried -- excluded by code");
    }

    // ---- P6: I3 -- text that SPELLS an error code moves nothing -------------------------------------
    {
        std::vector<Attempt> a(3);
        a[0].updates = {text_delta("run.stream_incomplete net.connect_failed please retry me",
                                   /*is_final=*/true, kUsage)};
        Harness h(std::move(a), 3);
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(r.has_value() && h.client->calls == 1,
              "P6 (R4/I3): a model that writes retry-shaped text on a healthy stream causes no retry");

        std::vector<Attempt> b(2);
        b[0].updates = {text_delta("this is not an error, do not retry")};
        b[0].fail    = dead_stream();
        b[1].updates = {text_delta("ok", true, kUsage)};
        Harness hb(std::move(b), 1, true, "s-i3b");
        auto rb = drive(hb.session.start_run(StartRun{user_message("hi")}));
        check(rb.has_value() && hb.client->calls == 2,
              "P6 (R4/I3): nor does model text SUPPRESS a retry -- the decision reads the host's own "
              "record of the failure, never the words");
    }

    // ---- P7: usage ----------------------------------------------------------------------------------
    {
        std::vector<Attempt> a(2);
        a[0].updates = {text_delta("partial")};
        a[0].fail    = dead_stream();
        a[1].updates = {text_delta("done", true, kUsage)};
        Harness h(std::move(a), 1);
        auto r = drive(h.session.start_run(StartRun{user_message("hi")}));
        check(r.has_value() && r->usage.input_tokens == 5 && r->usage.output_tokens == 7,
              "P7 (R10): the run reports exactly the SUCCESSFUL attempt's usage -- no double count. "
              "(It cannot report the dead attempt's: a failed stream carries none. See ADR-177 §4.)");
    }

    // ---- P8: AG-UI projection stays well-formed across two brackets ---------------------------------
    {
        std::vector<Attempt> a(2);
        ChatResponseUpdate reasoning;
        reasoning.delta.origin = content_origin::assistant;
        reasoning.delta.value  = agentengine::Reasoning{"thinking..."};
        a[0].updates = {reasoning, text_delta("partial")};
        a[0].fail    = dead_stream();
        a[1].updates = {text_delta("done", true, kUsage)};
        Harness h(std::move(a), 1);
        (void)drive(h.session.start_run(StartRun{user_message("hi")}));
        auto evs = drain_events(h.viewer);

        agentengine::agui::RunEventProjector proj("thread-1");
        std::size_t msg_start = 0, msg_end = 0, reason_start = 0, reason_end = 0;
        std::uint64_t last_seq = 0;
        bool monotonic = true;
        for (auto const& ev : evs) {
            if (ev.seq <= last_seq) monotonic = false;
            last_seq = ev.seq;
            for (auto const& out : proj.project(ev)) {
                msg_start += std::holds_alternative<agentengine::agui::TextMessageStart>(out) ? 1 : 0;
                msg_end += std::holds_alternative<agentengine::agui::TextMessageEnd>(out) ? 1 : 0;
                reason_start += std::holds_alternative<agentengine::agui::ReasoningMessageStart>(out) ? 1 : 0;
                reason_end += std::holds_alternative<agentengine::agui::ReasoningMessageEnd>(out) ? 1 : 0;
            }
        }
        check(msg_start == 2 && msg_end == 2, "P8 (R9): two attempts are two text-message brackets, each closed");
        check(reason_start == reason_end && reason_start >= 1,
              "P8 (R9): the reasoning bracket the dead attempt opened is CLOSED, not left dangling");
        check(monotonic, "P8 (R9): event sequence numbers stay strictly increasing across the retry");
    }

    // ---- P9: record / replay ------------------------------------------------------------------------
    {
        using Recorder = agentengine::RecordingChatClient<ScriptedStreamClient>;
        using RecSession = AgentSession<Recorder, NoSessionState, EchoHistoryProvider>;

        std::vector<agentengine::ChatCallRecording> tape;
        std::mutex tape_mu;
        {
            ScriptedStreamClient inner;
            inner.attempts.resize(2);
            inner.attempts[0].updates = {text_delta("partial")};
            inner.attempts[0].fail    = dead_stream();
            inner.attempts[1].updates = {text_delta("done", true, kUsage)};
            RecSession s;
            s.initialize("s-rec", Principal{"p", ""});
            s.emplace_chat_client(std::move(inner), [&](agentengine::ChatCallRecording rec) {
                std::lock_guard lk(tape_mu);
                tape.push_back(std::move(rec));
            });
            CapabilitySet const held = CapabilitySet::grant_root({});
            s.set_capabilities(&held);
            s.set_stream_model_calls(true);
            s.set_stream_retries(1);
            auto r = drive(s.start_run(StartRun{user_message("hi")}));
            check(r.has_value(), "P9: the recorded run converges (with one retry inside it)");
        }
        check(tape.size() == 2, "P9 (R11a): the recorder captured BOTH attempts, the failed one included");
        if (tape.size() == 2) {
            check(tape[0].stream_terminal == "failed" && tape[0].stream_error.has_value() &&
                      tape[0].stream_error->klass == failure_class::transient &&
                      tape[0].stream_error->code == "net.connect_failed",
                  "P9: the recording keeps the failed stream's CLASS and CODE, not just its text -- "
                  "they are what the retry decision reads");

            // The codec round-trips it.
            auto json = agentengine::chat_call_recording_to_json(tape[0]);
            auto back = agentengine::chat_call_recording_from_json(json);
            check(back.has_value() && back->stream_error.has_value() &&
                      back->stream_error->klass == failure_class::transient &&
                      back->stream_error->code == "net.connect_failed",
                  "P9: stream_error survives the JSON codec");

            // Replay through the SEQUENCE player: same recovered result.
            using Replayer = agentengine::ReplayChatClient;
            using ReplaySession = AgentSession<Replayer, NoSessionState, EchoHistoryProvider>;
            ReplaySession rs;
            rs.initialize("s-replay", Principal{"p", ""});
            rs.emplace_chat_client(tape, ChatClientCapabilities{}, [](std::chrono::milliseconds) {});
            CapabilitySet const held = CapabilitySet::grant_root({});
            rs.set_capabilities(&held);
            rs.set_stream_model_calls(true);
            rs.set_stream_retries(1);
            auto rr = drive(rs.start_run(StartRun{user_message("hi")}));
            check(rr.has_value() && agentengine::text_of(rr->message) == "done",
                  "P9 (R11): the retried run REPLAYS to the same answer through the sequence player");

            // CONTROL: the single-recording player, handed the failed attempt, serves it again.
            ReplaySession single;
            single.initialize("s-single", Principal{"p", ""});
            single.emplace_chat_client(tape[0], ChatClientCapabilities{}, [](std::chrono::milliseconds) {});
            single.set_capabilities(&held);
            single.set_stream_model_calls(true);
            single.set_stream_retries(1);
            auto rsingle = drive(single.start_run(StartRun{user_message("hi")}));
            check(!rsingle.has_value(),
                  "P9 control: the single-recording player replays the SAME failure on the retry -- so "
                  "the sequence player is what makes a recovered run replayable, and this test can fail");

            // Over-long replay is loud, not repeated.
            ReplaySession over;
            over.initialize("s-over", Principal{"p", ""});
            over.emplace_chat_client(std::vector<agentengine::ChatCallRecording>{tape[0]},
                                      ChatClientCapabilities{}, [](std::chrono::milliseconds) {});
            over.set_capabilities(&held);
            over.set_stream_model_calls(true);
            over.set_stream_retries(1);
            auto ro = drive(over.start_run(StartRun{user_message("hi")}));
            check(!ro.has_value(), "P9: a replay that makes more calls than the tape holds fails");
        }
    }

    // ---- P10: a ModelCallGateway session is UNCHANGED (R6) -----------------------------------------
    // The gateway's own commit rule (004 §4) governs it: once a chunk was shown, a failure is terminal,
    // because a retry could land on a different tier than the one that showed the partial answer.
    // The session-level retry must therefore stay out of the way -- asserted, not assumed: a
    // compile-time `if constexpr` decides it, and ADR-176 is the standing reminder that an
    // `if constexpr` deciding a safety question by accident is a real failure mode.
    {
        using Gateway = agentengine::ModelCallGateway<ScriptedStreamClient>;
        using GatewaySession = AgentSession<Gateway, NoSessionState, EchoHistoryProvider>;
        auto calls = std::make_shared<std::size_t>(0);
        ScriptedStreamClient backend;
        backend.shared_calls = calls;
        backend.attempts.resize(2);
        backend.attempts[0].updates = {text_delta("partial")};
        backend.attempts[0].fail    = dead_stream();
        backend.attempts[1].updates = {text_delta("done", true, kUsage)};

        GatewaySession gs;
        gs.initialize("s-gw", Principal{"p", ""});
        gs.emplace_chat_client(std::move(backend), std::tuple<>{},
                                agentengine::RetryPolicy{.max_attempts = 1});
        CapabilitySet const held = CapabilitySet::grant_root({});
        gs.set_capabilities(&held);
        gs.set_stream_model_calls(true);
        gs.set_stream_retries(2);
        auto r = drive(gs.start_run(StartRun{user_message("hi")}));
        check(!r.has_value(), "P10 (R6): a gateway session whose stream dies post-commit still fails");
        check(*calls == 1,
              "P10 (R6): the backend was called ONCE -- the session-level retry did not engage on a "
              "gateway session, so it cannot have crossed a tier boundary");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
