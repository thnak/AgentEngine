// Live, end-to-end HITL (human-in-the-loop / suspend-for-approval, ADR-029) probes against a REAL
// remote model over OpenRouter. Every existing suspend/approval test (test_rt_agent_session_
// suspend_approval.cpp, examples/05_human_approval.cpp) drives a SCRIPTED ChatClientT -- the model's
// tool-calling behavior is dictated by the test, never genuinely decided by an LLM. This file is the
// first to drive `AgentSession`'s real suspend-for-approval mechanism from a REAL model's own tool
// calls, the same "canned server proves plumbing, real server proves the bytes are actually
// well-formed" gap test_openrouter_live_e2e.cpp's own top comment names, applied to the approval
// seam specifically instead of the raw ChatClient layer.
//
// WHAT THIS FILE IS ACTUALLY HUNTING FOR (found by reading agent_session.hpp's real suspend/resume
// code before writing a single case here, then confirmed live):
//
//   BUG-1 (misleading approval_requested fan-out): `run_rounds()`'s suspend pre-check computes
//   `any_needs_approval` correctly (only a call that resolves to `approval_outcome::needs_decider`
//   counts) -- but the loop that actually EMITS `approval_requested` events, right below it, iterates
//   over ALL of `calls`, not just the ones that needed a decider. A round that mixes one gated call
//   with one perfectly ordinary, never-gated call fires `approval_requested` for BOTH call_ids. Any
//   host surface built on this event stream (the AG-UI `RunEventProjector` included: `approval_
//   requested`'s own case in protocol/agui/projection.hpp projects EVERY one of them into its own
//   `Interrupt`) tells a human "this needs your approval" for a call that never actually did.
//
//   BUG-2 (collateral denial): `resolve_interaction()`'s `!request.approved` branch (agent_session.hpp)
//   applies `make_denial_result(call.call_id, "denied by operator", ...)` to EVERY ToolCall in
//   `pending_calls` -- i.e. every call in the round that suspended -- not just the one(s) that
//   actually needed a decider. A human denying the ONE gated call in a mixed round silently blocks an
//   unrelated, never-gated call too, and that call's synthetic tool result FALSELY tells the model
//   (and, downstream, the user) it was "denied by operator" -- a fabricated explanation for a call no
//   operator was ever asked about.
//
//   BUG-3 (no-explanation approval surface): neither `Interaction` (core/interaction.hpp) nor
//   `run_event_payload::ApprovalRequested`/`ApprovalResolved` (core/run_event.hpp) carries the
//   pending call's tool_name or arguments -- only `call_id`/`interaction_id`. A human asked to
//   approve or deny has, from the public suspend surface alone, no way to learn WHAT they are being
//   asked to approve. The only route to that information is reaching into `session.history()` and
//   re-deriving it via `tool_calls_of(history.back())` -- an internal convention this file has to
//   replicate by hand, not a documented, supported query.
//
// BUG-1 and BUG-3 are provable by reading the type definitions alone (this file still demonstrates
// them against REAL model output, since a live tool call is what a real caller would actually see).
// BUG-2 needs a real model to genuinely produce two parallel tool calls -- one gated, one not -- in
// ONE round, which is why it belongs here rather than in the offline suite.
//
// Also probes the more informal bug categories named alongside this file's own motivating request:
//   "mid-conversation drop": does history_ (and hence what a real model can recall) survive a full
//   suspend -> resolve_interaction -> resume round trip intact, across MULTIPLE start_run() calls on
//   the same session -- or does anything get silently dropped/reordered across that boundary?
//   "no-explanation": when a real model's gated action is DENIED, does its own next reply actually
//   tell the user why nothing happened -- or does it silently drop the subject? Model prose is
//   nondeterministic (I5), so this is reported as a note for a human to read, never a hard assertion.
//
// I5 (nondeterminism crosses a recorded seam) is respected the same way test_rt_agent_session_live_
// multitool_e2e.cpp respects it: gated on the same environment variables, labelled `live-network`,
// hard assertions are STRUCTURAL (which tools ran, what the event stream/history actually contain)
// except where a deliberately unguessable token (HITL-5's magic phrase, matching MT-1's own "un-round
// number" precedent) makes a content check load-bearing rather than a guess.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4) -- identical environment-variable contract as
// test_rt_agent_session_live_multitool_e2e.cpp: AGENTENGINE_OPENROUTER_API_KEY (required, else SKIP),
// AGENTENGINE_OPENROUTER_MODEL (optional), AGENTENGINE_OPENROUTER_HOST (optional).

#ifdef AGENTENGINE_WITH_HTTPS

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResolveInteraction;
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

// Tracks BUG-1/BUG-2 regression checks SEPARATELY from g_failures -- these are expected to currently
// fail, since they pin known, already-documented pre-existing product bugs (this file's own top
// comment), not this test's own correctness. Folding them into g_failures/the process exit code would
// make a genuine regression anywhere else in this file indistinguishable from "the known bug is still
// there" -- a maintainer or CI gate reading only pass/fail could never tell the two apart.
int g_known_bug_failures = 0;
void known_bug_check(bool cond, char const* what) {
    if (!cond) {
        ++g_known_bug_failures;
        std::fprintf(stderr, "FAIL (known pre-existing bug, NOT a regression in this test): %s\n", what);
    } else {
        std::fprintf(stderr,
                     "  ok (this bug appears FIXED now -- update this file's own top-comment claim "
                     "about it): %s\n",
                     what);
    }
}
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
// OpenRouter's dashboard Activity view groups/labels rows by this (the `X-Title` header), NOT by the
// `user` field (`end_user_id`, configure_session's own `id` param below) -- confirmed directly against
// a real run: without a per-file X-Title, every case here reads as an anonymous, unlabeled request no
// matter how distinct its `end_user_id` is. `end_user_id` still does its real job (OpenRouter's
// prompt-caching affinity, session.hpp is threaded per-case for exactly that); this is the SEPARATE
// field that makes this file's own traffic findable in the dashboard at all.
constexpr char const* kXTitle = "AgentEngine: hitl-live-e2e";
constexpr int kMaxRounds = 6;

// ---- Two tools: one gated (always_require), one ordinary (never gated) --------------------------

struct SendArgs { std::string recipient; std::string body; };
AE_JSON_SCHEMA(SendArgs, recipient, body)
struct SendReply { bool sent = false; };
AE_JSON_SCHEMA(SendReply, sent)

[[nodiscard]] bool& send_invoked_log() {
    static bool invoked = false;
    return invoked;
}

struct SendMessageTool
    : Tool<SendMessageTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "send_message";
    static constexpr std::string_view description =
        "Sends a message to a recipient. Requires human approval before it actually sends.";
    using Args = SendArgs;
    using Reply = SendReply;
    static result<Reply> invoke(Args, EffectContext&) {
        send_invoked_log() = true;
        return Reply{true};
    }
};

struct GetWeatherArgs { std::string location; };
AE_JSON_SCHEMA(GetWeatherArgs, location)
struct GetWeatherReply { double temp_c = 0.0; };
AE_JSON_SCHEMA(GetWeatherReply, temp_c)

[[nodiscard]] bool& weather_invoked_log() {
    static bool invoked = false;
    return invoked;
}

// Deliberately ORDINARY -- no Approval<> override at all, so this tool's default approval_mode never
// requires a human. The whole point of HITL-4 is that this tool's own execution should NEVER be
// gated on anything a human decides about SendMessageTool.
struct GetWeatherTool : Tool<GetWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description = "Get the current weather for a city, in Celsius.";
    using Args = GetWeatherArgs;
    using Reply = GetWeatherReply;
    static result<Reply> invoke(Args, EffectContext&) {
        weather_invoked_log() = true;
        return Reply{13.7};
    }
};

class TwoToolHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<SendMessageTool, GetWeatherTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<TwoToolHistoryProvider>);

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// text_of(Message const&) comes from agentengine/core/tool_call_extraction.hpp (already included) --
// no local redefinition here, which would only collide with it under `using namespace agentengine`.

[[nodiscard]] bool has_text(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Text>(item.value)) return true;
    }
    return false;
}

// Case-insensitive substring search -- only ever used for a NOTE, never a hard check, on real model
// prose (I5).
[[nodiscard]] bool icontains(std::string const& haystack, std::string needle) {
    std::string h = haystack;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(needle) != std::string::npos;
}

using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;
using Session = AgentSession<RealClient, NoSessionState, TwoToolHistoryProvider>;

// Shared setup every case below repeats, IN PLACE on a default-constructed `Session` the caller
// already owns -- `AgentSession` is neither copyable nor movable (it embeds a `stream_producer`),
// so this cannot return one by value.
void configure_session(Session& session, std::string const& id, std::string const& host,
                        std::string const& model, InMemorySecretStore& store,
                        ChatClientCapabilities const& caps, CapabilitySet const& held) {
    // `id` is passed as this test CASE's own stable id (one Session -> one conversation, every request
    // across turns A/B/C in HITL-5 included) -- to BOTH `end_user_id` (OpenAI/Anthropic abuse-tracking
    // only) and `session_id` (docs/research/2026-08-21-openrouter-session-id-header.md: OpenRouter's
    // OWN prompt-cache sticky-routing key, sent as the `x-session-id` header -- a DIFFERENT field from
    // `end_user_id`, which that vendor does not use for cache routing at all, correcting this file's
    // own earlier claim that it did). A fresh/random value per call would defeat caching for every
    // multi-turn case in this file.
    session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store, kPathPrefix,
                                 sandbox::resolve_host, /*ca=*/std::string{}, /*http_referer=*/std::string{},
                                 /*x_title=*/kXTitle, /*end_user_id=*/id, /*seed=*/std::nullopt,
                                 /*transport=*/sandbox::ProviderTransport::tls,
                                 /*scan_response_format_leaks=*/false, /*session_id=*/id);
    session.initialize(id, Principal{"live-hitl-e2e-principal", ""}, /*token_budget=*/std::nullopt,
                        /*max_turns=*/static_cast<std::uint64_t>(kMaxRounds));
    session.set_capabilities(&held);
    session.set_suspend_for_approval(true);
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "test_rt_agent_session_hitl_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_rt_agent_session_hitl_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    // Both tools declare an empty capability ceiling (Capabilities<>) -- the ONLY real grant needed
    // is cap::Secret, for the real OpenAIChatClient to resolve its own API key at the point of use.
    CapabilitySet const held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.parallel_tool_calls = true;
    caps.max_output_tokens = 1024;

    // ==================== HITL-1: baseline suspend + APPROVE against a real model ===================
    {
        send_invoked_log() = false;
        Session session;
        configure_session(session, "hitl1", host, model, store, caps, held);

        Message const ask = user_message(
            "Use the send_message tool to message \"team@example.com\" with the body \"Ship it, "
            "HITL-1.\" Call the tool now, do not ask for confirmation yourself -- the system will "
            "handle that.");
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(session.start_run(StartRun{ask}));
        check(!r1.has_value(),
              "HITL-1: start_run() fails while a REAL model's own gated tool call awaits approval");
        if (!r1.has_value()) note("HITL-1 r1 error", r1.error().code + ": " + r1.error().message);
        check(!send_invoked_log(), "HITL-1: send_message's invoke() was not reached before approval");
        check(session.has_open_interactions(), "HITL-1: a real Interaction opened");

        bool saw_approval_requested = false;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::approval_requested) saw_approval_requested = true;
        }
        check(saw_approval_requested, "HITL-1: approval_requested fired for the real model's tool call");

        if (session.has_open_interactions()) {
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            auto r2 = drive(session.resolve_interaction(ResolveInteraction{interaction_id, true, std::nullopt}));
            check(r2.has_value(), "HITL-1: approving resumes the run and it converges against the real model");
            check(send_invoked_log(), "HITL-1: send_message's invoke() ran for real after approval");
            check(!session.has_open_interactions(), "HITL-1: the interaction closed");
            if (r2.has_value()) note("HITL-1 final reply", text_of(r2->message));
        }
    }

    // ==================== HITL-2: baseline suspend + DENY -- probes "no-explanation" =================
    {
        send_invoked_log() = false;
        Session session;
        configure_session(session, "hitl2", host, model, store, caps, held);

        Message const ask = user_message(
            "Use the send_message tool to message \"team@example.com\" with the body \"Ship it, "
            "HITL-2.\" Call the tool now, do not ask for confirmation yourself -- the system will "
            "handle that.");
        auto r1 = drive(session.start_run(StartRun{ask}));
        check(!r1.has_value(), "HITL-2: the run suspends the same way HITL-1's did");
        check(session.has_open_interactions(),
              "HITL-2: a real Interaction opened -- without this, start_run() could have failed for "
              "any OTHER reason (a transient provider error, a regression in the suspend path itself) "
              "and the deny-path body below would silently never run, with this case still reporting "
              "green");

        if (session.has_open_interactions()) {
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            auto r2 =
                drive(session.resolve_interaction(ResolveInteraction{interaction_id, false, std::nullopt}));
            check(r2.has_value(), "HITL-2: denying still lets the run converge (a denial is an ordinary "
                                    "tool error fed back, not a run failure)");
            check(!send_invoked_log(), "HITL-2: send_message's invoke() was NEVER called after denial");
            if (r2.has_value()) {
                std::string const reply = text_of(r2->message);
                note("HITL-2 final reply (after denial)", reply);
                bool const explains = icontains(reply, "approv") || icontains(reply, "denied") ||
                                       icontains(reply, "unable") || icontains(reply, "couldn't") ||
                                       icontains(reply, "cannot") || icontains(reply, "sorry") ||
                                       icontains(reply, "not sent") || icontains(reply, "didn't send");
                if (!explains) {
                    std::fprintf(stderr,
                                 "  ?? NOTE (possible \"no-explanation\" bug): after a real denial, the "
                                 "model's reply above does not appear to mention the action was denied "
                                 "or unsent -- a user reading only this reply may not learn their "
                                 "request silently failed. Not asserted as a hard failure (I5: model "
                                 "prose is nondeterministic), but worth a human's attention.\n");
                } else {
                    note("HITL-2 explanation check", "reply appears to acknowledge the denial/non-send");
                }
            }
        }
    }

    // ==================== HITL-3: BUG-3 -- the approval surface exposes no explanation ===============
    // Demonstrates, against a REAL model's own tool call, that neither the RunEvent stream nor the
    // Interaction record tells an approver WHAT is pending -- confirming this is a real usability
    // gap in the shipped API, not a hypothetical one.
    {
        send_invoked_log() = false;
        Session session;
        configure_session(session, "hitl3", host, model, store, caps, held);

        // Deliberately mundane wording -- an earlier draft of this prompt asked the model to "wire
        // $50,000", which a real model sometimes refuses outright (a text reply declining, no tool
        // call at all) rather than emitting the gated call this case needs to demonstrate BUG-3
        // against. The distinguishing content this case actually needs is the recipient/body TEXT
        // being recoverable via the history workaround below, not that it sounds high-stakes.
        Message const ask = user_message(
            "Use the send_message tool to message \"finance@example.com\" with the body \"Quarterly "
            "budget summary attached, HITL-3 marker.\" Call the tool now, do not ask for confirmation "
            "yourself -- the system will handle that.");
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(session.start_run(StartRun{ask}));
        check(!r1.has_value(), "HITL-3: the run suspends on the real model's gated call");
        check(session.has_open_interactions(), "HITL-3: a real Interaction opened");

        std::optional<agentengine::run_event_payload::ApprovalRequested> captured_payload;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::approval_requested) {
                captured_payload = std::get<agentengine::run_event_payload::ApprovalRequested>(ev->payload);
            }
        }
        check(captured_payload.has_value(), "HITL-3: an approval_requested event was captured");

        // BUG-3, part A: the event itself. ApprovalRequested has exactly two fields (call_id,
        // interaction_id) -- neither names the tool or its arguments. There is no third field to read.
        if (captured_payload) {
            note("HITL-3 approval_requested.call_id", captured_payload->call_id);
            note("HITL-3 approval_requested.interaction_id", captured_payload->interaction_id);
            std::fprintf(stderr,
                         "  ?? CONFIRMED (BUG-3): run_event_payload::ApprovalRequested carries only "
                         "call_id/interaction_id -- a live consumer of this event CANNOT learn this "
                         "call is send_message(\"finance@example.com\", \"Quarterly budget "
                         "summary...\") from the event alone, no matter how sensitive the real "
                         "action is.\n");
        }

        // BUG-3, part B: the Interaction record itself, reached via the public open_interactions()
        // query -- same story: interaction_id/run_id/reason/timestamps, nothing tool-identifying.
        if (session.has_open_interactions()) {
            Interaction const& interaction = session.open_interactions().front();
            note("HITL-3 Interaction.reason",
                 interaction.reason == interaction_reason::approval ? "approval" : "other");
            std::fprintf(stderr,
                         "  ?? CONFIRMED (BUG-3): Interaction (core/interaction.hpp) itself has no "
                         "tool_name/arguments field either -- the PUBLIC suspend surface (events + "
                         "Interaction) gives an approver zero information about what they are being "
                         "asked to approve.\n");
        }

        // The only route that DOES work: reach past the public surface into session.history() and
        // re-derive it by hand, replicating agent_session.hpp's own internal convention
        // (`tool_calls_of(history_.back())`) that resolve_interaction() itself relies on.
        if (!session.history().empty()) {
            std::vector<ToolCall> const pending = tool_calls_of(session.history().back());
            check(!pending.empty(),
                  "HITL-3: the ONLY way to recover what's pending is manually reading session.history() "
                  "and re-deriving it via tool_calls_of() -- undocumented, and NOT reachable from the "
                  "public Interaction/RunEvent surface a real host would build an approval UI against");
            if (!pending.empty()) {
                note("HITL-3 recovered tool_name (via history workaround)", pending.front().tool_name);
                note("HITL-3 recovered arguments_json (via history workaround)",
                     pending.front().arguments_json);
            }
        }

        // Clean up: deny, so this session doesn't linger with an open interaction past this scope.
        if (session.has_open_interactions()) {
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            (void)drive(session.resolve_interaction(ResolveInteraction{interaction_id, false, std::nullopt}));
        }
    }

    // ==================== HITL-4: BUG-1 + BUG-2 -- mixed round, forced parallel calls =================
    // Forces the REAL model to call BOTH the ordinary get_weather tool AND the gated send_message
    // tool together, in ONE response -- mirroring test_rt_agent_session_live_multitool_e2e.cpp's own
    // MT-2 forced-parallel technique. If the model cooperates, this demonstrates BUG-1 (approval_
    // requested fires for get_weather too) and BUG-2 (denying blocks get_weather's invoke() and feeds
    // it a false "denied by operator" result) against real, non-scripted model output.
    {
        send_invoked_log() = false;
        weather_invoked_log() = false;
        Session session;
        configure_session(session, "hitl4", host, model, store, caps, held);

        Message const ask = user_message(
            "You must call BOTH get_weather for \"Seattle\" AND send_message to \"team@example.com\" "
            "with body \"Ship it, HITL-4.\" together, as two separate tool calls issued in this SAME "
            "response -- not one after the other across two turns. Do not answer with text yet; only "
            "issue both tool calls now.");
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r1 = drive(session.start_run(StartRun{ask}));
        check(!r1.has_value(), "HITL-4: the round suspends (at least one of the two calls is gated)");

        std::vector<agentengine::run_event_payload::ApprovalRequested> approval_events;
        while (auto ev = viewer.next()) {
            if (ev->kind == run_event_kind::approval_requested) {
                approval_events.push_back(
                    std::get<agentengine::run_event_payload::ApprovalRequested>(ev->payload));
            }
        }
        note("HITL-4 approval_requested count", std::to_string(approval_events.size()));

        std::vector<ToolCall> pending;
        if (!session.history().empty()) pending = tool_calls_of(session.history().back());
        note("HITL-4 pending tool calls in the suspended round", std::to_string(pending.size()));
        for (auto const& c : pending) note("HITL-4   .. call", c.tool_name + " (" + c.call_id + ")");

        bool const model_cooperated = pending.size() >= 2;
        if (!model_cooperated) {
            std::fprintf(stderr,
                         "  note: the real model did not issue both tool calls in one round this time "
                         "(got %zu) -- HITL-4's BUG-1/BUG-2 demonstration is skipped for this run, same "
                         "model-cooperation caveat test_rt_agent_session_live_multitool_e2e.cpp's own "
                         "MT-2 documents. This is NOT a pass/fail signal on the bugs themselves.\n",
                         pending.size());
        } else {
            // BUG-1: approval_requested should have fired ONLY for the call(s) that actually needed a
            // decider (send_message) -- if it ALSO fired for get_weather's call_id, that is exactly
            // the misleading fan-out this file's top comment names.
            bool const weather_call_got_approval_event = std::any_of(
                pending.begin(), pending.end(), [&](ToolCall const& c) {
                    if (c.tool_name != "get_weather") return false;
                    return std::any_of(approval_events.begin(), approval_events.end(),
                                        [&](auto const& ev) { return ev.call_id == c.call_id; });
                });
            known_bug_check(
                !weather_call_got_approval_event,
                "HITL-4 (BUG-1 regression check -- expected to FAIL until fixed): approval_requested "
                "must NOT fire for get_weather's call_id -- it was never gated and never needed a "
                "human decision. A FAIL here is agent_session.hpp's run_rounds() emitting approval_"
                "requested for every call in the round instead of only the ones resolve_approval_"
                "outcome() actually flagged needs_decider.");

            // Deny the interaction -- and check the collateral damage.
            if (session.has_open_interactions()) {
                std::string const interaction_id = session.open_interactions().front().interaction_id;
                auto r2 = drive(
                    session.resolve_interaction(ResolveInteraction{interaction_id, false, std::nullopt}));
                check(r2.has_value(), "HITL-4: denying still lets the run converge");

                known_bug_check(
                    weather_invoked_log(),
                    "HITL-4 (BUG-2 regression check -- expected to FAIL until fixed): get_weather's "
                    "invoke() must NOT be skipped by a denial aimed at send_message -- get_weather "
                    "was never gated. A FAIL here means resolve_interaction()'s denial branch denies "
                    "EVERY pending call in the round, not just the one(s) that actually needed a "
                    "decider -- collateral denial of an unrelated, ungated call.");

                // Find get_weather's ToolResult in the message resolve_interaction() folded into
                // history, and check whether it was ALSO stamped "denied by operator" -- a fabricated
                // explanation for a call no operator was ever asked about.
                bool found_weather_result = false;
                bool weather_result_falsely_denied = false;
                for (Message const& m : session.history()) {
                    if (m.role != role::tool) continue;
                    for (ContentItem const& item : m.content) {
                        auto const* tr = std::get_if<ToolResult>(&item.value);
                        if (!tr) continue;
                        bool const is_weather_call = std::any_of(
                            pending.begin(), pending.end(), [&](ToolCall const& c) {
                                return c.tool_name == "get_weather" && c.call_id == tr->call_id;
                            });
                        if (!is_weather_call) continue;
                        found_weather_result = true;
                        if (tr->is_error && !tr->content.empty()) {
                            if (auto const* err = std::get_if<Error>(&tr->content.front().value)) {
                                weather_result_falsely_denied = (err->message == "denied by operator");
                            }
                        }
                    }
                }
                check(found_weather_result, "HITL-4: get_weather's ToolResult is present in history");
                if (weather_result_falsely_denied) {
                    std::fprintf(stderr,
                                 "  ?? CONFIRMED (BUG-2): get_weather's own ToolResult is stamped "
                                 "\"denied by operator\" -- the exact same synthetic-denial text as "
                                 "the real gated call -- even though no operator was ever asked about "
                                 "get_weather specifically. The model (and any user-facing summary "
                                 "built from it) is told a false reason for why get_weather's result "
                                 "never came back.\n");
                }
            }
        }
    }

    // ==================== HITL-5: "mid-conversation drop" -- context across suspend/resume ==========
    // Establishes a deliberately unguessable fact in turn A (matching MT-1's own "un-round number, a
    // model could not plausibly fabricate this" precedent, applied to free text instead of a number),
    // then runs a full gated-tool suspend/approve cycle in turn B, then asks in turn C whether the
    // turn-A fact survived. A real model correctly recalling an arbitrary token is only explicable by
    // history_ genuinely retaining turn A's messages across the suspend/resume boundary intact.
    {
        send_invoked_log() = false;
        Session session;
        configure_session(session, "hitl5", host, model, store, caps, held);
        constexpr char const* kMagicPhrase = "zeppelin-quartz-77";

        // ---- Turn A: plant the fact, no tool call expected -----------------------------------------
        auto turnA = drive(session.start_run(StartRun{user_message(
            "Remember this exact code phrase for later, you will be asked to repeat it: '" +
            std::string(kMagicPhrase) +
            "'. For this message only, just acknowledge in one short sentence -- do not call any "
            "tool.")}));
        check(turnA.has_value(), "HITL-5 turn A: converges with no tool call");
        if (turnA.has_value()) check(has_text(turnA->message), "HITL-5 turn A: carries a text reply");
        std::size_t const history_after_turn_a = session.history().size();
        check(history_after_turn_a >= 2,
              "HITL-5 turn A: history holds at least the user message and the assistant reply");

        // ---- Turn B: a real gated tool call, suspended then approved -------------------------------
        auto turnB_start = drive(session.start_run(StartRun{user_message(
            "Now use the send_message tool to message \"team@example.com\" with the body \"Ship it, "
            "HITL-5.\" Call the tool now, do not ask for confirmation yourself -- the system will "
            "handle that.")}));
        check(!turnB_start.has_value(), "HITL-5 turn B: the gated call suspends the round");
        check(session.has_open_interactions(), "HITL-5 turn B: a real Interaction opened");

        result<agentengine::rt::AgentResponse> turnB_resumed{
            std::unexpected(error{failure_class::contract, "not resolved", "test.not_resolved"})};
        if (session.has_open_interactions()) {
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            turnB_resumed =
                drive(session.resolve_interaction(ResolveInteraction{interaction_id, true, std::nullopt}));
        }
        check(turnB_resumed.has_value(), "HITL-5 turn B: approving resumes and converges");
        check(send_invoked_log(), "HITL-5 turn B: send_message really ran after approval");
        check(!session.has_open_interactions(), "HITL-5 turn B: the interaction closed");

        // ---- Turn C: does the model still know turn A's fact? --------------------------------------
        auto turnC = drive(session.start_run(StartRun{user_message(
            "What was the exact code phrase I asked you to remember earlier in this conversation? "
            "Reply with ONLY the phrase, nothing else, no punctuation.")}));
        check(turnC.has_value(), "HITL-5 turn C: converges with no further tool call");
        if (turnC.has_value()) {
            std::string const reply = text_of(turnC->message);
            note("HITL-5 turn C reply", reply);
            check(icontains(reply, kMagicPhrase),
                  "HITL-5 (\"mid-conversation drop\" check): the real model's turn-C reply still "
                  "contains the turn-A magic phrase -- proving history_ was not truncated, reordered, "
                  "or otherwise corrupted across the suspend -> resolve_interaction -> resume boundary "
                  "in between. A model could not plausibly guess this exact token on its own.");
        }

        // History must have grown monotonically across the whole suspend/resume dance -- nothing
        // silently dropped: turn B's own [user message (start_run's own push), assistant tool-call,
        // tool result, assistant final reply] (>= 4 more, not 3 -- turn B's user message is its own
        // real entry, distinct from turn A's), plus turn C's [user, assistant] (2 more).
        check(session.history().size() >= history_after_turn_a + 4 + 2,
              "HITL-5: session.history() grew monotonically across turns A/B/C -- no messages were "
              "silently dropped across the suspend/resume boundary");
    }

    if (g_known_bug_failures != 0) {
        std::fprintf(stderr,
                     "test_rt_agent_session_hitl_live_e2e: %d known pre-existing bug(s) still present "
                     "(BUG-1/BUG-2 -- see the FAIL (known pre-existing bug...) lines above). NOT "
                     "counted toward this binary's pass/fail exit code -- fix agent_session.hpp, then "
                     "these should flip to 'ok (this bug appears FIXED now...)' and this file's own top "
                     "comment should be updated to match.\n",
                     g_known_bug_failures);
    }

    // g_failures alone drives the exit code -- g_known_bug_failures tracks BUG-1/BUG-2 separately
    // (known_bug_check() above) so a genuine regression anywhere in this file is never masked by, nor
    // confused with, "the already-documented product bug is still there".
    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_hitl_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr,
                 "test_rt_agent_session_hitl_live_e2e: %d FAILURE(S) -- see BUG-3 notes above for "
                 "confirmed pre-existing product defects versus genuine regressions in this test "
                 "itself.\n",
                 g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
