# ADR-155 — `agent.progress` for CodeAct: streaming progress without a new frame type

**Status:** Judged (2026-08-31, project-owner sign-off). Designed (deliberately diverging from GitHub
issue #31's own first sketch, with the divergence named explicitly — §2), implemented, and proven end
to end against the REAL `agentengine_python_worker.exe` process — 10 checks (R1-R4) all pass, and the
full related native-jail/python-worker/report-progress test suite (7 targets, including ADR-060's own
`test_agent_session_tool_call_progress.cpp`) shows no regression. See §5/§6.

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.progress`'s spec, "Report progress on
long work → run event stream", zero capability), GitHub issue #31 (the full three-piece gap this ADR
closes), `decisions/ADR-060-tool-call-progress-reporting.md` (`EffectContext::report_progress`, the
already-real, already-tested channel this ADR reaches from CodeAct — §3 of that ADR named this exact
follow-on and deliberately left it undone), `src/backends/native_jail/
mediated_python_worker_protocol.hpp` (`kQueryProgress`), `src/backends/native_jail/
native_jail_backend.cpp` (`dispatch_worker_query()`'s new `progress` kind branch, `exec_session()`'s
frame loop — the fail-closed `terminate_worker()` fallback issue #31 itself named as the landmine a
naive implementation would hit).

## 1. The question

Per issue #31: three concrete pieces are missing — a Python-side `agent.progress` module, a worker-side
binding sending some new signal to the host mid-script, and a host-side branch recognizing that signal.
The issue's own text warns: without the third piece, adding only the first two would make every
`agent.progress` call **crash the running worker outright**, since `exec_session()`'s frame-loop
fallback (any unrecognized frame) calls `terminate_worker()`.

**Stated so it has a wrong answer:** can `agent.progress.report(text)` reach the ALREADY-real, ALREADY-
tested `EffectContext::report_progress` channel (ADR-060) from inside a running script, mid-execution,
without the script pausing/aborting (unlike `agent.ask`) and without needing a new wire-protocol frame
TYPE at all — reusing the SAME request/response envelope and the SAME fail-closed exec_seq check the
`worker_query` mechanism already has, so the "kills the worker" landmine issue #31 names cannot be
reached even by construction, not merely by remembering to add a branch?

## 2. The design — a deliberate divergence from issue #31's own suggested shape

Issue #31 sketched: "a **new frame type** (e.g. `worker_progress`)... sent... without waiting for the
script to finish." Implementing that literally would require (a) a whole new frame TYPE at the
`FramedChannel` level, (b) a genuinely one-way send capability the worker process does not currently
have anywhere (today it only ever calls a request/response `QueryFn`), and (c) a new branch in
`exec_session()`'s own `if (type == ...) ... else if (type == ...) ...` dispatch chain — and CRITICALLY,
that new branch is the one place a forgotten-or-buggy addition reproduces exactly the crash issue #31
itself warns about.

**This ADR does something narrower and safer instead: `agent.progress.report(text)` is implemented as
one more `worker_query` KIND** (`kQueryProgress = "progress"`), reusing the EXISTING `worker_query`/
`worker_query_response` envelope `call_tool`/`open`/`listdir`/etc. already use — no new frame TYPE, no
new branch in `exec_session()`'s dispatch chain at all (it already has an `if (type == wp::kWorkerQuery)
{ ...; dispatch_worker_query(inst, *frame, ctx); continue; }` branch; a new `kind` is just one more
`else if` inside `dispatch_worker_query()` itself, the SAME shape nine other kinds already use).

**This closes issue #31's own "kills the worker" landmine BY CONSTRUCTION, not by remembering to add a
branch**: there is no new frame type for the fallback `terminate_worker()` path to ever NOT recognize
in the first place. It also gets RT1 Finding 1's exec_seq-mismatch fail-closed check — already applied
uniformly, once, to every `worker_query` kind — for free, with no duplicate implementation.

**Trade accepted, named honestly:** this makes `agent.progress.report()` a synchronous, blocking round
trip (the script waits for the host's `{"ok": true}` ack before continuing) rather than a genuinely
fire-and-forget one-way send. This is NOT the same as `agent.ask`'s abort/suspend — the script's own
Python-level execution continues normally once the ack returns, proven by R2 below (`print()` calls
before/after/between `report()` calls all appear in order). §7 names the residual honestly: a script
that calls this in an extremely tight loop pays one IPC round trip per call, same as `agent.tools`'
`call_tool` already does for every native tool call — not a new cost class this ADR introduces.

**Reaching `EffectContext::report_progress` needed NO `AgentSession`-side change at all.** `exec_session()`
receives the SAME `EffectContext& ctx` the tool pipeline's `invoke_tool()` passed to `execute_code`'s
own `invoke()` — and because `execute_code` is itself an ordinary tool call, `agent_session.hpp`'s own
three bracketed call sites (ADR-060 §2) have ALREADY bound `ctx.report_progress` to the live,
`force_tainted()`-wrapping closure for the current `call_id` by the time `exec_session()`'s frame loop
ever runs. The new `progress` kind's handler just calls `ctx.report_progress(ContentItem{.value =
Text{text}})` directly — exactly ADR-060 §3's own named, deliberately-deferred follow-on.

## 3. Deliberately out of scope

- Structured/typed progress payloads — plain string only, matching `ToolCallDelta`'s own existing
  shape and ADR-060 §3's own identical exclusion.
- `Backgroundable`/`StandingEffect` progress delivery — `execute_code` is not `Backgroundable` in this
  codebase today; ADR-060's own structural absorption for that case is untouched, unaffected either way.
- MCP-progress-notification / AG-UI-SSE-frame projection of a REAL (not hand-constructed)
  `tool_call_delta` produced via THIS specific path — not exercised here, same residual ADR-060 §7 named.

## 4. Red-team notes

Built and run for real. The one thing worth naming explicitly (not a defect, a deliberate design
choice defended above): reusing `worker_query` instead of inventing a new frame type is ITSELF the
red-team-motivated correction to issue #31's own first sketch — found by tracing the exact failure mode
the issue named, before any code was written, not discovered by a failing test after the fact (unlike
ADR-153's two findings). R2/R4 below are the falsifiable proof that this choice actually closes the
"kills the worker" hazard: R2 proves the worker keeps running immediately after a call, and R4 proves
the SAME worker handles a completely unrelated LATER call normally, ruling out latent damage.

## 5. Executed evidence

**Windows, MSVC (Visual Studio 18 Community), MSBuild, Debug, existing `build/` tree.** New target,
run via `ctest`:

```
  ok: R1 setup: runner initializes a real jailed worker process
  ok: R1: exec_session() returns a real outcome, not a hang/crash
  ok: R1: an AttributeError from a script is an ordinary 'ok' outcome, not a policy_violation/crash -- not reachable without the opt-in
  ok: R2 setup: runner initializes with agent.progress exposed
  ok: R2: exec_session() returns a real outcome
  ok: R2: the run converges normally -- the worker was NOT terminated by any call
  ok: R2: the script kept running across and after every call -- this is not a suspend/abort the way agent.ask() is, and the worker was not crashed the way an unhandled frame type would (issue #31's own named landmine)
  ok: R3: three calls in one script produced three real events -- a streaming channel, not a last-call-wins declaration
  ok: R3: events carry the real text, in the real call order
  ok: R4: the SAME worker handles a later, unrelated call normally afterward
test_agent_progress_codegen: ALL PASS
```

**No regression** — the full related suite, rebuilt and rerun via `ctest`:

```
test_agent_session_tool_call_progress .......... Passed  (ADR-060's own native-tool proof, unaffected)
test_mediated_python_runner_smoke .............. Passed
test_native_jail_python_worker_slice1 .......... Passed
test_agent_output_codegen ...................... Passed  (ADR-154, shares this ADR's own new opt-in-flag pattern)
test_agent_progress_codegen .................... Passed
test_native_jail_python_worker_handle_relay .... Passed
test_agent_session_suspend_codeact_ask ......... Passed  (ADR-057's own agent.ask test)
100% tests passed, 0 tests failed out of 7
```

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| `agent.progress` is absent (loud failure, not silent) without the opt-in | **CORRECT** | R1's checks |
| A real call reaches `ctx.report_progress()` with the real text | **CORRECT** | R2/R3's checks |
| The script continues normally, not suspended/aborted | **CORRECT** | R2's stdout-ordering check |
| Multiple calls stream as multiple, correctly-ordered events | **CORRECT** | R3's checks |
| The worker is NOT crashed/terminated by any call — the "kills the worker" landmine issue #31 named is closed | **CORRECT, by construction (no new frame type exists to mishandle)** | R2 (converges normally) + R4 (a later, unrelated call also succeeds on the same handle) |
| No regression to `agent.ask`/native `report_progress`/the worker lifecycle | **CORRECT** | Full 7-target suite, 100% pass |

## 7. Residuals to name up front

- Each `report()` call is a synchronous IPC round trip (same cost class as `agent.tools`' `call_tool`,
  not a new one) — not genuinely fire-and-forget, a deliberate trade for closing issue #31's own named
  landmine by construction rather than by discipline (§2).
- Plain string payload only — no structured/typed progress, matching ADR-060 §3.
- `Backgroundable` delivery, and real MCP/AG-UI projection of a call produced through this path
  specifically, remain untouched — same residuals ADR-060 §7 already named.
