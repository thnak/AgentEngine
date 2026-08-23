#pragma once
// Implements 027-Vocabulary-and-Naming.md §4/§5: `ae::task<T>`.
//
// ADR-037 (removing Quark as core runtime): `agentengine::task<T>` used to split into two concrete
// types -- `task<void>` stayed `quark::task<void>` (Quark's ADR-007 dispatch-handler type) for as
// long as any real `quark::Actor` still existed to be dispatched that way, while `task<T>` for
// `T != void` already resolved to `agentengine::rt::task<T>`. That split is now GONE: every
// `quark::Actor` type this project ever defined (`core/agent_session.hpp`, `workflow/supervisor.hpp`,
// `workflow/executor.hpp`, `project/registry.hpp`, `project/lifecycle.hpp`,
// `trust/spawn_cost_budget.hpp`'s `SpawnCostBudgetActor`) has been deleted, so nothing left anywhere
// in this tree ever dispatches through Quark's async-handler jump table or calls
// `quark::task<>::detach()` -- the ONE structural reason `task<void>` couldn't simply retarget too.
// `agentengine::rt::task<T>` (rt/task.hpp) has always had a real `T = void` full specialization
// (built from the start of ADR-037's Phase 1, specifically so it can be `co_await`ed from ANY
// coroutine context) -- so `task<T>` for every `T`, including `void`, now resolves to
// `agentengine::rt::task<T>` through one blanket alias. This header has zero Quark dependency now.
//
// Named residual, not fixed in this pass: several `ContextProvider` conformers' `on_turn_end` still
// returns `task<std::monostate>` rather than bare `task<>` (== `task<void>`) -- a workaround from
// when `task<void>` aliased the deliberately-NOT-awaitable `quark::task<void>` (context_provider.hpp/
// context_assembly.hpp's own comments explain why). That workaround is no longer STRICTLY necessary
// now that `task<void>` is genuinely awaitable, but changing it means touching every conformer's
// return type across the whole tree -- a real API-shape decision, not a side effect of this cleanup.
//
// One visible consequence of keeping it: `rt::task<T>::await_resume()` is [[nodiscard]], so
// `co_await p.on_turn_end(...)` yields a `std::monostate` that every caller then drops, which MSVC
// reports as C4834 (several sites, e.g. rt/agent_session.hpp, core/composed_context_provider.hpp).
// Those sites write `(void)co_await ...` -- the standard
// spelling for "this value is deliberately discarded", and here the discarded value is literally
// this codebase's own "no value" type. That is an annotation at the call site, not a silenced
// diagnostic; retiring it for good is the same API-shape decision named above.

#include "agentengine/rt/task.hpp"

namespace agentengine {

template <class T = void>
// ae-naming-lint: allow task — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using task = agentengine::rt::task<T>;

}  // namespace agentengine
