// PROVE-PHASE POSITIVE PROBE: real concurrent race between materialize() (holding exclusivity_ for
// 150ms, simulating real async work) and release_branch() from a DIFFERENT thread, started as close
// to simultaneously as possible. If §19.3's fix genuinely closes round 4's TOCTOU, release_branch()'s
// critical section must never overlap materialize()'s -- proven here by inspecting the real event
// log's ordering, not assumed from the source code alone.

#include "sandbox_session.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    // --- Test 1: real concurrent race, repeated 20 times to rule out a lucky ordering ------------
    // TWO valid outcomes exist, both correct per §19.3's design, and this test must accept either:
    //   (a) materialize() wins the race: it runs to completion (2 log events), THEN release_branch()
    //       runs (2 more events) -- 4 events total, no overlap.
    //   (b) release_branch() wins the race: it completes first (2 events, sets released_=true), and
    //       materialize() -- serialized behind the SAME lock -- observes released_==true the instant
    //       it acquires the lock and fails closed WITHOUT ever logging "materialize:enter" (this
    //       probe's own code only logs after the released_ check) -- 2 events total, and
    //       materialize()'s own result must reflect the rejection. An earlier version of this test
    //       incorrectly asserted "always 4 events", which was a test-authoring bug (same class of
    //       mistake identity_authority's own probe_positive.cpp made and fixed): outcome (b) is not a
    //       failure, it is the exclusivity fix working exactly as intended -- materialize() must never
    //       be allowed to proceed once release_branch() has already resolved the session.
    int outcome_a_count = 0, outcome_b_count = 0;
    for (int trial = 0; trial < 20; ++trial) {
        SandboxEventLog log;
        SandboxSession session(&log);

        result<void> materialize_result;
        std::thread t_materialize(
            [&]() { materialize_result = block_on(session.materialize(150)); });
        // Give materialize() a head start into its critical section on most runs, but also let some
        // trials race genuinely close to the start -- both are real, valid interleavings a scheduler
        // could produce.
        std::this_thread::sleep_for(std::chrono::milliseconds(trial % 3 == 0 ? 0 : 10));
        std::thread t_release([&]() { (void)block_on(std::move(session).release_branch()); });

        t_materialize.join();
        t_release.join();

        auto idx = [&](std::string const& name) {
            for (std::size_t i = 0; i < log.events.size(); ++i)
                if (log.events[i] == name) return i;
            return static_cast<std::size_t>(-1);
        };

        if (log.events.size() == 4) {
            // Outcome (a): materialize() won and ran to completion.
            CHECK(materialize_result.has_value());
            std::size_t mat_enter = idx("materialize:enter");
            std::size_t mat_exit = idx("materialize:exit");
            std::size_t rel_enter = idx("release_branch:enter");
            std::size_t rel_exit = idx("release_branch:exit");
            CHECK(mat_enter < mat_exit);
            CHECK(rel_enter < rel_exit);
            // No overlap: materialize's critical section must fully finish before release's starts
            // (release can never win a race it started after materialize already holds the lock).
            CHECK(mat_exit < rel_enter);
            ++outcome_a_count;
        } else {
            // Outcome (b): release_branch() won; materialize() must be rejected, not silently
            // succeed and not silently do nothing -- its own result must say so.
            CHECK(log.events.size() == 2);
            CHECK(idx("materialize:enter") == static_cast<std::size_t>(-1));
            CHECK(!materialize_result.has_value());
            CHECK(materialize_result.error().code == "sandbox_session.already_released");
            ++outcome_b_count;
        }
    }
    std::printf("[1] 20 real concurrent materialize()/release_branch() races: PASS "
                "(%d resolved as materialize-first, %d resolved as release-first; critical "
                "sections NEVER overlapped, and every release-first materialize() call was "
                "correctly rejected, not silently ignored)\n", outcome_a_count, outcome_b_count);

    // --- Test 2: released_ flag genuinely gates a LATER call, after release_branch() completed ---
    {
        SandboxEventLog log;
        SandboxSession session(&log);
        auto rel = block_on(std::move(session).release_branch());
        CHECK(rel.has_value());
        auto mat = block_on(session.materialize(1));
        CHECK(!mat.has_value());
        CHECK(mat.error().code == "sandbox_session.already_released");
    }
    std::printf("[2] materialize() after a completed release_branch(): REJECTED "
                "(sandbox_session.already_released)\n");

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
