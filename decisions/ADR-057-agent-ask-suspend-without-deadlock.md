# ADR-057 — Suspending `agent.ask()` mid-`execute_code` without deadlocking `session_mutex_`

**Status:** Proposed. Design → red-team → prove phases complete for Design B (abort-and-replay).
Real code implements ADR-057 §9's plan, the seven committed tests (B1-B7) all pass against a real
embedded CPython interpreter, and a full rebuild (964/964 targets) plus a full `ctest` sweep
(196/196 registered tests, 0 failed) confirm no regression anywhere else in the tree — see §5/§6
below for the executed evidence and per-claim verdicts. **Not yet Judged** — per this project's own
governance (`decisions/README.md`, `OpenQuestions.md` OQ-11), only the project owner marks an ADR
Judged; that has not happened.

**Relates to:** `decisions/ADR-029-suspend-for-human-approval.md` (the `Interaction`/suspend/resume
mechanism this reuses, and the mutex-acquisition shape whose limits this ADR runs into),
`decisions/ADR-030-session-scoped-codeact-wiring.md` (`CodeActRunnerBinding`'s "at most one session
ever live against the shared runner" claim, which this design must not violate), `decisions/ADR-028-
session-scoped-stateful-tools.md` (`Backgroundable`/`BackgroundCompletionQueue`, the existing
off-coroutine-thread precedent Design C leans on), `001-Execution-Model.md` §2 (`Interaction`,
`InputRequired`), `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.ask` — `Elicit` capability, no
implementation today).

## 1. The question

`agent.ask` (026 §5) is specified as an ordinary `agent.*` module function, called from Python code
running *inside* an already-dispatched `execute_code` tool call — not a top-level tool the model
calls directly. Tracing the real call path: `invoke_tool()` runs at `rt/agent_session.hpp:999`,
synchronously, **while the calling coroutine still holds `session_mutex_`** (`AsyncMutex::Guard`,
acquired via `co_await session_mutex_.lock()` at `rt/agent_session.hpp:407`/`460` — the I1 "one
session, one executor" guard). `execute_code` runs `MediatedPythonRunner::run()` inline on that same
call stack; `agent.ask()` would be a `PyCFunction` called from deep inside that stack, synchronously,
on the same OS thread.

ADR-029 already built a real suspend/resume mechanism (`Interaction`, `ResolveInteraction`, validate-
then-resolve, reject-a-second-concurrent-entry), but it suspends **before** any tool is invoked — a
genuine `co_await`-able point in `run_rounds()`, with `session_mutex_` released by construction once
the coroutine returns without responding. `agent.ask()` has no such point: the call needing to
suspend is already three stack frames past the moment the mutex was acquired.

**Stated so it has a wrong answer:** can a Python call inside a running `execute_code` genuinely pause
for a human answer and later resume with that answer injected as a real return value — continuing the
SAME running script, not a replay — without (a) holding `session_mutex_` for the wait, which
deadlocks (§2 below), and (b) silently abandoning I1 for the window the lock isn't held (some other
effect running against this session's capabilities while the ask is outstanding)?

## 2. Why the literal ADR-029 port is wrong, not just harder

The naive design — reuse `ResolveInteraction`, widen it with an answer payload, have `agent.ask()`
block synchronously on a condition variable that `resolve_interaction()` signals — **deadlocks by
construction**. The coroutine blocked inside `agent.ask()` is the one holding `session_mutex_`.
`resolve_interaction()` (the handler for the message meant to deliver the answer) needs that same
mutex to ever run (`rt/agent_session.hpp:460`). The answer can never be delivered, because delivering
it requires the lock the question is holding. This is not a race or an edge case — it is the only
possible outcome of that literal design, every time, and is the concrete reason this needs its own
ADR rather than a two-line follow-up to ADR-029.

## 3. Designs to red-team

### Design A — drop the lock around the blocking wait

`real_execute_code()` releases `session_mutex_` immediately before the interpreter would block inside
`agent.ask()`, blocks the OS thread on a condition variable, and the resolving call re-acquires the
mutex only to signal it and hand back the answer.

**Claimed correct:** cheapest change — no new thread, no new dispatch path, `execute_code` still runs
where it always has.

**Where a red-team should aim:** releasing the literal `std::mutex`/`AsyncMutex` is not the same as
preserving I1. For the window the lock is dropped, nothing stops a second `StartRun` from acquiring
`session_mutex_` and mutating `history_`/`mounted_skills_`/`ExecState` — precisely the state
`execute_code` is mid-flight against on the blocked thread. This needs a second, narrower guard
("this session has one outstanding ask; refuse other entry points") independent of the mutex itself —
structurally close to ADR-029 finding #5 (reject a fresh `StartRun` while an approval `Interaction` is
open), but that finding only had to protect against a competing `StartRun`; here the guard must let
*exactly one* thing through — the `ResolveInteraction`-equivalent call answering this ask — while
still blocking everything else, including a second concurrent ask attempt.

### Design B — no real pause; abort and replay

`agent.ask()` raises, unwinding the whole script. `execute_code` reports "suspended pending input" as
its outcome (a new `ExecOutcome` class, not a crash). The model is expected to issue a *new*
`execute_code` call once the answer is known, with the answer available to a re-run of the same
script (e.g. injected as a pre-supplied value the script checks for before calling `agent.ask()`
again).

**Claimed correct:** no deadlock is possible, because CPython is never actually paused mid-stack —
the interpreter call has already returned by the time anyone is waiting on a human.

**Where a red-team should aim:** this breaks 026 §5's own explicit design constraints — "ordinary
Python idiom... no callbacks, no handles to close" — by requiring every script that might call
`agent.ask()` to be written replay-safe/idempotent up to that point, and it throws away any local
state accumulated before the ask, undermining CodeAct's own stated value proposition ("one execution
instead of five model round trips," 026 §4b) for exactly the task class — interactive, human-in-the-
loop work — where that value proposition matters most. Should be treated as the fallback if A and C
are both defeated, not the first choice.

### Design C — run `execute_code` on a background thread, `Backgroundable`-shaped

Dispatch the Python call the way `Backgroundable`/`BackgroundCompletionQueue` already dispatch other
tool work (`rt/agent_session.hpp:90-234` — real, existing machinery from ADR-028, not proposed new
infrastructure) — off the coroutine thread entirely. The interpreter thread blocks on a condition
variable with the GIL released (`Py_BEGIN_ALLOW_THREADS`, the standard CPython idiom for exactly this
shape) while waiting for an answer. The session's own coroutine stays free to process a later
resolving message, acquire `session_mutex_` normally, signal the condvar with the answer, and return
— the two roles never contend for the same lock at the same instant, so the deadlock in §2 cannot
occur.

**Claimed correct:** the only one of the three that preserves both I1 (no interleaving — enforced the
same way `Backgroundable` already enforces it for other background work) and genuine continuation
(the same running interpreter, same local variables, same stack — not a replay).

**Where a red-team should aim:**
- The `Interaction{reason=input}` has to be opened from *inside* the native `PyCFunction` callback,
  not from `run_rounds()`'s pre-invoke branch (ADR-029's shape) — this is genuinely new code, even
  though it can share the `Interaction`/resolve-message primitives.
- Does a call sitting mid-execution on a background thread, for an unbounded duration (a human may
  never answer), stretch or violate `CodeActRunnerBinding`'s (ADR-030) "at most one session is ever
  bound to the shared runner" claim? ADR-030 proved first-caller-wins/fail-closed for *binding*; it
  did not evaluate a bound session holding the runner mid-call across an arbitrarily long human wait.
  Needs checking against ADR-030's own claim text and tests directly, not assumed compatible.
- `MediatedPythonRunner::run()`'s existing single-call contract (`mediated_python_runner.cpp:1262`)
  was written assuming one call runs to completion on one thread; splitting "block waiting for input"
  out from "the rest of the script" needs the C callback itself to release the GIL correctly and
  reacquire it before returning into CPython — a real, checkable CPython-embedding correctness
  property (a missed `Py_END_ALLOW_THREADS` is a silent, hard-to-reproduce hang or crash), not a
  design-level given.

## 4. The red-team attack

**Run this pass. Method: direct reading of the real source cited below (`rt/agent_session.hpp`,
`rt/async_mutex.hpp`, `rt/task.hpp`, `rt/thread_pool.hpp`, `core/tool_pipeline.hpp`,
`core/codeact_runner_binding.hpp`, `core/interaction.hpp`, `src/backends/native_jail/
mediated_python_runner.cpp`, `tools/cli_chat.cpp`), not the ADR draft's paraphrase. No standalone
probe was compiled for this pass — every finding below is reasoning from the real, checked-out
source, stated as such, not as executed evidence. Two of the findings below (0-1 and 0-2) are
independent of all three designs and were not anticipated by the draft at all; they are the load-
bearing findings of this pass and are given first.**

### Finding 0-1 (fatal, cross-cutting) — the one real production driver already deadlocks on Design A, exactly, with N=1

`tools/cli_chat.cpp:749` constructs `agentengine::rt::ThreadPool pool(1);` — **exactly one worker
thread** — and every `start_run()` call is submitted to it via `run_start_job()`
(`cli_chat.cpp:556-561`) and `pool.submit(run_start_job(...))` (`cli_chat.cpp:855`), then the caller
blocks on `fut.get()` (`cli_chat.cpp:857`). `ThreadPool::run_job()` (`rt/thread_pool.hpp:198-217`)
drives a submitted job with a straight-line `while (!item.job.done()) item.job.resume();`, and the
pool's own file banner (`rt/thread_pool.hpp:7-19`) states explicitly, as a **known, deliberate scope
limit**, that this loop does **not** support "general coroutine-parking-across-threads (a coroutine
suspending mid-body on an external async event and being resumed later by a DIFFERENT thread than
the one that started it)" — calling that "a genuinely separate, harder problem" needing "its own
dedicated design → red-team → prove pass before anything depends on it."

Design A's own text is explicit that it "blocks the OS thread on a condition variable" and that "the
resolving call re-acquires the mutex only to signal it." In the one real, tested production wiring
this codebase has, both of those things are the *same* thread: `execute_code` already runs on
"`rt::ThreadPool`'s one worker thread" by the project's own words
(`cli_chat.cpp:363-364`, `:372-373`), and nothing else is submitted to `pool` except through the same
single queue. If `agent.ask()` genuinely parks that thread, there is no second thread left to run the
job that would deliver the `ResolveInteraction`-shaped answer — `pool.submit(...)` for that job would
simply sit in `queue_` forever, since the one worker that could dequeue it is the one blocked inside
CPython. **This reproduces §2's exact deadlock, not a milder version of it, with the minimum possible
number of concurrent asks (one), in the actual current wiring — before I1 is even a consideration.**
Enlarging the pool only raises the threshold from 1 to N concurrent outstanding asks before the same
failure recurs (see Finding C3 below) — it does not remove it, and does not touch Design A's real problem,
which is that the thread doing the OS-level block is the *only* thread this codebase's own driver
gives session work to run on.

### Finding 0-2 (fatal for Design C specifically) — this embedding has never been built or verified for multi-thread CPython access, and says so in its own comments

`mediated_python_runner.cpp` never calls `Py_BEGIN_ALLOW_THREADS`, `Py_END_ALLOW_THREADS`,
`PyEval_SaveThread`, `PyEval_RestoreThread`, or `PyGILState_Ensure`/`PyGILState_Release` anywhere —
confirmed by direct search of the whole file, zero hits. The file's own header comment
(`mediated_python_runner.cpp:88-92`) states the reason: *"CPython's GIL serializes execution, so a
single set of TU-static pointers [`g_current_ctx`, `g_current_config`, `g_effective_keep_set`,
`config_.tool_bridge`], updated at well-defined points, is safe — **there is never more than one
`run()` call active at a time in this process**."* `g_current_ctx` is set at `run()` entry and only
cleared at `run()` exit (`mediated_python_runner.cpp:1273`/`1280`) — for the whole duration of a call,
including any time spent blocked deep inside it.

`tools/cli_chat.cpp:366-370` (in the *exact file* this ADR asks to inspect) says this even more
plainly, already, independent of ADR-057: *"a real, reproduced crash (CPython's C API touched from a
thread that never acquired its GIL state, since nothing in `mediated_python_runner.cpp` calls
`PyGILState_Ensure`; **this codebase's embedding was never built or verified for multi-thread
access, and hardening it is real, separate, out-of-scope follow-on work, not attempted here**) is
exactly as reachable through a ThreadPool worker as it was through an Engine worker."* The fix that
comment describes is "init and every real Python call now consistently happen on the SAME thread" —
i.e., the project's *current, working* answer to "how do we keep this safe" is "never let a second
thread touch it," not "release the GIL and let another thread take over," which is Design C's entire
mechanism.

Design C's "the interpreter thread blocks... with the GIL released... standard CPython idiom"
description is accurate about vanilla CPython embeddings in the abstract, but not about *this*
embedding: releasing the GIL here does not merely risk a generic re-entrancy bug, it removes the sole
synchronization this file's own author already named as the reason `g_current_ctx`/
`config_.tool_bridge`/`g_effective_keep_set` are safe as bare TU-statics, for as long as a human
takes to answer. And Design C's own text requires a *second* thread to eventually touch CPython's
state to deliver the answer (the resolving coroutine or the worker resuming past the wait) — the
precise "thread that never acquired its GIL state" scenario the project's own comment already
identifies as a real, previously-reproduced crash class, with no `PyGILState_Ensure` anywhere to make
it safe. This is not a hypothetical risk needing a probe to surface; it is a documented, known-crash
class this project has already hit once (per the comment) and explicitly deferred hardening for.

---

### Design A — drop the lock around the blocking wait

- **A1 (fatal).** Finding 0-1 above applies directly and unconditionally: in the real driving loop
  this codebase actually has, Design A deadlocks on the very first outstanding ask, independent of
  whatever second guard protects I1. Dropping `session_mutex_` was never the scarce resource; the
  scarce resource is the one OS thread `ThreadPool(1)` hands session work to run on, and Design A
  parks it synchronously for the human's whole response time.
- **A2 (must-fix, found independent of A1).** The "second, narrower guard" Design A gestures at
  cannot simply reuse ADR-029 finding #5's existing mechanism. `start_run()`'s admission check
  (`rt/agent_session.hpp:419-428`) only rejects a fresh `StartRun` when an open `Interaction` with
  `reason == interaction_reason::approval` exists — it does **not** check `reason ==
  interaction_reason::input`. That narrowing was deliberate and tested: ADR-029 finding #5's own text
  says an open `input`/`auth` `Interaction` "legitimately coexists with an ordinary fresh `StartRun`
  (a host's own passivate/reactivate bookkeeping)," and 026 §5's own table (`026-Agent-Facing-Runtime-
  Surface.md:174`) maps `agent.ask` to exactly `interaction_reason::input`. If `agent.ask()` reuses
  `interaction_reason::input` as-is, a second `StartRun` arriving while an ask is outstanding sails
  straight through the existing check and starts a genuinely concurrent second round against the SAME
  `history_`/`exec_state_`/`mounted_skills_`/bound `runner_binding_` the blocked script is still
  mid-flight against — the exact I1 violation this ADR itself names as the thing that must not happen.
  Fixing this needs either a *new* `interaction_reason` value (distinct from ordinary `input`, since
  ordinary `input`/`auth` interactions are proven-and-tested to be safe to coexist with a fresh
  `StartRun` and this one is not) with `start_run()`'s admission check widened to cover it too, or an
  equivalent bespoke guard — genuinely new code, not a reuse of an existing predicate, and easy to get
  wrong by assuming "input-reason interactions are already handled" from ADR-029's own precedent.
- **Verdict: DEFEATED.** A1 alone is fatal in the current runtime; A2 would still need a real fix even
  if A1 were somehow not applicable (e.g. a future multi-worker driving loop).

### Design B — no real pause; abort and replay

- **Steelman confirmed, and stronger than the draft credited.** Design B is the only one of the three
  that needs zero new runtime-substrate machinery: no new thread, no GIL games, no dropped
  `session_mutex_`, no interaction-reason collision (A2 doesn't apply — the round genuinely completes
  and the guard genuinely releases, the same shape every other fail-closed branch in `run_rounds()`
  already uses). It is buildable today, against the runtime as it actually exists, which none of the
  other two are (Findings 0-1/0-2).
- **New finding, not named in the draft (must-fix or accept-as-residual, not fatal).** The draft
  frames Design B's cost as "throws away any local state accumulated before the ask." That undersells
  it: a replayed script does not just lose *local variable* state, it **re-executes every side-
  effecting statement before the `agent.ask()` call a second time** — including mediated `open()`/
  `socket()` calls and any `agent.tools.foo(...)` bridged tool call the script made earlier in the
  same run (`mediated_python_runner.cpp`'s `call_tool` bridge, `:634-652`). None of those are
  idempotent by construction; `invoke_tool()`'s own idempotency key (`tool_pipeline.hpp:357`,
  `derive_idempotency_key`) is keyed on `{run_id, turn_index, call_index, arguments}` for the
  *outer*, model-visible tool call (`execute_code` itself), not on anything inside the script Design B
  re-runs verbatim — a second `execute_code` invocation is a genuinely new call with a new
  `call_index`/idempotency key, so the outer pipeline provides no protection against the script's own
  interior effects repeating. "Write scripts idempotent up to the ask" is a real, load-bearing
  requirement this design pushes onto every script author, not merely a style preference — a script
  that writes a file or calls a paid API before its first `agent.ask()` and is then replayed would do
  that write/call twice. Worth naming explicitly as Design B's real cost, distinct from and worse than
  "loses local state." **Now demonstrated, not just argued — see B7 in §5/§6 below: a mediated file
  write before `agent.ask()` is shown to run twice on replay, under a real embedded interpreter.**
- **Verdict: SURVIVES AS-IS**, with the side-effect-repetition residual named (not fixed) — the same
  "residuals named, not silently assumed complete" standard this project applies elsewhere (e.g.
  ADR-028 §6, ADR-030 §5).

### Design C — run `execute_code` on a background thread, `Backgroundable`-shaped

- **C1 (fatal to the draft's own framing).** Design C's central claim is that this reuses "real,
  existing machinery from ADR-028, not proposed new infrastructure." That is false for this specific
  tool. `ExecuteCodeTool`'s descriptor is built via `make_tool_descriptor_with_invoke<ExecuteCodeTool>`
  (`cli_chat.cpp:431-432`), which unconditionally sets `captures_session_state = true`
  (`tool_pipeline.hpp:134`). `background_task()` — the ONLY function that ever detaches a thread in
  this codebase's tool pipeline — refuses any descriptor with `captures_session_state` set, by name,
  before ever reaching step 8 (`tool_pipeline.hpp:519-523`, error code
  `tool.state_capturing_not_backgroundable`). So the literal, existing `Backgroundable`/
  `background_task()`/`start_background_task()` path **cannot** run `execute_code` at all, today, by
  design — the guard ADR-028 built to prevent exactly this class of hazard is already sitting in the
  way. Design C therefore cannot "reuse" this machinery; it must build a **new**, parallel dispatch
  path outside `background_task()` that does what `captures_session_state`'s guard exists to prevent.
- **C2 (fatal, restates Finding 0-2 in this design's own terms).** That new dispatch path is exactly
  the "detach a background thread that then touches CPython's C API" scenario `cli_chat.cpp:366-370`
  already names as a real, previously-reproduced crash class this embedding has never been hardened
  against. Not a hypothetical — the project's own comment says this crash has already happened once.
- **C3 (fatal in the current wiring; structural even with a larger pool — restates Finding 0-1 in this
  design's own terms, plus a new observation).** Design C's claim that "the session's own coroutine
  stays free to process a later resolving message" does not hold merely because the Python call moves
  to a different OS thread. `AsyncMutex::Guard` (`rt/async_mutex.hpp:78-105`) is an ordinary local
  variable living in `start_run()`'s/`resolve_interaction()`'s **coroutine frame**
  (`rt/agent_session.hpp:407`/`460`); a C++20 coroutine's frame — and everything declared in it,
  including that `Guard` — survives every `co_await` suspension inside it and is destroyed only when
  the coroutine itself completes or is destroyed. Suspending `run_rounds()` at some new "wait for the
  background thread" awaiter does **not**, by itself, release `session_mutex_` — the `Guard` is still
  alive, unreleased, in the enclosing frame. For "the coroutine stays free" to be literally true,
  `run_rounds()` would have to be restructured to explicitly drop and later reacquire the very same
  `Guard` Design A drops — i.e., **Design C, done correctly, must incorporate Design A's own mutex-
  drop mechanism (and inherits A2's interaction-reason gap with it)**; it is not a third, independent
  alternative to Design A, it is Design A plus a background thread plus new CPython-thread-safety
  obligations. And even granting that the guard is explicitly dropped: whatever awaiter `run_rounds()`
  suspends on to wait for the background thread is, again, exactly the "external suspension" kind
  `rt/thread_pool.hpp`'s own banner (`:7-19`) says its `run_job()` driving loop does not support — the
  single pool worker (Finding 0-1) would either busy-loop re-`resume()`ing a coroutine that isn't
  really ready yet, or (if built correctly to avoid that) needs a genuine cross-thread-resumption
  primitive this codebase has explicitly deferred as separate, harder, unbuilt work.
- **C4 (must-fix, a genuinely new variant of the ADR-028 hazard class, not just a name-check).** Even
  setting aside C1-C3: a bespoke background thread built to bypass `captures_session_state`'s guard
  would hold a closure over `this` → `ToolDeclaringHistoryProvider`'s own `mounted_skills_`/
  `exec_state_`/`runner_binding_` (`cli_chat.cpp:531-539`) for however long the human takes to answer.
  `AgentSession::fork_from()` (`rt/agent_session.hpp:569-591`) copies `history_provider_` by
  assignment and is **completely unguarded — it never acquires `session_mutex_` at all**, a residual
  ADR-030 §5 already names as "unexercised in production today" specifically because `cli_chat.cpp`
  never calls `fork_from()`. `clear_in_process_state_locked()` (`rt/agent_session.hpp:701-705`) *does*
  acquire `session_mutex_`, so if Design A/C's own lock-drop (C3) is in effect during the wait, a
  concurrent delete could run and reset `history_provider_` to `HistoryProviderT{}`
  (`rt/agent_session.hpp:626`) via assignment **in place** — the same object address the detached
  thread's closure still points at — while that thread is mid-read/write against
  `exec_state_`/`runner_binding_` through it. This is the identical dangling-reference/data-race
  *class* ADR-028's `captures_session_state` check was built to prevent, reintroduced through a
  different entry point that check does not — and cannot, since it isn't `background_task()` —
  cover. ADR-030 §5's "unexercised residual" framing was written assuming any window where this
  matters is vanishingly brief (an ordinary synchronous native call); Design C turns that window into
  "up to however long a human takes to answer," which is the exact stretch the question sheet asked
  this pass to check ADR-030's own claim against — and it does not hold once the window is that long.
- **C5.** Inherits A2 verbatim (same `interaction_reason::input` admission gap) if `agent.ask()`
  reuses the same reason tag, which nothing about Design C changes.
- **Verdict: DEFEATED AS SKETCHED.** Not "needs a named fix" in the ADR-029/030 sense of a scoped,
  same-pass correction — C1-C3 each require genuinely new, unbuilt runtime infrastructure this
  project's own comments already call separate, harder, deferred work: (a) a dispatch path that
  detaches a session-state-capturing closure, which is the exact thing ADR-028 exists to forbid, (b)
  a CPython embedding actually verified safe for multi-thread access (real `PyGILState_Ensure`-based
  thread-state handoff, currently absent everywhere in this codebase), and (c) genuine cross-thread
  coroutine parking/resumption beyond what `rt::ThreadPool` documents itself as supporting today.
  Building all three as a drive-by inside this ADR would be building foundational runtime substrate
  under the banner of a narrow feature design, which is exactly the shape this project's own
  governance asks to avoid.

### A fourth direction, not in the draft, found during this pass and not designed here

Windows fibers (`CreateFiber`/`SwitchToFiber`) are worth naming as an unexplored option, because they
sidestep Finding 0-2 specifically: a fiber switch parks the interpreter's C stack *without* moving
CPython execution to a different OS thread — the OS thread identity that "acquired its GIL state" (by
virtue of being the one and only thread that ever calls into CPython, per this embedding's existing
safety argument) never changes, so `PyGILState_Ensure`/multi-thread hardening might not be a
prerequisite the way it is for Design C. This platform (Windows-primary per CLAUDE.md) already has the
primitive available. Untested, unscoped, and not evaluated against `session_mutex_`'s own semantics or
`ThreadPool`'s driving loop here — named as a promising avenue for a future design pass, not a fourth
candidate this pass evaluated on equal footing with A/B/C.

## 5. Executed evidence

**Run** (this pass, prove phase, 2026-08-14). Real code implementing §9's plan was written across
`include/agentengine/core/interaction.hpp`, `include/agentengine/rt/interaction_codec.hpp`,
`include/agentengine/sandbox/sandbox.hpp`, `include/agentengine/core/effect_context.hpp`,
`include/agentengine/core/run_event.hpp`, `include/agentengine/rt/agent_session.hpp`, a new
`src/backends/native_jail/agent_ask_codegen.hpp`, `src/backends/native_jail/
mediated_python_runner.{hpp,cpp}`, and `tools/cli_chat.cpp`, plus a new
`tests/test_agent_session_suspend_codeact_ask.cpp` (B1-B7) registered in `tests/CMakeLists.txt`
alongside the other `AGENTENGINE_BUILD_PYTHON_RUNNER`-gated tests.

**Build.** Configured and built in Release, as this project's own notes require for any
Python-runner target (the vendored CPython NuGet package ships only `python313.lib`, not the debug
variant):

```
cmake -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DAGENTENGINE_BUILD_PYTHON_RUNNER=ON -DAGENTENGINE_WITH_HTTPS=ON
cmake --build build-py -j 8
```

(Generator note: this environment's CMake 4.1.1 does not know the installed Visual Studio Build
Tools 2026 / MSVC 19.51 toolset by a `-G "Visual Studio ..."` name, so Ninja + an explicit
`vcvarsall.bat x64` environment was used instead of the multi-config VS generator other notes in this
repo assume — same toolchain, same `/MD` Release ABI, different generator front-end. Configure pulled
a fresh vendored CPython 3.13.5 + numpy/pandas via the existing `AGENTENGINE_VENDOR_PYTHON` FetchContent
path, unchanged from this project's documented default.)

**New test target — direct run:**

```
./build-py/tests/test_agent_session_suspend_codeact_ask.exe
```

Result: **all checks pass** (B1 through B7, every named `check()` in the file — see the per-claim
table in §6 for the individual claims this run actually exercised).

**Regression — full suite, actually run to completion.** `cmake --build build-py -j 8` (every
target, 964/964, examples and every test executable included — not just the tests this pass touched)
built with **zero errors**; the only warnings anywhere in the log are pre-existing `C4834` ("discarding
[[nodiscard]] return value") hits at `agent_session.hpp:1112`/`1237`, the SAME two call sites that
already existed and already warned before this pass (`co_await history_provider_.on_turn_end(...)`'s
return value, discarded the same way at every one of its call sites, old and new), plus unrelated
pre-existing `getenv`/unused-local warnings in other tests untouched by this pass. No new warning
class was introduced.

Then the full registered test suite was run for real and blocked on to completion (not sampled):

```
ctest --output-on-failure -j 4        # from build-py, with the vendored Python DLL dir on PATH
```

**Result: 196/196 tests passed, 0 failed, in 43.32s.** This includes, by exact name from the real
ctest log:

- `test_agent_session_suspend_codeact_ask` (this pass's own new B1-B7 target) — **Passed** (0.41s).
- `test_rt_agent_session_suspend_approval` (ADR-029's own SU1-SU6 suite, the precedent B1-B7
  mirror) — **Passed** (0.01s) — confirms this pass's changes to `interaction_reason`/
  `ResolveInteraction`/`EffectContext`/`run_event_kind` (all additive), `start_run()`'s widened
  admission check (the pre-existing `approval` arm is untouched; only a NEW `codeact_ask` arm was
  added alongside it), and `resolve_interaction()`'s new early branch (taken only when
  `reason == codeact_ask`, which SU1-SU6 never opens) left ADR-029's own suite byte-for-byte
  unaffected.
- `test_reference_agent_containment_invariance` (026 §1a/§8 G4) — **Passed** (0.08s).
- Every other `AGENTENGINE_BUILD_PYTHON_RUNNER`-gated test (`test_mediated_python_runner_*`,
  `test_python_*`, `test_native_jail_*`, `test_reference_agent_task_corpus`, etc.) — **all Passed**,
  confirming the `sandbox.hpp`/`mediated_python_runner.{hpp,cpp}` changes (the new
  `exec_outcome_class::ask_pending`, `ExecOutcome::ask_prompt`, `ExecRequest::preseeded_answers`
  fields, the new `AskPending` sentinel exception machinery) did not perturb any existing sandbox/
  Python-embedding test.
- Every non-Python-runner test in the tree (worktree store, capability tokens, A2A/MCP/AG-UI,
  workflow supervisor, chat-client translation, etc.) — **all Passed**, confirming the `Interaction`/
  `run_event.hpp`/`EffectContext` additions (touched by many unrelated `AgentSession<...>`
  instantiations across the test suite, per the build log) are genuinely additive.
- The 6 `live-network`-labeled tests (real OpenAI/Anthropic/OpenRouter/llama.cpp calls) completed
  and reported Passed under this environment's own configuration — unrelated to this pass either way
  and not a claim this ADR relies on.

This supersedes the earlier, narrower "full build compiled clean but ctest not run" caveat from an
in-progress version of this evidence section — the full sweep has now actually been run to
completion and is 100% green.

**A real plan correction made during this pass, not silently smoothed over:** §9's original text says
the preseeded-answer field is threaded "as a new field on the internal (non-model-facing) exec
request." Read literally, `MediatedPythonRunner::run()`'s only request parameter is
`sandbox::ExecRequest` (`language`, `source`) — the actual generic "internal exec request" type,
shared by every `Runner` backend (shell included). The field (`preseeded_answers`) was added there,
not to a new bespoke struct, since `ExecRequest` already IS "the internal, non-model-facing exec
request" 010 §3a's own vocabulary names — a shell runner or any other `Runner` simply never reads it,
matching `ExecOutcome::result_repr`'s own "empty/unused, not a gap" convention one struct over. Threading
the ANSWER content itself from `AgentSession::resolve_interaction()`'s `codeact_ask` branch down to
`real_execute_code()` needed a second new field, on `EffectContext` (`codeact_preseeded_answers`) —
not named explicitly in §9's own text, but required because `ExecuteCodeArgs` (the model-facing JSON
schema, `cli_chat.cpp`) must never carry it (I3), and `EffectContext` is the one thing already
threaded, unmodified, from `AgentSession` through `invoke_tool()` into every tool's own `invoke()`
call — the same seam `bound_capabilities` already uses for a different per-call value. Named here
because it is a real, if small, elaboration of §9's plan that only became concrete while writing the
code, not something §9's own prose fully specified in advance.

**A real bug found and fixed during this pass (not a plan correction, an implementation mistake):**
the first draft of `AgentSession::resolve_codeact_ask()` named its local `ToolResult` variable
`result` — which shadows the `agentengine::result<T>` alias template for the rest of that function's
scope, causing a real MSVC C2760/C2065 compile failure the moment `result<void> const erased = ...`
was written a few lines later. Fixed by renaming the local to `tool_result`. Caught by the compiler,
not by review — named here as the kind of thing a "did it compile AND pass" bar catches that a
read-through would not have.

**A real test-authoring mistake found and fixed during this pass (the actual reason B1 initially
HUNG, not a hang in any of the new production code):** the first draft of every B-test script called
`agent.ask(...)` without a preceding `import agent` statement. `agent` is a module resolvable via
`sys.modules['agent']` (so `import agent`/`from agent import ask` succeeds) but is **not** a bare name
auto-injected into a script's own globals — the identical requirement `agent.tools`/`agent.files`
already have in every OTHER test in this codebase (they all call `import agent`/`from agent import
tools` first). Without the import, the script raised an ordinary `NameError`, `real_execute_code()`
returned an ordinary (non-`ask_pending`) tool result, `run_rounds()`'s new ask-pending check never
fired, and the round folded an ordinary tool-result message and continued to the NEXT round —
which, against a `ScriptedChatClient` with only one scripted entry and `max_turns` left at this
project's own unbounded-by-default setting, repeated the identical tool call forever. Confirmed by:
(1) an isolated probe (`MediatedPythonRunner::run()` called directly, bypassing `AgentSession`
entirely) proving the CPython/`mediated_python_runner.cpp` layer itself does NOT hang and correctly
returns `ask_pending` for both an imported and (via leftover `sys.modules` state from an earlier call
in the same process) an un-imported reference; (2) temporarily bounding `max_turns` on the failing
scenario, which converted the hang into a fast, clearly-labeled `run.max_turns_exceeded` failure; (3)
dumping the folded tool-result content, which showed the real `NameError` traceback. Fixed by adding
`import agent` to every B-script; not a defect in `run_rounds()`, `resolve_interaction()`,
`mediated_python_runner.cpp`, or any other production file this pass touched.

## 6. Per-claim verdicts

Decided by observed output (the test run in §5), not argument, matching `decisions/README.md`'s own
standard.

| Claim | Verdict | Evidence |
|---|---|---|
| B1 — `agent.ask()` suspends the round; `start_run()` completes with `kSuspendedForCodeActAsk`; a real `Interaction{reason=codeact_ask}` opens; nothing folds into `tool_results_message` yet | **CORRECT** | All 9 B1 checks pass, including the exact history-size/tail-role assertions proving nothing was folded, and the `codeact_ask_requested` event carrying the real prompt text. |
| B2 — a fresh `StartRun` sent while a `codeact_ask` interaction is open is rejected (mirrors ADR-029's SU2; the test that would have caught Finding A2 if it were missing) | **CORRECT** | All 5 B2 checks pass: rejected with `run.approval_pending`, no new `run_id` minted, the one open interaction untouched. |
| B3 — resolving with an answer re-runs the script and completes correctly for a script with a deterministic, side-effect-free prefix (`x = 1` before `agent.ask()`, used after) | **CORRECT** | All 8 B3 checks pass, including the direct read of the folded `execute_code` reply showing stdout `1 42` — `x` recomputed fresh on replay, `y` the preseeded answer, combined correctly. |
| B4 — two sequential `agent.ask()` calls in one script: first resolve produces a SECOND ask-pending suspension under the SAME `interaction_id` with an updated prompt; second resolve completes | **CORRECT** | All 11 B4 checks pass, including the event-stream proof that the first `codeact_ask_requested` carries `'q1'` and the second carries the UPDATED `'q2'`, both under the identical `interaction_id`. |
| B5 (positive control) — a script with no `agent.ask()` call completes in one round exactly as before this change | **CORRECT** | All 4 B5 checks pass: one round, no `Interaction` opened, `ChatClientT` called exactly twice (the ordinary pre-existing shape), converges to the scripted response. |
| B6 — a round with `execute_code` plus another tool call, where the script ask-pends, fails closed with `run.codeact_ask_in_multi_call_round_unsupported` | **CORRECT** | All 4 B6 checks pass: the round fails with the named sentinel code, no `Interaction` opens, and history holds only the user input + assistant tool-call message — no `tool_results_message` folded. |
| B7 (residual, demonstrated) — a mediated file write BEFORE `agent.ask()` is shown, on replay, to repeat that write | **CORRECT** | All 5 B7 checks pass, including the literal on-disk assertion: the marker file reads `"X"` after the first suspension and `"XX"` after resolve — the write ran twice, turning §4's Design-B residual from an argued claim into a demonstrated one. |

**Overall verdict for this pass: Design B (abort-and-replay), as concretely specified in §9, is
CORRECT and BUILDABLE against the real runtime as it exists today** — no new runtime substrate was
needed (confirming §4's own lean), the two red-team findings this ADR was built to close (Finding
0-1's deadlock class and Finding A2's admission gap) are both closed and tested (B1/B2), and the
named residual (side-effect repetition on replay) is now a permanent, passing regression test (B7)
rather than only a documented risk.

**What this pass does NOT establish**, named rather than silently assumed: (1) genuine mid-script
pause (Design C's actual goal, §7 below) remains blocked on the separate, harder, unbuilt
prerequisites Findings 0-1/0-2 named — this pass did not attempt them and should not be read as
having made progress on them; (2) `Interaction::expires_at_ns`/cancellation-of-a-stuck-interpreter
remain exactly as unwired as §8's residuals already named, unchanged by this pass; (3) the full
`ctest` sweep (§5) exercises this pass's changes only through the deterministic, offline test
fixtures already in the tree — it is not a claim about behavior against a live model, which no test
in this ADR's own scope (B1-B7) attempts either, matching every other suspend/resume ADR's own
offline-only precedent (ADR-029).

## 7. Current lean, and what a judge should not do with it

**The red-team pass reverses the draft's own lean.** Design C — the draft's favorite — is the one
most thoroughly defeated: §4's Findings 0-1/0-2/C1-C4 show it does not merely need a scoped fix, it
needs at least three pieces of genuinely new, unbuilt runtime substrate (a session-state-capturing
background dispatch path deliberately outside ADR-028's guard, a CPython embedding actually hardened
for multi-thread access, and cross-thread coroutine resumption beyond what `rt::ThreadPool` documents
itself as supporting) that this project's own comments already name as separate, harder, explicitly
deferred work — none of which this ADR should attempt as a drive-by. Design A is flatly defeated by
Finding 0-1: in the one real production driver this codebase has (`ThreadPool(1)` in `cli_chat.cpp`),
it reproduces §2's exact deadlock on the very first outstanding ask, before I1 is even a
consideration. **Design B is the only one of the three that survives** — not because it was the
draft's preferred fallback, but because it is the only one buildable against the runtime as it
actually exists today, with a real, previously-unnamed cost (§4: replay can repeat a script's own
side effects, not merely lose local state) that should be named as a residual, not hidden.

**This reversed lean must not be treated as a final decision either.** A judge should read this as:
adopt Design B for a first cut (it needs no new runtime infrastructure and its residual is nameable,
matching this project's "residuals named, not fixed" standard), and treat genuine mid-script pause
(Design C's actual goal) as blocked on separate, prerequisite ADR(s) for the runtime substrate gaps
Findings 0-1/0-2 surfaced — not as a problem this ADR can solve by picking harder between A/B/C as
sketched. The fiber-based direction named at the end of §4 is worth a future design pass specifically
because it might close the CPython multi-thread gap (Finding 0-2) without requiring the
never-verified-safe cross-thread embedding hardening C2 flags — but it is unexplored, not a fourth
candidate this pass adjudicated. This is the same discipline that caught ADR-029's own first draft (an
in-place `StartRun` field extension) failing on a hard, measured constraint (the 192-byte message-pool
ceiling) that no amount of careful reading of the design doc alone would have surfaced — here, reading
the REAL driving loop and the REAL embedding's own comments, not the design sketch's paraphrase of
them, is what surfaced it.

**UPDATE (this pass, prove phase, 2026-08-14):** §5/§6 now supersede this section's own "lean" framing
with real, executed evidence for Design B specifically — the lean held. §9 (below) is no longer a
"prove-phase plan, written before implementation"; it is what was actually built, with the two
corrections/one bug named in §5. This ADR is ready for the project owner's Judge pass on Design B; it
is NOT ready to be read as having resolved Design C's own prerequisites, which remain open questions
for a future ADR.

## 9. Concrete mechanism for Design B (prove-phase plan, written before implementation)

Design B survived §4 as a sketch ("`agent.ask()` raises, the model re-issues `execute_code`"). Before
writing real code, the actual mechanism needs to be nailed down against the real types it touches —
this section is that plan, grounded in the current shapes of `ExecOutcome`
(`sandbox/sandbox.hpp:143-156`), `ExecuteCodeArgs`/`ExecuteCodeReply` (`cli_chat.cpp:93-105`), and
`run_rounds()`'s existing pre-invoke suspend branch (`rt/agent_session.hpp:959-1006`) as the shape to
extend, not copy.

- **New `interaction_reason::codeact_ask`** (`interaction.hpp`), distinct from plain `input` — Finding
  A2 already established that plain `input`/`auth` interactions are proven-and-tested to coexist with
  a fresh `StartRun`, and a codeact ask must NOT permit that (the outstanding ask's replay state is
  keyed to a specific suspended round). `start_run()`'s admission check
  (`rt/agent_session.hpp:419-428`) gets a matching new arm.
- **`ExecOutcome` gains `exec_outcome_class::ask_pending`** plus a new `std::string ask_prompt` field
  (empty except when `klass == ask_pending`) — the same "empty means legitimately absent, not
  unpopulated" convention `result_repr` already documents on the struct one field up.
- **A new `agent.ask(prompt: str) -> str`** module function, codegen'd the same shape as
  `agent_tools_codegen.hpp`/`agent_files_data_codegen.hpp` (a new, pure-C++, Python-free
  `agent_ask_codegen.hpp`), bound to a new `_ae_internal.ask_or_raise(prompt)`. Its C implementation
  consults a `std::vector<std::string>` of **preseeded answers** threaded in as a new field on the
  internal (non-model-facing) exec request — NOT added to `ExecuteCodeArgs`'s `AE_JSON_SCHEMA`, since
  that schema is what the model sees and preseeded answers are host-driven replay state, never
  something the model supplies directly. An unconsumed preseeded answer at the current call index is
  returned directly (`str`) and the index advances; with none available, it raises a dedicated
  sentinel Python exception carrying the prompt text, caught in `MediatedPythonRunner::run()` and
  translated to `ExecOutcome{klass: ask_pending, ask_prompt: "..."}`.
- **`real_execute_code()`** (`cli_chat.cpp`) maps an `ask_pending` outcome to a `ToolResult` carrying a
  new sentinel error code, `codeact.ask_pending`, with the prompt in the error detail — never folded
  as an ordinary tool success or failure.
- **`run_rounds()`'s invoke loop** (`rt/agent_session.hpp:992-1006`), after `invoke_tool()` returns:
  check the result for `codeact.ask_pending`. **Scoped deliberately to a single-call round** — if the
  round has more than one pending tool call and one of them ask-pends, fail the round closed with a
  new, clearly named error (`run.codeact_ask_in_multi_call_round_unsupported`) rather than guessing
  what should happen to the other calls' already-committed side effects; multi-call-round asks are a
  named residual, not solved here (the analogous scoping call ADR-029 §6 already made for partial,
  per-call approval). For the single-call case: open `Interaction{reason=codeact_ask}`, store a
  `PendingCodeActAsk{source, language, answers_so_far={}, tool_call_id}` record keyed by
  `interaction_id`, emit `input_required` plus a new `codeact_ask_requested` event carrying the
  prompt, and suspend via the same "never fold, never fabricate a response" shape the approval branch
  already uses (a new sentinel, `kSuspendedForCodeActAsk`).
- **`resolve_interaction()`** gains a `reason == codeact_ask` branch: validates the interaction is open
  and `history_`'s tail is still the exact suspended assistant tool-call message (identical validation
  order to ADR-029's own fix for finding #4), appends the caller-supplied answer to the stored
  record's `answers_so_far`, and re-invokes `real_execute_code()` directly against the STORED
  `source`/`language` plus the updated `answers_so_far` — bypassing the model and the public
  `ExecuteCodeArgs` schema entirely, since this is host-driven replay, not a new model-issued call. If
  the re-run ask-pends again, the SAME interaction stays open (its stored prompt/record updated, a
  fresh `codeact_ask_requested` fires, the caller gets `kSuspendedForCodeActAsk` again) — chaining
  through as many questions as one script asks without minting a new `interaction_id` per question. If
  the re-run completes (success or ordinary failure, not another ask-pending), the interaction closes,
  the real `ToolResult` folds into `tool_results_message` exactly where the original call would have,
  and `run_rounds()` continues normally for any further rounds.
- **`ResolveInteraction` gains `std::optional<std::string> answer`** (additive; interpreted only when
  `reason == codeact_ask`, alongside the existing `approved` field used only for `reason == approval`)
  — safe to widen now that ADR-037 removed the Quark message-pool byte cap that forced ADR-029's own
  separate-message design in the first place.

### Tests this plan commits to before calling it proven (deterministic, offline, no live model — matching `tests/test_agent_session_suspend_approval.cpp`'s own style)

- **B1** — a script calling `agent.ask()` suspends the round; the `start_run()` call completes with
  `kSuspendedForCodeActAsk`; a real `Interaction{reason=codeact_ask}` opens; nothing folds into
  `tool_results_message` yet.
- **B2** — a fresh `StartRun` sent while a `codeact_ask` interaction is open is rejected (mirrors
  ADR-029's SU2, exercising the NEW admission-check arm A2 found missing — this is the test that
  would have caught A2 if it existed before this pass).
- **B3** — resolving with an answer re-runs the script and completes with the correct result for a
  script whose only side effect is a deterministic local computation before and after `agent.ask()`
  (e.g. `x = 1; y = agent.ask("q"); print(x, y)`) — proving replay of a **deterministic, side-effect-
  free prefix** behaves indistinguishably from real continuation for that narrow case, explicitly not
  a general continuation claim.
- **B4** — two sequential `agent.ask()` calls in one script: the first resolve produces a SECOND
  `ask_pending` suspension under the SAME `interaction_id` with an updated prompt; the second resolve
  completes.
- **B5 (regression/positive control)** — a script with no `agent.ask()` call at all completes in one
  round exactly as before this change — proves the new code path is additive, not a regression to the
  ordinary case.
- **B6** — a round with `execute_code` plus another tool call, where the script ask-pends, fails
  closed with `run.codeact_ask_in_multi_call_round_unsupported` — proves the named scoping boundary is
  real and tested, not merely asserted in prose.
- **B7 (residual, demonstrated not just claimed)** — a script that performs a mediated `open()`/write
  BEFORE its `agent.ask()` call is shown, on replay, to repeat that write — turning §4's
  side-effect-repetition finding into a permanent regression/documentation test, the same "residuals
  named AND demonstrated" standard Phase G4 already set (finding the `find_fs_write` capability bug
  while building the quota check, not merely asserting it existed).

## 8. Residuals to name up front, regardless of which design is chosen

- **Answer shape.** Start with a plain string only, matching 026 §5's "small and boring, guessable
  from its name" constraint — a typed/schema'd answer (mirroring `agent.output`'s eventual
  `OutputSchema<T>` enforcement, 003 §4) is a strictly separate, later extension.
- **Multiple `agent.ask()` calls in one script.** Should fall out for free under Design C (each call
  is just another blocking wait on the same background thread); not free under Design B (each call
  compounds the replay problem). **Now proven, not merely "not proven either way" — see B4 in §5/§6:
  Design B chains multiple asks correctly, under the same `interaction_id`, at the real cost the
  replay mechanism always had (each resolve re-runs the whole script from the top).**
- **Expiry.** `Interaction::expires_at_ns` is already unwired project-wide (ADR-029 §6 names this;
  unchanged since). This ADR should not be the one that fixes it — an `agent.ask()` with nobody ever
  answering stays suspended exactly as long as an unresolved approval does today. Unchanged by this
  pass.
- **Cancellation of a stuck interpreter.** What kills a background thread blocked on an ask forever —
  or blocked inside CPython generally — is the same open problem as the still-unbuilt wall-clock/
  memory enforcement residual named in the Milestone 3 breakdown (`docs/planning/milestone-3-
  worktree-interpreter-codeact-breakdown.md`, Phase G4's "residuals, named explicitly"). Building a
  real "interrupt a running embedded interpreter" primitive here would plausibly seed that gate too,
  but that is an opportunity to note, not a scope this ADR should silently absorb. Not relevant to
  Design B specifically (nothing blocks synchronously waiting for a human under Design B — the
  interpreter call has already returned), but still open for a hypothetical future Design
  C-shaped mechanism.
- **Real UI/transport wiring** (an actual human-facing surface calling whatever resolves the ask) is
  out of scope here, matching ADR-029 §6's identical scoping call for approval. `tools/cli_chat.cpp`'s
  own `execute_code` wiring surfaces an `agent.ask()` call as an ordinary tool failure
  (`codeact.ask_pending`) with no resume path — named explicitly in that file's own comment, not
  silently left to look like a real dead end.
