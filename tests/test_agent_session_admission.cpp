// Milestone 5 Phase H2 (018-Identity-Authorization-and-Secrets.md §2: "Admission -- may this
// principal start this run on this session at all? ... checked before ChatClient::chat() is ever
// reached -- session ownership, not just effect-level capability checks (007, already real)").
// Before this task, `AgentSession::handle(Ask<StartRun,...>)` ran unconditionally for whatever
// caller sent the ask -- there was no notion of "the wrong principal is trying to use this
// session" anywhere in the tree. `StartRun::caller` (agent_session.hpp) is the new, OPT-IN
// admission field: `std::nullopt` (the default) skips the check entirely, matching every one of
// the ~44 pre-existing `StartRun{message}` call sites across `tests/` that predate H1's principal-
// establishment mechanism (Phase F4's `token_budget`/`ChatClientRegistry const*` precedent for
// "additive, old call sites unaffected"). `caller` is `SessionCaller` (id + tenant_id only), NOT
// the full `Principal` -- Quark's fixed-capacity message pool (`kMaxPayload = 192` bytes, a
// Quark-side constant this project's "never fork Quark" rule doesn't get to grow) measured
// `Ask<StartRun, AgentResponse>` at 208 bytes with a full `Principal` embedded, already over
// budget; `StartRun`'s own comment has the full sizing story. This file proves the narrower
// mechanism is real, not vacuous: (1) a matching caller is admitted and the run proceeds normally;
// (2) an id mismatch (same tenant) is denied, fail-closed, before ChatClient::chat() is ever
// reached; (3) a same-id-different-tenant caller is ALSO denied -- an id match alone is not
// ownership, foreshadowing Phase I's cross-tenant denial suite; (4) a principal's OWN identity,
// even one legitimately derived `on_behalf_of` the session's owner (H1's `derive_on_behalf_of`), is
// NOT admitted to start a run on the principal it was derived FROM -- `SessionCaller` cannot
// express delegation, so this wire-level check is a strictly conservative exact-match rule:
// delegation never implicitly grants a sub-agent authority to start runs on its parent's own
// session; (5) omitting `caller` entirely preserves the pre-H2 unconditional-admission behavior.

#include <iostream>
#include <memory_resource>
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

// Same shape as EchoChatClient (test_agent_session_isolation.cpp) -- a fixed reply is sufficient
// here since this file is about whether `chat()` is reached AT ALL, not about what it returns.
class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
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
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

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

ae::SessionCaller as_caller(ae::Principal const& p) { return ae::SessionCaller{p.id, p.tenant_id}; }

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient>;

    // --- (1) A matching caller is admitted; the run proceeds normally --------------------------
    {
        quark::TestKit<Session> kit;
        ae::Principal const owner = ae::make_embedded_principal("p-owner", "tenant-a");
        kit.actor().initialize("s-match", owner);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1"), as_caller(owner)});
        AE_CHECK(r.has_value(), "H2-R1: a caller matching the session's owning principal is admitted");
        AE_CHECK(kit.actor().history().size() == 2,
                 "H2-R2: an admitted run appends both the input and the reply to history");
        AE_CHECK(kit.actor().admission_denied_count() == 0,
                 "H2-R3: an admitted run never increments admission_denied_count()");
    }

    // --- (2) An id mismatch (same tenant) is denied, fail-closed --------------------------------
    {
        quark::TestKit<Session> kit;
        ae::Principal const owner    = ae::make_embedded_principal("p-owner", "tenant-a");
        ae::Principal const stranger = ae::make_embedded_principal("p-stranger", "tenant-a");
        kit.actor().initialize("s-mismatch", owner);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1"), as_caller(stranger)});
        AE_CHECK(!r.has_value(),
                 "H2-R4: a caller whose id doesn't match the session's owner never resolves with a "
                 "response -- fail closed, the same shape TestKit::ask documents for a handler that "
                 "never replied (never a hang)");
        AE_CHECK(kit.actor().history().empty(),
                 "H2-R5: a denied run never appends anything to history -- denial happens before "
                 "run_counter_ even increments");
        AE_CHECK(kit.actor().admission_denied_count() == 1,
                 "H2-R6: admission_denied_count() observes the denial even though the ask itself "
                 "never resolved");
    }

    // --- (3) Same id, different tenant is ALSO denied -- id match alone is not ownership --------
    {
        quark::TestKit<Session> kit;
        ae::Principal const owner        = ae::make_embedded_principal("p-shared-id", "tenant-a");
        ae::Principal const other_tenant = ae::make_embedded_principal("p-shared-id", "tenant-b");
        kit.actor().initialize("s-cross-tenant", owner);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1"), as_caller(other_tenant)});
        AE_CHECK(!r.has_value(),
                 "H2-R7: a same-id caller from a DIFFERENT tenant is denied -- an id collision "
                 "across tenants is not ownership (018 §6), foreshadowing Phase I's cross-tenant "
                 "denial suite");
        AE_CHECK(kit.actor().admission_denied_count() == 1, "H2-R8: the cross-tenant attempt is counted");
    }

    // --- (4) A principal derived on_behalf_of the owner is NOT admitted to run on the owner's ---
    // --- session using its OWN identity -- SessionCaller can't express delegation, so this ------
    // --- wire-level rule is deliberately conservative: exact match only. ------------------------
    {
        quark::TestKit<Session> kit;
        ae::Principal const owner = ae::make_embedded_principal("p-owner", "tenant-a");
        kit.actor().initialize("s-delegated", owner);

        auto derived = ae::derive_on_behalf_of(owner, "sub-agent-1");
        AE_CHECK(derived.has_value(), "H2-R9: deriving a first-hop delegated principal succeeds");

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1"), as_caller(*derived)});
        AE_CHECK(!r.has_value(),
                 "H2-R10: a principal derived on_behalf_of the owner is DENIED when it presents its "
                 "own (derived) id at the StartRun boundary -- delegation never implicitly grants a "
                 "sub-agent authority to start runs on its parent's own session; the general, "
                 "delegation-aware `principal_admitted_for` predicate is exercised directly (not "
                 "through this narrower wire type) in test_principal_delegation.cpp");
        AE_CHECK(kit.actor().admission_denied_count() == 1, "H2-R11: the attempt is counted as a denial");
    }

    // --- (5) Omitting `caller` entirely preserves the pre-H2 unconditional-admission behavior ---
    {
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-legacy", ae::Principal{"p-legacy", ""});

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});  // no `caller` -- matches every pre-H2 call site
        AE_CHECK(r.has_value(),
                 "H2-R12: a StartRun with no caller asserted skips the admission check entirely -- "
                 "unchanged behavior for every pre-H2 call site across tests/");
        AE_CHECK(kit.actor().admission_denied_count() == 0, "H2-R13: no check ran, so nothing was denied");
    }

    if (g_failures == 0) {
        std::cout << "OK: all agent_session admission checks passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
