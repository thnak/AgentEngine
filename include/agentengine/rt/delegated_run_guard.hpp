#pragma once
// decisions/ADR-193 §9 (red team round 3, GitHub issue #113): the one rule every delegated body follows -- a body
// that runs another agent or workflow charges that run's whole spend to its caller exactly once, on every exit
// path: success (in its outcome's usage), failure (through `EffectContext::charge_delegated_usage`), and a throw
// (through the same hook, from a guard's destructor). These are the two guards that make the throw path and the
// cleanup path hold without a `try`/`catch`:
//
// - `ChargeOnUnwind` runs its charge when the guarded scope is left by an exception. It is a destructor, never a
//   catch-and-rethrow: clang-cl's ASan build crashes in the Windows unwinder on a `throw;` from such a frame (CI,
//   PR #110). A throw from the charge itself is swallowed, since a second exception while unwinding terminates.
// - `OnScopeExit` runs its action on every exit, a throw included -- used to detach a call-scoped event tap that
//   holds a reference into the caller's `EffectContext`, so no tap outlives the frame that owns that context.
//
// Users: `rt::run_child_agent_session` (agent.spawn), `rt::agent_session_as_executor_body` (workflow agent nodes),
// `rt::WorkflowChatClient`'s worker.

#include <exception>
#include <type_traits>
#include <utility>

namespace agentengine::rt {

namespace delegated_run_detail {

template <class F>
class ChargeOnUnwind {
public:
    explicit ChargeOnUnwind(F const& charge) noexcept : charge_(charge) {}
    ChargeOnUnwind(ChargeOnUnwind const&) = delete;
    ChargeOnUnwind& operator=(ChargeOnUnwind const&) = delete;
    ~ChargeOnUnwind() {
        if (std::uncaught_exceptions() <= entry_exceptions_) return;  // left normally: the caller charges
        try {
            (void)charge_();
        } catch (...) {  // NOLINT(bugprone-empty-catch): a second throw while unwinding would terminate
        }
    }

private:
    F const& charge_;
    int const entry_exceptions_ = std::uncaught_exceptions();
};

template <class F>
class OnScopeExit {
public:
    explicit OnScopeExit(F action) noexcept(std::is_nothrow_move_constructible_v<F>) : action_(std::move(action)) {}
    OnScopeExit(OnScopeExit const&) = delete;
    OnScopeExit& operator=(OnScopeExit const&) = delete;
    ~OnScopeExit() {
        try {
            action_();
        } catch (...) {  // NOLINT(bugprone-empty-catch): may run while unwinding
        }
    }

private:
    F action_;
};

}  // namespace delegated_run_detail

}  // namespace agentengine::rt
