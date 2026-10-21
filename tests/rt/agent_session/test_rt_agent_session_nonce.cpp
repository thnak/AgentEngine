// Proves decisions/ADR-244-required-choices-step-1b.md §2.4 (G-C5 b, end to end): the session mints its own
// `SessionNonce`, and every new incarnation of it -- a clear, a fork, a restore -- carries a nonce the bound
// Python runner refuses. Read where it matters: the `EffectContext` the session hands its chat client.
//
//   N1  a run's context carries a minted nonce.
//   N2  two sessions get different identities.
//   N3  (positive control) two runs of one unchanged session carry the SAME nonce, so the "differs" checks
//       below can fail.
//   N4  clear_in_process_state() advances the epoch, keeping the identity.
//   N5  a fork carries its own identity, never its source's.
//   N6  restore_from_record() into the session that saved the record advances the epoch.
//   N7  restore_from_record() into another session keeps that session's identity, never the saved one's.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/block_on.hpp"

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
    return agentengine::rt::block_on(std::move(t));
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::SessionNonce;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

Message message(role r, content_origin origin, std::string text) {
    Message m;
    m.role = r;
    ContentItem item;
    item.origin = origin;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// The nonce of the context each model call is made under, in call order.
std::vector<SessionNonce> g_seen;

class NonceRecordingClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        g_seen.push_back(ctx.session_nonce);
        co_return ChatResponse{message(role::assistant, content_origin::assistant, "ok"), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
};
static_assert(agentengine::ChatClient<NonceRecordingClient>);

using Session = AgentSession<NonceRecordingClient>;

// The nonce of one fresh run on `s`.
SessionNonce run_nonce(Session& s) {
    auto r = drive(s.start_run(StartRun{message(role::user, content_origin::user, "hi")}));
    if (!r.has_value()) std::fprintf(stderr, "  run failed: %s\n", r.error().code.c_str());
    return r.has_value() && !g_seen.empty() ? g_seen.back() : SessionNonce{};
}

}  // namespace

int main() {
    Session a;
    a.emplace_chat_client();
    a.initialize("nonce-a", agentengine::Principal{"p-a", ""});
    SessionNonce const a1 = run_nonce(a);
    check(a1.minted(), "N1: a run's context carries a minted session nonce");

    Session b;
    b.emplace_chat_client();
    b.initialize("nonce-b", agentengine::Principal{"p-b", ""});
    SessionNonce const b1 = run_nonce(b);
    check(b1.minted() && b1.identity != a1.identity, "N2: two sessions get different identities");

    SessionNonce const a2 = run_nonce(a);
    check(a2 == a1, "N3 (positive control): two runs of one unchanged session carry the same nonce");

    a.clear_in_process_state();
    a.initialize("nonce-a", agentengine::Principal{"p-a", ""});
    SessionNonce const a3 = run_nonce(a);
    check(a3.identity == a1.identity && a3.epoch != a1.epoch,
          "N4: a cleared session keeps its identity and moves to a new epoch");

    Session fork;
    fork.emplace_chat_client();
    fork.fork_from(a, "nonce-fork");
    SessionNonce const f1 = run_nonce(fork);
    check(f1.minted() && f1.identity != a1.identity, "N5: a fork carries its own identity, never its source's");

    auto const record = a.to_record();
    SessionNonce const before_restore = run_nonce(a);
    a.restore_from_record(record);
    SessionNonce const a4 = run_nonce(a);
    check(a4.identity == before_restore.identity && a4.epoch != before_restore.epoch,
          "N6: restoring a record into the session that saved it moves it to a new epoch");

    Session c;
    c.emplace_chat_client();
    c.initialize("nonce-c", agentengine::Principal{"p-c", ""});
    SessionNonce const c1 = run_nonce(c);
    c.restore_from_record(record);
    SessionNonce const c2 = run_nonce(c);
    check(c2.identity == c1.identity && c2.identity != a1.identity && c2.epoch != c1.epoch,
          "N7: a record restored into another session takes that session's identity and a new epoch");

    if (g_failures == 0) {
        std::fprintf(stderr, "ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
