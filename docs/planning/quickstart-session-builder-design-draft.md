# Quickstart session builder — a convenience facade over `AgentSession`'s wiring — design draft

**Status: promoted from prototype to a supported feature (2026-08-22) — §2a/§2b/§2c/§2d/§3/§4
implemented, red-teamed EIGHT times total across this file's history. §2b was implemented in a 4th
pass, then red-teamed three times (round 4 §0f, round 5 §0g, round 6 §0h): round 4 found and fixed a
real session-isolation gap (`fork_from()` aliasing stateful composed providers, finding 9) plus a
diagnostics gap (finding 10); round 5 found finding 9's own fix was NECESSARY BUT NOT SUFFICIENT — a
second, worse bypass of the same class through a MOVE rather than a copy (finding 11, fixed); round 6
found no functional bug. Round 7 (§0i) then gave finding 7 its own dedicated round at last, and found
a real bug — NOT in this facade, but in the `AgentSession` mechanism it wires into:
`resolve_codeact_ask()`'s ask-pending branch never advanced `turn_index`, so `.max_turns()`/
`.token_budget()` were completely bypassed by a non-converging CodeAct ask loop (finding 13, fixed at
the root in `rt/agent_session.hpp`, also recorded in `decisions/ADR-057-agent-ask-suspend-without-
deadlock.md` §8). **Round 8 (§0j) then gave findings 11 and 13 their own dedicated re-examination
round: finding 11 held up clean, but a NEW bug (finding 15, MEDIUM) was found in the SAME class —
`LazyComposedContextProvider::engage()` was not exception-safe, letting a throw-then-retry duplicate a
contributor on the wire — fixed with a strong exception guarantee. Finding 13's surrounding mechanism
also yielded two LOW findings (16, 17) in `rt/agent_session.hpp`, both fixed there too. Full suite
222/222 after.** A convenience facade over
already-Reviewed RFCs,
not a new invariant or capability shape, so promotion did not require its own ADR (CLAUDE.md's
`design → red-team → prove → judge` cycle is reserved for contested/hot-path/security-critical
designs; this used the lighter `design → prototype → red-team → fix` cycle throughout, per §7 below).
Round 1 found and fixed two real issues (§0b findings 1-2); round 1 also found a third, real gap it
left open, fixed in a same-session follow-up (§0b finding 3) — round 2 (§0c) then red-teamed THAT fix
specifically and found it did not actually deliver on its own claim, fixing it again (§0c findings 3-4,
re-numbered to match the header's own comment). §2d landed in a later same-session pass, corrected from
this draft's own original sketch during implementation (finding 5, header's own comment —
`.require_approval_for(...)` read backwards against the real `ApprovalDecider` mechanism; implemented
as `.approve_tools(...)` instead) — round 3 (§0d) then red-teamed THAT landing specifically and found a
real, LIVE-REPRODUCED hang unrelated to the approval logic itself (finding 7: no `max_turns`/
`token_budget` bound, and `Bundle::ask()`'s own §0b-finding-1 guard does not catch this class of hang),
plus a documentation overclaim — both fixed. §2b (history/context composition), explicitly NOT
implemented through three rounds for a real structural reason (finding 6, header's own comment), is
now RESOLVED — see §2b below and finding 8, header's own comment, for the mechanism
(`ComposedQuickstartSessionBuilder`/`detail::LazyComposedContextProvider`) and what round 3's own
analysis had NOT anticipated (the default-constructibility constraint runs deeper than finding 6's
text captured). Matches this project's `design → red-team → prove → judge` discipline (CLAUDE.md),
same honesty level as `docs/planning/tool-optimizer-provider-design-draft.md` and `docs/planning/
model-call-gateway-routing-design-draft.md`. Real, compiling, passing code:
`include/agentengine/core/session_builder.hpp`, `tests/test_session_builder.cpp` (65/65 checks,
Windows/MSVC, `AGENTENGINE_WITH_HTTPS=ON`, full suite 221/221 `ctest -LE live-network`). Still not
implemented: `.with_fallback()`/`.with_middleware()`/`.with_content_replay()`. This draft's own
`.raw_client_only()` escape hatch is now IMPLEMENTED (2026-09-03) as `RawQuickstartSessionBuilder<
ChatClientT, Store>` (`session_builder.hpp`, right after the `OpenAiSessionBuilder`/
`AnthropicSessionBuilder` aliases) — see §0k below and that class's own header comment for the full
account, including the two-readings ambiguity this draft's own §2a prose left open and how it was
resolved.

## §0b. Red-team pass against the real code — two findings fixed, one still open

A fresh, adversarial pass (independent of the implementer, per this project's own red-team practice)
against `session_builder.hpp`/the test/the design draft's own §5 self-red-team found three real issues.
Full detail lives in the header's own top comment (kept next to the code it describes); summarized here
for the record:

1. **FIXED — real UB, not hypothetical.** `Bundle::ask()` originally reused `examples/*.cpp`'s naive
   `while(!done()) resume()` idiom against a real `AgentSession`. That idiom is safe only when
   `session_mutex_` (`rt::AsyncMutex`) is never contended — guaranteed by construction in a linear,
   single-threaded example `main()`, but NOT guaranteed for `Bundle`, a reusable, potentially-shared
   object with no built-in single-caller discipline. Under real contention, `AsyncMutex` parks the
   coroutine for a later, possibly different thread's `unlock()` to resume directly — a second, blind
   `resume()` on that same handle is a genuine cross-thread double-resume race, undefined behavior per
   the C++20 coroutine spec. This is the EXACT bug class `rt/thread_pool.hpp`/`rt/drive_leaf_task.hpp`
   already found and fixed elsewhere in this codebase (their own file-top comments name it explicitly)
   — the red-team's contribution was recognizing `Bundle::ask()` reintroduces it at a new call site the
   existing fixes don't cover. **Fixed** with two layers: a heap-owned `ask_mutex_` serializes every
   `.ask()` call one `Bundle` ever makes, and the drive itself is now bounded to one `resume()` with a
   fail-closed error if that alone doesn't finish (matching `drive_leaf_task.hpp`'s own shape) as
   defense in depth. **Not covered**: a caller reaching the raw session via `Bundle::session()` and
   calling `start_run()` directly, concurrently with `.ask()` — bypasses `ask_mutex_`, inherits
   `AgentSession`'s ordinary I1 obligation like any other direct use. Not proven by a live concurrency
   test (named gap in the test file's own top comment) — verified by code review against the cited
   precedent only.
2. **FIXED — real hygiene bug.** `.api_key_from_env()` pushed a new `cap::Secret` grant on every call,
   unconditionally — calling it twice (e.g. correcting a typo'd env-var name) left a phantom, unusable
   grant for the first name behind, an I4 attributability smell (an audit reading `capabilities()` back
   would see a grant with no corresponding usable secret). **Fixed**: the auto-derived grant now lives
   in its own `primary_secret_grant_` field, overwritten (not appended) each call — last-call-wins,
   matching `api_key_ref_`'s own semantics. Regression-proofed: `tests/test_session_builder.cpp`'s
   "B4" case.
3. **STILL OPEN — real, disclosed, not yet fixed.** `build()` requires `Store` to be default-
   constructible and expose `.set(name, value)` — properties `InMemorySecretStore` (the default, and
   the only type this file's own tests exercise) has, but which are neither part of the `SecretStore`
   concept nor present on this project's own real production store, `AgentEngineSecretStore` (no
   default constructor, no `.set()`). `InMemorySecretStore` is `trust/secret.hpp`'s own designated
   **test-only** backend ("Production code constructs `AgentEngineSecretStore`... never this" — its own
   header comment); its values sit in a plain, never-zeroized map, a direct contradiction of 018 §4's
   secret-hygiene invariant the rest of that file goes out of its way to uphold. A host following this
   facade's DEFAULT, documented path (`OpenAiSessionBuilder`/`AnthropicSessionBuilder`,
   `.api_key_from_env()`) currently wires a real credential into this project's own test-only,
   non-hygienic store, with nothing at the API surface stopping them.

**FIXED in a same-session follow-up pass, then found WRONG in its first form by a second red-team round
(§0c below).** `.api_key(SecretRef)` separately declares which ref the `ChatClient` resolves against
and auto-grants its `cap::Secret`, independent of how `.store()` got populated — this part held up.
`.api_key_from_env()` survives as `requires`-gated, test-only sugar — composing `.api_key()` + `.store()`
under the hood — that simply does not exist as a callable member (a hard compile error, not a silent
wrong assumption) when `Store` isn't default-constructible-and-`.set()`-able. A real, disclosed behavior
change came with this fix: `build()` now MOVES `store_` out of the builder, so a SECOND `build()` call
on the same instance now fails closed with `quickstart_builder.no_store` rather than the prior
red-team's verified "produces two independent Bundles" non-finding (below) — proven by
`test_session_builder.cpp`'s "B6". No formal `try_compile()` compile-fail gate was added for
the `.api_key_from_env()` `requires`-clause rejection (this project's own established idiom for "must
not compile" claims, e.g. `tests/compile_fail/`) — skipped as disproportionate for a mechanically
obvious constraint, unlike ADR-071's `native_provider_families_distinct` gate, which tested genuinely
non-obvious compile-time logic. **What did NOT hold up, per §0c: the original `.store(Store)` shape**
(by value) — see §0c for the real, compiler-confirmed counter-example and the actual fix.

Two non-findings, verified rather than assumed at the time of the original pass: `build()` genuinely
fails closed on every branch (checked independent of the two the test exercises), and `build()` called
twice was safe before the finding-3 fix above (produced two fully independent `Bundle`s, no aliasing)
— that second property is now INTENTIONALLY changed by the finding-3 fix, not a regression the red-team
would have flagged had it reviewed the fix.

**Verdict from the pass, before the above fixes:** not safe to build §2b/§2d on top of without fixing
finding 1 first; finding 3 should be resolved or loudly disclosed before presenting this as production-
ready. **All three findings are now closed** — 1 and 2 fixed and red-teamed; 3 fixed in a same-session
follow-up, real and tested, but not yet independently red-teamed the way 1/2 were (see §0c: that pass
happened next, and found the finding-3 fix's own `.store()` shape was itself wrong).

## §0c. Second red-team pass, specifically against finding 3's own fix — found it was wrong, fixed again

A fresh pass, aimed narrowly at re-checking finding 3's fix rather than the whole surface (matching how
`.api_key_from_env()`'s original bug in §0b's finding 2 was itself found by a pass that came after the
first fix landed — this project's own established rhythm of re-checking a fix with the same rigor as
the original code). Two real issues found, both fixed; full detail in the header's own top comment
(findings 3 and 4 there — this section summarizes):

1. **FIXED — the fix's own central claim was false, compiler-confirmed.** `.store(Store store)` took a
   `Store` BY VALUE — the header's own comment (and this draft's, above) claimed this worked "for any
   real `SecretStore` conformer." A red-team pass compiled a concrete counter-example against the real
   headers and got a real error: `trust/secret_quarantine.hpp`'s `QuarantineSecretStore` (a real,
   already-shipped conformer, ADR-068) holds `mutable std::mutex mu_` directly, making it neither
   movable nor copyable (`std::mutex`'s copy constructor is deleted, no move constructor exists) — a
   by-value `.store(Store)` parameter cannot even BIND to one, a hard `C2280` compile error, not a
   subtle behavioral gap. This is the exact same class of claim finding 3 itself was supposed to close
   ("a host could not use this builder with a real production secret store, full stop") — the fix moved
   the gap to a second real conformer its own test (B5) never covered. **Fixed**: `.store()` is now a
   variadic, forwarding EMPLACE (`template <class... Args> store(Args&&... args)`), matching
   `AgentSession::emplace_chat_client`'s own established idiom in this exact codebase — constructs
   `Store` IN PLACE from whatever arguments its constructor needs, never requiring `Store` to be movable
   OR copyable at all. `.api_key_from_env()`'s internal use updated to match (default-emplace via
   `.store()`, then mutate through the `unique_ptr` — never moves a `Store` value either).
   Regression-proofed against the actual counter-example: `test_session_builder.cpp`'s "B7"
   builds a real session against `QuarantineSecretStore` — if `.store()` ever regresses back to a
   by-value shape, this stops COMPILING, not merely stops passing.
2. **FIXED — a real test-gap/overclaim, not a bug in the code itself.** B5 (finding 3's proof for the
   generic path) never called `.resolve()`/`chat()` — resolution only happens inside `chat()`
   (`protocol/openai/chat_client.hpp`), which no test in this file invokes (its own top comment names
   this scope limit). So B5 only proved "constructs and wires successfully," not the capability-
   name-match between `.api_key(SecretRef)`'s auto-granted `cap::Secret` and what `EnvSecretSource`/
   `AgentEngineSecretStore` actually check/look up — despite the design draft's own wording claiming
   "proven end to end." **Fixed**: `test_session_builder.cpp`'s new "B5b" calls
   `AgentEngineSecretStore::resolve()` directly against a real `EnvSecretSource`, using the EXACT
   capability-grant shape `.api_key()` produces, and checks the resolved value matches the real env var
   — independent of the full session/`Bundle` machinery, closing the overclaim for real rather than
   just softening the wording.

Non-findings from this pass, verified rather than assumed: the `Store`/`CapabilitySet` reference-lifetime
mechanics §0/§3 were originally worried about are sound under the new emplace-style `.store()` too — the
pass traced `Primary primary(..., *store_, ...)`'s dereference against `std::move(store_)`'s later
handle-transfer precisely and confirmed no dangling-reference window exists (a `unique_ptr` move never
relocates its pointee); the `TestOnlyPopulatableSecretStore` concept gate was checked against every
`.set(name,value)`-shaped type in the tree and correctly admits only `InMemorySecretStore`; and a
legitimate "first `build()` fails due to no store, host calls `.store()`, retries `build()`" workflow
was compiled and run, producing exactly one correct capability grant, no duplication.

**Verdict from this second pass:** finding 3's fix, as it first landed, did not actually deliver on its
own stated generality — real, compiler-verified, not cosmetic. Now fixed and regression-proofed
(findings 3-4 above are closed); neither blocked §2b/§2d (which don't touch this surface) but both are
now closed rather than left as a known gap. This pass's own fixes have NOT themselves been through a
third, independent red-team round — same disclosure posture the original finding 3 had before this
pass found it wrong.

## §0d. Third red-team pass, against §2d's `.approve_tools()`/`.policy()` landing — a real, live-reproduced hang, plus a documentation overclaim

A fresh pass, explicitly asked to take the approval-gating surface seriously as security-adjacent (a
finding that could let something auto-approve that shouldn't would be the most severe class possible
here) — none of that shape was found; what WAS found is a real availability gap unrelated to the
approval logic itself. Full detail in the header's own top comment (finding 7); summarized here:

1. **FIXED — real, LIVE-REPRODUCED hang, not a hypothetical.** `build()` never passed `max_turns`/
   `token_budget` to `AgentSession::initialize()` — that method's own raw default for both is
   `std::nullopt`, and `run_rounds()`'s turn loop is genuinely unbounded when `max_turns_` is unset. The
   red-team compiled and ran a live probe: a scripted `ChatClient` that keeps requesting a tool
   `.approve_tools()` denies, never emitting a terminating text-only reply — ordinary retry-on-denial
   behavior a real model could plausibly exhibit, not a contrived adversarial input. **The process hung
   indefinitely** (killed after 120+ seconds of no progress). Critically, `Bundle::ask()`'s own "bounded
   to one `resume()`" guard (§0b finding 1) provides ZERO protection here: every `chat()` call in this
   engine runs synchronously to completion within one `resume()` — confirmed by finding 1's own comment
   — so the entire unbounded turn loop executes inside that single guarded `resume()` call, a scenario
   the guard was never designed to see. A host using `.approve_tools()` exactly as documented — the
   sugar's whole selling point — had a live, silent hang vector with no failure surfaced. **Fixed**:
   `.max_turns(std::optional<uint64_t>)`/`.token_budget(...)` setters added; `max_turns_` now defaults
   to a FINITE value (25) instead of mirroring `AgentSession`'s own raw unbounded default — a deliberate,
   disclosed divergence from the lower-level API, matching §2a's own "one rung above bare" philosophy
   applied to turn-bounding instead of retry. `.max_turns(std::nullopt)` remains available as an
   explicit, informed opt-out for a host with their own external timeout/cancellation layer.
   Regression-proofed (wiring only, not a re-run of the live hang probe — see the scope-limit note in
   `test_session_builder.cpp`'s own comment on "B11"/"B12"/"B13", and finding 7's own account
   in the header for why a full live-hang test isn't included here — this builder has no
   `.raw_client_only()` escape hatch yet to substitute a scripted client without real network).
2. **FIXED — documentation overclaim, corrected in §2d above.** The design draft's own §2d text claimed
   `.policy()`/`.approve_tools()` were "proven end to end" by "B9"/"B10" — false as written; those tests
   only call the extracted decider directly, never through a live round. A repo-wide grep found exactly
   one non-null `set_approval_decider(...)` call site in the whole codebase: `session_builder.hpp` itself
   — every other approval test in this project deliberately exercises the human-suspend path instead. The
   red-team verified the real wiring live (a scripted round, `approval_decider_` genuinely reached and
   consulted through `invoke_tool()`) and it held up correctly — so this is a test-gap/wording issue, not
   a functional bug — but the claim as written was not true. Wording corrected; a live-round test for
   this specific wiring remains a named, not-yet-added gap.

Non-findings, verified rather than assumed: the `false`-from-installed-decider vs. absent-decider
distinction genuinely doesn't matter behaviorally at the real call site (`tool_pipeline.hpp`'s `approve
&& approve(...)` short-circuits identically either way) — the one place it WOULD matter
(`suspend_for_approval_ && !approval_decider_`) is dead code for every session this builder produces,
since `suspend_for_approval_` defaults `false` and `build()` never touches it (a real, disclosed design
smell — a host cannot currently get BOTH human-suspend-for-unlisted-tools AND auto-approve-for-named-
tools through this sugar — but it fails closed, not open, so not itself a blocking finding). The
allowlist's `std::find` does an exact match with no substring/case-fold bypass; a typo'd name fails
closed. The lambda's captured `allow` vector is a real, independent copy, safe regardless of the
builder's own lifetime after `build()` returns. `.policy()` and `.approve_tools()` compose correctly per
`resolve_approval_outcome()`'s own documented fallthrough — no shadowing bug.

**Verdict:** not clean before the fix — a real, reproduced availability hole directly on the surface this
round was asked to examine. Fixed now; not yet independently red-teamed a fourth time, same disclosure
posture every prior "just fixed" state in this file has had before its own next round.

## §0e. Fourth pass — §2b (history/context composition) resolved, not yet red-teamed

Not a red-team round — a design → prototype pass, resolving §2b (below), which every prior round left
explicitly unattempted. `ComposedQuickstartSessionBuilder<Provider, Store, Ms...>` + `detail::
LazyComposedContextProvider<Ms...>` (see finding 8, header's own comment, and §2b below for the full
account). Real, compiling, passing code — B14-B17 in `tests/test_session_builder.cpp`, driven via
CodeGraph exploration of `rt/agent_session.hpp`/`core/composed_context_provider.hpp`/`core/skill_
provider.hpp` first, matching this project's "explore before editing" convention. Full suite 221/221
(`ctest -LE live-network`), zero regressions; `tests/test_session_builder.cpp` alone now 47/47.

**Not yet independently red-teamed** — every prior round in this file found something real in whatever
landed most recently (finding 3 → 4, finding 5 → 7); this pass has had no round against it yet at all,
a real gap, not an oversight.

## §0f. Fourth red-team pass, against §0e's §2b landing — a real session-isolation gap, plus two diagnostic/test-gap fixes

An independent, adversarial pass (fresh agent, no prior context, matching this project's own "independent
of the implementer" practice) against `ComposedQuickstartSessionBuilder`/`LazyComposedContextProvider`,
scoped explicitly to exclude everything already 3x red-teamed (§0b/§0c/§0d). Verdict: **not clean** —
found and fixed one real, HIGH-severity design gap plus one comprehensibility gap and two test gaps.

1. **HIGH, I1/I4-adjacent — `AgentSession::fork_from()` silently aliases stateful composed providers
   across sessions.** `fork_from()` (`rt/agent_session.hpp:1022`) plain copy-assigns `history_provider_`.
   `LazyComposedContextProvider<Ms...>`'s `contributors_` holds descriptors whose closures capture a
   `shared_ptr<Ms>` BY VALUE (`make_context_provider_descriptor`, pre-existing, `context_assembly.hpp`)
   — an implicit memberwise copy therefore aliases the SAME underlying provider instances, not
   independent ones. LIVE-VERIFIED by the red-team's own probe: copy-assigning a stateful fixture,
   mutating the original via `on_turn_end`, then reading the COPY's `on_context` showed the mutation. A
   forked session was meant to diverge independently; instead a turn-end effect attributed to one
   `Principal` becomes visible through another session's identity. The underlying mechanism predates this
   pass (`core/composed_context_provider.hpp`'s `ComposedContextProvider<Ms...>` has the identical
   exposure) — what's new is that this pass is what first makes it reachable through documented, public
   API composing real stateful providers into a real, fork-capable session, contradicting §7's own
   "no new capability semantics, no new authority path" claim as originally written. **Fixed**:
   `LazyComposedContextProvider` made move-only (copy ctor/assignment deleted, move ctor/assignment
   explicit) — `fork_from()` is an ordinary, non-template member function, so this turns the silent
   runtime aliasing into a compile error at the exact call site, zero effect on every already-passing
   path. `ComposedContextProvider`'s own identical exposure is UNCHANGED — out of this file's scope,
   named as a real, disclosed residual.
2. **MEDIUM, diagnostics — an empty `Ms...` pack produced a confusing multi-error cascade.** Verified: a
   standalone probe compile of `ComposedQuickstartSessionBuilder<Provider::openai, InMemorySecretStore>`
   (zero `Ms`) produced ~20 lines of unrelated failures deep inside `<expected>`/`<type_traits>` before
   the real constraint violation. **Fixed**: `requires (sizeof...(Ms) >= 1)` added directly on
   `ComposedQuickstartSessionBuilder` itself (previously only on the inner `LazyComposedContextProvider`)
   — re-verified after the fix: one clean `C7602: associated constraints are not satisfied` error,
   nothing else. (MSVC required the identical `requires` clause on `Bundle`'s own friend declaration too,
   or the friend and the real definition disagreed — `error C3864: requires clause is incompatible with
   the declaration` — a real MSVC-specific gotcha, not present with clang/g++, fixed alongside.)
3. **Test gap, closed — `on_turn_end` fan-out untested for this type.** `test_composed_context_provider
   .cpp` already proves this for its sibling `ComposedContextProvider`; B14-B17 never did for
   `LazyComposedContextProvider`. Closed as B19.
4. **Test gap, closed — wire order untested with TWO real contributions.** B16 only proved order using
   one contributor that contributes nothing (`HistoryProvider<Window<0>>` on empty history) + one that
   does — unable to distinguish "order preserved" from "only the non-empty one shows up regardless of
   position." Closed as B18, using two instances of the same non-default-constructible fixture type with
   distinct text.

Full suite 221/221 (`ctest -LE live-network`) after all four fixes; `tests/test_session_builder.cpp` now
51/51. **Verdict: findings 9-10 (header's own numbering) fixed; not yet independently red-teamed a fifth
time**, same disclosure posture every prior "just fixed" state in this file has had before its own next
round.

## §0g. Fifth red-team pass, against §0f's finding-9 fix — necessary but not sufficient

An independent, adversarial pass, explicitly prioritized at re-examining finding 7 (round 3's fix) and
findings 9-10 (round 4's fixes) while remaining free to sweep the whole file — matching this project's
own established red-team scoping. Verdict: **finding 7 and finding 10 both held up; finding 9's fix did
not.**

**Finding 11 (I1/I4-adjacent) — finding 9's move-only fix closed the COPY aliasing path but left a MOVE-
based one open, and the moved-from side degraded silently instead of failing closed.** Finding 9 deleted
`LazyComposedContextProvider`'s copy ctor/assignment to block `fork_from()`'s aliasing copy at compile
time, but left move ctor/assignment `= default`ed. A defaulted move correctly drains `contributors_` (a
vector) on the source, but `engaged_` (a plain `bool`) is trivially copied, not reset — so the MOVED-FROM
instance kept `engaged_ == true` over an now-empty `contributors_`. LIVE-REPRODUCED by the red-team's own
probe: `session2.history_provider() = std::move(session1.history_provider())` — the exact bypass route
`on_context()`'s own comment already named as reachable — left session1's `on_context()` **silently
returning a successful, empty contribution** instead of the `not_engaged` error its own guard exists to
produce, and `engage()`'s `already_engaged` guard then **permanently blocked ever re-engaging it** (no
recovery path). This directly falsifies finding 9's own claim that `fork_from()` is "the one real place
this bites" — the same public accessor reaches an *engaged* instance through an entirely different,
ordinary-looking write, and that path is worse: it bypasses BOTH of the class's own fail-closed guards
rather than tripping either one.

**Fixed**: move ctor/assignment now explicitly reset the moved-from side's `engaged_` to `false` (and
defensively clear its `contributors_`, rather than relying on a vector move's typically-but-not-
guaranteed-empty post-move state) — restores the class's own invariant ("`engaged_` implies
`contributors_` is populated") across a move, so a moved-from instance correctly fails closed via the
existing `not_engaged` guard and can be `engage()`d again. Regression-proofed: `tests/test_session_
builder.cpp`'s "B20" — proves the moved-from side now fails with `not_engaged`, that it can be
re-engaged, and that the moved-to side correctly carries the source's own contribution. Deliberately NOT
changed: a move-assignment INTO an already-engaged target still silently replaces its contributors with
no diagnostic — the red-team's own report explicitly separated this from finding 11 proper, since it is
ordinary `operator=` replacement semantics, identical to `history_provider() = HistoryProviderT{}`'s own
pre-existing silent-reset behavior (needed by `AgentSession::clear_in_process_state()`), not a new hazard
the move fix introduced or needed to close.

**Verified non-findings this round** (explored, no bug found): `fork_from()` still genuinely fails to
compile (re-confirmed via a standalone `cl.exe` probe against the current code, `error C2280` at the
exact call site); no other place in `agent_session.hpp` copies or copy-assigns `history_provider_`
(`restore_from_record()` doesn't touch it; `clear_in_process_state()`'s move-assignment from a fresh
prvalue is the only other write site, unaffected by the finding-11 fix); moving a whole `Bundle` after
`engage()` works correctly (heap-owned via `unique_ptr`, so a `Bundle` move only relocates the pointer,
never exercises `HistoryProviderT`'s own movability); `ComposedQuickstartSessionBuilder`'s duplicated
setters have NOT drifted from `QuickstartSessionBuilder`'s (diffed method-by-method, including finding
7's `25`-turn default on both); the default `ContextBudget{}` does not silently drop composed content;
no I2/I3 path found across `.approve_tools()`/`.policy()`/`.providers()`/`.grant()`/`.max_turns()`/
`.token_budget()`/`.api_key()`.

Full suite 221/221 (`ctest -LE live-network`, one unrelated pre-existing flake in `test_native_jail_
parity_windows` confirmed to pass on retry — not touched by this file) after the fix; `tests/test_
session_builder.cpp` now 55/55. **Verdict: finding 11 fixed; not yet independently red-teamed a sixth
time** — same disclosure posture every prior "just fixed" state in this file has had before its own
next round. Finding 7 (round 3) remains at that same posture too — this round found nothing new against
it, but that is a verified non-finding, not proof of correctness beyond what round 5 actually checked.

## §0h. Sixth red-team pass — no functional bug found, one real test-coverage gap closed

An independent, adversarial pass, explicitly re-scoped to prioritize the two items still standing:
finding 7 (round 3's fix, never independently re-examined since) and finding 11 (round 5's fix, given
finding 9's own fix was found insufficient on ITS first re-examination, so finding 11 deserved genuinely
skeptical scrutiny rather than a rubber stamp). Deliberately harsher probing than any prior round applied
to the move machinery specifically: self-move-assignment, move-construction as a distinct path from
move-assignment, a third-generation move-then-re-engage-then-move-again, and a fresh `max_turns`/
`token_budget` audit (loop bound exactness, argument-order at both `initialize()` call sites, whether
`token_budget` is checked before or after a call, whether the two builders' finite defaults have drifted,
whether a slow/pathological `on_context()` could escape the turn bound).

**Verdict: clean — no I1-I4-class or correctness/UB bug found.** This is the first round since round 1
to find no live bug; not proof nothing remains, only that this specific, disclosed probing found none.

**One real test-coverage gap, closed as B21a-d:** B20 (round 5) only proved the class's own invariant
survives ONE generation of move-**assignment** between two distinct instances. Untested: self-move-
assignment (a real edge case the `if (this != &other)` guard exists for, never exercised); move-
**construction** specifically (a separately hand-written code path, not automatically covered by proving
move-assignment correct); a third-generation move (move → re-engage → move again — whether repeated
`clear()`/`reserve()` cycles corrupt anything); and a double-`.build()` regression test on
`ComposedQuickstartSessionBuilder` at all (the base `QuickstartSessionBuilder` has had one since round 1,
B6 — §2b never got its own equivalent, despite finding 11 changing the exact move machinery `build()`/
`engage()` now depend on). Round 6 itself verified all four hold via temporary probes (self-move-assign
preserves content; move-construction resets the source's `engaged_` identically to move-assignment;
third-generation moves carry the right content with the intermediate correctly `not_engaged`; double-
build fails closed with `no_store`, matching the base builder) before reporting this as a coverage gap
rather than a live bug — all four made permanent as B21a-d.

Also re-confirmed as verified non-findings, fresh eyes: `run_rounds()`'s loop runs exactly `max_turns_`
iterations with no off-by-one; both builders' `initialize()` call sites pass `(token_budget_, max_turns_)`
in the correct order, matching `AgentSession::initialize()`'s real parameter order; `token_budget_` is
checked AFTER each call returns (pre-existing `AgentSession` behavior, not introduced or fixable by this
file); the two builders' `25`-turn defaults have not drifted from each other; a same-`name`'d pair of
`RequiredArgProvider` contributors (B18/B19's own fixture) doesn't create attribution ambiguity, since
`assemble_context`'s provenance stamping uses the contributor's INDEX, not its name, as the real
disambiguator.

Full suite 221/221 (`ctest -LE live-network`) after B21a-d landed; `tests/test_session_builder.cpp` now
65/65. Finding 7 has now survived TWO rounds (5, 6) without a new finding against it specifically —
still disclosed as never independently re-examined in its OWN dedicated round the way findings 9/11
were, but no longer untouched by any later round's fresh-eyes sweep either.

## §0i. Seventh red-team pass — finding 7's own dedicated round, at last — a real bug found in `AgentSession` itself

The one item named at the end of §0h as still lacking its own dedicated round: an independent,
adversarial pass scoped SPECIFICALLY and exclusively to `max_turns_`/`token_budget_` (finding 7),
explicitly told to go deep rather than broad. Two live reproductions built as temporary probes.

**Finding 13 (HIGH) — `AgentSession::resolve_codeact_ask()`'s ask-pending branch never advanced
`turn_index`; `max_turns_`/`token_budget_` were COMPLETELY bypassed by a non-converging CodeAct ask
loop.** The bug lives in `rt/agent_session.hpp`, not in `session_builder.hpp` itself — this facade's
own `max_turns_`/`token_budget_` wiring was always correct; what it wires INTO had a real gap this
round's dedicated depth found. `run_rounds()`'s bound only ever inspects `effect_context_.turn_index`;
that field is incremented in exactly four places, and the ask-pending branch returns strictly BEFORE
all four whenever a CodeAct script asks another follow-up question — so `run_rounds()` (where the
bound actually lives) is never even re-entered while the ask loop continues. LIVE-REPRODUCED: a
scripted always-ask-pending tool, `max_turns_ == 3`, driven through 50 `resolve_interaction()` round
trips — `run.max_turns_exceeded` never fired, `turn_index` never left 0, the model (`chat()`) was
called exactly once for the whole 50-round exchange. Host-paced (each round needs a genuine external
`resolve_interaction()` call, not a CPU-spin hang), but any automation layer built on ADR-057's own
`agent.ask()`/`resolve_interaction()` mechanism that answers ask-prompts in a loop reproduces the same
runaway-cost class finding 7 exists to prevent, completely unprotected by the very setter whose entire
purpose is that protection — and unlike `.max_turns(std::nullopt)`, there is no setting that would have
warned a host this specific path is unbounded. **Fixed** in `rt/agent_session.hpp`'s
`resolve_codeact_ask()`: the ask-pending branch now increments `turn_index` and checks it against
`max_turns_` itself, failing closed with `run.max_turns_exceeded` before suspending for another ask
once the cap is reached — mirroring exactly how the ordinary (non-codeact) approval-resume branches one
function up already increment once per call regardless of approved/denied. Regression-proofed:
`tests/test_rt_agent_session_codeact_ask_max_turns.cpp` (R1/R2, no real `MediatedPythonRunner` needed —
any tool returning `error_code == "codeact.ask_pending"` reaches the same path) — verified to actually
have teeth by reverting the fix and confirming the test fails exactly the way the red-team's own probe
did. Also recorded as an addendum to `decisions/ADR-057-agent-ask-suspend-without-deadlock.md` §8, since
the mechanism the gap lived inside is that ADR's own (still Proposed, not yet Judged).

**Finding 14 (MEDIUM, documentation gap, not fixed with code) — the finding-7 disclosure never named
that `max_turns_`/`token_budget_` bound turn COUNT, never turn DURATION.** Live-reproduced: a
`ContextProvider::on_context()` that spins forever with no `co_await` hangs `start_run()` past a 20s
timeout even with `max_turns_ == 25`, the builder's real default. Architecturally expected —
`ContextProvider`/`Tool`/`ChatClientT` are host-authored, trusted code — not a defect in the fix. But
the header's own finding-7 text explains why `Bundle::ask()`'s bounded-resume guard doesn't catch a
model-retry hang without ever stating the identical blind spot applies to a stuck `on_context()`/tool
`invoke()`/`ChatClientT::chat()` itself. Not fixed with code in this pass — the honest gap is in the
DISCLOSURE, addressed by this section existing.

**Two LOW findings, informational:** (a) a suspend exactly at `turn_index == max_turns_ - 1` gets one
extra, unguarded unit of resolution work before the next check catches it — a real ±1 divergence from
"the cap is exact," not itself a hang (each call is host-paced), undisclosed anywhere until now, not
fixed (a one-round grace at the boundary, judged not worth the added complexity of tightening further in
this pass). (b) B11-B13 (session_builder.hpp's own finding-7 regression tests) only ever proved value-
readback through `AgentSession::max_turns()`, matching their own disclosed scope limit — but
`tests/test_rt_agent_session_tool_call_loop.cpp`'s R4/R5 (predating finding 7's own fix commit) already
give the real "does `max_turns_` bound `run_rounds()`'s ordinary loop" proof, live, no network, just
uncross-referenced from this file. Worth a cross-reference, not a new test.

Full suite 222/222 (`ctest -LE live-network`) after the fix (`rt/agent_session.hpp` + the new
`tests/test_rt_agent_session_codeact_ask_max_turns.cpp`) landed — a full rebuild confirmed zero
regressions anywhere else in the tree, including every OTHER `AgentSession` consumer. **Verdict: finding
7's own dedicated round finally ran, and found a real bug — not in this facade, but in the mechanism it
wires into.** Independently re-examined next, in round 8 (§0j below).

## §0j. Eighth red-team pass — findings 11 and 13 both get their own dedicated re-examination round, at last

The two items §0i's own verdict left open: an independent, adversarial pass scoped specifically to (1)
`LazyComposedContextProvider`'s move ctor/assignment (finding 11's own fix, round 5) and (2)
`resolve_codeact_ask()`'s `turn_index` fix (finding 13, round 7) — explicitly told to try to break both
with live reproductions, not merely re-read the diff. Two live probes built, one real new bug found in
each target area.

**Finding 11 — CLEAN.** Self-move-assignment on an engaged instance, and move-assignment INTO an
already-engaged target from a DIFFERENT engaged source, were both live-probed with a destructor-
counting mock provider: no leak (the old contributor's `shared_ptr`-owned instance is genuinely
destroyed by `vector::operator=(vector&&)`), no double-invoke, no aliasing. The header's own claim that
move-into-an-engaged-target is "ordinary, expected `operator=` semantics... not a new hazard" is now
independently verified, not merely trusted from the comment.

**Finding 15 (NEW, MEDIUM) — `LazyComposedContextProvider::engage()` was not exception-safe.**
`build_contributors()` used to `push_back` each `Ms`'s descriptor directly into `contributors_` inside
a comma-fold. If a LATER `Ms`'s move constructor (or `make_context_provider_descriptor()`'s own
internal `make_shared`, a realistic `std::bad_alloc` path) threw partway through, `contributors_` was
left PARTIALLY populated while `engaged_` stayed `false` — the `engaged_ = true` assignment was never
reached. Since `engaged_ == false`, `engage()`'s own `already_engaged` guard did NOT block a retry —
and round 5's own fix comment explicitly frames retry-after-failure as safe, expected recovery — but
`contributors_.reserve()` does not clear existing elements, so a retry's `build_contributors()`
APPENDED onto the stale entries instead of starting fresh. LIVE-REPRODUCED: a throwing-provider mock
(instrumented to throw on a specific, counted move — calibrated so the throw fires genuinely INSIDE
`engage()`'s own machinery, not while binding the caller's argument tuple), first `engage()` throws
after 1 of 2 contributors pushed, a retry with fresh, non-throwing providers succeeds, then ONE
`on_context()` call invoked the stale first-attempt contributor a SECOND, duplicate time — exactly the
"duplicate every contributor on the wire" hazard `already_engaged` exists to prevent, reintroduced via
the exception path. **Fixed**: `engage()` now builds into a LOCAL vector and publishes into
`contributors_` (and flips `engaged_`) only once every `Ms` has been constructed without throwing — a
strong exception guarantee, so a throw now leaves `contributors_`/`engaged_` completely untouched and
a retry genuinely starts fresh instead of accumulating wreckage. Regression-proofed: `tests/test_
session_builder.cpp`'s "B22" — verified to have teeth (reverting the fix reproduces the exact
double-invoke failure this section describes).

**Finding 13's surrounding mechanism — the original fix confirmed correct; two new LOW findings found
and fixed nearby.** `token_budget_` irrelevance to the ask-loop confirmed genuinely correct (no model
call happens during pure ask/resolve cycling — only tool re-invocation). Double-resolve after
`run.max_turns_exceeded` confirmed clean (both `pending_codeact_asks_` and `open_interactions_` are
erased unconditionally before that branch returns; a second resolve attempt correctly gets
`unknown_id`). The already-disclosed ±1 grace (§0i's own LOW finding (a)) was probed further and
confirmed NOT to compound across multiple, separate ask-phases within one run — a multi-phase scripted
probe (a new ask-phase starting every round) still hit the exact same 3-call bound as the original
single-continuous-chain test, ruling out a "worse than characterized" hypothesis.

Two NEW LOW findings, both confirmed by direct source reading (not just trusted from the sub-agent that
first surfaced them), and both fixed:
- **Finding 16.** `resolve_codeact_ask()`'s missing-record guard carried a comment claiming "should be
  unreachable in practice" — WRONG. `restore_from_record()` restores `open_interactions_` (which can
  contain a `codeact_ask`-reason `Interaction`) but `PendingCodeActAsk` is deliberately NOT included in
  `AgentSessionRecord` at all (`PendingCodeActAsk`'s own comment already discloses this durability gap,
  directly related to §8's "Expiry" residual in `decisions/ADR-057-agent-ask-suspend-without-deadlock.
  md`) — so resolving a codeact_ask interaction that survived a session restore genuinely reaches this
  branch. It used to return `fatal` WITHOUT erasing the interaction from `open_interactions_`, leaving
  it stuck open forever with no cancel path (every future resolve attempt hit the same branch again).
  **Fixed**: erases the interaction too (best-effort) before returning the fatal error — still fails
  closed, but recoverably. Verified by code review only, not live-reproduced: reaching this branch
  through the public API alone would require reconstructing `history_` to look like a genuinely
  suspended tool-call turn, and `history_` has no public mutator — matching this project's own
  disclosed-scope-limit precedent for round 1's `ask_mutex_` concurrency fix (§0b finding 1 above,
  "verified by code review against the cited precedent only").
- **Finding 17.** `clear_in_process_state()` never cleared `pending_codeact_asks_`, unlike every other
  piece of interaction state it resets (`open_interactions_`, `standing_effects_`, etc.) — its own
  contract is "no residue left to read back through ANY of this class's own accessors" (005 §6), which
  this silently violated in spirit (no accessor existed to observe it, so not the letter). A
  pooled/reused `AgentSession` (the `Stateless<N>` pooling pattern) that clears+reinitializes with a
  DIFFERENT `session_id_` after an in-flight codeact-ask permanently retained that record — full script
  source plus every answer given — for the C++ object's remaining lifetime, unreachable for erasure
  since future interaction ids embed the new session_id and can never match the orphaned key. **Fixed**:
  now cleared alongside everything else. A new `pending_codeact_ask_count()` accessor was added (mirrors
  `admission_denied_count()`'s own shape) specifically so this previously-unobservable leak could
  actually be checked — regression-proofed and verified to have teeth: `tests/test_rt_agent_session_
  codeact_ask_max_turns.cpp`'s new "R3".

Full suite 222/222 (`ctest -LE live-network`) after all three fixes (`session_builder.hpp`'s `engage()`,
and `rt/agent_session.hpp`'s two LOW fixes) landed — a full rebuild confirmed zero regressions anywhere
else in the tree. **Verdict: both open items from §0i's verdict got their dedicated round. Finding 11
held up; finding 13's own fix held up, but the round found one new MEDIUM bug (15) in the neighboring
class it was told to also stress, and two LOW findings (16, 17) in the mechanism finding 13 lives
inside.** Finding 15 has not yet been independently re-examined a second time.

## §0k. `.raw_client_only()` implemented (2026-09-03) — resolving a real ambiguity this draft's own §2a prose left open

This section's own §2a prose ("bypasses `ModelCallGateway` entirely and installs the bare client
directly — for a deterministic fake (`JokerChatClient`...) or a test double") reads two different ways:

1. **Reading (a), "unwrap the same real backend":** keep `Primary` (`OpenAIChatClient`/
   `AnthropicChatClient`, from `.openai()`/`.anthropic()`), skip only `ModelCallGateway`'s retry/
   circuit-breaker wrapping around it. A small, well-scoped change (one NTTP + one method + an
   `if constexpr` in `build()`).
2. **Reading (b), "install a caller-supplied ChatClient":** let the host hand over an entirely
   different, already-constructed `ChatClientT` (a scripted double, unrelated to `Provider`), bypassing
   `.openai()`/`.anthropic()`/credential machinery altogether.

These are NOT the same feature, and only one of them resolves what two other files in this codebase
independently say `.raw_client_only()`'s absence blocks: `tests/test_session_builder.cpp`'s own comment
on B14-B17 ("it has no `.raw_client_only()` escape hatch... to substitute a scripted client without real
network") and `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md:1218`'s identical
framing. Reading (a) alone does NOT close that gap — a bare `Primary` is still a real OpenAI/Anthropic
backend that still does real HTTP the instant `.ask()` is called. Only reading (b) does.

Project owner confirmed reading (b) (2026-09-03, ambiguity surfaced via `AskUserQuestion`) as the
intended, larger design — explicitly accepting the bigger scope (a new builder type, not a fluent method
on `QuickstartSessionBuilder`) over the smaller reading that would have left the B14-B17/ADR-102 claim
still false after "implementing" `.raw_client_only()`.

**Implemented as `RawQuickstartSessionBuilder<ChatClientT, Store>`** (`session_builder.hpp`, right after
the `OpenAiSessionBuilder`/`AnthropicSessionBuilder` aliases) — a separate builder type, not a fluent
`QuickstartSessionBuilder` method, for the identical structural reason §2b needed
`ComposedQuickstartSessionBuilder`: a caller-supplied `ChatClientT` is a genuinely different C++ type
from `Primary`/`ModelCallGateway<Primary>`, so there is no single `build()` return type a runtime toggle
on the existing class could produce. `ChatClientT` is concept-constrained
(`agentengine::ChatClient`, `core/chat_client.hpp`) for a named diagnostic instead of a template-error
cascade. Duplicates `QuickstartSessionBuilder`'s `session_id()`/`principal()`/`max_turns()`/
`token_budget()`/`store()`/`grant()`/`approve_tools()`/`policy()` rather than sharing them through a
common base — the same, already-named simplification §2b's own comment makes, for the identical reason
(extracting a shared base would mean touching the already-shipped, already-red-teamed §2a class too, a
larger, separate-risk refactor). Does NOT duplicate `.declare_capabilities()`/`.endpoint()`/`.api_key()`/
`.api_key_from_env()` — those exist only to configure `Primary`'s construction, which this builder never
does; there is no credential slot on this class at all, so `build()`'s `CapabilitySet` holds only
explicit `.grant()` calls, never an auto-derived `cap::Secret`.

**A real correctness gap found and fixed during implementation, not anticipated by this section's own
sketch above:** unlike `QuickstartSessionBuilder::build()`, `!store_` alone cannot detect a repeat
`.build()` call here. The base builder's double-build guard works because `.api_key(...)` is mandatory
and `store_` moving out is the ONLY way `!store_` becomes true after a successful build — so a second
call reliably hits `no_store`. `RawQuickstartSessionBuilder` has no mandatory credential, and `Store`
defaults to `InMemorySecretStore` (default-constructible) — without an explicit guard, a second
`.build()` call would silently default-construct a FRESH `Store` (since `store_` was moved out by the
first call) and move an already-moved-from `client_` into a second session, producing a Bundle wrapping
a moved-from `ChatClientT` instead of failing closed. **Fixed**: an explicit `built_` flag fails closed
with `raw_quickstart_builder.already_built` — a DIFFERENT error code from the base builder's `no_store`,
deliberately, so a caller (or a test) can tell which guard actually fired rather than reading `no_store`
as evidence the dedicated check ran.

**Round 2 (code review, same day): `built_` was itself flipped too early.** The first version set
`built_ = true` right after the `!store_` check, BEFORE `capabilities`/`session` were allocated — both a
real `std::bad_alloc` path (`std::make_unique<...>`, per `AgentSession::emplace_chat_client`'s own
declaration comment, `rt/agent_session.hpp`). A transient allocation failure there permanently bricked
the builder (every retry hit `already_built` forever) even though `client_` had not been touched yet and
a retry would have been perfectly safe — the doc comment's own promise ("never a silent partial build")
did not actually hold for this specific throw window. **Fixed**: `built_` now flips immediately before
`client_` is actually consumed (`emplace_chat_client(std::move(client_))`), not one statement earlier —
any throw BEFORE that point leaves `client_`/`store_` untouched, so `built_` staying `false` correctly
permits a real retry; any throw AT OR AFTER that point means `client_` is gone regardless, so `built_`
must already read `true` to stop a retry from moving an already-moved-from client into a second session.
Same pass also fixed a minor, real inefficiency: `CapabilitySet::grant_root(grants_)` copied `grants_`
where `std::move(grants_)` is free and correct (`grants_` is never read again, and this code path runs
at most once per instance).

Regression-proofed: `tests/test_session_builder.cpp`'s new B26-B29 (5th pass) — B26 proves the happy
path (default-constructed `Store`, `.grant()` reaching `capabilities()`); **B27 is the proof that
actually closes the B14-B17/ADR-102 gap**, driving a REAL, live `AgentSession::start_run()` (via
`Bundle::ask()`) against a scripted `ScriptedChatClient` fixture (same shape as `JokerChatClient`, kept
test-local since `examples/` isn't linked into the test binary) with zero network dependency and reading
back the exact scripted reply text; B28 proves the double-build guard fires with its own error code, and
that the first, successful `Bundle` remains fully usable afterward; B29 proves the `no_store` fail-closed
path for a genuinely non-default-constructible `Store` (a local `NoDefaultStore` fixture). Full suite
green (`test_session_builder` 93/93 checks standalone, 0 failures; full-tree rebuild and the rest of
`ctest -LE live-network` unaffected — 12 pre-existing, unrelated Docker/native-jail/sandbox-timing
failures in that run, none linking `session_builder.hpp`) on Windows/MSVC, `AGENTENGINE_WITH_HTTPS=ON`.

Reading (a) (stripping `ModelCallGateway` off an otherwise real `.openai()`/`.anthropic()`-configured
`Primary`) remains unimplemented — a real, smaller, separate gap if a host ever specifically wants
"real backend, no retry/breaker," named here rather than silently conflated with this class. Not yet
independently red-teamed a second time (this is the first pass); a candidate follow-up, matching this
draft's own established discipline for a change of this size.

## 0. Correction found during implementation — §3's own fix does not compile as written

**`AgentSession` cannot be moved or copied at all.** It holds `rt::AsyncMutex session_mutex_`
(`agent_session.hpp`) directly as a data member; `AsyncMutex`'s copy constructor is explicitly deleted
and it declares no move constructor of its own (`async_mutex.hpp`) — by the ordinary C++ rule ("a
user-declared copy/move special member suppresses every implicitly-generated move member"),
`AgentSession` is therefore neither movable nor copyable. §3 below (kept verbatim, as originally
written) designed `Bundle` around a plain, **move-constructed** `SessionT session_` member — that does
not compile, full stop, not merely a lifetime risk.

**Fix, implemented:** `Bundle` heap-allocates the `AgentSession` itself (`std::unique_ptr<SessionT>`),
configured in place via `initialize()`/`emplace_chat_client()`/`set_capabilities()` and never moved
afterward — matching how every existing example/test already uses `AgentSession` (constructed once, in
place, configured, never relocated). This closes §3's own `Store`/`CapabilitySet` reference-lifetime
finding as a side effect, via one uniform rule ("everything long-lived is heap-owned by `Bundle` at a
stable address") instead of the session-specific exception §3 originally proposed. §3's *diagnosis*
(the reference-lifetime hazard) was correct; only its proposed *fix shape* was wrong. Left unedited
below for the historical record — read this correction first.

**Origin:** a same-session conversation started from "is the engine API too verbose to build a new
app with?" — verified concretely, not assumed: `examples/01_hello_agent.cpp` is ~125 lines for an
*offline, fake-client* hello-world; a real provider (`tools/cli_chat.cpp:1051-1076`) needs a hand-
assembled `SecretStore` + `Capability{cap::Secret{...}}` + `CapabilitySet::grant_root({...})` before a
`ChatClientT` can even be constructed. This draft is the concrete design for the convenience layer
that conversation converged on, extended per this session's own follow-up: AgentEngine's model-call
and context-provider surfaces are each a *stack* of composable wrapper types (`ModelCallGateway`,
`MiddlewareModelCallGateway`, `ContentReplayGateway`; `HistoryProvider<Window/Summarize>`,
`ComposedContextProvider<Ms...>`), not one type each — a builder that defaults to the *simplest*
member of each stack (a bare `OpenAIChatClient`, a bare `HistoryProvider<Window<0>>`) would be
strictly worse than what a careful integrator already builds by hand today.

## 1. What problem this solves, and what it explicitly does not touch

**Solves:** the *bootstrap/wiring* ceremony a host must perform once, at startup, to get from "I have
an API key and a model name" to a running `AgentSession` — secret storage, capability granting,
`ChatClientT` construction and stacking, `HistoryProviderT` composition, sending one message and
reading the reply back as plain text.

**Does not touch:** `Agent<Policies...>` authoring (CRTP policy composition + its declarative YAML/
JSON equivalent, I6) — CLAUDE.md's locked "v1 authoring surfaces are C++ CRTP and declarative YAML/
JSON" decision is about *how an agent's behavior is declared*, not about how a host constructs and
configures the runtime objects (`SecretStore`, `CapabilitySet`, `ChatClientT`, `AgentSession`) that
declaration eventually runs inside. This builder is a facade over the *second* thing only. Named
explicitly so a future reader doesn't mistake it for a second authoring surface and reopen a locked
decision that was never actually in scope.

## 2. Design — per-slot, matching what already exists, not inventing new mechanism

The house rule from `tool-optimizer-provider-design-draft.md` applies here too: prefer an ordinary
composite over existing, already-proven types to a new generic mechanism. Every builder method below
constructs exactly the same object graph a careful hand-written integration already would — it is a
facade, not a new runtime concept.

### 2a. `ChatClientT` slot — the stack, defaulted one rung above "bare client"

Verified via CodeGraph (`include/agentengine/core/model_call_gateway.hpp`,
`include/agentengine/protocol/{openai,anthropic}/chat_client.hpp`):

| Layer (outer → inner) | Type | ADR |
|---|---|---|
| discard-and-retry on policy violation | `ContentReplayGateway<Inner>` | 069 |
| cross-cutting middleware chain | `MiddlewareModelCallGateway<Inner, Ms...>` | 033/036 |
| retry + circuit-breaker + failover | `ModelCallGateway<Primary, Fallback...>` | 036 |
| raw backend | `OpenAIChatClient<Store>` / `AnthropicChatClient<Store>` | 004 |

```cpp
// core/session_builder.hpp (sketch, not implemented)
template <SecretStore Store>
class QuickstartSessionBuilder {
public:
    QuickstartSessionBuilder& openai(std::string model, SecretRef key, ChatClientCapabilities caps);
    QuickstartSessionBuilder& anthropic(std::string model, SecretRef key, ChatClientCapabilities caps);
    // ^ exactly ONE primary backend call, required -- picks host/port/path_prefix by well-known
    //   provider default (api.openai.com / api.anthropic.com), overridable via .endpoint(...).

    QuickstartSessionBuilder& with_fallback(/* same shape as the primary call, any backend type */);
    QuickstartSessionBuilder& with_retry(RetryPolicy);          // else ModelCallGateway's own default
    QuickstartSessionBuilder& with_middleware(auto... ms);      // wraps in declared order
    QuickstartSessionBuilder& with_content_replay(ContentReplayTrigger);

    // ...history/context (2b), capability/secret (2c), approval (2d)...

    auto build();   // returns a move-only Bundle -- see §3, the store-lifetime landmine
};
```

`build()` **always** produces at least `ModelCallGateway<Primary>` (empty fallback tuple, default
`RetryPolicy{}`/`BreakerConfig{}`) — never a bare `OpenAIChatClient`/`AnthropicChatClient` directly in
`AgentSession`'s `ChatClientT` slot. Justification, cited: `ModelCallGateway`'s own constructor
(`model_call_gateway.hpp:136-138`) accepts a **default-constructed** `RetryPolicy`/`BreakerConfig` and
an **empty** `std::tuple<>` fallback — the marginal ceremony over a bare client is one extra template
argument, not one extra concept the host must learn, so there is no honest "simple mode" that a raw
client is actually simpler than. `MiddlewareModelCallGateway`/`ContentReplayGateway` stay opt-in
(`with_middleware`/`with_content_replay`) since they need host-supplied types (middleware conformers,
a `ContentReplayTrigger`) the builder cannot default sensibly.

**Composition order is builder-owned, not caller-owned.** `.with_middleware(...)` and
`.with_content_replay(...)` may be called in any order the host finds natural; `build()` always nests
them `ContentReplayGateway<MiddlewareModelCallGateway<ModelCallGateway<...>, Ms...>>` — outer-to-inner
fixed, per `002-Agent-Model-and-Authoring.md:191-196`'s own stated reasoning for why this nesting order
is correct (review the final, post-retry outcome once, not once per retry attempt). A host calling
these methods in the "wrong" order gets the *correct* object graph anyway — the one property a facade
must guarantee, or it isn't saving the host from a real mistake class.

**Escape hatch, not a second default:** `.raw_client_only()` bypasses `ModelCallGateway` entirely and
installs the bare client directly — for a deterministic fake (`JokerChatClient`,
`examples/01_hello_agent.cpp`) or a test double, where retry/breaker semantics are meaningless noise.
Named explicitly as escape hatch, not a coequal option, so a reader doesn't reach for it by habit in
production code.

**IMPLEMENTED, 5th pass (2026-09-03): as `RawQuickstartSessionBuilder<ChatClientT, Store>`, a separate
builder type, resolving a real ambiguity this very paragraph's prose left open — see §0k below for the
full account (two competing readings, which one project-owner confirmed, the correctness gap found and
fixed during implementation, and the regression proof).

### 2b. `HistoryProviderT`/context slot — RESOLVED and red-teamed three times (rounds 4-6, §0f-§0h), see findings 8-12

Verified via CodeGraph (`include/agentengine/core/composed_context_provider.hpp`,
`include/agentengine/core/skill_provider.hpp`): `ComposedContextProvider<Ms...>`'s own file-top comment
(`composed_context_provider.hpp:30-33`) states contributor order **is** wire-message order; separately,
`skill_provider.hpp`'s own history (cited in `history_and_skills_provider.hpp`) already established
that skills must precede history on the wire. This is exactly the class of detail a hand-rolled
integration gets right once by luck and a builder should get right by construction, every time — the
motivation below still holds; only the sketch below turned out not to be directly implementable.

```cpp
QuickstartSessionBuilder& window(std::size_t n = 0);          // HistoryProvider<Window<n>>, default
QuickstartSessionBuilder& with_skills(std::vector<SkillSourceDescriptor>);
QuickstartSessionBuilder& with_memory(/* MemoryProvider ctor args */);
QuickstartSessionBuilder& with_rag(/* VectorRagContextProvider ctor args */);
QuickstartSessionBuilder& with_tool_optimizer(/* once it exists -- see tool-optimizer-provider-design-draft.md */);
```

**Found during implementation, not anticipated by this sketch:** `HistoryProviderT` is a COMPILE-TIME
type parameter on `AgentSession<ChatClientT, StateT, HistoryProviderT>`, exactly like `ChatClientT`
(§2a). §2a's own builder already solved the analogous problem for `Provider` (openai vs. anthropic) by
making it a template parameter chosen ONCE, at construction — not a runtime fluent toggle, because a
runtime choice between two different C++ types has no single, clean `build()` return type. That solution
does not scale to `.with_skills()`/`.with_memory()`/`.with_rag()`: unlike `Provider` (one value, chosen
once), history/context composition is naturally MULTI-VALUED and incremental — any subset, in any
combination, of an open-ended set of contributors. A single extra template parameter can pick one of two
things; it cannot represent "any subset of N optional contributors" without either an exponential blow-up
of specializations or a genuinely different construction shape.

Two real ways to actually build this were named, neither attempted at the time:
1. A true type-changing fluent builder — every setter `&&`-qualified, returning a NEW specialization of
   `QuickstartSessionBuilder` with an extended `HistoryProviderT`. A real, cross-cutting refactor of this
   whole file (`.api_key()`/`.store()`/`.grant()`/`.approve_tools()` would all need the same treatment
   for the chain to keep working end to end), not an additive change scoped to just the new methods.
2. A distinct, separately-templated builder type specifically for the composed-context case (e.g.
   `ComposedQuickstartSessionBuilder<Provider, Store, Ms...>`), accepting the up-front ceremony of naming
   the composition shape in the type instead of building it up fluently.

**RESOLVED, 4th pass (2026-08-22): option 2, implemented as `ComposedQuickstartSessionBuilder<Provider,
Store, Ms...>`.** `Ms...` is fixed at the builder's own declaration (same reasoning as `Provider`); a
single `.providers(std::tuple<Ms...>, budgets)` call supplies the real, host-constructed values instead
of one setter per provider type (there is no generic way to name "the SkillsProvider slot" in an
arbitrary pack without either a `requires`-constrained lookup-by-type or exposed index positions — a
real, deliberately NOT-taken third option, since the underlying providers' own constructors are already
the ergonomic surface, e.g. `SkillsProvider{sources}` written directly into the tuple). Option 1 (the
type-changing fluent builder) was not attempted — option 2's own mechanism made it unnecessary, not
merely deferred a second time.

**What this analysis had NOT anticipated, found during implementation:** `AgentSession<...>::
history_provider_` is a PLAIN value member, always default-constructed (`rt/agent_session.hpp:2059`; no
user-declared `AgentSession` constructor exists to route around this) — so `HistoryProviderT` must be
default-constructible REGARDLESS of which of the two options above got picked. The project's own
existing `ComposedContextProvider<Ms...>` (`core/composed_context_provider.hpp`) only satisfies that
when EVERY `Ms` is itself default-constructible — true for `HistoryProvider<Window<n>>` or a mock, never
true for real `SkillsProvider` (explicit ctor, requires a `vector<SkillSourceDescriptor>`, no default),
`MemoryProvider`/`VectorRagContextProvider` (both take required reference parameters). Confirmed
empirically: `tests/test_composed_context_provider.cpp`'s only real `AgentSession` proof
(`ThreeWayProvider`) uses three deliberately default-constructible MOCK providers, never a real
`SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider` — composing any of those three into a real
`AgentSession` had never actually been exercised, a materially bigger gap than this section's original
text captured (it named "the shape of the builder problem," not "the underlying `AgentSession`-slot
constraint blocks the interesting cases regardless of builder shape"). Fixed with
`detail::LazyComposedContextProvider<Ms...>`, kept local to `session_builder.hpp` — see finding 8,
`session_builder.hpp`'s own top comment, for the mechanism. Also found, separately: `composed_context_
provider.hpp`'s own comment ("no emplace_*/accessor pair" for `history_provider_`) is now STALE —
`AgentSession::history_provider()` (`rt/agent_session.hpp:647`) is a real, mutable-reference accessor,
added since that comment was written; not relied on to bypass the default-constructibility constraint
above (it doesn't — `AgentSession` still needs to default-construct the member once before the accessor
can be used), but relied on to install the REAL, engaged provider set after that first, placeholder
default construction.

### 2c. Capability/secret slot — pure sugar, zero new default authority (unchanged from the prior survey, IMPLEMENTED)

`.openai(model, key, caps)` above does **not** itself grant `cap::Secret` — it only names which
`SecretRef` the constructed `ChatClientT` will resolve at call time (`chat_client.hpp:891-893`'s own
"resolution happens inside chat(), never at construction" rule, unchanged). Granting is a **separate,
mandatory** call:

```cpp
QuickstartSessionBuilder& secret_from_env(std::string secret_name, char const* env_var);
    // -> Store::set(secret_name, getenv(env_var)) + a queued Capability{cap::Secret{secret_name, ttl}}
QuickstartSessionBuilder& grant(Capability);   // escape hatch for anything else (FsRead, NativeExec, ...)
```

`build()` fails (a `result<Bundle>` error, not a thrown exception or a silent no-op) if `.openai(...)`/
`.anthropic(...)` named a `SecretRef` that no `.secret_from_env(...)`/`.grant(...)` call ever covered —
catching the exact "host forgot to grant the key it referenced" mistake at build time instead of at
first-call time deep inside a coroutine. This is a real ergonomic win the raw API does not offer today
(a missing grant currently surfaces as a `chat()`-time `failure_class::policy` error, mid-run).

Implemented as `.api_key(SecretRef)` + `.store(Args&&...)` + the `requires`-gated `.api_key_from_env(...)`
convenience — a real generalization past this sketch's `secret_from_env`-only shape, and past two rounds
of red-team findings; see §0b/§0c for the full account of what changed and why.

### 2c. Capability/secret slot — pure sugar, zero new default authority (unchanged from the prior survey)

`.openai(model, key, caps)` above does **not** itself grant `cap::Secret` — it only names which
`SecretRef` the constructed `ChatClientT` will resolve at call time (`chat_client.hpp:891-893`'s own
"resolution happens inside chat(), never at construction" rule, unchanged). Granting is a **separate,
mandatory** call:

```cpp
QuickstartSessionBuilder& secret_from_env(std::string secret_name, char const* env_var);
    // -> Store::set(secret_name, getenv(env_var)) + a queued Capability{cap::Secret{secret_name, ttl}}
QuickstartSessionBuilder& grant(Capability);   // escape hatch for anything else (FsRead, NativeExec, ...)
```

`build()` fails (a `result<Bundle>` error, not a thrown exception or a silent no-op) if `.openai(...)`/
`.anthropic(...)` named a `SecretRef` that no `.secret_from_env(...)`/`.grant(...)` call ever covered —
catching the exact "host forgot to grant the key it referenced" mistake at build time instead of at
first-call time deep inside a coroutine. This is a real ergonomic win the raw API does not offer today
(a missing grant currently surfaces as a `chat()`-time `failure_class::policy` error, mid-run).

### 2d. Approval/policy slot — thin sugar over `ApprovalDecider`/`PolicyDecider` (ADR-070), CORRECTED then IMPLEMENTED

Original sketch, kept for the record — **this method name is wrong against the real mechanism**:

```cpp
QuickstartSessionBuilder& require_approval_for(std::vector<std::string> tool_names);  // WRONG NAME, see below
QuickstartSessionBuilder& policy(PolicyDecider);
```

**Found during implementation, before any code landed:** `ApprovalDecider` (`core/tool_pipeline.hpp`) is
consulted ONLY for a call a tool's OWN declared `approval_mode` already marked as needing a decision
(`always_require`, or `policy_driven` with no `PolicyDecider`) — it has no power to make MORE tools
require approval than their own declaration already does; `require_approval_for(tool_names)` reads as
though it could. **Implemented instead as `.approve_tools(std::vector<std::string> tool_names)`**:
installs a decider that auto-approves ONLY the named tools and denies every other already-gated call —
the safe default. This can only ever turn an already-required human decision into an immediate deny or
an immediate host-declared approve; it can never skip a decision that was never required, and can never
turn a decision into an approval for a tool not explicitly named (I2: narrows/decides among
already-required decisions, never widens which calls need one). If never called, no `ApprovalDecider` is
installed at all — verified, not merely asserted (`test_session_builder.cpp`'s "B8": the
session's `approval_decider()` is genuinely empty, `static_cast<bool>(...)` false, not an always-false
stand-in). `.policy(PolicyDecider)` is unchanged from the sketch — a thin, unmodified pass-through to
`set_policy_decider`. **Correction (round 3, §0d): "proven end to end" below overclaimed what "B9"/"B10"
actually test** — both only extract the `ApprovalDecider`/`PolicyDecider` and call it directly, never
through a live `start_run()` round; a round-3 red-team pass live-verified the real wiring separately
(a scripted round, `approval_decider_` genuinely consulted through `invoke_tool()`) and it held up, but
that proof does not live in this repo's own test suite yet — a named, disclosed test gap, not a bug.

Landed in this same pass, then red-teamed once (§0d) — see §0d for what that pass found (a real,
live-reproduced hang unrelated to the approval logic itself, plus the overclaim corrected above).

## 3. The one integration point that must not be gotten wrong

**`OpenAIChatClient<Store>`/`AnthropicChatClient<Store>` hold `Store const&` — a reference member, not
a value** (confirmed: `chat_client.hpp:949`, `Store const& store_;`). A naive `build()` that
stack-allocates the `SecretStore` as a local inside the builder method and returns
`AgentSession<...>` **by value** produces an immediately-dangling reference the moment `build()`
returns — a real, silent memory-safety bug, not a hypothetical one, and exactly the kind of landmine a
convenience layer exists to remove rather than reintroduce under a friendlier name.

**Required shape:** `build()` must return a single move-only `Bundle` that owns the `Store` at a
stable address (heap-allocated inside `Bundle`, e.g. `std::unique_ptr<Store>`) *and* the
`AgentSession<...>` referencing it, constructed in that order and never separated — the same
"ownership and the reference into it travel together, or not at all" rule `Store const&` already
imposes on any hand-written integration today. `Bundle` is not copyable (matching `AgentSession` itself
being move-only through its `chat_client_`/`history_provider_` members) and exposes `.session()`/
`.ask(text)` (§4) as its only surface — the raw `Store`/`ChatClientT` are not meant to be reached back
out to individually, since doing so would let a caller separate them again.

## 4. `.ask(text)` — the one-shot round-trip sugar

```cpp
// Bundle::ask, sketch
result<std::string> ask(std::string text) {
    auto r = drive(session_.start_run(StartRun{user_message(std::move(text))}));
    if (!r) return std::unexpected(r.error());
    return text_of(r->message);
}
```

Directly mirrors `examples/01_hello_agent.cpp:83-101,114-120`'s own `user_message()`/`drive<T>()`/
`text_of()` sequence — no new mechanism, just named and owned once instead of copy-pasted per example/
app. `drive<T>()`'s own safety precondition (`agentengine/rt` task never genuinely suspends on external
I/O within one `resume()` loop) holds for a synchronous, non-gateway-streaming call exactly as it does
in every existing `rt::` test file — `Bundle::ask` does not change that precondition, it packages an
already-safe idiom.

## 5. Self-red-team

- **I2 (no ambient authority):** every `.grant(...)`/`.secret_from_env(...)`/`.require_approval_for(...)`
  call maps 1:1 to an explicit `Capability`/`ApprovalDecider` the host itself named — `build()` never
  synthesizes a capability the host didn't request (§2c). The `build()`-time "referenced secret has no
  grant" check (§2c) is a *stricter* failure mode than today's raw API, never a looser one — it can only
  reject a construction that would already have failed later, never admit one that wouldn't have.
- **I3 (model output never authority):** nothing in the builder's surface accepts a `Tainted<T>` or
  anything derived from a `ChatResponse`/tool result — every builder method's arguments are host-authored
  literals/config, matching ADR-071 §4 property 4's identical requirement for `Native*Provider`
  construction.
- **Does defaulting to `ModelCallGateway<Primary>` (§2a) change observable behavior vs. a bare client in
  a way a host must know about?** Yes, named honestly rather than hidden: retries change *timing* (a
  transient failure that would have surfaced immediately now retries per `RetryPolicy::default`'s own
  attempt count/backoff first) and `run_start`'s own event-stream warning
  (`agent_session.hpp:826-832`, "a gateway-routed round makes several real backend calls... before the
  per-run token budget is checked") applies unconditionally the moment `build()` installs a gateway by
  default. This is a real, disclosed trade this draft makes deliberately (§2a's own justification: the
  marginal ceremony is near-zero, so defaulting to the safer stack is the right call) — not a residual
  to fix later.
- **Does the `ComposedContextProvider` ordering choice (§2b) hide a decision the host might disagree
  with?** The ordering itself (skills before history) is not this design's own opinion — it is
  `history_and_skills_provider.hpp`'s already-shipped, already-correct wire-order rule, just applied
  generically instead of re-derived per app. A host who genuinely needs a different order has the
  existing `ComposedContextProvider<Ms...>` (or `AgentSession`'s raw template parameter) as an escape
  hatch — the builder narrows the *easy* path, it does not remove the general one.
- **Store-lifetime landmine (§3):** the one finding in this pass with real memory-safety consequences if
  gotten wrong — called out as its own numbered section rather than folded into the self-red-team list,
  matching this project's convention of giving a load-bearing implementation risk its own visible home
  (`tool-optimizer-provider-design-draft.md §3`'s identical treatment of its own highest-risk point).
- **I6 (declarative/native equivalence):** does not apply — reconfirmed in §1, this facade sits below
  `Agent<Policies...>`/YAML authoring, never inside it. No declarative-surface analogue is owed by this
  design; flagged so a future reader doesn't manufacture a YAML-builder-parity requirement that was
  never actually implied.

## 6. Open questions — not designed here

- Should `Bundle` also expose a streaming `.ask_stream(text) -> stream<std::string>` sugar over
  `chat_stream()`/`stream_model_calls_`? Real demand likely (see `examples/07_streaming.cpp`), but adds
  its own event-plumbing surface — deliberately deferred to keep this draft's first cut to the
  synchronous case only. **Confirmed, during this same conversation, to be structurally impossible on
  top of §2a's current default `ChatClientT` (`ModelCallGateway<Primary>`) as things stand today** — a
  gateway-typed `AgentSession` emits zero `model_delta` events regardless of `stream_model_calls_`
  (`agent_session.hpp`'s own `ModelCallGatewayLike` branch). Tracked separately, deliberately not
  designed here or fixed in isolation: `docs/planning/quickstart-builder-streaming-gap.md`.
- Whether a `.dev_defaults()` convenience (wires an in-memory, non-persistent `SecretStore` +
  `Window<0>` + no approval gate, for a scratch/prototype session) is worth naming as a *second*,
  explicitly-labeled preset distinct from the production default in §2a/§2b — flagged, not designed,
  since a mislabeled "dev" preset that quietly becomes someone's production config is a real, seen-
  before failure mode this draft would rather not invent casually.
- Interaction with the `Native*Provider` family (ADR-071, Judged): should `QuickstartSessionBuilder`
  gain `.with_native_shell(...)`/`.with_native_python(...)` sugar over `cap::NativeExec` +
  `NativeCapabilityAnnouncer<Ps...>`? Deferred — that family's own compile-time family-distinctness
  guarantee (ADR-071 §5g) needs its `Ps...` pack fixed at the builder's own template-instantiation
  point, which interacts with this builder's currently-runtime, fluent-chain shape in a way this draft
  has not worked out yet.

## 7. What this draft is not

Not an ADR. §2a/§2b/§2c/§2d/§3(as corrected in §0)/§4 are now real, tested code. §2a/§2c/§2d/§3/§4 are
red-teamed three times (§0b, §0c, §0d); §2b (§0e's 4th pass) has been red-teamed THREE times (§0f
round 4, §0g round 5, §0h round 6) — every finding from all six rounds is closed. The fallback/
middleware/content-replay/raw-client-only surface remains design-only, unimplemented. This facade's
own risk profile is low but NOT zero-new-authority in every respect, a claim an earlier version of
this section made too broadly: §0f's finding 9 and §0g's finding 11 (both `LazyComposedContextProvider`
state silently crossing/vanishing across session identities via `AgentSession::fork_from()`/
`history_provider()`) are real, I1/I4-adjacent session-isolation gaps, not merely "ordinary C++ hygiene
bugs" the way every finding across §0/§0b/§0c/§0d was — neither touches I2/I3's capability/authority
MECHANISMS directly (nothing gets approved or granted that shouldn't), but both let one session's
identity observe or silently lose another's mutable state, the class of thing I4 (every effect is
attributable) cares about. §0g in particular was the sharper lesson: finding 9's OWN fix (move-only)
was itself red-teamed and found insufficient on its first re-examination — a real instance of this
project's own repeated pattern (finding 3 → 4, finding 5 → 7) applying to a SECURITY-ADJACENT fix, not
just an ergonomics one. §0h then re-examined finding 11 under DELIBERATELY harsher probing than that
pattern would have predicted was needed, and found it held — this project's first clean red-team round
since round 1, a genuine (if narrow) data point that the fix-then-verify cycle converges rather than
finding something new indefinitely. Fixed both real findings by narrowing exposure (deleting/correcting
a capability the type had, adding none) rather than granting anything, so this continues to follow the
lighter `design → prototype → red-team → fix` cycle, not the full `design → red-team → prove → judge`
gauntlet ADR-070/071-class changes require — but §0f/§0g are why this section no longer claims "no new
capability semantics, no new authority path" as a blanket fact about this whole file. Finding 11
(round 5's fix) still stands ready for its own next, independent red-team round; finding 7 has now
survived two rounds' worth of fresh-eyes scrutiny (rounds 5 and 6) without its own dedicated round the
way findings 9/11 got, a meaningfully different disclosure posture than "never examined" but not the
same as "independently red-teamed." If a later pass surfaces a finding that touches I2/I3's own
mechanisms directly, that finding gets its own escalation at that point, matching this project's
established practice — not decided in advance here.
