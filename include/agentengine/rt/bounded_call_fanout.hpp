#pragma once
// decisions/ADR-160-parallel-tool-batch-scheduler.md §5 "Fan-out" / SHOULD-FIX 7. A batch's real
// concurrent execution mechanism for step 8 (invoke) of an already-admitted tool call -- explicitly
// NOT `agentengine::rt::ThreadPool` (rt/thread_pool.hpp): that type resumes a `task<void>` exactly
// once and FAULTS it if it isn't `done()` yet, which would silently abandon (and leak the capability
// tickets of) any job whose `invoke()` genuinely suspends on something other than a nested,
// synchronously-resolving `task<T>` -- e.g. a nested `invoke_agent_tool()` waiting on a DIFFERENT
// session's own `AsyncMutex` under real contention. This type sidesteps that hazard class entirely
// by never touching a coroutine at all: each job is a plain callable run to completion, synchronously,
// on one real OS worker thread -- if it needs to block waiting on something, it just blocks, exactly
// like today's single-threaded sequential dispatch loop already does, generalized to N threads.
//
// SHORT-LIVED, NOT A PERSISTENT POOL: `run_jobs_bounded()` spawns up to `worker_cap` worker threads,
// runs every job in `jobs` to completion across them, and joins before returning -- a fork-join
// parallel-for, not a long-lived, globally-shared executor. This sidesteps every lifetime/ownership
// question a persistent pool would raise (who owns it, when does it shut down, what happens to a
// stuck job across pool destruction) at the cost of paying thread-creation overhead once per
// fan-out-eligible batch rather than reusing warm threads -- an accepted trade-off (ADR-160 §5): a
// tool-call batch's own admission/approval work already dwarfs a few thread creations, and the
// alternative (a persistent, engine-lifetime pool) is exactly the kind of standing infrastructure
// this ADR's own red-team-corrected design deliberately avoids introducing as a THIRD ad hoc
// concurrency mechanism (`Backgroundable`'s detached `std::thread`, `rt::ThreadPool`, and now this,
// each solving a different, narrower problem) without a stated reason for each one's own shape.
//
// BOUND, ALWAYS: `worker_cap` caps how many OS threads a single call ever creates, regardless of
// `jobs.size()` -- an unbounded-size batch fanned out to an unbounded thread count is a real
// resource-exhaustion vector (I8), no different in kind from why `Background<max_concurrent>`
// (007 §3) is a bounded capability rather than an unconditional grant. `run_jobs_bounded()` itself
// does not decide what that bound SHOULD be for a given caller (ADR-160 §6: the concurrency bound's
// ultimate SOURCE -- a capability, a run-level config knob, or a fixed constant -- is still an open
// question) -- it only guarantees that whatever bound the caller passes is respected exactly.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace agentengine::rt {

// Runs every callable in `jobs` exactly once, across at most `worker_cap` real worker threads
// (fewer if `jobs.size() < worker_cap`), blocking until all have completed. Each `Job` is invoked
// with no arguments and is expected to write its own result into storage it alone owns (e.g. its
// own captured output slot) -- this function coordinates ONLY which job index each worker claims
// next (a single shared, lock-free `std::atomic<std::size_t>` counter), never anything about a
// job's own data, so two jobs never contend on anything this function introduces.
//
// A job that throws is NOT caught here -- this mirrors `invoke_tool()`'s own existing convention
// (tool failures are communicated through `result<T>`'s error channel, not C++ exceptions; nothing
// in the pre-existing sequential dispatch path catches a stray exception from `tool->invoke()`
// either), so an uncaught throw from a job has exactly the same `std::terminate` consequence it
// would have had running sequentially on the caller's own thread today -- this function does not
// newly introduce that risk, and does not newly guard against it either.
template <class Job>  // Job: void() -- see file banner
inline void run_jobs_bounded(std::vector<Job>& jobs, std::size_t worker_cap) {
    if (jobs.empty()) return;
    std::size_t const worker_count = worker_cap == 0 ? 1 : std::min(worker_cap, jobs.size());

    std::atomic<std::size_t> next_index{0};
    // std::jthread, not std::thread: RAII join on scope exit covers every path out of this function,
    // including a job throwing on one worker while others are still running -- the same "destructor
    // guarantees the join, no hand-written join-on-every-exit-path code to get wrong" reasoning
    // `rt::ThreadPool`'s own file banner already established for this codebase's other primitives.
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&jobs, &next_index]() {
            for (;;) {
                std::size_t const i = next_index.fetch_add(1, std::memory_order_relaxed);
                if (i >= jobs.size()) return;
                jobs[i]();
            }
        });
    }
    // `workers` going out of scope below joins every thread -- this function does not return until
    // every job has actually completed.
}

}  // namespace agentengine::rt
