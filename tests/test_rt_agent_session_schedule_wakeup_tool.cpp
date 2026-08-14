// Proof for ADR-053 §5's own named follow-up (closed here): `schedule_wakeup` exposed as a real,
// MODEL-callable declared tool (006 §6b, 019 §2's "Agent-callable, not just host-triggered" framing),
// not just the host-side `AgentSession::schedule_wakeup()` method
// test_rt_agent_session_schedule_wakeup.cpp already proves. Reuses the exact ScriptedChatClient/
// tool_call_message fixture shape test_rt_agent_session_tooling_and_delegation.cpp already
// established for this kind of end-to-end tool-call proof.
//
//   G1 -- a session with NO cap::Schedule grant is never OFFERED the tool at all (never sent to the
//         model) -- avoids advertising a tool the model could never successfully call.
//   G2 -- a session WITH a cap::Schedule grant IS offered it, with the real name/description/schema.
//   G3 -- end-to-end success: a scripted model tool-call for "schedule_wakeup" produces a real
//         StandingEffect, visible via list_standing_effects(), with a real handle_id round-tripped
//         back through the ToolResult -- never CounterTool-style ScheduleWakeupTool::invoke()'s own
//         unreachable sentinel.
//   G4 -- end-to-end failure (horizon exceeded): the SAME real error schedule_wakeup() itself would
//         produce surfaces through the ordinary ToolResult error channel, and nothing is registered.
//   G5 -- defense in depth: even if a model hallucinates "schedule_wakeup" against a session that was
//         never offered it (no Schedule grant, G1), invoke_tool()'s own ordinary step-1 resolve
//         rejects it with "tool.unknown_name" -- the tool genuinely isn't in that turn's table, not
//         merely hidden from a UI.
//   G6 -- max_active is enforced end to end through the tool-call path too, not just the direct
//         AgentSession::schedule_wakeup() API S4 already covers -- a second call in the SAME round
//         against a Schedule<..,1> grant fails closed via the ordinary ToolResult error channel.

#include <cstdio>
#include <string>

#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
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

using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message text_message(std::string text) {
    Message m;
    m.role       = role::assistant;
    m.message_id = "m-text";
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_message(std::string call_id, std::string tool_name, std::string args_json) {
    Message m;
    m.role       = role::assistant;
    m.message_id = "m-" + call_id;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                            content_origin::assistant, call_provenance::vendor_structured};
    m.content.push_back(item);
    return m;
}

// Scripted ChatClientT that ALSO captures the last ChatRequest it was handed -- needed for G1/G2 to
// inspect what tools the model was actually offered, not just what it was told to call.
class ScriptedCapturingChatClient {
public:
    ScriptedCapturingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<Message> script;
        std::size_t          call_count = 0;
        std::optional<ChatRequest> last_request;
    };

    void set_script(std::vector<Message> script) { state_->script = std::move(script); }
    [[nodiscard]] ChatRequest const& last_request() const { return *state_->last_request; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        state_->last_request = request;
        Message reply = (state_->call_count < state_->script.size())
                            ? state_->script[state_->call_count]
                            : text_message("done");
        ++state_->call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedCapturingChatClient>);

[[nodiscard]] agentengine::ToolDescriptor const* find_tool(ChatRequest const& req, std::string_view name) {
    for (agentengine::ToolDescriptor const& d : req.tools) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> handle_id_of(Message const& m) {
    for (ContentItem const& item : m.content) {
        auto const* tr = std::get_if<ToolResult>(&item.value);
        if (!tr || tr->is_error || tr->content.empty()) continue;
        auto const* d = std::get_if<agentengine::Data>(&tr->content[0].value);
        if (!d) continue;
        auto parsed = agentengine::json::parse(d->json);
        if (!parsed) continue;
        auto const* h = parsed->find("handle_id");
        if (h && h->is_string()) return h->as_string();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> error_message_of(Message const& m) {
    for (ContentItem const& item : m.content) {
        auto const* tr = std::get_if<ToolResult>(&item.value);
        if (!tr || !tr->is_error || tr->content.empty()) continue;
        auto const* e = std::get_if<agentengine::Error>(&tr->content[0].value);
        if (e) return e->message;
    }
    return std::nullopt;
}

using Session = AgentSession<ScriptedCapturingChatClient>;

}  // namespace

int main() {
    // --- G1: NOT offered without a cap::Schedule grant ------------------------------------------
    {
        Session session;
        session.initialize("s-g1", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script({});  // converges immediately with the default "done" text reply
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "G1 setup: the run converges");
        check(find_tool(client.last_request(), "schedule_wakeup") == nullptr,
              "G1: a session with no cap::Schedule grant is never offered schedule_wakeup at all");
    }

    // --- G2: offered, with the real name/description/schema, once a grant exists ----------------
    {
        Session session;
        session.initialize("s-g2", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script({});
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::Schedule{std::chrono::seconds{3600}, 5}});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "G2 setup: the run converges");
        agentengine::ToolDescriptor const* td = find_tool(client.last_request(), "schedule_wakeup");
        check(td != nullptr, "G2: a session WITH a cap::Schedule grant IS offered schedule_wakeup");
        if (td != nullptr) {
            check(!td->description.empty(), "G2: the offered descriptor carries a real description");
            check(td->args_schema_json.find("delay_ms") != std::string::npos &&
                      td->args_schema_json.find("label") != std::string::npos,
                  "G2: the offered descriptor's own args schema names both real fields");
        }
    }

    // --- G3: end-to-end success -- a real StandingEffect, a real handle_id round-tripped back -----
    {
        Session session;
        session.initialize("s-g3", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script(
            {tool_call_message("c1", "schedule_wakeup", R"({"delay_ms":5000,"label":"digest"})")});
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::Schedule{std::chrono::seconds{3600}, 5}});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("remind me later")}));
        check(r.has_value(), "G3: the run converges after the tool call resolves");
        check(session.list_standing_effects().size() == 1,
              "G3: a real StandingEffect was registered through the tool-call path");
        if (session.list_standing_effects().size() == 1) {
            auto const& eff = session.list_standing_effects().front();
            check(eff.kind == agentengine::standing_effect_kind::schedule_wakeup,
                  "G3: its kind is schedule_wakeup");
            check(eff.label == "digest", "G3: its label is the model-supplied one, round-tripped correctly");
        }

        // Find the role::tool history message and confirm the ToolResult carries a REAL handle_id,
        // matching the one registered above -- proving the closure's return value actually reached
        // the model-visible tool result, not just AgentSession's own internal bookkeeping.
        std::optional<std::string> observed_handle;
        for (Message const& m : session.history()) {
            if (m.role == role::tool) observed_handle = handle_id_of(m);
        }
        check(observed_handle.has_value() && !observed_handle->empty(),
              "G3: a real, non-empty handle_id was returned through the ordinary ToolResult channel");
        if (observed_handle.has_value() && session.list_standing_effects().size() == 1) {
            check(*observed_handle == session.list_standing_effects().front().handle_id,
                  "G3: the returned handle_id is the SAME one AgentSession itself registered -- not a "
                  "coincidentally-shaped value, and never ScheduleWakeupTool::invoke()'s own "
                  "unreachable sentinel");
        }
    }

    // --- G4: end-to-end failure (horizon exceeded) -- the real error, nothing registered ----------
    {
        Session session;
        session.initialize("s-g4", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script({tool_call_message("c1", "schedule_wakeup",
                                              R"({"delay_ms":7200000,"label":"too far out"})")});
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::Schedule{std::chrono::seconds{3600}, 5}});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("remind me way later")}));
        check(r.has_value(), "G4: the run itself still converges (a tool error, not a run failure)");
        check(session.list_standing_effects().empty(),
              "G4: a delay past the granted horizon registers nothing, through the tool-call path too");

        std::optional<std::string> observed_error;
        for (Message const& m : session.history()) {
            if (m.role == role::tool) observed_error = error_message_of(m);
        }
        check(observed_error.has_value() && observed_error->find("horizon") != std::string::npos,
              "G4: the model sees the REAL schedule_wakeup() error message, through the ordinary "
              "ToolResult error channel -- not swallowed or genericized");
    }

    // --- G5: defense in depth -- a hallucinated call against an ungranted session is refused -------
    {
        Session session;
        session.initialize("s-g5", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script(
            {tool_call_message("c1", "schedule_wakeup", R"({"delay_ms":1000,"label":"x"})")});
        CapabilitySet const held = CapabilitySet::grant_root({});  // no Schedule grant at all
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "G5: the run still converges (a tool error, not a run failure)");
        check(session.list_standing_effects().empty(),
              "G5: nothing is registered -- the hallucinated call never reaches schedule_wakeup() at all");
        std::optional<std::string> observed_error;
        for (Message const& m : session.history()) {
            if (m.role == role::tool) observed_error = error_message_of(m);
        }
        check(observed_error.has_value() && observed_error->find("unknown tool") != std::string::npos,
              "G5: rejected at invoke_tool()'s own ordinary step-1 resolve (tool.unknown_name) -- the "
              "tool genuinely isn't in this turn's table, since G1 never offered it");
    }

    // --- G6: max_active enforced end to end through the tool-call path, not just the direct API ----
    {
        Session session;
        session.initialize("s-g6", Principal{"p", ""});
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script({});  // two calls land in the SAME round, built directly below
        Message both_calls;
        both_calls.role       = role::assistant;
        both_calls.message_id = "m-both";
        ContentItem first;
        first.origin = content_origin::assistant;
        first.value  = ToolCall{"c1", "schedule_wakeup", R"({"delay_ms":1000,"label":"first"})",
                                content_origin::assistant, call_provenance::vendor_structured};
        ContentItem second;
        second.origin = content_origin::assistant;
        second.value  = ToolCall{"c2", "schedule_wakeup", R"({"delay_ms":1000,"label":"second"})",
                                 content_origin::assistant, call_provenance::vendor_structured};
        both_calls.content.push_back(first);
        both_calls.content.push_back(second);
        client.set_script({both_calls});

        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::cap::Schedule{std::chrono::seconds{3600}, 1}});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("remind me twice")}));
        check(r.has_value(), "G6: the run converges");
        check(session.list_standing_effects().size() == 1,
              "G6: only ONE of the two same-round calls succeeds -- max_active(1) enforced live, "
              "through the tool-call path, exactly like the direct API's own G9-analog");
    }

    if (g_failures == 0) {
        std::printf("test_rt_agent_session_schedule_wakeup_tool: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_schedule_wakeup_tool: %d failure(s)\n", g_failures);
    return 1;
}
