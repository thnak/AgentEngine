# Design draft: real session→sandbox lifecycle wiring (Phase 2 of the "every session gets a
# real sandbox" roadmap)

**Status:** Design draft, Revision 6 — a third red-team pass (2026-08-25) CONFIRMED Revision 5's
gap 1 and gap 3 resolutions, but REFUTED gap 2's stated evidence: "no second caller of
`clear_in_process_state()` exists" was false, the same overclaim-from-an-incomplete-grep pattern
this draft has now repeated three times (Rev 1's fabrication, Rev 3's unproven claim, now this).
Gap 2 is reopened and corrected below, not dismissed. **A fourth red-team pass, same day, then
independently re-verified Revision 6's own correction against the real test files and
`composed_context_provider.hpp` directly and found it accurate on every point checked** — the
first round of four on this draft to confirm a self-directed revision without finding a new error.
Still not an ADR — see §4. Written as the direct
follow-on to two things that are now real, built, and tested in this
tree: the `sandbox-backend-registry-design-draft.md` / `ADR-080` pair (Proposed, awaiting Judged
— resolves *which* `SandboxBackend` a deployment picks, explicitly does **not** wire any real
consumer), and this session's own Tier-1 work (`EffectContext::sandbox_fs`, `SessionShellSandbox`
in `src/backends/native_jail/session_shell_wiring.hpp`) which just became the **second** real,
production-shaped sandbox consumer in this codebase — and, like the first (`cli_chat.cpp`'s
Python wiring), it deliberately bypasses `SandboxBackendRegistry`/`SandboxHandle` entirely. That
repetition is the actual finding this draft is built around.

## Revision 2 (2026-08-25) — what the red-team found and how this draft changed

Three independent agents red-teamed Revision 1 in parallel (security/I2-I3, C++ correctness,
architecture-fit against real ADR text). One finding is serious enough to change this draft's
central thesis; the rest sharpen or correct specific claims.

**Confirmed, load-bearing — fixed in this revision:**

1. **Revision 1's claim that Option B "doesn't touch ADR-029/030's already-Judged claim semantics
   at all" is false as concretely specified, and the false part is specific: `CodeActRunnerBinding`
   "release-on-drop."** The architecture-fit reviewer checked `include/agentengine/core/
   codeact_runner_binding.hpp` directly — it has **no release method at all**. Its own top comment
   states the design intent plainly: multiple sessions were never meant to share one interpreter's
   lifetime *even one-at-a-time* — `bind()` is first-caller-wins for the life of the process, full
   stop, exactly because ADR-030 evaluated and rejected a release/reclaim scheme (cwd/env/the work
   mount are real OS-process-global resources that a per-session `ExecState` cannot isolate no
   matter how the binding is owned). Revision 1's §2 item 2 invented a "release-on-drop" mechanism
   that does not exist and, if built, would reopen ADR-030's exact rejected hazard *sequentially*
   instead of concurrently — a real regression, not a paperwork error. **Fixed in §2 below**:
   Option B's session-lifecycle hook now applies fully to Shell (which has no such constraint) and
   only partially to Python (construct-lazily-on-first-use, yes; release-on-teardown, no — a
   session that has bound the interpreter keeps it bound for the process's life, exactly as
   `CodeActRunnerBinding` already ships). No new capability is proposed to close that gap; it stays
   exactly as ADR-030 left it.
2. **Real gap Revision 1 never named: `AgentSession::fork_from()` and `clear_in_process_state()`
   both do `effect_context_ = EffectContext{}`, silently zeroing `sandbox_fs` back to `nullptr`.**
   The C++ correctness reviewer traced this directly (`agent_session.hpp:1197`/`1235`) and found it
   is the same class of hazard already named in-repo for `capabilities_` (line 1187's comment:
   callers must re-call `set_capabilities()` after `fork_from()`) — but `sandbox_fs` has no
   equivalent documented re-arm story. Fails toward `nullptr` (safe direction — downstream tool
   checks already treat that as "no sandbox," per Tier-1's own `ReadSandboxFile` test), so this is
   a silent capability *loss*, not a security hole, but it needs an explicit, tested re-arm step
   the same way `capabilities_` has one. **Added to §2 below.**
3. **Real gap Revision 1 glossed over: no seam exists today to write `sandbox_fs` from outside
   `AgentSession` at all** — unlike `capabilities_` (`set_capabilities()`), there is no
   `set_sandbox_fs()`/mutable accessor anywhere in `agent_session.hpp` (grep-confirmed by the
   correctness reviewer). Revision 1's Option B framing implied this seam either already existed or
   was trivial to bolt on "beside" `AgentSession` without touching it — false. A new public mutator
   on `AgentSession` itself is required, which means Option B **does** touch `AgentSession`, just a
   narrower slice of it than Option A would. **Corrected in §1/§2 below** rather than left as an
   unstated assumption.
4. **Revision 1 overclaimed that Option B closes `ADR-080`'s own named residual.** The
   architecture-fit reviewer read `ADR-080` §8 directly: the residual is specifically about a real
   execution path *constructing and using* a `SandboxHandle`. Option B never does that for
   Python/Shell — it only reads a resolved backend's *name* to pick a mount-directory policy, which
   is not "using what comes back" in the sense `ADR-080` means. **Corrected in §1 below**: Option B
   narrows `ADR-080`'s residual (a real, if partial, step toward resolving it) but does not close
   it; `wasm`'s tool-bridge path remains the only real `SandboxHandle` consumer either way.
5. **The per-session mount-policy resolution step (§2 item 1) named a need but not an algorithm,
   and specifically never pinned down that its key input must be host-code-only by construction.**
   The security reviewer traced the one real precedent (`cli_chat.cpp`'s single hardcoded shared
   scratch directory for every session) and found no existing per-session derivation to point to as
   a safe template — if a real implementation later derives the path from `session_id` or any
   agent-supplied label, the draft must say explicitly that value is host-minted, never traced back
   to `agent.spawn`'s `agent_id` or any model-influenced string, the same way `HostSandboxSelection`
   already forecloses non-host construction for backend names. **Added as an explicit requirement
   in §2 below**, not left implicit.
6. **Minor misattribution, fixed**: Revision 1 repeatedly bundled "ADR-029/030" as one risk. Only
   ADR-030 has claim/bind semantics; ADR-029 (suspend-for-approval) has no claim mechanism at all.
   References below now name ADR-030 specifically.

**Reviewed and cleared, unchanged:**

- The security reviewer found `sandbox_fs`'s *population* mechanism itself preserves the dynamic-
  check discipline `ReadSandboxFile` already establishes — no confused-deputy path found; the
  residual risk is that nothing *forces* a future `Tool` built against `sandbox_fs` to replicate
  that own-check pattern rather than trusting non-null as authorization. Added as an explicit test
  obligation in §2.
- The security reviewer also found Option A, if pursued later, does not structurally lose per-call
  attribution (`EffectContext&` already threads through `SandboxBackend::exec()`'s real signature)
  — *provided* any such implementation keeps `exec()` invocation strictly inside `invoke_tool()`'s
  existing 10-step pipeline, never called from a hand-built `EffectContext` outside it. Noted for
  the record in Option A's own section, since Option A is still deferred either way.
- Architecture-fit confirmed Option B does not reopen `ADR-012` (template-parameter-kind is
  untouched — Option B operates entirely downstream of it) and stays orthogonal to CLAUDE.md's
  "no microvm profile" decision.
- The correctness reviewer confirmed one Revision-1-adjacent worry does *not* apply: `AgentSession`
  is heap-owned via `unique_ptr` and never moved after construction, so a `sandbox_fs` pointer set
  once at session-sandbox-construction time is not at risk from session relocation.
- The correctness reviewer also confirmed `EffectContext` is a **single, mutated-in-place member**
  on `AgentSession` (`effect_context_`), not reconstructed per call — meaning §2 item 3's "populate
  for every call" is actually simpler than Revision 1's phrasing implied: one assignment at
  lifecycle-construction time, already visible to every subsequent call under `session_mutex_`.
  Simplified in §2 below.

**Left open, not resolved by this revision**: whether `start_background_task()`'s by-value copy of
`EffectContext` onto a detached thread keeps a copied `sandbox_fs` pointer valid if the owning
`SessionShellSandbox` is torn down while that background task is still in flight — the correctness
reviewer flagged this as unverified (only `report_progress`'s reset is named at that call site
today). Carried into §3 as a genuinely open question for the next pass, not guessed at here.

## Revision 3 (2026-08-25) — Revision 2's open items resolved against real machinery, not guessed

Revision 2 left four real items open: how the lifecycle hook actually attaches to a session
without a `session_builder.hpp` step that doesn't exist, whether a new `AgentSession` mutator is
really required, what `fork_from()` should do about a stale `sandbox_fs`, and how the mount-policy
key's provenance gets pinned down. Re-reading `session_builder.hpp`'s own dense, real, three-round
red-team history (findings 8-11, `ComposedContextProvider<Ms...>`/ADR-074) plus
`context_provider.hpp`'s actual `ContextProvider` concept signature answers three of the four
directly, from already-Judged code, not from new design:

1. **`ContextProvider::on_context(SessionContext&, EffectContext& ctx)` already takes `ctx` BY
   MUTABLE REFERENCE** (`context_provider.hpp:101-104`, concept-checked). `AgentSession` already
   calls it every round (`agent_session.hpp:1099`/`1854`/`2116`/`2260` — four distinct branches
   of `run_rounds()`, each rebuilding `ToolTable const tool_table =
   ToolTable::from_descriptors(contribution->tools)` fresh from that same call), never once at
   session construction and never cached across rounds. **This supersedes Revision 2 finding 3.**
   A `SandboxToolProvider` conforming to this concept — constructing `SessionShellSandbox` lazily
   on its first `on_context()` call, holding it as its own member — can (a) contribute
   `run_shell`'s `ToolDescriptor` into `contribution->tools` and (b) assign
   `ctx.sandbox_fs = held_sandbox_->filesystem_adapter()` directly, in the same call, with **no
   new `AgentSession` mutator needed at all**. Revision 2's "no `set_sandbox_fs()` seam exists"
   finding was accurate as far as it went, but the fix isn't adding one — it's using the seam that
   already exists for exactly this purpose.
2. **This is not a new mechanism to design — it's the existing `Ms...`/`ComposedContextProvider<Ms...>`
   composition `ComposedQuickstartSessionBuilder::providers(...)` already supports today**, the
   same call a host already uses for `HistoryProvider<Window<N>>`/`SkillsProvider`/
   `VectorRagContextProvider`. A host that wants session-scoped shell wanting nothing more than
   `.providers(std::make_tuple(HistoryProvider<Window<8>>{}, SandboxToolProvider{host_root}, ...))`.
   **This supersedes Revision 2's still-open "does `session_builder.hpp` need a new
   `.with_sandbox(...)` step" question — no new builder step is needed**, the existing
   `.providers(...)` call is already general enough. (`SessionShellSandbox` itself, Tier-1's
   standalone factory, becomes the thing `SandboxToolProvider` constructs and owns internally —
   not replaced, just given a real caller.)
3. **`fork_from()`'s real, current body (`agent_session.hpp:1194`) does `history_provider_ =
   source.history_provider_` — a plain copy-assignment, not a move.** `session_builder.hpp`'s own
   findings 9/11 (round 4/5 red-team) found and fixed the EXACT hazard a stateful
   `SandboxToolProvider` composed via `Ms...` would reintroduce here — `shared_ptr`-captured
   provider state silently aliasing across a fork — by making `ComposedContextProvider<Ms...>`
   **move-only**, carried "into `core/composed_context_provider.hpp` itself... at the source" per
   that file's own 2026-08-23 update note (`ADR-074`). A `SandboxToolProvider` holding a
   non-copyable `std::unique_ptr<SessionShellSandbox>` inherits that same move-only property, so
   **composing it into `HistoryProviderT` makes `fork_from()` a hard compile error at line 1194
   for that session type** — the same "turn a silent runtime aliasing hazard into a compile-time
   one" fix ADR-074 already established for the identical problem shape, not a new one this draft
   invents. **This supersedes Revision 2 finding 2's framing** ("needs the same re-arm story as
   `capabilities_`") — the actual, structurally-safer answer is "a session composed with a live
   sandbox mount cannot be `fork_from()`'d at all, by construction, until a future design
   explicitly decides what fork-and-carry-a-sandbox should mean" (mirroring how `capabilities_`
   itself is "deliberately still NOT copied" by `fork_from()` today, per that function's own
   comment at line 1184-1187 — sandbox state joins capabilities as "the fork must start over,"
   not as "the fork silently inherits a live handle"). This needs verification (a real compile
   attempt against a real `SandboxToolProvider`, not just this reasoning) before being trusted —
   flagged in §3, not asserted as proven.
4. **Mount-policy key provenance — no new wrapper type needed, unlike Revision 2 speculated.**
   `SandboxToolProvider`'s constructor argument (the host scratch root) is an ordinary C++
   constructor call written in host source at `.providers(std::make_tuple(...))`'s call site —
   the same "host code by construction" trust tier `.api_key()`/`.store()` already operate at in
   this exact builder, not a new problem needing `HostSandboxSelection`'s own wrapper-type
   treatment. **What Revision 2 did NOT surface and this pass did**: if `SandboxToolProvider`
   derives a *per-session* subdirectory from `session_id` (needed so concurrent sessions don't
   collide on one shared scratch root — `cli_chat.cpp`'s own single hardcoded path was only ever
   safe because it serves one interactive session at a time), `session_id` is **not** the same
   trust tier as a host-literal constructor argument — `QuickstartSessionBuilder::session_id(id)`
   accepts a plain `std::string`, and nothing in this codebase's admission path guarantees it
   can't contain `../`-style path-traversal content if a host ever derives it from anything
   caller-supplied (a different threat than I3's model-output concern, but a real one: path
   traversal via a weakly-validated identifier). **New requirement, not in Revision 2**: any
   per-session subdirectory derivation must sanitize/reject traversal sequences before joining to
   the host root, and this needs its own explicit test, not an assumption that `session_id` is
   always host-safe.

**Also resolved, mechanical fix, precedented, no design risk**: the correctness reviewer's open
question about `start_background_task()`'s by-value `EffectContext` copy applies to `sandbox_fs`
exactly as it already does to `report_progress` — `tool_pipeline.hpp:745` already resets
`ctx.report_progress = [](ContentItem) {};` unconditionally on `background_task()`'s local copy,
specifically because a raw/closure reference into session-owned state must never survive into a
detached thread with no synchronization against `fork_from()`/`clear_in_process_state()`.
`sandbox_fs` is exactly the same shape of hazard (a raw pointer into state a
`SandboxToolProvider`/`SessionShellSandbox` owns) and is not currently covered by the existing
`captures_session_state` guard (`tool_pipeline.hpp:724`, which only blocks backgrounding a
*stateful-descriptor* tool, not a plain tool that happens to read `ctx.sandbox_fs`). **Concrete
fix, mirrors an already-Judged line exactly**: add `ctx.sandbox_fs = nullptr;` immediately next to
line 745's `report_progress` reset in `background_task()`. This is small, additive, and precedented
closely enough that it does not need a further red-team pass before implementation — unlike
everything else in this draft.

**Still genuinely open after this pass** (carried into §3, not resolved here):
- Whether `cli_chat.cpp`'s real Python wiring goes through `history_provider_`/`on_context()` at
  all, or hand-builds its `ToolTable` outside that mechanism entirely — not verified this pass.
  This matters because it decides whether a `PythonToolProvider` analog can reuse the same
  `ContextProvider`-composition design this revision gives Shell, or whether Python's session-
  lifecycle wiring has to stay a structurally different (direct, `cli_chat.cpp`-style) mechanism
  regardless. Recommend verifying before Revision 4, not guessing either way.
- Point 3 above (the `fork_from()` compile-error claim) is reasoned from real code but not proven
  by actually attempting the compile against a real `SandboxToolProvider` type.

## Revision 4 (2026-08-25) — Revision 3's central claim proven empirically; a second red-team pass found new gaps

Two things happened this pass: (1) the `background_task()` fix (Revision 3's §2 item 5) was
actually implemented (`tool_pipeline.hpp`, next to the existing `report_progress` reset) and
proven by a new regression test (`tests/test_agent_session_tool_call_progress.cpp`, case "E") —
this item is **done**, not just designed. (2) Two more red-team agents were run: one built and
compiled a real, minimal probe against real MSVC to test Revision 3's central `fork_from()`
prediction directly; the other did a fresh, broader pass over the whole `ContextProvider`-
composition design now that it's the draft's central claim.

**Revision 3's central prediction: CONFIRMED by actual compile, not just reasoning.**
A throwaway probe (`ComposedContextProvider<FakeSandboxToolProvider>` where the fake type holds a
non-copyable `std::unique_ptr<int>`, same shape as the real `SandboxToolProvider`/
`SessionShellSandbox`) was compiled with real MSVC against this tree's actual headers, following
`tests/test_composed_context_provider.cpp`'s own known-working instantiation pattern. Result: a
real compile error, pinpointed by MSVC's own instantiation trace to the exact predicted line —
`agent_session.hpp(1194)`, `history_provider_ = source.history_provider_;` — via `composed_context_
provider.hpp:105-120`'s user-deleted copy ctor/assignment. Revision 3 §3's claim holds: composing a
`SandboxToolProvider` into `HistoryProviderT` genuinely makes `fork_from()` a compile error, not a
silent runtime bug. The same probe pass also confirmed `on_context()` runs fresh on every one of
`AgentSession`'s four real call sites (`resolve_interaction()`'s approval-resume branch,
`resolve_codeact_ask()`, `resolve_hook_decision()`, and `run_rounds()`'s own per-turn loop), with
every denial/early-return branch still funneling into a fresh `run_rounds()` call rather than
reusing a stale `ContextContribution` — the "`sandbox_fs` self-heals every round" claim (§2 item 3)
holds structurally, not just by design intent.

**New, real gaps the broader pass found — Revision 3 did not consider these:**

1. **`EffectContext` is an undocumented side-channel that DOES chain across composed providers, in
   `Ms...` declared order — unlike `ContextContribution`, which is deliberately non-chained (OQ-18,
   `context_assembly.hpp`'s own stated design).** `assemble_context()`'s real loop
   `co_await`s each contributor's `on_context(session_ctx, ctx)` **sequentially, passing the same
   mutable `EffectContext&`** to every one in turn. Whatever `SandboxToolProvider` writes to
   `ctx.sandbox_fs` is visible to every LATER-declared provider's own `on_context()` in that same
   round (and to every `Tool::invoke()` after it) — and invisible to any EARLIER-declared one. This
   mutation carries **no `ContributorProvenance` stamp at all**, unlike messages/tools. Not a
   capability bypass by itself (`ReadSandboxFile` still does its own `ctx.capabilities` check
   regardless of ordering), but a real, silent, previously-unnamed hazard: a *different
   `ContextProvider`*, not just a `Tool`, could come to depend on `ctx.sandbox_fs`'s non-null-ness
   as an implicit signal, with zero enforced discipline and a correctness result that silently
   depends on `Ms...` declaration order. **New requirement**: the real ADR must either document
   `Ms...` ordering as a load-bearing contract for any provider reading a field another provider
   writes, or `SandboxToolProvider` must not rely on the field surviving to later providers at all
   (only to `Tool::invoke()`, which happens strictly after every provider's `on_context()` has run).
2. **`clear_in_process_state()` — a real, previously-undiscussed gap distinct from `fork_from()`.**
   It resets `history_provider_ = HistoryProviderT{}`, which for a `ComposedContextProvider<Ms...>`
   means default-constructed and **unengaged**. Nothing calls `engage()` again afterward. Every
   subsequent `on_context()` on that (reused/pooled) session fails closed with `composed_context.
   not_engaged` — safe-direction (fails closed, not open), but undiscussed by Revision 3, which
   only reasoned about `fork_from()`. **New requirement**: whatever calls `clear_in_process_state()`
   on a session meant to be reused must also re-`engage()` (or re-construct) its providers,
   `SandboxToolProvider` included — currently nothing does.
3. **§2 item 1's ADR-080 connection is asserted, not actually designed.** The claim that the
   per-session mount-policy step is "reachable from `register_agent<A>()`'s already-real
   `sandbox_profile`/`SandboxBackendRegistry` wiring" has no concrete mechanism anywhere in this
   draft or the code linking a `SandboxBackendRegistry::resolve_*()` result to
   `SandboxToolProvider`'s own constructor argument. `SandboxToolProvider` as designed constructs
   `SessionShellSandbox` directly, exactly like Tier-1 already does, fully bypassing the registry —
   consistent with `ADR-080` §7's own disclosed "no production consumption" scope, but this draft's
   own §2 item 1 oversold the connection as more designed than it is. **Corrected**: strike the
   "reachable from... wiring" framing until a real mechanism is sketched, or accept explicitly that
   the registry and `SandboxToolProvider` are NOT connected in this design at all (Option B, as
   concretely specified, resolves *which backend name* separately from *whether/how a session gets
   a sandbox* — two decisions this draft has not actually wired together).
4. **Python wiring question — definitively answered, not still open.** `tools/cli_chat.cpp`'s real
   `ToolDeclaringHistoryProvider` is a `ContextProvider` conformer used directly as `AgentSession`'s
   `HistoryProviderT` (not via `ComposedContextProvider`/`.providers(...)`), and its `on_context()`
   already does exactly `SandboxToolProvider`'s proposed shape: builds `ToolDescriptor`s (`execute_
   code`, `mount_skill`) AND touches its own session-scoped state in the same call. This *confirms*
   the general "provider contributes tools + mutates state via on_context()" pattern is sound and
   already shipped in production — but composing a `PythonToolProvider` analog into `Ms...` inside
   ONE `ComposedContextProvider<Ms...>` alongside `SandboxToolProvider` hits a real, different
   obstacle: `CodeActRunnerBinding`'s state is not safely copyable/movable the way this composition
   mechanism assumes for its own move-only fix, and the "no release, ever" contract (Revision 2
   finding 1) doesn't compose cleanly with `ComposedContextProvider<Ms...>`'s own per-instance
   lifecycle assumptions. Left as its own, separate, unresolved design question if Python's session-
   lifecycle wiring is ever pursued via this same shape — not blocking Shell's design, but not a
   free extension either.
5. **The draft was stale relative to the tree it cites.** `tool_pipeline.hpp:754`'s
   `ctx.sandbox_fs = nullptr;` (§2 item 5's fix) has now actually been implemented and tested (see
   this section's opening paragraph) — no longer a "must reset," now a "done, proven by test E."

## Revision 5 (2026-08-25) — closing Revision 4's three open gaps

**Gap 1 (`EffectContext` cross-provider chaining) — resolved with an explicit contract, not new
mechanism.** The hazard is real (`assemble_context()` threads the same mutable `EffectContext&`
through every composed provider's `on_context()` sequentially, in `Ms...` order — confirmed
Revision 4), but nothing in this design actually *needs* a second provider to read
`ctx.sandbox_fs` — only `Tool::invoke()` does (`ReadSandboxFile`, Tier-1's own worked example),
and every `Tool::invoke()` call happens strictly *after* every provider's `on_context()` has
already run for that round (`ToolTable::from_descriptors(contribution->tools)` is built only once
all contributors have returned). **Resolution**: state this as a hard rule, not a disclosed risk —
*fields a `ContextProvider` writes onto `EffectContext` inside `on_context()` may be relied on only
by code that runs after every provider's `on_context()` has returned for that round (i.e.
`Tool::invoke()`); a `ContextProvider`'s own `on_context()` must never depend on another
provider's `EffectContext` write, regardless of declared order.* This converts an order-dependent
correctness bug into an ordinary, statable API contract — the same discipline this project already
applies elsewhere (e.g. `ReadSandboxFile`'s own "do your own dynamic check, never trust ambient
state" rule). **New test obligation for the real implementation**: a positive control composing
`SandboxToolProvider` with a second, order-varied dummy provider that DOES try to read
`ctx.sandbox_fs` from its own `on_context()`, proving the read is unreliable (present or absent
depending on declared order) — makes the hazard visible in a test rather than only in a comment,
the same "don't just document a footgun, prove it's a footgun" standard `HostSandboxSelection`'s
own file already sets.

**Gap 2 (`clear_in_process_state()`'s unengaged-provider gap) — REOPENED in Revision 6: Revision
5's resolution rested on a false empirical claim, the same overclaim-from-incomplete-grep pattern
this draft has now made three times (Revision 1's fabricated `CodeActRunnerBinding` claim,
Revision 3's unproven-until-compiled `fork_from()` claim, now this).** Revision 5 claimed
`clear_in_process_state_locked()`'s call from `delete_session()` was the function's *only* real
caller, grep-confirmed. A third red-team pass actually ran that grep properly and found this false:
`tests/test_rt_agent_session_codeact_ask_max_turns.cpp:283` calls the **unlocked**
`clear_in_process_state()` directly, with its own comment explicitly framing the fix at that site
around **"a pooled/reused session object"** — this codebase's own commentary already treats
post-clear reuse as a real, contemplated scenario, not a hypothetical Revision 5 could dismiss.
More directly: `tests/test_rt_agent_session_tooling_and_delegation.cpp`'s case S5 (line ~519) calls
`fork.clear_in_process_state()` and then **actually reuses the object** — `fork.initialize(...)`
followed by a second, successful `fork.start_run(...)`. Session reuse after
`clear_in_process_state()` is a real, exercised, currently-passing pattern in this codebase, not
something that "never happens." Revision 5's `delete_session()`-only framing was wrong.

**What survives, narrowed and now honestly qualified**: S5's `StatefulSession` uses
`StatefulCounterProvider` directly as `HistoryProviderT` — NOT `ComposedContextProvider<Ms...>` —
so it does not actually exercise the specific `composed_context.not_engaged` hazard this draft is
reasoning about; no current test proves reuse after `clear_in_process_state()` against a
`ComposedContextProvider<Ms...>`-based session specifically. But given that reuse-after-clear is
already a real, live, tested pattern in this codebase for OTHER `HistoryProviderT` shapes, it
cannot be assumed away for a `ComposedContextProvider<Ms...>`-based one (which is exactly what
`SandboxToolProvider`'s design requires) — the honest position is the opposite of Revision 5's:
**assume session reuse after `clear_in_process_state()` is a real possibility for any session
type, `SandboxToolProvider`-composed ones included, until proven otherwise.** If a
`ComposedContextProvider<Ms...>`-based session is ever reused this way, it WOULD hit
`composed_context.not_engaged` with no re-arm story, exactly as Revision 4 originally found.
**Real requirement, reinstated**: the real ADR must document that any code reusing a session after
`clear_in_process_state()` must re-`engage()` `history_provider()` with a fresh provider tuple
(`SandboxToolProvider` included) before that session's next `on_context()` call — mirroring
`capabilities_`'s own already-documented "re-call `set_capabilities()` after `fork_from()`"
contract exactly. The `engage()` API to do this already exists and is already proven as "a real
recovery path" (`test_composed_context_provider.cpp`'s own P3b case) — the requirement is
documenting the obligation, not building new mechanism, but it IS a real, disclosed, testable
requirement, not something this draft may strike as a non-issue.

**Gap 3 (`SandboxBackendRegistry` connection) — the exact structural reason it doesn't exist,
found.** `check_sandbox_profile_availability()` (`agent_registry.hpp:366-371`) is the only place
`SandboxBackendRegistry::resolve_strict()`'s result is ever consulted at registration time, and its
real body **discards the resolved backend entirely**: `registry->resolve_strict(current_platform())
.transform([](auto*) { return; })` — the `RegisteredSandboxBackend const*` `resolve_strict()`
actually returns is thrown away, keeping only success/failure. `AgentMetadata.sandbox_profile`
(a `SandboxProfileDescriptor{is_strict, traits}`) therefore never learns *which* backend won, only
*that* one exists for this platform — so there is no data available anywhere past
`register_agent<A>()`'s own call for a session-construction-time call site to read, even in
principle. **What actually connecting them would require, concretely, none of which exists today**:
(a) `check_sandbox_profile_availability()` (or a sibling) would need to retain and return the
winning backend's name, not just discard it into a `result<void>`; (b) `AgentMetadata` (or a new
field beside `sandbox_profile`) would need to carry that name forward past registration; (c) host
code building a session would need to read that field and pass a value derived from it into
`SandboxToolProvider`'s constructor — no automatic wiring, since `register_agent<A>()` (once, per
agent *type*) and `SandboxToolProvider` construction (once, per session *instance*, at
`.providers(...)` call time) are different lifecycle points with no existing bridge between them.
**Recommendation, not yet decided**: don't build (a)-(c) speculatively — nothing today needs it,
since `native_jail` is the only real backend implementing Python/Shell either way (Option A/B/C's
own analysis), and the registry mostly matters for choosing among *alternate* backends, none of
which implement Python/Shell yet. Accept Option B, as concretely specified, as **two genuinely
separate decisions** — which backend name `Strict`/named resolution picks, and whether/how a
session gets a sandbox at all — rather than one connected pipeline. §2 item 1's "reachable from...
wiring" framing is corrected below to state this plainly instead of asserting a connection that
isn't there.

## 0. Re-grounding the question against real, current code (2026-08-25)

Verified directly, not assumed from the roadmap plan that named this phase:

- **There are now exactly two real, production-shaped sandbox-consuming code paths in this
  codebase, and both bypass `SandboxBackend`/`SandboxHandle` entirely:**
  1. `tools/cli_chat.cpp`'s `shared_python_runner()` — builds a `MediatedPythonConfig` with
     `mount_roots[kWorkMount] = scratch.wstring()` directly, hands it to
     `NativeJailBackend::create_python_worker()`. No `SandboxSpec`, no `SandboxHandle`, no
     `SandboxBackendRegistry::resolve_*()` call anywhere in that path.
  2. `src/backends/native_jail/session_shell_wiring.hpp`'s `SessionShellSandbox` (this session,
     Tier-1 Phase 1) — constructs a `MediatedFileSystemAdapter` + `ExecState` +
     `MediatedShellRunner` directly against a host scratch directory, same shape, same bypass.
     Its own top comment says so explicitly: *"the same 'no `SandboxBackend::create()`/
     `SandboxHandle` abstraction needed' shape `cli_chat.cpp`'s own Python wiring already
     established."*
- **`grep`-confirmed (this session): every non-test file that references `SandboxHandle` is
  either a backend's own implementation** (`native_jail_backend.{hpp,cpp}`,
  `linux_native_jail_backend.{hpp,cpp}`, `kata_backend.{hpp,cpp}`, `wasm_backend.{hpp,cpp}`,
  `mediated_python_runner.hpp`, `job_object_limits.{hpp,cpp}`, `wasm_tool_bridge.hpp`,
  `seccomp_filter.hpp`, `python_worker_main.cpp`, `jailed_worker_rpc.hpp`,
  `app_container_profile.hpp`) **or is `sandbox_backend_registry.hpp`/`sandbox.hpp` themselves.**
  The registry draft's own §0 already found this for the pre-Tier-1 state ("exactly one call
  site, and it is a test fixture"); Tier-1's new Shell wiring added a second real production
  consumer and it *also* didn't reach for `SandboxHandle` — it reached for the same lower-level
  primitive (`FileSystemAdapter`) `cli_chat.cpp` already used. This is not a coincidence — it's
  the same engineer(s), under the same constraints, reaching for the same tool twice.
- **`AgentMetadata.sandbox_profile` is still write-only in production** — compiled and validated
  at `register_agent<A>()` time (`check_sandbox_profile_availability()`, now real for `Strict`
  resolution since `ADR-080`'s code landed), but nothing downstream reads it to actually construct
  or route through a backend. This is `ADR-080`'s own disclosed scope boundary, still true today.

**The question this draft actually has to answer is therefore not "how do we call
`SandboxBackendRegistry::resolve_*()` from `AgentSession`" — that's a small, mechanical addition.
It's: given that the two real, working, tested consumer paths that exist today were both built
by going *around* the `SandboxBackend`/`SandboxHandle` abstraction rather than through it, is that
abstraction actually the right shape for what a session needs, or does "session→sandbox lifecycle
wiring" mean something narrower than "make every execution flow through `SandboxHandle`"?**

## 1. Three ways to narrow ADR-080's residual, not one (Revision 2: "close" softened to "narrow" — see finding 4)

### Option A — Make `SandboxHandle`/`SandboxBackend::exec()` the real per-session execution path

Replace both `cli_chat.cpp`'s and `SessionShellSandbox`'s direct `MediatedFileSystemAdapter`/
`MediatedPythonRunner`/`MediatedShellRunner` construction with: resolve a `SandboxBackendRegistry`
entry for the session (`Strict` or named), call `backend->create(spec, ctx)` to get a real
`SandboxHandle`, route every `execute_code`/`run_shell` call through `backend->exec(handle, req,
ctx)`, call `backend->destroy(handle)` on session teardown.

- **Pro**: this is the abstraction's actual stated purpose (008 §2a) — a session becomes
  portable across `native_jail`/`wasm`/`kata`/a future `microvm` backend without its own code
  changing, which neither of today's two direct-wiring paths can do (each is hard-wired to
  `native_jail`'s own concrete types).
- **Con, real and load-bearing**: `SandboxBackend::exec()`'s signature
  (`ExecRequest const&, EffectContext&) -> result<ExecOutcome>`) is coarser than what
  `MediatedShellRunner`/`MediatedPythonRunner` actually expose today — `ExecState` persistence
  across calls (proven this session, `test_session_shell_wiring.cpp`'s "cd survives across
  calls" case) and `CodeActRunnerBinding`'s claim arbitration (ADR-030) both depend on identity
  and state that live *inside* the concrete runner types, not in the generic `ExecOutcome` return
  shape. Making this real either means widening `SandboxBackend`'s contract (a bigger, contested
  change — this concept is a locked, Judged shape per 008) or means the registry's `exec()`
  closures become thin adapters that still, underneath, hold the same concrete
  `MediatedShellRunner`/`MediatedPythonRunner` state they do today — in which case the "backend
  portability" benefit is partly illusory for the two backends that actually matter for
  Python/Shell today (only `native_jail` implements either).
- **Cost**: touches the hottest, most security-load-bearing path in the whole engine
  (`execute_code`, ADR-030's already-hard-won claim semantics). Real risk of silently reopening a
  Judged finding if the abstraction seam is drawn in the wrong place.
- **Revision 2 addition (security review, cleared as a non-blocker but must be stated)**: if this
  option is ever pursued, `EffectContext&` already threads through `exec()`'s real signature, so
  per-call attribution is *not* structurally lost — provided every real call to `backend->exec()`
  happens strictly inside `invoke_tool()`'s existing 10-step pipeline (`tool_pipeline.hpp`), never
  from code that hand-builds an `EffectContext` outside it. A future ADR pursuing Option A must
  state this as a hard requirement, not an implementation detail.

### Option B — Scope `SandboxBackendRegistry` consumption to what it's actually good at today, leave Python/Shell on their proven direct path

Accept that `native_jail`'s Python/Shell wiring is, correctly, a special-cased fast path — not
because nobody got around to routing it through the registry, but because `ExecState`/
`CodeActRunnerBinding`'s state and identity requirements are native-jail-specific in ways the
generic `SandboxBackend` contract doesn't (and arguably shouldn't) express. Scope "session→sandbox
lifecycle wiring" down to the piece that's actually missing and actually general: making
`register_agent<A>()`'s already-real `sandbox_profile` *decision* (Strict/named resolution,
`ADR-080`'s code) reach the place that decides *whether* a session gets `SessionShellSandbox`/
Python wiring constructed at all and with *which* concrete host directory/mount policy — i.e., the
registry stays the tool for "which backend wins," and something new (a session-lifecycle hook that
does add a narrow, disclosed mutator to `AgentSession` — see Revision 2 finding 3, this is no
longer claimed to leave `AgentSession` untouched) is the tool for "given that answer, build the
session's actual `run_shell`/`execute_code` tools." `SandboxHandle`/`exec()` remain real and
exercised by `wasm`'s tool-bridge path (already a real, working consumer, per
`test_wasm_tool_bridge.cpp` — the one case where the generic contract already fits, because WASM
component calls genuinely are stateless per-call in a way shell/Python sessions are not) and by
any future backend whose own execution model fits the generic shape, without forcing Python/Shell
through it.
- **Pro**: doesn't touch `ADR-030`'s already-Judged claim-arbitration *decision* (bind-once, no
  release) or `ExecState`'s proven shape at all — additive, matches this session's own Tier-1
  judgment call (documented in `session_shell_wiring.hpp`'s own top comment) that direct wiring
  was the right choice, not a shortcut taken under time pressure. **Revision 2 correction**: it
  does add a small, disclosed new mutator surface to `AgentSession` itself (§2 item 2) — "doesn't
  touch `AgentSession` at all" was Revision 1's overclaim, not this revision's.
- **Con**: `SandboxBackendRegistry`'s `Strict`/named resolution becomes a decision that gates
  *construction-time policy* (which mount root, which resource limits) rather than *which code
  path executes a call* — a narrower, less impressive-sounding claim than "sessions are portable
  across sandbox backends," and one that should be stated honestly rather than oversold as more
  than it is. **Revision 2 correction**: it also does not close `ADR-080`'s own named residual
  ("a real execution path constructs and uses a `SandboxHandle`") — only `wasm`'s tool-bridge path
  does that today, unchanged by Option B. Option B narrows the gap `ADR-080` names without closing
  it.

### Option C — Do both, in sequence: B now, A later if a second backend ever actually needs to run Python/Shell

Ship Option B's narrower wiring first (it's additive, doesn't reopen `ADR-030`'s Judged claim
*decision*, and makes real progress on `ADR-080`'s residual without fully closing it). Leave
Option A's harder question — does `SandboxBackend::exec()`'s contract need to widen to carry real
session state across calls — genuinely open, to be answered only when there is a second real
backend (`kata`, a future `microvm`) that needs to run Python/Shell and therefore needs the
answer, rather than speculatively designing it against a hypothetical.

**This draft's recommendation, red-teamed but still not project-owner-decided**: Option C, with
Revision 2's correction that Option B's Python half stays permanently bound once constructed
(matching `CodeActRunnerBinding` exactly as shipped — no release, no rebind), while its Shell half
can genuinely construct-and-release per session lifecycle with no such constraint. Option A's cost
is real and its benefit is currently unrealized (no second backend implements Python/Shell today
to make "portability" concrete rather than aspirational); Option B makes real, honestly-scoped
progress on the disclosed residual without touching anything Judged. A future ADR revisiting
Option A should be triggered by a real second-backend need, not by this draft's own momentum.

## 2. What Option B concretely requires (if this is the direction taken) — Revision 2, substantially corrected

1. **A per-session sandbox *policy* resolution step, independent of `SandboxBackendRegistry` in
   practice (Revision 5 correction — the earlier "reachable from `register_agent<A>()`'s wiring"
   framing was asserted, not real; see Revision 5 above, gap 3, for the exact structural reason):**
   given that `native_jail` is, in practice, the only real backend implementing Python/Shell today,
   decide the concrete host scratch directory / mount policy for *this* session directly — no data
   path exists from a resolved `RegisteredSandboxBackend` to this decision, and building one is not
   recommended speculatively (Revision 5, gap 3). This is new regardless — today `cli_chat.cpp` and
   `SessionShellSandbox`'s callers each pick their own scratch directory ad hoc; nothing central
   owns "which real host path does session `s-123` get."
   **Revision 3 correction (supersedes Revision 2's "needs a `HostSandboxSelection`-style wrapper
   type" suggestion)**: the host-root argument itself needs no new wrapper type — it's an ordinary
   constructor argument written in host source at `SandboxToolProvider{host_root}`'s own
   `.providers(...)` call site, the same "host code by construction" trust tier `.api_key()`/
   `.store()` already operate at in this exact builder. **What actually needs a hard requirement,
   found this pass, not Revision 2's**: if `SandboxToolProvider` derives a *per-session*
   subdirectory from `session_id` (needed so concurrent sessions don't collide on one shared root
   — `cli_chat.cpp`'s hardcoded single path was only ever safe for one interactive session at a
   time), `session_id` is a weaker trust tier than a host-literal argument —
   `QuickstartSessionBuilder::session_id(id)` accepts a plain `std::string` with no traversal
   validation anywhere on that path today. The real implementation must sanitize/reject `../`-style
   sequences before joining `session_id` to the host root, with its own explicit test — a path-
   traversal concern, not an I3/model-output one, but still a real, previously-unnamed requirement.
2. **A session-lifecycle hook, with Python and Shell now handled differently (Revision 2 finding
   1 — this replaces Revision 1's single unified claim):**
   - **Shell**: session creation → construct `SessionShellSandbox` lazily on first use (008 §6's
     already-declared laziness); session teardown → destroy it (ordinary destructor-driven
     cleanup, already how `SessionShellSandbox` behaves — verified, no claim/release primitive
     involved at all).
   - **Python**: session creation → construct/bind the shared Python runner lazily on first use,
     exactly as `cli_chat.cpp` does today; session teardown → **no release**. Once a session has
     bound the process's one `CodeActRunnerBinding`, it stays bound for the life of the process —
     this is `ADR-030`'s real, shipped, Judged contract, not a gap this draft proposes to close.
     A session-lifecycle hook that tries to "release Python on teardown so another session can
     use it" is explicitly **out of scope for Option B** and would itself need a new,
     independently red-teamed ADR (it reopens exactly the hazard `ADR-030` rejected, sequentially
     instead of concurrently).
   - This is the actual "lifecycle" piece the phase is named for, and it's currently absent for
     both: nothing today calls `SessionShellSandbox::create()` from anywhere other than a test or
     (once wired) an explicit host call. `session_builder.hpp` has no opt-in step for this yet —
     Tier-1 Phase 1 explicitly left this as a standalone factory rather than a builder step.
   - **Revision 3 correction (supersedes Revision 2's "new `AgentSession` mutator" finding)**: no
     new `AgentSession` mutator is needed. `SandboxToolProvider` (a new type conforming to the
     existing `ContextProvider` concept) constructs `SessionShellSandbox` lazily on its own first
     `on_context()` call and, in that same call, both contributes `run_shell`'s `ToolDescriptor`
     and assigns `ctx.sandbox_fs` directly — `on_context()`'s `EffectContext&` parameter is already
     the seam, reused via the existing `Ms...`/`ComposedContextProvider<Ms...>` composition
     `.providers(...)` already supports (see Revision 3 above for the full reasoning against real
     code). No change to `AgentSession` itself.
   - **Revision 3 correction (supersedes Revision 2's "needs `capabilities_`-style re-arm" finding)**:
     `fork_from()`/`clear_in_process_state()` resetting `effect_context_ = EffectContext{}` is not
     the hazard it first appeared to be, because `sandbox_fs` is no longer session-lifetime state
     living directly on `effect_context_` — it's re-derived fresh on every round's `on_context()`
     call from `SandboxToolProvider`'s own held state. The real, sharper question Revision 3 found
     instead: `fork_from()`'s real body plain-copies `history_provider_`
     (`agent_session.hpp:1194`), which is exactly the shape `session_builder.hpp`'s own findings
     9/11 (`ADR-074`) already fixed by making `ComposedContextProvider<Ms...>` move-only — so a
     `SandboxToolProvider` holding a non-copyable `unique_ptr<SessionShellSandbox>` should make
     `fork_from()` a compile error for any session composed with it, the same safe-by-construction
     outcome ADR-074 already chose for this exact problem shape. Needs a real compile-time proof
     against a real `SandboxToolProvider`, not just this reasoning, before being trusted (§3).
3. **`EffectContext::sandbox_fs` populated from `SandboxToolProvider::on_context()`, visible to
   every subsequent call in that round.** Revision 3 correction: this is a per-round assignment
   made by the provider itself via `on_context()`'s `EffectContext&` parameter, not a one-time
   session-lifecycle write onto `effect_context_` — see Revision 3 above.
4. **A positive-control test obligation, not just the worked example already proven.**
   `ReadSandboxFile` (Tier-1) proves one tool does its own dynamic capability check before
   touching `sandbox_fs`. Nothing today *forces* a future `Tool` built against `sandbox_fs` to
   replicate that pattern rather than treating non-null as implicit authorization. The real ADR
   should require this as a named, tested contract (e.g., a compile-fail or runtime positive
   control analogous to `tests/compile_fail/sandbox_profile_positive_control.cpp`), not leave it
   as convention only.
5. **DONE (Revision 4)**: `background_task()` resets `ctx.sandbox_fs = nullptr;`
   (`tool_pipeline.hpp`, next to the existing `report_progress` reset), mirroring that line exactly.
   Implemented and proven by a new regression test, `tests/test_agent_session_tool_call_progress.cpp`
   case "E" — a `Backgroundable` tool given a live, non-null `sandbox_fs` in the caller's
   `EffectContext` observes `nullptr` on `background_task()`'s own detached thread. Full project
   rebuild and test suite green after this change.
6. **RESOLVED (Revision 5)**: `EffectContext`'s chaining across composed providers in `Ms...` order
   (unlike `ContextContribution`) is closed by an explicit contract, not new mechanism — `EffectContext`
   fields a provider writes in `on_context()` may be relied on only by code that runs after every
   provider's `on_context()` has returned for that round (i.e. `Tool::invoke()`), never by another
   `ContextProvider`'s own `on_context()`. `SandboxToolProvider` only needs `Tool::invoke()` to see
   `ctx.sandbox_fs`, which this rule already guarantees. A positive-control test proving the read is
   order-dependent from a second provider is a required test obligation for the real implementation
   (Revision 5 above).
7. **REOPENED (Revision 6, correcting Revision 5's false empirical claim)**: session reuse after
   `clear_in_process_state()` is a REAL, tested pattern in this codebase
   (`tests/test_rt_agent_session_tooling_and_delegation.cpp`'s case S5;
   `tests/test_rt_agent_session_codeact_ask_max_turns.cpp:283`'s own comment names "a pooled/reused
   session object" directly) — Revision 5's "only caller is `delete_session()`, grep-confirmed" was
   wrong. No current test reuses a session with a `ComposedContextProvider<Ms...>`-based
   `HistoryProviderT` specifically, so the exact `composed_context.not_engaged` hazard isn't proven
   to fire today — but it must be assumed reachable, not assumed away. **Real requirement**: the
   ADR must document that any code reusing a session after `clear_in_process_state()` re-
   `engage()`s `history_provider()` with a fresh provider tuple (`SandboxToolProvider` included)
   before that session's next `on_context()` call — mirroring `capabilities_`'s own already-
   documented "re-call `set_capabilities()` after `fork_from()`" contract. The `engage()` API
   already exists and is already proven as "a real recovery path"
   (`tests/test_composed_context_provider.cpp`'s own P3b case) — the gap is the documented
   obligation, not new mechanism, but it is real and must be named, not struck.

## 3. What this draft deliberately does not decide

- **Resolved this pass, not open any more**: whether `session_builder.hpp` needs a new opt-in step
  — no, the existing `.providers(...)`/`Ms...` composition already suffices (Revision 3 above).
  Whether a new `AgentSession` mutator is needed — no, `on_context()`'s existing `EffectContext&`
  parameter is the seam (Revision 3 above). Whether a `HostSandboxSelection`-style wrapper type is
  needed for the mount-policy host root — no, an ordinary constructor argument already matches
  `.api_key()`/`.store()`'s existing trust tier (Revision 3 above, §2 item 1).
- **Resolved this pass (Revision 4)**: the `fork_from()` compile-error prediction — confirmed by an
  actual MSVC compile against a real probe type, exact error at the exact predicted line. Whether
  `on_context()` runs fresh every round including all resume paths — confirmed, all four real call
  sites checked directly. Whether `cli_chat.cpp`'s Python wiring uses `on_context()`-style
  provider composition — confirmed yes (`ToolDeclaringHistoryProvider`), but NOT via
  `ComposedContextProvider<Ms...>`, and composing a Python analog into that mechanism alongside
  Shell hits `CodeActRunnerBinding`'s own constraints as a separate, unresolved question (Revision
  4 above) — the general pattern is validated even though the Python composition question isn't.
- Whether `wasm`'s tool-bridge path (the one place `SandboxHandle`/`exec()` already has a real,
  working, non-test consumer) should also be threaded through this same per-session lifecycle hook,
  or stays exactly as it is today (its own `WasmToolBridge` construction, independent of
  Python/Shell's session lifecycle) — genuinely orthogonal, deferred.
- **Resolved this pass, confirmed by a third red-team pass (Revision 5/6)**: whether/how
  `SandboxBackendRegistry`'s resolved backend should connect to `SandboxToolProvider`'s
  construction — it structurally can't today (`check_sandbox_profile_availability()` discards the
  resolved backend, keeping only success/failure, confirmed by an independent grep of every
  `resolve_strict`/`resolve_named` call site in the tree), and building the three pieces that would
  change that is not recommended speculatively. Accepted as two separate decisions (Revision 5
  above, gap 3). Whether `EffectContext`'s cross-provider chaining needs an explicit contract —
  resolved with one (Revision 5 above, gap 1), confirmed sufficient against every real
  `ContextProvider` conformer in the tree (none reads a field another provider wrote).
- **Reopened (Revision 6)**: whether `clear_in_process_state()` needs a re-engage story — Revision
  5 said no ("only real caller is a terminal deletion"); a third red-team pass found this false
  (real, tested session-reuse-after-clear exists in this codebase for other `HistoryProviderT`
  shapes) and reinstated the requirement (§2 item 7 above): document the re-`engage()` obligation
  for any code reusing a `ComposedContextProvider<Ms...>`-based session after this call.
- The exact per-session subdirectory naming/sanitization rule (Revision 3, §2 item 1) — the
  *requirement* (sanitize `session_id` against traversal) is now pinned down; the concrete
  algorithm is not. Also still open: whether a subdirectory of a durable per-session worktree,
  once Phase 4a's durable `WorktreeObjectStore` exists, should replace a plain temp dir — connects
  to the roadmap's own Phase 4 dependency note about Suspend/Resume needing durable state.

## 4. Process note

Per this project's own rule (`CLAUDE.md`: *"Contested, hot-path, or security-critical designs go
through design → red-team → prove → judge and produce an ADR, not an ad-hoc change"*) — this draft
has now been through FOUR red-team passes (2026-08-25: 3 agents on Revision 1→2, 2 agents on
Revision 3→4 including a real compile probe, 1 agent on Revision 5→6, 1 agent independently
re-verifying Revision 6's own correction) plus one small piece actually implemented and tested
(§2 item 5). The pattern across the first three passes is worth stating plainly, not glossed over:
**every one of the first three red-team rounds on this draft found this draft's own self-directed
reasoning wrong or overstated at least once** — Revision 1's fabricated `CodeActRunnerBinding`
claim, Revision 3's unproven-until-compiled `fork_from()` claim, and Revision 5's false "no second
caller of `clear_in_process_state()`" claim. The fourth pass is the first to check a self-directed
revision (Revision 6's correction of Revision 5) and find it accurate on every point, including one
detail (`ComposedContextProvider::engage()` having no hidden precondition beyond its own
`engaged_` flag) that hadn't been checked before. **This does not mean the pattern is over** — one
clean pass after three dirty ones is evidence the correction happened to be right, not evidence the
underlying reasoning process has become more trustworthy. **The load-bearing lesson for whoever
picks this up next is unchanged**: this draft's own confidence in its own revisions should count
for very little without independent verification — every claim that sounds like "I checked the
real code and confirmed X" should still be re-checked, especially empirical/"grep-confirmed" ones,
which is exactly where all three real mistakes lived and exactly what the fourth pass had to
re-verify from scratch rather than take on faith.

## Promoted to `decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md` (2026-08-25)

A fifth red-team round (on the two remaining open items: per-session subdirectory sanitization,
Python composition) found real corrections to both (a double-hex-encoding error and an overclaimed
"no filtering needed" framing on the first; an understated integration cost and a missed
`fork_from()`-compile-error consequence on the second) — folded directly into the ADR rather than
into a Revision 7 here, since the ADR is now this design's authoritative record. This draft stays
as the full revision-by-revision history (six revisions, five red-team rounds); the ADR is the
consolidated decision, falsifiable claims, per-claim verdicts, and residuals. Read the ADR first;
come back here only for the blow-by-blow of how each claim was reached.

**What genuinely remains before this is ready for an ADR** (§3): the re-`engage()`-after-
`clear_in_process_state()` documentation requirement (reinstated, Revision 6, now independently
verified); the per-session subdirectory sanitization algorithm (still unwritten); Python's own
composition question (Revision 4 finding 4, explicitly deferred). No further red-team pass is
pending on the current text — the next step is either a fifth pass on the two still-open items
above, writing the actual ADR with these findings carried forward as named residuals, or starting
implementation of the pieces already confirmed safe (the `EffectContext` ordering contract, the
`clear_in_process_state()` re-arm requirement) as small, precedented additions similar to §2 item
5's already-shipped fix.
