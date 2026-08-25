#pragma once
// Implements docs/planning/agent-session-decomposition-design-draft.md §2a -- the standing-effects/
// background-task bookkeeping cluster factored out of rt::AgentSession (agent_session.hpp). Owns
// StandingEffect storage and the background-completion handoff queue, plus the small pure
// operations on them; ToolTable/ApprovalDecider-shaped dispatch and per-call capability/authority
// resolution stay in AgentSession, the only caller -- this type deliberately knows nothing about
// either, so it stays a small, densely testable value-ish type rather than a second "everything"
// object. See the design draft for the full rationale (including why the entry-point/interaction-
// resolution/run_rounds() cluster is NOT factored out the same way) and the red-team findings that
// shaped this exact shape (guard-lifetime requirement, the honest drain_ready() ordering delta, the
// schedule_wakeup_impl naming, and why moving this state into a member subobject adds no lifetime
// risk -- AgentSession is structurally immovable).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/standing_effect.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::rt {

// What a completed background native tool call hands back. Moved verbatim from agent_session.hpp
// (Slice 3's own design, that file's banner has the full delivery-path rationale) -- plain data
// pushed into a BackgroundCompletionQueue, never routed through anything actor-shaped.
// ae-naming-lint: allow BackgroundTaskDone — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct BackgroundTaskDone {
    std::string handle_id;
    std::string call_id;
    agentengine::ToolResult result;  // was: bool ok. ok is now !result.is_error.
};

// The thread-safe handoff point between a detached worker thread (tool_pipeline.hpp's
// background_task() step 8) and whichever coroutine later drains it under AgentSession's own
// session_mutex_. Its OWN mutex, never session_mutex_ -- a plain std::thread cannot co_await
// anything, so the one lock it touches must be acquirable synchronously. Held behind a shared_ptr
// specifically so a worker's completion closure can capture a weak_ptr instead of a reference into
// the (possibly by then destroyed) session -- AgentSession is structurally immovable (its
// rt::AsyncMutex member's deleted copy ctor with no declared move ctor suppresses every implicit
// move member on AgentSession, and every real call site either heap-owns it via unique_ptr or
// constructs it in place, never relocated), so a StandingEffectRegistry held as a plain
// AgentSession member subobject is pinned for the session's whole life exactly as this queue
// already was when it lived directly on AgentSession -- moving it here adds no new lifetime risk.
// ae-naming-lint: allow BackgroundCompletionQueue — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct BackgroundCompletionQueue {
    std::mutex                     m;
    std::deque<BackgroundTaskDone> pending;
};

// One drained completion, ready for AgentSession to emit a run event for. Deliberately carries no
// emission logic itself -- the caller (AgentSession) must keep its AsyncMutex::Guard alive across
// both the drain_ready() call that produces these AND the loop that emits for them, in the same
// function scope; see the design draft §3 item 4 for why factoring the emit loop out from under the
// guard would reopen a real I1 window.
struct DrainedCompletion {
    std::string              owner_run_id;
    std::string              call_id;
    agentengine::ToolResult  result;
};

// Owns StandingEffect storage and the background-completion queue for one AgentSession. Plain data
// plus small pure operations -- no locking of its own opinion (AgentSession's session_mutex_ is
// what serializes access, exactly as it did when this state lived directly on AgentSession: the
// six AgentSession wrapper methods that call into this type keep their exact today's locked/
// unlocked split -- start_background_task()/schedule_wakeup() lock, cancel/list/due don't, ADR-061
// §20.5/§24.2's own deliberate choice, unchanged by this move) and no knowledge of ToolTable/
// ApprovalDecider/capability checking (that stays in AgentSession, which resolves per-call
// authority/capabilities itself and calls in here only for the bookkeeping half).
class StandingEffectRegistry {
public:
    StandingEffectRegistry() : completions_(std::make_shared<BackgroundCompletionQueue>()) {}

    [[nodiscard]] std::string mint_handle_id(std::string const& session_id) {
        return session_id + ":standing:" + std::to_string(++counter_);
    }

    [[nodiscard]] std::size_t count_of(agentengine::standing_effect_kind kind) const {
        return static_cast<std::size_t>(std::count_if(
            effects_.begin(), effects_.end(),
            [kind](agentengine::StandingEffect const& e) { return e.kind == kind; }));
    }

    void add(agentengine::StandingEffect effect) { effects_.push_back(std::move(effect)); }

    [[nodiscard]] std::vector<agentengine::StandingEffect> const& list() const noexcept {
        return effects_;
    }

    // Exposed so AgentSession::start_background_task()'s own background_task() completion closure
    // can capture a weak_ptr to the SAME queue instance this registry owns -- never capture this
    // shared_ptr itself strongly from a detached-thread closure; that would defeat the whole
    // "session (and its registry) gone => silently drop, no UAF" design this queue exists for.
    [[nodiscard]] std::shared_ptr<BackgroundCompletionQueue> const& completion_queue() const noexcept {
        return completions_;
    }

    [[nodiscard]] agentengine::result<void> cancel(std::string const& handle_id,
                                                     agentengine::Principal const& caller_principal) {
        auto it = std::find_if(effects_.begin(), effects_.end(),
                                [&](agentengine::StandingEffect const& e) { return e.handle_id == handle_id; });
        if (it == effects_.end()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "no such standing effect", "standing_effect.not_found"});
        }
        if (it->principal_id != caller_principal.id) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "cannot cancel a standing effect owned by a different principal",
                "standing_effect.cross_principal_denied"});
        }
        effects_.erase(it);
        return {};
    }

    [[nodiscard]] std::vector<agentengine::StandingEffect> due(
        std::chrono::steady_clock::time_point now) const {
        std::vector<agentengine::StandingEffect> out;
        for (agentengine::StandingEffect const& e : effects_) {
            if (e.kind == agentengine::standing_effect_kind::schedule_wakeup && e.fire_at.has_value() &&
                *e.fire_at <= now) {
                out.push_back(e);
            }
        }
        return out;
    }

    // ADR-061 §20.5/§20.6/§19.6: the real schedule_wakeup logic, UNLOCKED, taking exactly what it
    // needs as parameters -- called both by AgentSession's public schedule_wakeup() wrapper
    // (already resolved authority, already locked) and by the offer-gate closure in run_rounds(),
    // which runs already locked via its own caller and must not re-lock. Kept `_impl`-suffixed
    // rather than a bare `schedule_wakeup` name deliberately -- AgentSession still exposes a
    // public, LOCKED `schedule_wakeup()` wrapper of its own; giving this one the identical bare
    // name would throw away the exact naming signal ADR-061 §20.5 introduced the split to provide
    // (so a future maintainer editing the ScheduleWakeupTool closure can tell at a glance which one
    // is safe to call from inside an already-held lock). `now` is a REQUIRED parameter, never read
    // from an ambient clock internally -- load-bearing for I5, not just today's signature shape.
    [[nodiscard]] agentengine::result<agentengine::StandingEffect> schedule_wakeup_impl(
        std::chrono::milliseconds delay, std::string label, std::chrono::steady_clock::time_point now,
        agentengine::CapabilitySet const& held, agentengine::Principal const& principal,
        std::string const& run_id, std::string const& session_id) {
        auto const schedule_cap = held.find_schedule();
        if (!schedule_cap.has_value()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                        "Schedule<max_horizon, max_active> not granted",
                                                        "schedule_wakeup.not_granted"});
        }
        if (delay < std::chrono::milliseconds{0} ||
            std::chrono::duration_cast<std::chrono::seconds>(delay) > schedule_cap->max_horizon) {
            return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                        "delay exceeds the granted Schedule<max_horizon>",
                                                        "schedule_wakeup.horizon_exceeded"});
        }
        if (count_of(agentengine::standing_effect_kind::schedule_wakeup) >= schedule_cap->max_active) {
            return std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                        "Schedule<max_active> ceiling reached",
                                                        "schedule_wakeup.capacity_exceeded"});
        }

        agentengine::StandingEffect effect;
        effect.handle_id    = mint_handle_id(session_id);
        effect.session_id   = session_id;
        effect.principal_id = principal.id;
        effect.run_id       = run_id;
        effect.kind         = agentengine::standing_effect_kind::schedule_wakeup;
        effect.label        = std::move(label);
        effect.fire_at      = now + delay;
        effects_.push_back(effect);
        return effect;
    }

    // Drains every ready background-task completion and erases its matching StandingEffect,
    // returning what AgentSession needs to emit a ToolCallFinished event for each -- deliberately
    // does NOT emit itself (see DrainedCompletion's own comment). Caller must already hold
    // AgentSession's session_mutex_ for its whole call AND the subsequent emit loop.
    [[nodiscard]] std::vector<DrainedCompletion> drain_ready() {
        std::vector<BackgroundTaskDone> ready;
        {
            std::lock_guard<std::mutex> lock(completions_->m);
            ready.assign(std::make_move_iterator(completions_->pending.begin()),
                         std::make_move_iterator(completions_->pending.end()));
            completions_->pending.clear();
        }
        std::vector<DrainedCompletion> drained;
        drained.reserve(ready.size());
        for (BackgroundTaskDone& m : ready) {
            auto it = std::find_if(effects_.begin(), effects_.end(),
                                    [&](agentengine::StandingEffect const& e) { return e.handle_id == m.handle_id; });
            if (it == effects_.end()) continue;  // canceled or already resolved -- no-op
            std::string const owner_run_id = it->run_id;
            effects_.erase(it);
            drained.push_back(DrainedCompletion{owner_run_id, std::move(m.call_id), std::move(m.result)});
        }
        return drained;
    }

    // Resets effects_/counter_ back to empty/0 -- used by AgentSession::fork_from()/
    // clear_in_process_state(). Deliberately does NOT reset completions_ (the shared_ptr's
    // identity): a worker thread may already hold a weak_ptr to it; a stale completion for a
    // since-cleared effect is already a harmless no-op via drain_ready()'s own find_if miss.
    void reset() {
        effects_.clear();
        counter_ = 0;
    }

private:
    std::vector<agentengine::StandingEffect>   effects_;
    std::uint64_t                              counter_ = 0;
    std::shared_ptr<BackgroundCompletionQueue> completions_;
};

}  // namespace agentengine::rt
