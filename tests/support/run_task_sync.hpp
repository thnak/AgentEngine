#pragma once
// Test-only synchronous driver for an `ae::task<T>` (`agentengine::rt::task<T>`, `core/task.hpp`)
// whose body is known to never genuinely park -- e.g. a ChatClient test conformer exercised directly
// from a plain `main()`, with no `rt::ThreadPool`/session runtime anywhere in the picture.
// `rt::task<T>` has no synchronous "drive to completion and give me the value" API by design: it is
// meant to be `co_await`ed only from inside another coroutine (`rt/task.hpp`'s own banner comment;
// `task_value_return_test.cpp` never drives one from a plain function either). Milestone 5 Phase B4
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md) needs exactly this seam: several
// pre-existing tests call a `ChatClient` conformer's `chat()` directly, outside any session, and the
// conformers themselves are trivial (no real I/O, no cross-call `co_await` inside) -- so a minimal
// driver was enough historically. Since ADR-237 §5.1 (step 38) it delegates to `rt::block_on`, so a
// task that DOES genuinely park (a reactor await) is driven correctly too -- see the note below.
// Real, session-hosted call sites (`AgentSession::start_run`) run inline on the caller's own coroutine
// instead (historical: this comment used to describe `quark::task<T>` and a real `quark::Activation`
// before ADR-037 removed Quark as a dependency; `agentengine::rt::task<T>` has no actor/activation
// concept at all).

#include <utility>

#include "agentengine/rt/block_on.hpp"

namespace agentengine::test_support {

// `awaitable` is an `ae::task<T>` rvalue. Returns the awaiting `T` or rethrows whatever the awaited
// coroutine threw.
//
// ADR-237 §5.1 (step 38): this now delegates to `rt::block_on`, which makes the CALLING THREAD the
// driven task's home (`rt/block_on.hpp`'s own ADR-175 contract). That is REQUIRED now that a seam's
// body may genuinely park on a reactor operation: the original manual-resume driver here supplied no
// home, and `rt/reactor_await.hpp` REFUSES a homeless waiter (`Reactor::note_homeless_refusal`)
// rather than resuming it on the reactor thread -- so a reactor-awaiting task driven by the old loop
// would simply never resume. For the non-parking tasks this harness was written for (its original,
// now-removed doc comment named them: trivial `ChatClient` conformers with no real I/O) behaviour is
// identical -- the whole chain still runs inline on this thread. This replaced a hand-written driver
// whose ASan-confirmed stack-use-after-scope hazard (a closure temporary captured by the coroutine
// frame) had to be worked around with a named local; `rt::block_on` already carries that fix, so the
// workaround is gone with the driver.
template <class T, class Awaitable>
T run_task_sync(Awaitable&& awaitable) {
    return agentengine::rt::block_on(std::forward<Awaitable>(awaitable));
}

}  // namespace agentengine::test_support
