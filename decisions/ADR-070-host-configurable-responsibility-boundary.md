# ADR-070 — Should AgentEngine keep enforcing one strict safety posture everywhere, or name a
# formal seam for shifting part of that responsibility to the consumer dev, and where does that
# seam stop?

**Status:** Proposed (design → red-team → prove phases complete; awaiting explicit user "Judged").
Implemented: `include/agentengine/core/tool_pipeline.hpp` (`PolicyDecider`/`policy_decision`/
`resolve_approval_outcome`), `include/agentengine/core/agent_registry.hpp` (`invoke_agent_tool`
threading), `include/agentengine/rt/agent_session.hpp` (`set_policy_decider`/`policy_decider`,
`expired_interaction_ids`/`set_interaction_expiry`, `history_provider()`'s amended doc contract),
`include/agentengine/workflow/graph.hpp` (`TerminationBound::token_budget`'s amended doc contract),
`include/agentengine/rt/workflow_supervisor.hpp` (`token_budget_unenforced()`). Proven by
`tests/test_tool_pipeline.cpp` (6 new checks), `tests/test_rt_agent_session_suspend_approval.cpp`
(SU7-SU9, 20 new checks), `tests/test_rt_workflow_supervisor.cpp` (6 new checks) — real Windows/MSVC
build, see §5 for commands and counts.

**Relates to:** `007-Capability-and-Trust-Model.md` (owns I2/I3, the two invariants this ADR must not
weaken), `decisions/ADR-023-response-format-codec-seam.md` (the `text_derived` declassifier gate this
ADR deliberately leaves closed, citing that ADR's own red-team finding), `decisions/ADR-033-
middleware-model-call-chain.md` (the middleware declassification-position closure this ADR also
leaves closed), `decisions/ADR-029-suspend-for-human-approval.md` (§6's named expiry gap, amended
here), `decisions/ADR-066-context-provider-attribution-provenance.md` (§7's named single-provider-
path residual, amended here), `decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md`
(prior art for the pattern this ADR names formally), `OpenQuestions.md` OQ-19 (referenced, not
resolved — see §7).

## 1. The question

**Stated so it has a wrong answer:** the project owner asked for a deliberate policy shift — trade
some "safety above all" for a better features/safety balance, so building new frameworks and
features on top of AgentEngine is not blocked by defaults only AgentEngine itself is allowed to set,
by shifting part of that responsibility to the consumer dev (the application author building on
AgentEngine). Named scope: capability/sandbox defaults, approval/human-in-the-loop gates, the I3
model-output-authority boundary, the `ContextProvider` chain, and `Workflow`. Does honoring that
request mean loosening I2/I3's own mechanisms (relaxing `Tainted<T>`, opening the `text_derived`
declassifier allowlist, letting a middleware reach a declassifying position, letting a workflow
executor's capability ceiling come from an ambient source) — or does it mean something narrower: a
named, disciplined pattern for handing a host a decision seam that can only ever narrow or decide
among authority the host's own session already possesses, never mint or widen it? Getting this wrong
in the permissive direction reopens exactly the ambient-authority and model-output-as-authority
holes 007 exists to close; getting it wrong in the conservative direction (declining to do anything
because "it might weaken I2/I3") fails to honor a legitimate, explicit product decision and leaves
real, already-named gaps (ADR-029 §6, ADR-066 §7, the `token_budget` silence) undocumented.

## 2. Prior art surveyed, and what it already proves

Three research passes (capability/sandbox/approval defaults; the I3 taint/declassification boundary;
`ContextProvider`/`Workflow`) found that most of the flexibility the request asks for **already
exists**: `ApprovalDecider` (`tool_pipeline.hpp:317-318`, a fully host-supplied arbitrary decision
function), capability grant parameters (all host-authored data into `CapabilitySet::grant_root()`),
`SandboxBackend` (a swappable concept, not a closed engine enum), `ContextProvider` (a compile-time
concept, not a virtual interface — any host conformer is valid), RAG's `Embedder`/`VectorIndex`/
`ChunkingPolicy`, `corpus_scope`, `worktree_mode`, and every `TerminationBound` value are all already
host-authored/host-swappable. `decisions/ADR-068-runtime-secret-quarantine-host-delegated-
detection.md`'s `SecretDetector`/`QuarantineAuditHook` — host-injected, `nullptr`-by-default,
structurally barred from ever reaching a capability-granting position (`grant_eligible_ref_names()`
unreachable from any `Tool::invoke()` body, ADR-068 §5) — is the cleanest existing instance of the
shape this ADR names formally below. What was actually missing was (1) a named principle stating
this pattern is the intended mechanism, checked against for new work, and (2) a small number of real,
already-documented gaps where the pattern should apply but doesn't yet.

## 3. The competing designs

**Design A — status quo, no formal principle.** Keep adding host seams ad hoc, as each ADR happens
to need one, with no named checklist and no explicit "what must never become a seam" list. Steelman:
zero process overhead, and it's what produced `ApprovalDecider`/`SandboxBackend`/ADR-068 already,
each independently reasoned to the same shape. Rejected as the SOLE answer: the same survey that
found those three also found real inconsistency this ADR closes — `policy_driven`'s fail-closed
degrade has no seam at all (only `always_require`'s), and two real gaps (ADR-029 §6, ADR-066 §7) sat
undocumented as bugs rather than acknowledged, host-facing trade-offs. Without a named pattern, a
future contributor has no checklist to red-team a new seam against, and "responsibility shifted to
the host" silently means different, incompatible things in different corners of the codebase.

**Design B (chosen) — name the Delegated Decision Seam pattern, apply it to a bounded set of real
gaps, and name what must never become one.** Formalize the shape already implicit in `ApprovalDecider`/
`SandboxBackend`/ADR-068 as five required properties (§4), and implement four bounded instances of
it that close real, already-documented gaps rather than inventing hypothetical ones. Steelman:
matches this codebase's own established idiom exactly (no new machinery class), gives every future
"should this become host-configurable?" question a checklist instead of a fresh argument, and is
honest about the two things it is NOT doing — building 007 §5's full declarative policy DSL, and
implementing OQ-19 (both named out of scope, §7).

**Design C — rejected outright, named for the record.** Remove or weaken `Tainted<T>`'s mechanism,
the `text_derived` closed declassifier allowlist, or ADR-033's middleware closure, on the theory that
"the host is responsible now, so the engine doesn't need to enforce this." Rejected without a prove
phase: ADR-023's own red-team already ran exactly this experiment (a laxer text_derived declassifier)
and found it unsafe (007 §4's amendment); ADR-033's middleware closure exists BECAUSE a host-facing
seam at that exact position was found to let a compromised/buggy middleware forge a trusted
`ToolCall`, a confused-deputy hole. I2/I3 are invariants, not defaults — CLAUDE.md's locked-decisions
list requires a new ADR to relitigate them, and this one doesn't attempt to; it works *around* them,
narrowing or deciding among already-possessed authority, never touching the mechanism that makes
authority unforgeable in the first place.

## 4. The Delegated Decision Seam — five required properties

Every seam this ADR adds, and every future one checked against this ADR, must be:

1. **Explicit opt-in.** A host must be handed the seam by the engine; it is never reachable
   ambiently (I2). Every seam below defaults to `nullptr`/unset and requires an explicit `set_*`
   call.
2. **Fails closed/safe when unset.** The default reproduces today's exact behavior, byte-for-byte —
   proven per-item below as a regression claim, not merely asserted.
3. **Narrows or decides among already-possessed authority only.** Never mints, widens, or reaches a
   capability-granting or declassifying position. This is the property that keeps the pattern
   compatible with I2's attenuation-only rule (007 §3 property 2) and is checked structurally, not
   just documented, in §4a below.
4. **Host code, never model output.** The callable is trusted host/operator code; nothing tainted
   can occupy this seam's decision (I3 untouched by construction — the seam's function signature
   never accepts a `Tainted<T>`).
5. **Always audited (I4).** A delegated decision produces the same audit-record shape as an
   engine-made one — who decided is recorded, not laundered.

## 4a. What must NOT become a seam, and why (the other half of "balance")

- **`text_derived` tool-call auto-declassification**
  (`tool_pipeline_detail::is_auto_declassifiable_text_derived_call`, `tool_pipeline.hpp:364-378` /
  `trust::is_inert_for_text_derived_declassification`, `trust/capability.hpp:223-238`). ADR-023's own
  red-team already found a laxer version (schema-validation-as-declassifier) unsafe. This ADR's own
  `PolicyDecider` (§5a) is explicitly barred from this path — proven in §5's tests, not just stated.
- **`Tainted<T>::unsafe_view()`** (`core/tainted.hpp:28-32`) — the sole escape hatch stays
  ungeneralized; no policy parameter, no registration API for a host-authored declassifier.
- **ADR-033's closure of the middleware declassifying position** (`ModelCallContext` carries no
  `EffectContext&`/capability type) — stays closed; this ADR's `PolicyDecider` lives in
  `tool_pipeline.hpp`/`AgentSession`, never in the middleware chain, so it cannot become a second,
  differently-audited path to the effect ADR-033 blocked.
- **`SpawnBudget::attenuate_for_spawn()`** (ADR-006) **/ `SpawnCostBudgetActor`** (ADR-031) —
  compiler-enforced attenuation-only and no-refill stay structural, not host-configurable; this is
  the depth/cost ceiling itself, I2-adjacent, not a policy sitting on top of one.
- **I2's empty-by-default/attenuation-only guarantee** (`CapabilitySet::grant_root()` as sole
  non-empty producer, ADR-009) — this is the invariant, not a default sitting on top of it.
- **`check_workflow_executable()`'s hard rejection of `agent`/`sub_workflow` executors**
  (`workflow/graph.hpp:431`) — stays rejected. `OpenQuestions.md` OQ-19 carries an explicit, dated
  project-owner instruction ("document only, do not implement yet", 2026-08-13) that this ADR does
  not override. §7 records the pattern's answer to how OQ-19 would eventually resolve, as a
  cross-reference, not an implementation.

## 5. Falsifiable claims, evidence, and per-claim verdicts

Four bounded prove items, each a self-contained Delegated Decision Seam instance closing a real,
previously-named gap.

### 5a. `policy_driven` graduated resolution (`PolicyDecider`)

`Agent<...>`'s `policy_driven` approval mode unconditionally degraded to "requires the same
`ApprovalDecider`, fail-closed" (`tool_pipeline.hpp`'s file-top comment) because 007 §5's declarative
rule language was never built. Added `PolicyDecider = std::function<policy_decision(Principal
const&, ToolDescriptor const&, bool arguments_tainted)>` returning `{auto_approve, auto_deny,
require_approval}`, appended as a trailing, default-`{}` parameter to `invoke_tool()` and
`invoke_agent_tool()`, and as a new `AgentSession::set_policy_decider()`/`policy_decider()` member —
consulted at exactly the two places that decide anything about `policy_driven`: the main round
loop's `invoke_tool()` call and the suspend-for-approval pre-check immediately above it. The three
"already resolved by a real human" `invoke_tool()` call sites (`resolve_interaction()`'s approved
branch, `resolve_codeact_ask()`) keep their own `one_shot_approve` and the trailing parameter's
default — a human's explicit approval is never re-litigated by policy.

| Claim | Disproving experiment | Verdict |
|---|---|---|
| An unset `PolicyDecider` reproduces today's exact fail-closed `policy_driven` behavior. | Call a `policy_driven` tool with no `PolicyDecider` and no `ApprovalDecider`; assert denial. | **CORRECT** — `test_tool_pipeline.cpp`, "ADR-070 regression" check. |
| `auto_approve` bypasses `ApprovalDecider` entirely (a real short-circuit, not a decider that happens to say yes). | Wire a `PolicyDecider` returning `auto_approve` alongside an `ApprovalDecider` tripwire that fails the test if ever called; assert success AND the tripwire never fires. | **CORRECT** — `test_tool_pipeline.cpp`, both checks pass; audit record still `ok`. |
| `auto_deny` blocks the call WITHOUT consulting `ApprovalDecider`, even one that would approve. | Wire a `PolicyDecider` returning `auto_deny` alongside an `ApprovalDecider` tripwire that fails the test if ever called; assert denial (distinct error code `tool.policy_denied`) AND the tripwire never fires. | **CORRECT** — `test_tool_pipeline.cpp`. |
| `require_approval` falls through to `ApprovalDecider` exactly as an unset decider would. | Wire `require_approval` plus a real `ApprovalDecider`; assert it IS consulted. | **CORRECT** — `test_tool_pipeline.cpp`. |
| `PolicyDecider` can never bypass capability binding (step 4/7 runs before step 5, structurally). | `auto_approve` wired, but `held` lacks the tool's declared capability; assert denial with `tool.capability_not_held`, not a policy path. | **CORRECT** — `test_tool_pipeline.cpp`; this is also a structural proof of §4's property 3, not merely a policy-layer test. |
| `text_derived` never consults `PolicyDecider`, even for a `policy_driven` tool (§4a's must-not-loosen list). | Wire a `PolicyDecider` tripwire on a `text_derived` call to a `policy_driven`, capability-bearing tool; assert denial (no decider present) AND the tripwire never fires. | **CORRECT** — `test_tool_pipeline.cpp`. |
| The suspend-for-approval pre-check (not just `invoke_tool()`'s own step 5) recognizes a `PolicyDecider`'s verdict, so a resolved call never opens a needless human `Interaction`. | Session-level: `suspend_for_approval_` true, no `ApprovalDecider`, a `PolicyDecider` wired `auto_approve`/`auto_deny`; assert the run converges with NO `Interaction` ever opening, for both verdicts. | **CORRECT** — `tests/test_rt_agent_session_suspend_approval.cpp` SU7 (auto_approve, tool invoked, no suspend) and SU8 (auto_deny, tool never invoked, no suspend). |

Windows/MSVC build, `ninja test_tool_pipeline test_rt_agent_session_suspend_approval`: zero compile
errors. `test_tool_pipeline`: all checks pass (6 new ADR-070 checks plus all pre-existing ones,
including the full ADR-023 `text_derived` regression suite, unaffected). `test_rt_agent_session_
suspend_approval`: all 45 checks pass (SU1-SU9, SU7-SU9 new).

### 5b. ADR-029 §6 approval-suspend expiry (`expired_interaction_ids`/`set_interaction_expiry`)

`Interaction::expires_at_ns` (`core/interaction.hpp:53`) existed but nothing checked it, AND nothing
ever set it either (`open_interaction()`'s body only ever wrote `interaction_id`/`run_id`/`reason`) —
a suspended run with no human answer stayed suspended forever, a real, previously-named gap (ADR-029
§6). A found-during-implementation correction to this ADR's own original plan: `interaction.hpp`'s
own comment states "no real wall-clock source wired in anywhere in this project yet (Clock is not a
wired capability)" — an engine-internal timer/poll would have to invent exactly the untracked
nondeterminism I5 forbids. Implemented as a pure host-driven query pair instead of a callback+timer:
`AgentSession::set_interaction_expiry(interaction_id, expires_at_ns)` lets a host that learns of a
suspension (the `input_required` event already names the id) opt it into a timeout in the host's own
wall-clock terms; `AgentSession::expired_interaction_ids(now_ns)` lets the host later ask, in that
same host-supplied "now," which open interactions have passed their configured expiry. Resolving an
expired interaction reuses the ALREADY-EXISTING `resolve_interaction({id, approved: false})` — no new
resolution mechanism, and no new race: that path is already serialized by `session_mutex_` (I1)
against a concurrently-arriving real human answer.

| Claim | Disproving experiment | Verdict |
|---|---|---|
| An interaction nobody calls `set_interaction_expiry` for keeps `expires_at_ns == 0` — unchanged behavior for every existing caller. | Open a suspension, never call the setter; assert `expired_interaction_ids()` returns empty for any `now_ns`. | **CORRECT** — SU9. |
| `set_interaction_expiry` on an unknown id is a no-op returning `false`. | Call it with a fabricated id; assert `false` and no state change. | **CORRECT** — SU9. |
| `expired_interaction_ids(now_ns)` is exact: empty strictly before the configured expiry, non-empty at/after it (inclusive). | Set an expiry; query at `now_ns` one below, then at exactly the configured value. | **CORRECT** — SU9, both checks. |
| A host-driven expiry-denial resolves through the ordinary path, converges the run, and never invokes the gated tool. | Query, then call `resolve_interaction({id, approved:false})` for an expired id; assert convergence, no tool invocation, interaction closes. | **CORRECT** — SU9; identical shape to SU4's existing human-denial proof, now reached via the timeout path. |

`ADR-029-suspend-for-human-approval.md` §6 amended in place (this ADR's own note appended, the
original "still unwired" text kept verbatim above it, per this project's own "the reasoning that was
wrong is part of the record" convention for amendments, `decisions/README.md`).

### 5c. Workflow `token_budget` — host-monitored, no longer silent

`TerminationBound.token_budget` (`workflow/graph.hpp:184`) was host-settable but never read by
`WorkflowSupervisor::execute()` (confirmed: only `bound_max_rounds`/`deadline_ms` are checked) — a
silent gap, not a deliberate policy either way, and distinct from `AgentSession`'s own per-agent
`token_budget_`, which IS already enforced (`agent_session.hpp:1741`, unrelated field, not touched by
this ADR). Real enforcement was rejected as out of this ADR's bounded scope (it would require
correlating usage-tracking per-executor at the exact point `execute()` checks bounds, a real feature
this codebase does not have the plumbing for today). Right-sized fix: an unconditional, deterministic
query, `WorkflowSupervisor::token_budget_unenforced()`, plus a strengthened doc contract on the field
itself — so a host relying on it un-monitored gets a query it can check and act on (log, assert,
refuse to run), not silence it has to discover the hard way.

| Claim | Disproving experiment | Verdict |
|---|---|---|
| `token_budget_unenforced()` is `true` immediately after `initialize()`, before any round runs, whenever `token_budget` is set — no path defers or swallows the signal. | Set `token_budget`, call `initialize()`, check before `run_workflow()`. | **CORRECT** — new `test_rt_workflow_supervisor.cpp` check. |
| The signal still holds after a completed run (it reflects the DECLARED bound, not run-time consumption this engine never tracked). | Run the workflow to completion; re-check. | **CORRECT** — same test. |
| `token_budget_unenforced()` is `false` when the host never set `token_budget` — this is a signal about the declared bound, not a blanket warning. | A workflow with only `max_rounds` set; assert `false`. | **CORRECT** — same test. |

### 5d. ADR-066 §7 single-provider-path attribution — formalized, not fixed

`assemble_context()`'s provenance/attribution stamping (ADR-066) is structurally mandatory only on
the multi-contributor `ComposedContextProvider` path; `AgentSession`'s direct single-`HistoryProviderT`
slot — still today's dominant production path — bypasses it entirely, an already-named residual
(ADR-066 §7), not newly discovered. This item is documentation-only, matching the "narrows or decides
among already-possessed authority" property vacuously (there is no authority decision here at all,
only a discoverability gap): `AgentSession::history_provider()`'s own accessor comment now states
the two-tier contract explicitly (single-provider slot = no attribution, by deliberate design, not
omission; a host wanting mandatory stamping composes through `ComposedContextProvider` instead), and
ADR-066 §7 carries a matching amendment note. No falsifiable claim beyond "the documentation now says
what the code has always done" — verified by reading both edited comments back against
`assemble_context()`'s own real behavior, unchanged by this ADR.

## 6. The red-team attack

Applied hardest against §5a (the most novel, capability-adjacent item):

1. **Can a wired `PolicyDecider` reach a capability the tool never declared?** No — step 4/7's bind
   check runs unconditionally BEFORE step 5 in `invoke_tool()`'s own body; §5a's fifth claim proves
   this structurally (an `auto_approve` verdict against an unheld capability still denies with
   `tool.capability_not_held`, never reaching the policy branch's own success path).
2. **Can `text_derived` provenance route through the new seam to reach the closed declassifier
   allowlist?** No — `resolve_approval_outcome()` gates on `provenance != call_provenance::
   text_derived` before ever consulting `policy`; §5a's sixth claim proves the seam is provably
   unreachable for that provenance, with a tripwire, not just an assertion of intent.
3. **Does a `PolicyDecider`-driven auto-approve produce an audit record indistinguishable in shape
   from a decider-approved one (I4)?** Yes — `finish()`'s audit-population code in `invoke_tool()` is
   unconditional and runs identically regardless of which branch of step 5 was taken; §5a's second
   claim's `audit.ok` check confirms this for the new path specifically.
4. **Does the suspend pre-check's own separate consultation of `PolicyDecider` risk drifting from
   `invoke_tool()`'s own step-5 logic (two independently-maintained copies of the same decision)?**
   Closed structurally, not by convention: both call `resolve_approval_outcome()`, the SAME exported
   function — the identical "single source of truth" discipline `tool_call_requires_approval` already
   established for ADR-029's own pre-check, extended rather than duplicated.
5. **Does the ADR-029 expiry mechanism reintroduce the concurrent-timeout-vs-human-answer race the
   original plan worried about?** No new race exists to close: `set_interaction_expiry`/
   `expired_interaction_ids` never mutate resolution state themselves — resolving an expired
   interaction reuses `resolve_interaction()`, which already serializes via `session_mutex_` (I1)
   against a concurrent human answer for the SAME interaction; there is exactly one code path that
   ever closes an interaction, unchanged by this ADR.
6. **Does `token_budget_unenforced()`'s existence create a false sense of enforcement (a diagnostic
   read as a guarantee)?** Named directly in the field's own doc comment ("HOST-MONITORED ONLY... not
   an engine guarantee") and the accessor's comment — a text-level mitigation, not a structural one,
   because there is no structural enforcement to point to; this is the honest limit of a
   documentation-only fix and is named as such rather than oversold.

No FATAL finding. No design change resulted from this pass beyond the already-recorded, found-during-
implementation deviation in §5b (a query pair instead of a callback+timer, because no wall clock is
wired — discovered while implementing, not anticipated by the original plan handed to this ADR).

## 7. The decision

**Design B is adopted and implemented**, per §5's evidence. It binds:
- `007-Capability-and-Trust-Model.md` — no change to its own text; this ADR is the record that I2/I3
  were checked against and left intact (§3 Design C, §4a).
- `decisions/ADR-029-suspend-for-human-approval.md` §6 — amended in place (§5b).
- `decisions/ADR-066-context-provider-attribution-provenance.md` §7 — amended in place (§5d).
- `OpenQuestions.md` OQ-19 — **not resolved, not implemented.** This ADR names the pattern's answer
  to how OQ-19 would eventually resolve (a per-executor binding-time grant, attenuation-bounded by
  the workflow's own ceiling — never principal-derived/ambient, matching `docs/planning/agent-as-
  workflow-executor-design-draft.md`'s own second-pass direction) as a cross-reference, but the
  project owner's explicit, dated "document only, do not implement yet" instruction (OQ-19, 2026-08-
  13) stands unless and until the project owner separately lifts it.

**Full-tree evidence.** `ninja` (all targets, Windows/MSVC): zero compile errors. Full `ctest`:
**208/218 passed, 10 not-run** — `test_context_provenance`, `test_turn_middleware`,
`test_content_replay_gateway`, `test_secret_quarantine`, `test_rt_agent_session_turn_middleware`,
`test_rt_agent_session_content_replay`, `test_rt_agent_session_context_provenance`,
`test_rt_agent_session_quarantine_tool`, `test_rt_agent_session_turn_and_replay_composition`,
`test_tool_optimizer_provider` — the SAME 10 pre-existing, unrelated not-run targets ADR-066/067/068/
069 already document: confirmed via `ls` that none of these ten `.exe` files exist anywhere under
`build/tests/` even after this pass's full `ninja` (all targets) run — a pre-existing build-
configuration exclusion (matching the memory of this being CPython-embedding/PR-branch executables
never produced by this build), not a regression this ADR caused. Several of the ten DO include
`agent_session.hpp` transitively (confirmed by grep) — the reason they're skipped is that their
target is excluded from this configuration's build graph entirely, not that this ADR broke their
compile; a full `ninja` run that DID compile every changed header (`tool_pipeline.hpp`,
`agent_registry.hpp`, `agent_session.hpp`, `workflow_supervisor.hpp`, `graph.hpp`) against the whole
tree returned zero compile errors, which is the actual claim this evidence supports. Zero
regressions in any of the 208 tests that DID run, including the full pre-existing `policy_driven`/
approval/suspend/context-provider/workflow suites this ADR's changes sit inside.

**Explicitly out of scope, named rather than left implied:**
- 007 §5's full declarative policy DSL — still unbuilt; `PolicyDecider` is an evolutionary seam
  compatible with it (a future DSL-interpreting decider could be one more `PolicyDecider`
  implementation), not a substitute for it.
- Real engine-side enforcement of `TerminationBound.token_budget` — would need per-executor
  usage-tracking correlation this codebase does not have; §5c's diagnostic is the bounded substitute.
- `background_task()`'s own step 5 (`tool_pipeline.hpp`) does NOT consult `PolicyDecider` — it
  already has a pre-existing asymmetry with `invoke_tool()` (it checks `tool->approval !=
  never_require` directly, ignoring `provenance`/`text_derived` declassification entirely), named
  here as a residual this ADR did not touch, not silently left looking identical to `invoke_tool()`'s
  now-extended step 5.
- Lifting OQ-19's "document only" instruction (above).

**Residual risks:**
- A host that wires a `PolicyDecider` with buggy logic (e.g. always `auto_approve`) has fully
  replicated `never_require` for a `policy_driven` tool by its own choice — this is the intended
  shape of "responsibility transferred to the consumer dev," not a defect; §4's five properties are
  what keep this bounded (the host can only ever resolve calls to a tool it already holds capability
  for, never grant new capability), not a claim that a host cannot misconfigure its own policy.
- `set_interaction_expiry`/`expired_interaction_ids` are plain, unlocked methods (matching every
  other `set_*` session-configuration method on `AgentSession`, per I1's "one session, one executor"
  contract) — a host driving one session from more than one concurrently-arriving caller without the
  Tier-3 `require_authority_` admission path is already outside this class's documented safety
  envelope, unchanged by this ADR.
- `token_budget_unenforced()` is a diagnostic a host must actively check; nothing in this codebase
  currently calls it automatically (no default logger, no assertion) — a host that never reads it is
  in exactly the same position as before this ADR, just with a query now available rather than none.
