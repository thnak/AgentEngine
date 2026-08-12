// Proves ADR-029's suspend-for-human-approval mechanism (agent_session.hpp) — the "prove" half of
// design → red-team → prove → judge for that ADR. Deterministic, offline (no live model, no
// network) — a scripted `ChatClientT` test double drives every scenario, same shape as
// test_agent_session_tool_call_loop.cpp (ADR-027's own proof), which this suite deliberately does
// not duplicate: R1-R6 there already cover the DEFAULT (`suspend_for_approval_ == false`) path,
// including the "no decider configured => denied every round => max_turns_exceeded" case this ADR
// adds an opt-in alternative to, not a replacement for.
//
// Covers, one case per block in `main()`:
//   SU1 — an approval-needing call with `suspend_for_approval_ == true` and no `approval_decider_`
//         suspends the round: the `StartRun` ask never resolves (fail-closed, never a hang), a real
//         `Interaction` opens with `interaction_reason::approval`, and `input_required`/
//         `approval_requested` events fire.
//   SU2 — a fresh `StartRun` while that interaction is still open is rejected (red-team finding #5):
//         the second ask never resolves either, and no second run_id is minted.
//   SU3 — `ResolveInteraction{approved=true}` resumes the SAME run: the pending call is invoked for
//         real (through the ordinary `invoke_tool` pipeline, capability-checked same as any other
//         call), the interaction closes, and the run converges to a final text answer.
//   SU4 — `ResolveInteraction{approved=false}` resumes with a denial folded into history as an
//         ordinary tool error (never invoking the gated tool), and the run continues from there.
//   SU5 — an unknown `interaction_id` fails closed without mutating `open_interactions_`/`history_`
//         (red-team finding #4's ordering fix: validate before resolving).
//   SU6 — a `ResolveInteraction::caller` that doesn't match the session's owning principal is denied
//         at admission (red-team finding #6), the interaction stays open.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// ---- Test tools --------------------------------------------------------------------------------

struct GateArgs { int value = 0; };
AE_JSON_SCHEMA(GateArgs, value)
struct GateReply { int value = 0; };
AE_JSON_SCHEMA(GateReply, value)

[[nodiscard]] bool& gated_tool_invoked_log() {
    static bool invoked = false;
    return invoked;
}

// Empty capability ceiling (so a real approval decision, not a missing-capability one, is what's
// under test) but `always_require` -- the shape that needs a real human answer once
// `suspend_for_approval_` is on and no synchronous `approval_decider_` is configured.
struct GatedTool : ae::Tool<GatedTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>,
                              ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Needs approval before every call.";
    using Args = GateArgs;
    using Reply = GateReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        gated_tool_invoked_log() = true;
        return Reply{a.value};
    }
};

// ---- ContextProvider fixture: history + the one gated tool's declaration -----------------------

class GatedHistoryProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<GatedTool>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<GatedHistoryProvider>);

// ---- Scripted ChatClientT: a queue of pre-built responses ---------------------------------------

class ScriptedChatClient {
public:
    std::vector<ae::Message> scripted_responses;  // consumed in order, one per chat() call
    std::size_t call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::Message reply;
        if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ++call_count;
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::Message reply;
        if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ae::ChatResponseUpdate upd;
        upd.delta    = reply.content.front();
        upd.is_final = true;
        upd.usage    = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed  = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        ++call_count;
        return std::move(pair.consumer);
    }

    [[nodiscard]] static ae::Message make_text_message(std::string text) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-text";
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::Text{std::move(text)};
        m.content.push_back(std::move(item));
        return m;
    }

    [[nodiscard]] static ae::Message make_tool_call_message(
        std::string call_id, std::string tool_name, std::string args_json,
        ae::call_provenance provenance = ae::call_provenance::vendor_structured) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-" + call_id;
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                                   ae::content_origin::assistant, provenance};
        m.content.push_back(std::move(item));
        return m;
    }
};
static_assert(ae::ChatClient<ScriptedChatClient>);

[[nodiscard]] ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] bool has_text(ae::Message const& m, std::string const& expected) {
    for (ae::ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<ae::Text>(&item.value)) {
            if (t->text == expected) return true;
        }
    }
    return false;
}

using Session = ae::AgentSession<ScriptedChatClient, ae::NoSessionState, GatedHistoryProvider>;

}  // namespace

int main() {
    // ---- SU1: suspends instead of denying, mints a real Interaction ----------------------------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
        };
        kit.actor().initialize("s-su1", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);
        // Deliberately no set_approval_decider() -- suspension only kicks in when neither a
        // synchronous decider nor an off suspend_for_approval_ would otherwise handle it.

        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r.has_value(),
                 "SU1: the StartRun ask never resolves while suspended -- fail-closed, never a hang, "
                 "the same shape every other unresolved branch in run_rounds() already uses");
        AE_CHECK(!gated_tool_invoked_log(),
                 "SU1: gated_tool's invoke() was never reached -- suspension happens BEFORE any call "
                 "in the round is invoked");
        AE_CHECK(kit.actor().has_open_interactions(),
                 "SU1: a real Interaction opened for this suspension");
        AE_CHECK(kit.actor().open_interactions().size() == 1 &&
                     kit.actor().open_interactions().front().reason == ae::interaction_reason::approval,
                 "SU1: the open Interaction is tagged interaction_reason::approval");
        AE_CHECK(kit.actor().open_interactions().front().run_id == kit.actor().last_run_id(),
                 "SU1: the Interaction names the exact run_id that suspended");

        std::vector<ae::RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        bool saw_input_required = false;
        bool saw_approval_requested = false;
        for (ae::RunEvent const& ev : events) {
            if (ev.kind == ae::run_event_kind::input_required) saw_input_required = true;
            if (ev.kind == ae::run_event_kind::approval_requested) saw_approval_requested = true;
        }
        AE_CHECK(saw_input_required, "SU1: an input_required event fired");
        AE_CHECK(saw_approval_requested, "SU1: an approval_requested event fired for the pending call");
    }

    // ---- SU2: a fresh StartRun while suspended is rejected (red-team finding #5) ----------------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
        };
        kit.actor().initialize("s-su2", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r1.has_value(), "SU2 setup: the first StartRun suspends, same as SU1");
        std::string const run_id_at_suspend = kit.actor().last_run_id();

        auto r2 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("second, while suspended")});
        AE_CHECK(!r2.has_value(),
                 "SU2: a second StartRun sent while an interaction is open never resolves either -- "
                 "fail-closed rejection, not a second concurrent run");
        AE_CHECK(kit.actor().last_run_id() == run_id_at_suspend,
                 "SU2: no new run_id was minted -- the rejected StartRun never reached run_counter_ "
                 "at all");
        AE_CHECK(kit.actor().open_interactions().size() == 1,
                 "SU2: still exactly one open interaction -- the rejected StartRun didn't touch it");
    }

    // ---- SU3: ResolveInteraction{approved=true} resumes the SAME run, invokes for real -----------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
            // 2nd chat() call (after resume) falls through to the script's exhaustion default: text.
        };
        kit.actor().initialize("s-su3", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r1.has_value(), "SU3 setup: the run suspends");
        std::string const interaction_id = kit.actor().open_interactions().front().interaction_id;
        std::string const run_id_before_resume = kit.actor().last_run_id();

        auto r2 = kit.ask<ae::AgentResponse>(
            ae::ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt});
        AE_CHECK(r2.has_value(),
                 "SU3: resolving with approval=true lets the run converge to a final response");
        if (r2.has_value()) {
            AE_CHECK(has_text(r2->message, "done"), "SU3: the converged response is the scripted "
                                                     "post-resume text");
        }
        AE_CHECK(gated_tool_invoked_log(),
                 "SU3: gated_tool's invoke() WAS called after approval -- the exact pending call, "
                 "invoked for real through the ordinary pipeline");
        AE_CHECK(!kit.actor().has_open_interactions(),
                 "SU3: the interaction closed once resolved");
        AE_CHECK(kit.actor().last_run_id() == run_id_before_resume,
                 "SU3: the SAME run_id continued -- this was a resumption, not a fresh run (I4 "
                 "attributability)");
    }

    // ---- SU4: ResolveInteraction{approved=false} feeds back a denial, never invokes --------------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
            // 2nd chat() call (after the denial is fed back) falls through to plain text.
        };
        kit.actor().initialize("s-su4", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r1.has_value(), "SU4 setup: the run suspends");
        std::string const interaction_id = kit.actor().open_interactions().front().interaction_id;

        auto r2 = kit.ask<ae::AgentResponse>(
            ae::ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt});
        AE_CHECK(r2.has_value(),
                 "SU4: resolving with approval=false still lets the run converge -- the denial is an "
                 "ordinary tool error fed back, not a run-level failure");
        AE_CHECK(!gated_tool_invoked_log(),
                 "SU4: gated_tool's invoke() was NEVER called -- a denial never reaches the real "
                 "tool");
        AE_CHECK(!kit.actor().has_open_interactions(),
                 "SU4: the interaction closed even on denial");
    }

    // ---- SU5: an unknown interaction_id fails closed, mutates nothing -----------------------------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
        };
        kit.actor().initialize("s-su5", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r1.has_value(), "SU5 setup: the run suspends");

        auto r2 = kit.ask<ae::AgentResponse>(
            ae::ResolveInteraction{"no-such-interaction", /*approved=*/true, std::nullopt});
        AE_CHECK(!r2.has_value(),
                 "SU5: resolving an unknown interaction_id never resolves the ask -- fail closed");
        AE_CHECK(!gated_tool_invoked_log(), "SU5: gated_tool's invoke() was never reached");
        AE_CHECK(kit.actor().has_open_interactions() && kit.actor().open_interactions().size() == 1,
                 "SU5: the REAL open interaction is untouched -- an unknown id must not resolve or "
                 "otherwise mutate it");
    }

    // ---- SU6: a caller that doesn't match the owning principal is denied at admission -------------
    {
        gated_tool_invoked_log() = false;

        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "gated_tool", R"({"value":7})"),
        };
        kit.actor().initialize("s-su6", ae::Principal{"owner", "tenant-a"});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_suspend_for_approval(true);

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(!r1.has_value(), "SU6 setup: the run suspends");
        std::string const interaction_id = kit.actor().open_interactions().front().interaction_id;
        std::uint64_t const denied_before = kit.actor().admission_denied_count();

        auto r2 = kit.ask<ae::AgentResponse>(ae::ResolveInteraction{
            interaction_id, /*approved=*/true, ae::SessionCaller{"someone-else", "tenant-a"}});
        AE_CHECK(!r2.has_value(),
                 "SU6: a ResolveInteraction from an unrelated principal never resolves the ask -- "
                 "admission denies it before the interaction lookup ever runs");
        AE_CHECK(!gated_tool_invoked_log(), "SU6: gated_tool's invoke() was never reached");
        AE_CHECK(kit.actor().admission_denied_count() == denied_before + 1,
                 "SU6: the denial is counted, same admission shape as StartRun::caller");
        AE_CHECK(kit.actor().has_open_interactions(),
                 "SU6: the interaction stays open -- a denied resolver must not consume it");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_suspend_approval: OK\n"
                                   : "test_agent_session_suspend_approval: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
