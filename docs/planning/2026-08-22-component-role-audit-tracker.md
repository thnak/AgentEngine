# Component role/overlap audit — tracker

**Status:** open findings, no ADR yet — nothing here is a decision, just a recorded gap. **Practice:**
this file is the running log for an informal, recurring session ritual (started 2026-08-22): pick one
component at random, check whether it's still pulling its own weight, and whether its role overlaps
with something else in a way that's confusing rather than deliberate. Append new entries below rather
than opening a new file per session.

Each entry: what was picked, what was found, and its **disposition** — closed (fixed), tracked (real
gap, deliberately deferred, with why), or no finding (checked, nothing wrong).

---

## 2026-08-22 — `core/context_provider.hpp` and the `ContextProvider` composition cluster

Random pick landed on `core/context_provider.hpp` (the `ContextProvider` concept itself), which pulled
in three composite types that all do "merge N `ContextProvider`s into one" in slightly different ways:

- `HistoryAndSkillsProvider<H,S>` (`core/history_and_skills_provider.hpp`) — hand-written, fixed
  2-provider composite. The original.
- `ComposedContextProvider<Ms...>` (`core/composed_context_provider.hpp`) — its own top comment names
  itself as the generalization of the above to an arbitrary pack, built on the same
  `context_assembly.hpp::assemble_context()`.
- `detail::LazyComposedContextProvider<Ms...>` (`core/session_builder.hpp`) — a further variant needed
  because `AgentSession::history_provider_` is a plain, always-default-constructed value member, which
  `ComposedContextProvider<Ms...>` can only satisfy when *every* `Ms` is itself default-constructible —
  untrue for real `SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider`. `LazyComposedContextProvider`
  starts empty and is `engage()`d once, after construction, with the real values.

### Finding A — `HistoryAndSkillsProvider` is redundant and its ordering is a latent footgun

Fully subsumed by `ComposedContextProvider<Ms...>` since the latter was added
(`efdac09`). Grep confirms only two real (non-comment) usages left, both regression/e2e tests for a
historical wire-ordering bug (`tests/test_rt_agent_session_real_backend.cpp`,
`tests/test_rt_agent_session_skills_live_e2e.cpp`).

Its template parameter order is `<HistoryProviderT, SkillsProviderT>` but its constructor deliberately
pushes **skills first, history second** onto the wire (a documented, intentional inversion — the type's
own name/parameter order is not a reliable guide to wire order). That inversion is safe today only
because a code comment says so; nothing in the type signature communicates it. `ComposedContextProvider
<Ms...>` doesn't have this problem — declared order *is* wire order, always.

**Disposition: CLOSED (2026-08-23).** Folded into Finding B's redesign, exactly as the "Recommended
follow-up" below anticipated — `HistoryAndSkillsProvider` deleted, its two call sites migrated to
`ComposedContextProvider`. See the "Findings A and B closed" section near the end of this file and
`decisions/ADR-074-composed-context-provider-consolidation.md`.

### Finding B — `ComposedContextProvider` still has the fork-aliasing bug `LazyComposedContextProvider` was fixed for

`AgentSession::fork_from()` (`rt/agent_session.hpp:1059`) plain copy-assigns `history_provider_`.
`ContextProviderDescriptor`'s closures (`context_assembly.hpp`) capture each wrapped provider through a
`shared_ptr` by value, so copying a composite provider aliases the *same* underlying provider instances
rather than producing independent ones.

This exact bug was found live (round 4 red-team, `session_builder.hpp` finding 9) for
`LazyComposedContextProvider` — reproduced with a stateful fixture: mutate the original via
`on_turn_end`, read the copy's `on_context`, see the mutation. Fixed there by making
`LazyComposedContextProvider` move-only.

The same finding's own text explicitly disclosed the residual: *"The underlying shared_ptr-aliasing
mechanism in `ComposedContextProvider` itself is UNCHANGED — out of this file's scope."* Confirmed via
`git log` that no later commit touched `composed_context_provider.hpp` for this. `ComposedContextProvider`
is still plain-copyable today, and it's used directly in production (`NativeCapabilityAnnouncer`,
`src/backends/native_process/native_capability_announcer.hpp:77`, per ADR-071) as well as being available
to any consumer-dev host that composes providers this way and later calls `fork_from()`.

Checked severity for the one shipped direct user (`NativeCapabilityAnnouncer`): `NativeProcessProvider`'s
members (`approval_`, discovery patterns) are set at construction and never mutated per-call, so aliasing
it specifically is currently low-impact. The open exposure is generic: *any* consumer composing a
genuinely stateful provider (a memory provider, a skills provider, a RAG cache) directly through
`ComposedContextProvider` and then forking that session inherits the same aliasing `LazyComposedContextProvider`
was fixed for.

**Disposition: CLOSED (2026-08-23), superseding the "deliberately deferred" call below.** The original
reasoning (ADR-070's ship-first/harden-later posture) held at the time, but the project owner later
directed this be fixed for real once the blast radius was verified safe (zero real `fork_from()` caller
touches this type) — `ComposedContextProvider` is now move-only, closing the aliasing gap at the
source, not left for a host to work around. See the "Findings A and B closed" section near the end of
this file and `decisions/ADR-074-composed-context-provider-consolidation.md`. Original reasoning kept
below as a point-in-time record, not deleted:

*This is exactly the kind of
residual `ADR-070`'s ship-first/harden-later posture and the delegated-decision-seam already cover: the
engine is not trying to make every composition path fork-safe by construction before shipping the
feature surface; a host/consumer-dev composing stateful providers directly and calling `fork_from()`
owns that isolation decision, same as other already-disclosed ADR-070/ADR-071 residuals. Not silently
unaddressed — recorded here, plus already disclosed once in `session_builder.hpp`'s own finding 9 text.*

### Recommended follow-up — DONE (2026-08-23)

Per standing project-owner guidance on design taste (prefer a clean redesign over patching around
existing shapes for reuse's sake): when this cluster is next touched for real, the three-type shape
(`HistoryAndSkillsProvider` / `ComposedContextProvider` / `LazyComposedContextProvider`) is a better
candidate for **one coherent redesign** — e.g. a single composite type that is always safely
constructible in `AgentSession`'s plain value-member slot (closing the reason `LazyComposedContextProvider`
had to exist as a separate type) and move-only by construction (closing Finding B at the source instead
of per-conformer) — than for patching each layer separately.

*Originally recorded as "not scheduled; no ADR opened yet" — done, exactly as described above, in
`decisions/ADR-074-composed-context-provider-consolidation.md`. See the "Findings A and B closed"
section near the end of this file.*

### Disposition policy applied to Finding B (per project-owner direction, 2026-08-22) — SUPERSEDED 2026-08-23

`ComposedContextProvider`'s fork-aliasing gap is **not** being queued for an immediate point-fix. This
project has already made a deliberate, ADR-backed trade (`ADR-070`): ship a broad feature surface and
let a host/consumer-dev embedding AgentEngine as a library own the hardening of scope they've been
delegated, rather than the engine closing every residual before shipping. Finding B fits that posture —
a disclosed, understood, low-current-blast-radius gap — so it stays *tracked*, not rushed. If/when the
composition cluster gets its promised clean redesign (above), Finding B is closed at the source as a
side effect, not as a separate patch.

*Superseded: the project owner later directed the redesign actually happen this session, once the
blast radius was independently verified safe. Kept as a point-in-time record of the reasoning that held
at the time, not deleted — see the "Findings A and B closed" section near the end of this file.*

---

## 2026-08-22 — `ContextProvider` as an extension point ("AIContextProvider")

Follow-up request used the name `AIContextProvider`. That name does not exist in this codebase — it's
**MAF's** name for the equivalent seam. AgentEngine's own type is `ContextProvider`, and the difference
is deliberate on two axes, both already recorded in 005 §5 / `OpenQuestions.md` OQ-18 (judged,
2026-08-11):

- **Naming**: CLAUDE.md's "no .NET/managed-runtime vocabulary" rule — MAF supplies the *shape*, not the
  spelling.
- **Composition mechanics**: MAF's `AIContextProvider` runs contributors as a *sequential pipeline*
  (provider N sees provider N−1's already-merged `AIContext`). AgentEngine's `assemble_context()` runs
  contributors as *independent fan-out* (each `on_context()` sees only `SessionContext`, never a prior
  contributor's output) — a generic pipeline/reactive variant was designed, red-teamed, and rejected for
  reopening cross-contributor coupling. A concrete reactive need is meant to be solved with a
  purpose-built composite `ContextProvider` (the `HistoryAndSkillsProvider`/`ComposedContextProvider`
  idiom above), not by widening the generic seam.

**No finding on naming/mechanics — this boundary is intentional, judged, and consistently documented.**

### Correction: what reaches the model is actually two tiers, not one

User pointed out (2026-08-22) that "contributing context to the LLM" is a two-tier architecture, confirmed
against real code — the framing above only covered tier 1:

- **Tier 1 — contribute** (`ContextProvider.on_context`, this section's whole subject): every conformer
  produces a `ContextContribution` (instructions/messages/tools); `assemble_context()` fans them out
  independently and merges in declared order. Each contributor may only ADD.
- **Tier 2 — govern/filter** (`TurnMiddleware.on_turn`, `core/turn_middleware.hpp`, `ADR-067`): runs once,
  AFTER tier 1 has already merged everything, immediately before the model call (the `pre_model` point).
  Structurally SUBTRACTIVE/ADVISORY-ONLY against what tier 1 already produced — `redact_subspan()` can
  only remove a byte range from an already-declassified `TaintedText` (never introduce new, undeclassified
  text); `ToolSurfaceView` only allows `redact()`/`reorder()`/`annotate_description()` on tools tier 1
  already contributed, never touching `invoke`/`capability_ceiling`/`approval_mode`. A middleware can also
  `deny` (stop the whole chain, 017 §4's verdict vocabulary). Wanting to ADD new instruction text at this
  point is explicitly not supported — that must be a tier-1 contribution instead.

Not overlap — genuinely complementary halves of "what reaches the LLM," and the boundary between them
(add vs. govern-what-was-added) is structurally enforced by the types themselves, not just by convention.
**Implication for Finding C below**: an onboarding sample/doc for "customize what the LLM sees" needs to
teach both tiers together, not tier 1 alone, or a reader would not learn the ONE sanctioned place to
safely filter/redact already-contributed content.

### Conformer ecosystem check — no overlap found

Checked all five real `ContextProvider` conformers (`HistoryProvider`, `SkillsProvider`, `MemoryProvider`,
`VectorRagContextProvider`, `ToolOptimizerProvider`) plus `NativeProcessProvider`/`NativeCapabilityAnnouncer`.
Each file's top comment states its scope and explicitly disclaims overlap with its nearest neighbor —
e.g. `VectorRagContextProvider`'s own comment: mirrors `MemoryProvider`'s *shape* closely, but is a
deliberately separate class, not a specialization or subclass (`ADR-063` §2.1b), and that "one class per
kind" decision was independently red-teamed and confirmed "consistent with the real
`MemoryProvider`/`SkillsProvider` precedent, not merely asserted." Where two conformers genuinely share
logic, it's factored into a real shared helper instead of duplicated (`provenance_marker.hpp::
neutralize_forged_provenance_markers()`, used by both the memory and RAG providers). **No finding** —
this cluster is well-governed.

### Finding C — the actual "write your own" on-ramp is undocumented and unexampled

The user's own framing of this feature ("tính năng quan trọng cho phép tùy chỉnh và xây dựng tính năng")
is exactly right, but nothing in the tree demonstrates it end-to-end for a third-party/consumer-dev:

- `samples/` — named in `CONVENTIONS.md`'s own layout table as "runnable programs over the public
  surface" — contains only a `README.md` describing intent. Zero actual sample files exist, for any
  extension point, `ContextProvider` included.
- No guide anywhere (checked `docs/architecture/`, `docs/planning/`, RFC 005 itself) walks through
  writing and wiring a custom `ContextProvider` from scratch — RFC 005 §5 documents the concept's shape
  and its built-in kinds, not an authoring walkthrough.
- The only concrete "here's a type that satisfies the concept without being a built-in kind" reference
  in the whole tree is a hand-rolled mock conformer inside `tests/test_composed_context_provider.cpp` —
  not surfaced as a guide or sample anywhere a consumer-dev would find it.

Minor, non-blocking naming note found alongside this: `web/marketing/api/providers.html` already exists
and documents "Model providers" (`ChatClient` backends) — an unrelated seam that happens to share the
English word "provider." Not confusing in code (distinct type names), but worth remembering if a future
"Context providers" web page is added, so the two aren't titled ambiguously against each other.

**Disposition: PARTIALLY CLOSED (2026-08-23).** The "write your own `ContextProvider`" on-ramp now
exists: `examples/18_custom_context_provider.cpp` — a minimal `PirateStyleProvider` contributing one
instruction message, composed with the built-in `HistoryProvider` via `ComposedContextProvider`, wired
into a real `AgentSession`, proven end-to-end (the instruction is asserted present in the real outbound
`ChatRequest`, not just returned in isolation). Registered in `examples/CMakeLists.txt`, builds and
passes via `ctest`.

**Placed in `examples/`, not `samples/` as originally recommended** — a real, judgment-call deviation
from this finding's own original text, worth recording: `samples/` (the location this finding named,
per `CONVENTIONS.md`'s layout table) still contains only its own `README.md`, nothing else, ever.
`examples/` is a SEPARATE, already-populated, already-`ctest`-registered directory (17 numbered
"get-started" programs mirroring MAF's own sample progression) that is the tree's actual, live
convention for exactly this kind of runnable demonstration — confirmed by reading several existing
entries before adding a new one, not assumed. `CONVENTIONS.md`'s layout table naming `samples/` instead
of `examples/` is itself a minor documentation drift this finding's closure surfaced but did not fix
(out of this round's scope — a one-line table correction or a decision to retire one of the two
directories, left for a future pass).

Still open, not attempted this round: a "Writing a ContextProvider" web docs page (the second half of
the original recommendation) — the example above is the runnable proof; a docs page walking through it
in prose is separate, deferred work.

### Finding D — no declared limit on chain length (confirmed intentional); budget enforcement that exists is per-contributor and post-hoc, not aggregate/pre-flight

Checked both chains for an explicit maximum contributor/middleware count, and for what actually stops an
unbounded assembled context from reaching the model. Traced the real call path
(`context_assembly.hpp::assemble_context()` → `AgentSession::run_rounds()` → `run_model_call()`,
`rt/agent_session.hpp:1919-1956`).

- **Tier 1 (`ContextProvider` chain).** `ComposedContextProvider<Ms...>` / `LazyComposedContextProvider
  <Ms...>` take `Ms...` as a compile-time template pack — no project-declared cap (no `MaxProviders<N>`
  policy tag, unlike this codebase's own established idiom for other bounds, e.g. `MaxTurns<N>`,
  `TokenBudget<N>`). The only ceiling is whatever the compiler's own template-instantiation/recursion
  depth allows — an implementation limit, not a declared invariant.
- **Tier 2 (`TurnMiddleware` chain).** Same shape: `run_turn_middleware_chain<Ms...>`
  (`turn_middleware.hpp:207-261`) is a `constexpr`-recursive walk over `std::tuple<Ms...>`, one
  specialization per pack length, again bounded only by the compiler, not a declared count.
- **The budget mechanism that exists (`ContextBudget.max_tokens`) is per-contributor, not aggregate.**
  `assemble_context()` (`context_assembly.hpp:193-205`) trims only the OLDEST MESSAGES WITHIN one
  contributor's own contribution once THAT contributor's own declared budget is exceeded — deliberate,
  documented (drop order must stay predictable from each contributor's own declared budget alone, never
  a shared pool). There is no code path anywhere that caps the SUM of every contributor's output — N
  contributors each individually under budget (or left at the `max_tokens == 0` default, unbounded) can
  still combine into an arbitrarily large `ContextContribution`.
- **The one downstream numeric safety net, `token_budget_` (`AgentSession`), is checked AFTER the model
  call returns, not before.** `run_rounds()` builds `ChatRequest` directly from the fully-assembled,
  unchecked `contribution->messages`/`.tools` and sends it (`rt/agent_session.hpp:1919`, `run_model_call`)
  with no pre-flight size check at all. Only once the response comes back does
  `run_tokens_consumed_ += response->usage...` get compared against `token_budget_` (line 1950) — and
  that's a **cumulative, cross-round RUN budget** (stops future rounds once exceeded), not a per-request
  cap that could have stopped THIS request from being sent oversized in the first place. A first round
  alone, with generously-composed providers, is not gated by anything in-engine before hitting the wire.

**Disposition (confirmed by project owner, 2026-08-22): no chain-length limit is intentional, not a gap.**
Neither `ComposedContextProvider<Ms...>`/`LazyComposedContextProvider<Ms...>` (tier 1) nor
`run_turn_middleware_chain<Ms...>` (tier 2) should have a declared `MaxProviders<N>`/`MaxMiddlewares<N>`
cap — a consumer-dev composing their own tree of providers/middlewares is meant to be free to build
whatever shape they need there, unconstrained by an arbitrary engine-picked number. Matches this
project's already-established `ADR-070` posture: ship a broad, permissive composition surface; a
host/consumer-dev embedding AgentEngine owns the tradeoffs of what they compose. **Not tracked as
something to fix.**

What stays worth recording precisely (distinct from the count question above, not re-opened by this
disposition): the *token-budget* enforcement that does exist is per-contributor and post-hoc, not
aggregate/pre-flight — so "budgets are enforced" (I8) is real but narrower than the name alone suggests.
That's a factual note about what the mechanism currently does, kept here for accuracy, not a request to
add a count-based limit or otherwise constrain how freely a consumer-dev can compose chains.

### Finding E — per-contributor budget trimming is completely invisible by the time a dev could see it (I4 gap)

User flagged (2026-08-22): silent trimming is dangerous — a dev has no way to know it happened. Traced the
full path of `ContextAssemblyResult.drops` (the diagnostic `assemble_context()` itself already produces,
`ContextDrop{contributor_index, contributor_message_id, reason}`) from where it's created to wherever it
might surface. It doesn't, anywhere in a real run:

- `assemble_context()` (`context_assembly.hpp:165-`) genuinely records every drop into `out.drops` —
  the mechanism to know a drop happened exists and is unit-tested in isolation
  (`tests/test_context_assembly.cpp`: B3-R1 etc.).
- But **every real composite that wraps it throws `.drops` away**, identically, in three places:
  `ComposedContextProvider::on_context()` (`composed_context_provider.hpp:67-71`),
  `HistoryAndSkillsProvider::on_context()` (`history_and_skills_provider.hpp:77-81`), and
  `LazyComposedContextProvider::on_context()` (`session_builder.hpp:519-521`) all do the identical
  `ContextAssemblyResult assembled = co_await assemble_context(...); co_return assembled.combined;` —
  `assembled.drops` is computed, then discarded, never logged, never turned into an event.
- This isn't three independent oversights — it's **structurally forced** by the `ContextProvider` concept
  itself: `on_context()` must return `task<result<ContextContribution>>` (`context_provider.hpp:101-105`),
  and `ContextContribution` has no drops field at all. Any composite exposing itself as an ordinary
  `ContextProvider` has no type-level room to pass drop diagnostics further up, even if it wanted to.
- Confirmed `AgentSession` itself never sees real drops either: `rt/agent_session.hpp:1881` explicitly
  fabricates an EMPTY `ContextAssemblyResult{drops: {}}` around the already-drop-stripped
  `ContextContribution` it received from `history_provider_.on_context()`, purely as a type-adapter so
  `TurnMiddleware` has something to look at — the comment there is explicit that this is a workaround, not
  a claim the list is genuinely empty.
- No `run_event_kind` exists for "a contributor's own budget dropped N messages." Grepped
  `run_event.hpp`/`agent_session.hpp` for anything drop/trim-related — nothing.

**Net effect**: a consumer-dev who sets (or leaves at a too-tight non-default) a `ContextBudget` on their
own provider gets messages silently dropped from what the model sees, with zero observability anywhere in
a real `AgentSession` run — no event, no log, no return value carrying it. The only way to ever see a drop
happen is to call `assemble_context()` directly in a unit test, bypassing the actual session machinery
entirely. This is a real gap against **I4** ("every effect is attributable") — a drop is an effect on what
the model actually sees, and today it is not attributable through any production path.

**Disposition: CLOSED (2026-08-23).** Not in tension with Finding D's "no chain-length limit, by
design" — this isn't about constraining what a consumer-dev can compose, it's about them being unable to
find out afterward that their own chosen budget silently ate something. See the "Finding E closed"
section near the end of this file and `decisions/ADR-075-context-budget-fail-closed.md`.

**Project-owner direction on the fix shape (2026-08-22, not implemented this session): a budget-exceeded
trim should be an ERROR, not a silently-succeeding trim — and the condition needs to be something a test
can actually catch.** Concretely: today, exceeding a declared `ContextBudget.max_tokens` still returns
`result<ContextContribution>` as a plain success (`Ok`) with fewer messages inside it — there is no
`std::unexpected` anywhere on this path, so nothing in this codebase's own established `result<T>`
error-checking idiom can observe it, which is exactly why no test exercises it through a real
`AgentSession` run today. This has a real, already-shipped precedent to follow: `AgentSession`'s own
cross-round `token_budget_` does exactly this shape — `failure_class::resource`,
`"run.token_budget_exceeded"` (`rt/agent_session.hpp:1950-1956`) — fail closed on a declared budget being
exceeded, not silently continue. Applying the same shape to the PER-CONTRIBUTOR `ContextBudget` would mean
`assemble_context()` (or whichever seam owns the check) returns `std::unexpected` when a contributor's own
declared budget is exceeded, instead of trimming and returning `Ok` — making it a normal, testable
`AE_CHECK(!result.has_value())`-shaped assertion like every other fail-closed contract in this tree,
instead of requiring a caller to inspect an out-of-band `drops` list that (Finding E's own body above)
doesn't even reach them today.

**Named tension, resolved 2026-08-23 (was left open here):** `ContextBudget.max_tokens == 0` (unbounded)
is the default, and per `history_and_skills_provider.hpp`'s own (now-historical, that file is deleted —
Findings A/B) comment, budgets were opt-in specifically because unconditionally trimming (rather than
erroring) was already judged wrong for at least one real case (a `SkillsProviderT` advertisement that
must arrive whole or not at all). Turning "exceeded" into a hard error changes that case's own failure
mode too (from "silently arrives empty" to "the whole turn fails") — resolved by checking whether any
real, shipped caller actually depended on the graceful-degradation reading before choosing between them:
grepped every non-test `ContextBudget{...}` construction in the tree and found **zero** that set a
nonzero `max_tokens`. There was no real case to preserve dual behavior for — see
`decisions/ADR-075-context-budget-fail-closed.md` §3 for the full reasoning and why no "trim vs. error"
config knob was added alongside the fix.

### Finding F — a large file read (via a file-read tool or native `bash`'s `cat`) has no size cap anywhere by default, and can end up dropping the newest message when it finally hits a budget

User asked what happens when a tool like a file-read or `bash` reads a huge text file. Traced the whole
chain from read to wire, real code at each step:

1. **Read itself — capped only if the grant opts in, and capped means hard-fail, not truncate.**
   `worktree::mount_read()` (`core/worktree.hpp:1289-1328`) is the one mediated file-read primitive every
   file-reading path in this engine goes through (per `skill_provider.hpp`'s own comment: skills are read
   "with ordinary file operations... via `mount_read`"). Its only size control is
   `cap::FsRead.size_cap_bytes` (`trust/capability.hpp:83-87`) — `std::optional<uint64_t>`, **default
   `std::nullopt` (no cap)** unless whoever GRANTED that capability explicitly set one. If set and
   exceeded: `mount_read` refuses the ENTIRE read (`"worktree.mount_read_exceeds_size_cap"`) — there is no
   partial-read/streaming/truncate path at all, by design (confirmed: `object_store.get_blob(entry->digest)`
   fetches the whole blob before the size check even runs).
2. **Native `bash`'s `cat` builtin has no additional check of its own.**
   `mediated_shell_dispatch.cpp:143-154` — `cat` calls `fs.read_file(target)` (same `mount_read` path,
   same optional cap) then does an unconditional `out.stdout_text.assign(data->data(), data->size())` —
   whatever came back becomes the entire captured stdout, verbatim, no truncation logic at this layer
   either.
3. **`invoke_tool()` only records size, never enforces it.** `reply_bytes`
   (`tool_pipeline.hpp:581`, `= reply_json.size()`) exists purely for the audit record
   (`ToolInvocationAudit::result_bytes`) — nothing compares it against any limit.
4. **The (possibly huge) `ToolResult` lands in `history_` unbounded**, then on the NEXT round is replayed
   verbatim by `HistoryProvider<Window<N>>` (default `N == 0`, unbounded) into that round's
   `ContextContribution.messages` — gated only by whatever `ContextBudget` a caller optionally set on that
   provider (Finding D/E's own subject: default unbounded, and even when set, exceeding it silently trims
   rather than erroring, per Finding E).
5. **A newly-surfaced sharp edge, found while tracing this**: `assemble_context()`'s own trim loop
   (`context_assembly.hpp:198-204`, `while (total > max_tokens && drop_from < msgs.size())`) drops
   OLDEST-first with **no protection for the newest message**. If a caller HAS set a nonzero
   `ContextBudget` and the single huge tool-result message (almost always the newest one, just appended)
   is larger than the ENTIRE budget by itself, the loop keeps advancing `drop_from` right up through and
   including that newest message too — silently dropping the very thing the tool call was for, with no
   different treatment for "the message that would never fit no matter what else is dropped" vs. an
   ordinary old message being aged out.

**Net chain**: no cap by default at the read layer, no cap at the shell layer, no enforcement at the tool
pipeline layer, no aggregate cap at context assembly (Finding D, confirmed intentional), and the one
opt-in per-contributor budget that exists can silently discard the newest message rather than erroring
(Finding E, not yet given a design). A single `cat hugefile.txt` can carry an arbitrarily large payload
all the way to the wire completely ungated, unless a host manually set `cap::FsRead.size_cap_bytes` for
that specific grant.

**Disposition: tracked, not closed** (survey-and-mark only, per this session's explicit scope). Compounds
Finding E rather than introducing a fourth mechanism — the newest-message-can-be-dropped detail belongs to
Finding E's own eventual fix (fail-closed on budget-exceeded, not silent trim), not a separate design.

---

## 2026-08-22 — `core/worktree.hpp`: branching, merge, and out-of-scope access

Follow-up pick: the worktree/virtual-filesystem cluster (025), specifically its branching mechanism
(`sharing_mode`), the three-way merge on branch-join, and whether a guest can ever read/write outside
its own worktree scope. Traced real code, not just RFC/ADR text, at every layer.

### Branching (`sharing_mode`) — no finding

`SubWorktree` (`core/worktree.hpp:379-389`) models four modes as one small struct, not four
special-cased types: `shared` (same Ref as parent, immediate cross-visibility falls out of that alone),
`branch` (copy-on-write, new Ref seeded at the parent's current tree digest, `base_digest` captured at
creation for the later merge), `scratch` (new Ref seeded at a fresh empty tree, never touches the
parent), `readonly` (a pinned digest, not a Ref at all — `write_sub_worktree` fails closed on the mode
itself, before ever touching the store). Each mode's semantics are distinct, and `create_sub_worktree`/
`read_sub_worktree`/`write_sub_worktree` dispatch on the enum rather than duplicating logic per mode.
Real caller exists and is wired correctly: `workflow/worktree_scoping.hpp` (`mint_executor_worktrees`)
is the production entry point that turns a `Workflow`'s declared `worktree_mode` per executor into real
`SubWorktree` grants — not dead vocabulary.

**Self-disclosed residual, already tracked under ADR-032 §5, not re-logged here**: `resume_executor_worktrees`
(`worktree_scoping.hpp:169-214`) cannot reconstruct a `readonly` executor's pinned digest after a crash
(fails closed with `worktree_scoping.readonly_resume_unsupported` rather than fabricating a stale/empty
one) and cannot reconstruct a resumed `branch`'s `base_digest` (a later merge on that resumed branch would
need it re-supplied by a not-yet-built checkpoint-schema change). Both are named in the code's own
comments and in ADR-032 §5 — confirmed still accurate, no new gap found.

### Merge (`merge_trees`/`merge_branch_into_parent`) — no finding

Real three-way merge, not last-writer-wins: `merge_subtrees` (`core/worktree.hpp:504-583`) recurses
per-entry against the branch's own recorded ancestor (`base_digest`), so a change two levels deep in one
branch and an unrelated change two levels deep in the parent merge automatically without conflicting at a
shared ancestor directory. A genuine same-name-both-sides-differently conflict is reported, never guessed
— `MergeResult` carries `conflicts`, and a non-empty `conflicts` means **nothing was committed** (025 §4's
"a failed merge must never partially apply," confirmed by `merge_branch_into_parent`'s own control flow:
it returns the `BranchMergeOutcome` with conflicts and skips `commit_ref` entirely on that path).

Stale-parent race is handled as a real CAS, not assumed away: `merge_branch_into_parent` takes an
`expected_parent` snapshot and fails closed (`worktree.merge_stale_parent`) if the parent's Ref moved
since that snapshot was read, rather than silently merging against a parent that's already moved on.
`retry_merge_branch_into_parent` wraps it with a bounded retry loop that only re-attempts on that exact
stale-parent error — a real conflict (non-empty `conflicts`) is returned immediately, not retried into
a different outcome. `workflow/worktree_scoping.hpp:276` calls the retry wrapper (not the bare
single-attempt function) with `3` attempts — matches the documented reasoning that a stale-parent race
is a real, expected concurrency case here (a run's supervising root ref can move between snapshot and
commit), not a hypothetical.

### Out-of-worktree-scope access — no finding, but verified rather than taken on trust

This question spans two deliberately separate layers, and each is scoped correctly for what it actually
is:

1. **The in-memory content-addressed `Tree` layer (`core/worktree.hpp`, `mount_read`/`mount_write`).**
   No real OS filesystem sits underneath — a `Tree` is a flat `name -> digest` map with no parent
   pointers, no symlinks, nothing an OS resolves — so `..`/absolute-path escape has no mechanism to even
   attempt. `split_mount_path` rejects `.`/`..`/leading-or-trailing-or-double slash lexically as
   malformed input (`worktree.hpp:1089-1122`), not as attack mitigation — the header's own comment is
   explicit that this is deliberately NOT yet 025 §5's OS-level escape corpus, because there is nothing
   here for that corpus to apply to. `mount_read` additionally checks `granted.mount_id` matches exactly
   and `granted.path_prefix` covers the requested path (via the same `path_prefix_covers` primitive
   `CapabilitySet::attenuate` uses) before any store access — structurally can't reach a different mount
   or a narrower-than-granted subtree.

2. **The real-OS-backed materialization layer (`core/worktree_mount_fs.hpp`/`.cpp` + POSIX twin, ADR-014,
   Judged 2026-08-06) — where an actual path-escape vulnerability could physically exist.** This is a
   genuinely hardened primitive, not an aspirational one: Design A (canonicalize-then-string-check, reopen
   later) was proven vulnerable to TOCTOU by deterministic reproduction, not argued from principle; Design
   B (single handle-based open, containment verified from the resolved handle/`GetFinalPathNameByHandleW`
   on Windows, `/proc/self/fd`-based re-verification on Linux) was accepted and proven immune to the
   identical interleaving. 22 real-filesystem Windows checks + 21 real Linux checks, all passing, covering
   `..`, absolute redirects, ADS, `\\?\`, junction/symlink boundary-crossing (with an in-mount junction as
   a positive control — crossing is rejected, mere existence is not), and unicode dot-lookalike names. A
   real production bug (missing `FILE_SHARE_DELETE`) was found and fixed during the ADR's own prove pass.

   **Verified, not just cited, that ADR-014 §8.5's own largest-named residual is now closed**: at the time
   the ADR was written, "nothing yet forces every guest-reachable filesystem operation... to actually route
   through `open_within_mount_root` — no such caller exists (Phase E)." Grepped real callers today:
   `src/backends/native_jail/mediated_filesystem_adapter.cpp` (every read/write/list/delete op),
   `mediated_python_runner.cpp`'s `Internal_open` (the only way guest Python's `open()`/`io.open()` can
   reach a file), and `mediated_shell_dispatch.cpp` (every builtin — `cat`/`ls`/write/etc. — takes a
   `FileSystemAdapter&` and never touches a raw path itself) all route through it. Quota enforcement
   (ADR-014's own out-of-scope Phase C3) is real too and deliberately shared, not duplicated:
   `MediatedFileSystemAdapter::usage()` forwards to the same `mount_root_usage` scan Python's write path
   uses, so the shell's live-quota check can't drift from Python's ("Gap-12 fix, 2026-08-14" per that
   file's own comment). This is worth recording here precisely because the ADR's own text, read in
   isolation today, would incorrectly suggest this wiring is still missing — it is a point-in-time record
   (correctly so, ADRs in this project are never silently rewritten), and this entry is the place that
   notes the residual it named has since been closed.

   **Two named residuals from ADR-014 §8.3, re-checked, still open** (no new work landed since
   2026-08-06): 8.3 short-filename aliasing (e.g. `PROGRA~1`) is reasoned-covered but not executed against
   a volume with 8.3 generation actually enabled; real symbolic links (as distinct from junctions) were
   never exercised on Windows, only reasoned-equivalent by shared OS mechanism. Both Windows-only, both
   already self-disclosed in the ADR itself — not new findings, just confirmed still accurate.

**No finding overall for this round** — branching, merge, and the two-layer escape-prevention story are
all real, tested, and honestly self-documented where gaps remain. Recorded at this length specifically
because "no finding" after genuine verification (grepping real callers, not trusting the ADR's own
point-in-time claim) is itself useful signal for future audit rounds on this cluster.

---

## 2026-08-22 — Follow-up: does every session own a worktree, and can a child agent branch/merge/resume against it?

User asked directly (2026-08-22): shouldn't a session own one worktree, with child sessions/agents
branching from it so they can merge back, which in turn requires a real resume mechanism? This matches
025's own stated design (§Goal: "give every session a worktree"; §3's layout diagram: `session:s-42`
root with `/agents/researcher`, `/agents/writer` sub-worktrees) and 025 §10 Q5's resolution (`030`:
"one worktree, one session," an index of independent worktree refs at a Project layer above sessions,
not shared). Traced how much of that is actually wired today, beyond the previous round's primitive-level
"no finding" (merge/escape-prevention are solid) — the primitive being solid is not the same question as
whether a real `AgentSession` run ever reaches it.

### Finding G — `AgentSession` itself never creates or owns a worktree Ref

Reconfirmed from the previous round: zero `#include "agentengine/core/worktree.hpp"` anywhere in
`rt/agent_session.hpp`, no member of type `Ref`/`SubWorktree`/anything worktree-shaped. A real
`AgentSession` constructed today has no worktree unless something entirely outside the session type
(a workflow, a hand-wired `ContextProvider`, a host-supplied capability grant) separately creates one and
hands it in piecemeal via `Mount`/`cap::FsRead`/`cap::FsWrite`. 025's own "give every session a worktree"
goal is not true by construction anywhere in the engine.

### Finding H — the only real branch/merge wiring that exists is for workflow executors; `agent.spawn` (freeform child agents) has *no* sub-worktree wiring at all, and this is already self-disclosed

Two different "a session has children" mechanisms exist in this codebase's vocabulary, and only one of
them is wired to worktree branch/merge:

- **`workflow/graph.hpp` executors** — `workflow/worktree_scoping.hpp::mint_executor_worktrees` really
  does call `create_sub_worktree` per declared `worktree_mode`, and its merge-on-join hook really does
  call `retry_merge_branch_into_parent`. This is genuine, tested wiring (previous round's audit
  confirmed the primitives it calls are sound).
- **`agent.spawn`** (026's ad-hoc "an agent spawns a child agent" primitive, the more general shape the
  user's phrasing implies) — **has no real call path anywhere in this codebase**, and this is not a new
  finding, it's already self-disclosed at `OpenQuestions.md` line 456 (ADR-031, 2026-08-11), quoted
  verbatim because the wording is exact about what's missing: *"agent.spawn itself still has no real
  call path anywhere in this codebase (confirmed exhaustively during ADR-031's own design phase — no
  `Tool<>`-conforming spawn tool, no nested-agent-run invocation mechanism, no sub-worktree wiring, no
  production `AgentSession` tool-call loop able to host a spawned child; ADR-024 §7's own gap)."` Only
  `SpawnBudget`/`SpawnCostBudgetActor` (the depth/cost bound primitives, ADR-006/ADR-031) exist and are
  proven in isolation — the spawn mechanism itself that would need a worktree to branch from does not
  exist to wire one into.

So "child sessions/agents branch from the parent's worktree" is real **only** for the structured,
graph-declared multi-agent shape, not for the freeform one — a materially narrower claim than "every
session's children can branch and merge."

### Finding I — resume, even on the one path that IS wired, is confirmed still incomplete (cross-reference, not a new gap)

This is exactly the piece the user named as a hard requirement. Re-confirmed from the earlier session
(no new code landed since): `worktree_scoping.hpp::resume_executor_worktrees` (`worktree_scoping.hpp:169-214`)
already fails closed on a `readonly` executor resume (`worktree_scoping.readonly_resume_unsupported` —
the pinned digest was never durably stored) and cannot reconstruct a resumed `branch` executor's
`base_digest` (silently fine for ordinary read/write, but a merge attempted after resuming that branch
would be missing the three-way-merge ancestor `merge_branch_into_parent` requires — not fail-closed the
same way, a real correctness gap on the merge path specifically, not just an availability one). Both
gaps are self-disclosed in the function's own comment and in ADR-032 §5. **Not fixed by anything in this
codebase today.**

**Correction (2026-08-22, follow-up fork verification):** the sentence above originally cited a
"`workflow/checkpoint.hpp` `RunStateRecord` schema change" as the prerequisite. No file named
`workflow/checkpoint.hpp` exists in this tree — that was the old Quark-era file this machinery was ported
from (ADR-037); the real, current type is `RunStateRecord` in `include/agentengine/rt/workflow_supervisor.hpp:301-312`.
More precisely than "schema gap": `RunStateRecord`'s fields (`run_counter`/`run_id`/`rounds`/`pending`/
`partial`/`selected_output`/`failed_executor`/`unopened_ports`/`elapsed_ns`/`ports`) correctly contain
**no** worktree-shaped state at all — confirmed deliberate, not an oversight: `WorkflowSupervisor` itself
holds no `worktree.hpp` type and no object/ref-store reference of its own
(`workflow_supervisor.hpp:83-95`'s own comment), by design — the host is required to have already called
`worktree_scoping.hpp`'s `mint_executor_worktrees()`/`resume_executor_worktrees()` before ever driving the
supervisor. So the actual gap is narrower and more precisely located than "the checkpoint schema needs a
change": `SubWorktree::pinned_digest` (readonly) and `SubWorktree::base_digest` (branch) simply have **no
durable home anywhere** — not in the Ref store, not in `RunStateRecord`, not anywhere else. A fix belongs
in `worktree_scoping.hpp` itself (or a new small durable record scoped to just those two fields), not in
`RunStateRecord`'s own shape. Finding I's conclusion is unchanged; this only corrects where a real fix
would land.

**Adjacent no-finding (same follow-up fork):** `workflow/graph.hpp` (declarative graph-as-data only, no
execution — exists so 015's loader and the C++ authoring form share one `validate_workflow()`, I6),
`workflow_supervisor.hpp` (execution loop + ordinary single-slot crash-recovery checkpoint via
`rt::SessionStore` — only the latest checkpoint recoverable, a named, accepted narrowing vs. the Quark
original), and `workflow_time_travel.hpp` (an explicitly *additive* multi-version rewind log over
`rt::AppendLogStore`, kept separate from the checkpoint log because the two answer different questions —
"what is this run's state at index N" vs. "who rewound it and why" — not a second, competing restore
mechanism: `rewind_workflow()` hands its result back through the same `restore_from_record()`/
`resume_workflow()` path an ordinary resume already uses) are cleanly layered, each with a distinct job,
each disclosing its own scope narrowing honestly. I1/I4/I5 cross-check clean: every checkpoint/rewind is
an explicit, attributable record (`operator_id`/`reason` carried verbatim, never inferred).

### Net picture

The three pieces the user described are the RIGHT target architecture — it's what 025 itself specifies —
but today: (G) the session-worktree binding doesn't exist for any session; (H) branch/merge wiring exists
for exactly one of the two "session has children" mechanisms (workflow executors), not the other
(`agent.spawn`, which doesn't exist yet at all); and (I) even the one wired mechanism's resume story has
two named, unfixed holes, one of which specifically breaks the merge case the user is asking about.
Building the architecture the user described for real is not a small patch on top of what exists — it is
closing G, then either building `agent.spawn`'s entire missing call path (H) or scoping the requirement to
workflow executors only, then extending the checkpoint schema to fix (I) for the merge case specifically.

**Disposition: tracked, not closed** (session scope remains survey-and-mark, per this session's standing
instruction). Not implemented. If this becomes real design/implementation work, it is large enough
(spans 025, 026/`agent.spawn`'s own missing machinery, and giving `SubWorktree::pinned_digest`/`base_digest`
a durable home — see the correction above) to warrant its own ADR rather than a drive-by fix, consistent
with CLAUDE.md's "contested, hot-path, or security-critical designs go through design → red-team → prove →
judge."

---

## 2026-08-22 — Parallel sweep: trust/capability, sandbox backends, protocol surfaces, tool_pipeline/ModelCallGateway, memory/RAG/skill (deeper pass)

User asked to cover the remaining components broadly while stepping away, pre-approving commits for
whatever this round finds (push deferred to a later joint review). Ran six parallel investigations, each
scoped to one cluster, each required to verify claims against real code/callers rather than trust
comments alone — same discipline as every round so far. Synthesized below; letters continue from Finding I.

### `trust/` cluster (capability.hpp, secret.hpp, secret_quarantine.hpp, principal.hpp, bearer_token.hpp, endpoint_id.hpp, capability_token.hpp, capability_registry.hpp, hmac.hpp, secure_random.hpp, steady_deadline.hpp, policy_reachability.hpp)

**No finding, most of the cluster.** All 14 headers carry the same "one class per kind, disclaimed against
its nearest neighbor by name" discipline this session already found in the `ContextProvider` cluster
(ADR-063 §2.1b) — independently confirmed present here too. Specifically checked and found clean:
`capability.hpp`'s 17 `cap::*` kinds are all genuinely parameterized (007 §3 property 5 — no boolean-only
kind slipped through); `secret.hpp` (operator-declared) vs `secret_quarantine.hpp` (runtime-discovered) are
cleanly split by provenance, and the latter's own comment discloses a real design flaw found and fixed
during implementation (ADR-068's original design assumed inline capability-granting, structurally
impossible against `CapabilitySet`'s empty-by-construction/borrowed-`const*` shape — fixed by splitting
`quarantine()` from a host-only `grant_eligible_ref_names()` query, with `agent_initiated` refs permanently
ineligible for auto-grant, a real I3 enforcement); `capability_registry.hpp`'s cross-process revocation and
`BoundCapability::revoke()`'s in-process revocation share the word "revoke" but are correctly disclaimed,
different scopes; `principal.hpp`/`bearer_token.hpp`/`endpoint_id.hpp` are three distinct identity-adjacent
concepts, no confusion between them; `hmac.hpp`/`secure_random.hpp`/`steady_deadline.hpp` are properly
factored shared primitives, each citing a real second consumer by name (not premature abstraction);
`policy_reachability.hpp` already self-discloses its own narrower-than-its-name scope (M2 Phase B decision
4 only, not 007 §5's full declarative rule language).

### Finding J — model calls have no per-call attribution/audit record analogous to `ToolInvocationAudit` (real I4 asymmetry)

Every tool call gets a real `ToolInvocationAudit` (`tool_pipeline.hpp:344-367`): principal identity fields
(fixed for exactly this reason by ADR-061 §7 R26), idempotency key, duration, result size. Model calls get
none of this. Traced the real path: `AgentSession`'s round loop (`agent_session.hpp:1940-1942`) emits only
bare `run_event_kind::model_call_started`/`model_call_finished` markers — confirmed via `run_event.hpp:44`
these are plain enum values with **no payload struct**, unlike `run_failed` a few lines away in the same
call site which does carry one. Token usage is folded into `run_tokens_consumed_`, a running cumulative
total, not a discrete per-call record. `ModelCallGateway::call()`/`call_stream()` never construct or
surface anything `ToolInvocationAudit`-shaped, and `ctx.principal` — available at every model-call site —
is never stamped onto anything durable for that specific call, even though `call()` already computes a
real `IdempotencyKey` (line 176) that could anchor such a record. This is the exact class of gap ADR-061
§7 R26 already fixed once for tool calls ("an audit trail without identity cannot attribute anything")
— never extended to model calls, despite a model call being an equally real, budget-gated (I8), capability-
mediated effect. **Disposition: tracked, not closed.**

**Adjacent, no finding:** `tool_pipeline.hpp`/`model_call_gateway.hpp` role separation itself is clean —
tool-call mediation vs. model-inference mediation are genuinely distinct concerns sharing only
`EffectContext` and `IdempotencyKey` as common vocabulary, not duplicated logic; `ModelCallGateway` (retry/
failover only) vs `MiddlewareModelCallGateway` (middleware only) staying two small types instead of one
god-object matches the same "one class per kind" pattern found elsewhere this session. **Verified, not
assumed: the 2026-08-22 `report_progress`/`bound_capabilities` fix in `call_stream()` (commit 9208703) is
sound and complete** — `ctx_copy` unconditionally resets both fields before the detached thread captures
it (`model_call_gateway.hpp:230-231`), mirroring `tool_pipeline.hpp:679`'s own precedent for the identical
hazard class. Three pre-existing, already self-disclosed residuals near it (blocking `sleep_for` in retry
backoff with no coroutine-suspension point; `MiddlewareModelCallGateway`/`ContentReplayGateway` don't
implement `call_stream()`, silently losing streaming when wired through them, already warned about at
`start_run()`; the detached-thread `this`-capture's lifetime contract is disclosed, not structural) were
re-checked and remain accurately still-open — not new, not hidden.

### `protocol/` cluster (mcp/, a2a/, agui/, openai/, anthropic/)

**No finding on cross-surface overlap.** Each of the five surfaces has a distinct, non-overlapping job
(MCP: inbound tool discovery; A2A: content translation; AG-UI: the one *stateful* streaming projection of
the three; OpenAI/Anthropic: outbound model-provider backends on a completely different axis). Shared
reasoning is cited across files, not duplicated, matching this session's established pattern. The
CONVENTIONS.md L4→L2 boundary rule ("`agentengine::core` contains no `mcp::`/`a2a::` type") is genuinely
enforced almost everywhere — grepped `core/`, `trust/`, `workflow/`, `sandbox/`, `plugin/` for protocol
namespaces and found only two sites, both below.

### Finding K — `core/session_builder.hpp` names concrete protocol-backend types, a narrow but real letter-of-the-rule breach of CONVENTIONS.md's L4→L2 boundary

`session_builder.hpp:341,345` (inside `agentengine::quickstart`, a `core/` header) directly names
`agentengine::openai::OpenAIChatClient<Store>` and `agentengine::anthropic::AnthropicChatClient<Store>` —
both classified L4 by CONVENTIONS.md's own layout table. The rule's literal wording ("no `mcp::`/`a2a::`
type" in core) is written narrowly enough that this is arguably a materially different, smaller violation
than a wire-type leak — no OpenAI/Anthropic wire JSON crosses into `session_builder.hpp`, only the
conformer class *name* does, behind `#ifdef AGENTENGINE_WITH_HTTPS`, selected generically via a
`primary_client<Provider, Store>` trait for quickstart convenience. Confirmed via grep this is the only
site in the whole non-protocol tree doing this. **Disposition: tracked, not closed.** Worth a real decision
rather than silent tolerance either way: an explicit CONVENTIONS.md carve-out for concrete-backend-selection
convenience code, or moving this piece of `session_builder.hpp` out of `core/` into something L4-adjacent
(e.g. a `quickstart/` top-level dir). Low urgency — one file, one build-flag-gated site.

### Finding L — `conformance/` (the I7 gate's declared home) is empty scaffolding; the real conformance-flavored proofs live under `tests/` instead, untagged with a protocol revision

`conformance/README.md` is the only file in the directory. CONVENTIONS.md's layout table lists
`conformance/` as load-bearing and distinct from `tests/`, and its protocol-code rules require conformance
suites to be "tagged with [the protocol] revision." Real conformance-relevant proofs exist —
`tests/test_rt_cross_surface_equivalence.cpp` is the actual 013 §6 G3 proof (AG-UI/A2A projection
equivalence) — but they're filed as ordinary `tests/` entries with no revision tag, not where the project's
own binding contract says they should live. **Disposition: tracked, not closed.** Not "no conformance work
exists" — it's a location/naming mismatch that makes `conformance/` misleading to a reader trusting the
layout table (reads as unbuilt; isn't entirely).

### Finding M — both of ADR-005's Judged cross-process capability-delegation designs have zero production consumers, and this isn't named as a residual anywhere

`decisions/ADR-005-capability-bearer-tokens-cross-process.md` is Judged: Design A
(`trust/capability_token.hpp`, HMAC bearer token, accepted as default) and Design B
(`trust/capability_registry.hpp`, host-side opaque-ref registry, kept for immediate-revocation cases) are
both real, tested code. Grepped every real (non-test) `#include` of either header across `include/`/`src/`:
**none** — specifically checked `src/backends/remote/`, `src/backends/wasm/`, and `protocol/` (the exact
boundaries both headers' own comments name as their reason to exist: "the `remote` sandbox profile,
delegated A2A calls, a remote plugin") and found zero references. Same shape as the already-known
`agent.spawn`/`SpawnBudget` gap (small-proved-standalone-primitive, not yet wired) — but unlike that one,
this is not self-disclosed anywhere as a tracked residual. **Disposition: tracked, not closed.** Not a
defect in the primitives (both ADR-Judged, tested) — the boundaries that would consume one of them either
don't exist yet (`remote` backend, Finding confirmed empty below) or don't use real delegation yet. Worth a
one-line residual note in ADR-005 or `decisions/README.md`, the same lesson this session already drew from
ADR-014 §8.5's entry-point-census risk: "Judged" is not "wired."

### `sandbox/` seam + `src/backends/{wasm,native_jail,remote}` cluster

**No finding — isolation-parity holds structurally.** `WasmBackend`/`NativeJailBackend`/
`LinuxNativeJailBackend` each carry a compile-time `static_assert(SandboxBackend<T>, ...)` against the
identical 3-method+traits contract — CLAUDE.md's "gate, not goal" rule is genuinely enforced, not just
argued. **No finding — `remote/` is real vocabulary, zero implementation, honestly disclosed**: its
`README.md` states plainly this is deliberate, pending its own design→red-team→prove→judge pass; `sandbox.hpp`
already anticipates it structurally. Confirmed still accurately empty, on purpose.

### Finding N — the pre-mediation `ShellRunner`/`RealFileSystemAdapter` pair is dead in production, kept alive only by its own tests, with a stale top-comment actively misdirecting a reader

`real_filesystem_adapter.hpp`'s own top comment calls itself *"the ONLY `FileSystemAdapter` implementation
this project builds today... this project's actual near-term implementation target"* — stale since
`MediatedFileSystemAdapter` (ADR-014's real, capability/quota/TOCTOU-safe containment, verified wired end-
to-end in an earlier round this session) was built after and supersedes it for every real path. Grepped
every non-test reference to `ShellRunner`/`RealFileSystemAdapter`: **zero** production instantiation sites
anywhere outside `tests/`, yet both remain compiled into the default build target. Same shape as this
session's already-logged `HistoryAndSkillsProvider` finding — an earlier-generation spike, fully superseded,
not removed. **Disposition: tracked, not closed.** Low risk (unreachable from any real capability-granting
caller), but genuinely dead weight with an actively misleading comment.

### Finding O — `native_jail_backend.hpp`'s own header comment still promises a unification with the real Python/shell execution path that current locked architecture may have permanently obsoleted

The backend's comment states real `PythonRunner`/`ShellRunner` are expected to "become real Runners
plugging into this same `SandboxBackend` in M3." Checked whether that happened: the real wiring
(`tool_bridge.hpp`) routes `MediatedPythonRunner`'s `call_tool` straight to `core/tool_pipeline.hpp::invoke_tool`,
**never** through `NativeJailBackend::create/exec/destroy`. Real Python/shell execution today never enters
the `SandboxBackend` lifecycle at all — isolation is entirely embedded-CPython interpreter-level mediation,
consistent with CLAUDE.md's own locked decision ("the embedded native CPython interpreter is the one
mediated code-interpreter path, permanently... isolation strength comes from treating the whole execution
environment as the sandbox, not a second local isolation technology"). That's a coherent, deliberate
architecture — but `native_jail_backend.hpp`'s own comment (claiming it "alone does NOT close the
filesystem boundary... that composition happens once PythonRunner/ShellRunner route through this backend")
reads as still expecting a routing-through the locked decision may have permanently obsoleted for the
Python case specifically. **Disposition: tracked, not closed.** Not a security gap (ADR-014's mediation is
real and verified wired) — a documentation/expectation mismatch worth a maintainer pass, either updating
the comment or confirming routing is still intended for the *shell*-spawning-a-real-external-process case
specifically (less obviously covered by the "permanently embedded interpreter" rule, which is Python-
specific by its own wording).

### `core/corpus_source.hpp` / `corpus_chunk.hpp` / `embedder.hpp` / `mounted_skills_state.hpp` / `skill_tool_scoping.hpp` — finer-grain pass, no finding

One level deeper than the earlier "one class per kind" confirmation for the top-level Memory/RAG/Skills
split: `CorpusChunkRecord` is a deliberate third storage artifact (closes ADR-063 §4 findings 1/2 by
design); `corpus_source.hpp` honestly lists five residuals it does not solve (stale-chunk GC, index
persistence, a concurrent-writer race, unexamined symlink policy, no unmount lifecycle) rather than hiding
them; `embedder.hpp` cleanly mirrors `ChatClient`'s shape with one named, deliberate I5 asymmetry (no
Recording/Replay determinism harness); `MountedSkillsState` (mutable activation set) vs
`skill_tool_scoping.hpp` (pure filter function) split logic correctly, and the latter's self-flagged I3
hazard (declare-side/invoke-side tool lists must be recomputed on the same per-turn cadence or a "hidden"
tool stays silently callable) was spot-checked against its one real caller (`tools/cli_chat.cpp:595`) and
confirmed honored, not just asserted.

### Finding P — `SkillsProvider`'s private-store divergence (already known) has a downstream resume consequence not previously named

Re-confirmed at the construction-code level (not just the header comment) that `SkillsProvider` owns a
private, in-memory, per-instance object/ref store (`skill_provider.hpp:268-279`) rather than participating
in 025 §3's shared session worktree — already self-disclosed as an "As-built note" in 025 itself. The new
piece: `src/backends/native_jail/skill_mount_materializer.hpp::materialize_skill_mounts` copies that
private store out to a real host directory exactly once, explicitly "before a session's sandbox singleton
is ever constructed... not re-invoked mid-run" (its own comment). Because the private store is pure
in-memory and materialization is one-shot-pre-construction, **a session resumed in a new process
(`FileSessionStore::load`) has no automatic path back to a re-materialized skill mount** — a fresh
`SkillsProvider` re-resolving from its durable `sources_` plus a fresh `materialize_skill_mounts()` call
would reproduce it, but nothing wires that automatically on resume today. This is the same *shape* of gap
as Finding I (workflow-executor worktree resume needing manual re-wiring) — a second, independent instance
of the same pattern rather than a new root cause. **Disposition: tracked, not closed.**

### Round summary

Seven tracked findings this round (J-P, skipping none), zero closed, several substantial no-finding
confirmations recorded for future-round signal. Two findings (M, N) are "real but unwired/dead code,
already-Judged or already-superseded" — the same shape this session has now found three times
(`ComposedContextProvider`'s fork-aliasing residual, ADR-005's two designs, `ShellRunner`) — worth
noticing as a pattern in its own right: this codebase accumulates real, tested, ADR-backed mechanisms
that later work supersedes or never wires up, without a standing practice of marking them dead. Not
proposing a fix for that meta-pattern here — flagging it as something worth raising when this tracker
next gets reviewed as a whole.

---

## 2026-08-22 — Review pass: fact-checking this tracker's own citations and claims

User asked to run a review pass over this file itself, not new components. Four parallel forks, each
covering one section, re-verified every cited `file:line`, every quoted comment, and every behavioral
claim (A–P plus every "no finding" paragraph) against real code rather than trusting the tracker's own
prose — same discipline as every finding above, turned on this document. Result: everything holds.
Two trivial citation-line drifts found and left uncorrected as not worth a diff (Finding H's
`OpenQuestions.md` quote anchors at line 456; the quoted sentence actually starts at 454 — the quoted
text itself is verbatim-accurate. The worktree section's `merge_subtrees` citation says `504-583`; the
function actually runs `504-586` — 3 lines off at the tail). One real omission, corrected below.

### Correction to Finding N — the reason `ShellRunner`/`RealFileSystemAdapter` stay in the default build was missing

Finding N's own claim (zero non-test production instantiation, `MediatedFileSystemAdapter` supersedes
it for every real path, stale top-comment) is all still accurate and unchanged. What Finding N did not
say, and should have: staying in the default build target is not oversight, it's a **documented
decision**. `CMakeLists.txt:86-113` builds `agentengine_shell_runner` as a deliberately separate STATIC
library specifically so ADR-001 §7 finding 1's fix — Sh-S1's "zero references to a process-creation
primitive" check — can be verified at link-target granularity against the actual built artifact
(`tests/test_shell_runner_proof.cpp`), not against prose. Line 113's own comment calls out
`real_filesystem_adapter.{hpp,cpp}` by name as "off-limits to reuse" — ADR-001 decision 4 — precisely
because the mediated replacement (`agentengine_mediated_shell_runner`, same file, lines ~106-118) is
required to share *no* source with this proof target. So "why is this still compiled" is not an open
question — it's answered, just not in the header a reader of `real_filesystem_adapter.hpp` would
actually see. Finding N's severity/disposition is unchanged (tracked, not closed) — this only fixes
what would otherwise read as "nobody knows why this is still built."

---

## 2026-08-23 — Second review pass: a real correction to Finding F, and two trivial fixes

Ran a fresh, independently-verified fact-check across the whole file (four parallel forks, one per
dated section, each re-deriving claims from current code rather than trusting this file's own prior
"everything holds" verdict). Result: one real factual error found and corrected below (changes a
finding's conclusion, not just a citation), one arithmetic slip fixed, one completeness gap noted, all
other citations/quotes/behavioral claims across A–P re-verified accurate.

### Correction to Finding F — shell `cat`'s real read path has no size-cap mechanism at all, not "the same optional cap" as `mount_read`

Finding F step 2 claims native `bash`'s `cat` builtin reads via "the same `mount_read` path, same
optional cap" as `core/worktree.hpp::mount_read`. **This is wrong.** Traced the real call chain:
`mediated_shell_dispatch.cpp`'s `cat` (lines 149-153) calls `require_fs_read()` (lines 62-67, checking
only capability-grant authorization via `find_fs_read`, never a size cap) then
`fs.read_file(target)`. The real `FileSystemAdapter` implementation backing that call is
`MediatedFileSystemAdapter::read_file` (`mediated_filesystem_adapter.cpp:58-74`), which goes through
`open_within_mount_root` — the real-OS-backed handle-based containment layer this tracker's own
worktree section calls "layer 2" — and reads the whole file via `GetFileSizeEx`/`ReadFile`. It never
calls `core/worktree.hpp::mount_read` (layer 1) at all, and confirmed by grep: `size_cap_bytes` is
referenced in exactly two places in the whole `src/backends/native_jail/` tree —
`core/worktree.hpp:1322` (`mount_read`'s own enforcement) and `mediated_python_runner.cpp` (Python's
`open()` bridge) — **never** in `mediated_filesystem_adapter.cpp` or `mediated_shell_dispatch.cpp`.

**Net effect on Finding F's own conclusion**: the tracker's original chain implied a host *could*
mitigate the unbounded-`cat`-read risk by setting `cap::FsRead.size_cap_bytes` on the shell's read
grant. That knob is never consulted on this path. The real gap is strictly worse than originally
described — there is no existing size-cap mechanism reachable through shell `cat` at all, opt-in or
otherwise, not merely an optional one that defaults off. Finding F's overall disposition (tracked, not
closed, compounds Finding E) is unchanged; only the size of the gap it describes is corrected upward.

### Fix to the parallel-sweep round summary — arithmetic error, not a citation drift

"Nine tracked findings this round (J-P, skipping none)" (end of the parallel-sweep section) is wrong by
simple count: J, K, L, M, N, O, P is seven letters, not nine, and the round's own body only ever
describes seven findings. Corrected in place below (search "Nine tracked findings" if diffing).

### Noted, not corrected — Finding H doesn't cite the closest adjacent artifact

`decisions/ADR-059-invoke-agent-tool-capability-attenuation.md` (2026-08-14) is directly about
`agent_registry.hpp::invoke_agent_tool()`, which its own text calls the mechanism a real
`agent.spawn`/agent-to-agent call path would eventually build on, and it fixes a real I2
ambient-authority bug there (ADR-059 §3 explicitly disclaims building `agent.spawn` itself, so this
doesn't contradict Finding H's "no real call path" conclusion). Finding H cites only
`OpenQuestions.md`/ADR-031 and never mentions ADR-059 — a reader relying on Finding H alone would miss
that the one real piece of spawn-adjacent glue that does exist just had a capability leak fixed in it,
still awaiting judge sign-off. Not correcting the finding text for this (doesn't change its conclusion),
recorded here as a completeness note for whoever next touches this area.

---

## 2026-08-23 — Fixes applied: Finding F (`cat` size-cap), Finding N (stale comment), Finding M (residual note)

Project owner directed fixing the safe, no-ADR-needed items plus Finding F's `cat` gap specifically
(explicitly accepting that the latter is a real runtime/security-relevant change, not deferring it
behind a full design→red-team→prove→judge pass — a narrow, deliberate exception to CLAUDE.md's usual
gate for security-critical changes, justified here because the fix only completes enforcement of a
capability field (`cap::FsRead.size_cap_bytes`) that was already ADR-designed and already partially
enforced elsewhere (`mount_read`), not because it invents new security policy).

**Finding F — partially closed.** `mediated_shell_dispatch.cpp`'s `require_fs_read` now returns the
resolved `cap::FsRead` grant (not just success/failure); `cat` checks `data->size()` against
`granted.size_cap_bytes` after the read and fails closed with `shell.cat_exceeds_size_cap` (a
catchable, ordinary command failure, not a hard stop — same treatment as the existing quota-exceeded
errors) if exceeded. New regression test `E3-Q6` (`tests/test_mediated_shell_runner_smoke.cpp`) proves
an oversized file is refused and never reaches `stdout_text`; the whole `test_mediated_shell_runner_smoke`
suite plus its two dependents (`test_worktree_mount_sync`, `test_mediated_shell_runner_hostile_corpus`)
still pass in full. **Known compromise, disclosed, not fixed here**: the check runs AFTER
`fs.read_file()` returns, not before — `FileSystemAdapter` has no stat-only size probe, so (matching
`mount_read`'s own identical precedent) the oversized read still happens in memory; only the escape
into `stdout_text`/context is prevented.

**Finding F, Python twin gap — also closed (2026-08-23, same session, follow-up).** The SAME
`size_cap_bytes`-unenforced gap found while implementing the `cat` fix above also existed on
`mediated_python_runner.cpp`'s `open()` read bridge. Fixed there too, and actually more cleanly than
`cat`'s own fix: `Internal_open`'s read branch now captures the resolved `cap::FsRead` grant (was
previously discarded after a truthiness check, the same `find_fs_read`-return-value pattern `cat`'s fix
already established), and — because this call site already holds a real, already-opened
`SafeFileHandle` from `open_within_mount_root` before ever wrapping it into a Python file object —
checks `GetFileSizeEx` on that handle against `granted_read.size_cap_bytes` and raises
`PermissionError` BEFORE any bytes are read, not after. This closes the one disclosed compromise in the
`cat` fix (reading fully into memory before the check can run) for this call site specifically; `cat`
still has that residual (`FileSystemAdapter`'s interface has no stat-only probe, out of scope to widen
that interface for this fix). New regression tests `E2-C11` (oversized file → `open()` itself raises
`PermissionError`, content never reaches guest code) and `E2-C12` (positive control: a cap with
headroom still succeeds) added to `tests/test_mediated_python_runner_smoke.cpp`; full suite re-run and
passes (Release config — this target has no vendored Debug-mode CPython lib, a pre-existing,
unrelated-to-this-fix build environment constraint, confirmed by reproducing the same link failure on
the pre-fix tree via `git stash`).

Finding F's remaining scope — Finding E's fail-closed-vs-trim design question for the per-contributor
`ContextBudget` mechanism — is still tracked, not closed; that one needs a real design decision, not a
wiring completion, so it stays out of this session's fix scope.

**Finding N — comment fixed.** `real_filesystem_adapter.hpp`'s top comment no longer claims to be "the
only `FileSystemAdapter` implementation this project builds today" or "this project's actual near-term
implementation target" — replaced with an accurate statement naming `MediatedFileSystemAdapter` as the
real, production-used implementation and explaining (matching the already-correct account already
recorded in this tracker's prior review-pass section) why this type still ships in the default build on
purpose. `native_jail_backend.hpp`'s stale M3-unification comment corrected too, along the same lines as
Finding O's own text. Nothing about WHY these files still build changed — only the comments a reader
would actually see now say so.

**Finding M — residual note added.** `decisions/README.md`'s ADR-005 row now states plainly that both
Design A (`capability_token.hpp`) and Design B (`capability_registry.hpp`) have zero production
consumers today. The designs themselves are unchanged and still unwired — this is a documentation fix,
not a wiring fix, so Finding M's own disposition (tracked, not closed) is otherwise unchanged.

---

## 2026-08-23 — Findings A and B closed: the ContextProvider composition cluster consolidated

The tracker's own "Recommended follow-up" (end of the 2026-08-22 `ContextProvider` composition cluster
section, above) — one coherent type instead of `HistoryAndSkillsProvider`/`ComposedContextProvider`/
`LazyComposedContextProvider` as three separate ones — was implemented this session, following a plan
reviewed and approved before any code was written, plus a new ADR
(`decisions/ADR-074-composed-context-provider-consolidation.md`, Judged, project-owner sign-off).

**Finding A (`HistoryAndSkillsProvider` redundant) — closed.** Deleted
(`include/agentengine/core/history_and_skills_provider.hpp`). Its two real call sites
(`tests/test_rt_agent_session_real_backend.cpp`, `tests/test_rt_agent_session_skills_live_e2e.cpp`)
migrated to `ComposedContextProvider<Skills, History>` — args reordered (skills first), not left in the
old `<History, Skills>` order, since `ComposedContextProvider`'s wire order is its declared order,
always, unlike the old type's own hard-coded skills-first constructor. The real ordering regression
check (`R3b`, `test_rt_agent_session_real_backend.cpp`) still passes: `skill_pos < history_pos` on the
real wire body.

**Finding B (`ComposedContextProvider`'s fork-aliasing bug) — closed at the source.**
`ComposedContextProvider` is now move-only (copy ctor/assignment deleted), carrying forward
`LazyComposedContextProvider`'s own already-red-teamed fix (round 5 finding A: a correct moved-from
`engaged_`/`contributors_` reset, not a naive `=default` move) verbatim. `AgentSession::fork_from()`'s
plain `history_provider_ = source.history_provider_;` is now a compile error for any session using this
type as `HistoryProviderT`, closing the I1/I4-adjacent aliasing gap the tracker's own live-reproduced
probe found. New regression test (`tests/test_composed_context_provider.cpp` Part 3) proves this
directly on the type itself, not just through `ComposedQuickstartSessionBuilder`'s own indirection:
`static_assert(!std::is_copy_constructible_v<...>)`, plus a runtime move-no-aliasing proof (mutate via
one instance, move it, confirm the moved-from instance is genuinely `not_engaged` and the moved-to
instance carries the real content).

**A real bug found and fixed DURING implementation, not anticipated by the plan**: see
`decisions/ADR-074-composed-context-provider-consolidation.md` §4 for the full account — an early
"auto-engage the default constructor when every `Ms` is default-constructible" design silently broke
`ComposedQuickstartSessionBuilder::build()` for any real caller whose chosen providers happened to all
be default-constructible (`.engage()` would fail with `already_engaged`), caught by
`tests/test_session_builder.cpp`'s own pre-existing B22 test cascading into 5 failures before the fix.
Resolved by making default construction always start unengaged, unconditionally — matching
`LazyComposedContextProvider`'s original, simpler behavior — with four call sites that relied on the old
eager auto-engage ergonomics updated to call `.engage()` explicitly.

**Verified clean**: every real/test call site in the tree rebuilt and re-run —
`test_composed_context_provider`, `test_session_builder` (full B14–B22 suite), `test_tool_optimizer_provider`,
`test_rt_agent_session_context_provenance`, `test_rt_agent_session_real_backend`,
`test_native_capability_announcer` all pass; `test_rt_agent_session_skills_live_e2e` compiles and
gracefully skips (no live OpenRouter key in this environment). `session_builder.hpp`'s own extensive
round-4/5/8 red-team narrative (file-top comment) is preserved verbatim as a point-in-time historical
record, with a short dated note redirecting a reader to where those same findings now live.

---

## 2026-08-23 — Finding E closed: budget-exceeded is now a hard failure, not a silent trim

The project owner directed a real redesign of `assemble_context()`'s budget-exceeded path (explicitly
authorizing a from-scratch rewrite of the mechanism, not a reuse-preserving patch, if it produced a
cleaner result), matching the fix shape already recorded here on 2026-08-22 but left unimplemented, plus
resolving the "named tension" that section left open. New ADR:
`decisions/ADR-075-context-budget-fail-closed.md` (Judged, project-owner sign-off).

**`assemble_context()`'s return type widened** from `task<ContextAssemblyResult>` to
`task<result<ContextAssemblyResult>>` (`context_assembly.hpp`). A contributor whose own contribution
exceeds its own declared `ContextBudget.max_tokens` now fails the WHOLE call closed — `failure_class::resource`,
code `"context_assembly.contributor_budget_exceeded"`, naming which contributor (index and declared name)
and the measured-vs-declared token counts — instead of silently trimming that contributor's oldest
messages and returning success. Matches the already-shipped `token_budget_` precedent
(`rt/agent_session.hpp`) at a different layer, rather than inventing a second shape.
`ComposedContextProvider::on_context()` propagates the failure verbatim rather than unwrapping
`ContextAssemblyResult.drops` into a silently-still-successful `ContextContribution`.

**The "named tension" (trim-vs-error dual behavior) resolved by checking real usage first, not by
guessing.** Grepped every non-test `ContextBudget{...}` construction in the whole tree before deciding —
zero real callers set a nonzero `max_tokens` today. There was no shipped graceful-degradation case to
preserve alongside the cleaner behavior, so no config knob was added to choose between "trim" and
"error" per contributor — one behavior, matching this session's now-twice-applied "prefer a clean
redesign over a patch that preserves an existing compromise" guidance (`ADR-074`'s own §2 was the first
application). See the ADR's own §3 for the full reasoning.

**`ContextAssemblyResult.drops`/`ContextDrop` are kept, not deleted** — confirmed (independently, via
`decisions/ADR-049-cross-provider-reasoning-exclusion.md` §2's own prior finding) that nothing else in
the tree ever produced a real one either, but the type remains real, load-bearing vocabulary
`TurnMiddleware::TurnContext` (`turn_middleware.hpp`) is built around independent of `assemble_context()`'s
own mechanism. Widening or repurposing that broader type was out of this fix's scope — flagged, not
touched.

**A stale comment found and fixed alongside this work, not part of Finding E itself**:
`composed_context_provider.hpp`'s own file-top comment still described the "auto-engage when every `Ms`
is default-constructible" design `ADR-074` §4 found broken and reverted — contradicting the constructor's
own comment three lines below it. Corrected to match the real, shipped behavior (always starts unengaged).
A second stale comment in `tests/test_composed_context_provider.cpp` making the same false claim about
`ThreeWayProvider` was corrected too.

**Verified clean**: `tests/test_context_assembly.cpp` (rewritten — proves the failure's class/code/attributed-contributor
plus an exactly-at-budget boundary case, in place of the old trim-proving assertions),
`tests/test_composed_context_provider.cpp` (new Part 1b proves the composite propagates the failure;
Part 1a keeps the order/tool-survival/turn-end-fan-out coverage, now with budgets out of the way),
`tests/test_context_provenance.cpp`/`tests/test_memory_provider.cpp` (signature-only migrations — neither
exercises a nonzero budget, confirmed unaffected). Full affected-target Debug rebuild and `ctest` run,
zero failures.

---

## 2026-08-23 — Follow-up: `sandbox/` cluster re-checked with a sharper question — does the embedded CPython interpreter actually sit inside any isolation boundary?

User asked directly whether LLM providers overlap (no finding — see git history of this session for that
check, not logged as its own dated entry since it produced no finding) and then asked to check `sandbox/`
specifically. The earlier parallel-sweep round (2026-08-22, above) already recorded "no finding —
isolation-parity holds structurally" for `sandbox/` + `src/backends/{wasm,native_jail,remote}`, and Finding
O already named that real Python execution never reaches `NativeJailBackend`'s create/exec/destroy
lifecycle. This round pushes one level further than Finding O did: not just "does Python route through the
jail," but "is the interpreter process contained by *anything* OS-level at all."

### Finding Q — the embedded CPython interpreter runs with zero OS-level resource containment; the "OS-level jail as a second layer" CLAUDE.md's own architecture calls for was never wired onto it

CLAUDE.md's own locked decision ("No `microvm` sandbox profile") states the intended architecture
explicitly: *"Isolation strength for the interpreter/shell comes from treating the whole execution
environment as the sandbox — worktree, capabilities, resource limits, network policy — with CPython's
dangerous entry points mediated at the point of use **and the OS-level jail as a second layer** (008
§1b)."* Two layers are named as the design: (1) CPython-internal mediation, (2) an outer OS-level jail.
Checked whether layer 2 is actually wired onto the interpreter process, not just cited as intended.

Layer 1 is real and verified in earlier rounds: `PythonLockdownInterpreter`/`MediatedPythonRunner`
(`src/backends/native_jail/python_lockdown.{hpp,cpp}`, `mediated_python_runner.cpp`) implement a genuine
C-level import allowlist (meta-path finder, ADR-002), a caller-gated import tier (ADR-003), and
capability-mediated file I/O through `MediatedFileSystemAdapter` (ADR-014). None of this is in question.

Layer 2 does not exist for this process, confirmed three ways:
1. **No `SandboxBackend` involvement at all** — `PythonLockdownInterpreter`/`MediatedPythonRunner` never
   construct a `SandboxHandle`, never call `create()`/`exec()`/`destroy()`, carry no
   `static_assert(SandboxBackend<...>)`. Consistent with, and one level more specific than, Finding O's own
   "real Python execution never reaches `NativeJailBackend` at all" finding.
2. **No OS resource cap of any kind on the interpreter's own process.** Grepped
   `mediated_python_runner.{hpp,cpp}` for every resource-limit vocabulary this codebase uses elsewhere
   (`JobObjectLimits`, `ResourceLimits`, `cgroup`, `rlimit`, `memory_bytes`, `wall_ms`, `cpu_ms`) — zero
   matches. The interpreter runs in-process with the host, sharing its memory/CPU, under no Job
   Object/cgroup/namespace at all.
3. **The one deadline check that exists is pre-flight only, not preemptive.** `tool_pipeline.hpp:579-583`'s
   own comment is explicit: *"deadline checked at the call boundary, not preemptible mid-call, decision 2
   defers it."* Once `PythonLockdownInterpreter::run()` starts executing guest code, nothing external can
   interrupt it.

**Net effect**: guest Python that never imports a blocked module (so layer 1's import lockdown has nothing
to catch) but simply spins (`while True: pass`) or allocates unboundedly in pure Python objects runs
unbounded, sharing the host process, with no OS-level kill, no mid-call timeout, no memory cap. This is a
real gap against the project's own "Machine safety" rule (CLAUDE.md: sandbox/hostile tests must be
resource-capped so a proof of containment can't itself take the machine down) — and a sharper one than
Finding O described, because Finding O's framing ("does Python route through the jail for filesystem
containment") left the resource-limit dimension unexamined.

**A real asymmetry worth naming**: `NativeExecRequest`/`spawn_native_process`
(`src/backends/native_process/native_process_spawn.hpp`, ADR-071) — a class of provider explicitly built
*weaker* than the sandboxed interpreter (no AppContainer at all, by design, per that file's own top comment)
— still gets `cpu_ms_cap`/`wall_ms_cap`/`memory_bytes_cap` enforced best-effort via the same Windows Job
Object primitive `NativeJailBackend` uses. So today, the deliberately-weaker native-process class has
*more* OS-level resource containment than the "one mediated code-interpreter path, permanently" class does.
Not evidence either mechanism is wrong on its own — evidence the two were built independently and never
reconciled on this one axis.

**Disposition: tracked, not closed.** This is a survey-and-log finding, not a same-session fix (per this
session's standing scope) — a real fix (wiring `PythonLockdownInterpreter`'s host process into
`NativeJailBackend`/`LinuxNativeJailBackend`, or giving it its own lighter Job Object/cgroup wrap the way
`native_process_spawn.hpp` already does) is a genuine design decision (which OS primitive, cross-platform
parity, whether the *shell*'s still-unbuilt process-spawn case per Finding O should be unified with this
fix rather than solved twice) that belongs behind this project's own `design → red-team → prove → judge`
gate for a security-relevant change, not a drive-by patch.

---

## 2026-08-23 — Deeper follow-up on Finding Q, at user's explicit request ("the sandbox missing safety
components is extremely concerning"): this is a spec-vs-shipped-code conflict, not merely an unwired
residual, and it is currently more severe than Finding Q described

### Finding R — the interpreter runs T3 (least-trusted) code with the RFC's own mandatory third defense
layer (the kernel-jail backstop) entirely absent, in-process with the host, and a same-day code comment
(under Finding O) already asserts this is permanently resolved without any ADR having actually decided that

Traced the RFC text itself, not just prior comments describing it, to check whether "interpreter-level
mediation only, no kernel jail" was ever an accepted design for the code interpreter specifically (as
opposed to Finding Q's framing, which treated the missing resource cap as an unwired piece of an otherwise-
agreed architecture).

**The RFC is explicit that the kernel jail is not optional for the interpreter, and says exactly why.**
`008-Sandbox-and-Isolation.md` §1b (lines 169–174) lays out the interpreter's isolation as three layers —
(1) closed import allowlist, (2) per-module dangerous-behavior mediation, (3) **kernel-level jail as
backstop** — and states the reason layer 3 exists in so many words: *"relying solely on interpreter
mediation has no answer for a bug in the mediation itself."* §3's profile table (line 262) defines
`native-jail`'s boundary as "Kernel-enforced... **plus** mediated at the point of use" — both, not either.
§3's own prose (lines 294–296) states plainly: *"the code interpreter, which is locked to `native-jail`
permanently."* `010-Python-Code-Interpreter.md` §2 (line 63) repeats it ("under `native-jail`") and §6
(line 246) states code executed via `execute_code` runs at **the sandbox's trust tier, T3** — and 008's own
profile table (line 264) states the in-process `none` profile is scoped to "first-party trusted code only;
**refuses to load T2/T3 code**." T3 code is, by the RFC's own vocabulary, exactly the class `none` is
defined to reject.

**What's actually shipped delivers `none`'s boundary while the RFC requires `native-jail`'s.** Confirmed in
the prior round (Finding Q) and reconfirmed here: `PythonLockdownInterpreter`/`MediatedPythonRunner` never
touch `SandboxBackend`/`NativeJailBackend` at all, and run as an ordinary in-process CPython embed sharing
the host binary's own address space — `tools/cli_chat.cpp`'s own comment states the runner is *"a SHARED,
process-wide `MediatedPythonRunner`"*, one instance for the whole process's lifetime (ADR-002 §5.5.6's own
scope: "one `PythonLockdownInterpreter` == one OS process"). So today, T3-classified guest code executes
with zero kernel-level containment, in the same address space as the host, every session's `AgentSession`
state, and any capability/secret material resident in that process's memory — exactly the failure mode
008 §1b names layer 3 to catch (a bug in layers 1–2, or in an allowed module's own native extension code —
NumPy/pandas are large C codebases explicitly named as intended `native-jail` cargo in 010's own table).

**This has already been characterized as settled, without an ADR, and that characterization does not hold
up against the RFC text.** `native_jail_backend.hpp`'s own "Correction (2026-08-23)" comment (written this
session under Finding O, by a different/concurrent audit pass) states: *"That closes the filesystem
boundary for Python by an entirely different, now-permanent mechanism than 'compose with this backend'"* —
read naturally, this asserts the kernel-jail requirement no longer applies to Python. But neither CLAUDE.md
nor 008/010 actually say that: CLAUDE.md's own "No `microvm` sandbox profile" paragraph names "the OS-level
jail as a second layer" as part of the intended architecture, not as superseded by interpreter mediation;
008 §1b's own reasoning (quoted above) is specifically about why interpreter mediation alone is
insufficient. No ADR was written to actually amend 008 §3's "locked to `native-jail` permanently" or 010
§6's T3 classification. Per CLAUDE.md's own governing rule — *"The project is spec-driven... When code and
a spec disagree, the spec wins; if the spec is wrong, fix the spec first (with an ADR), then the code"* —
this comment's framing is not a legitimate resolution of the conflict; it is an unreviewed reinterpretation
of a security-critical requirement, exactly the class of change CLAUDE.md's own `design → red-team → prove
→ judge` gate exists for.

**Compounding factors already established (Finding Q), restated here because they sharpen this finding's
severity rather than standing alone**: no OS resource cap of any kind on the interpreter process (no
memory/CPU/wall-clock kill), and the one deadline check that exists is pre-flight-only, not preemptible
mid-execution. So the missing layer-3 backstop is not merely "one more hardening pass away" — today there
is no resource ceiling either, meaning even the coarsest possible containment (kill a runaway or
memory-bombing guest) is absent alongside the finer-grained escape-containment the kernel jail would
provide.

**Disposition: tracked, not closed — flagged as this tracker's most severe finding to date.** Every other
finding in this file so far is a disclosed, understood gap (an unwired residual, a stale comment, a design
question deliberately deferred). This one is different in kind: the RFC states a specific security
requirement in specific, reasoned terms; the shipped code does not meet it; and the one place in the tree
that comments on the discrepancy asserts — without an ADR — that the requirement no longer applies. Recommend
this be raised to the project owner directly rather than left for the next routine audit pass: the two
live questions are (1) does 008/010's "kernel jail, permanently, for T3 code" requirement still hold, in
which case the interpreter needs real work to route through `native-jail` (or an equivalent lighter OS-level
wrap, matching `native_process_spawn.hpp`'s own precedent) — or (2) is interpreter-mediation-alone actually
the intended, accepted design going forward, in which case 008 §1b/§3 and 010 §6 need an ADR amending them
to say so explicitly, with the residual risk (mediation bugs, native-extension bugs, resource exhaustion)
named and accepted on the record rather than implied by an unreviewed comment. Either answer is legitimate;
leaving it undecided, with the code silently delivering the weaker of the two, is not.

**Update (2026-08-23) — Slice 1 implemented, question (1) above answered by action: real work.** Project
owner directed a full redesign (not a spec-downgrade) via `design → red-team → prove` (2 independent
design candidates, judged and merged; 3 parallel red-team lenses — authority-leak, resource/DoS/lifecycle,
parity/buildability; a finalized spec; real implementation + real tests, all independently re-verified —
built and re-run on this machine, not taken on the implementing agents' own word). Result: the embedded
CPython interpreter now runs as a genuinely separate, AppContainer + Job-Object-jailed Windows worker
process (`src/backends/native_jail/python_worker_main.cpp`, `jailed_worker_rpc.{hpp,cpp}`,
`mediated_python_worker_protocol.hpp`, `python_worker_mediation.{hpp,cpp}`), spawned/owned by
`NativeJailBackend::create_python_worker()`/`exec_session()`, with a real session-scoped watchdog thread
that preempts mid-execution (`while True: pass` under an 800ms budget: killed in ~810ms, verified directly)
and a real Job Object memory cap (512MiB allocation under a 64MB cap: contained, verified directly) —
closing Finding Q's "pre-flight-only" gap and Finding R's "zero kernel backstop for T3 code" gap for real,
not by comment. `call_tool`/`agent.tools`/`agent.ask` still relay through the real host-side tool pipeline
(`bridge_tool_call`) with `exec_seq` anti-replay and single-flight (`call_mutex`) closing the authority-leak
red-team lens's own finding.

**Named, not silently dropped: Slice 2 is a real, disclosed residual, not a hidden regression.** Real
file-open/listdir/socket-connect relay (`HandleRelay`) was explicitly out of Slice 1's scope (the
finalized spec's own minimum-slice boundary) — every such guest call is denied
(`not_implemented_this_slice` → guest-visible `PermissionError`) rather than reaching the filesystem/network
today. This is a genuine regression against the pre-jail behavior for six existing test files
(`test_mediated_python_runner_smoke`/`_agent_files_data`/`_skill_mounts`/`_error_mapping`/`_hostile_corpus`,
`test_reference_agent_task_corpus`) — confirmed by re-running them: every failure is a clean, disclosed
deny (file/socket-dependent checks only), never a crash or silent wrong answer. Linux
(`LinuxNativeJailBackend`) and `execute_shell`'s own still-separate process-spawn question (Finding O) were
also explicitly out of scope for this pass, per the same spec.

**One real, undisclosed-until-independent-verification bug found and fixed in this pass, worth naming as
its own lesson**: `tests/test_agent_session_suspend_codeact_ask.cpp`'s B7 scenario (a residual-demonstration
test for ADR-057 §4, unrelated in its own original intent to this redesign) silently hung the whole test
run for the full 60s `ctest` timeout instead of failing cleanly — its old body assumed the mediated write
it depends on always succeeds and always calls `session.open_interactions().front()` unconditionally; under
Slice 1 that write is now denied pre-`agent.ask()`, so no Interaction ever opens and `.front()` on an empty
vector was UB, invisible in this build because the file never called `support/crt_fail_fast.hpp`'s
`fail_fast_on_windows()` (every other native-jail test in the suite does). Fixed: the test's assertions now
match Slice 1's real behavior, the empty-vector case is asserted rather than blindly indexed, and
`fail_fast_on_windows()` is now called — B7's ORIGINAL scientific claim (ADR-057 §4's replay-repeats-writes
residual) is preserved in a comment, not deleted, with an explicit note to restore it once Slice 2 lands.
Independently reproduced the 60s hang myself before applying the fix, and reproduced the fix (0.89s, passes)
after — this was caught by actually running the implementing agents' own test suite personally, not by
trusting their self-report, the same discipline this whole tracker file is built on.

**Disposition: Finding Q closed for real (preemptible wall-clock + memory containment, verified). Finding R
narrowed, not yet fully closed** — Slice 1 gives `execute_code` a genuine kernel-level backstop on Windows,
answering this finding's own live question in favor of "the requirement still holds, build the real thing"
rather than a spec-downgrade ADR. Still open: Slice 2 (real file/socket relay, closing the six disclosed
regressions above) and the Linux port (`LinuxNativeJailBackend` parity) — both named, scoped follow-on work,
not silently dropped. No ADR has been written for this yet; the design/red-team/prove record lives in this
tracker entry and the workflow's own transcript until one is.
