# ADR-060 — Giving `EffectContext` a real, call-scoped `report_progress()` channel

**Status:** Proposed. Red-teamed, implemented (with one named correction to §2's own design, not the
original sketch verbatim), and proven — real code, 13 new checks (P1-P3, D) all pass against a real
`rt::AgentSession`/`tool_pipeline.hpp`, and a clean full rebuild plus a full `ctest` sweep (155/155
tests, 0 failed, run three times across two rebases) confirm no regression elsewhere in the tree — see
§5/§6 below for the executed evidence and per-claim verdicts. **Not yet Judged** — per this project's
own governance (`decisions/README.md`, `OpenQuestions.md` OQ-11), only the project owner marks an ADR
Judged; that has not happened.

**Relates to:** `013-UI-and-Streaming-Surfaces.md` §1 ("`ToolCallDelta`'s producer... a call to
`EffectContext.report_progress` during `invoke()` (006 §6a) is the only source" — the still-unbuilt
mechanism this ADR closes), `006-Tool-and-Function-Plane.md` §6a (the tool-authoring side of the same
contract), `include/agentengine/core/effect_context.hpp` (the struct this ADR adds a field to),
`include/agentengine/rt/agent_session.hpp` (`run_rounds()`'s three `invoke_tool()` call sites, the
real wiring point), `include/agentengine/core/run_event.hpp` (`ToolCallDelta`, `run_event_kind`,
already real — this ADR gives them their first real producer), `026-Agent-Facing-Runtime-Surface.md`
§5 (`agent.progress`, the CodeAct module this ADR is a prerequisite for, not itself in scope — see §3),
`decisions/ADR-028-session-scoped-stateful-tools.md` (`captures_session_state`/`Backgroundable`, the
existing dangling-reference/data-race hazard class this ADR's own red-team found a NEW door into),
`decisions/ADR-057-agent-ask-suspend-without-deadlock.md` §9 (the `codeact_preseeded_answers`
bracketing precedent this ADR's own field reuses the shape of, not the literal bracket).

## 1. The question, and what's already real (re-verified directly, not assumed)

`agent.progress` (026 §5) is blocked on "013's run event stream, including giving `EffectContext` a
reverse channel to push events through" (Milestone 3's own Phase G2 finding). Re-checking that
directly against the current tree found the event-stream HALF is already real and substantial —
`AgentSession::enable_event_stream()` (`rt/agent_session.hpp:431-433`) returns a genuine
`stream<RunEvent>` (`core/stream.hpp`, `rt::channel<T>`-backed), and `emit_run_event()`/
`emit_run_event_for()` are real member functions already firing for `RunStarted`/`ToolCallStarted`/
`TurnFinished`/etc. throughout the round loop. `ToolCallDelta` and `run_event_kind::tool_call_delta`
are real, already-declared types (`run_event.hpp`), exercised today only by `tests/test_mcp_progress.cpp`
and `tests/test_rt_agui_projection.cpp` — both construct `ToolCallDelta{...}` directly as test
fixtures, proving the PROJECTION machinery (event → MCP progress notification / AG-UI SSE frame), not
a real producer.

**What's actually missing, confirmed by a direct grep for `report_progress`:** `EffectContext`
(`core/effect_context.hpp`) has no such field or method — a tool's `invoke(Args, EffectContext&)`
has no way to reach back into the session's own `emit_run_event()` to push a `ToolCallDelta` for
itself, mid-call. That is the entire remaining gap. Unlike ADR-057's `agent.ask` problem, **this one
has no deadlock or cross-thread hazard**: a progress report is a pure, synchronous, fire-and-forget
push into an event stream the session already owns and is already running on the same call stack —
there is no suspend, no wait, no second thread. This should be a materially smaller, more contained
fix than ADR-057/041.

**Stated so it has a wrong answer:** can `EffectContext` carry a real, call-scoped progress-reporting
channel that a `Tool::invoke()` calls synchronously, routing through the SAME session's
`emit_run_event()` with the CORRECT `call_id` for whichever call is currently in flight — without
leaking a stale binding from a previous call into a later one, and without interfering with the
model-call phase of the same, reused `EffectContext` object?

**Correction found during red-team (§4): "no cross-thread hazard" was wrong.** The synchronous,
foreground `invoke_tool()` path genuinely has none — but `Backgroundable` tools go through a
COMPLETELY SEPARATE call path (`tool_pipeline.hpp::background_task()`) that the design as originally
sketched did not consider at all. See §4 Finding 5 for the real, confirmed hazard this introduced and
§2's corrected design below for the fix.

## 2. The design

`EffectContext` gains one new field:

```cpp
std::function<void(std::string_view)> report_progress = [](std::string_view) {};
```

Default-initialized to a no-op — matching this codebase's own "an optional-but-always-safe-to-call"
idiom rather than requiring every tool author to null-check before calling it. A tool's `invoke()`
calls `ctx.report_progress("25% done")` whenever it wants to report; with no session-side listener
bound (e.g. a test, or a code path that never sets it), this is a harmless no-op.

**Wiring, in `run_rounds()`/`resolve_interaction()`, at all THREE real `invoke_tool()` call sites**
(`rt/agent_session.hpp`, confirmed by direct grep to still be at lines 628, 1050, 1233 — unchanged
across the rebases this pass did, see §4 Finding 4 for the one correction to how the ADR draft
described these three sites relative to `codeact_preseeded_answers`'s own bracket): bind
`effect_context_.report_progress` to a closure capturing the CURRENT call's `call_id` immediately
before that one `invoke_tool()` call, and reset it back to the no-op immediately after:

```cpp
effect_context_.report_progress = [this, call_id = req.call_id](std::string_view text) {
    emit_run_event(run_event_kind::tool_call_delta,
                    run_event_payload::ToolCallDelta{call_id, std::string(text)});
};
ToolResult tool_result = invoke_tool(tool_table, held, req, effect_context_, ..., &audit);
effect_context_.report_progress = [](std::string_view) {};
```

Because `invoke_tool()` already receives `effect_context_` by reference — it is not a copy, it is the
session's own live member (confirmed: every real call site passes `effect_context_` directly, never a
copy) — a tool's `invoke()` reaching `ctx.report_progress(...)` really does call back into the exact
session instance that dispatched it, with no new plumbing beyond this one field and its per-call
bracket.

**Correction found during red-team (must-fix, §4 Finding 5):** `tool_pipeline.hpp::background_task()`
— the ONLY function that ever detaches a `std::thread` to run a `Backgroundable` tool's `invoke()` —
takes `EffectContext ctx` **by value**, not by reference. `AgentSession::start_background_task()`
copies `effect_context_` into that by-value parameter with **no `session_mutex_` acquisition**
(documented "PLAIN, UNLOCKED" in the file's own banner). If that copy races one of the three brackets
above being open on a different thread of control, a live `report_progress` closure — capturing
`this` (the `AgentSession`) — could be copied onto `background_task()`'s own detached thread, and an
ORDINARY call to `ctx.report_progress(...)` from the backgrounded tool's `invoke()` would then call
back into `emit_run_event()` from off-thread, racing `run_event_seq_by_run_` (an unlocked
`std::unordered_map`) against the session's own coroutine thread. **Fixed structurally**:
`background_task()` resets its own local copy's `ctx.report_progress` to the safe no-op
unconditionally, before step 8 ever runs — see §4 Finding 5 for the full analysis and §5/§6 for the
regression test (check D) proving this closes the hazard.

## 3. Deliberately out of scope

- **`agent.progress` itself (026 §5's CodeAct module).** This ADR gives `EffectContext` a reverse
  channel any NATIVE `Tool<>::invoke()` can call. Whether/how a Python script running inside
  `execute_code` reaches the SAME mechanism (presumably via a new `_ae_internal.report_progress(text)`
  binding, mirroring `agent.ask`'s own `_ae_internal.ask_or_raise` shape) is a smaller, separately-
  scoped follow-on once this lands — not attempted here.
- **Structured/typed progress payloads** (e.g. a percentage number, a stage enum) — `ToolCallDelta`'s
  existing shape is `{call_id, text}` (plain string), unchanged by this ADR.
- **Streaming/backpressure semantics beyond what `stream<RunEvent>`/`rt::channel<T>` already provide**
  — this ADR is a producer, not a new consumer or transport.
- **`StandingEffect`/background-task progress** (`006 §6b`) — `report_progress` as designed here is
  scoped to a synchronous, in-flight `invoke_tool()` call; a `Backgroundable` tool running on a
  detached thread (ADR-028) is a different delivery shape not addressed here. **Confirmed, not just
  assumed, by §4's own red-team**: `background_task()` now structurally guarantees a backgrounded
  tool's `report_progress()` calls are silently absorbed by a no-op, making this exclusion true by
  construction rather than merely true because nobody had tried yet.

## 4. The red-team attack

**Run this pass.** Method: direct reading of the real, current-tree source (`core/effect_context.hpp`,
`rt/agent_session.hpp`, `core/tool_pipeline.hpp`, `core/run_event.hpp`, `decisions/ADR-028-...md`,
`decisions/ADR-057-...md`), not the draft's own paraphrase, after rebasing this worktree onto `main`
(`git merge-base HEAD main` showed this worktree was 6 commits behind at pass start — ADR-057/041/042
all landed since this branch's own base — rebased clean, no conflicts, re-confirmed a second time
later in this same pass when `main` advanced 2 more unrelated commits during the pass; see §5).

### Finding 1 (confirmed-clean) — `invoke_tool()` really does pass `EffectContext` by reference, unmodified, all the way to `Tool::invoke()`

Read `tool_pipeline.hpp::invoke_tool()` in full. Its signature takes `EffectContext& ctx` (a
reference); step 8 calls `tool->invoke(request.arguments, ctx)` with that same reference, setting only
`ctx.bound_capabilities` around the call and clearing it after (`tool_pipeline.hpp:432-434`) — no copy
anywhere in between. All three real call sites in `rt/agent_session.hpp` (grepped directly: lines 628,
1050, 1233 — unchanged from the draft's own citation, confirming the draft's "re-locate, don't trust"
caution was warranted as a discipline but didn't actually find drift this time) pass `effect_context_`
— the session's own live member — never a local copy. §2's basic mechanism is sound for the
synchronous, foreground path: a bound `report_progress` closure genuinely reaches the exact session
instance that dispatched the call.

### Finding 2 (confirmed-clean) — no leaked binding between sequential calls in one round, no suspension inside a bracket

At all three sites, the bind-invoke-reset sequence is a single synchronous stretch with no `co_await`
between binding and resetting — confirmed by reading the code immediately around each of the three
`invoke_tool()` calls. `run_rounds()`'s own multi-call loop (site 3, the `for (std::size_t i = 0; i <
calls.size(); ++i)` loop) rebinds `effect_context_.report_progress` fresh at the top of each iteration
and resets it to the no-op immediately after that iteration's `invoke_tool()` returns, before the next
iteration's own bind — so two sequential calls in the same round can never observe each other's
binding. No suspension point exists inside any bracket where a concurrent `resolve_interaction()`
(guarded by the SAME `session_mutex_`, I1) could interleave and observe a stale binding either.

### Finding 3 (minor correction, not a defect) — the draft's "SAME call sites as `codeact_preseeded_answers`" claim overstates the existing precedent

The draft (§2, original) said the `report_progress` bracket goes "at the SAME call sites"
`codeact_preseeded_answers` already brackets. Reading `rt/agent_session.hpp` directly: of the three
real `invoke_tool()` call sites, `codeact_preseeded_answers` is bracketed at only ONE of them (the
`resolve_interaction()` `codeact_ask` branch, around line 1043-1051) — it is specific to codeact-ask
replay and was never bracketed at the other two (the `resolve_interaction()` approval-approved branch
around line 628, or `run_rounds()`'s own main loop around line 1233). `report_progress` needs its OWN
independent bracket at all three, not a shared one. Not a defect in the design — the bracketing
*discipline* (set immediately before, reset immediately after, per call) is exactly right and worth
reusing — just a factual correction to how the draft described the precedent. Implemented per §2
above: three independent brackets, one per site.

### Finding 4 (confirmed-clean, documented not structurally prevented) — a tool that stashes `ctx.report_progress` into a member and calls it later

The sharpest question the task posed: could a tool copy `ctx.report_progress` (a copyable
`std::function`) into a member variable and call it after `invoke()` returns and the bracket has reset
it, and if so, is that harmless or corrupting?

`Tool<Derived, Policies...>::invoke(Args, EffectContext&)` (the ordinary path,
`make_tool_descriptor<ToolT>()`) is a **static** function — it has no `this` of its own to persist
anything into across calls; a namespace-scope or function-local `static` inside such a tool COULD
technically stash a copy, but that stash still only ever holds a closure that itself captured
`this = AgentSession*` and `call_id` BY VALUE at bind time (the closure itself, not the tool, is what
would need to reach the session). The only tool shape that can meaningfully reach and persist
something tied to a live `AgentSession` in the first place is a session-scoped stateful tool built via
`make_tool_descriptor_with_invoke()` (ADR-028) — its `custom_invoke` closure is exactly what already
captures `this` (typically the owning `ContextProvider`, itself a session-scoped member with a real
lifetime tied to the session).

Read `tool_pipeline.hpp:519-523` directly: `background_task()`'s own authorize step refuses ANY
descriptor with `captures_session_state == true` — set unconditionally by
`make_tool_descriptor_with_invoke()` (`tool_pipeline.hpp:134`) — before step 8 ever runs. This is
exactly the ADR-028 guard, and it applies here without modification: a session-scoped stateful tool
(the only shape that could stash something reaching back to the session) can **never** be
`Backgroundable`, so a stashed-and-later-called copy of `report_progress` can only ever be invoked
synchronously, on the SAME session's own `session_mutex_`-serialized coroutine thread (I1), on some
LATER call to that same tool. Worst case: `emit_run_event()` fires for a `tool_call_delta` whose
`call_id` is real but stale (from an earlier, already-completed call) — a data-quality nuisance for a
downstream MCP-progress/AG-UI projection (it would need to notice the `call_id` isn't a currently
in-flight one and drop or otherwise handle it), never a memory-safety or concurrency hazard, since
`this` (the `AgentSession`) is read fresh on each call (`effect_context_.run_id`, etc.) and is
guaranteed live for as long as the stashing tool's own owning provider is alive (a member of the same
session).

**Verdict: confirmed-clean.** Matches this codebase's own existing convention for `EffectContext`'s
other borrowed/reference-shaped fields (`capabilities`, `bound_capabilities` — "borrowed; never owned
here," enforced by documentation, not the type system). Documented in `effect_context.hpp`'s own field
comment rather than structurally prevented — consistent with, not a lowering of, this codebase's
existing bar for this class of field.

### Finding 5 (MUST-FIX, the load-bearing finding of this pass) — `Backgroundable` tools reach `report_progress` through a completely separate, unsynchronized call path

The task's own §4 anticipated this as one candidate to check ("is there a real hazard from a
`Backgroundable` tool... given `EffectContext` itself is not necessarily safe to reference after
`invoke_tool()` returns for a backgrounded call... worth confirming this new field doesn't reopen
[the ADR-028 hazard class] through a different door"). It does.

**The mechanism, read directly, not assumed:**

1. `tool_pipeline.hpp::background_task()` (`:491-598`) — the ONE function in this codebase that ever
   detaches a `std::thread` to run a tool's `invoke()` — has the signature
   `background_task(ToolTable const&, CapabilitySet const&, ToolCallRequest const&, EffectContext ctx,
   ...)`. `ctx` is a **by-value parameter** (`:492`), not a reference. This is unlike `invoke_tool()`
   (Finding 1) and is a genuine, real copy of whatever `EffectContext` the caller passed.
2. `AgentSession::start_background_task()` (`rt/agent_session.hpp:793-836`) is the one production
   caller of `background_task()`, and passes `effect_context_` directly (`:813`) into that by-value
   parameter. The file's own banner (`:119-131`, SLICE 3's own design writeup) documents this method
   as **"PLAIN, UNLOCKED"** — deliberately not acquiring `session_mutex_` — and states explicitly:
   *"a host that calls these three [`start_background_task`/`cancel_standing_effect`/
   `list_standing_effects`] concurrently with a `start_run()`/`resolve_interaction()` in flight on
   another 'thread of control' races... a real, pre-existing-in-kind precondition."* This is not a
   hypothetical concurrent-call scenario this pass invented — it is the file's own, already-accepted,
   documented calling contract.
3. §2's three brackets (`run_rounds()`/`resolve_interaction()`) set `effect_context_.report_progress`
   to a closure capturing `[this, call_id]` for the synchronous duration of one `invoke_tool()` call,
   then reset it. If `start_background_task()` is called — as documented, legitimately — while one of
   those three brackets is open, on a genuinely different thread of control, the field being copied at
   step 2 above can observe that bound closure. (The data race on the field itself is technically UB
   per the C++ object model regardless of outcome; the concrete, non-hypothetical consequence below is
   what makes this must-fix rather than a theoretical nicety.)
4. That copied `EffectContext` — carrying the live, `this`-capturing closure — is handed to the
   backgrounded tool's own `invoke()`, running on `background_task()`'s genuinely detached
   `std::thread` (`:560-594`). If that tool calls `ctx.report_progress(...)` — a completely ORDINARY,
   INTENDED use of the very feature this ADR adds, requiring no tool misuse or malicious intent at all
   — the closure calls `emit_run_event()` (`rt/agent_session.hpp:897`) → `emit_run_event_for()`
   (`:900-909`), which executes `++run_event_seq_by_run_[run_id]` — a plain `std::unordered_map`
   mutation with **no lock** — from the detached background thread, potentially concurrently with the
   session's own coroutine thread calling the SAME `emit_run_event()` for its own, unrelated events.

**This is a confirmed, concrete data race on a live `std::unordered_map`**, not the same class as the
OTHER, already-accepted races on `EffectContext`'s remaining fields when copied by
`start_background_task()` (`run_id`, `turn_index`, the `capabilities`/`bound_capabilities` pointers) —
those are passive data, already named and accepted as a pre-existing, out-of-scope precondition by the
file's own banner, and `background_task()`'s own comment (`:478-483`) explicitly frames the pointer
fields as "the SAME 'host owns it, must outlive' contract... not a new hazard this function
introduces." `report_progress` is categorically different: it is an ACTIVE CALLBACK that, once
observed as bound, reaches back into `this` and mutates session-owned, unsynchronized state — the
exact hazard SHAPE ADR-028's `captures_session_state` guard exists to prevent, reopened here through a
door that guard does not cover (it only checks the TOOL's own descriptor flag, never whether the
`EffectContext` being copied itself carries a session-reaching closure).

**Severity: must-fix (would have been fatal if shipped as originally sketched).** Unlike the other,
already-accepted `EffectContext` races (inert data, out of scope, pre-existing), this hazard is newly
and directly introduced by this ADR's own design, and reachable through completely ordinary use of the
feature being added — no tool misuse, no adversarial input, no exotic reentrancy required.

**Corrective action (implemented, see §2 above and §5/§6 below):** `background_task()` resets its own
local copy's `ctx.report_progress` to the safe no-op, unconditionally, immediately after the existing
`captures_session_state` guard and before step 4/7 (authorize+bind) ever runs — structurally, at the
ONE function that ever detaches the thread, matching ADR-028's own "closed here structurally rather
than left as a documented-only rule a future caller could violate" precedent exactly. This guarantees
a backgrounded tool's `invoke()` never observes a live `report_progress` callback, regardless of
whatever was (possibly racily) copied in — closing the hazard at its single reachable choke point.
This does not remove a promised capability: §3 already excluded `StandingEffect`/background-task
progress delivery as a different, unaddressed shape; the fix makes that exclusion true by
construction, not merely true because nobody had tried yet. Proven directly (test D, §5/§6): with the
fix temporarily reverted, the exact regression this finding predicts was reproduced deterministically
(the caller's closure DID fire from the detached thread); with the fix restored, it does not.

### Finding 6 — `codeact_preseeded_answers`/`bound_capabilities`, checked for the same class of leak

`bound_capabilities` is safe: `background_task()`'s own worker thread sets it fresh
(`ctx.bound_capabilities = &bound;`, `:563`, using ITS OWN locally-bound vector) before any use,
unconditionally overwriting whatever was copied in from the caller — no window where a stale value
from the caller's own bracket is read. `codeact_preseeded_answers` is pure passive data (a
`vector<string>`), never a callback into `this` — copying a stale value has no callback-into-session
consequence, and it is bracketed only around `resolve_interaction()`'s own synchronous, non-
`Backgroundable` `execute_code` replay call, unaffected by this ADR either way.

### Overall verdict

**The design in §2 does NOT survive as originally sketched — it needed one named, structural
correction (Finding 5's fix in `background_task()`), not a different approach.** With that correction
applied, the design survives red-team: Findings 1/2/6 confirm the core mechanism is sound and leak-
free on the synchronous path; Finding 3 is a documentation correction, not a design defect; Finding 4
is confirmed-clean under the existing ADR-028 guard and documented (not structurally re-enforced,
matching precedent for this class of field); Finding 5's fix closes the one real, must-fix hazard this
pass found, proven by a regression test that fails without the fix and passes with it (§5/§6).

## 5. Executed evidence

**Rebase.** `git merge-base HEAD main` at pass start showed this worktree 6 commits behind `main`
(`main` tip `4c706d1`, ADR-057/041/042). `git rebase main` completed cleanly, no conflicts. Later in
the same pass, after all code/tests were written and the first full build+test sweep had already run
green, `main` advanced 2 more commits (`35383da`, `a60a39f` — both `web/marketing/` illustration-page
changes, confirmed via `git diff --stat` to touch only `ApiTrustSandboxReference.tsx`/
`apiContent.ts`, nothing under `include/`, `src/`, or `tests/`). Rebased a second time
(`git stash push -u` → `git rebase main` → `git stash pop`, matching ADR-059's own precedent) —
clean, no conflicts — so the final build/test evidence below reflects the fully-integrated tree, not
a stale base.

**Windows, MSVC (Visual Studio 18 Community, `vcvarsall.bat x64`), Ninja, Debug, default `build/`
tree** (fresh configure — no prior `build/` existed in this worktree). Environment note for this
session specifically: `cmd.exe /c <path>` invoked through this Bash tool silently mis-executes unless
`MSYS_NO_PATHCONV=1` is set first — MSYS's own path-conversion rewrites the literal `/c` flag into a
Windows drive path before `cmd.exe` ever sees it, producing a bare interactive shell instead of running
the target script (confirmed by direct diagnosis: identical invocation without the env var produces
only the interactive banner and no script output at all; with it, `vcvarsall.bat`'s own real output
appears). Recorded here since the task's own build recipe didn't anticipate it and a future pass in
this same environment would otherwise lose time rediscovering it.

```
CONFIGURE_EXIT=0
...
NINJA_EXIT=0
```

Full `ninja -j 8`: **zero errors**, both on the full-from-scratch sweep run immediately after the
first rebase (every target built) and on the incremental sweep run after the second rebase (`main`
having advanced 2 more, unrelated commits mid-pass — CMake auto-reconfigured, then rebuilt every
target whose dependency graph the reconfigure touched: 480 targets that run, `NINJA_EXIT=0`). Across
both runs, every warning touching any file this pass changed is pre-existing: `C4834` ("discarding
`[[nodiscard]]` return value") at `agent_session.hpp:1173`/`1333` — the SAME `co_await
history_provider_.on_turn_end(...)` discard already warned at every one of its call sites before this
pass, only the exact line numbers shifted from this pass's own insertions — plus the pre-existing,
unrelated `C4996` (`getenv`)/`C4702` (unreachable code) classes ADR-057/042's own evidence sections
already recorded. No new warning class was introduced anywhere in either build.

**New test target — direct run** (`build/tests/test_agent_session_tool_call_progress.exe`):

```
  ok: P1: the run converges normally
  ok: P1: exactly one tool_call_delta event fired for the one call that reported progress
  ok: P1: the event carries the real call_id of the in-flight call
  ok: P1: the event carries the exact text the tool reported
  ok: P2: the run converges normally
  ok: P2 (positive control): a tool that never calls report_progress() produces no tool_call_delta event -- the ordinary path is unaffected by this field's existence
  ok: P3: the run converges normally
  ok: P3: both sequential calls in the same round each reported progress -- two distinct events, no dropped or merged binding
  ok: P3: the first call's event is tagged with its OWN call_id (c1)
  ok: P3: the second call's event is tagged with its OWN call_id (c2), not a stale binding leaked from the first call's own bracket
  ok: D setup: the Backgroundable tool call is accepted
  ok: D setup: the detached thread actually finishes
  ok: D: the backgrounded tool DID call ctx.report_progress() from its own detached thread, but background_task()'s own structural reset means the CALLER's closure -- captured into this function's by-value EffectContext copy -- never fires; the cross-thread hazard ADR-060 §4 found is closed at its one reachable choke point, not merely documented
test_agent_session_tool_call_progress: ALL PASS
SINGLE_TEST_EXIT=0
```

**Regression test D actually verified to be non-vacuous** (this codebase's own "a test that cannot
fail proves nothing" bar, `CLAUDE.md`): the `background_task()` fix (§2/§4 Finding 5) was temporarily
reverted (the reset line commented out), the single test target rebuilt and rerun — check D **FAILED**
exactly as predicted (`fired.load() == 0` was false; the caller's closure genuinely fired from the
detached thread), while checks P1-P3/D-setup were unaffected. The fix was then restored, the target
rebuilt, and all 13 checks passed again. This confirms check D is a real, falsifiable regression test
for Finding 5, not a vacuously-passing one.

**Full sweep**, `ctest --output-on-failure -j 4` from the same `build/` tree, run three times across
this pass: immediately after the first rebase (155/155, confirming the newly-added test and no
regression), again after the fix-revert/restore verification cycle from the same base (155/155,
confirming the restored fix), and a final time — the evidence quoted below — after the second rebase
onto `main`'s further-advanced tip, from the final, restored source state:

```
100% tests passed, 0 tests failed out of 155

Total Test time (real) =  20.99 sec
CTEST_EXIT=0
```

**155/155 tests passed, 0 failed, 0 skipped** — every test in the default (non-Python-gated) tree
(154 tests as of ADR-059's own evidence, plus this pass's one new target), confirming no regression
anywhere else in the tree from this pass's changes to `effect_context.hpp`/`tool_pipeline.hpp`/
`agent_session.hpp` — including `test_rt_agent_session_suspend_approval` (ADR-029's own SU1-SU6),
`test_rt_agent_session_background_task` (ADR-037 Slice 3's own B1-B6), `test_agent_tool_invocation`
(ADR-059's own attenuation suite), and every other test that instantiates `AgentSession<...>` or calls
`invoke_tool()`/`background_task()`, all passing unchanged.

## 6. Per-claim verdicts

Decided by observed output (the test runs in §5), not argument, matching `decisions/README.md`'s own
standard.

| Claim | Verdict | Evidence |
|---|---|---|
| §2's core mechanism (bracket at three real call sites, reference not copy) is sound for the synchronous path | **CORRECT** | Finding 1/2 (direct source reading); P1/P2/P3 all pass |
| P1 — a tool calling `report_progress()` produces a real `tool_call_delta` with the correct `call_id` on `enable_event_stream()` | **CORRECT** | P1's 4 checks pass: exactly one event, correct `call_id` ("c1"), correct text ("half done") |
| P2 (positive control) — a tool that never calls it produces no such event | **CORRECT** | P2's checks pass: `saw_delta == false` |
| P3 — two sequential calls in one round each get their OWN correctly-tagged `call_id`, no leaked binding | **CORRECT** | P3's checks pass: exactly 2 delta events, one tagged "c1", one tagged "c2", neither duplicated or dropped |
| Stashed-closure question (Finding 4) — same-session, same-thread stash is at worst a stale-but-real `call_id`, never a memory-safety hazard | **CORRECT (confirmed-clean, documented not structurally enforced)** | Direct source reading: `captures_session_state` (ADR-028) already forces this path onto the synchronous, `session_mutex_`-serialized thread; no test needed beyond the existing ADR-028 guard's own S1-S4 suite, which this pass's changes leave untouched (full sweep, unaffected) |
| Backgroundable question (Finding 5) — is there a real cross-thread hazard, and is it closed | **CORRECT — real hazard found, corrected, and the correction proven** | Finding 5's direct source analysis (`background_task()`'s by-value `ctx`, `start_background_task()`'s "PLAIN, UNLOCKED" documented contract); test D fails when the fix is reverted and passes when restored — a demonstrated closure, not merely an argued one |
| No regression to the rest of the tree | **CORRECT** | Full sweep, 155/155, 100% pass, 0 skipped, twice (both rebases) |

**Overall verdict for this pass: the design in §2, AS CORRECTED by Finding 5's fix, is CORRECT and
BUILDABLE against the real runtime as it exists today.** The original draft's design would have been
a real, live hazard if implemented verbatim (Finding 5) — this pass did not implement it verbatim; it
implemented the corrected version and proved the correction closes the gap it found, not merely
asserted it.

**What this pass does NOT establish**, named rather than silently assumed: (1) `agent.progress`'s own
CodeAct/`_ae_internal` wiring (§3) — untouched, a separate follow-on; (2) structured/typed progress
payloads — untouched, `ToolCallDelta`'s existing `{call_id, text}` shape is unchanged; (3) real
`StandingEffect`/background-task progress DELIVERY (as opposed to the safe absorption this pass
proves) — deliberately out of scope (§3), and this pass's own fix makes the CURRENT absence of that
delivery a structural guarantee rather than an accident; (4) MCP progress notification / AG-UI SSE
frame projection of a real `tool_call_delta` from a live tool — this pass proves the EVENT is real and
correctly attributed; wiring it into `test_mcp_progress.cpp`/`test_rt_agui_projection.cpp`'s own
projection layer end-to-end (currently exercised only via hand-constructed fixtures, per §1) is a
separate, not-attempted-here integration.

## 7. Residuals to name up front

- `agent.progress`'s own CodeAct wiring (§3) — separate follow-on.
- No structured payload shape — plain string only, matching `ToolCallDelta`'s existing declared shape.
- `Backgroundable`/`StandingEffect` progress delivery — separate, unaddressed shape (§3), now backed
  by a structural (not merely documented) guarantee that a backgrounded tool's `report_progress()`
  calls are silently absorbed rather than leaking a hazard, per Finding 5's fix.
- The end-to-end MCP-progress-notification / AG-UI-SSE-frame projection of a REAL (not
  hand-constructed) `tool_call_delta` event, produced by an actual running tool via this ADR's own
  mechanism, is not exercised by any test in this pass's own scope — `test_mcp_progress.cpp`/
  `test_rt_agui_projection.cpp` continue to construct their fixtures by hand, unchanged by this ADR.
