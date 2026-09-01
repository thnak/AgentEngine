# ADR-159: mid-run cancellation for `WorkflowSupervisor` (GitHub issue #37)

## 1. The question

MAF's `workflow_cancellation.py` sample cancels a running workflow mid-execution and the run stops
before completing its remaining steps. `WorkflowSupervisor`'s `TerminationBound` has
`max_rounds`/`deadline_ms`/`max_stalls`/`max_resets` — all DECLARED limits, checked only at round
boundaries — but nothing lets a caller signal "stop this run now" from OUTSIDE, imperatively, for
its own reasons (a user closing a UI, an upstream request being aborted). Confirmed via direct
grep: zero matches for "cancel" anywhere in `workflow_supervisor.hpp` before this ADR.

## 2. An already-established precedent — reused, not reinvented

`agentengine::stream<T>`/`stream_producer<T>` (`core/stream.hpp`) already solved the identical
shape of problem for a different subsystem: an out-of-band cancellation signal a consumer raises
that a producer running on a different thread must observe (ADR-017). The mechanism — a
`std::stop_source`, shared by copy, `cancel()` calling `request_stop()`, a `std::stop_token` exposed
for cooperative polling — is not a one-off there either: already used across `rt/thread_pool.hpp`
(`std::jthread`'s own stop mechanism), `core/model_call_gateway.hpp`, both `chat_client.hpp`
protocol adapters, and `sandbox/provider_http_client.hpp`/`net_egress_proxy.hpp`. This ADR reuses
that established idiom rather than inventing a bespoke `CancellationToken` type.

## 3. Red-team pass, before any code existed

An independent red-team pass (fresh agent, zero prior context) confirmed the central mechanism
sound — thread safety (`std::stop_source::request_stop()`/`std::stop_token::stop_requested()` are
specified by the C++20 standard as safely callable concurrently across threads with no external
synchronization), call-site completeness, checkpoint isolation, I1/I2/I3, and the rejection of a
`run_mutex_`-acquiring alternative (it would have to wait for the very run it's trying to stop to
release the mutex first, defeating the purpose) — all traced against the real code, not assumed.

Two real problems found and fixed before implementation:

1. **MUST-FIX — a second exhaustive `switch (workflow_status)` the first design draft's own file
   list missed**: `workflow_as_executor_detail::status_tag()` (`rt/workflow_as_executor.hpp`) has
   no `default:` case, unlike `finish()`'s own switch in `workflow_supervisor.hpp`. This project
   builds under `-Wall -Wextra -Werror` (gcc/clang) as a hard gate — adding `workflow_status::
   cancelled` without a case there would have broken that build (MSVC alone would not have caught
   it). Fixed: both switches updated together.
2. **MUST-FIX (proof-plan) — the round-boundary proof needed this codebase's own established
   deterministic-liveness shape, not a timing race**, or it risked the exact false-negative class
   this session has now caught three times (the ThreadPool budget work's racy background sampler;
   ADR-157 Pass 2's `fan_in`-merge false negative; this would have been a third instance). Nothing
   in the original proof wording forced the run to still genuinely be executing when `cancel()`
   lands. Fixed: the proof (C1, below) mirrors `test_rt_workflow_event_stream.cpp`'s own W12 shape —
   a body deliberately blocked on a `condition_variable`, `cancel()` issued and CONFIRMED to have
   landed while the body is still genuinely blocked, then released, then confirmed the run stops
   at `rounds == 1`.

Two SHOULD-FIX refinements, both incorporated: the "cheap, round-boundary" framing was sharpened to
say plainly that for a wide fan-out round, or any round containing a `sub_workflow` dispatch (which
runs an entire nested multi-round sub-run synchronously before the outer loop ever rechecks
`cancel_source_`), "next round boundary" can be an effectively unbounded wait; and the
nested-cancellation residual was reframed — a host that already holds a `shared_ptr<
WorkflowSupervisor> inner` (required to call `bind_sub_workflow()` at all) can call `inner->
cancel()` directly, TODAY, with zero new machinery, so the real remaining gap is only AUTOMATIC
fan-out from one outer `cancel()` call, not "nested cancellation" as a whole.

Full findings and resolution: `docs/planning/workflow-mid-run-cancellation-design-draft.md` §2b.

## 4. The accepted mechanism

- `WorkflowSupervisor` gains a private `std::stop_source cancel_source_;` member (default-
  constructed, always present, mirroring `multiplex_sink_`'s own "always valid, no null state"
  shape), plus two public methods: `void cancel() noexcept` (calls `request_stop()`; callable from
  any thread while a run is in flight on another) and `[[nodiscard]] std::stop_token
  cancellation_token() const noexcept` (a caller can hold this independently, mirroring
  `stream_producer<T>::stop_token()`'s own exposed-handle shape).
- New `workflow_status::cancelled` enumerator, an honest, expected non-error termination — the same
  shape `bound_max_rounds`/`bound_max_stalls` already are. `finish()` needed zero changes (its
  `switch` already has a `default:` that routes any unhandled status through `workflow_run_failed`
  with the right tag); `workflow_status_tag()` and `workflow_as_executor_detail::status_tag()` both
  gained the new case (§3 finding 1).
- `execute()`'s round loop: one new check, in the EXACT SAME spot every other termination bound is
  already checked (top of the `while (!state_.pending.empty())` loop, right after the existing
  `deadline_ms` check) — never a special early gate before the port-resolution prologue, for
  consistency with `max_rounds`/`deadline_ms`, which don't gate that block either.
- Cooperative mid-call opt-out: `EffectContext` gains `std::stop_token cancellation;` (default-
  constructed — always safe to read even when nobody wires it, the same "optional-but-always-safe-
  to-call" idiom `report_progress`/`agent_turn_sink` already establish). Implementation refinement
  over the original plan: rather than adding another explicit parameter to `run_executor_job()`
  (already carrying `sink`/`executor_id`/`round`/`attempt`/`path_prefix`), `execute()`'s dispatch
  loop builds a per-delivery COPY of `contexts_[idx]` with `ctx.cancellation = cancel_source_.
  get_token()` set before passing it in — `EffectContext` already IS the per-call vehicle for
  exactly this kind of opt-in signal (mirrors `capabilities`/`bound_capabilities` riding along on
  `ctx` rather than as separate parameters), so no new function-signature plumbing was needed.

## 5. What this ADR does not claim

- Does not preempt an already-dispatched, in-flight executor body that never checks
  `ctx.cancellation` — the same already-accepted characteristic every existing bound has today (the
  issue's own text: "even `deadline_ms` only takes effect at the next round check, not mid-call").
  For a wide fan-out round, or any round containing a sub_workflow dispatch, "next round boundary"
  can be an effectively unbounded wait, not a uniformly cheap one.
- Does not automatically fan out one outer `cancel()` call into every bound nested `sub_workflow`
  instance. A host that already holds a `shared_ptr<WorkflowSupervisor> inner` can call `inner->
  cancel()` directly today with zero new machinery (the same thread-safety guarantee applies to any
  instance, nested or not) — only AUTOMATIC propagation from a single outer call is deferred, a real
  follow-on for a future issue reusing the `ScopedForwardedEventSink`-style dispatch-time-wiring
  pattern issue #42 item 3 established.
- Does not take an extra checkpoint at the cancellation point. The existing per-round `checkpoint_
  hook_` call already fires at the end of the round BEFORE cancellation is checked (right after that
  round's results are folded) — a cancelled run's last completed round is always already
  checkpointed if a hook is set, with zero new checkpoint call needed.
- Does not add `cancel`/`cancellation_token` to `TerminationBound` (`workflow/graph.hpp`). Every
  `TerminationBound` field is a DECLARED, graph-authored limit; cancellation is an imperative,
  caller-driven external signal — kept as a `WorkflowSupervisor`-level runtime feature, mirroring
  `set_checkpoint_hook()`'s own "opt-in configuration call, not a graph field" precedent.
- Does not persist cancellation state through checkpoint/resume — a fresh/restored instance's
  `cancel_source_` is always unrequested; a caller wanting a restored run to stay cancelled must
  call `cancel()` again after restore. Matches `nesting_depth_`/`event_path_prefix_`'s own
  already-established non-persistence precedent.
- `EffectContext::cancellation` reaches no further into the call stack than a body's own direct
  read of it — distinct from `EffectContext::deadline` (a per-call timeout already consumed by
  `core/model_call_gateway.hpp` for a different purpose) and not itself derived from model output
  (I3) or gated behind any capability (I2). Not an I5 concern in practice: no `ExecutorBody`/
  `WorkflowSupervisor` code path in this codebase makes any I5 replay-determinism claim today,
  unlike `deadline` (excluded from `SelectFn`'s own visible `EffectContext` specifically for that
  reason, `core/routing_model_call_gateway.hpp`) — disclosed here so a future I5-related change to
  this layer knows to revisit it, not silently assumed harmless.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | `cancel()` called from a separate thread while a multi-round run is in flight stops the run with `workflow_status::cancelled` before completing every round it otherwise would have — proven deterministically (a genuinely blocked body, `cancel()` confirmed to land while it is still blocked), not by a timing race that could pass for the wrong reason. | CORRECT | `test_rt_workflow_cancellation.cpp` C1 |
| 2 | The round already in flight when `cancel()` was called still completes normally and its real output is preserved (`WorkflowResult::partial`) — cancellation does not truncate or corrupt work already under way, it only prevents future rounds. | CORRECT | C1/C2 (same scenario) |
| 3 | A body that explicitly checks `ctx.cancellation.stop_requested()` observes a `cancel()` call made from a different thread while it is running, and can act on it (return early / a distinguishable result). | CORRECT | C3 |
| 4 | A body that does NOT check the token runs to its own natural completion for that one call, even after `cancel()` was called mid-call — genuinely cooperative, not accidentally preemptive. | CORRECT | C4 |
| 5 | A run that never calls `cancel()` completes with its ordinary status, every round, byte-for-byte unaffected by the mechanism's mere presence. | CORRECT | C5 |
| 6 | `cancellation_token()` returns a handle a caller can hold independently of `EffectContext`; it observes the SAME request `cancel()` made. | CORRECT | C6 |
| 7 | Claims 1-4's own checks are genuinely load-bearing, not vacuous — adversarially verified by mutation. | CORRECT (adversarially verified) | Temporarily short-circuited the round-loop check (`if (false && cancel_source_.stop_requested())`). Rebuilt and reran: 8 of 17 checks failed exactly as expected (every check whose correctness depends on cancellation actually taking effect) while the checks independent of it (C1's "landed" observation, C5, C6) kept passing. Reverted; rebuilt; reran — clean, 17/17 passing again. |
| 8 | Every pre-existing test still passes; the wider repo-wide suite is unaffected. | CORRECT | Full `ctest -C Debug`: see Status. |
| 9 | The full project builds clean, including under `-Werror`/`/WX`. | CORRECT | Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC), zero errors |

## 7. Files changed

**New:**
- `docs/planning/workflow-mid-run-cancellation-design-draft.md`
- `tests/test_rt_workflow_cancellation.cpp`

**Edited:**
- `include/agentengine/rt/workflow_supervisor.hpp` — `cancel_source_`, `cancel()`,
  `cancellation_token()`, `workflow_status::cancelled`, `workflow_status_tag()`'s new case, the
  round-loop check, `execute()`'s dispatch loop wiring `ctx.cancellation`.
- `include/agentengine/rt/workflow_as_executor.hpp` — `status_tag()`'s own new case (§3 finding 1).
- `include/agentengine/core/effect_context.hpp` — `EffectContext::cancellation`.
- `tests/CMakeLists.txt` — new target registration.

## Status

**Proposed — implemented, independently red-teamed once (design draft before any code existed,
revised against both MUST-FIX findings), the central mechanism adversarially verified by mutation,
pending project-owner sign-off.** 17/17 checks passing in `test_rt_workflow_cancellation.cpp`. Full
project building clean (zero errors, MSVC/Visual Studio 18, Debug). Full repo-wide `ctest`:
317/318 passing (the one failure pre-existing and confirmed unrelated — the long-documented
matplotlib/pandas environment gap; `test_rt_spawn_cost_budget` did not fail this run).
