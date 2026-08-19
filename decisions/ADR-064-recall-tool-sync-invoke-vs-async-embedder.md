# ADR-064 — Can a synchronous `ToolDescriptor::invoke` safely reach `Embedder::embed_batch()`'s `task<T>`?

**Status:** Proposed (2026-08-19). Designed, then red-teamed once (§4, `general-purpose` agent, no
prior context) — found 2 critical + 3 real-gap + 2 minor findings against the design, **plus one
real, pre-existing, latent bug in already-shipped code** (`ThreadPool`+`AsyncMutex`+`AgentSession` —
see §4 finding 1). Design B revised in place below to fix the findings that are fixable within this
design (bound corrected 64→1, double-wrapped `result<T>` fixed, migration checklist added, citations
corrected). **The separately-flagged `ThreadPool` bug has since been FIXED for real** (§7 top —
`rt/thread_pool.hpp`, `tests/test_rt_thread_pool.cpp` T6, 194/194 suite green), independent of this
ADR's own Design B. **Design B itself is not yet implemented or proven** — §5/§6 still require real
code + tests before this ADR can move past Proposed.

**Relates to:** `decisions/ADR-063-retrieval-augmented-context-provider-shape.md` §7 (the exact gap
this ADR resolves — `VectorRagContextProvider::recall(query)`'s `invoke` fails closed today because
of this unresolved question), `006-Tool-and-Function-Plane.md` §6b (`Backgroundable`/`StandingEffect`),
`decisions/ADR-037-remove-quark-as-core-runtime.md` (introduced `agentengine::rt::task<T>` and
`agentengine::rt::ThreadPool`, the two primitives this ADR builds on), `include/agentengine/core/
tool_pipeline.hpp` (M2 Phase B's own documented decision: "a single NATIVE tool call, synchronous —
`ae::task<T>` deferred, decision 2" — the decision this ADR is finally revisiting, not overturning
casually).

## 1. The question, stated so it has a wrong answer

`ToolDescriptor::invoke` is `std::function<result<json::Value>(json::Value const&, EffectContext&)>`
— synchronous, no coroutine involved. `Embedder::embed_batch()` (`core/embedder.hpp`) is
`task<result<std::vector<std::vector<float>>>>` — a coroutine. `VectorRagContextProvider`'s
`recall(query)` tool needs to call the latter from inside the former. **Is there a way to do this
safely — provably not hanging the calling thread forever if the specific `Embedder` conformer being
driven ever genuinely suspends on external I/O — without changing `ToolDescriptor::invoke`'s
signature (and therefore every one of its 25+ call sites, per §3's red-teamed count: `rt/
agent_session.hpp`, `protocol/mcp/server.hpp`, `src/backends/native_jail/mediated_python_runner.cpp`,
and every test that constructs a `ToolCallRequest`) to itself return a coroutine?**

A wrong answer here is a real hang in production the moment a network-backed `Embedder` conformer is
composed into a live `AgentSession` — not a test failure, a frozen session.

## 2. Background — what already exists, verified by reading the actual code (not assumed)

Four facts, each independently checked against the real source before any design below relies on it:

1. **`rt::task<T>` already has a first-class, generic "direct driving" API**, not test-only:
   `resume()`, `done()`, `faulted()`, `fault_ptr()`, and — for `T != void` — `take_value()`
   (`rt/task.hpp:193-208`), whose own comment states its purpose plainly: *"Mode (b): direct driving
   ... `take_value()` moves the result out (or rethrows a fault)."* This type was designed, from the
   start, to support being driven synchronously by an executor — it is not something this ADR would
   be bending out of shape.

2. **`rt::ThreadPool` (`rt/thread_pool.hpp`) already does exactly this, in real production code, not
   a test.** Its `run_job()` (private, static) is `while (!item.job.done()) item.job.resume();` —
   the identical shape this ADR needs a sibling of. Its own top comment states an explicit, narrow
   scope: *"this type does NOT attempt general coroutine-parking-across-threads... What THIS type
   does is much smaller and self-contained: take a `task<void>` whose body may internally `co_await`
   other NESTED `task<T>`s (those all resolve synchronously via symmetric transfer... no external
   suspension is ever involved) and run it to completion on one worker thread."*
   **CAVEAT, added after red-team (§4 finding 1): this scope claim is NOT actually true of the code
   `run_job()` drives in production today.** `AgentSession::start_run()`/`resolve_interaction()`
   themselves begin with `co_await session_mutex_.lock()`, and `AsyncMutex::LockAwaiter::
   await_suspend` (`rt/async_mutex.hpp:129-142`) genuinely parks the coroutine handle when contended
   — exactly the "external suspension" `ThreadPool`'s own comment disclaims. This is a real, latent,
   pre-existing bug independent of this ADR (see §4 finding 1) — `ThreadPool`'s idiom is *used* in
   production, and is *safe today only because* `tools/cli_chat.cpp` always constructs `ThreadPool
   pool(1)` with strictly serial job draining, not because the scope claim holds in general. This
   weakens (does not invalidate) the "load-bearing precedent" framing below — Design B's own
   soundness rests on `OpenAIEmbedder::embed_batch()`'s STRONGER property (fact 3: awaits nothing at
   all, not merely "only nested task<T>"), not on `ThreadPool`'s scope claim being generally true.

3. **`OpenAIEmbedder::embed_batch()`'s actual body (`protocol/openai/embedder.hpp:229-275`, read in
   full) never `co_await`s anything at all.** Every step — `store_.resolve(...)`,
   `sandbox::perform_provider_https_exchange(...)`, `json::parse(...)` — is a plain, synchronous,
   BLOCKING function call (`perform_provider_https_exchange` returns `result<NetEgressResponse>`
   directly, not a task; its own sibling `perform_provider_streaming_exchange`'s comment confirms the
   non-streaming exchange is "one COMPLETE blocking fetch"). The only coroutine keywords in the whole
   function are `co_return`. This is a STRONGER property than ThreadPool's own "nested task<T> only"
   scope requires: this task doesn't even nest another task — it never suspends, period, by
   inspection. (Whether this stays true for every FUTURE `Embedder` conformer is exactly what §3's
   `synchronous_leaf` trait exists to keep honest, not assume.)

4. **In real production use (`tools/cli_chat.cpp`), `AgentSession::start_run()` — and everything it
   calls, including every tool's `invoke()` — already runs entirely on ONE `rt::ThreadPool` worker
   thread, not the calling/main thread**, via `run_start_job` (`cli_chat.cpp:584-589`) submitted to a
   `ThreadPool pool(1)` (`cli_chat.cpp:777`), with the CLI's own thread blocking on the returned
   `std::future` (`cli_chat.cpp:883-885`). This means a tool's synchronous `invoke()` body already
   executes off the original calling thread, on a worker dedicated to that session — and, critically,
   **a real, in-tree provider call (`ChatClient::chat()`, called synchronously from inside
   `run_rounds()`) already blocks that worker thread for the duration of a real network round-trip,
   completely unremarked, today.** Making `recall(query)`'s embedding call do the same thing is not a
   new risk category for this codebase; it is the SAME pattern every other provider call already
   uses, because there is no real async I/O reactor anywhere in this codebase yet (`rt/task.hpp`'s
   own top comment: *"AgentEngine's own future executor (not yet built)"*).

**Historical caution, not a hypothetical:** this project's own history (Quark-era ADR-018, since
removed with Quark itself per ADR-037) already found a real reentrancy hazard in exactly this class
of problem — a coroutine resumed across the wrong context. `core/stream.hpp`'s own comment cites it
by name. This ADR treats "just resume() it" as a claim that must be proven per-conformer, not
asserted once and trusted forever — see Design B's `synchronous_leaf` trait below.

## 3. Competing designs

### Design A — Make `ToolDescriptor::invoke` (and the whole tool pipeline) genuinely async

`ToolDescriptor::invoke` becomes `task<result<json::Value>>`; `make_tool_descriptor<ToolT>()`/
`make_tool_descriptor_with_invoke()` wrap `co_await`-driven bodies; `invoke_tool()`/`background_task()`
(`tool_pipeline.hpp`) and every caller of `ToolCallRequest` — `rt/agent_session.hpp`'s
`resolve_interaction()`/`run_rounds()`, `protocol/mcp/server.hpp`'s `handle_tools_call`,
`protocol/a2a/server.hpp`, `src/backends/native_jail/mediated_python_runner.cpp`'s CodeAct tool
bridge — all become `co_await`-ing callers.

**Steelman:** this is the "eventually correct" shape. Every tool, present and future, gets a uniform
async contract; no per-conformer trust flag is ever needed; a genuinely async I/O reactor, whenever
this codebase builds one (`rt/task.hpp`'s own named future work), plugs in without another signature
change. It also directly closes the M2 Phase B decision's own "`ae::task<T>` deferred" debt instead
of routing around it again.

**Cost, measured, not guessed — corrected after red-team (§4 finding 5):** the original draft cited
"16 call sites... including `protocol/a2a/server.hpp`"; red-team independently grepped
`a2a/server.hpp` and found ZERO references to `invoke`/`ToolCall` — that specific citation was wrong
(it dispatches via `RunStarter`/`AgentSession`, not tool invocation directly). The corrected number,
from an unfiltered repo grep for `ToolCallRequest`/`make_tool_descriptor`, is **25+ real source/test
files** — an understatement in the original draft, not an overstatement; the rejection of Design A
on blast-radius grounds is, if anything, stronger than first stated. `protocol/mcp/server.hpp` and
`src/backends/native_jail/mediated_python_runner.cpp`'s CodeAct tool bridge remain the two real,
separately-conformance-gated (I7) surfaces this ADR has no mandate to re-open. This is exactly the
kind of "contested, hot-path... design" CLAUDE.md says needs its OWN `design → red-team → prove →
judge` pass, not a side effect of fixing one RAG tool's gap.

### Design B — A narrow, opt-in "leaf task" driver, reusing `ThreadPool`'s own proven idiom (RECOMMENDED)

Extract `ThreadPool::run_job()`'s `while (!done()) resume();` shape into a small, standalone,
reusable primitive in `agentengine::rt` — sketch, not final code, REVISED after red-team (§4 findings
1-3: bound corrected 64→1 with the reasoning made explicit, the double-wrapped `result<T>` bug named
and left to the call site to unwrap explicitly rather than hidden behind a misleading comment):

```cpp
namespace agentengine::rt {

// Drives `t` to completion via the SAME idiom ThreadPool::run_job() already uses in production
// (thread_pool.hpp) -- for a task<T> that is ALREADY running on a worker thread (no new thread, no
// new submission; this is an inline, same-thread drive, not a dispatch).
//
// SOUNDNESS IS A PER-CALLER CONTRACT, NOT TYPE-ENFORCED: `t` must never internally await anything
// except other task<T>/task<void> in a pure symmetric-transfer chain -- ideally, like
// OpenAIEmbedder::embed_batch() today, await NOTHING at all. Call sites must check the producing
// conformer's own `synchronous_leaf` declaration (below) before calling this; never call it against
// a conformer that declares `false`. Do NOT cite ThreadPool's own scope comment as proof this is
// safe in general -- red-team (§4 finding 1) found that comment does not hold for the code
// ThreadPool itself drives in production (AgentSession's session_mutex_.lock() genuinely suspends).
// synchronous_leaf carries a materially HIGHER review bar than this codebase's other declared traits
// (Capabilities<...>/EffectClass<...>): a wrong declaration here fails via undefined behavior
// (resuming a not-actually-ready coroutine handle), not a soft, contained error.
//
// BOUND IS 1, NOT A LARGER NUMBER -- deliberately, per red-team finding 2: under the stated contract
// (only nested task<T>/task<void> awaits, real C++20 symmetric transfer), a conforming leaf task
// reaches done() in EXACTLY ONE resume() call, regardless of nesting depth -- symmetric transfer
// never returns control to this loop until the whole chain completes or hits a genuinely-suspending
// non-task awaitable. There is no legitimate scenario where the iteration count should ever be 2, so
// a larger "safety margin" bound (the original 64 was wrong) does not add safety -- it only delays
// detecting a violated contract. IMPORTANT CAVEAT, stated plainly rather than overclaimed (red-team
// finding 2): this bound is a hang-preventer for an ALREADY-suspended, already-corrupted-state
// coroutine, not a soundness guarantee -- by the time the second resume() would fire,
// await_resume() on the foreign awaitable has already run against a precondition that was never
// satisfied (e.g. treating a mutex as acquired when it was not). The REAL protection is the
// synchronous_leaf review discipline; this bound only stops an already-bad situation from becoming
// an unbounded hang on top of everything else.
template <class T>
[[nodiscard]] result<T> drive_leaf_task(task<T> t) {
    if (!t.done()) t.resume();
    if (!t.done()) {
        return std::unexpected(error{failure_class::fatal,
            "drive_leaf_task() needed more than one resume() to reach done() -- the task suspended "
            "on something other than a nested task<T>/task<void>, violating its synchronous_leaf "
            "contract. The coroutine state from here on is not trustworthy (see this function's own "
            "comment) -- this error exists to stop cleanly, not to recover.",
            "rt.leaf_task_contract_violation"});
    }
    // take_value() rethrows a fault via std::rethrow_exception (a genuine C++ exception escaping the
    // coroutine body -- allocation failure, a bug, NOT the ordinary provider-error channel, which for
    // every task<result<U>> in this codebase already flows through the normal co_return/return_value
    // path as an ordinary std::unexpected(...) value, not a fault). Translated to ae::error here so a
    // caller never has to catch task<T>'s exception-based fault protocol directly.
    try {
        return t.take_value();
    } catch (...) {
        return std::unexpected(error{failure_class::transient,
                                      "leaf task faulted", "rt.leaf_task_faulted"});
    }
}

}  // namespace agentengine::rt
```

**Double-wrapped `result<T>` at the call site (red-team finding 3, real, must be handled explicitly,
not papered over):** every `Embedder::embed_batch()` already returns `task<result<U>>` (this
codebase's own convention: the task's OWN success/fault channel is for coroutine-level failure, the
ORDINARY provider error — a failed HTTP call, a bad response — flows through the `result<U>` value
itself). So `drive_leaf_task(embedder.embed_batch(...))` returns `result<result<U>>` — TWO layers,
each meaning something different, and a caller must unwrap both explicitly, not assume
`drive_leaf_task` flattens them:

```cpp
// Inside recall's invoke(), sketch:
auto driven = rt::drive_leaf_task(embedder_.embed_batch(query_batch, ctx));
if (!driven) return std::unexpected(driven.error());       // OUTER: task-level fault (rare) or a
                                                             // violated synchronous_leaf contract
auto& embedded = *driven;                                  // result<vector<vector<float>>>
if (!embedded) return std::unexpected(embedded.error());   // INNER: the ORDINARY provider error
                                                             // channel (network failure, bad
                                                             // response) -- the common failure case
// ... use *embedded ...
```

`core/embedder.hpp`'s `Embedder` concept grows one new, REQUIRED, explicit trait — a `static
constexpr bool synchronous_leaf` — matching this project's own "declared, not inferred" idiom (the
same posture `Capabilities<...>`/`EffectClass<...>` already use for tools, generalized to this
concept): a conformer must actively assert *"my `embed_batch()` never awaits anything but nested
`task<T>`/`task<void>`"* before `VectorRagContextProvider::recall()`'s `invoke` is allowed to call
`drive_leaf_task()` against it — checked with `if constexpr (EmbedderT::synchronous_leaf)` at the
call site; a conformer declaring `false` gets the EXISTING documented fail-closed error, unchanged,
no regression. `OpenAIEmbedder` declares `true` (justified by fact 3 above, re-audited any time its
body changes — this is the ONE place in this design where correctness still rests on human review,
same as it would under any design that doesn't rebuild the entire C++ coroutine-analysis toolchain).

**Migration checklist (red-team finding 4 — always-triggered, name it explicitly, not implicitly):**
adding a REQUIRED trait to `Embedder` breaks every existing conformer's own `static_assert(Embedder<
...>)` until it adds the one-line declaration. Confirmed by red-team as exactly 4 conformers in the
tree today, all genuinely leaf by inspection (no `co_await`, only `co_return`), so each is a true
one-line fix, not a design problem — but must actually be done, together, in the same change:
`OpenAIEmbedder` (`protocol/openai/embedder.hpp`, production), `MockEmbedder` in
`tests/test_corpus_source.cpp`, `MockEmbedder` in `tests/test_vector_rag_context_provider.cpp`, and
`AlternatingEmbedder` in `tests/test_vector_rag_context_provider.cpp`.

**Steelman:** smallest blast radius by a wide margin — touches `core/embedder.hpp` (one new trait),
a new ~20-line `rt/drive_leaf_task.hpp`, and `vector_rag_context_provider.hpp`'s `recall` `invoke`
closure (plus the 4-conformer migration checklist above). Zero change to `ToolDescriptor`, zero
change to the MCP/A2A/native-jail tool-dispatch surfaces, zero change to any OTHER tool in this
codebase. Its driving idiom (`resume()` until `done()`) is the same one `ThreadPool` already ships in
production — but, per red-team finding 1, this design does NOT lean on `ThreadPool`'s own scope claim
as proof of safety (that claim doesn't hold for `ThreadPool`'s own real usage today); it leans on the
STRONGER, independently-verified property that `OpenAIEmbedder::embed_batch()` awaits nothing at all
(fact 3), which no amount of nesting depth changes.

**Cost:** the safety property is a declared CONTRACT, not a compiler-checked one — `synchronous_leaf
= true` is only as correct as whoever wrote it and whoever reviews the next change to that
conformer's `embed_batch()` body, and per red-team finding 2, a WRONG declaration is caught only
AFTER the damage (a corrupted coroutine/protocol state), not before it — `drive_leaf_task()`'s
1-resume bound stops the situation from ALSO becoming an unbounded hang, nothing more. This design
accepts "fail loud, fast, and diagnosably, with the real protection being human review discipline
at declaration time" as the achievable bar, not "provably impossible to get wrong," and says so
plainly rather than overclaiming — the `synchronous_leaf` review bar is explicitly higher than this
codebase's other declared traits (`Capabilities<...>`/`EffectClass<...>`), where a wrong declaration
fails softly, not via undefined behavior.

### Design C — Declare `recall` `Backgroundable` (006 §6b), complementary to B, not exclusive

Use the ALREADY-BUILT `Backgroundable`/`background_task()` mechanism (006 §6b, Milestone 7 Phase B)
`ExecuteCodeTool` already uses for arbitrarily-long Python scripts: `recall`'s `invoke` still runs
synchronously (Design B or otherwise) but is dispatched off the model's own immediate turn, reporting
"pending" and completing later via the existing background-task-completion callback.

**Steelman:** the right tool for a GENUINELY long operation — a large corpus, a slow provider, a
multi-sub-batch embed. Reuses proven machinery instead of inventing a second one.

**Why NOT the primary fix for `recall` specifically:** `recall(query)` embeds exactly ONE query
string, one HTTP round-trip — the same cost shape as the `ChatClient::chat()` call every turn already
makes synchronously today (fact 4 above). Backgrounding a sub-second call trades a small, bounded
wait for a strictly worse model-facing UX (a "check back later" tool result instead of an answer) for
no real benefit in the common case. **Recommended as a follow-on, not a replacement**: expose
`Backgroundable` as a caller-configurable OPTION on `VectorRagContextProvider` for deployments with
slow/rate-limited embedding backends, layered on top of Design B's actual driving mechanism, not
instead of it.

### Design D — Build a real async executor / cross-thread coroutine parking (deferred, not attempted here)

The "fully general" answer: a real reactor that can suspend a coroutine on genuine external I/O and
resume it later, safely, from any thread. `rt/thread_pool.hpp`'s own top comment already names this
as *"a genuinely separate, harder problem... needs its own dedicated design → red-team → prove pass
before anything depends on it."* Out of scope for this ADR, same as `GraphRagContextProvider` was
named-not-designed in ADR-063 §7 — naming it here so it is not silently forgotten, not attempting it.

## 4. Red-team (2026-08-19, `general-purpose` agent, no prior context)

Verified §2's 4 facts independently (not trusting this ADR's own citations) by reading the actual
source, then probed the specific questions this ADR's own first draft named. Findings:

**CRITICAL 1 — `ThreadPool`'s "narrow scope" claim is not actually true of the code it drives today;
it is safe only because production always uses exactly one worker.** `AgentSession::start_run()`/
`resolve_interaction()` (`rt/agent_session.hpp:585,647`) begin with `co_await
session_mutex_.lock()`. `AsyncMutex::LockAwaiter::await_suspend` (`rt/async_mutex.hpp:129-142`)
**genuinely suspends** when contended: it parks the coroutine handle in a `waiters_` deque, to be
resumed only by a *different* logical flow's `unlock()` call later — not a `task<T>` symmetric-
transfer chain, exactly the "external suspension" `ThreadPool`'s own comment disclaims. Same shape in
`rt/channel.hpp`'s `next_awaiter::await_suspend` (`channel.hpp:371-383`). If `session_mutex_` is ever
genuinely contended, the naive `while (!done()) resume();` loop would call `resume()` on a parked
`LockAwaiter` handle, running `await_resume()` (which unconditionally constructs a `Guard`) as if the
mutex had been handed over when it hadn't — a real double-holder bug — and the legitimate `unlock()`
later calling `.resume()` on that SAME handle again is a genuine coroutine-handle double-resume, UB
per the C++20 spec. **This is real, in-tree, reachable in principle — masked only because
`tools/cli_chat.cpp` always constructs `ThreadPool pool(1)` with strictly serial job draining, so
contention has not been observed to occur in the one deployment that exists.** This is a
**pre-existing, latent bug in `ThreadPool`+`AsyncMutex`+`AgentSession`, independent of this ADR** —
flagged prominently at the top of §7, not silently absorbed into a residual bullet. It does NOT
invalidate Design B's application to `OpenAIEmbedder` specifically (which rests on the STRONGER,
independently-verified "awaits nothing at all" property, fact 3 — unaffected by this finding), but it
means the "load-bearing precedent" framing needed correcting, which §2/§3 above now do.

**CRITICAL 2 — the original `kMaxDriveIterations = 64` bound was miscalibrated and does not catch
the real failure mode before damage.** Under the stated leaf-task contract, real C++20 symmetric
transfer means a conforming task reaches `done()` in EXACTLY ONE `resume()` call, regardless of
nesting depth — there is no legitimate scenario where the count should approach even 2, let alone 64.
Worse: the failure doesn't wait for the bound — the SECOND `resume()` call already runs
`await_resume()` against an unsatisfied precondition on whatever foreign awaitable the task actually
parked on, corrupting state (per finding 1's mechanism) before the loop gets anywhere near a large
bound. **Fixed in §3 above**: bound corrected to 1, with the ADR now stating plainly that this bound
prevents an unbounded HANG on top of an already-bad situation, not the corruption itself — the real
protection is the `synchronous_leaf` declaration's own review discipline.

**REAL GAP 3 — Design B's original sketch double-wraps `result<T>`** when applied to
`embed_batch()`'s actual return type (`task<result<std::vector<std::vector<float>>>>`), producing
`result<result<...>>` — inconsistent with this codebase's flat `result<T>` convention, and the
original sketch's comment obscured this rather than showing it. **Fixed in §3 above**: the two layers
(task-level fault vs. the embedder's own ordinary error channel) are now named explicitly, with a
usage sketch showing both must be unwrapped at the call site.

**REAL GAP 4 — the new REQUIRED `synchronous_leaf` trait breaks all 4 existing `Embedder` conformers
in the tree today** (each pinned by its own `static_assert(Embedder<...>)`): `OpenAIEmbedder`
(production), `MockEmbedder` in `tests/test_corpus_source.cpp`, `MockEmbedder` and
`AlternatingEmbedder` in `tests/test_vector_rag_context_provider.cpp`. All four are, by inspection,
genuinely leaf — a true one-line fix each — but this always-triggered migration was not named as a
checklist item in the original draft. **Fixed in §3 above.**

**REAL GAP 5 — one of Design A's own cited call sites was wrong.** `protocol/a2a/server.hpp`,
originally cited as needing a `co_await`-shape change, contains zero references to
`invoke`/`ToolCall` (independently grepped — it dispatches via `RunStarter`/`AgentSession`, not tool
invocation directly). The real, unfiltered count is 25+ files, not 16 — an understatement in the
original draft, not an overstatement, so Design A's rejection is if anything stronger than first
argued. **Fixed in §3 above.**

**MINOR 6 — Design C's "recall is sub-second" claim is asserted by analogy to `ChatClient::chat()`'s
round-trip, not independently measured.** No benchmark evidence of either call's real latency exists
yet. Not a defect — the ADR already listed this as pending rather than claiming it proven — restated
here so it isn't lost.

**MINOR 7 — `synchronous_leaf` carries a materially higher review bar than this codebase's other
declared traits** (`Capabilities<...>`/`EffectClass<...>`), since a wrong declaration there fails via
UB rather than a soft, contained error. The ADR should say this plainly rather than imply parity with
those other traits. **Fixed in §3 above.**

**Checked, no issue found — thread-safety of `drive_leaf_task`/a shared `Embedder` instance across
concurrent sessions.** `session_mutex_` already serializes all tool dispatch within one session (I1),
and `OpenAIEmbedder::embed_batch()` has no shared mutable state (`store_` is a `const&`, everything
else local) — sharing one instance across sessions/workers is independently safe.

**Red-team's own bottom line:** Design B's application to the one real conformer that exists today
(`OpenAIEmbedder`) is sound and holds up under independent verification. The design's safety argument
originally leaned on two things that didn't hold as first stated — `ThreadPool`'s precedent, and the
64-iteration bound — both now corrected above. None of the findings overturn the recommendation of
Design B; they tighten it.

## 5. Executed evidence

**Not yet gathered.** §4 was a design-level (no implementation) red-team pass — real code, tests, and
measurements (mirroring ADR-063's own two-implementation-pass shape) are the next step if this ADR
proceeds, not yet done here.

## 6. Per-claim verdicts

Per `decisions/README.md`'s bar — decided by observed output, not argument. Only claims with real,
executed evidence get CORRECT/WRONG; everything else stays honestly PENDING/INCONCLUSIVE.

- **§2 facts 1, 3, 4 (task<T>'s direct-driving API exists; `OpenAIEmbedder::embed_batch()` awaits
  nothing; production already runs tool `invoke()` on one worker thread and blocks it for real
  network calls) — CORRECT, independently re-verified by red-team against the real source**, not
  merely re-stated from the original draft.
- **§2 fact 2 as ORIGINALLY stated ("ThreadPool's scope claim holds in general") — WRONG.** Red-team
  found a real counter-example in `ThreadPool`'s own actual production usage (finding 1). The
  CORRECTED claim in §2/§3 above (Design B rests on `OpenAIEmbedder`'s stronger "awaits nothing"
  property, not on `ThreadPool`'s scope claim) has not itself been executed/tested yet — INCONCLUSIVE
  pending §5.
- **Design B's `kMaxDriveIterations` claim — the original ("64, a generous safety margin") was
  WRONG**, per finding 2's symmetric-transfer reasoning (a real, argued disproof, not yet a compiled
  test). The corrected claim ("exactly 1 for any conforming leaf task") is argued, not yet proven by
  an executed test — INCONCLUSIVE pending §5.
- **Design A's blast-radius claim — the specific `protocol/a2a/server.hpp` citation was WRONG**
  (finding 5); the overall conclusion (large blast radius, reject for this ADR's scope) is CORRECT
  and, if anything, understated in the original draft.
- **Everything else (whether `drive_leaf_task()` as revised actually compiles and behaves as
  designed; whether the 4-conformer migration is complete and green; whether `recall`'s real
  end-to-end invoke works) — PENDING**, no implementation exists yet to test.

## 7. The decision — not made; this is the proposal awaiting the loop

**✅ FIXED (2026-08-19), separately from this ADR's own scope.** §4 finding 1's `ThreadPool`+
`AsyncMutex`+`AgentSession` reentrancy/UB hazard has been fixed directly in `rt/thread_pool.hpp`
(not gated on this ADR's own Design B proceeding): `ThreadPool::run_job()` now resumes a job exactly
once and fails it loudly (a diagnosable `JobOutcome::faulted`, `rt.leaf_task_contract_violation`-
shaped message) instead of looping `resume()` if it isn't `done()` yet — safe by construction now,
not merely by luck of `tools/cli_chat.cpp` always using `ThreadPool pool(1)`. New
`tests/test_rt_thread_pool.cpp` T6 proves a job that genuinely contends an `AsyncMutex` fails cleanly
AND that the mutex is unharmed afterward (the abandoned job's `destroy()` correctly self-removes from
the waiter queue, reusing `AsyncMutex`/`channel<T>`'s own already-proven cancellation-safety per
ADR-017's "drop the handle = cancel" idiom). Documented as an addendum to `decisions/ADR-037-remove-
quark-as-core-runtime.md`'s own index row (the executed ADR whose own red-team predicted exactly this
risk class). Full suite 194/194 (`ctest -LE live-network`), zero regressions. This fix is
independent of whether Design B (below) is ever implemented — it hardens `ThreadPool` itself, for
every current and future caller, not just `recall`/`Embedder`/RAG.

**What this document recommends, after one red-team pass:** Design B (the revised
`rt::drive_leaf_task()` with a 1-resume bound, an explicit `Embedder::synchronous_leaf` trait with an
honestly-stated higher review bar, the double-wrap made explicit at the call site, and the 4-conformer
migration checklist), with Design C's `Backgroundable` option layered on top as a documented follow-on
for slow-backend deployments, and Design A/D explicitly named as the longer-term direction this ADR is
NOT attempting. **Do not implement against this document yet** — one red-team pass on the DESIGN is
real progress, not a finish line; per `decisions/README.md`, §5 (executed evidence: real code, real
tests, a real build) and §6 (verdicts decided by observed output) still need to exist before this
moves toward Judged. The next step, if this proceeds, is implementing Design B for real and proving
it — mirroring ADR-063's own design → red-team → implement → red-team-again shape — not skipping
straight to Judged on the strength of one clean design review.
