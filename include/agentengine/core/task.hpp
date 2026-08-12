#pragma once
// Implements 027-Vocabulary-and-Naming.md §4/§5: `ae::task<T>`.
//
// ADR-037 (removing Quark as core runtime): `agentengine::task<T>` covers TWO structurally different
// roles that happen to share one name, and this header now resolves them to two different concrete
// types rather than one blanket alias:
//
//   - `task<void>` (bare `task<>`) is Quark's ADR-007 async-handler-selection type -- the ONE exact
//     type Quark's dispatch jump table matches to pick an actor handler's async execution mode, and
//     the ONLY type ever `detach()`ed to Quark's own executor (`quark::task<>::detach()`). This role
//     is structurally tied to the actor engine and stays `quark::task<void>`, UNCHANGED, for as long
//     as any real `quark::Actor` (`core/agent_session.hpp`, `workflow/supervisor.hpp`,
//     `workflow/executor.hpp`, `project/registry.hpp`, `project/lifecycle.hpp`,
//     `trust/spawn_cost_budget.hpp` -- the six still-live actor types named in ADR-037's own removal
//     scoping) still exists to be dispatched this way. Retargeting this half would not compile against
//     any of them.
//
//   - `task<T>` for `T != void` (ADR-047) is a nested, awaitable, value-returning coroutine --
//     `quark::task<T>`'s OWN banner already establishes it has NO Quark-specific coupling beyond
//     being an ordinary C++20 Awaitable (`task<T>` IS its own awaiter via `await_ready`/
//     `await_suspend`/`await_resume`, symmetric-transfer completion, no `detach()`, no dispatch-table
//     involvement). `agentengine::rt::task<T>` (rt/task.hpp) implements that exact same protocol --
//     it was built, from the start of ADR-037's Phase 1, specifically so it can be `co_await`ed from
//     ANY coroutine context, `quark::task<void>`-based or not (already proven: every real
//     `rt::AgentSession`/`rt::WorkflowSupervisor` test this session co_awaits `rt::task<T>` from
//     `rt::task<T>` bodies; this change proves the reverse direction -- a `quark::task<void>` actor
//     handler co_awaiting an `rt::task<T>` -- works identically, since C++20 coroutine composition is
//     direction-agnostic: a coroutine does not know or care what "kind" of type it is awaiting, only
//     that the awaited object implements the protocol). Every conformer of `ChatClient`/
//     `ContextProvider` (chat_client.hpp/context_provider.hpp) names its return type through THIS
//     alias, never `quark::task<T>` directly -- so retargeting it here closes the "every real
//     conformer still returns quark::task<T>" gap named throughout this project's rt:: files, for
//     every conformer, in one place, with zero per-conformer source changes required.
//
// `agentengine::rt::task<T>`'s own promise_type has no `T != void` specialization gap either way --
// see rt/task.hpp's own banner for why it was built to support both "co_await-ed by a parent
// coroutine" and "driven directly via resume()/done()" from day one, unlike `quark::task<T>`, which
// only ever supported the former.

#include "agentengine/rt/task.hpp"
#include "quark/core/task.hpp"

namespace agentengine {

namespace task_detail {
template <class T>
struct task_for {
    using type = agentengine::rt::task<T>;
};
template <>
struct task_for<void> {
    using type = quark::task<void>;
};
}  // namespace task_detail

template <class T = void>
using task = typename task_detail::task_for<T>::type;

}  // namespace agentengine
