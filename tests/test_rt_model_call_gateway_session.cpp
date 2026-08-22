// ADR-037 Phase 2: porting G5 (test_model_call_gateway.cpp's own "AgentSession integration" block)
// onto agentengine::rt::AgentSession directly -- the one case in that file that actually drove an
// AgentSession/quark::TestKit at all (G1-G4/G6-G9 exercise ModelCallGateway/MiddlewareModelCallGateway
// standalone, with no AgentSession involvement whatsoever, and are left completely untouched in
// test_model_call_gateway.cpp -- confirmed by reading that file: G1-G4/G6-G9's own code never
// references agent_session.hpp or quark::TestKit, only G5's block does).
//
// Kept as its own file, separate from test_rt_agent_session_real_backend.cpp, on purpose: that file's
// two ported claims both need a real canned local HTTP server, a real OpenAIChatClient, and the
// AGENTENGINE_WITH_HTTPS build gate. This file needs none of that -- `ModelCallGatewayLike` is a
// DIFFERENT shape of ChatClientT than a raw `ChatClient` conformer (core/chat_client.hpp's own
// `ModelCallGatewayLike` concept), driven here entirely through a deterministic, offline, scripted
// backend (`ScriptedGatewayBackend`, copied verbatim from test_model_call_gateway.cpp's own fixture --
// this suite's established "no shared test helpers" discipline). Folding this into the real-backend
// file would blur two genuinely different integration questions into one file.
//
// `ModelCallGateway<Primary, Fallback...>::call()` already returns `agentengine::task<result<
// ChatResponse>>` -- per rt/agent_session.hpp's own file banner, that alias resolves to
// `agentengine::rt::task<result<ChatResponse>>` for any non-void T (core/task.hpp's per-T split), so
// `ModelCallGateway<ScriptedGatewayBackend>` already satisfies `ModelCallGatewayLike` against
// `rt::AgentSession` with ZERO adapter code needed -- the same "no per-conformer changes needed" claim
// rt/agent_session.hpp's own banner already established for the coroutine-type layer generally.
//
// Ported claim:
//   G5 -- AgentSession integration: rt::AgentSession<ModelCallGateway<...>, ...> converges a real
//         start_run() with zero model_delta events (the accepted, named ADR-036 trade) and fires the
//         ADR-036 gateway warning.
//
// New claim (unified-streaming-design-draft.md §3, Piece A):
//   G6 -- the SAME gateway-typed session, with set_stream_model_calls(true): real, live model_delta
//         events DO fire now (proving run_model_call()'s corrected nested-if-constexpr dispatch
//         actually reaches call_stream(), not just that it compiles), the run still converges to the
//         same final text, and start_run()'s own gateway warning names the narrower streaming trade
//         instead of the old blanket "no live model_delta events" claim.

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/model_call_gateway.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;

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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- this fixture's chat_stream() closes/fails the stream
// synchronously, never genuinely parks.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// ---- Copied verbatim from test_model_call_gateway.cpp's own fixtures (this suite's own "no shared
// test helpers" discipline) -- only what G5 itself actually needs: a scripted backend with a real
// chat_stream(), a text_delta() builder, a zero-jitter source, and a fast retry policy. G1-G4/G6-G9's
// OWN copies in test_model_call_gateway.cpp are untouched by this file's existence. --------------------
struct ScriptedOutcome {
    bool succeed = true;
    std::vector<agentengine::ChatResponseUpdate> updates;
    agentengine::failure_class fail_klass = agentengine::failure_class::fatal;

    static ScriptedOutcome ok(std::vector<agentengine::ChatResponseUpdate> u) {
        return ScriptedOutcome{true, std::move(u), {}};
    }
};

class ScriptedGatewayBackend {
public:
    std::vector<ScriptedOutcome> outcomes;

    ScriptedGatewayBackend()
        : call_count_(std::make_shared<std::size_t>(0)),
          observed_idempotency_keys_(std::make_shared<std::vector<std::string>>()) {}

    [[nodiscard]] std::size_t call_count() const { return *call_count_; }

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }

    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest const&,
                                                                              agentengine::EffectContext&) {
        co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                       "this fixture only implements chat_stream()",
                                                       "test.no_chat"});
    }

    agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest const& request,
                                                                        agentengine::EffectContext&) {
        agentengine::stream_config<agentengine::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = agentengine::make_stream<agentengine::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        observed_idempotency_keys_->push_back(request.idempotency_key.value_or(std::string{}));
        std::size_t const call_count = *call_count_;
        if (call_count < outcomes.size()) {
            ScriptedOutcome const& o = outcomes[call_count];
            if (o.succeed) {
                for (auto const& upd : o.updates) {
                    auto pushed = pair.producer.push(upd);
                    (void)pushed;
                }
                pair.producer.close();
            } else {
                pair.producer.fail(
                    agentengine::error{o.fail_klass, "scripted_failure", "test.scripted_failure"});
            }
        } else {
            pair.producer.fail(agentengine::error{agentengine::failure_class::fatal,
                                                    "no more scripted outcomes", "test.no_more_outcomes"});
        }
        ++*call_count_;
        return std::move(pair.consumer);
    }

private:
    std::shared_ptr<std::size_t> call_count_;
    std::shared_ptr<std::vector<std::string>> observed_idempotency_keys_;
};
static_assert(agentengine::ChatClient<ScriptedGatewayBackend>);

[[nodiscard]] agentengine::ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                                            std::optional<agentengine::Usage> usage = std::nullopt) {
    agentengine::ChatResponseUpdate upd;
    upd.delta.origin = agentengine::content_origin::assistant;
    upd.delta.value  = agentengine::Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

double no_jitter() { return 0.0; }

agentengine::RetryPolicy fast_retry_policy() {
    agentengine::RetryPolicy p;
    p.max_attempts = 3;
    p.base_delay = std::chrono::milliseconds(1);
    p.max_delay = std::chrono::milliseconds(1);
    p.jitter_fraction = 0.0;
    return p;
}

// G5's own fixture: the minimal ContextProvider the original used, ported verbatim.
struct GatewayHistoryProvider {
    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, agentengine::EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<GatewayHistoryProvider>);

[[nodiscard]] agentengine::Message user_message(std::string text) {
    agentengine::Message m;
    m.role = agentengine::role::user;
    agentengine::ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value = agentengine::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    // ---- G5: AgentSession integration -- ModelCallGatewayLike plugs into ChatClientT directly --------
    using GatewayT = agentengine::ModelCallGateway<ScriptedGatewayBackend>;
    using Session = AgentSession<GatewayT, NoSessionState, GatewayHistoryProvider>;
    static_assert(agentengine::ModelCallGatewayLike<GatewayT>,
                  "G5 setup: ModelCallGateway<ScriptedGatewayBackend> satisfies ModelCallGatewayLike "
                  "with zero adapter code, per rt/agent_session.hpp's own coroutine-type-layer claim");

    Session session;
    ScriptedGatewayBackend primary;
    primary.outcomes = {
        ScriptedOutcome::ok({text_delta("gateway-routed answer", true, agentengine::Usage{4, 2, 0, 0, 0.0})}),
    };
    session.emplace_chat_client(primary, std::make_tuple(), fast_retry_policy(), agentengine::BreakerConfig{},
                                 GatewayT::JitterSource(&no_jitter));
    session.initialize("s-g5", agentengine::Principal{"p", ""});
    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({});
    session.set_capabilities(&held);

    auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

    agentengine::result<agentengine::rt::AgentResponse> r =
        drive(session.start_run(StartRun{user_message("hello")}));
    check(r.has_value(), "G5: a gateway-backed rt::AgentSession run converges");
    if (r.has_value()) {
        check(agentengine::text_of(r->message) == "gateway-routed answer",
              "G5: the gateway's response reaches AgentResponse");
    }

    std::vector<agentengine::RunEvent> events;
    while (auto ev = viewer.next()) events.push_back(std::move(*ev));
    std::size_t delta_count = 0;
    bool saw_gateway_warning = false;
    for (auto const& ev : events) {
        if (ev.kind == agentengine::run_event_kind::model_delta) ++delta_count;
        if (ev.kind == agentengine::run_event_kind::warning) {
            auto const& p = std::get<agentengine::run_event_payload::Warning>(ev.payload);
            if (p.message.find("ModelCallGateway") != std::string::npos) saw_gateway_warning = true;
        }
    }
    check(delta_count == 0,
          "G5: zero model_delta events fire for a gateway-routed round -- the accepted, named trade (a "
          "retried/failed-over/middleware-reviewed attempt cannot be shown live)");
    check(saw_gateway_warning,
          "G5: the ADR-036 gateway warning fires, naming the trade -- a visible fact about the run, not "
          "a silent one, matching ADR-034's own established pattern");

    // ---- G6: the SAME gateway type, streaming enabled -- real live model_delta events now fire -------
    {
        static_assert(agentengine::ModelCallGatewayStreamLike<GatewayT>,
                      "G6 setup: ModelCallGateway<ScriptedGatewayBackend> satisfies "
                      "ModelCallGatewayStreamLike once call_stream() exists on it unconditionally");

        Session session2;
        ScriptedGatewayBackend primary2;
        primary2.outcomes = {
            ScriptedOutcome::ok({text_delta("live "), text_delta("streamed answer", true,
                                                                   agentengine::Usage{4, 2, 0, 0, 0.0})}),
        };
        session2.emplace_chat_client(primary2, std::make_tuple(), fast_retry_policy(),
                                      agentengine::BreakerConfig{}, GatewayT::JitterSource(&no_jitter));
        session2.initialize("s-g6", agentengine::Principal{"p", ""});
        session2.set_capabilities(&held);
        session2.set_stream_model_calls(true);

        auto viewer2 = session2.enable_event_stream(std::pmr::get_default_resource());

        agentengine::result<agentengine::rt::AgentResponse> r2 =
            drive(session2.start_run(StartRun{user_message("hello")}));
        check(r2.has_value(), "G6: a streaming, gateway-backed run converges");
        if (r2.has_value()) {
            check(agentengine::text_of(r2->message) == "live streamed answer",
                  "G6: the accumulated text matches exactly what live delta pushes reconstruct");
        }

        std::vector<agentengine::RunEvent> events2;
        while (auto ev = viewer2.next()) events2.push_back(std::move(*ev));
        std::string joined_deltas;
        std::size_t delta_count2 = 0;
        bool saw_streaming_warning = false;
        for (auto const& ev : events2) {
            if (ev.kind == agentengine::run_event_kind::model_delta) {
                ++delta_count2;
                auto const& d = std::get<agentengine::run_event_payload::ModelDelta>(ev.payload);
                if (auto const* t =
                        std::get_if<agentengine::run_event_payload::ModelTextDelta>(&d.value)) {
                    joined_deltas += t->text;
                }
            }
            if (ev.kind == agentengine::run_event_kind::warning) {
                auto const& p = std::get<agentengine::run_event_payload::Warning>(ev.payload);
                if (p.message.find("streaming enabled") != std::string::npos) saw_streaming_warning = true;
            }
        }
        check(delta_count2 == 2,
              "G6: real model_delta events fire for a gateway-routed round once call_stream() is "
              "actually reached -- proving the dispatch fix (Rev 7, Finding 4-new) works at runtime, "
              "not just that it compiles");
        check(joined_deltas == "live streamed answer",
              "G6: the live-pushed deltas, joined in order, match the accumulated final text exactly");
        check(saw_streaming_warning,
              "G6: start_run()'s own warning names the NARROWER streaming trade (Finding 5-new) -- not "
              "the old blanket 'no live model_delta events' claim, which would now be false");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_model_call_gateway_session: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_model_call_gateway_session: %d FAILURE(S)\n", g_failures);
    return 1;
}
