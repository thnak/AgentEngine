// Milestone 4 Phase C2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 005
// §6's "Redact — replace content in place with a tombstone carrying reason and actor" had no
// implementation before this task. Proves: the tombstone replaces ALL of a message's original
// content (nothing survives), reason/actor round-trip through it, an unknown message_id fails
// rather than silently doing nothing, and -- the cross-phase claim 005 §6 itself makes ("must
// propagate to... derived summaries") -- a subsequent `HistoryProvider<Summarize<N,...>>` call
// (Phase B4) sees the tombstone, never the original redacted text.

#include <iostream>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/json_value.hpp"
#include "support/run_task_sync.hpp"

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

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value  = ae::Text{"ok"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

// Reports back a concatenation of every message it was asked to summarize -- proves whether the
// ORIGINAL redacted text or the tombstone reached it (005 §6: "propagates to... derived
// summaries").
class RecordingSummarizerClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const& request, ae::EffectContext&) {
        std::string joined;
        for (auto const& m : request.messages) {
            if (m.content.empty()) continue;
            if (auto const* t = std::get_if<ae::Text>(&m.content.front().value)) {
                joined += t->text + ";";
            } else if (auto const* c = std::get_if<ae::Custom>(&m.content.front().value)) {
                joined += "[" + c->type_id + "];";
            }
        }
        ae::ContentItem item{};
        item.value  = ae::Text{joined};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-summary";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }  // unused; empty/invalid stream
};
static_assert(ae::ChatClient<RecordingSummarizerClient>,
              "RecordingSummarizerClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient>;

    // --- Redacting an unknown message_id fails, rather than silently doing nothing ---------------
    {
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-redact-unknown", ae::Principal{"p-june", ""});
        auto bad = kit.actor().redact("no-such-id", "gdpr", "operator-1");
        AE_CHECK(!bad.has_value() && bad.error().code == "session.redact.unknown_message_id",
                 "C2-C1: redacting an unknown message_id returns a real error, not a silent no-op");
    }

    // --- Redact replaces ALL content items with exactly one tombstone, carrying reason/actor ------
    quark::TestKit<Session> kit;
    kit.actor().initialize("s-redact", ae::Principal{"p-june", ""});

    ae::Message secret = make_user_turn("my secret ssn is 123-45-6789", "m-secret");
    ae::ContentItem extra{};
    extra.value  = ae::Text{"a second content item, also secret"};
    extra.origin = ae::content_origin::user;
    secret.content.push_back(extra);

    auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{secret});
    AE_CHECK(r1.has_value(), "setup: turn against the mock succeeds");
    AE_CHECK(kit.actor().history().size() == 2 && kit.actor().history()[0].content.size() == 2,
             "setup: the source message has 2 content items before redaction");

    auto redacted = kit.actor().redact("m-secret", "gdpr-erasure-request", "operator-1");
    AE_CHECK(redacted.has_value(), "C2-R1: redacting a message that exists succeeds");

    ae::Message const& after = kit.actor().history()[0];
    AE_CHECK(after.content.size() == 1,
             "C2-R2: the tombstone replaces ALL content items with exactly one -- nothing of the "
             "original 2 items survives");
    auto const* custom = std::get_if<ae::Custom>(&after.content.front().value);
    AE_CHECK(custom != nullptr && custom->type_id == "ae:redacted",
             "C2-R3: the tombstone is a Custom content item, namespaced \"ae:redacted\" (003 §1's "
             "own designed escape hatch for a non-core kind)");

    if (custom) {
        auto parsed = ae::json::parse(custom->payload_json);
        AE_CHECK(parsed.has_value(), "C2-R4: the tombstone payload is valid JSON");
        if (parsed.has_value()) {
            auto const* reason = parsed->find("reason");
            auto const* actor  = parsed->find("actor");
            AE_CHECK(reason != nullptr && reason->as_string() == "gdpr-erasure-request",
                     "C2-R5: the tombstone carries the exact reason passed to redact()");
            AE_CHECK(actor != nullptr && actor->as_string() == "operator-1",
                     "C2-R6: the tombstone carries the exact actor passed to redact() (I4 "
                     "attribution)");
        }
    }

    // --- 005 §6: redaction propagates to derived summaries (Phase B4's Summarize<N>) --------------
    // The redacted message is now history[0]; force it into the "older" (summarized) slice by
    // keeping a window of just the last message (history[1], the assistant's reply).
    {
        ae::HistoryProvider<ae::Summarize<1, RecordingSummarizerClient>> provider;
        ae::SessionContext session_ctx{kit.actor().session_id(), kit.actor().principal(),
                                        kit.actor().history()};
        ae::EffectContext ctx{};
        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value() && out->messages.size() == 2,
                 "setup: Summarize<1> over 2 history messages produces 1 summary + 1 verbatim");
        if (out.has_value() && !out->messages.empty()) {
            auto const* summary_text = std::get_if<ae::Text>(&out->messages.front().content.front().value);
            AE_CHECK(summary_text != nullptr &&
                         summary_text->text.find("123-45-6789") == std::string::npos,
                     "C2-R7: the ORIGINAL redacted text (the SSN) never reaches the summarizer");
            AE_CHECK(summary_text != nullptr &&
                         summary_text->text.find("[ae:redacted]") != std::string::npos,
                     "C2-R8: the summarizer sees the TOMBSTONE instead -- redaction propagates to "
                     "a derived summary computed after it, exactly 005 §6's own rule");
        }
    }

    std::cout << (g_failures == 0 ? "test_agent_session_redact: OK\n" : "test_agent_session_redact: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
