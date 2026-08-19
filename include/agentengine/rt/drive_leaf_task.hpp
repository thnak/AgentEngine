#pragma once
// Implements decisions/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md §3 Design B --
// `drive_leaf_task<T>()`, a narrow, opt-in "leaf task" driver reusing `rt::ThreadPool::run_job()`'s
// own proven idiom (rt/thread_pool.hpp) for a `task<T>` that is ALREADY running on the calling
// thread -- no new thread, no submission; this is an inline, same-thread drive, not a dispatch.
//
// SOUNDNESS IS A PER-CALLER CONTRACT, NOT TYPE-ENFORCED: `t` must never internally await anything
// except other task<T>/task<void> in a pure symmetric-transfer chain -- ideally, like
// `OpenAIEmbedder::embed_batch()` today, await NOTHING at all. Call sites must check the producing
// conformer's own `synchronous_leaf` declaration (core/embedder.hpp's `Embedder` concept) before
// calling this; never call it against a conformer that declares `false`. Do NOT cite `ThreadPool`'s
// own scope comment as proof this is safe in general -- ADR-064's own red-team found that comment
// does not hold for the code `ThreadPool` itself drives in production today (`AgentSession`'s
// `session_mutex_.lock()` genuinely suspends under contention, see thread_pool.hpp's own top
// comment). This function instead leans on a STRONGER, PER-CONFORMER, independently-verified
// property: a specific `Embedder::embed_batch()` that awaits nothing at all, which no amount of
// nesting depth changes. `synchronous_leaf` carries a materially HIGHER review bar than this
// codebase's other declared traits (`Capabilities<...>`/`EffectClass<...>`): a wrong declaration
// here fails via undefined behavior (resuming a not-actually-ready coroutine handle), not a soft,
// contained error.
//
// BOUND IS 1, NOT A LARGER NUMBER -- deliberately: under the stated contract (only nested
// task<T>/task<void> awaits, real C++20 symmetric transfer), a conforming leaf task reaches done()
// in EXACTLY ONE resume() call, regardless of nesting depth -- symmetric transfer never returns
// control to this function until the whole chain completes or hits a genuinely-suspending non-task
// awaitable. There is no legitimate scenario where a second resume() would ever be correct, so a
// larger "safety margin" bound does not add safety -- it only delays detecting a violated contract.
// CAVEAT, stated plainly rather than overclaimed: this bound is a hang-preventer for an ALREADY-
// suspended, already-corrupted-state coroutine, not a soundness guarantee -- by the time a second
// resume() would fire, `await_resume()` on the foreign awaitable has already run against a
// precondition that was never satisfied. The REAL protection is the `synchronous_leaf` review
// discipline at the declaration site; this bound only stops an already-bad situation from ALSO
// becoming an unbounded hang.

#include "agentengine/core/error.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// Double-wrapped `result<T>` at the call site is intentional, not an oversight: every
// `Embedder::embed_batch()` already returns `task<result<U>>` (this codebase's own convention --
// the task's OWN success/fault channel is for coroutine-level failure, the ORDINARY provider error
// flows through the `result<U>` value itself). So `drive_leaf_task(embedder.embed_batch(...))`
// returns `result<result<U>>` -- TWO layers, each meaning something different, and a caller must
// unwrap both explicitly:
//
//   auto driven = rt::drive_leaf_task(embedder.embed_batch(query_batch, ctx));
//   if (!driven) return std::unexpected(driven.error());       // OUTER: task-level fault (rare) or
//                                                                // a violated synchronous_leaf contract
//   auto& embedded = *driven;                                  // result<vector<vector<float>>>
//   if (!embedded) return std::unexpected(embedded.error());   // INNER: the ORDINARY provider error
//                                                                // channel (network failure, bad
//                                                                // response) -- the common failure case
// Red-team (2026-08-19, against this implementation, not just the paper design) found the first
// version of this function wrapped only `take_value()`'s own rethrow in try/catch, leaving
// `resume()` itself uncovered -- an unexplained deviation from `ThreadPool::run_job()`
// (rt/thread_pool.hpp), the idiom this function's own top comment claims to reuse, whose try/catch
// explicitly covers resume() too as "defense-in-depth... against a future change to task<T>'s
// contract or misuse this type can't statically rule out." Verified against task<T>'s actual
// promise_type (task.hpp): `unhandled_exception()` is `noexcept` and never rethrows, so `resume()`
// cannot throw from a body exception TODAY -- this was not an exploitable gap, but the asymmetry was
// real and unexplained. Fixed by wrapping the whole drive (resume() through take_value()) in one
// try/catch, matching run_job()'s shape exactly rather than reasoning independently about which
// calls can throw.
template <class T>
[[nodiscard]] result<T> drive_leaf_task(task<T> t) {
    try {
        if (!t.done()) t.resume();
        if (!t.done()) {
            return std::unexpected(error{
                failure_class::fatal,
                "drive_leaf_task() needed more than one resume() to reach done() -- the task "
                "suspended on something other than a nested task<T>/task<void>, violating its "
                "synchronous_leaf contract. The coroutine state from here on is not trustworthy (see "
                "this function's own comment) -- this error exists to stop cleanly, not to recover.",
                "rt.leaf_task_contract_violation"});
        }
        // take_value() rethrows a fault via std::rethrow_exception (a genuine C++ exception escaping
        // the coroutine body -- allocation failure, a bug, NOT the ordinary provider-error channel,
        // which for every task<result<U>> in this codebase already flows through the normal
        // co_return/return_value path as an ordinary std::unexpected(...) value, not a fault).
        // Translated to ae::error here so a caller never has to catch task<T>'s exception-based fault
        // protocol directly.
        return t.take_value();
    } catch (...) {
        return std::unexpected(error{failure_class::transient, "leaf task faulted", "rt.leaf_task_faulted"});
    }
}

}  // namespace agentengine::rt
