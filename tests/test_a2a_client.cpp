// Milestone 7 Phase D4 (012-A2A-Conformance.md §3/§4a, docs/planning/milestone-7-protocol-
// conformance-breakdown.md). Proves `A2aClient` (protocol/a2a/client.hpp) against a REAL `A2aServer`
// (server.hpp, D3) wired over a real `agentengine::rt::AgentSession` for the happy-path
// send/get/cancel passthrough, and a hand-rolled `CardFetcher` for the digest-pinned Agent Card
// caching/rug-pull properties that need fine control over what a "peer" returns across multiple
// fetches -- the identical split `test_mcp_client.cpp` already uses (a real server for the happy
// path, a mock for multi-call cache control).
//
// ADR-037: ported off `quark::Engine`/`Actor`/`ActorRef` onto `rt::AgentSession` directly, matching
// test_a2a_server.cpp's own port -- `A2aClient` (client.hpp) itself needed NO source changes: it was
// already fully transport-agnostic before this port, with no `AgentSession` dependency of any kind.

#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/protocol/a2a/client.hpp"
#include "agentengine/protocol/a2a/server.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace ae  = agentengine;
namespace art = agentengine::rt;
namespace a2a = agentengine::a2a;

template <class T>
T drive(art::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value  = ae::Text{"reply to run " + ctx.run_id};
        item.origin = ae::content_origin::assistant;
        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;  // generous enough that a small scripted response never blocks on credit
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd{};
        upd.delta.value  = ae::Text{"reply to run " + ctx.run_id};
        upd.delta.origin = ae::content_origin::assistant;
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CannedChatClient>);

using Session = art::AgentSession<CannedChatClient>;

struct Harness {
    Session session;

    Harness() {
        session.initialize("s-a2a-client", ae::Principal{"p-owner", ""});
        session.emplace_chat_client();
    }

    [[nodiscard]] a2a::RunStarter starter() {
        return [this](art::StartRun req) -> ae::result<a2a::RunOutcome> {
            ae::result<art::AgentResponse> resp = drive(session.start_run(std::move(req)));
            if (!resp) {
                return std::unexpected(ae::error{ae::failure_class::transient,
                                                  "the run did not complete", "a2a.run_did_not_complete"});
            }
            return a2a::RunOutcome{session.last_run_id(), *resp};
        };
    }
};

a2a::Message text_message(std::string text) {
    a2a::Message m;
    m.message_id = "wire-msg-1";
    m.role        = a2a::a2a_role::user;
    a2a::Part p;
    p.value = a2a::TextPart{std::move(text)};
    m.parts.push_back(std::move(p));
    return m;
}

a2a::AgentCard make_card(std::string skill_name) {
    a2a::AgentCard card;
    card.name        = "remote-agent";
    card.description = "A remote peer.";
    card.version      = "1.0.0";
    a2a::AgentSkill skill;
    skill.id          = skill_name;
    skill.name        = skill_name;
    skill.description = "A skill.";
    card.skills.push_back(skill);
    return card;
}

}  // namespace

int main() {
    Harness h;
    a2a::A2aServer server(h.starter(), "ctx-remote");

    a2a::RemoteAgentTransport transport;
    transport.send_message = [&server](a2a::Message const& m) { return server.send_message(m); };
    transport.get_task     = [&server](std::string const& id) { return server.get_task(id); };
    transport.cancel_task  = [&server](std::string const& id) { return server.cancel_task(id); };

    // --- D4-1/2/3: happy-path client passthrough against a REAL server ------------------------------
    {
        a2a::A2aClient client(transport, []() -> ae::result<a2a::AgentCard> { return make_card("s1"); },
                               "test-client");

        auto sent = client.send_message(text_message("hello"));
        check(sent.has_value(), "D4-1: send_message() passes through to the real remote server");
        std::string task_id;
        if (sent.has_value()) {
            task_id = sent->id;
            check(sent->status.state == a2a::task_state::completed,
                  "D4-1: the real remote run completes successfully");
        }

        auto fetched = client.get_task(task_id);
        check(fetched.has_value() && fetched->id == task_id,
              "D4-2: get_task() passes through and retrieves the same real task");

        auto cancelled = client.cancel_task(task_id);
        check(!cancelled.has_value(),
              "D4-3: cancel_task() on an already-terminal task passes through the server's own "
              "faithful rejection, not silently swallowed into a false success");
    }

    // --- D4-4: the first fetch_agent_card() caches, and is exactly what the fetcher returned --------
    {
        int fetch_count = 0;
        a2a::A2aClient client(transport,
                               [&fetch_count]() -> ae::result<a2a::AgentCard> {
                                   ++fetch_count;
                                   return make_card("search");
                               },
                               "card-client");
        auto first = client.fetch_agent_card();
        check(first.has_value() && first->skills.size() == 1 && first->skills.front().id == "search",
              "D4-4: the first fetch returns the real card content");
        check(!client.rug_pull_detected(), "D4-4: no rug pull on the very first fetch");
        check(client.cached_card().has_value() && client.cached_card()->name == "remote-agent",
              "D4-4: the card is cached after fetching");
    }

    // --- D4-5: an UNCHANGED card across two fetches never flags a rug pull --------------------------
    {
        a2a::A2aClient client(transport, []() -> ae::result<a2a::AgentCard> { return make_card("fixed"); },
                               "stable-client");
        (void)client.fetch_agent_card();
        (void)client.fetch_agent_card();
        check(!client.rug_pull_detected(),
              "D4-5: fetching the identical card twice never flags a rug pull");
    }

    // --- D4-6: a card that CHANGES between fetches (a different skill) IS detected -- §4a's own -----
    // --- "re-approved rather than silently trusted" rule.                                          ---
    {
        int calls = 0;
        a2a::A2aClient client(
            transport,
            [&calls]() -> ae::result<a2a::AgentCard> {
                ++calls;
                return calls == 1 ? make_card("original-skill") : make_card("DIFFERENT-skill");
            },
            "rugpull-client");
        (void)client.fetch_agent_card();
        check(!client.rug_pull_detected(), "D4-6: no rug pull detected on the first fetch");
        (void)client.fetch_agent_card();
        check(client.rug_pull_detected(),
              "D4-6: a card whose skills changed between fetches under the same client is detected "
              "as a rug pull, per §4a's digest-pinning rule");
    }

    // --- D4-7: a CardFetcher failure is propagated, never silently swallowed into an empty card -----
    {
        a2a::A2aClient client(
            transport,
            []() -> ae::result<a2a::AgentCard> {
                return std::unexpected(
                    ae::error{ae::failure_class::transient, "card unreachable", "test.card_unreachable"});
            },
            "failing-card-client");
        auto fetched = client.fetch_agent_card();
        check(!fetched.has_value(), "D4-7: a CardFetcher failure surfaces as a real error");
        check(!client.cached_card().has_value(),
              "D4-7: a failed fetch never populates the cache with anything");
    }

    if (g_failures == 0) {
        std::printf("test_a2a_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_client: %d failure(s)\n", g_failures);
    return 1;
}
