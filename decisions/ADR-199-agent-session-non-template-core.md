# ADR-199 — `AgentSession`'s non-template core: compile the turn loop once, not once per session type per file

- **Status**: **Proposed — design + implementation + proof (2026-09-26); red team round 1 (same day): 1 MAJOR + 4 MINOR,
  all fixed or recorded (§8).**
- **Date**: 2026-09-26
- **Origin**: GitHub issue #115 (E3), the build-time investigation after red-team round 4.
- **Touches**: `include/agentengine/rt/agent_session.hpp` (now the thin template),
  `include/agentengine/rt/agent_session_core.hpp` (new), `src/rt/agent_session_core.cpp` (new), `CMakeLists.txt`
  (`agentengine_rt_session`, linked through `agentengine::core`), `tests/CMakeLists.txt` (two `try_compile` source
  lists).

## 1. The question

`rt::AgentSession<ChatClientT, StateT, HistoryProviderT>` was one 3,200-line class template, all in a header. Every
translation unit that used it compiled the whole turn loop again, once for each session type it named:
`test_delegation_provenance` has nine, so `run_rounds()` — a 640-line coroutine, 4–6 s of MSVC optimizer time per copy —
was code-generated nine times in that one file. There are 105 files like it (89 tests, 11 examples, 5 tools). Issue
#115 measured this as the dominant cost of a cold build: MSVC Release took 43–45 minutes, and the agent-session tests
averaged 24.9 s per file in Debug against 12.2 s for the rest.

Almost none of that code depends on the template parameters. Can it be compiled once, without changing what a session
does?

## 2. Designs considered

- **A. Non-template base compiled in `src/` (chosen).** Every member that does not name `ChatClientT`, `StateT` or
  `HistoryProviderT` moves to `AgentSessionCore`, whose large bodies live in `src/rt/agent_session_core.cpp`. The
  template derives from it and keeps the bound client, provider and state. The core reaches them through six private
  virtual hooks.
- **B. `extern template` on a dummy-parameterized core.** No out-of-line split is needed if every member stays in the
  class, but members defined in the class are inline, and the standard lets a compiler instantiate inline members despite
  `extern template` ([temp.explicit]/10). Clang does exactly that at `-O1` and above (available_externally) — the
  optimizer cost this ADR exists to remove. Rejected.
- **C. Precompiled headers or unity builds.** These cut parsing, not codegen: a PCH still instantiates `run_rounds()` per
  session type per file. Issue #115 measured the optimizer, not the front end, as the cold-build cost. Not a substitute;
  might complement A later.
- **D. Type-erase the chat client with a `std::function`-style wrapper held by value.** Same run-time shape as A, but it
  adds an owning erasure type and changes `emplace_chat_client()`'s return type. A keeps the public surface unchanged.

## 3. Decision

Design A.

- **What moves.** Everything that does not name a template parameter: admission, `start_run()`, `resolve_interaction()`
  and the approval / hook-decision / CodeAct resume paths, `run_rounds()`, `dispatch_tool_calls()`, events, delegated
  usage, standing effects, snapshot records, and every setter and accessor. Bodies are moved verbatim; small inline
  accessors stay inline in the core header.
- **What stays in the template.** `chat_client_`, `state_` and `history_provider_`, and every member that touches them:
  `emplace_chat_client()`, `has_chat_client()`, `state()`, `history_provider()` (ADR-116's `bind_owner`),
  `run_model_call()` (it branches with `if constexpr` on the client's type), `fork_from()` and
  `clear_in_process_state()` (they copy or reset the provider and state), `clear_in_process_state_locked()`, and
  `make_default_chat_client()`. `fork_from()` and `clear_in_process_state()` call `fork_core_from()` and
  `clear_core_state()` for the rest.
- **The hooks.** `run_model_call`, `bound_on_context`, `bound_on_turn_end`, `bound_filter_cross_provider_reasoning`,
  `bound_has_chat_client` and `bound_model_route`. They are **private** pure virtuals, and every other core member is private too; the
  template is the core's only friend and is `final` (§8, red team). `bound_model_route()` returns an enum computed with the same nested `if constexpr` as
  before; `start_run()`'s ADR-034/ADR-036 warnings and `should_retry_stream()` now branch on it at run time.
- **Why virtual calls are allowed here.** CONVENTIONS.md permits type erasure "only at declared seams (provider, sandbox
  backend, store) and never inside a turn's hot loop". The chat client is the provider seam and the history provider is
  the context seam. Each hook is called once per model call, context assembly or turn end, never per token: the
  streaming drain stays inside the template's `run_model_call()`, statically bound to the client's `chat_stream()`.
- **Link.** `agentengine_rt_session` is a static library linked through `agentengine::core`'s interface, as
  `agentengine_rt_file_log` already is (ADR-195 E32). Every existing consumer keeps building without a CMake change.
  The two `try_compile` gates that compile `agent_session.hpp` into an isolated mini-project (ADR-096 C2 and ADR-195
  §3.8) list the new source file explicitly.

## 4. Behaviour — what is and is not the same

The claim is that no observable behaviour changes. What differs, precisely:

1. `fork_from()` copies `state_` and `history_provider_` **after** the core fields are copied and reset, not in the
   middle. Nothing reads between them, and both still happen under the source's `session_mutex_`. The one visible
   difference: if the provider's copy-assignment throws, the target's core fields are already reset. Before, some were
   reset and some were not. Neither order was a documented guarantee.
2. `clear_in_process_state()` resets `state_` and the provider after the core fields rather than among them. Nothing
   reads in between.
3. The client-type branches in `start_run()` and `should_retry_stream()` are run-time switches on `bound_model_route()`
   instead of `if constexpr`. The enum is computed from exactly the same concepts, in the same nesting.
4. `AgentSession` is now polymorphic (one vtable pointer, six indirect calls per round) and `final`. Nothing in the
   tree derived from it. The core's members stay private: the template reaches them as the core's one friend.
5. The core's bodies are compiled with the library's flags. In a Release build, `NDEBUG` is defined for `src/` and
   stripped for `tests/` (tests/CMakeLists.txt keeps assertions live). An inline function used by both could then have
   two different bodies (an ODR difference the linker resolves by picking one). `agent_session.hpp` itself contains no
   `assert`; the headers it includes are unchanged. `agentengine_rt_file_log` already has the same property.

## 5. Evidence

- **Moved verbatim.** A normalized line diff (whitespace collapsed, comments and blank lines dropped) between the old
  header and the three new files differs only in signatures (`AgentSessionCore::` qualification, default arguments
  kept on declarations only), the six hook call sites, the `fork`/`clear` split and the class skeletons.
- **Builds.** Full MSVC Debug build of every target, `/W4 /WX`.
- **Tests.** Full non-live suite: 381/381 pass (the 11 Docker-daemon tests excluded; Docker Desktop was down locally,
  CI runs them).
- **Compile-fail gate still bites for the right reason.** `tests/compile_fail/sandbox_tool_provider_rejects_fork_from.cpp`
  still fails, at `fork_from()`'s `history_provider_ = source.history_provider_` (C2280, deleted copy-assignment), and its
  positive control still compiles and links. Making a hook virtual instantiates it with the class. The hooks were
  therefore chosen so that none touches a member a legitimate provider might lack: `fork_from()` and
  `clear_in_process_state()` stay ordinary template members, instantiated only when called.
- **Compile time.** See §6.

## 6. Measurements

Single translation units, MSVC 14.51, compiled serially, one at a time, on the same machine, same session:
`/O2 /Ob2 /DNDEBUG /MD` (REL) and `/Od /Ob0 /RTC1 /MDd /Z7` (DBG), `/Bt+` for front-end/back-end split. CPU is the
compiler process's total CPU time (the back end is multi-threaded, so CPU exceeds wall).

| Translation unit | REL CPU before → after | REL wall | DBG CPU before → after |
|---|---|---|---|
| `test_delegation_provenance` (9 session types) | 136.8 s → 83.4 s (−39 %) | 40.2 → 28.8 s | 23.2 → 16.4 s (−29 %) |
| `test_approval_resume` | 72.3 → 36.0 s (−50 %) | 25.2 → 13.8 s | 13.5 → 9.2 s (−32 %) |
| `test_rt_agent_session_stream_retry` | 77.0 → 46.1 s (−40 %) | 23.7 → 16.5 s | 14.3 → 9.9 s (−31 %) |
| `test_rt_agent_workflow_executor` | 52.9 → 37.2 s (−30 %) | 18.8 → 16.1 s | 11.2 → 10.5 s (−6 %) |
| `test_json_value` (control: no session) | 5.8 → 5.5 s | 3.8 → 3.7 s | 2.5 → 2.6 s |

The saving is in the back end, as issue #115 predicted: REL back-end wall for `test_approval_resume` went from 16.7 s to
6.3 s. Front-end time falls less (8.5 → 7.5 s), because the core header still parses everything the old one did
(§7). The core itself is one translation unit, compiled once per build. The control file does not move, so the
difference is not machine noise.

## 7. Residuals

- The template still instantiates `run_model_call()` per client type, including its streaming drain. It is small
  (about 80 lines) and must stay templated to keep the drain statically bound.
- `agent_session_core.hpp` still includes everything the old header did, because test and example files rely on it for
  transitive includes. Trimming those includes is further front-end time for issue #115 (E5), not done here.
- Point 5 of §4 (the NDEBUG difference) is inherited from how `tests/` strips `NDEBUG`, not introduced here.
- The hooks are virtual, so they are instantiated whenever a session type is (red team MINOR 3). A client that satisfies
  the `ChatClient` concept but whose `chat_stream()` cannot actually be called from `run_model_call()` now fails when a
  session is constructed, not only when it runs. No in-tree type is affected; the ADR-096 C2 compile-fail gate still
  fails only at `fork_from()`.

## 8. Red team round 1 (2026-09-26)

An independent pass diffed every moved body against the old header and ran probes under MSVC, g++-14 and clang++-20.
Every moved body matched line for line. Findings:

- **MAJOR — dangling `TurnView` (fixed).** `bound_on_turn_end` took `TurnView` by value and returned the provider's
  lazy task. A provider whose `on_turn_end` takes `TurnView const&` (the concept allows it) kept a reference to the hook's
  parameter, which is gone before the task first runs. g++-14 with ASan reported stack-use-after-return, and at `-O2`
  the span read garbage. MSVC hid it: the parameter lived in the caller's coroutine frame there. No in-tree provider
  takes `const&`, so the suite passed. Fix: the hook takes `TurnView const&`, so the caller's temporary lives through
  the `co_await`, exactly as before the split. The same probe, re-run against the fix under g++-14 with ASan and
  `detect_stack_use_after_return=1`: `TURNVIEW INTACT`.
- **MINOR — reachable internals (fixed).** The core's former private members had become `protected`, and
  `AgentSession` was not `final`. A class derived from `AgentSession` could re-override the hooks, for example to skip
  the media gate in `run_model_call`. A member pointer formed inside such a class could set another session's
  `unattended_by_`. Host code is trusted either way, so this broke neither I2 nor I3, but it widened the surface.
  Fix: every core member is private again, `AgentSession<...>` is its one friend, and the template is `final`. The
  probe now fails to compile (C3246, C2248).
- **MINOR — COFF section count (fixed).** The core object had about 44,000 sections under the CI ASan flags, against
  the 65,279 limit without `/bigobj`. The library now builds with `/bigobj` on MSVC.
- **MINOR — stale evidence (fixed).** The first 381/381 run predated a header edit. Rebuilt everything and re-ran:
  381/381.
- **MINOR — hooks instantiated eagerly.** Recorded in §7.
- **Checked and benign:**
  - Destruction order is now safer: the template's members die while the core's mutex and event sinks are still
    alive, and nothing calls a hook during destruction.
  - `bound_on_context` passes through references the caller owns.
  - The headers the core includes contain no `#if` or `assert`, so NDEBUG and the HTTPS define cannot change what the
    library and a consumer see.
  - The core and three gateway tests compile clean under g++-14 and clang++-20 with `-Wall -Wextra -Werror`.
