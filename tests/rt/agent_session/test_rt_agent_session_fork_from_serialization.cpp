// Proves ADR-102 Phase 5's own real fix (rt/agent_session.hpp's own `fork_from()`) -- an independent
// red-team pass, repeated across ADR-102 Phase 3 §22 (`SandboxRuntime::merge_into()`/`discard()`) and
// Phase 4 §29, found that `AgentSession::fork_from()` ran with NO serialization at all against a
// concurrent, in-flight `start_run()`/`resolve_interaction()` on its own `source` -- unlike every OTHER
// public entry point on this class, which all acquire `session_mutex_` (I1) for their whole duration.
// `fork_from()` now acquires `source.session_mutex_` (driven synchronously via `agentengine::rt::
// block_on()`, since `fork_from()` itself stays a plain, non-coroutine function) before copying any of
// `source`'s fields.
//
// This file proves the fix with a REAL two-thread race, not merely by re-running the existing suite:
// a genuinely slow `ChatClient::chat()` call holds `session_mutex_` for a measurable duration inside a
// real `start_run()` round on one thread, while a concurrent `fork_from()` call races it on another.
//
//   [1] fork_from() called concurrently with an in-flight start_run() on the same source does not
//       observe a torn/incomplete history -- the forked target's own copy contains BOTH the round's
//       real input message AND its real final response message, never just the input alone (which a
//       broken, unserialized fork_from() could observe if it read source.history_ mid-round, before
//       the response was pushed).
//   [2] the self-fork case (source IS the calling session) still works correctly and does not
//       self-deadlock -- fork_from() never held session_mutex_ before acquiring it fresh here, so the
//       uncontended fast path applies even when source and *this are the same object.
//   [3] ADR-123: the REENTRANT case -- fork_from() called on the CALLING thread from INSIDE an
//       already-in-flight start_run() round on the very session being forked FROM (e.g. synchronously
//       from a tool closure/ChatClient::chat() body) does NOT self-deadlock. Before ADR-123 this hung
//       forever (block_on()'s own busy-wait spinning against a lock only the caller's own already-
//       parked outer Guard could ever release) -- proven here with a bounded wait, not an infinite
//       join, so a real regression fails this test instead of hanging the whole suite.

#include "agentengine/rt/agent_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace agentengine;
using agentengine::rt::AgentResponse;
using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
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

Message text_message(std::string text) {
    Message m;
    m.role = role::assistant;
    m.message_id = "m-text";
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// A ChatClient whose one real call SLEEPS for a measurable duration before returning a converging
// (non-tool-call) response -- simulating a real, slow model call holding `session_mutex_` for the
// whole time `start_run()`'s own round is in flight, matching this file's own real contention shape.
class SlowChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    agentengine::task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        co_return ChatResponse{text_message("round complete"), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<SlowChatClient>);

using SlowSession = AgentSession<SlowChatClient>;

// [3]'s own fixture: a ChatClient that, from INSIDE its own chat() call (i.e. while the owning
// session's session_mutex_ is held on THIS exact thread by the in-flight start_run() round), calls
// fork_from() reentrantly -- the exact shape ADR-123's own file-banner comment (agent_session.hpp)
// names as the near-future agent.spawn-style hazard. Forks FROM the live, in-flight session INTO a
// separate, freshly-constructed target (not self-into-self) -- a self-into-self fork's own semantics
// mid-round are deliberately out of scope (fork_from()'s own comment: "forking into an already-live,
// concurrently-running session is not a documented or supported operation"); this fixture only tests
// the property that actually matters here, whether the call deadlocks, not what a self-into-self copy
// would mean.
std::atomic<bool> g_reentrant_fork_ran{false};

// `self_` is deliberately `void*`, not `AgentSession<ReentrantChatClient>*` -- naming that
// concept-constrained template-id ANYWHERE in this class's own declarative region (even just to form a
// pointer TYPE, not instantiate the class) requires evaluating `ChatClient<ReentrantChatClient>`,
// which needs `ReentrantChatClient` to already be a COMPLETE type; it isn't yet, mid-definition. Only
// `chat()`'s own BODY (an inline member function, compiled as if placed immediately after this class's
// closing brace, by which point `ReentrantChatClient` is complete) can safely name
// `AgentSession<ReentrantChatClient>` directly -- confirmed the hard way, by a real, initially-failed
// compile attempt using a pre-declared `ReentrantSession` alias in the member signature/field instead.
class ReentrantChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    // Set once, right after emplace_chat_client() returns a reference to this object -- the owning
    // session must exist first, so this can't be done from a constructor.
    void set_self(void* self) { self_ = self; }

    agentengine::task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        auto* self = static_cast<AgentSession<ReentrantChatClient>*>(self_);
        AgentSession<ReentrantChatClient> target;
        target.initialize("reentrant-fork-target", Principal{"reentrant-target-owner", ""});
        target.fork_from(*self, "reentrant-fork-target-renamed");
        g_reentrant_fork_ran.store(true, std::memory_order_release);
        co_return ChatResponse{text_message("reentrant round complete"), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    void* self_ = nullptr;
};
static_assert(agentengine::ChatClient<ReentrantChatClient>);

using ReentrantSession = AgentSession<ReentrantChatClient>;

}  // namespace

int main() {
    // [1] a real two-thread race: fork_from() concurrent with an in-flight, genuinely slow start_run().
    {
        SlowSession source;
        source.initialize("source-session", Principal{"source-owner", ""});
        source.emplace_chat_client();
        CapabilitySet const held = CapabilitySet::grant_root({});
        source.set_capabilities(&held);

        std::atomic<bool> round_started{false};
        std::thread round_thread([&] {
            round_started.store(true, std::memory_order_release);
            auto r = drive(source.start_run(StartRun{user_message("go")}));
            check(r.has_value(), "the slow round itself converges successfully");
        });

        // Give the round thread a real head start so it genuinely acquires session_mutex_ FIRST --
        // matching test_rt_block_on.cpp's own established contention-inducing delay.
        while (!round_started.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        SlowSession target;
        target.initialize("target-session-not-yet-forked", Principal{"target-owner", ""});
        target.fork_from(source, "target-session");

        round_thread.join();

        check(target.history().size() == 2,
              "fork_from(), raced against a concurrent in-flight start_run() on the same source, "
              "copied the COMPLETE post-round history (2 messages: the real input plus the real final "
              "response) -- not a torn, incomplete snapshot a broken, unserialized fork_from() could "
              "have observed mid-round");
        if (target.history().size() == 2) {
            bool found_response = false;
            for (ContentItem const& item : target.history()[1].content) {
                if (auto const* t = std::get_if<Text>(&item.value)) {
                    if (t->text == "round complete") found_response = true;
                }
            }
            check(found_response,
                  "the copied history's second message is genuinely the round's own real, completed "
                  "response, not a placeholder or partial state");
        }
    }

    // [2] self-fork still works and does not self-deadlock (fork_from() never held session_mutex_
    // before this call, so the uncontended fast path applies even when source == *this).
    {
        SlowSession self_fork_session;
        self_fork_session.initialize("self-fork-session", Principal{"self-fork-owner", ""});
        self_fork_session.emplace_chat_client();
        std::string const session_id_before = self_fork_session.session_id();
        self_fork_session.fork_from(self_fork_session, "self-fork-session-renamed");
        check(self_fork_session.session_id() == "self-fork-session-renamed",
              "self-fork completes without deadlocking and updates session_id_ as expected");
        check(self_fork_session.session_id() != session_id_before, "session_id_ genuinely changed");
    }

    // [3] ADR-123: the REENTRANT case -- fork_from() called from INSIDE an in-flight start_run() round
    // on the very session being forked FROM, on the SAME thread. Bounded wait, never an infinite join:
    // before ADR-123 this hung forever (block_on()'s own busy-wait spinning against a lock only the
    // caller's own already-parked outer Guard could ever release), so a real regression here must FAIL
    // this check, not hang the whole test binary.
    {
        ReentrantSession source;
        source.initialize("reentrant-source", Principal{"reentrant-source-owner", ""});
        ReentrantChatClient& client = source.emplace_chat_client();
        client.set_self(&source);
        CapabilitySet const held = CapabilitySet::grant_root({});
        source.set_capabilities(&held);

        std::atomic<bool> round_done{false};
        result<AgentResponse> round_result;
        std::thread round_thread([&] {
            round_result = drive(source.start_run(StartRun{user_message("go")}));
            round_done.store(true, std::memory_order_release);
        });

        bool completed_in_time = false;
        for (int i = 0; i < 500; ++i) {  // up to ~5 seconds
            if (round_done.load(std::memory_order_acquire)) {
                completed_in_time = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        check(completed_in_time,
              "[3] a fork_from() call made reentrantly from inside chat() (same thread, session_mutex_ "
              "already held by the in-flight round) does NOT self-deadlock -- the round completes "
              "within a bounded wait instead of hanging forever");
        if (completed_in_time) {
            round_thread.join();
            check(round_result.has_value(), "[3] the round itself still converges successfully");
            check(g_reentrant_fork_ran.load(std::memory_order_acquire),
                  "[3] the reentrant fork_from() call inside chat() genuinely ran to completion, not "
                  "skipped or short-circuited");
        } else {
            // Never join a thread that may be permanently spinning in block_on()'s own busy-wait --
            // detach and let process exit reclaim it (this whole binary is about to return FAILURE).
            round_thread.detach();
        }
    }

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- AgentSession::fork_from() genuinely serializes against a "
                     "concurrent in-flight start_run() on its own source (real, two-thread race, not "
                     "merely reasoned about), closing the structural gap ADR-102 Phase 3/4 both "
                     "independently disclosed, without introducing a self-fork deadlock, AND (ADR-123) "
                     "does not self-deadlock when called reentrantly from inside a live round on the "
                     "same thread.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
