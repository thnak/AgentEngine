# ADR-158 — Tool concurrency exclusivity as a policy dimension

**Status:** Proposed — design corrected by one independent red-team pass (§3), implemented, and
proven (real code + tests, §7). **Awaiting project-owner judgment** — per this project's `design →
red-team → prove → judge` discipline (CLAUDE.md), this ADR is not "Judged" until the project owner
signs off; recorded here in Proposed state so the evidence trail is complete and reviewable before
that sign-off. What's implemented is the *declarative surface only* — `ExclusivityGroup<Name>`, its
two compile-time guardrails, and `ToolDescriptor::exclusivity_group` — matching `Parallelizable`'s
own existing shape (a real, tested declaration with **zero pipeline logic yet**, since 006 §8 G4's
parallel-batch scheduler remains deferred). A second, scheduler-time red-team/implementation pass is
still needed once G4 itself is built, for the open questions §5 does not resolve (determinism proof,
remote/MCP defaults, non-transitivity). **NOTE:** drafted and implemented locally as "ADR-157"; renumbered
to ADR-158 after `git push` was rejected and a `git fetch`/rebase found `origin/main` had, in the
meantime, independently claimed ADR-157 for a concurrent, unrelated change
(`decisions/ADR-157-sub-workflow-nested-request-port.md`, issues #33/#38) — the same
multiple-sessions-racing-on-the-next-free-ADR-number pattern this repo's own ADR-149/150/151 NOTEs
already document happening repeatedly. Every in-file self-reference (this file, `core/tool.hpp`,
`core/tool_pipeline.hpp`, `tests/CMakeLists.txt`, the new test and compile_fail files) was updated to
match; GitHub issue #41's own title/body still say "ADR-157" (issues aren't renumbered the way files
are — see the issue's own follow-up comment noting the change).

**Relates to:** GitHub issue #41 (files this exact gap, filed after this draft's first version —
see §0). `006-Tool-and-Function-Plane.md` §5 (Concurrency — the binary "batch is parallel only
when every call is `Parallelizable`" rule) and §8 G4 (parallel-batch determinism, itself still deferred
— `include/agentengine/core/tool.hpp`'s own comment on `Parallelizable`: *"M2 proves a single native
tool call, never a parallel batch... this carries no pipeline logic yet"*). `include/agentengine/core/
tool_pipeline.hpp` (`PolicyDecider`, `policy_decision`, `ToolCallRequest`). `decisions/ADR-070-host-
configurable-responsibility-boundary.md` (the Delegated Decision Seam `PolicyDecider` belongs to, and
its five required properties any new seam here must also satisfy — property 4 in particular, §3 below).
`decisions/ADR-151-delegated-agent-approval-policy.md` (the only existing `PolicyDecider` reference
implementation). `007-Capability-and-Trust-Model.md` §3 (`Background<max_concurrent>`, the one
concurrency ceiling that already exists). `CONVENTIONS.md`'s policy-idiom rule ("no RTTI, no
reflection, no `virtual` for policy on the hot path... type erasure is permitted only at declared
seams... never inside a turn's hot loop") — binding on §4's design below. `include/agentengine/trust/capability.hpp`
(`cap::decl::ToolCall<agentengine::fixed_string Name>` and siblings — the real precedent §4 now follows,
found by the red-team, not by this draft's first version). `include/agentengine/core/fixed_string.hpp`.

## 0. Where this question comes from

Originally sourced from a 2026-08-31 comparative review of Microsoft Agent
Framework's (MAF, `github.com/microsoft/agent-framework`) last two weeks of activity: MAF issue #7914
("design per-tool concurrency controls"), Python PR #7881 (`concurrency_group` on `FunctionTool`,
**still open, deliberately incomplete**), and .NET PR #7650 (`AllowConcurrentInvocation`, a simple
boolean, merged). MAF's own findings, taken at face value:

- **Python defaulted to unconditional concurrency.** Every tool call in a batch ran concurrently with
  no way for a tool author to force serialization, producing races on stateful tools (a write followed
  by a dependent read landing out of order) — issue #7386, the motivating report for PR #7881.
- **.NET defaulted to unconditional sequencing.** `FunctionInvokingChatClient` ran every batch fully
  serially even when the model emitted calls that were provably independent and safe to parallelize —
  issue #7640, the motivating report for PR #7650.
- **The richer fix (`concurrency_group`, mutual exclusion between specific tools) was explicitly
  deferred**, not because it was rejected, but because issue #7914 found it needed real cross-cutting
  design work first: semantics for MCP-hosted tools the framework doesn't control the declaration of,
  cancellation/fail-closed ordering guarantees, and cross-run/session scoping. Only the simple boolean
  opt-in shipped; the exclusivity-group mechanism did not.

AgentEngine has not yet built its parallel-batch scheduler at all (006 §8 G4, still deferred), which is
a better time to decide this policy surface than after, per MAF's own experience of shipping
unconditional concurrency first and finding the race class in production. This reasoning is sound on
its own terms; §3 below narrows one place it was overstated (MAF's PRODUCTION incident is not the same
shape of evidence as AgentEngine's pre-implementation design question).

Filed as **GitHub issue #41** (same repository), which restates this gap independently against the
current source and cites this draft as its own suggested-scope pointer.

## 1. The question, stated so it has a wrong answer

**Does AgentEngine need a declared, per-tool concurrency-exclusivity dimension — "this tool must never
execute concurrently with [another specific tool / another call to itself]," enforced even inside an
otherwise-`Parallelizable` batch — before 006 §8 G4's parallel-batch scheduler is implemented? Or is
the current binary rule (006 §5: a batch is parallel only when *every* call in it is `Parallelizable`;
otherwise the tool author is expected to make concurrent execution safe internally, e.g. with its own
lock) sufficient, leaving exclusivity as an implementation detail inside individual tools rather than a
first-class engine concept?**

## 2. What already exists today (verified against the current source; re-confirmed by an
independent red-team pass reading the same files directly, §3)

- `Parallelizable` (`core/tool.hpp`) is a declared policy tag with **zero pipeline logic** right now —
  its own comment states plainly that 006 §8 G4 (parallel batch execution) is deferred, so nothing in
  the pipeline reads this tag to actually run calls concurrently yet. This ADR is scoped ahead of that
  gate landing, not against a shipped scheduler.
- 006 §5's rule is **whole-batch, all-or-nothing, and unconditional**: `Parallelizable` is "a claim
  about external effects — that concurrent execution **with other tools** is observably equivalent to
  some sequential order — and declaring it on a state-mutating tool is a defect, not a tuning choice."
  The claim has no carve-out clause — it is a promise of safety with *everyone*, not with everyone
  outside some named exception set. There is no existing vocabulary for "safe to run alongside most
  tools, but never alongside tool X specifically" — a tool author facing that shape today has exactly
  one honest option: declare it non-`Parallelizable`, forcing the *entire* batch to serialize even when
  only one other call in it actually conflicts.
- `PolicyDecider` (`core/tool_pipeline.hpp`, ADR-070) is `std::function<policy_decision(Principal
  const&, ToolDescriptor const&, bool arguments_tainted)>` — evaluated **once per call**, with no
  parameter carrying what other calls are concurrently in-flight in the same batch or turn, and no
  parameter carrying a call's actual (possibly tainted) `arguments` — only the derived taint flag. It
  structurally cannot express "deny/defer this call because another in-flight call conflicts with it".
- `Background<max_concurrent>` (007 §3) is a single scalar cap on how many **backgrounded/detached**
  tasks one session may have in flight at once — a structurally separate mechanism from an ordinary,
  synchronous, in-turn tool-call batch. A `Backgroundable` call is only ever reachable through the
  explicit `background_task(tool, args)` wrapper, never as a member of a model-emitted batch's parallel
  dispatch; the two mechanisms operate over disjoint call shapes today, and this ADR's own scope (§4)
  is written to keep it that way rather than inventing an interaction rule between them.
- No pre-existing tool-pair exclusivity/lock mechanism exists anywhere in `core/tool*.hpp`,
  `rt/agent_session.hpp`, or the workflow layer (confirmed by the red-team via a direct grep of
  `include/agentengine/core` and `include/agentengine/rt`); the only other uses of the word
  "exclusivity" in the RFC set (001 §4, 005 §26) refer to I1's unrelated session-level `AsyncMutex`
  single-executor guarantee, not to tool-pair concurrency.

## 3. A correction to the design draft, from an independent red-team pass

An independent, adversarial review (a fresh session, no prior context, briefed only with CLAUDE.md, the
GitHub issue, and this draft's first version) was run before any implementation code was written, per
CLAUDE.md's `design → red-team → prove → judge` discipline. It found three MUST-FIX defects and four
SHOULD-FIX weaknesses in the first draft, all incorporated into §4-§5 below.

**MUST-FIX 1 — the first draft's Option A silently redefined `Parallelizable`'s meaning while claiming
it did not.** The first draft proposed a tool declare **both** `Parallelizable` and a new
`ExclusivityGroup<Tag>` together, with the group "partitioning an *otherwise-`Parallelizable`* batch."
But `Parallelizable` (006 §5) is defined as an *unconditional* claim — safe alongside *every* other
tool, no exceptions — so a tool that also carries a group tag would, under that phrasing, be making two
contradictory claims about itself at once: unconditionally safe (`Parallelizable`) and safe-except-
within-its-group (`ExclusivityGroup`). The first draft's own §5 then claimed *"`Parallelizable`'s
existing binary semantics are unmodified by this draft"* — false for any tool that also declares a
group. **Resolution (§4 below):** `ExclusivityGroup<Name>` is redesigned to be a **distinct, alternative
concurrency claim**, not an addition layered on top of `Parallelizable`. A tool declares one or the
other, never both — `ExclusivityGroup<Name>` *is* a (weaker, group-scoped) parallelizability claim in
its own right, and 006 §5 needs an honest amendment (a second, named way to opt into a parallel batch),
not a claim of zero change.

**MUST-FIX 2 — a direct, in-document self-contradiction about scope with `Background<max_concurrent>`.**
The first draft's §2 said the composition question with `Background<max_concurrent>` was answered
"explicitly, in §5"; its own §4 said the same question "takes no position yet"; §5 contained no such
content either way. **Resolution:** actually resolved, not merely reworded — see §2's last bullet above
and §4 below: `ExclusivityGroup` and `Background<max_concurrent>` govern disjoint call shapes (an
ordinary in-batch call vs. an explicitly detached background task), so no interaction rule is needed;
this is now stated once, in one place, consistently.

**MUST-FIX 3 — the proposed mechanism, as literally sketched, would have violated this codebase's
binding no-RTTI convention.** `Approval<M>`/`EffectClass<C>` (the idiom the first draft cited as
precedent) are NTTPs over a closed, engine-defined `enum` — the enum value itself is the runtime value,
needing no cross-tool identity comparison. `ExclusivityGroup<Tag>` needs the opposite: two independently
compiled tools must be able to assert "we mean the same group," which requires comparing an arbitrary
`Tag` type for identity at runtime once erased onto a type-erased `ToolDescriptor`. The obvious
implementation (`std::type_index(typeid(Tag))`) is RTTI, and `CONVENTIONS.md` states plainly: "no RTTI,
no reflection... for policy on the hot path... type erasure is permitted only at declared seams... never
inside a turn's hot loop" — a per-turn batch-partition step is squarely inside that scope. **Resolution
(§4 below):** re-specified as `ExclusivityGroup<agentengine::fixed_string Name>`, matching the REAL,
already-shipped precedent for exactly this shape — `trust/capability.hpp`'s `cap::decl::ToolCall<
agentengine::fixed_string Name>` and siblings — a structural-type NTTP resolved to a plain
`std::string`/`std::string_view` at `make_tool_descriptor<ToolT>()` time (i.e. at startup, matching
CONVENTIONS.md's own "resolved to metadata at startup" requirement), never at dispatch, never via RTTI.

**SHOULD-FIX, also incorporated:**
- A *named group* is an equivalence class, not a pairwise relation — the original problem statement
  (§1: "must never run concurrently with *another specific tool*") is pairwise, and a single group tag
  per tool cannot express a non-transitive conflict pattern (A excludes B, B excludes C, A and C are
  mutually fine) except by over-serializing A and C together. Named as an explicit limitation, §5.
- The existing policy-tag fold idiom (`declared_approval()` et al.) silently keeps only the
  *last*-declared value if a tool mistakenly declares the same kind of tag twice. For `approval_mode`
  this is low-stakes; for a concurrency-safety guarantee it would silently narrow protection with no
  diagnostic. Named as an open question needing an explicit (not inherited-by-default) answer, §5.
- Option B's rejection is sharper than "cross-call state it has never carried": `ToolCallRequest`
  (`tool_pipeline.hpp`) carries a call's actual, possibly-tainted `arguments` — ADR-070's own property 4
  keeps tainted content out of the `PolicyDecider` seam entirely (only the derived `arguments_tainted`
  flag crosses it today). A `std::span<ToolCallRequest const>` parameter would, for the first time, hand
  a decider other calls' real model-authored argument content, not merely a taint flag — a concrete I3-
  adjacent objection, not just a structural-size one. Narrowed and restated in §4.
- ADR-151's `approve_delegated_calls()` reads only `Principal const& caller` — it never reads
  `ToolDescriptor` or `arguments_tainted` at all, so a trailing, ignored parameter on `PolicyDecider`
  would be a mechanical, not semantic, coupling. The first draft's "depends on this staying pure" framing
  overstated the actual risk to that specific reference policy; corrected in §4.
- MAF's cited harm (issue #7386) was a **shipped, production** race from an already-live scheduler;
  AgentEngine has no live scheduler yet, so Option C's rejection should rest on the (sufficient, already-
  present) design-debt argument — avoiding a breaking change to `Parallelizable` once G4 lands — not on
  a borrowed safety-incident framing that doesn't reproduce here. Corrected in §4.

The red-team also independently confirmed, by reading the source directly rather than trusting this
draft's quotes: 006 §5's and 001 §4's rules are quoted accurately; `Parallelizable`'s "no pipeline logic
yet" state and G4's deferred status are accurate; `PolicyDecider`'s signature and per-call-only scope are
accurate; ADR-070's five Delegated Decision Seam properties are summarized accurately; `Background<
max_concurrent>` is genuinely a different mechanism, not the same concept under another name; and no
pre-existing exclusivity/lock mechanism already solves this anywhere in the tree.

## 4. Design sketch, corrected (for a second, implementation-time red-team pass to attack — still not
a decision)

**`ExclusivityGroup<agentengine::fixed_string Name>` — a distinct, alternative concurrency claim,
not layered on top of `Parallelizable`.** A tool declares *either* bare `Parallelizable` (unconditional,
006 §5's existing, unmodified meaning — safe alongside literally everything) *or*
`ExclusivityGroup<Name>` (a new, narrower, group-scoped claim — safe alongside any call outside group
`Name`, but must serialize with any other in-flight call also declaring `Name`), never both. This is a
genuine amendment to 006 §5, honestly stated as one (not "no change"): a second, named way for a tool to
opt into a parallel batch, alongside the existing unconditional one. `Name` is an `agentengine::
fixed_string` NTTP (matching `cap::decl::ToolCall<Name>`'s own precedent), resolved to a plain string at
`make_tool_descriptor<ToolT>()` time and stored on `ToolDescriptor` as ordinary runtime data — no RTTI,
no `typeid`, no type erasure inside the dispatch/batch-partition step itself. The (future) batch-
formation step partitions an admitted batch into concurrency classes: each distinct `Name` is one class
(its members serialize with each other, in the model's emitted order, reusing 006 §5's existing
result-append determinism rule so G4's own claim is not weakened); each ungrouped `Parallelizable` call
is its own singleton class; different classes run concurrently.

**Scope, resolved:** `ExclusivityGroup` governs concurrency only among calls dispatched together within
one model-emitted batch (an ordinary, synchronous, in-turn concept). `Background<max_concurrent>`
governs a structurally separate mechanism — explicitly detached, `Backgroundable`-only calls reached
through `background_task(...)`, never a member of a batch's parallel dispatch. The two mechanisms
operate over disjoint call shapes and need no composition rule; this ADR does not propose one because
none is needed, not because the question was skipped.

**Option B — widen `PolicyDecider` to see the in-flight batch — still rejected, on a sharper ground.**
Adding a parameter such as `std::span<ToolCallRequest const> concurrent_calls` would, for the first
time, hand a host decider other in-flight calls' actual (possibly model-derived, possibly tainted)
`arguments` — not merely a taint flag — directly conflicting with ADR-070 property 4's deliberate design
that tainted content never occupies this decision seam. (A narrower variant passing only
`ToolDescriptor`s of concurrently in-flight calls, with no `arguments`, would not trip this specific
objection — noted here as a distinct, unexplored variant, not analyzed further in this draft.) Rejected
for §1's problem shape regardless: whether two tools' effects can safely overlap is a property of what
the tools *do*, which their own author is positioned to declare — the same author-declares-it idiom
`Approval<M>`/`EffectClass<C>`/`Backgroundable` already use — not a per-call host policy decision.

**Option C — defer until G4 lands, decide then — rejected on the design-debt ground alone.** Deciding
this policy surface now, before G4's scheduler exists to enforce it, avoids designing that scheduler
against `Parallelizable`'s current purely-binary semantics and then needing a breaking change once
exclusivity is added later. (MAF's own issue #7386 was a production incident from an *already-shipped*
scheduler — evidence that unconditional concurrency is a real hazard once built, not direct evidence
about AgentEngine's own pre-implementation sequencing choice; the design-debt argument stands on its
own without leaning on that incident's framing.)

## 5. Open questions — one resolved by this implementation, three still deferred to G4 itself

- **Multi-declaration fail-closed behavior — RESOLVED (§7).** A tool declaring more than one
  `ExclusivityGroup<Name>`, or both `Parallelizable` and a group, now fails to compile
  (`static_assert`s in `Tool<Derived, Policies...>`, `core/tool.hpp`) rather than silently keeping only
  the last-declared value the way `declared_approval()`'s ordinary fold does — matching this project's
  "reject outright, never silently narrow" precedent. Proven via two `tests/compile_fail/*.cpp` pairs
  (§7), not merely asserted.
- **Non-transitivity — still open, not addressed by this implementation.** A named group is an equivalence class; it cannot express a genuinely pairwise,
  non-transitive conflict (A excludes B, B excludes C, A and C are mutually fine) without over-
  serializing A and C together. Is this an acceptable simplification for v1 (name it as a residual), or
  does the real-world need justify a pairwise mechanism (a materially more complex design, not sketched
  here)?
- **Determinism under I5 — still open, not addressed by this implementation.** Serializing calls within
  one group must still produce the *same* replay-stable history order 006 §5/G4 already require for
  parallel batches — unprovable today since no batch-formation/scheduling code exists yet (G4 itself is
  still deferred); this implementation only proves the declarative surface (§7), not scheduling
  behavior. Needs its own test once G4 is built, not asserted by analogy to the existing rule.
- **Remote/MCP-hosted tools — still open, not addressed by this implementation.** Can a tool this engine
  does not itself define (`tool_source::mcp_server`,
  `tool_source::remote_agent`) declare an `ExclusivityGroup` at all, or is every such tool treated as its
  own ungrouped singleton by default? Whichever answer is chosen must be a stated fail-closed/fail-open
  trade-off, not an unexamined default inherited from how `ToolDescriptor` happens to be constructed for
  that source today.

## 7. Evidence

**`include/agentengine/core/tool.hpp`** — `ExclusivityGroup<agentengine::fixed_string Name>` (a new
declared policy tag, same file, right after `Parallelizable`), `tool_detail::policy_is_parallelizable
<Policy>` and `tool_detail::policy_exclusivity_group<Policy>` (compile-time detection traits),
`Tool<Derived, Policies...>::declared_exclusivity_group()` (the compile-time accessor, same idiom as
`declared_backgroundable()`), and two `static constexpr`/`static_assert` guardrails enforcing MUST-FIX
1 and §5's multi-declaration resolution. Both guardrails are written as immediately-invoked lambdas
over a comma-operator fold (matching this struct's own pre-existing idiom for every other accessor),
**not** a raw arithmetic fold (`(static_cast<int>(pack_expr) + ...)`) — that form was tried first and
found, empirically, to make MSVC (Visual Studio 18) reject it with C3520 ("parameter pack must be
expanded in this context") for the real, pre-existing, EMPTY-`Policies...` instantiation
`Tool<agentengine::rt::ScheduleWakeupTool>` (`include/agentengine/rt/agent_session.hpp`) — a genuine
build break this ADR's own full-rebuild step (below) caught, not a hypothetical.

**`include/agentengine/core/tool_pipeline.hpp`** — `ToolDescriptor::exclusivity_group` (`std::optional
<std::string>`, appended last, this struct's own established convention), populated by both
`make_tool_descriptor<ToolT>()` and `make_tool_descriptor_with_invoke<ToolT>()` from `ToolT::
declared_exclusivity_group()`. Every hand-built `ToolDescriptor` that predates this field (memory_
provider.hpp, vector_rag_context_provider.hpp, mcp_tool_bridge.hpp, native_providers.hpp — the same
list `effect_class`'s own comment already names) fails closed to `std::nullopt` automatically
(`std::optional`'s own default construction), needing no changes at any of those sites.

**`tests/test_tool_exclusivity_group.cpp`** (new, 10 checks) proves the declarative surface end to
end, through the REAL `Tool<Derived,Policies...>` and `make_tool_descriptor<ToolT>()` — not a
synthetic stand-in: an undeclared tool and a bare-`Parallelizable` tool both report no exclusivity
group (the MUST-FIX 1 regression check: `Parallelizable` must never populate it); a tool declaring
`ExclusivityGroup<"db-write">` reports that exact name, on both the compile-time accessor and the
runtime `ToolDescriptor`; a SECOND, independently-declared tool type sharing the same group name
reports an identical group value — the real point of the `fixed_string` NTTP choice (MUST-FIX 3):
correlation by declared name across unrelated compiled types, not by C++ type identity; and a
distinct group name never collides with another.

**`tests/compile_fail/tool_rejects_parallelizable_and_exclusivity_group.cpp`** and **`tests/
compile_fail/tool_rejects_duplicate_exclusivity_group.cpp`** (new) — both proven, via `tests/
CMakeLists.txt`'s `try_compile()` idiom (the same mechanism `tainted_no_implicit_conversion.cpp`/
`sandbox_profile_rejects_non_conforming_type.cpp`/etc. already use), to **fail to compile**: a tool
declaring both `Parallelizable` and `ExclusivityGroup<Name>` (MUST-FIX 1), and a tool declaring two
different `ExclusivityGroup<Name>`s (§5's multi-declaration resolution). No separate positive-control
compile_fail file was added — following the ADR-102 §3 C1 gate's own precedent (its own comment: "the
main runtime test... serves as this pair's own positive control") — since `test_tool_exclusivity_
group.cpp` already compiles and passes with bare `Parallelizable` alone, `ExclusivityGroup<Name>`
alone, and two independent tools sharing one group name.

**Built and run for real (Debug/MSVC, Visual Studio 18, `cmake --build build --config Debug`)**: zero
compile errors, zero new warnings. CMake reconfigure's own `try_compile()` gates print `ADR-158
compile-fail proof: OK` alongside every pre-existing compile-fail gate (007 §9 G1/G2, ADR-012,
ADR-096 C2, ADR-102 §3 C1, ADR-071, 014 §1, Milestone 3 A3) — none regressed. Full repo-wide `ctest -C
Debug`: **253/253 tests passed, 0 failed** (2 pre-existing, unrelated Windows-only skips: `test_shell_
runner_no_process_creation`, `test_mediated_shell_runner_no_process_creation`), including the new
`test_tool_exclusivity_group` and the pre-existing `test_tool_pipeline` (unchanged, zero regression).
`python tools/naming_lint.py`: clean — `ExclusivityGroup` carries its own `ae-naming-lint: allow`
comment (same convention as `Parallelizable`/`Backgroundable`); the 4 pre-existing, unrelated
naming-lint findings (`WorkflowCheckpointManager`, `Transcript`, `MagenticWorkflowBuilder`,
`multiplex_sink`) are untouched by this change.

A follow-up independent code-review pass (fresh session, no prior context) re-derived every claim
above directly from the on-disk code and re-ran the full configure/build/`ctest`/`naming_lint.py`
sequence itself from scratch — confirmed all of it exactly, including the two `static_assert`s'
lambda-fold shape, `ScheduleWakeupTool` as the real empty-`Policies...` instantiation that first
caught the MSVC C3520 defect, the 10-check test file's exact scope, both compile_fail files' intent,
the 253/253-passed/0-failed/2-skipped `ctest` result, the unchanged 4-item `naming_lint.py` baseline,
and that all four named hand-built `ToolDescriptor` sites (plus no others) default `exclusivity_group`
to `nullopt` untouched. Zero discrepancies found.

## 8. What this ADR does not claim

- **Does not build 006 §8 G4's parallel-batch scheduler.** Nothing in this ADR reads `ToolDescriptor::
  exclusivity_group` at dispatch time — like `Parallelizable` itself, it is a real, tested, but
  currently-inert declaration. `ToolTable`/`invoke_tool()` are unchanged; a batch of calls is still
  dispatched exactly as before this ADR.
- **Does not resolve determinism (I5), remote/MCP-tool defaults, or the non-transitivity limitation**
  named in §5 — all three need G4's actual scheduler to exist before they can be tested for real, and
  are explicitly left for that follow-on work, not silently assumed solved here.
- **Amends, rather than merely adds to, 006 §5's text.** Per MUST-FIX 1's resolution,
  `ExclusivityGroup<Name>` is a second, honestly-named way to opt into a parallel batch, alongside the
  existing unconditional `Parallelizable` claim — 006 §5's prose itself has not yet been edited to say
  so; doing that is copy-editing the RFC to match this ADR, not a further design decision, and is named
  here as a small remaining paperwork step rather than performed silently.
- **Does not touch `PolicyDecider`'s existing signature.** Option B (widening it to see the in-flight
  batch) remains rejected, on the ADR-070-property-4 ground §3/§4 state; no code in `tool_pipeline.hpp`
  besides the new `ToolDescriptor` field changed.
- **Does not change any existing tool's behavior.** Every tool in this codebase that predates this ADR
  declares neither `Parallelizable` nor `ExclusivityGroup<Name>`, or declares bare `Parallelizable`
  alone (both compile unchanged, confirmed by the full, zero-regression rebuild in §7) — the two new
  static_asserts only ever fire for a tool that newly declares an invalid combination, which no
  existing tool in this tree does.
- **Originates from external comparative research, not an internally-noticed gap** — unlike most of
  this repository's other ADRs, the initial prompt was a cross-project review (MAF), not direct code
  reading that independently found the same question; GitHub issue #41 grounds it against this
  codebase's own source directly, same as any other design-stage issue here.
