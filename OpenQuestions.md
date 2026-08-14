# Open Questions

Cross-cutting questions that no single RFC owns, or that block promotion of several. Per-RFC
questions stay in their RFC's §Open questions; this file is for the ones that would change the
shape of the project.

**Legend:** 🔴 blocks a v1 decision · 🟠 needed before implementation of its area · 🟡 can wait

**Three open cross-cutting questions as of 2026-08-14: OQ-19, OQ-20, OQ-21.** OQ-1 through OQ-18 are all
resolved. New questions are added here as they're identified; per-RFC open questions that don't change
the shape of the project stay in their own RFC's §Open questions and are never promoted here by
default.

---

## Open

### OQ-19 — Where does an agent-executor's `CapabilitySet` come from? 🟠

`executor_kind::agent` (014 §3/§7) is real at the graph-declaration layer but structurally refused at
execution (`check_workflow_executable()`, `workflow/graph.hpp:431`) — no runtime bridge wraps a real
`AgentSession` as a workflow node yet. Full gap analysis, current-state citations, and a source-grounded
study of how MAF (.NET) built the equivalent bridge:
`docs/planning/agent-as-workflow-executor-gap.md`, `docs/research/2026-08-13-maf-agent-as-workflow-executor.md`.
A design draft exists and has been red-teamed twice:
`docs/planning/agent-as-workflow-executor-design-draft.md`. First pass: the original "no core-seam
change needed" claim was FALSE against real code (a genuine checkpoint/resume amnesia bug and a
genuine concurrent double-resume race, both FATAL as originally scoped). Second pass (2026-08-13),
resolving the punch list: capability sourcing reuses `WorkflowSupervisor::initialize()`'s already-
present but unused `contexts` parameter (no new API needed for the grant itself, only a new
`check_workflow_executable()` overload to verify it); the concurrent-hazard fix quarantines only the
specific hazardous delivery through the EXISTING failure-policy channel, not a whole-round abort (the
first attempt at this resolution was itself found too harsh); and a proposed `TaggedExecutorBody`
wrapper was FATAL as scoped (breaks every real call site) — fixed via `std::function::target<T>()`,
which has zero precedent anywhere in this codebase and needs its own positive-control test before
being trusted. Read the design draft before implementing.

MAF's own design gives no precedent for this question — it has no in-process capability/authority
system analogous to `CapabilitySet`/`EffectContext::capabilities`. A future ADR must decide: does an
agent-executor's capability ceiling come from the workflow's own declaration (analogous to a tool's
`capability_ceiling`), a per-executor binding-time grant, or something derived from the invoking run's
principal? Blocks 014 §3's `agent`-kind executor and, transitively, honest "multi-agent orchestration"
(as opposed to multi-*function*-node orchestration with a model call inside one function, which is all
that's demonstrated today, per the gap doc's `examples/16_group_chat_live.cpp` citation). **Explicit
project-owner direction (2026-08-13): document only, do not implement yet.**

### OQ-20 — Coalescing concurrent agents onto one vendor batch inference call 🟠

User proposal (2026-08-13): since providers support batch inference, let concurrently-running agents
share one batch submission to save cost. 004 §8 Q1 already resolved that `batch` rides
`Backgroundable`/`StandingEffect` (not a bespoke structure) — that prerequisite shipped for TOOL calls
in M7 Phase B but was never extended to model calls. Full gap analysis, vendor-API research (OpenAI/
Anthropic batch mechanics, fetched and cited), and six concrete open design questions (batch-eligibility
granularity, coalescing-coordinator ownership/flush trigger, N-way result fan-out onto the existing
completion-queue idiom, `custom_id`/principal attribution and a named cross-tenant metadata-leak
question, poll-ownership/durability, and a caller-visible cost-vs-latency policy surface):
`docs/planning/batch-inference-coalescing-gap.md`, `docs/research/2026-08-13-vendor-batch-inference-apis.md`.
**Resolved, red-teamed once:** `docs/planning/batch-inference-coalescing-design-draft.md` — reuses
`request_port`/`Interaction` (durable) instead of `StandingEffect` (non-durable today) for the
suspend/resume shape, needs no new message type (`resume_workflow()` already resolves N ports one at
a time), and surfaced a real, independent gap worth fixing on its own regardless of batch's fate:
**`WorkflowSupervisor::resume_workflow()` has no caller/admission check at all** — unlike
`AgentSession::resolve_interaction()`'s own `principal_admitted_for()` check (ADR-029). Five remaining
punch-list items, none implemented.

**Load-bearing finding, confirmed from real vendor docs, not assumed:** batch mode is structurally
single-shot — neither OpenAI's nor Anthropic's batch API lets a batched request see a tool result and
continue the same turn. A multi-round `AgentSession` tool-calling loop would need each round submitted
as its own batch job, multiplying a turn's latency by however many rounds it takes (often <1h to 24h
PER round). This does not block the idea — it fits N independent single-shot calls (e.g. workflow
fan-out nodes) well — but it means "batch mode" cannot be a blanket accelerator for arbitrary agents.
**Explicit project-owner direction (2026-08-13): document only, do not implement yet.**

### OQ-21 — External process hooks (Claude-Code-style lifecycle interception) 🟠

User observation (2026-08-14): AgentEngine has no equivalent of Claude Code's Hooks feature, and MAF
(this project's developer-model prior art) has no such concept either. Confirmed, not assumed: real
vendor docs for both Claude Code's Hooks and the OpenAI Agents SDK's `RunHooks`/`AgentHooks` were
fetched and cited — `docs/research/2026-08-14-claude-code-hooks-mechanics.md`. **Load-bearing finding:
"Hooks" names two architecturally unrelated things across the industry.** The OpenAI Agents SDK's
hooks are in-process, observation-only async callbacks — the same shape as 002 §5's own declared
`Middleware<Ms...>`, which is already a strict superset (deny-capable at its one wired point, the
model-call interception, ADR-033) of what OpenAI ships. Claude Code's Hooks are out-of-process,
config-driven, deny/rewrite-capable external commands — and its own docs state plainly that a hook
"runs with the full permissions of the user running Claude Code," unsandboxed. That execution model is
exactly what **I2** and `009-Plugin-and-Extension-System.md` §5 (which names `subprocess` explicitly as
unavailable to any plugin world) already rule out — the real gap is the event taxonomy and the
allow/deny/rewrite decision shape, not Claude Code's ambient-authority process-exec model.

Full gap analysis grounded in real code citations (`middleware.hpp`, `tool_pipeline.hpp`'s
`ApprovalDecider`, `agent_session.hpp`'s `run_rounds()`, `run_event.hpp`'s already-real observation
stream): `docs/planning/external-process-hooks-gap.md`. **Resolved, red-teamed once:**
`docs/planning/external-process-hooks-design-draft.md` — scopes narrowly to the tool-call point (the
one clean choke point `invoke_tool()` already provides) by extending the already-proven
`ApprovalDecider` seam rather than adding a parallel Middleware chain; routes any hook that reaches an
external process through the EXISTING suspend/resume `Interaction` mechanism (ADR-029, a new
`interaction_reason::hook_decision`) instead of an inline blocking call; and leaves `RunEvent` untouched
as the complete, already-real answer for every observational (non-gating) hook — 16 of Claude Code's 24
named events are pure observation and need zero core change to serve externally today. Red-team found
two fatal problems in the first-pass draft, both closed in the resolved version: (1) `start_run()` holds
`session_mutex_` for the whole run with no timeout, so an inline-`co_await`ed external-dispatch hook
would stall the entire session indefinitely — closed by the suspend/resume correction; (2) no concrete
insertion point existed for a provenance-downgrade guard on a hook-rewritten tool call, reopening the
exact confused-deputy hole ADR-023/ADR-033 already closed once — closed by
`enforce_hook_rewritten_tool_call_provenance()`, diffing the request's arguments before/after the hook
runs. Four must-fix gaps also closed (chain-runner mismatch against `run_rounds()`'s real seven-exit-path
shape, so run/turn-level *gating* hooks are scoped out as separate future work; two-independent-gates
ambiguity between `ApprovalDecider` and a naive new hook pair, closed by sequencing the hook stage
strictly before `ApprovalDecider` rather than running them independently; the observation-vocabulary-vs-
trigger-mechanism conflation; and the `RunContext`/`TurnContext` field list, narrowed away entirely once
scope shrank to tool-call only). **Second pass (2026-08-14), run/turn-level internal (in-process)
hooks:** an RAII `TurnBoundaryGuard`/`RunBoundaryGuard` mechanism was proposed to close Q1's own named
gap (only 1 of `run_rounds()`'s 7 exit paths emits a matching `turn_finished`) without restructuring the
loop — the firing mechanism itself is real and precedented (`AsyncMutex::Guard` already does exactly
this to release `session_mutex_` across all 7 exits). Red-team found two of the capabilities proposed on
top of it **structurally impossible**, not just hard: a destructor cannot be a coroutine (C++20 forbids
it), so an async/external-dispatching hook cannot run from one; and `co_return` finalizes a coroutine's
result before any local's destructor runs, so an "`after_turn` hook overrides the outcome and forces
another round" capability (the Claude-Code-`Stop`-hook analog) cannot be delivered from a destructor
either — both would need explicit calls placed before each `co_return`, the exact restructuring this
approach tried to avoid. **Resolved narrower**: the guard survives scoped to synchronous, non-throwing,
non-overriding, observation-only `after_turn`/`after_run`, closing the 5-of-7-exits-have-no-closing-
event gap for real. `before_run`/`before_turn` **denial** (ordinary inline synchronous calls, unaffected
by the destructor findings) stays available, confirmed clean. Gating/override power and any
external-dispatching hook at run/turn level remain explicitly unresolved, now for a sharper, correctly-
identified reason. Six remaining punch-list items, none implemented — real, named follow-on work, not
silently dropped. **No project-owner implementation direction yet — this is a fresh design draft, not a
standing "document only" instruction like OQ-19/OQ-20.**

---

## Resolved

### OQ-18 — Should `ContextProvider` composition become a sequential pipeline, like MAF's?

005 §5. A source-grounded pass on MAF's actual `AIContextProvider` mechanics
(`docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md` §2) confirmed a real,
previously-undocumented divergence: AgentEngine's `assemble_context()`
(`include/agentengine/core/context_assembly.hpp`) composes multiple `ContextProvider`s as
**independent fan-out** — each contributor sees only `SessionContext`/`EffectContext`, never a
prior contributor's `ContextContribution` — merging all results afterward in declared order. MAF's
`AIContextProvider.InvokingCoreAsync` is instead a **sequential pipeline**: provider N receives
provider N−1's already-merged `AIContext` as its own input (`ChatClientAgent.cs:772-785`, confirmed
by MAF's own doc comment), so a later provider can react to — dedupe against, build on, or suppress
— an earlier one's contribution within the same turn.

**Resolved by design → red-team → judge, no ADR needed (2026-08-11): reject a generic
pipeline/reactive mechanism entirely; fan-out stays.** A concrete opt-in design (a second
`ReactiveContextProvider` concept, detected at compile time, giving only providers that ask for it
a read-only `accumulated_so_far` parameter) was drafted and red-teamed. The red-team found it
insufficient as scoped and unnecessary as a generic mechanism:

1. **No provenance, so the motivating example doesn't actually work.** Neither `ContextContribution`
   nor `Message` tracks *which* provider contributed a given message/tool
   (`include/agentengine/core/context_provider.hpp`, `content.hpp`) — unlike MAF, which
   source-stamps every contributed message (`AIContextProvider.cs:174-176`,
   `WithAgentRequestMessageSource`) specifically so a later provider can tell what it's reacting to.
   A bare `accumulated_so_far` parameter without that stamp can only pattern-match flat merged
   content heuristically — fragile, and in tension with I4 (every effect is attributable).
2. **Reopens the exact coupling `context_assembly.hpp` already rejected**, just moved from the
   budget layer (its own comment explicitly rejects a shared cross-contributor budget pool) to the
   content layer — a reactive provider's output would depend on iteration order and every earlier
   contributor's actual content, the same shape of unpredictability already ruled out once.
3. **"Zero-cost, opt-in" doesn't survive the type-erasure boundary.** `ContextProviderDescriptor`'s
   `std::function`-based closures are already the one shape `assemble_context` iterates; supporting
   two provider shapes uniformly either widens every descriptor's signature (paid by non-reactive
   providers too) or adds a real per-iteration runtime branch — a correctable overclaim, not fatal
   on its own, but one more reason this isn't actually free.
4. **A no-code, principle-only entry would break this project's own ADR discipline.** Every existing
   ADR (`decisions/README.md`'s own definition; checked against all 26) pairs a decision with real,
   implemented, tested C++23 code — none is argument-only.
5. **The disagreement is resolvable by reading — a strictly better, already-working alternative
   exists in this repo.** `include/agentengine/core/history_and_skills_provider.hpp`'s
   `HistoryAndSkillsProvider<HistoryProviderT, SkillsProviderT>` is a real, proven pattern: a
   bespoke class implementing `ContextProvider` that owns concrete sub-provider instances directly
   (not via a generic table) and decides their composition itself — proven for the ordering problem
   (skills-advertisement-before-history on the wire, regression-tested in
   `test_agent_session_skills_real_backend.cpp`). The same pattern solves a future reactive need
   (e.g. a Phase-G `MemoryProvider` deduping against a `SkillsProvider`'s advertisement) with
   **better** provenance than the generic mechanism would have had — the composite's own code calls
   each sub-provider directly, so it knows exactly which one produced what by construction, no
   stamping mechanism required — at zero cost to every other provider that doesn't need it.

**Decision:** `assemble_context`/`ContextProvider` are unchanged. When a concrete cross-provider
reactive need materializes, solve it with a purpose-built composite provider in the
`HistoryAndSkillsProvider` idiom, not by extending the generic seam. Revisit only if a *third*
independent pairing shows the composite pattern doesn't scale — and if so, build and prove it as a
real ADR with the provenance question answered as part of the implementation, not deferred again.
This also resolves 005 §5's "named `ContextProvider` after MAF" framing: the compose-mechanics
divergence from MAF is confirmed real and is now a recorded, judged design choice, not an
undisclosed gap.

Full text: `docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md` §2;
`include/agentengine/core/context_assembly.hpp`; `include/agentengine/core/history_and_skills_provider.hpp`.

### OQ-11 — Licence and governance

024 Q1/Q3/Q4. Licence (MIT assumed, matching Quark), release cadence versus Quark, ADR judging
authority, and a security disclosure process. All required before any public release.

**Resolved, three parts (2026-08-04):**

1. **Licence** — MIT, confirmed by the project owner, matching Quark exactly. `LICENSE` written at
   the repo root.
2. **Governance/quorum** — not invented speculatively for a contributor population that doesn't
   exist: git state is local-only, no remote, no outside contributors, so the project owner is
   unambiguously the ADR judge (024 §4) today. A quorum rule is owed once the project actually takes
   outside contributions — new design work done at that time, matching Quark's governance if Quark
   has settled one by then, a fresh minimal default otherwise. Nothing about promoting an RFC or
   landing an ADR is blocked by this in the meantime.
3. **Security disclosure** — `SECURITY.md` written at the repo root: private reporting channel
   (placeholder contact pending a public remote), acknowledgement/disclosure timeline, the existing
   024 §3 security-exception carve-out cross-referenced, and OQ-10's corpus-publication split
   (classification public, payloads private) folded in as the disclosure policy's publication rule.

Full text: 024 §1 (C ABI versioning row, incidental to this pass), §7 Q1/Q3/Q4; `LICENSE`;
`SECURITY.md`.

### OQ-10 — Evaluation of safety controls

017 Q4 / 022 Q2. The adversarial corpus is the project's only real evidence that the safety layers
work, and a corpus without an owner and a cadence decays into a fixed set of attacks that the code
has been tuned against.

**Resolved by inheriting an existing mechanism rather than inventing a new one (2026-08-04):**

- **Cadence** — every 024 §4.2 design→red-team→prove→judge cycle touching 007/008/017/018 already
  produces adversarial attempts as part of judging its ADR; those attempts become corpus entries by
  default when the ADR is judged. The corpus grows exactly as often as security-relevant design work
  happens, with a quarterly backstop check so a quiet quarter doesn't let it go stale.
- **Owner** — the same authority that judges an ADR (024 §7 Q3's resolution: the project owner,
  while solo). One authority, not two, so corpus review carries the same enforcement weight as the
  design process feeding it.
- **Publication** — classification (categories, counts, which gate each maps to) is publishable and
  CI-generated, matching 021 §2's honesty-requirement pattern; the payloads themselves stay private
  until the fix proven effective against them ships, mirroring standard CVE disclosure practice.
  Written into `SECURITY.md` as the disclosure policy's publication rule, not tracked separately.

Full text: 017 §9 Q4, 022 §5, 022 §8 Q2, `SECURITY.md`.

### OQ-9 — Emerging protocols not designed against

A2UI (agent-generated declarative UI), AP2 (agentic payments), X42 (cross-boundary trust and
governance) are real and moving, and none is designed against here. 013's projection model means
A2UI is additive. AP2 and X42 would touch 007 and 018 structurally — payments in particular imply
`at-most-once` effects (019 §3) with real money attached.

**Resolved, three different answers for three different protocols (2026-08-04):**

1. **A2UI** — already closed, not merely deferred: 013 §3's projection architecture makes adoption
   additive by construction (AG-UI is already a projection of the internal event stream; A2UI is
   another one). No invariant or mechanism needs to preemptively account for it.
2. **AP2** — the architecture already has the right-shaped attachment point and doesn't need to
   change to accommodate it later: a payment mandate is an ordinary `Capability` (007 §3), and
   019 §3's `at-most-once` effect classification already anticipates non-idempotent external effects
   generally, payments included. What's missing is AP2-specific knowledge (mandate semantics, its own
   conformance suite) — genuinely new spec-writing once the protocol is in front of us, not an
   invariant change, and not done speculatively without dated research on its actual wire shape
   (CLAUDE.md's research-is-dated-and-cited rule).
3. **X42** — folded into 018 §8 Q1 rather than tracked separately: cross-boundary agent trust is
   already an open question there (SPIFFE-style workload identity vs. OAuth-only), and X42, if
   adopted, is a candidate *answer* to that question — another inbound-identity row feeding the same
   `Principal` (007 §2) — not a new mechanism sitting outside 018's model.

**Explicitly not done:** designing AP2 or X42's actual mechanics. This resolution closes what the
*architecture* owes them (an attachment point that already exists and is sufficient in shape); it
does not design either protocol, which stays out of scope until one is dated, cited research away
from being real.

Full text: 013 §3, 018 §8 Q1.

### OQ-8 — Second authoring surface

Python and .NET bindings are deferred (Specification §D3), with a C ABI as the intended seam
(020 Q3). Freezing that ABI shape too early constrains the bindings; too late and the bindings are
retrofitted onto whatever the C++ surface happens to be.

**Resolved, design early against stable concepts, freeze late against a real consumer (2026-08-04):**
the dilemma assumed the C ABI is a mechanical export of the C++ authoring surface (002's CRTP
templates), so its timing had to either anticipate or trail that surface. It can't be that: templates
have no ABI to export across a C boundary, so the C ABI was always a small, hand-designed seam over a
handful of already-stable concepts — `StartRun`/`ask_stream` and the `RunEvent` drain loop (020 §3a),
config resolution (020 §2), capability grants (007 §3) resolved to opaque handles — none of which
move the way a template signature would. Design work can start now, scoped to exactly that list.
Freezing is what waits, gated on a **reference out-of-process consumer** (a prototype Python or .NET
binding, revised at least once against what it exposed as wrong) before 024 §2's no-source-break
promise applies — the same argument-alone-does-not-reach-Proven discipline 024 §4 already applies
generally. The C ABI is now its own versioned artifact (024 §1), independent of "Engine" (020 §3a's
source-level, not binary-stable, embedding contract) — conflating the two was part of what made the
timing feel harder than it is.

Full text: 020 §8 Q3, 024 §1, Specification §D3.

### OQ-12 — A second WASM runtime?

008 §1a rejects `wasm3` for the plugin ABI: it has only partial Component Model and WASI P2 support,
which is the one axis the ABI depends on. But its advantages are real — best-in-class cold start,
tiny footprint, portability down to microcontrollers — and they matter for two niches: ultra-short
high-frequency guest calls (a content filter evaluated per message) and constrained deployments where
Wasmtime's footprint is prohibitive.

A second runtime behind the same host interface means **two sandbox-escape surfaces and two
conformance stories**. That is a real cost, and the benefit is currently an assumption: the cited
cold-start comparison is generic, not measured on our workload. Blocked on a 023-budget measurement
before it is even a candidate.

**Resolved 2026-08-03 by `decisions/ADR-008-wasm3-cold-start-vs-wasmtime.md`: the measurement now
exists, and it clears the gate.** A real, symmetric, correctness-checked cold-start comparison (fresh
parse/compile+instantiate+call+teardown per iteration, 1000 iterations, four consistent runs on this
machine) found wasm3's p50 ≈ 0.7 µs against Wasmtime's p50 ≈ 111-128 µs (**160-180x**), p99 ≈
0.7-1.0 µs against 241-289 µs (**250-400x**) — a real, structural gap (bytecode interpretation vs.
full JIT compilation every cold-start cycle), not measurement noise. **This does not change 008
§1a's rejection of wasm3 as the primary plugin runtime** — that rejection is about Component
Model/WASI 0.3 support, which this measurement is silent on. **What it resolves is narrower and
exactly what OQ-12 asked**: the cold-start assumption was untested and is now tested, on our own
workload shape, and it holds up strongly. Whether to actually build a scoped wasm3 backend behind
008's `SandboxBackend` contract remains a separate, larger decision this ADR does not make — it
would need its own pass evaluating 008 §1a's named cost ("two sandbox-escape surfaces and two
conformance stories"), and Wasmtime's realistic cached/precompiled deployment shape (which was not
measured here and could substantially narrow the practical gap) is still unmeasured.

### OQ-5 — Span-level taint

003 Q3 / 007 Q2 / 017 Q2. Per-part taint is what 003 specifies and it is coarse: a message that
mixes user text with a quoted tool result taints wholesale. Span-level taint would make
declassification and structural separation far more precise, at a real cost in the content model's
complexity and in every mapping layer.

**Resolved 2026-08-03 by `decisions/ADR-007-span-level-taint-vs-per-item.md`: no, keep per-item
taint.** A small prove built both a span-level taint prototype and the naive way someone would
plausibly implement it, and red-teamed the naive version rather than assuming it correct. Result: the
precision claim is real (per-item taint does deny genuinely trusted material that shares an item with
tainted material — confirmed with 003's own named scenario, a preamble concatenated with a tool
payload) but the naive `concat` implementation silently **under-tainted the actual danger bytes**
(and over-tainted the trusted ones) on the first attempt — a new, security-relevant bug class that
per-item taint structurally cannot have, because it carries no byte offsets to mis-shift. Separately,
the concrete scenario the precision claim was worried about is **already solved today** without spans
— keeping the trusted and tainted material as two separate `ContentItem`s (017 §3's existing
structural-separation idiom) reaches identical precision, using only mechanisms 003/017 already
specify. Net: span-level taint would trade a coarse-but-structurally-safe mechanism for a
precise-but-fragile one, for a case already covered another way — judged not worth it. Left
explicitly open: a single `ContentItem` needing to stay *partially* tainted after partial
declassification (e.g. a mostly-trusted summary embedding one still-tainted verbatim quote) is not
served by the two-item split and is not addressed by this ADR; if a concrete instance blocks a real
declassifier later, it should be re-opened narrowly against that case, not as "add span-level taint
generally" again.

### OQ-13 — Worktree merge policy for concurrent agents

025 §4 fails a merge on conflict and surfaces it, retaining both versions. Two unresolved parts:
whether the *model* should be offered conflict resolution as a task (it is often capable, and it is
also a good way to lose work silently), and whether `shared` mode should be permitted at all for
concurrent siblings — single-writer serialization makes it *safe* but still means an agent's files
change under it between reads.

**Resolved 2026-08-03, two parts.** First (model-assisted resolution): escalate to a human by
default; the model may **draft** a merged file but applying it always goes through the same
argument-hash-bound approval 006 §4 already requires elsewhere — never an auto-apply, because "the
model resolved it" is the same forbidden pattern 007 §4 already names in a different guise (a
data-loss decision derived from model output). Second (whether `shared` should be permitted for
concurrent siblings): confirmed by a small deterministic concurrency prove (scratchpad,
`shared_mode_readskew.cpp`, MSVC ASan, 10/10 clean runs, no Quark dependency — it models 025 §4's
single-writer-per-tree contract, not the real worktree actor) that single-writer serialization
prevents data races and lost single-file updates but **does not** prevent a concurrent sibling from
observing a torn *cross-file* view mid-update (one file already updated, a related one not yet) — a
reliable, on-every-run hazard given a forced interleaving, not a rare timing accident. **Decision:
`shared` stays available but is not the default for concurrent siblings** — 025 §3's existing
default (`branch`) was already right, and this prove is the falsifiable reason why; using `shared`
for concurrent siblings now requires an explicit opt-in that documents the read-skew hazard rather
than a bare mode flag. See `025-Worktree-and-Virtual-Filesystem.md` §3 and §10 Q1/Q2 for the full
resolution.

### OQ-6 — Plugin distribution

009 Q2. First-party registry versus OCI artifacts. OCI is content-addressed, signed, mirrorable, and
already deployed in every environment that would run this — which is a strong argument for not
building a registry. It also drags in an OCI client dependency and an authentication story.

**Resolved 2026-08-03 by a real, executed pull round trip** (not a design debate) against a public
OCI registry (Docker Hub, `library/hello-world`): anonymous bearer-token exchange (one plain HTTPS
GET returning JSON) → image-index manifest fetch → platform-specific manifest fetch → content-
addressed config blob fetch (redirecting to separate CDN storage) — five HTTPS requests total, every
returned digest independently re-hashed with SHA-256 and matched byte-for-byte against the server's
`docker-content-digest`. This confirms the "drags in a client dependency" cost was overstated for
AgentEngine's actual need: a **pull-only** client (token exchange, manifest fetch, blob fetch by
digest, SHA-256 verify) is a few hundred lines against an HTTP capability the engine needs anyway,
not a heavyweight OCI SDK — push, garbage collection, and resumable chunked upload are never needed
because AgentEngine is never the publisher (009 §7/§8 already assume third-party or operator-side
tooling publishes plugins and skills). **Decision: reuse OCI, no first-party registry** — see
`009-Plugin-and-Extension-System.md` §11 Q2 for the full resolution, including the one concrete gap
this surfaced (blob storage lives on a host distinct from the registry API host, so the egress
allowlist for a plugin pull must name both) and the one item explicitly left as follow-on design
work (mapping `plugin.aepkg`, §3, onto an OCI artifact's layer/annotation/referrer shape).

### OQ-14 — In-sandbox library surface area

026 §5 makes the `agent` library the CodeAct action space, which means every module added widens both
what the agent can accomplish and the host's attack surface. `agent.spawn` is the sharpest case:
model-written code creating runs is powerful and is a recursion-and-cost hazard; depth and budget
bounds are necessary and not obviously sufficient. There is no principle yet for what earns a place
in the library beyond case-by-case justification.

**Resolved 2026-08-03, two parts.** First, a falsifiable curation rubric (026 §5, four rules: maps
to exactly one trust-boundary-crossing thing or a zero-capability reporting channel; removing it
forces a strictly worse channel for a task class already argued for; not expressible as an ordinary
combination of other granted modules; every symbol still passes the "guessable from its name" bar on
its own) — applied to all nine current modules (all pass, for stated reasons) and to a plausible
rejected candidate (`agent.email`, which fails rule 3) to show the rubric actually discriminates
rather than rubber-stamps. Second, `agent.spawn`'s named "sharpest case" — small-proved and
red-teamed in `decisions/ADR-006-agent-spawn-depth-budget-bound.md`: depth bounds are sufficient
against unbounded recursion, conditional on the effect-mediation boundary already assumed elsewhere
(006 §9 G4) holding.

**The cost half is now also real, real-proven code, but only the standalone consume-pool
primitive — not a wired `agent.spawn` call path (2026-08-11,
`decisions/ADR-031-spawn-cost-budget-actor-primitive.md`).** `SpawnCostBudgetActor` (historical: a
real Quark `Sequential` actor holding a consumable token pool, proven under a REAL, multi-worker
`quark::Engine`, not just `quark::TestKit`, which cannot exercise a genuine race — ADR-037 later
ported this onto `agentengine::rt::SpawnCostBudget`, `rt::AsyncMutex` replacing `quark::Actor<
Sequential>` as the serialization mechanism, see `include/agentengine/rt/spawn_cost_budget.hpp`) was
proven to reject the exact double-spend 026 §9 Q1's sketch warned a bare copyable value
type would suffer: 8 concurrent callers contending for a 1000-token pool at 130 tokens each never
grant more than 1000 total, with the serialized dispatch closing the check-then-decrement
race, no lock code written by hand. **Not resolved by this**: `agent.spawn` itself still has no real
call path anywhere in this codebase (confirmed exhaustively during ADR-031's own design phase — no
`Tool<>`-conforming spawn tool, no nested-agent-run invocation mechanism, no sub-worktree wiring, no
production `AgentSession` tool-call loop able to host a spawned child; ADR-024 §7's own gap). Wiring
`SpawnCostBudgetActor` to a real spawn is future work, blocked on that larger, separately-scoped
machinery landing first — the same "small-proved standalone, not yet wired" shape ADR-006 already
established for the depth half. Wall-clock/deadline budgeting stays fully open, unchanged.

### OQ-16 — CodeAct has no discoverability story for its own granted surface

026 §4 gives `agent.tools` a real introspection story — generated docstrings, a `.pyi` stub,
`dir(tools)`/`help()` sourced from each Tool's declared metadata (006). That treatment stops at
`agent.tools`: the other seven `agent.*` modules (`files`, `data`, `memory`, `notes`, `output`,
`progress`, `ask`, `spawn`) had no equivalent, and nothing told the model *which* top-level modules
were even present for this session before it tried one. Not the same question as OQ-14 (which is
about what should be allowed to *exist* in the library, i.e. curation) — this one is about the model
discovering what has already been *granted*.

**Resolved 2026-08-03 by the two-part candidate resolution already sketched here, small-proved
in `include/agentengine/trust/agent_library_manifest.hpp`** (026 §5a): pull side generalizes
`agent.tools`' `dir()`/`help()` pattern to the whole `agent` namespace; push side extends 026 §7's
tool-surface token budget to a capability summary injected into `instructions` at session start.
Both are generated from the same `CapabilitySet`, proven (not just asserted) to agree with each
other across several grant combinations, including that an ungranted module is absent from both.
**The two named sub-questions are decided**: an ungranted module is **omitted**, not listed as
denied (026 §1a's existing "we omit, we do not lie" precedent, and pull-side `dir()`/`help()`
already covers the wasted-attempt case cheaply); **no separate persistent artifact** — pull-side
introspection already covers "detail on demand" at zero additional prompt cost. **Known limitation,
not solved here**: today's placeholder `Capability{kind}` (trust/capability.hpp) can't yet
distinguish a `/memory`-mount grant from any other mount, so `agent.memory`/`agent.notes` are gated
by plain `fs_read`/`fs_write` rather than mount-scoped grants — sharpens automatically once 007's
parameterized capability representation exists (007 §9, still open), not this resolution's job to
build.

Naming caution carried through unchanged: this is engine-generated and per-session, never mounted
or called a "skill" (009 §8's vocabulary is reserved for authored, distributable content). New gate:
026 G7. Full text: 026 §5a/§5b, §7, §8 G7.

### OQ-7 — Wasmtime version pinning

009 Q4 / 021 Q3. WASI 0.3 ships enabled by default from Wasmtime 46; earlier versions need the
0.3 release candidate. The async ABI difference between 0.2 and 0.3 is not a small compatibility
surface, and the plugin ABI is supposed to be stable. Pin, or support a range?

**Resolved 2026-08-03 by a small, concrete build-and-check** (not a design debate, per the project
owner's direction): **pin to a single version, currently 47.0.3** (the latest release as of this
date — v46 has already been superseded), no 0.2/0.3-RC range. Evidence: downloaded the real,
official `wasmtime-v47.0.3-x86_64-windows-c-api.zip` release directly from GitHub; confirmed the
shipped headers contain full Component Model support (`include/wasmtime/component/*`) and WASI
0.3's async primitives (`stream`/`future` types referenced in `component/func.h`,
`component/linker.h`, `component/types/val.h`) — grounded in the actual downloaded artifact, not
documentation; compiled and linked a minimal C++ smoke test against `wasmtime.dll.lib` with MSVC
19.51.36252 (the same toolset the rest of this project builds with) and ran a real
engine→store→module→instance→function-call round trip, which returned the correct result. A range
was rejected because there is no existing deployment depending on an older pin (design phase, no
external users) and because testing both 0.2 and 0.3 async ABI paths for zero present benefit is
exactly the "not a small compatibility surface" cost the question itself flagged — a straight
`GIT_TAG`/version pin (matching the existing `FetchContent` pattern already used for `nlohmann_json`
in `tests/CMakeLists.txt`) is simpler and sufficient. ~~**Not attempted:** actually instantiating a
real `.wasm` component (as opposed to a core module) through the `wasmtime_component_*` APIs — that
needs `wasm-tools`/`cargo-component` to produce a component binary, out of this small prove's scope;
the Component Model support claim rests on header inspection plus the module-level round trip, not
a component-level execution.~~ **Closed 2026-08-05 by M2 task D3**
(`decisions/ADR-010-wasm-component-host-manifest-capability-binding.md`): a real component, compiled
via `cargo component build --target wasm32-unknown-unknown`, loads, instantiates, and executes
through these same headers — engine/store/linker/instance/func-call all exercised for real, on
Windows and Linux, not merely the module-level round trip this entry originally stopped at.

### OQ-3 — Do capabilities cross process boundaries as tokens?

007 Q1 / 008 Q4 / 018 Q2. The `remote` sandbox profile, remote plugins, and delegated A2A calls all
want to carry *attenuated* authority across a process or network boundary. A macaroon-style bearer
capability with caveats is the known answer; it adds a crypto dependency, a revocation problem, and
a new forgery surface. Without it, each remote path invents its own bespoke authority protocol —
which is worse.

**Resolved 2026-08-03 by `decisions/ADR-005-capability-bearer-tokens-cross-process.md`**, a small
prove (per the project owner's direction: real C++23, red-teamed, not a full-scale build) rather
than a design→prove loop measuring overhead first: **yes, narrowly.** A self-verifying HMAC-chained
bearer token (`trust/capability_token.hpp`) is accepted for the `ExpiresAt`/`PathPrefix` caveat
classes proven there — attenuation-only and forge-resistant under red-team (bit-flip, field tamper,
caveat-strip, caveat-reorder, fabricated-parent derivation all rejected; clean under MSVC ASan, zero
findings). The anticipated revocation problem is real and was not solved inside the token: a minted
token is valid until its own caveats lapse, with no way to unmint it early, so any capability needing
immediate revocation should use the ADR's other proven design (a host-side `CapabilityRegistry`)
instead, or a short-lived token re-minted frequently. The anticipated performance win was **not**
established — measured **INCONCLUSIVE**, favoring the registry in this pass's specific (unoptimized,
same-process) measurement — see the ADR §6-§9 before assuming either design is faster. Windows-only
for now (021 §2); a Linux HMAC backend is unbuilt and named as a residual risk, not urgent given the
Windows-now/Linux-next sequencing (OQ-1).

### OQ-2 — Is the single-agent turn loop a workflow?

001 Q1 / 014 Q1. Making the turn loop a special case of the workflow graph would buy one execution
model, one checkpointing story, one visualization, and one replay mechanism, at the risk of paying
graph overhead on the overwhelmingly common single-agent path.

**Resolved 2026-08-03, by grounding in MAF's own source rather than a design→prove overhead
measurement** (candidate resolution originally proposed): **no, keep them separate.**
`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §1 shows MAF's `agent.run()` never touches
`Workflow`/`Executor` machinery, and the dependency direction is the opposite of "agent is a
special-cased workflow" — `AgentExecutor` wraps `agent.run()` to let an agent opt into being one node
of a graph, not the reverse. Since AgentEngine's developer model is deliberately MAF-shaped
(CLAUDE.md), this settles the question by precedent rather than by re-deriving it from an overhead
benchmark: 001 §3's turn loop stays its own lightweight coroutine; 014 §1's `Executor = an agent | a
function | a sub-workflow | a request port` already has the right shape for the opt-in case.

A second, structural argument reaches the same conclusion independently: 014 §1's `Executor` already
defines workflows as built *from* agents, so making the turn loop itself a workflow graph would
invert or merge that dependency (001 needing 014's graph machinery to define a run, while 014 still
needs 001 to know what an agent is), forcing a "graph-of-one is special" carve-out that defeats the
uniformity the question wanted in the first place — the overhead concern becomes moot rather than
needing to be measured, since no implementation exists yet to benchmark a graph-of-one against the
direct loop and the layering argument doesn't depend on that measurement anyway. What *is* unified,
at the layer that actually should own it: turn boundaries and workflow superstep boundaries are peer
checkpoint-boundary kinds sharing one `Store` and replay mechanism (019 §1), and both project onto
one internal event stream (013 §1) — the shared checkpointing/observability story the candidate
resolution wanted, without collapsing the two execution models that produce it. Full reasoning:
001 §10 Q1, 014 §9 Q1.

### OQ-4 — Unifying human-in-the-loop and long-running work

**Escalated from 🟠 after the A2A/AG-UI research**, which showed the three protocols do not merely
differ in encoding — they differ in *control flow*, and each demands a different correlation
identity:

| Protocol | Shape | Identity it requires |
|---|---|---|
| **MCP** `2026-07-28` | Client **retries the original request** with a *new* JSON-RPC id | `requestState` (opaque, client **MUST NOT** parse) |
| **A2A** v1.0 | Task **stays alive** in `INPUT_REQUIRED`; client sends a new message | `taskId` |
| **AG-UI** | Run **ends** with an interrupt outcome; client starts a **new run** | `interruptId` |

A retry, a continuation, and a restart, plus the same three-way split recurring for long-running
tool/task work (`Suspended`, MCP's `tasks` extension, A2A's task lifecycle).

**Resolved (2026-08-04), in two parts:**

1. **Human-in-the-loop correlation** — a new internal identity, `Interaction.interaction_id`
   (001 §2), is the one thing the run itself knows; each protocol adapter maps it into its own
   required shape and nothing more:
   - **AG-UI**'s `interruptId` *is* `interaction_id` (naming equivalence, no encoding); its
     structural "every open interrupt needs one covering resume" rule already enforces the
     multi-interaction case a run with several concurrent workflow request ports (014 §4) can hit.
   - **MCP** carries `interaction_id` inside `requestState`, integrity-protected per 011 §8a — the
     new JSON-RPC id MCP mints on every retry carries no correlation meaning for us.
   - **A2A** needed no new mapping: `Task ← Run` (012 §1) is already a direct identity, so `taskId`
     maps to the run/session identity and outlives any single `INPUT_REQUIRED`. Where a task has
     multiple concurrent open `INPUT_REQUIRED` requests, each is its own `Interaction` (structured
     per `run_id`, `reason: input`), and the per-interaction `interaction_id` rides alongside
     `taskId` as task metadata (012 §5a) — the one-`taskId`-many-`interaction_id`s case a flat A2A
     identity alone couldn't disambiguate.
   - The pre-pause "emit pending state first" rule AG-UI's spec states explicitly is generalized to
     every adapter (001 §2), not treated as an AG-UI-only obligation.
2. **Long-running work** — recognizing the three shapes were never peers at the same granularity
   dissolved the question rather than requiring a fourth mechanism: A2A tasks already **are** our
   `Suspended` run (no gap); MCP's tasks extension, which is scoped to a single long tool call, maps
   onto `Backgroundable`/`StandingEffect` (006 §6b); a whole-run pause surfaced over MCP reuses the
   same `requestState`-carried `interaction_id` as MRTR. See 019 §8 Q2 for the full breakdown.

Full mapping: 013 §2.2. Written into 001 §2, 011 §3.4/§13 Q2, 012 §5a, 013 §2.2, 014 §4, 019 §8 Q2.

### OQ-1 — macOS and Quark's PAL

Quark has `linux_x86_64` and `windows_x86_64` PAL backends; there was no macOS backend, and RFC 021
claimed macOS as a Supported tier anyway — the largest single portability risk in the project,
carried with no owner and no CI evidence.

**Resolved 2026-08-03 (project-owner decision), sharpened to a full drop: macOS is not a target,
full stop** — dropped as a target platform entirely, project-wide, not contributed upstream and not
merely downgraded to a lower tier. No PAL backend will be contributed upstream for it, and no RFC may
claim macOS support. 021 §2's target matrix now lists macOS as **Unsupported — no claim**; every
"Windows, Linux, and macOS" claim across CONVENTIONS.md, the Specification, README, and RFCs
008/009/010/025 was edited to match. Delivery of the two remaining platforms is explicitly sequenced
rather than simultaneous: **Windows x86-64 is the v1 target and the only platform under active
implementation now; Linux x86-64 is the next target, taken up once the Windows implementation reaches
a stable state** (021 §2). This replaces the "contribute upstream vs. downgrade the tier" framing the
question was originally posed with — neither candidate resolution was taken; macOS is dropped
outright rather than downgraded to a lower tier.

008 §1's `no microvm profile` decision, whose original rationale cited the macOS PAL gap, was
re-grounded instead in a Windows-hosting finding (`docs/research/2026-microvm-windows-portability.md`,
2026-08-04: Firecracker is architecturally Linux+KVM-only, no production-grade Windows-hosted path
exists either) so the decision doesn't lose its footing now that macOS is moot rather than missing.
Re-adding macOS later, if a macOS PAL backend is ever built for `agentengine::pal` (historical: this
was originally framed as "contributed to Quark," back when Quark was still the vendored PAL; ADR-037
removed that dependency), is a fresh decision starting at Best-effort — not a reinstatement.

Updated to match: `021-Platform-Support-and-Portability.md` §2 (target matrix), §3 (per-subsystem
table), §5 (CI matrix), §6 (G1), §7 (this question); `CONVENTIONS.md` Target & scope;
`AgentEngineSpecification.md` (portability property, WASM-artifact claim); `008-Sandbox-and-
Isolation.md` (goal statement, locked-decision bullet, `wasm`/`native-jail` profile table, G1 gate);
`009-Plugin-and-Extension-System.md` (goal statement, G1 gate); `010-Python-Code-Interpreter.md`
(shell-portability discussion, G1 and G6 gates); `024-Versioning-Compatibility-and-Governance.md`
Q2; `025-Worktree-and-Virtual-Filesystem.md` G2; `README.md` (plugin-portability line);
`src/backends/native_jail/README.md` and `src/backends/wasm/README.md`. `decisions/ADR-001/002/004`
and the dated `docs/research/*.md` notes are left as-is — they are historical records of what was
actually run/researched, not live claims.

**Owner:** n/a — scope removed rather than resourced. **Unblocks:** 021 promotion (§6 G1/G3/G4 now
read against two platforms, not three); no macOS claim remains anywhere in the project.

### OQ-17 — No generic/first-party skills or tools catalog

006, 009 §7/§8, 026 §5. MAF ships no first-party generic skills or tools — every `SKILL.md` in its
repo is sample/demo content, not a shipped standard library — so a generic catalog for AgentEngine
was new design surface, not something to port.

**Resolved by candidate (a):** 009 §7 is now explicitly the generic tool catalog (source-agnostic,
capability-crossing operations only — math/JSON/unit-conversion stays un-Tooled per 026 §1's code
interpreter), headed by a first-party `read_content`-class tool whose preview-plus-`BlobRef` shape
(006 §7, 028 §2) exists specifically to close the token-budget hazard a naive "read this file" tool
would open. 009 §8f adds the generic-skills half: `using-the-code-interpreter`, `using-codeact`,
`reading-large-content`, `producing-structured-output`, `shell-pipelines`, shipped in-repo and
mounted by default under the same grant model as any skill. Related, still open: **OQ-14** (agent.*
library curation) is the same "what earns a place" question one level down, at the Python-surface
level rather than the tool/skill-catalog level.

### OQ-15 — Module-name import gating can't distinguish a trusted package's internals from guest code

Originally raised by `decisions/ADR-002-pythonrunner-embedding-and-mediation.md`'s prove phase
(2026-08-01): making `import numpy, pandas` work under `PythonRunner`'s import allowlist required
granting roughly 130 top-level module names, not 2 — including `ctypes`, `winreg`, `_wmi`, `_winapi`,
and `subprocess`, all transitively required by numpy's own platform-detection code, and all worked
examples (008 §1b, ADR-002) of names the mechanism exists to deny. The finder itself was not broken
(it enforces exactly the allowlist it's given, correctly) — the problem was granularity: it sees
*which module* is being imported, never *which code is asking*, so granting `numpy` also grants
guest code identical, indistinguishable access to `ctypes` directly.

**Resolved by `decisions/ADR-003-caller-aware-import-gating.md` (2026-08-01)**, candidate resolution
1 (caller-aware import gating): designed, red-teamed, revised twice (once after red-team, once after
an additional gap surfaced during independent prove-phase re-verification), implemented, and proven
against the real target (CPython 3.13.5, numpy 2.3.3, pandas 2.3.3, Windows/MSVC+clang) for the
gated-name set `ctypes`/`_ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess`. 010 §9 G1's flagship
NumPy+pandas success case may now be described as closed-by-construction for these six names
specifically, not merely kernel-jail-bounded, within the scope below.

**Not airtight — read ADR-003 §9 in full before relying on this for a new package policy.** §9.2/§9.3
name the residual risks explicitly: a gadget-chaining variant (§6.1) and a fail-closed C-reentrancy
question (§6.5) remain open; registry-pointer address-reuse safety (claim B12) is INCONCLUSIVE rather
than proven; and the mechanism has a demonstrated history — three independent misses across design,
red-team, and the initial prove pass, each finding a different unhandled entry point into CPython's
import machinery — of missing entry points, not a hypothetical risk. A future caller-gated name or
CPython version should be specifically re-verified, never assumed covered by the existing skip-anchor
set. Candidate resolution 2 (accept-and-document tiering, ADR-002 §10.2) remains the fallback for any
gated name or package this mechanism has not been checked against.
