#pragma once
// Implements 027-Vocabulary-and-Naming.md §4/§5: `ae::task<T>` is Quark's coroutine return type,
// used verbatim -- no AgentEngine-side reimplementation, matching `ae::result<T>`'s own
// `std::expected` alias in error.hpp. `task<void>` is the async-handler-selection type (ADR-007's
// hybrid dispatch); `task<T>` for T != void (ADR-047, now real -- third_party/quark bumped to
// `dcb191f`) is a nested, awaitable inner coroutine a `task<void>` handler (or another `task<T>`)
// `co_await`s to get a value back. See `quark/core/task.hpp`'s own banner comment for the full
// contract; this header adds nothing beyond the name.
//
// Milestone 5 Phase B4 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): the
// concrete first use is `ChatClient::chat()`'s real, RFC-literal signature (004 §1).

#include "quark/core/task.hpp"

namespace agentengine {

template <class T = void>
using task = quark::task<T>;

}  // namespace agentengine
