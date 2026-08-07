// Milestone 4 Phase C1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): 005
// §6's "Fork — copy-on-write new session id from a history prefix" had no implementation before
// this task. This proves `fork_from()` reuses A1's real isolation (the fork gets its own, distinct
// `session_actor_id()`) and A4's real persistence (the fork snapshots/loads through the exact same
// free functions, unmodified) directly, and that the fork is a true COPY, not a shared reference --
// mutating either session after the fork never crosses back to the other.

#include <iostream>
#include <string>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

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

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value  = ae::Text{"run=" + ctx.run_id};
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

struct ScratchState {
    int notes = 0;
};

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
    using Session = ae::AgentSession<CannedChatClient, ScratchState>;

    // --- Set up a source session with real history/state/metadata ------------------------------
    quark::TestKit<Session> source_kit;
    source_kit.actor().initialize("s-source", ae::Principal{"p-iris", "tenant-5"});
    source_kit.actor().state().notes = 7;
    source_kit.actor().metadata()["k"] = "v";
    auto r1 = source_kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("one", "m-1")});
    AE_CHECK(r1.has_value(), "setup: turn 1 against the source session succeeds");
    auto r2 = source_kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("two", "m-2")});
    AE_CHECK(r2.has_value(), "setup: turn 2 against the source session succeeds");
    AE_CHECK(source_kit.actor().history().size() == 4,
             "setup: source history has 4 entries (2 user turns + 2 replies)");

    // --- Fork: a full-history copy under a new session_id --------------------------------------
    quark::TestKit<Session> fork_kit;
    fork_kit.actor().fork_from(source_kit.actor(), "s-fork");

    AE_CHECK(fork_kit.actor().session_id() == "s-fork" &&
                 ae::session_actor_id("s-fork") != ae::session_actor_id("s-source"),
             "C1-R1: the fork gets a real, distinct session_actor_id (A1's isolation, reused "
             "unmodified) -- never the same ActorId as its source");
    AE_CHECK(fork_kit.actor().principal().id == "p-iris" &&
                 fork_kit.actor().principal().tenant_id == "tenant-5",
             "C1-R2: principal is copied from the source");
    AE_CHECK(fork_kit.actor().history().size() == 4,
             "C1-R3: a fork with no prefix length copies the WHOLE history");
    AE_CHECK(fork_kit.actor().state().notes == 7 && fork_kit.actor().metadata().at("k") == "v",
             "C1-R4: state and metadata are copied alongside history");
    AE_CHECK(fork_kit.actor().last_run_id().empty(),
             "C1-R5: a fork inherits no run identity -- it has had no Run of its own yet (001 §1)");

    // --- Divergence: mutating the fork never crosses back to the source, and vice versa --------
    auto r3 = fork_kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("fork-only", "m-3")});
    AE_CHECK(r3.has_value() && fork_kit.actor().last_run_id() == "s-fork:run:1",
             "C1-R6: the fork's own first run_id starts fresh at 1, prefixed by ITS OWN session_id "
             "-- not a continuation of the source's counter");
    AE_CHECK(source_kit.actor().history().size() == 4,
             "C1-R7: running a turn on the fork does not affect the source's history (true copy, "
             "not a shared reference)");

    source_kit.actor().state().notes = 99;
    AE_CHECK(fork_kit.actor().state().notes == 7,
             "C1-R8: mutating the source's state after the fork never crosses back into the fork");

    // --- A partial-prefix fork keeps only the requested number of leading messages -------------
    quark::TestKit<Session> partial_fork_kit;
    partial_fork_kit.actor().fork_from(source_kit.actor(), "s-fork-partial", std::size_t{2});
    AE_CHECK(partial_fork_kit.actor().history().size() == 2,
             "C1-R9: a fork with history_prefix_len=2 keeps exactly the first 2 messages");

    // --- Fork persists through A4's existing snapshot machinery, unmodified --------------------
    quark::InMemoryStore store;
    auto const fork_id = ae::session_actor_id(fork_kit.actor().session_id());
    auto const fence = store.acquire_fence(fork_id);
    auto saved = ae::save_agent_session_snapshot(fork_kit.activation(), store, fork_kit.actor(), fence);
    AE_CHECK(saved.has_value(), "C1-R10: the fork snapshots through the unmodified A4 free function");

    auto loaded = ae::load_agent_session_snapshot(store, "s-fork");
    AE_CHECK(loaded.has_value() && loaded->has_value() && (*loaded)->session_id == "s-fork" &&
                 (*loaded)->principal_id == "p-iris",
             "C1-R11: the fork loads back through the unmodified A4 free function, with its own "
             "identity, not the source's");

    std::cout << (g_failures == 0 ? "test_agent_session_fork: OK\n" : "test_agent_session_fork: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
