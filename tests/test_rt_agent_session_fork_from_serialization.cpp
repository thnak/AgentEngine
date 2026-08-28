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

#include "agentengine/rt/agent_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace agentengine;
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

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- AgentSession::fork_from() genuinely serializes against a "
                     "concurrent in-flight start_run() on its own source (real, two-thread race, not "
                     "merely reasoned about), closing the structural gap ADR-102 Phase 3/4 both "
                     "independently disclosed, without introducing a self-fork deadlock.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
