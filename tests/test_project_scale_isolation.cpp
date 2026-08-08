// Implements 030-Project-Workspace-and-Lifecycle.md §7's promotion gate G1: "N >= 100
// concurrently active Projects... pausing one measurably drops its member sessions' activation
// count to zero and its sandbox count to zero... with zero observable effect (latency, event
// ordering) on the other N-1." Milestone 6 Phase J
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md) -- the milestone's own exit
// criterion, half 2 of 2.
//
// 100 real AgentSession<CannedChatClient> actors, each with its OWN ProjectSupervisor (100 of
// those too -- 200 activations total, all lazily-declared actors per decision 8's own reasoning,
// never 200 threads). Every one of the 100 gets a real StartRun before anything is paused. ONE
// Project (#50) is paused; the other 99 are then each driven through a SECOND real Run and
// measured against their own first-round timing.
//
// SANDBOXES: not asserted here. Nothing in this milestone wires a real sandbox allocation into
// AgentSession or WorkflowSupervisor -- 008's sandbox backend is a separate M2 subsystem this
// phase's own actors never touch -- so "sandbox count drops to zero" would be vacuously true
// (nothing was ever allocated) rather than a real measurement. Named as a scope limitation, not
// silently claimed proven.
//
// MACHINE SAFETY (CLAUDE.md): 4 workers / 4 shards -- 200 actor activations is an actor count, not
// a thread count. No sleeps anywhere in this file.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/project/lifecycle.hpp"

using namespace quark;
using namespace agentengine;

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

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::yield();
    }
    return p();
}

class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"ok"};
        item.origin = content_origin::assistant;
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }
};
static_assert(ChatClient<CannedChatClient>, "CannedChatClient must satisfy the ChatClient concept");

using Session = AgentSession<CannedChatClient>;

[[nodiscard]] Message user_turn(std::string text, std::string message_id) {
    ContentItem item{};
    item.value  = Text{std::move(text)};
    item.origin = content_origin::user;
    Message m{};
    m.role       = role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

constexpr std::size_t kProjectCount = 100;
constexpr std::size_t kPausedIndex  = 50;  // an interior project, not the first or last -- the
                                            // failure mode a boundary-only test would miss

}  // namespace

int main() {
    auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    if (!built) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    Engine<>                   eng(*built);
    quark::detail::MessagePool pool(4096);
    LocalRouter                router(eng.post_courier(), pool);

    std::vector<std::unique_ptr<Session>>            sessions(kProjectCount);
    std::vector<std::unique_ptr<Activation>>         session_acts(kProjectCount);
    std::vector<ActorRef<Session>>                   session_refs;
    std::vector<std::unique_ptr<ProjectSupervisor>>  supervisors(kProjectCount);
    std::vector<std::unique_ptr<Activation>>         supervisor_acts(kProjectCount);
    std::vector<ActorRef<ProjectSupervisor>>         supervisor_refs;
    session_refs.reserve(kProjectCount);
    supervisor_refs.reserve(kProjectCount);

    for (std::size_t i = 0; i < kProjectCount; ++i) {
        sessions[i] = std::make_unique<Session>();
        sessions[i]->initialize("s-scale-" + std::to_string(i), ae::Principal{"p-scale", ""});
        session_acts[i] =
            std::make_unique<Activation>(sessions[i].get(), Session::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Session>(i), *session_acts[i]);
        session_refs.push_back(router.get<Session>(i));

        supervisors[i] = std::make_unique<ProjectSupervisor>();
        supervisor_acts[i] = std::make_unique<Activation>(
            supervisors[i].get(), ProjectSupervisor::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<ProjectSupervisor>(i), *supervisor_acts[i]);
        supervisor_refs.push_back(router.get<ProjectSupervisor>(i));
        supervisors[i]->register_member(session_refs[i]);
    }

    eng.start();

    // =========================================================================================
    // Round 1: every one of the 100 Projects does a real Run, before anything is paused.
    // =========================================================================================
    bool round1_ok = true;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        auto r = block_on(session_refs[i].ask<AgentResponse>(StartRun{user_turn("hi", "m-1")}));
        if (!r.has_value() || sessions[i]->last_run_id() != ("s-scale-" + std::to_string(i) + ":run:1")) {
            round1_ok = false;
        }
    }
    check(round1_ok, "setup: all 100 Projects' member sessions complete a real first Run");

    // Baseline timing over a representative sample of the 99 that will stay UNPAUSED (every 10th,
    // skipping the one about to be paused) -- measured before touching #kPausedIndex at all. This
    // extra ask bumps the SAMPLED sessions' own run counters one further than the rest, so
    // `expected_run` (below) tracks each session's own count individually rather than assuming
    // every one of the 99 is at the same number after this point.
    std::vector<std::size_t> sample;
    for (std::size_t i = 0; i < kProjectCount; i += 10) {
        if (i != kPausedIndex) sample.push_back(i);
    }
    std::vector<std::uint64_t> expected_run(kProjectCount, 1);
    auto const                 t0 = std::chrono::steady_clock::now();
    for (std::size_t i : sample) {
        (void)block_on(session_refs[i].ask<AgentResponse>(StartRun{user_turn("baseline", "m-b")}));
        expected_run[i] = 2;
    }
    auto const baseline_elapsed = std::chrono::steady_clock::now() - t0;

    // =========================================================================================
    // 030 §7 G1 itself: pause ONE Project (#50).
    // =========================================================================================
    ProjectRecord rec;
    rec.project_id   = "proj-scale-" + std::to_string(kPausedIndex);
    rec.principal_id = "p-scale";
    rec.status        = project_status::active;

    ProjectRecord paused = pause_project(supervisor_refs[kPausedIndex], *supervisors[kPausedIndex], rec);
    check(paused.status == project_status::paused, "G1: pause_project marks the target Paused");

    check(wait_until([&] { return session_acts[kPausedIndex]->went_dormant(); }, std::chrono::seconds(2)),
          "G1: the paused Project's member session activation drops to zero (census)");
    check(wait_until([&] { return supervisor_acts[kPausedIndex]->went_dormant(); }, std::chrono::seconds(2)),
          "G1: the paused Project's OWN supervising actor activation drops to zero too (census)");

    // =========================================================================================
    // G1's own bar: ZERO OBSERVABLE EFFECT on the other N-1 -- correctness (every one still works,
    // its OWN run sequence uninterrupted) and latency (the same sample, timed again, is not
    // meaningfully slower than the untouched baseline).
    // =========================================================================================
    bool         all_others_ok = true;
    std::size_t  correct_count = 0;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        if (i == kPausedIndex) continue;
        auto r = block_on(session_refs[i].ask<AgentResponse>(StartRun{user_turn("again", "m-2")}));
        ++expected_run[i];
        bool const ok = r.has_value() && sessions[i]->last_run_id() ==
                                             ("s-scale-" + std::to_string(i) + ":run:" +
                                              std::to_string(expected_run[i]));
        if (ok) ++correct_count;
        if (!ok) all_others_ok = false;
    }
    check(all_others_ok && correct_count == kProjectCount - 1,
          "G1: all 99 OTHER Projects' member sessions complete a real SECOND Run, each continuing "
          "its OWN run-id sequence uninterrupted -- pausing #50 disturbed none of their event "
          "ordering");

    auto const t1 = std::chrono::steady_clock::now();
    for (std::size_t i : sample) {
        (void)block_on(session_refs[i].ask<AgentResponse>(StartRun{user_turn("after-pause", "m-a")}));
    }
    auto const after_pause_elapsed = std::chrono::steady_clock::now() - t1;

    std::fprintf(stderr, "  .. baseline sample latency = %lld us, after-pause sample latency = %lld us\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(baseline_elapsed).count()),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(after_pause_elapsed).count()));
    // Generous bound (3x), not a tight one -- machine noise is real and this is a single-shot
    // measurement, not a benchmark harness. The property being caught is a STRUCTURAL regression
    // (an accidental global lock/serialization introduced by pause_project), which would show up
    // as an order-of-magnitude change, not a percentage one.
    check(after_pause_elapsed < baseline_elapsed * 3,
          "G1: the SAME sample of untouched Projects is not meaningfully slower after pausing #50 "
          "-- pause_project's passivate() calls are scoped to ONE ActorId at a time (ADR-034), so "
          "no shared lock or serialization point touches the other 99");

    // Paused #50 itself still fails closed / stays inert -- not part of G1's own text, but the
    // natural sanity check that "paused" actually means something for the one Project it targets.
    check(session_acts[kPausedIndex]->went_dormant() && supervisor_acts[kPausedIndex]->went_dormant(),
          "sanity: #50 is still Dormant after the other 99 did real work -- it was not incidentally "
          "reactivated by anything above");

    eng.stop();

    if (g_failures == 0) {
        std::printf("test_project_scale_isolation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_project_scale_isolation: %d failure(s)\n", g_failures);
    return 1;
}
