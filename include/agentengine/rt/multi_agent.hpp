#pragma once
// Implements the v1 slice of `docs/planning/dynamic-multi-agent-fanout-design-draft.md`
// (rounds 1-14: design -> red-team -> corrected design, 3 independent fresh-context red-team passes)
// -- host-authored dynamic multi-agent fan-out over `agentengine::rt::AgentSession`. NOT an ADR yet;
// this file is the "prove" phase real code + tests (`tests/test_rt_multi_agent.cpp`) that a future ADR
// will cite as its executed evidence, per `decisions/README.md`'s own requirement that an ADR record
// real, run evidence, not a design read as correct.
//
// v1 SCOPE, DELIBERATELY NARROWED (round 12): `spawn()` is safe to call ONLY from a
// `ToolDescriptor::invoke` body running inside an ORDINARY `AgentSession`'s own turn loop -- never
// from a plain `WorkflowSupervisor::ExecutorBody`, and never (until it exists) from an `agent`-kind
// executor's internal session. Both of those call shapes source their `EffectContext` from
// `WorkflowSupervisor::contexts_`, a value populated ONCE at `initialize()` time (before the run even
// has a `run_id_`) -- a wiring-time-only ceiling, not the genuinely live, per-dispatch value
// `apply_dispatch_authority()` maintains for an ordinary `AgentSession`. Calling `spawn()` from either
// unsafe caller class is a design error this file does not attempt to detect (round 10's own residual:
// `EffectContext` carries no provenance marker to check against) -- it is a documented, not yet
// structurally enforced, restriction.
//
// NOT included in this file (named, not silently dropped): `pipeline()` (the gap doc's original
// multi-stage sketch was never actually consumed by any of the 14 design rounds' concrete use cases;
// `parallel()`'s own `max_in_flight` throttle already provides bounded-concurrency dispatch for the
// single-stage "one child per item" shape this design actually needs); `ScatterGather` (Primitive 2,
// blocked on `executor_kind::agent`'s still-unbuilt runtime bridge); `FanOutProvider` (round 8's
// `ContextProvider`-wrapped model-facing surface -- a separate, later increment, not part of
// Primitive 1's own native-library scope); vendor batch-inference sharing (round 13: structurally
// incompatible with this file's `ThreadPool`-based dispatch, needs `OpenPort`/`Interaction`'s durable
// suspend-resume instead, which is `batch-inference-coalescing-design-draft.md`'s mechanism, not this
// one's); write-capable `MemoryProvider` sharing across concurrently-dispatched children (round 14:
// `core/memory.hpp`'s own `write_seq` prediction is a documented, unresolved concurrent-writer race --
// out of scope for this file to fix, and not exercised by anything here).

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/retry_policy.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::rt::multi_agent {

// round 2's own finding: `is_retryable(failure_class)` is defined twice in this codebase already
// (`workflow_supervisor.hpp`: `transient || resource`; `model_call_gateway.hpp`'s
// `model_call_gateway_detail::is_retryable`: `transient` only) with no comment reconciling the
// divergence -- a real, pre-existing, independently-landable fix this file does not attempt (would
// need picking a winner for BOTH existing call sites, not just this new one). This file reuses the
// BROADER definition (round 2's own decision): a `resource`-classified failure (the same class
// `run.token_budget_exceeded` itself uses) is exactly what a bounded retry exists for, and
// `ModelCallGateway`'s own narrower version already sits behind its own separate backoff+breaker
// layer this file's callers do not have. `contract`/`policy` failures are NEVER retried -- retrying a
// capability denial can never succeed (`attenuate()` fails closed identically every attempt) and would
// only burn attempts/`Budget` spend on an unwinnable call (an I2-adjacent hazard, round 2).
[[nodiscard]] inline bool is_retryable(agentengine::failure_class klass) noexcept {
    return klass == agentengine::failure_class::transient ||
           klass == agentengine::failure_class::resource;
}

// A child's construction is entirely the factory's own responsibility -- session id, chat client,
// history/context-provider composition, any `token_budget_`/`max_turns` the host wants for the
// child's OWN run. `spawn()` supplies exactly one input the factory cannot know ahead of time: the
// resolved child `Principal` (round 7/9's own fix -- sourced from the CALLER's live `EffectContext`,
// never a bare caller-fabricated value, round 1's original FATAL #2). Capabilities are set by `spawn()`
// itself AFTER construction, via the already-existing `set_capabilities()` setter -- no factory
// involvement needed there.
//
// MUST return a genuinely fresh, uniquely-owned session every call (round 1 fix 3) -- a `SessionFactory`
// that memoizes or returns a reference to something reused across calls reopens the exact concurrent
// double-resume race `agent-as-workflow-executor-design-draft.md` found once already for the
// static-graph case. `std::unique_ptr` is the type-level guarantee: nothing this file's own dispatch
// code does can alias two calls' sessions together, because each call gets its own distinct object.
template <class ChatClientT, class StateT, class HistoryProviderT>
using SessionFactory = std::function<std::unique_ptr<AgentSession<ChatClientT, StateT, HistoryProviderT>>(
    agentengine::Principal const& child_principal)>;

// Round 6/7/9/12: two independent ceilings guarding two different resources, checked together under
// ONE mutex (not two independent atomics -- round 9's own residual named that as a subtler TOCTOU risk
// than the original unguarded design round 2 first found). `max_spawns` is a true reservation --
// admitting more than `max_spawns` is a runtime-impossible case, matching `attenuate()`'s own
// fail-closed discipline for capabilities. `max_tokens` is an honest backstop, NOT a reservation --
// per-child token cost is unknowable before that child actually runs (round 7's own finding), so this
// can only ever refuse NEW work once the running total (from already-completed children) has crossed
// the ceiling; it cannot bound what a single already-admitted batch of children collectively spend
// (round 11's finding) -- `max_in_flight` exists specifically to bound the BLAST RADIUS of that gap
// (round 12), not to close it, because closing it would need per-child cost to be knowable in advance,
// which it structurally is not.
//
// Not copyable/movable -- the same anti-aliasing discipline `ThreadPool` itself already applies
// (`thread_pool.hpp`), for the same reason: worker-thread code holds a reference to this object for
// the duration of a dispatch, so relocating it while dispatch is in flight would be a use-after-move
// hazard, not merely bad style.
class Budget {
public:
    Budget(std::size_t max_spawns, std::uint64_t max_tokens, std::size_t max_in_flight)
        : in_flight_slots_(static_cast<std::ptrdiff_t>(max_in_flight == 0 ? 1 : max_in_flight)),
          max_spawns_(max_spawns),
          max_tokens_(max_tokens) {}

    Budget(Budget const&)            = delete;
    Budget& operator=(Budget const&) = delete;
    Budget(Budget&&)                 = delete;
    Budget& operator=(Budget&&)      = delete;

    // Atomic admission check for a WHOLE `parallel()` batch: refuses (reserves nothing) if admitting
    // `spawn_count` more would exceed `max_spawns`, OR if the running token total has ALREADY crossed
    // `max_tokens` (the backstop -- see the class banner). Never a partial admission of a batch.
    [[nodiscard]] bool try_reserve(std::size_t spawn_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (spawns_reserved_ + spawn_count > max_spawns_) return false;
        if (tokens_spent_ >= max_tokens_) return false;
        spawns_reserved_ += spawn_count;
        return true;
    }

    // BLOCKS the calling thread (an ordinary semaphore acquire, not a coroutine suspension) until a
    // dispatched-but-not-yet-debited slot is free. Called from `parallel()`'s own DRIVING loop -- the
    // code that decides whether to submit another job to `ThreadPool` yet -- never from inside a job
    // already submitted TO the pool. This is deliberate (round 12): a plain thread block here never
    // touches `ThreadPool`'s own documented "genuine external suspension" hazard
    // (`thread_pool.hpp`'s own file banner) at all, because nothing here is a coroutine parking
    // mid-body -- it is the ordinary blocking wait an admission loop makes before its next submission.
    void acquire_in_flight_slot() { in_flight_slots_.acquire(); }

    // Success path: debits real, observed spend and releases the in-flight slot together -- one call
    // site, so a caller cannot forget to pair a debit with its release.
    void debit_tokens(agentengine::Usage const& usage) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tokens_spent_ += usage.input_tokens + usage.output_tokens;
        }
        in_flight_slots_.release();
    }

    // Failure path: a child that never produced an `AgentResponse` has no `Usage` to debit (a real,
    // named residual -- round 14: if the vendor actually billed for tokens generated before the
    // failure, this codebase has no mechanism to know that amount). The in-flight slot must still be
    // released regardless of why the child ended, or a failing child would leak its slot permanently.
    void release_in_flight_slot_without_debit() { in_flight_slots_.release(); }

    [[nodiscard]] std::size_t remaining_spawns() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_spawns_ > spawns_reserved_ ? max_spawns_ - spawns_reserved_ : 0;
    }
    [[nodiscard]] std::uint64_t remaining_tokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_tokens_ > tokens_spent_ ? max_tokens_ - tokens_spent_ : 0;
    }

private:
    mutable std::mutex        mutex_;
    std::counting_semaphore<> in_flight_slots_;
    std::size_t               spawns_reserved_ = 0;
    std::uint64_t             tokens_spent_    = 0;
    std::size_t const         max_spawns_;
    std::uint64_t const       max_tokens_;
};

// One dynamically-spawned child. `parent_call_ctx` is the LIVE `EffectContext` governing the CURRENT
// call -- e.g. exactly what a `ToolDescriptor::invoke(args, EffectContext& ctx)` body already receives
// (round 7/9's fix; NEVER `AgentSession::capabilities()`/`::principal()`, both session-level STATIC
// baselines, wrong under Tier-3/ADR-061 -- round 6's original FATAL findings).
template <class ChatClientT, class StateT, class HistoryProviderT>
[[nodiscard]] task<agentengine::result<AgentResponse>> spawn(
    agentengine::EffectContext const& parent_call_ctx,
    SessionFactory<ChatClientT, StateT, HistoryProviderT> const& factory,
    StartRun request,
    std::vector<agentengine::Capability> const& narrower_grant,
    std::optional<agentengine::Principal> delegate_principal = std::nullopt) {
    if (!parent_call_ctx.capabilities) {
        co_return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "spawn() requires a live capability grant on the caller's EffectContext",
            "multi_agent.no_live_capabilities"});
    }

    agentengine::Principal child_principal = parent_call_ctx.principal;
    if (delegate_principal.has_value()) {
        if (!agentengine::principal_admitted_for(*delegate_principal, parent_call_ctx.principal)) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "the requested delegate principal is not admitted for the caller's own principal",
                "multi_agent.delegate_not_admitted"});
        }
        child_principal = *delegate_principal;
    }

    // attenuate() fails closed if narrower_grant isn't a subset of what the CALLER actually holds
    // right now -- makes "child exceeds parent" a runtime-impossible case, not a documentation note
    // (round 1's own fix 1, unchanged in shape; only WHERE the parent set comes from was ever wrong).
    agentengine::result<agentengine::CapabilitySet> narrowed =
        parent_call_ctx.capabilities->attenuate(narrower_grant);
    if (!narrowed) co_return std::unexpected(narrowed.error());

    std::unique_ptr<AgentSession<ChatClientT, StateT, HistoryProviderT>> child = factory(child_principal);
    if (!child) {
        co_return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "SessionFactory returned a null session -- contract violation (must return a fresh, owned "
            "session)",
            "multi_agent.factory_returned_null"});
    }
    // Lifetime: `narrowed` (a local) outlives `child` (declared after it) within this coroutine frame,
    // and this function co_awaits `child->start_run()` to completion before EITHER goes out of scope --
    // matching `EffectContext::capabilities`' own documented "caller must outlive every use" contract
    // (`effect_context.hpp`) applied to `set_capabilities()`'s identical non-owning-pointer shape.
    child->set_capabilities(&*narrowed);

    co_return co_await child->start_run(std::move(request));
}

// A retry attempt is a genuinely new session + resource mint (round 1 fix 3/6), not exempt from the
// same cost `Budget` exists to bound (round 5/14's own conclusion) -- every attempt, not just the
// first, goes through `try_reserve`/`acquire_in_flight_slot`/`debit_tokens` exactly like an
// independent `spawn()` call would from `parallel()`'s own dispatch loop. `budget` is mandatory here
// (unlike bare `spawn()`) because retry's whole point is bounded reuse of a real ceiling.
template <class ChatClientT, class StateT, class HistoryProviderT>
[[nodiscard]] task<agentengine::result<AgentResponse>> spawn_with_retry(
    agentengine::EffectContext const& parent_call_ctx,
    SessionFactory<ChatClientT, StateT, HistoryProviderT> const& factory,
    StartRun const& request,
    std::vector<agentengine::Capability> const& narrower_grant,
    agentengine::RetryPolicy const& policy,
    Budget& budget,
    std::optional<agentengine::Principal> delegate_principal = std::nullopt) {
    agentengine::result<AgentResponse> last_error = std::unexpected(agentengine::error{
        agentengine::failure_class::contract, "RetryPolicy::max_attempts must be >= 1",
        "multi_agent.zero_max_attempts"});

    for (std::uint32_t attempt = 0; attempt < policy.max_attempts; ++attempt) {
        if (!budget.try_reserve(1)) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::resource, "fan-out budget exceeded before dispatch",
                "multi_agent.fanout_budget_exceeded"});
        }
        budget.acquire_in_flight_slot();

        // A fresh copy of `request` per attempt -- StartRun carries a Message the caller may not want
        // moved-from twice; a retried attempt is a genuinely separate spawn(), not a resumption of the
        // failed one (round 1 fix 3's own "fresh, owned" contract applies per attempt too).
        agentengine::result<AgentResponse> outcome =
            co_await spawn(parent_call_ctx, factory, request, narrower_grant, delegate_principal);

        if (outcome.has_value()) {
            budget.debit_tokens(outcome->usage);
            co_return outcome;
        }
        budget.release_in_flight_slot_without_debit();

        last_error = std::move(outcome);
        // Contract/policy failures are never retried -- attenuate() fails closed IDENTICALLY on every
        // attempt, so retrying a denial can never succeed; it would only burn attempts and Budget spend
        // on an unwinnable call (round 2's own I2-adjacent finding).
        if (!is_retryable(last_error.error().klass)) co_return last_error;
    }
    co_return last_error;
}

namespace detail {

// A coroutine lambda's frame holds only a POINTER to the closure, destroyed at the end of the
// immediately-invoked expression that constructs it -- test_rt_thread_pool.cpp's own "hard-won
// lesson" for why every `ThreadPool`-submitted job in this codebase is a plain, named free function,
// never a lambda capturing by reference into a temporary. `thunk`/`budget`/`out_slot` are all
// caller-owned and outlive this job (the caller's own `futures` vector is `.get()`'d, synchronously,
// before `parallel()` returns -- the same "always fully awaited before scope exit" contract `spawn()`
// itself relies on).
inline task<void> run_one_thunk(std::function<task<agentengine::result<AgentResponse>>()>* thunk,
                                 Budget* budget, agentengine::result<AgentResponse>* out_slot) {
    try {
        agentengine::result<AgentResponse> outcome = co_await (*thunk)();
        if (outcome.has_value()) {
            budget->debit_tokens(outcome->usage);
        } else {
            budget->release_in_flight_slot_without_debit();
        }
        *out_slot = std::move(outcome);
    } catch (std::exception const& e) {
        budget->release_in_flight_slot_without_debit();
        *out_slot = std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                        std::string("fan-out child job threw: ") + e.what(),
                                                        "multi_agent.child_threw"});
    } catch (...) {
        budget->release_in_flight_slot_without_debit();
        *out_slot = std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                        "fan-out child job threw a non-std::exception",
                                                        "multi_agent.child_threw"});
    }
    co_return;
}

}  // namespace detail

// All-at-once dispatch, a real barrier (awaits every thunk before returning any result) -- a thunk that
// faults resolves to an error in its own slot; this function itself never throws (round 1's own
// "Workflow tool's own parallel() semantics" precedent). `max_in_flight` (via `budget.
// acquire_in_flight_slot()`) throttles how many thunks may be dispatched-but-not-yet-debited at once;
// with `max_in_flight >= thunks.size()`, behavior is byte-for-byte the traditional "submit all N at
// once" shape -- the throttle narrows exposure, it does not change what this function promises to its
// own caller (round 12).
[[nodiscard]] inline task<std::vector<agentengine::result<AgentResponse>>> parallel(
    ThreadPool& pool, Budget& budget,
    std::vector<std::function<task<agentengine::result<AgentResponse>>()>> thunks) {
    std::vector<agentengine::result<AgentResponse>> out(
        thunks.size(), std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                           "fan-out budget exceeded before dispatch",
                                                           "multi_agent.fanout_budget_exceeded"}));
    if (!budget.try_reserve(thunks.size())) co_return out;

    std::vector<std::future<JobOutcome>> futures;
    futures.reserve(thunks.size());
    for (std::size_t i = 0; i < thunks.size(); ++i) {
        budget.acquire_in_flight_slot();
        futures.push_back(pool.submit(detail::run_one_thunk(&thunks[i], &budget, &out[i])));
    }
    for (auto& f : futures) {
        (void)f.get();  // out[i] is already populated by run_one_thunk() itself, success or failure --
                         // this just synchronizes-with each job's completion before returning.
    }
    co_return out;
}

}  // namespace agentengine::rt::multi_agent
