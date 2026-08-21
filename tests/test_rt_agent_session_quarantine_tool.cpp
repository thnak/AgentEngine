// Proves decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md's own named residual
// closed for real: "no production call site wires scan_and_quarantine()/
// make_quarantine_secret_tool_descriptor() into AgentSession/assemble_context() anywhere." trust/
// secret_quarantine.hpp's new QuarantineToolProvider (a real ContextProvider conformer wrapping
// QuarantineSecretStore) now occupies AgentSession's HistoryProviderT slot directly -- this file runs
// a REAL round where the model calls quarantine_secret as an ordinary tool, gets back a redacted
// reply, and the store genuinely records the quarantined value, all through the real
// AgentSession/ToolTable/invoke_tool pipeline, not a hand-built EffectContext.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/secret_quarantine.hpp"

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

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

// Scripted to call quarantine_secret on its FIRST response, then return an ordinary final answer
// once it sees the tool's own (redacted) result -- a real, minimal two-round tool-call loop, the
// same shape every other live-tool-call test in this tree already uses.
class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<ChatRequest> requests;
    };

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest req, EffectContext&) {
        state_->requests.push_back(req);
        Message m;
        m.role = role::assistant;
        if (state_->requests.size() == 1) {
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value  = ToolCall{"call-1", "quarantine_secret", R"({"text":"sk-hardcoded-secret"})"};
            m.content.push_back(item);
            co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
        }
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{"done, secret handled"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;

    // `session.history_provider()` cannot be reassigned after construction (agent_session.hpp's own
    // established constraint -- HistoryProviderT is a plain, default-constructed value member with
    // no emplace_*/accessor pair, the same limitation history_and_skills_provider.hpp's own top
    // comment documents; QuarantineSecretStore additionally holds a std::mutex, making it neither
    // copy- nor move-assignable even if that constraint didn't exist), so this test exercises the
    // provider's DEFAULT audit hook (nullptr) -- the store still records every quarantine correctly
    // with no hook configured (QuarantineSecretStore::quarantine() only conditionally invokes it).
    AgentSession<ScriptedChatClient, NoSessionState, agentengine::QuarantineToolProvider> session;
    session.initialize("quar-t1", Principal{"p1", ""}, std::nullopt, /*max_turns=*/4);
    session.emplace_chat_client();

    auto outcome = drive(session.start_run(StartRun{user_message("please handle this secret")}));
    check(outcome.has_value(), "T1: a real round calling quarantine_secret as an ordinary tool converges");
    if (outcome.has_value()) {
        check(std::get<Text>(outcome->message.content.front().value).text == "done, secret handled",
              "T1: the final answer is the model's own second response, after the tool round-trip");
    }

    check(session.history_provider().store().grant_eligible_ref_names().empty(),
          "T1: even reached through the REAL tool pipeline, the minted ref is NEVER grant-eligible -- "
          "the I3 fix (ADR-068 §5) holds end to end, not just in the standalone unit test");

    bool secret_leaked_verbatim = false;
    bool found_redaction_marker = false;
    // A ToolResult's own reply is nested ONE level deeper -- tool_pipeline.hpp's invoke_tool() wraps
    // a successful reply as `Data{json}` INSIDE `ToolResult::content`, and the Message this session
    // actually stores carries that whole `ToolResult` as ONE top-level ContentItem (via
    // tool_results_message()), not the Data item directly -- this scan must recurse into it.
    auto scan_items = [&](std::vector<ContentItem> const& items, auto&& self) -> void {
        for (ContentItem const& item : items) {
            if (auto const* t = std::get_if<Text>(&item.value)) {
                if (t->text.find("sk-hardcoded-secret") != std::string::npos) secret_leaked_verbatim = true;
                if (t->text.find("[quarantined secret:") != std::string::npos) found_redaction_marker = true;
            }
            if (auto const* d = std::get_if<agentengine::Data>(&item.value)) {
                if (d->json.find("sk-hardcoded-secret") != std::string::npos) secret_leaked_verbatim = true;
                if (d->json.find("[quarantined secret:") != std::string::npos) found_redaction_marker = true;
            }
            if (auto const* tr = std::get_if<agentengine::ToolResult>(&item.value)) self(tr->content, self);
        }
    };
    for (Message const& m : session.history()) scan_items(m.content, scan_items);
    check(!secret_leaked_verbatim,
          "T1: the raw secret text never appears anywhere in durable session history -- only the "
          "redacted '[quarantined secret: ...]' marker does");
    check(found_redaction_marker,
          "T1: the redaction marker DOES appear -- positive proof quarantine() actually ran through "
          "the real tool pipeline, not merely absence-of-leak by the tool never having been reached");

    std::printf(g_failures == 0 ? "test_rt_agent_session_quarantine_tool: OK\n"
                                 : "test_rt_agent_session_quarantine_tool: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
