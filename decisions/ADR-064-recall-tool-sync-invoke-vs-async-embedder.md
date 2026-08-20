# ADR-064 — Can a synchronous `ToolDescriptor::invoke` safely reach `Embedder::embed_batch()`'s `task<T>`?

**Status:** Judged (2026-08-19, project owner sign-off). Design B implemented and proven (2026-08-19).
Designed, then red-teamed once (§4, `general-purpose` agent, no prior context) — found
2 critical + 3 real-gap + 2 minor findings against the design, **plus one real, pre-existing, latent
bug in already-shipped code** (`ThreadPool`+`AsyncMutex`+`AgentSession` — see §4 finding 1). Design B
revised in place below to fix the findings that are fixable within this design (bound corrected 64→1,
double-wrapped `result<T>` fixed, migration checklist added, citations corrected). **The
separately-flagged `ThreadPool` bug was FIXED for real** (§7 top — `rt/thread_pool.hpp`,
`tests/test_rt_thread_pool.cpp` T6), independent of this ADR's own Design B. **Design B itself is now
implemented and proven** — `rt/drive_leaf_task.hpp` (new), `Embedder::synchronous_leaf`
(`core/embedder.hpp`), all 4 real conformers migrated, `VectorRagContextProvider::recall`'s real
`invoke`, a new `tests/test_rt_drive_leaf_task.cpp` (D1-D5), and real end-to-end coverage in
`tests/test_vector_rag_context_provider.cpp` (R8/R8b/R8c) — see §5/§6. **A second red-team pass
against this real implementation (§5's own subsection) found no blocking defect** — one real
`resume()`/try-catch asymmetry fixed, one comment's own justification corrected (and now backed by a
new test, R12, driving `recall` through the actual `shared_ptr`-backed wiring), one overclaimed
thread-safety line narrowed (`IndexT` sharing named as a real-but-unreachable residual), and one more
real scenario covered (R13: `recall` against an empty index). **Both of red-team pass 2's own
initially-named-but-not-closed test gaps were closed the same day**: R14 (a stale, unbacked index
entry reached through `recall`'s invoke is silently skipped, not a whole-call failure) and R15 (8
real threads calling `recall`'s invoke concurrently against the SAME provider instance all succeed
correctly, stable across 5 repeated runs — scope-limited to no concurrent WRITER, which remains a
real, named, currently-unreachable residual). Full suite **195/195** (`ctest -LE live-network`), zero
regressions.

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
else local) — sharing one instance across sessions/workers is independently safe. **Scope correction
(red-team against the real implementation, 2026-08-19): this checked only `Embedder`, not `IndexT`.**
`VectorRagContextProvider` takes its index by reference specifically so it CAN be shared across
provider instances (this file's own constructor comment); `BruteForceCosineIndex`
(`core/vector_index.hpp`) had zero internal synchronization at the time of this finding. If a shared
index were ever concurrently WRITTEN by a future ingestion process (`CorpusSource`, still not built
per ADR-063 §2.4A) while a live session's `recall()` concurrently called `index_->search()`, that
would have been a genuine data race — not reachable then (no concurrent-writer ingestion path existed
in the tree), a real, named residual left open at the time. **CLOSED the same day, in a follow-up
after this ADR's initial "Judged" sign-off, at the project owner's explicit request**: rather than
leave this a permanently-deferred "unreachable so far" note, `BruteForceCosineIndex` gained a real
`std::shared_mutex` (`add_batch()` takes a unique/writer lock; `search()`/`contains()`/`size()` take a
shared/reader lock) — see §5/§6/§7 below for the executed proof (a real interleaved writer+4-readers
stress test, 200 writes, zero corrupted reads, stable across 5 runs). This paragraph's finding is
preserved as the historical record of what red-team pass 2 found; it is no longer this ADR's current
state.

**Red-team's own bottom line:** Design B's application to the one real conformer that exists today
(`OpenAIEmbedder`) is sound and holds up under independent verification. The design's safety argument
originally leaned on two things that didn't hold as first stated — `ThreadPool`'s precedent, and the
64-iteration bound — both now corrected above. None of the findings overturn the recommendation of
Design B; they tighten it.

## 5. Executed evidence

**Gathered (2026-08-19).** Design B implemented for real, exactly as revised in §3:

- `rt::drive_leaf_task<T>()` (`include/agentengine/rt/drive_leaf_task.hpp`, new file) — the 1-resume-
  bound driver, verbatim to the revised sketch.
- `Embedder`'s new REQUIRED `synchronous_leaf` trait (`core/embedder.hpp`) — checked via `{
  T::synchronous_leaf } -> std::convertible_to<bool>;` in the concept itself, so a conformer that
  omits it fails `static_assert(Embedder<...>)` at compile time, not silently.
- All 4 real conformers in the tree migrated, each a genuine one-line addition as predicted (§4
  finding 4): `OpenAIEmbedder` (`protocol/openai/embedder.hpp`, `synchronous_leaf = true`, justified
  by its body never awaiting anything, fact 3), `MockEmbedder` in `tests/test_corpus_source.cpp`,
  `MockEmbedder` and `AlternatingEmbedder` in `tests/test_vector_rag_context_provider.cpp` (all three
  `true`, each genuinely leaf — `co_return` only — by inspection).
- `VectorRagContextProvider::recall`'s `invoke` (`core/vector_rag_context_provider.hpp`) now branches
  on `if constexpr (EmbedderT::synchronous_leaf)`: the `true` path drives `embedder_.embed_batch()`
  via `drive_leaf_task()`, unwraps both `result<T>` layers explicitly (per §3's revised sketch),
  searches `index_`, and renders results through the SAME `render_scored_chunk()` helper
  `on_context()` already used (the file's own pre-existing comment had already named this as the
  intended shared point). The `false` path is the ORIGINAL fail-closed `failure_class::contract`
  error, byte-identical in spirit, so a conformer that cannot safely support this path regresses to
  nothing worse than before.
- `make_recall_tool_descriptor()` changed from `const` to non-`const` (a real, necessary consequence
  of capturing `this` to reach `embedder_.embed_batch()` — none of this codebase's `Embedder`
  conformers except `OpenAIEmbedder` declare `embed_batch` `const`, so a `const`-captured `this` would
  have broken every mock). Capturing `this` (rather than individual members by value, `MemoryProvider`'s
  own pattern) is sound here because every real caller stores this provider at a stable heap address
  for its whole lifetime before ever calling `on_context()` (`context_assembly.hpp::
  make_context_provider_descriptor()`'s own `std::make_shared<ProviderT>`) — documented at the capture
  site, not merely asserted in this ADR.

**Tests, both new and extended:**

- `tests/test_rt_drive_leaf_task.cpp` (NEW, 10 checks, D1-D5): D1 a conforming leaf task drives in
  exactly one `resume()`, both `result<T>` layers unwrap to the real value; D2 the double-wrap is
  preserved (outer succeeds, inner carries the leaf's own ordinary `std::unexpected` error) — not
  flattened; D3 a genuine C++ exception thrown inside the leaf task's body maps to a DISTINCT outer
  `rt.leaf_task_faulted` error, proving the two failure channels (D2 vs D3) are not conflated; D4 a
  leaf task that itself `co_await`s another nested `task<T>` (real symmetric transfer) still resolves
  in exactly one `resume()`, regardless of nesting depth, matching §3's own claim; D5 — reproducing
  §4 finding 1's exact hazard, but directly against `drive_leaf_task()` itself rather than only
  through `ThreadPool` — a task genuinely parked on a contended `AsyncMutex::lock()` is reported as a
  violated `synchronous_leaf` contract (`rt.leaf_task_contract_violation`), and the abandoned
  contender's destruction does not corrupt the mutex for later legitimate use (a third, later lock on
  the same mutex still succeeds cleanly).
- `tests/test_vector_rag_context_provider.cpp` — R8/R8b/R8c rewritten from the old fail-closed-stub
  assertion to real end-to-end coverage: `recall()` invoked with well-formed args against
  `MockEmbedder` (`synchronous_leaf = true`) now SUCCEEDS, returning both corpus chunks ranked by the
  query's own embedding, carrying the identical citation-label + tainted-content-neutralization
  discipline `on_context()`'s own default injection already proves (R8c directly checks for the same
  citation marker bytes) — proving `render_scored_chunk()` really is shared, not reimplemented. R9
  (malformed args) unchanged in intent, still confirms schema validation runs before any
  embedder/index work.
- Full build: clean (MSVC/Debug, this session's toolchain) — zero new warnings.
- Full suite: **195/195 passed, 0 failed** (`ctest -LE live-network`), one new test target added
  (`test_rt_drive_leaf_task`), zero regressions against the pre-existing 194.

**Not measured in this pass, CLOSED in a follow-up the same day** (named, not silently skipped): the
`synchronous_leaf = false` fallback path was exercised only by the existing byte-for-byte-preserved
error message/code, not by a NEW conformer that declares `false` end-to-end through `recall`'s
`invoke` — the 4 real conformers in the tree today all declare `true`, so there was no
`false`-declaring conformer in the tree to drive that branch through a real `if constexpr`
instantiation. **Closed**: a new, dedicated `NonLeafEmbedder` test conformer
(`tests/test_vector_rag_context_provider.cpp`) declares `synchronous_leaf = false`; R16 drives
`recall`'s `invoke` against it and confirms the fail-closed fallback behaves exactly as designed
through a real instantiation, not merely a compile check.

### Red-team pass 2 (2026-08-19, `general-purpose` agent, no prior context, against the REAL
implementation, not the paper design §4 already covered)

Read every file this pass touched (`rt/drive_leaf_task.hpp`, `core/embedder.hpp`,
`core/vector_rag_context_provider.hpp`, `protocol/openai/embedder.hpp`, `context_assembly.hpp`,
`composed_context_provider.hpp`, `rt/thread_pool.hpp`, `rt/task.hpp`, `rt/agent_session.hpp`'s three
real `invoke_tool()` call sites, both test files) end-to-end by tracing actual code, not re-reading
this document's own claims. Findings:

**REAL, FIXED — `drive_leaf_task()` didn't wrap `resume()` in try/catch, an unexplained deviation from
`ThreadPool::run_job()`, the idiom this function's own top comment claims to reuse.** `run_job()`
(`rt/thread_pool.hpp`) wraps its whole body, including `resume()`, in try/catch as explicit
"defense-in-depth... against a future change to `task<T>`'s contract or misuse this type can't
statically rule out." The first version of `drive_leaf_task()` wrapped only `take_value()`. Verified
against `task<T>::promise_type::unhandled_exception()` (`rt/task.hpp`, `noexcept`, never rethrows)
that this was not currently exploitable — `resume()` cannot throw from a body exception today — but
the asymmetry was real and unexplained, not a deliberate, argued choice. **Fixed**: the whole drive
(`resume()` through `take_value()`) is now one try/catch, matching `run_job()`'s shape exactly.

**NAMED, NOT A BUG — `synchronous_leaf` is a purely declarative, non-type-enforced trust boundary.**
No portable C++20/23 mechanism exists to statically verify a coroutine body's suspension behavior, so
this cannot be meaningfully strengthened within the type system — both this file and `core/
embedder.hpp`'s own comment are already honest that a lying declaration produces real UB. Not a defect;
restated here so it isn't lost as a residual review-discipline dependency, not a solved problem.

**REAL, NARROWED — the ADR's own "Checked, no issue found" thread-safety line (§4) read broader than
what was actually checked.** It verified `Embedder`/`OpenAIEmbedder` only, saying nothing about
`IndexT` sharing. **Fixed above** (this section, the paragraph immediately preceding this
subsection) — narrowed to name the real, currently-unreachable `IndexT` concurrent-write residual
explicitly rather than let the original phrasing imply a broader guarantee than was checked.

**REAL, FIXED — an imprecise justification in `make_recall_tool_descriptor()`'s own comment.** The
comment claimed "every real caller stores this provider... via `make_shared<ProviderT>`," but one of
the two real wiring shapes in this tree (a provider plugged in directly as an `AgentSession`/composite
provider's own template-parameter member) doesn't go through `make_shared` at all — it's still safe,
for a different reason (a stable member-subobject address), which the comment didn't name. The
underlying CONCLUSION (capturing `this` is sound) was independently re-traced end-to-end by red-team
and confirmed correct for both real shapes — **only the comment's own reasoning was incomplete, not
the design**. **Fixed**: the comment now names both wiring shapes explicitly.

**REAL, TEST GAP, NOW CLOSED — no test exercised `recall`'s `invoke` through the actual
`make_shared<ProviderT>`-backed path the comment specifically cites, only through a still-in-scope
stack-local `Provider`.** New `tests/test_vector_rag_context_provider.cpp` R12 moves a provider into a
real `ContextProviderDescriptor` (`context_assembly.hpp::make_context_provider_descriptor()`) BEFORE
calling `on_context()`/`recall`, proving the captured `this` really does follow the provider into
shared_ptr-managed heap storage (the original stack-local `provider` is moved-from and out of scope by
the time `recall`'s `invoke` runs) rather than pointing at a stale address.

**REAL, TEST GAP, NOW CLOSED — no test drove `recall`'s `invoke` against a genuinely empty index**
(on_context()'s own R10 only covered an embedder FAILURE, a structurally different scenario from a
successful call that simply finds nothing). New R13 proves an empty index still returns a clean, empty
`results: []` reply through `recall`'s real invoke path, not an error.

**Follow-up (2026-08-19, same day): both named gaps above CLOSED with real tests.**

- **Stale index entry, closed by R14.** A chunk id added directly to the index with NO matching
  `CorpusChunkRecord`/blob ever written (the same end state a re-mount that dropped a chunk without
  reconciling the index would leave — ADR-063 §7's named lifecycle gap), alongside one real, present
  chunk. `recall`'s invoke still succeeds, returning exactly the one real chunk and silently skipping
  the stale entry — `render_scored_chunk()`'s best-effort skip-on-miss posture (`on_context()`'s own
  design) genuinely holds through the `recall` invoke path too, not just `on_context()`, now proven,
  not merely inherited by construction.
- **Concurrent `recall()` calls, closed by R15.** Before writing this test, independently verified
  (not assumed) that every read path `recall`'s invoke touches under contention — `InMemoryWorktree
  ObjectStore::get_blob()`/`get_tree()` (plain `unordered_map` lookups, no mutex, no mutable cache),
  `InMemoryAppendLogStore`'s own read path (already internally `mutex`-guarded — safe even against a
  concurrent WRITER, not just readers), and `BruteForceCosineIndex::search()` (pure read, no internal
  mutable state) — are each safe for concurrent reads with no writer present, which is exactly this
  scenario (no ingestion path exists in the tree today, so no writer is ever actually concurrent with
  `recall()` in production either). R15 then drives 8 real `std::thread`s calling `recall`'s invoke
  concurrently against the SAME `VectorRagContextProvider` instance, using a purpose-built
  `StatelessEmbedder` (zero mutable member state — this file's other mocks deliberately mutate their
  own state for scripted single-threaded determinism and would race on THEIR OWN state if driven
  concurrently, which would test the wrong thing). All 8 threads return the correct result; stable
  across 5 repeated runs (no observed flake).
- Both proofs live in `tests/test_vector_rag_context_provider.cpp`. Full suite: **195/195**
  (`ctest -LE live-network`), zero regressions — R14/R15 are new checks inside the existing target,
  not new `ctest` targets, so the target count is unchanged.
- **Open at the time of this follow-up, CLOSED by a later same-day follow-up (see §6's `IndexT`
  verdict and §7):** `IndexT` sharing under a genuinely CONCURRENT WRITER was, at this point,
  correctly left as a real, named, currently-unreachable residual (no writer exists in the tree yet)
  rather than exercised. Superseded, not merely restated, below — `BruteForceCosineIndex` gained a
  real `std::shared_mutex`, making this case safe by construction rather than merely unreachable.

**Checked, no other issue found:** the double-wrapped `result<T>` unwrap in `recall`'s `invoke` body
matches `on_context()`'s equivalent unwrap and `drive_leaf_task()`'s own documented contract exactly,
no wrong-branch bug; `render_scored_chunk()`'s const-correctness and identical
capability/mount/object-store arguments across both call paths; the 1-resume bound's correctness,
re-verified directly against `task<T>`'s actual `await_suspend`/`FinalAwaiter` symmetric-transfer
implementation, not merely re-argued; `OpenAIEmbedder::synchronous_leaf = true`'s justification,
re-verified structurally (`perform_provider_https_exchange` returns `result<T>` directly, never a
`task`); the `k=10` hardcoded recall max-results, a reasonable, deliberately independent judgment call
from `max_injected_`; cross-session `EffectContext` safety (each `AgentSession` owns its own instance,
no structural sharing exists independent of `recall`); both new/extended test files' assertions are
real and specific, not tautological.

**Red-team pass 2's own bottom line:** Design B's real implementation holds up — the core soundness
argument (symmetric transfer bounding `drive_leaf_task` to one `resume()`, `OpenAIEmbedder` genuinely
never suspending, the double-wrapped `result<T>` unwrapped correctly, `this`-capture lifetime backed
by real ownership with no path to invoke a dangling closure) is correct by direct code tracing, not
just by this document's own say-so. Nothing found was a blocking defect; the fixes above tighten an
already-sound implementation rather than repair a broken one.

## 6. Per-claim verdicts

Per `decisions/README.md`'s bar — decided by observed output, not argument.

- **§2 facts 1, 3, 4 (task<T>'s direct-driving API exists; `OpenAIEmbedder::embed_batch()` awaits
  nothing; production already runs tool `invoke()` on one worker thread and blocks it for real
  network calls) — CORRECT, independently re-verified by red-team against the real source**, not
  merely re-stated from the original draft.
- **§2 fact 2 as ORIGINALLY stated ("ThreadPool's scope claim holds in general") — WRONG.** Red-team
  found a real counter-example in `ThreadPool`'s own actual production usage (finding 1). The
  CORRECTED claim (Design B rests on `OpenAIEmbedder`'s stronger "awaits nothing" property, not on
  `ThreadPool`'s scope claim) is now **CORRECT, executed**: `test_rt_drive_leaf_task.cpp` D1/D4 prove
  a conforming leaf task (including one with nested `task<T>` composition) drives cleanly through
  `drive_leaf_task()` on this exact property, independent of `ThreadPool`.
- **Design B's `kMaxDriveIterations`/bound claim — the original ("64, a generous safety margin") was
  WRONG**, per finding 2's symmetric-transfer reasoning. The corrected claim ("exactly 1 resume() for
  any conforming leaf task, regardless of nesting depth") is now **CORRECT, executed**:
  `test_rt_drive_leaf_task.cpp` D1 (flat) and D4 (nested) both drive to completion in the single
  `resume()` `drive_leaf_task()` actually performs — the implementation has no loop to prove this
  against, so the executed test is direct confirmation, not an inference.
- **Design A's blast-radius claim — the specific `protocol/a2a/server.hpp` citation was WRONG**
  (finding 5); the overall conclusion (large blast radius, reject for this ADR's scope) is CORRECT
  and, if anything, understated in the original draft.
- **`drive_leaf_task()`'s contract-violation detection (§4 finding 1's exact hazard, reproduced
  directly rather than only through `ThreadPool`) — CORRECT, executed**: `test_rt_drive_leaf_task.cpp`
  D5 drives a task genuinely parked on a contended `AsyncMutex::lock()` and confirms both the
  diagnosable `rt.leaf_task_contract_violation` error AND that the abandoned contender's destruction
  does not corrupt the mutex for later legitimate use — the two halves of the claim, both checked, not
  just asserted.
- **The double-wrapped `result<T>` shape (§3, finding 3) — CORRECT, executed**: D2 confirms the outer
  layer succeeds (task-level completion) while the inner layer independently carries the leaf's own
  `std::unexpected` error, unflattened; D3 confirms a genuine thrown exception surfaces through the
  OUTER layer instead (`rt.leaf_task_faulted`) — the two failure channels are observably distinct, not
  merely documented as distinct.
- **The 4-conformer `synchronous_leaf` migration (finding 4) — CORRECT, executed**: all 4 compile
  (`static_assert(Embedder<...>)` passes for each), each a genuine one-line addition as predicted.
- **`recall`'s real end-to-end invoke against a `synchronous_leaf` `Embedder` — CORRECT, executed**:
  `test_vector_rag_context_provider.cpp` R8/R8b/R8c drive a real embed + search + citation-rendered
  reply through the actual `ToolDescriptor::invoke` closure, not a hand-constructed shortcut. R9
  confirms schema validation (reject-not-coerce) still runs before any embedder/index work.
- **The `synchronous_leaf = false` fallback path (unchanged fail-closed behavior for a non-leaf
  conformer) — CORRECT, executed (closed in a same-day follow-up).** A new, dedicated
  `NonLeafEmbedder` test conformer (`synchronous_leaf = false`) drives `recall`'s `invoke` through a
  REAL `if constexpr` false-branch instantiation for the first time (R16) — the documented, stable
  error code fires exactly as designed, not merely proven to compile.
- **`this`-capture lifetime safety in `make_recall_tool_descriptor()` (red-team pass 2) — CORRECT,
  executed.** New R12 drives `recall`'s `invoke` through the REAL `make_shared<ProviderT>`-backed
  `ContextProviderDescriptor` wiring (the specific path the code comment cites), with the original
  stack-local provider already moved-from and out of scope — the captured `this` still resolves to
  live data, not a stale address. The comment's own justification text was independently found
  incomplete (it named only one of two real wiring shapes) and has been corrected; the underlying
  safety conclusion was unaffected and is now backed by an executed test, not argument alone.
- **`recall`'s invoke against a genuinely empty index (red-team pass 2) — CORRECT, executed.** New R13
  confirms a clean, empty `results: []` reply, not an error — a real, reachable scenario (a
  freshly-mounted, not-yet-ingested corpus) no prior test drove through the invoke path.
- **`IndexT` thread-safety under a shared, concurrently-written index (red-team pass 2) — CORRECT,
  executed, closed for real (same-day follow-up), not merely narrowed.** No concurrent-writer
  ingestion path exists in the tree today (ADR-063 §2.4A's `CorpusSource` is not built) — but rather
  than leave this a permanently-deferred residual, `BruteForceCosineIndex` (`core/vector_index.hpp`)
  gained a real `std::shared_mutex` (`add_batch()` takes a unique/writer lock; `search()`/
  `contains()`/`size()` take a shared/reader lock), proven by a new `test_vector_index.cpp` block
  with one genuine writer thread interleaved with 4 reader threads (200 writes, zero corrupted
  reads, stable across 5 repeated runs). This is now correct BY CONSTRUCTION for whenever a real
  concurrent-writer `CorpusSource` does land, not merely "safe because unreachable today."
- **`recall`'s invoke against a stale (unbacked) index entry (red-team pass 2's own named residual) —
  CORRECT, executed.** New R14 (§5 follow-up) confirms `render_scored_chunk()`'s best-effort
  skip-on-miss posture genuinely holds through `recall`'s invoke path, not just `on_context()`.
- **Concurrent `recall()` calls against the SAME provider instance, originally proven only for the
  no-writer case (red-team pass 2's own named residual) — CORRECT, executed, and no longer
  scope-limited.** R15 (§5 follow-up) drives 8 real threads through `recall`'s invoke concurrently
  with no writer present, all returning the correct result, stable across 5 repeated runs. The
  concurrent-WRITER case this originally excluded is now ALSO covered — not by R15 itself, but by
  `BruteForceCosineIndex`'s own new `std::shared_mutex` (the `IndexT` verdict immediately above),
  which makes any concurrent `recall()` reader safe against a concurrent index writer by
  construction, independent of which specific test exercises which specific interleaving.

## 7. The decision — Judged (2026-08-19, project owner sign-off)

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
risk class). Full suite 194/194 (`ctest -LE live-network`) at the time of that fix, zero regressions.
This fix is independent of Design B — it hardens `ThreadPool` itself, for every current and future
caller, not just `recall`/`Embedder`/RAG.

**Design B is implemented and proven (2026-08-19).** `rt::drive_leaf_task<T>()`
(`rt/drive_leaf_task.hpp`), the required `Embedder::synchronous_leaf` trait (`core/embedder.hpp`), the
4-conformer migration, and `VectorRagContextProvider::recall`'s real `invoke` implementation
(`core/vector_rag_context_provider.hpp`) are all real, compiled, tested code — see §5 for the full
evidence and §6 for per-claim verdicts. `recall(query)` is now genuinely invocable end-to-end for any
`Embedder` conformer that declares `synchronous_leaf = true` (all 4 real conformers in the tree today
do); a conformer declaring `false` still gets the original, byte-identical fail-closed error.

**A second red-team pass, against this real implementation (not the paper design §4 already covered),
found no blocking defect** — see §5's "Red-team pass 2" subsection for the full findings. One real gap
was fixed (`drive_leaf_task()` now wraps `resume()` in the same try/catch as `take_value()`, matching
`ThreadPool::run_job()`'s own defense-in-depth exactly, closing an unexplained deviation from the
idiom it claims to reuse); one comment's own justification was corrected (the `this`-capture safety
argument named only one of two real wiring shapes — fixed, and now backed by a new test, R12, that
drives `recall` through the specific `make_shared<ProviderT>`-backed path the comment cites); one
overclaim was narrowed (§4's original blanket thread-safety line covered `Embedder` only, not `IndexT`
— narrowed to name a real-but-currently-unreachable `IndexT` concurrent-write residual explicitly);
and one more real, reachable scenario gained test coverage (R13: `recall` against a genuinely empty
index). R12/R13 are new checks inside the existing `test_vector_rag_context_provider` target, not new
`ctest` targets — the target count is unchanged. **The two test gaps initially left named, not
closed, in that pass (a stale index entry reached through `recall`, and concurrent `recall()`
calls) were closed the same day** with R14 and R15 respectively (§5's follow-up) — the only
scope-limit that remained was a concurrent WRITER against a shared `IndexT` — also since CLOSED (same
day, real fix, see below), not left INCONCLUSIVE. Full suite **195/195** (`ctest -LE live-network`),
zero regressions.

**Accepted:** Design B (the revised `rt::drive_leaf_task()` with a 1-resume bound,
`Embedder::synchronous_leaf` with an honestly-stated higher review bar, the double-wrap made explicit
at the call site, and the 4-conformer migration checklist — all executed), with Design C's
`Backgroundable` option remaining a documented, NOT-yet-implemented follow-on for slow-backend
deployments, and Design A/D explicitly named as the longer-term direction this ADR is not attempting.
**Both residuals initially left open (§5/§6) were closed the same day, post-Judged, at the project
owner's explicit request:** the `synchronous_leaf = false` fallback path is now exercised at runtime
through a real, dedicated `NonLeafEmbedder` test conformer (R16), confirming the `if constexpr` false
branch behaves exactly as designed, not merely compiles; and `BruteForceCosineIndex` (§6) gained a
real `std::shared_mutex`, making a concurrent WRITER against a shared `IndexT` genuinely safe by
construction rather than merely "unreachable today," proven by a real interleaved writer+4-readers
stress test in `test_vector_index.cpp`. No residual remains open against this ADR's own scope.
