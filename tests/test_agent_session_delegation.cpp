// Milestone 5 Phase H4 (018-Identity-Authorization-and-Secrets.md §1: "No token passthrough ...
// delegation [is] expressed as on_behalf_of (007 §2), never by relaying someone else's bearer
// token"). Before this task, `AgentSession::effect_context_.principal` was assigned NOWHERE in the
// codebase (verified: no `effect_context_.principal = ...` or `effect_context.principal = ...`
// assignment existed anywhere under include/agentengine before Phase H) -- every outbound
// `ChatClient::chat()` call (004 §1, and 018 §4's "a native seam backend ... is held to the
// identical discipline [as a plugin]") saw a default-constructed, permanently empty `Principal{}`
// regardless of who actually owned the session. This file proves the real fix, on the one surface
// this milestone actually builds (outbound provider calls, decision 1's own scoping): (1) a
// normally-initialized (non-delegated) session's outbound call carries EXACTLY its own owning
// principal, closing the "always empty" gap directly; (2) a session initialized with a principal
// produced by `derive_on_behalf_of()` (H1, trust/principal.hpp) -- the shape a sub-agent invocation
// acting for a parent principal would use -- carries that SAME derived identity (kind=agent,
// on_behalf_of set, tenant preserved) through to the outbound call, with no separate/parallel path
// that could instead smuggle a forwarded caller token.

#include <iostream>
#include <memory_resource>
#include <optional>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
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

// `AgentSession` default-constructs its `ChatClientT` member with no external wiring hook (the
// same constraint `ScriptedUsageChatClient`'s own comment names, test_agent_session_token_budget.cpp)
// -- so the only way for a test to observe what `EffectContext` a call actually received is a
// static capture slot, reset at the top of each scenario below. Tests in this file run strictly
// sequentially (a single `main()`, no concurrency), so this is safe.
class CapturingChatClient {
public:
    inline static std::optional<ae::Principal> last_seen_principal;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        last_seen_principal = ctx.principal;

        ae::ContentItem item{};
        item.value  = ae::Text{"reply"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);

        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"reply"};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CapturingChatClient>,
              "CapturingChatClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_turn(std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{"hello"};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

} // namespace

int main() {
    using Session = ae::AgentSession<CapturingChatClient>;

    // --- (1) A root (non-delegated) session's outbound call carries exactly its own principal ---
    {
        CapturingChatClient::last_seen_principal.reset();

        quark::TestKit<Session> kit;
        ae::Principal const owner = ae::make_embedded_principal("p-owner", "tenant-a");
        kit.actor().initialize("s-root", owner);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(r.has_value(), "H4-R1: the run itself succeeds");
        AE_CHECK(CapturingChatClient::last_seen_principal.has_value(),
                 "H4-R2: chat() was reached and captured an EffectContext::principal");
        AE_CHECK(*CapturingChatClient::last_seen_principal == owner,
                 "H4-R3: the outbound call's EffectContext::principal is EXACTLY the session's own "
                 "owning principal -- previously this was always a default-constructed, empty "
                 "Principal{} regardless of who owned the session");
    }

    // --- (2) A session initialized with a derived on_behalf_of principal threads it through -----
    {
        CapturingChatClient::last_seen_principal.reset();

        ae::Principal const parent = ae::make_embedded_principal("p-parent", "tenant-a");
        auto derived = ae::derive_on_behalf_of(parent, "sub-agent-1");
        AE_CHECK(derived.has_value(), "H4-R4: deriving the delegated principal succeeds");

        quark::TestKit<Session> kit;
        kit.actor().initialize("s-delegated", *derived);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(r.has_value(), "H4-R5: the delegated session's run succeeds");
        AE_CHECK(CapturingChatClient::last_seen_principal.has_value(),
                 "H4-R6: chat() was reached for the delegated session too");

        ae::Principal const& seen = *CapturingChatClient::last_seen_principal;
        AE_CHECK(seen.id == "sub-agent-1",
                 "H4-R7: the outbound call carries the DERIVED id, not the parent's id -- the "
                 "sub-agent runs as its own principal, never as the host (007 §2)");
        AE_CHECK(seen.on_behalf_of == "p-parent",
                 "H4-R8: the outbound call names exactly who this delegated call is acting for -- "
                 "the real on_behalf_of expression 018 §1 asks for instead of token passthrough");
        AE_CHECK(seen.tenant_id == "tenant-a",
                 "H4-R9: tenant is preserved unchanged across delegation -- never elevated (018 §6)");
        AE_CHECK(seen.kind == ae::principal_kind::agent,
                 "H4-R10: a delegated principal's kind is always `agent`, never re-labeled as the "
                 "more-trusted `human`/`service` the parent might have been");
        AE_CHECK(seen.delegation_depth == 1,
                 "H4-R11: delegation_depth reflects exactly one hop from the parent");
    }

    if (g_failures == 0) {
        std::cout << "OK: all agent_session delegation checks passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
