// Prove phase for ADR-058 -- "Wiring OutputSchema<T> into a live run, and what 'validation'
// actually means here". Design B (a narrow, additive opt-in setter on AgentSession) survived the
// red-team pass with two named fixes (result<void>, not bool; "new call site," not an existing
// caller changing -- ADR-058 §4 B1/B3). This file IS that new call site (per ADR-058 §8's own
// "A new, real call site is required" section): a small demo struct with AE_JSON_SCHEMA(...) stands
// in for the real T in OutputSchema<T>, driven through a deterministic, offline, scripted-ChatClient
// -- the same style tests/test_rt_agent_session_suspend_approval.cpp already established, copied
// rather than shared (that file's own "no cross-test-file coupling" precedent).
//
//   O1 -- a scripted response whose text is valid JSON matching the declared schema: the round
//         converges, AgentResponse::structured_output_json is populated with the exact text, no
//         error.
//   O2 -- a scripted response whose text does NOT match the schema (fails schema::from_json<T>):
//         the run fails closed with run.output_schema_validation_failed,
//         structured_output_json stays unset (there is no AgentResponse at all -- the run itself
//         failed).
//   O3 (positive control) -- a session with no set_output_schema() call at all behaves byte-for-byte
//         as before this change: the validator never runs (proven by scripting a response whose
//         text would FAIL validation if the validator ran, and observing the run still converges),
//         structured_output_json always unset.
//   O4 -- with output_schema_strategy::native set, the real ChatRequest reaching the scripted
//         ChatClient actually carries output_schema_json -- proves the request-side wiring.
//   O5 -- with output_schema_strategy::tool_shaped set, the real ChatRequest does NOT carry
//         output_schema_json (proves the deliberate native-only request-side scoping), while the
//         response is still validated the same way as O1.

#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentResponse;
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

// Same "safe because nothing here genuinely suspends on an external wake" reasoning as every other
// rt::AgentSession test file's own drive<T>().
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
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ContentItem;
using agentengine::output_schema_strategy;

// -- The demo T standing in for the real T in OutputSchema<T> (ADR-058 §8's own call: "a two-field
//    struct with AE_JSON_SCHEMA is enough") ----------------------------------------------------------

struct DemoOutput {
    std::string name;
    int         value = 0;
};
AE_JSON_SCHEMA(DemoOutput, name, value)

// The validator closure a real caller would write at the ONE place that knows both T and constructs
// the session -- AgentSession itself never sees DemoOutput, matching ADR-058 §4 B2's own finding.
[[nodiscard]] std::function<agentengine::result<void>(std::string_view)> make_demo_validator() {
    return [](std::string_view text) -> agentengine::result<void> {
        auto parsed = agentengine::json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto decoded = agentengine::schema::from_json<DemoOutput>(*parsed);
        if (!decoded) return std::unexpected(decoded.error());
        return {};
    };
}

// -- HistoryProviderT fixture: plain history passthrough, no tools -- this pass's own claims are
//    about the model-call/response seam, not the tool-call loop. -------------------------------------

class SimpleHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(
        SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<SimpleHistoryProvider>);

// -- The scripted backend -- extends test_rt_agent_session_suspend_approval.cpp's own shape with a
//    recorded-request log, needed for O4/O5 (proving what actually reached the ChatClientT, not just
//    what the response-side validator did with what came back). ------------------------------------

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
        std::vector<ChatRequest> received_requests;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] std::vector<ChatRequest> const& received_requests() const {
        return state_->received_requests;
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        state_->received_requests.push_back(request);
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout this file
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

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

using Session = AgentSession<ScriptedChatClient, NoSessionState, SimpleHistoryProvider>;

}  // namespace

int main() {
    std::string const demo_schema_json = agentengine::schema::json_schema_of<DemoOutput>();

    // ---- O1: valid JSON text -> converges, structured_output_json populated with the exact text ---
    {
        std::string const valid_json = R"({"name":"widget","value":42})";

        Session session;
        session.initialize("o1", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{text_response(valid_json), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_output_schema(demo_schema_json, output_schema_strategy::native,
                                   make_demo_validator());

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "O1: a schema-valid response converges without error");
        if (outcome.has_value()) {
            check(outcome->structured_output_json.has_value(),
                  "O1: structured_output_json is populated");
            check(outcome->structured_output_json.has_value() &&
                      *outcome->structured_output_json == valid_json,
                  "O1: structured_output_json holds the EXACT scripted text, not a re-serialization");
        }
    }

    // ---- O2: schema-invalid JSON text -> fails closed, structured_output_json never populated -----
    {
        // Missing the required "value" field (DemoOutput::value is a plain int, not
        // std::optional<int> -- schema::from_json_field's own "required" rule) -- fails
        // schema::from_json<DemoOutput> at the type-driven-parse layer.
        std::string const invalid_json = R"({"name":"widget"})";

        Session session;
        session.initialize("o2", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{text_response(invalid_json), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_output_schema(demo_schema_json, output_schema_strategy::native,
                                   make_demo_validator());

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(!outcome.has_value(), "O2: a schema-invalid response fails the run closed");
        check(!outcome.has_value() && outcome.error().code == "run.output_schema_validation_failed",
              "O2: the failure is specifically run.output_schema_validation_failed");
    }

    // ---- O3 (positive control): no set_output_schema() -- byte-for-byte as before this change -----
    {
        // Deliberately NOT valid JSON at all -- if the validator ran, this would fail. The point of
        // O3 is proving it never runs when unconfigured, not merely that a JSON-shaped text passes.
        std::string const not_json_at_all = "plain prose, not JSON, not schema-shaped";

        Session session;
        session.initialize("o3", Principal{"p", ""});
        session.emplace_chat_client().set_script(
            {{text_response(not_json_at_all), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        // No session.set_output_schema(...) call -- the regression/positive-control case.

        check(!session.has_output_schema(), "O3 setup: has_output_schema() is false when unconfigured");

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "O3: with no set_output_schema(), a non-JSON response STILL converges -- the validator "
              "never ran, proving this path is additive, not a regression on the ordinary case");
        if (outcome.has_value()) {
            check(!outcome->structured_output_json.has_value(),
                  "O3: structured_output_json stays unset when no schema was ever declared");
            auto const* t = std::get_if<Text>(&outcome->message.content.front().value);
            check(t != nullptr && t->text == not_json_at_all,
                  "O3: the converged response is the scripted text, unmodified");
        }
    }

    // ---- O4: native strategy -> the real ChatRequest carries output_schema_json --------------------
    {
        std::string const valid_json = R"({"name":"widget","value":7})";

        Session session;
        session.initialize("o4", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({{text_response(valid_json), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_output_schema(demo_schema_json, output_schema_strategy::native,
                                   make_demo_validator());

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "O4 setup: the run converges");
        check(client.received_requests().size() == 1,
              "O4: exactly one ChatRequest reached the scripted ChatClientT");
        if (!client.received_requests().empty()) {
            ChatRequest const& sent = client.received_requests().front();
            check(sent.output_schema_json.has_value(),
                  "O4: under output_schema_strategy::native, the real ChatRequest carries "
                  "output_schema_json");
            check(sent.output_schema_json.has_value() && *sent.output_schema_json == demo_schema_json,
                  "O4: the carried text is the exact compiled schema, not a placeholder");
        }
    }

    // ---- O5: tool_shaped strategy -> the real ChatRequest does NOT carry output_schema_json,
    //          while the response is still validated the same way as O1 ------------------------------
    {
        std::string const valid_json = R"({"name":"widget","value":9})";

        Session session;
        session.initialize("o5", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({{text_response(valid_json), Usage{1, 1, 0, 0, 0.0}}});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_output_schema(demo_schema_json, output_schema_strategy::tool_shaped,
                                   make_demo_validator());

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(),
              "O5: the run still converges under tool_shaped (real forced-tool-call behavior is a "
              "named residual, ADR-058 §3 -- only the request-side scoping and response-side "
              "validation are under test here)");
        check(client.received_requests().size() == 1,
              "O5: exactly one ChatRequest reached the scripted ChatClientT");
        if (!client.received_requests().empty()) {
            ChatRequest const& sent = client.received_requests().front();
            check(!sent.output_schema_json.has_value(),
                  "O5: under output_schema_strategy::tool_shaped, the real ChatRequest does NOT "
                  "carry output_schema_json -- the deliberate native-only request-side scoping");
        }
        if (outcome.has_value()) {
            check(outcome->structured_output_json.has_value() &&
                      *outcome->structured_output_json == valid_json,
                  "O5: the RESPONSE is still validated (and structured_output_json still populated) "
                  "the same way as O1 -- only the request-side carriage differs by strategy");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_agent_session_output_schema: ALL PASS\n");
    return 0;
}
