# ADR-170 — `sandbox_exec_started`/`sandbox_exec_finished` were declared, specified and projected, but never emitted by anything. Where does the producer actually belong, and is `{exec_id}` enough to carry?

- **Status:** Proposed — implemented, proven by real test execution (S1–S9, 41 checks, including a
  shipped production producer driven through the real 006 §3 pipeline), full 277-test suite re-run at
  100%, pending project-owner sign-off.
- **Date:** 2026-09-04.
- **Scope:** `include/agentengine/core/run_event.hpp` (modified — `run_event_payload::SandboxExec`
  gains `backend`/`stage`/`ok`/`error_code`),
  `include/agentengine/core/effect_context.hpp` (modified — new `sandbox_exec_sink` field and the
  `SandboxExecScope` bracket),
  `include/agentengine/rt/agent_session.hpp` (modified — the sink bound/reset at the same three
  `invoke_tool()` bracket sites `report_progress` already uses),
  `include/agentengine/core/tool_pipeline.hpp` (modified — one reset in `background_task()`),
  `include/agentengine/tools/extract_pdf_text.hpp` (modified — a real producer on both platform
  branches, plus `next_exec_id()`; all four PDF tools share this function),
  `src/backends/native_jail/session_shell_wiring.hpp` (modified — the second real producer),
  `include/agentengine/protocol/agui/projection.hpp` (modified — the new fields reach the wire),
  `tests/test_sandbox_exec_events.cpp` (new), `tests/CMakeLists.txt` (additive wiring).
  **No existing test file changed.**
- **Related specs:** GitHub issue #64 (the defect this closes) · `013-UI-and-Streaming-Surfaces.md`
  §1 (the event vocabulary, amended by this ADR to name the producer), §2.1 (the AG-UI
  `ActivitySnapshot` mapping, already implemented) · `008-Sandbox-and-Isolation.md` §2 (the
  `SandboxBackend` concept whose `create`/`exec` already take an `EffectContext`), §6 (`per_session`
  sandbox lifetime — why one producer honestly has no `create` stage), §8 (post-hoc observability,
  the thing that already existed and is not a live signal) ·
  `decisions/ADR-060-tool-call-progress-channel.md` (the `report_progress` reverse channel this
  mirrors, including its bracket discipline and its detached-thread finding) ·
  `decisions/ADR-152-workflow-event-streaming.md` (the "one field per audience" rule this follows) ·
  `decisions/ADR-160-parallel-tool-batch-scheduler.md` (the per-call `EffectContext` copy, and the
  `run_event_mutex_` that fixes the map race but not the lifetime half) ·
  `docs/planning/2026-09-01-streaming-and-event-support-matrix.md` (the project's own disclosed open
  question, answered by this ADR).

## 1. The question

`run_event_kind::sandbox_exec_started` and `sandbox_exec_finished` have existed since the enum was
written. 013 §1 lists `SandboxExecStarted · SandboxExecFinished` as normative members of the internal
run event stream. `protocol/agui/projection.hpp` projects both to an AG-UI `ActivitySnapshot`, and
`tests/test_rt_agui_projection.cpp` tests that projection.

**Nothing in the tree ever emitted one.** The only construction site anywhere was the synthetic event
in that projection test. So the practical consequence issue #64 names is real: sandbox provisioning —
a `native-jail` cold start, or a `docker create` plus a worktree seed — was a fully opaque blocking
wait from any caller's perspective, on every surface (AG-UI, MCP progress, A2A, SSE). The only thing
that surfaced sandbox timing at all was 008 §8's post-hoc metrics: dashboards after the fact, not a
live signal to a waiting UI.

Two questions, then: **where does the producer belong**, and **is `SandboxExec{exec_id}` enough to
carry the signal 013 §1 promises?**

## 2. Where the producer belongs — and why the issue's own answer could not work

Issue #64's step 1 proposed: *"most naturally wherever `AgentSession` drives a sandboxed tool call
through to `SandboxBackend::create()`/`exec()` (008 §2), emitting `sandbox_exec_started` before
`create()`+`exec()` begins ... mirroring how `tool_call_started`/`tool_call_finished` already bracket
a tool dispatch."*

**That call site does not exist.** `AgentSession` never calls `SandboxBackend::create()`, `exec()`, or
`destroy()`; `rt/agent_session.hpp` contains two incidental mentions of the word "sandbox" in
comments and nothing else. The suggestion traces to a true-but-differently-scoped line in the
project's own matrix doc — *"Only `AgentSession` itself ever emits these"* — which is a statement
about `emit_run_event()` being a private method of that class, not about the class knowing when a
sandbox runs. It does not.

What the real call sites *do* have is an `EffectContext&`, by contract:

- `SandboxBackend::create(spec, ctx)` / `exec(handle, request, ctx)` (`sandbox/sandbox.hpp`) —
  reached in production by `extract_pdf_text_detail::invoke_worker()`, shared by all four PDF tools.
- `Runner::run(request, state, ctx)` (`sandbox/runner.hpp`) — reached in production by
  `SessionShellSandbox::run()`, the live `run_shell` tool.

So the reverse channel this needed already existed structurally; only the field was missing. ADR-170
adds `EffectContext::sandbox_exec_sink`, a **dedicated** field rather than a reuse of
`report_progress`, for the same "one field per audience" reason ADR-152's two workflow bridge fields
are separate from it: `report_progress` is bound per call and carries that call's `call_id` into a
`tool_call_delta`; a sandbox exec is not a tool-call delta, has its own correlation id, and is emitted
from beneath the tool by code with no notion of which model tool call it serves.

`AgentSession` binds and resets that sink at exactly the three `invoke_tool()` bracket sites
`report_progress` already uses (`dispatch_tool_calls()`'s sequential loop, `make_call_ctx()` for the
parallel path, and the codeact-ask replay) — not as a permanent field, because `effect_context_` is
wholesale reset by `fork_from()` and `clear_in_process_state()`, and a permanently-bound sink would
be silently lost by either.

### A third execution stack, named rather than silently skipped

`ExecutionSurface`/`SandboxRuntime::run()` (`sandbox/sandbox_runtime.hpp`) is a separate exec stack
that takes **no `EffectContext` at all** — and per GitHub issue #63 it is not wired to
`SandboxBackend` either. It emits nothing under this ADR. Wiring it needs a signature change that
belongs with #63's own work, not smuggled in here.

### `SandboxBackendRegistry` deliberately not wired

`sandbox/sandbox_backend_registry.hpp` type-erases `create`/`exec` behind `std::function`s and looks
like the natural single choke point. It is not wired, for two reasons, both checked rather than
assumed: it has **zero consumers anywhere in the tree** (`RegisteredSandboxBackend` appears in no file
but its own), so wiring it would add unexercised code; and because the one live `SandboxBackend`
consumer calls the backend *directly*, a future host that routed the same backend through the registry
would get **two** started/finished pairs for one exec. Named as the right future choke point in §7.

## 3. The payload — why `exec_id` alone was not enough

`SandboxExec` was one string. A UI receiving it cannot say which backend is provisioning, and — the
load-bearing gap — cannot tell the slow half from the fast half. That distinction *is* the feature:
issue #64's "practical consequence" section is entirely about cold start being opaque.

Added, all defaulted and appended last so every existing positional `SandboxExec{id}` compiles
unchanged:

- **`backend`** — which backend/runner ran it (`"native-jail"`, `"linux-native-jail"`,
  `"mediated-shell"`).
- **`stage`** — `"create"` vs `"exec"`. A **plain string, not an enum**: the set of meaningful stages
  is a property of whichever execution stack a producer sits on (`SandboxBackend`'s
  create/exec/destroy, `Runner`'s single `run`, a future `ExecutionSurface`'s reset/run/drain), and an
  engine-side enum would either enumerate all of them speculatively or force a real producer to lie.
- **`ok` / `error_code`** — meaningful only on `sandbox_exec_finished`. The projection reflects this:
  they are **omitted** from a started event's `ActivitySnapshot` rather than emitted at their
  defaults, because `"ok": true` next to `"status": "started"` reads as a result that has not happened
  yet.

**No producer fabricates a stage it did not perform.** `SessionShellSandbox` emits `"exec"` only: its
sandbox is provisioned once per session (008 §6's `per_session` lifetime) and reused, so there is no
per-call provisioning phase, and reporting a synthetic `"create"` would be an invented cold start on a
mount that has existed since the session began.

## 4. `SandboxExecScope` — fail-closed by construction

A raw call site emitting the two halves by hand has an obvious failure mode, and the real producer has
it four times over: `extract_pdf_text_detail::invoke_worker()` has four early returns between
`create()` and `exec()`. So the bracket is an RAII scope, and its default outcome is
**`ok = false, error_code = "sandbox.exec.abandoned"`** — a call site that returns early reports
honestly that the exec did not finish, rather than emitting nothing (a start with no end, which every
consumer must then time out on its own) or a success that never happened. The caller marks the real
outcome with `succeeded()` or `failed(code)`.

Neither copyable nor movable: two live objects sharing one `exec_id` would emit two finished events
for one exec, and the whole value of the type is that the pairing is structural. The destructor
swallows a throwing host sink — an observability event is never worth terminating a run over, and a
destructor has no channel to report through anyway.

One deliberate asymmetry between the two producers, and it is not an oversight: the PDF worker reports
a non-`ok` `exec_outcome_class` as a **failed** exec (the tool itself turns exactly that into an error
result, and the event stream must not disagree with the tool result a UI shows beside it), while
`run_shell` reports it as a **completed** exec (a command that ran and exited non-zero really did
execute; its own success/failure travels to the model in `RunShellReply::ok`).

## 5. The detached-thread hazard, closed at the same choke point

`tool_pipeline.hpp::background_task()` already resets `report_progress` and `sandbox_fs` on its
by-value `EffectContext` copy before the detached `std::thread` runs. `sandbox_exec_sink` is bound at
the same three sites, captures the same `[this]` into the originating session, and ends at the same
`emit_run_event_for()` — so it is reset there too, unconditionally.

Worth being precise about *which* half of the original hazard remains: ADR-160 gave
`emit_run_event_for()` a `run_event_mutex_`, which eliminates the `run_event_seq_by_run_` map race
ADR-060 §4 found. What it does **not** fix is object lifetime — a detached thread outliving a session
that has since been forked, cleared, or destroyed. That is what this reset closes, and S7 proves it by
running a `Backgroundable` tool that genuinely opens a scope on the detached thread and asserting the
caller's live sink never fires.

## 6. Evidence

`tests/test_sandbox_exec_events.cpp`, S1–S9, 41 checks.

| | claim |
|---|---|
| S1 | the started event fires at construction, the finished event at scope exit, both carrying exec_id/backend/stage |
| S2 | **fail-closed default**: an abandoned scope reports `ok=false`, `"sandbox.exec.abandoned"` — never a phantom success, never a dangling start |
| S3 | `failed(code)` carries that exact code; `succeeded()` clears it |
| S4 | **end to end**: a tool opening a scope produces real events on a real `AgentSession`'s `enable_event_stream()`, with real monotonic `seq` and run attribution — they went through `emit_run_event()`, not a side channel |
| S5 | positive control: a silent tool produces no sandbox events, and the tool genuinely ran (not an empty-run false pass) |
| S6 | two sandboxing calls in one round produce exactly two pairs; a silent tool in a *later* round produces none — the per-call bracket leaks no stale binding |
| S7 | `background_task()`'s detached thread never reaches the caller's sink |
| S8 | the AG-UI projection carries `backend`/`stage`, includes `ok`/`error_code` on finished, and **omits** them on started |
| S9 | **a real, shipped producer**: `SessionShellSandbox`'s `run_shell`, through the real 006 §3 ten-step pipeline against a real directory, emits a genuine pair tagged `"mediated-shell"`/`"exec"`, with a distinct `exec_id` per call |

S9 is what issue #64's step 3 asked for — the events firing on the real path, not only in a
projection unit test. The PDF producer is not covered by an equivalent live test here: it needs
`AE_PDF_WORKER_EXE_PATH` and a real PDF fixture, and its own tests are already conditional; the code
is written and compiles on both platform branches, and this is disclosed rather than claimed as
proven.

**Build and suite.** Clang 21 / Ninja / Debug under this project's `-Werror` warnings target: zero
warnings, zero errors on a full rebuild. Full `ctest -j8`: **277/277 passed**, including the existing
`test_rt_agui_projection` (whose synthetic `SandboxExec{"exec-1"}` still compiles and passes
unchanged — the proof that the payload widening is genuinely additive) and
`test_session_shell_wiring` (unchanged by the producer added to the file it tests).
`tools/naming_lint.py` and `tools/milestone_status_lint.py` both OK.

## 7. What this explicitly does NOT do

- **Does not emit from `ExecutionSurface`/`SandboxRuntime::run()`** — that stack takes no
  `EffectContext` at all, and per issue #63 is not wired to `SandboxBackend` either. Belongs with
  #63's work.
- **Does not wire `SandboxBackendRegistry`** (§2). It is the right future choke point; today it has
  no consumers, and wiring both it and a direct caller would double-emit.
- **Does not emit a `destroy` stage.** `SandboxBackend::destroy()` is `void` and takes no
  `EffectContext` — there is nothing to emit through without a signature change, and teardown is not
  the wait a caller is blocked on.
- **Does not give the PDF producer a live end-to-end test** (§6). Code written, compiled on both
  platforms, not executed under an assertion in this suite.
- **Does not carry timing.** No duration field: a consumer derives elapsed time from the two events it
  already receives, and an engine-side duration would be a wall-clock read on a path that has no I5
  recording seam (008 §8's post-hoc metrics remain the place for measured durations).
- **Does not emit from a backgrounded tool**, by construction (§5). Same deliberate exclusion
  `report_progress` already carries.
- **Was not run through the full `design → red-team → prove → judge` process.** This is an
  observability channel that mints no authority, gates nothing, and reuses an established reverse-
  channel shape at a new field. The adversarial work that did happen was the code-read that found the
  issue's own proposed call site does not exist (§2).

## 8. Promotion gate

**G1 (met).** A real, shipped tool performing a real sandbox execution produces a
`sandbox_exec_started`/`sandbox_exec_finished` pair on a real `AgentSession` event stream, correctly
tagged with backend and stage, correlated by a stable `exec_id`, and projected to AG-UI with those
fields intact — while a tool that performs no sandbox execution produces none. Falsifiable and it does
fail: reverting the `session_shell_wiring.hpp` producer fails S9, reverting the `agent_session.hpp`
bracket fails S4/S6, and reverting the `tool_pipeline.hpp` reset fails S7.

**G2 (met).** Purely additive: no existing test file changed, and the pre-existing synthetic
projection test still compiles and passes against the widened payload.

**G3 (open, for the project owner).** Should `stage` be promoted to a closed enum once a second
execution stack (issue #63's `ExecutionSurface`) starts emitting, or does the open-string form
continue to earn its keep? Not decided here; §3 records why it is a string today.
