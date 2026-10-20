# ADR-234 — The Harness composition point: one opinionated default stack over the run bridge

**Status:** **Proposed (2026-10-01).** Design (§2–§4), two adversarial self red-team rounds (§5: round 1
against the design, 7 findings; round 2 against the working code, 9 findings — 9 fixed in total, the rest
disclosed), implemented and proven (§6). Judge is the project owner's.

**Relates to:** GitHub issue #60 (the follow-on #56 named). Composes ADR-166 (`TodoProvider`), ADR-167
(`PlanExecuteMode`), ADR-168 (`run_with_bounded_reflection`) on ADR-226 (the run bridge). ADR-070 (Delegated
Decision Seam; "default to enabling" without relaxing I2/I3), ADR-066 (context attribution), ADR-074/116
(move-only context slots), ADR-192 (unattended mode), 002 §2/§3/§9 Q2, 005 §3/§4/§5. Research:
`docs/research/2026-10-01-agent-harness.md` (MAF's .NET and Python harness source, read for this ADR).
New: `include/agentengine/core/harness.hpp`, `tests/core/agent/test_harness_session.cpp`,
`examples/35_harness_agent.cpp`. Changed: `core/agent_session_bridge.hpp` (an `Inner` parameter, a
descriptor filter, the typed-validator helper — all in `agent_bind_detail`), `core/session_builder.hpp`
(`Bundle::drive()`), 002 (§2.2 new, §3 note).

## 1. The question

MAF turns a bare chat client into a fully loaded agent with one call — `chatClient.AsHarnessAgent()` (.NET),
`create_harness_agent(client)` (Python) — with a fixed default stack, each piece individually switchable.
Every piece AgentEngine has (todo list, plan/execute gate, bounded reflection, approval, compaction, skills,
background, telemetry) is opt-in, wired one provider/decider at a time. Stated so it has a wrong answer:

> What one call turns a declared agent into a session carrying an opinionated default stack, such that every
> piece is individually removable, every default is actually active, and the session holds no authority the
> caller did not pass?

## 2. What MAF does (from the source, research §2)

Both MAF factories are construction-time conveniences over ordinary agents, providers and decorators — not a
new agent type. Defaults: todo, plan/execute mode, file memory, standing-approval decorator, OpenTelemetry and
web search on; compaction only when token limits are given; looping only with an evaluator; file access,
background agents, shell opt-in; skills **on in .NET** (scanning `Directory.GetCurrentDirectory()`) but
**opt-in in Python**. Default file memory writes under `{cwd}/agent-file-memory`. Harness instructions go
before the agent's.

## 3. Shape: builder-time convenience on the bridge, not `HarnessAgent<A, Overrides...>`

**Steelman of `HarnessAgent<A, Overrides...>`.** It reads like MAF's one-liner and like the rest of 002: `struct
Writer : HarnessAgent<Writer, Tools<...>>` and the harness is part of what the agent *is*; overrides would be
tags (`NoTodo`, `NoPlanGate`), checked by `register_agent<A>()`, and 002 §9 Q2's narrowing rule would apply to
them for free. Compile-time selection, no runtime branch — 002 §2's own idiom.

**Steelman of the builder.** Several pieces are not tags and cannot be: a reflection evaluator is host code, a
telemetry sink is a host callback, skill sources are host data, the plan gate's "planning-safe" predicate is
host code (ADR-070's seam), a `Summarize<N, S>` history needs a summarizer client. A builder takes those as
values; a template would need a second, runtime channel for them anyway.

**Decision: the builder**, as `make_harness_session<A, HistoryT>()` / `bind_harness_session<HistoryT>()`, the
exact twins of ADR-226's `make_agent_session<A>()` / `bind_agent_session()`, running the same
`agent_bind_detail::bind()`. Reasons, each checked against the code:

1. **002 §3's rule** ("a knob belongs here only if it changes *what the agent is*"). Whether a deployment
   counts events, parks approvals for a human, or re-runs until a host evaluator is satisfied is *how the
   deployment runs* the agent — the same agent declaration is run with and without a harness. Only the plan
   gate is arguably "what the agent is", and ADR-167 already decided against a `Mode<PlanExecute>` tag.
2. **The compiler.** `register_agent<A>()` compiles a fixed policy table into `AgentMetadata`; a
   `HarnessAgent<A, ...>` alias would either have to make every harness piece a new `AgentMetadata` field
   (and a 015 YAML key, I6) or be a second template that the compiler does not see. Neither is smaller than
   a function over the metadata the bridge already binds.
3. **I6 for free.** The builder works over any `AgentMetadata`, so a 015 document compiled by
   `compile_agent_document()` gets the identical harness (test H12). A template exists only on the native
   surface.
4. **No third construction path.** ADR-226 just replaced hand-built sessions with one bridge. The harness *is*
   a bridge session — every ADR-226 guarantee holds because the same function runs — plus pieces placed into
   the one context slot (`AgentToolSurface<Inner>`, `Inner = HarnessContextProvider`) and the session's
   existing setters.

To compose rather than fork, the bridge gained three internal seams (all in `agent_bind_detail`, none on a
public signature): an `Inner` template parameter for `AgentToolSurface<Inner>` (`AgentBundleWith<C, Inner>`;
`AgentBundle<C>` is unchanged), a `DescriptorFilter` applied after the agent-level floors, and
`output_validator_for<A>()`. `Bundle` gained `drive(fn)` — `fn(session)` under `ask_mutex_` and
`rt::block_on()`, so the harness's reflection loop and `resolve()` serialize with `ask()` instead of
bypassing it through `session()`.

## 4. The pieces: default on or off, decided against I2 and ADR-070

ADR-070 says default to enabling, through a seam that is host-opted, fails closed, narrows only, is host
code, and is audited — and is **not** a licence to relax I2/I3. Each piece is decided on that test: a
default may be ON only if it needs no authority the caller did not pass and can only narrow or delay.

| Piece | Default | Why | Off / on switch |
|---|---|---|---|
| instructions | **ON** | Engine-authored constant text on the untainted static channel, before the agent's (MAF's order). No authority. | `disable_harness_instructions`; `harness_instructions` replaces the text |
| todo (ADR-166) | **ON** | Five session-local, capability-free, `never_require` tools; bounded (200 items × 500 chars); adaptive (no context cost until used). | `disable_todo` |
| plan_execute (ADR-167) | **ON** | Only *denies* (auto_deny before the plan) and gives back each tool's declared behaviour after; opens only on a real `plan_ready` that re-checks a real `todos_add` (I3). Needs todo. | `disable_plan_execute`; `is_planning_safe` (host code) widens what runs while planning |
| approval | **ON** | A call that needs approval *parks* the run for a human (`run.suspended_for_approval`, continued by `resolve()`) instead of being denied. Approves nothing itself; a parked call never runs unless a human/host resolves it. | `disable_approval_suspension` (the bridge's deny-and-continue) |
| telemetry | **ON** | In-memory counters over the run-event stream; no I/O. Capped by the agent's own `Telemetry<Capture>`: `none` turns it off, `metadata_only` strips payloads before a sink sees them. A host sink is optional. | `disable_telemetry`; `telemetry_sink` |
| compaction | **OFF** | `Window<N>` drops content, `Summarize<N, S>` rewrites it with a second model call — losing or rewriting a conversation by default is 005 §4's "compaction that drops content" defect, and MAF also enables it only on request. | `HistoryT` (`HistoryProvider<Window<N>>`, `HistoryProvider<Summarize<N, S>>`) |
| skills | **OFF** | MAF .NET's default scans the process's cwd — file-system reach nobody passed (ambient authority, I2). Enabled only with a host-constructed `SkillsProvider<>` over host-chosen sources; advertisement only — a skill unlocks no tool (structurally enforced, §5 S1); reading a skill's files still needs a host-granted `cap::FsRead` through the narrowed grants. | `skills` (a `ContextProviderDescriptor` named "skills") |
| reflection (ADR-168) | **OFF** | Needs an evaluator, which is host code; there is no honest default (a model-judge default would be an uncounted second model call deciding on model output). Bounded by iterations **and** by default by the agent's per-run `TokenBudget` in aggregate (§5 R5). | `reflection_evaluator`, `reflection_max_iterations`, `reflection_max_total_tokens` |
| background | **not a piece** | `schedule_wakeup` is offered only when the session holds `cap::Schedule` — the caller's grant narrowed to the agent's ceiling (ADR-226). The harness grants nothing, so it neither turns this on nor off (H8). `start_background_task()` stays a host-only API. | the caller's grants |

Not built, deliberately: MAF's default file memory (cwd-rooted file writes — I2), default web search (no such
tool exists; it would need `cap::NetOut` the caller did not pass), and MAF's "don't ask again" standing
approvals (a remembered *human* decision is legitimate, but it needs a resolve-time "always" flag that
`ResolveInteraction` does not have; a residual, R-A below).

**Plan-gate coverage (red-team R1, the substantive design point).** ADR-167's decider is consulted only for
`policy_driven` tools, and `Tool<>`'s default approval is `never_require` (`tool.hpp` `declared_approval()`) — so
a default harness wired exactly as ADR-167's header suggests gates *nothing* a typical agent declares. The
harness, being the composition point, closes that: the `DescriptorFilter` raises each **agent-declared**
`never_require` tool that is not planning-safe to `policy_driven` (stricter: it now reaches a decider), and
the harness decider returns `auto_approve` for exactly those tools once the gate is open — their declared
behaviour, never more (ADR-070's `auto_approve` is never an approval for a `text_derived` call, which is the
same approval a `never_require` text-derived call already needed). Tools the agent declared `policy_driven`
go to the host's own `PolicyDecider` after the gate, or `require_approval` when there is none (= no decider,
ADR-070). The harness's own tools come from the context slot, not the declaration, so planning is never gated.

## 5. Red team

### Round 1 — against the design (before code)

- **R1 (FIXED, §4).** The default gate gated nothing (above). Positive control: H3 — `act` (never_require) is
  `denied by policy` before the plan and runs after; mutant M1 (no raise) killed.
- **R2 (FIXED, by construction).** Raising every non-planning-safe tool would raise `todos_add`
  (`at_most_once`, so not planning-safe by ADR-167's bar) and deadlock planning. Only agent-declared
  descriptors pass through the filter. H3 checks `todos_add` runs while the gate is closed.
- **R3 (FIXED).** `assemble_context()` skips a failing contributor. In a harness that means a failing
  `Summarize` history sends the model a round with no conversation, and a failing plan piece drops the gate's
  guidance while its decider still denies. `HarnessContextProvider` records each piece's error and fails the
  round (`harness_context.piece_failed`, naming the piece's own code). H9: zero model calls; the control shows a
  plain `ComposedContextProvider` returns success for the same failing provider. Mutant M3 killed. It also
  refuses a piece set with no `history` piece (`harness_context.no_history`; M13 killed).
- **R4 (FIXED).** The decider recognises a gated tool by name. `schedule_wakeup` is the one tool the engine
  injects into a round itself; a declared tool of that name would put the engine's tool under the post-gate
  `auto_approve`. Refused at bind while the gate is on (`harness.tool_name_collision`); declared names
  shadowing a harness tool are refused at bind too (instead of at the first round by `AgentToolSurface`).
  H10; mutant M9 killed.
- **R5 (FIXED).** ADR-168's loop resets the per-run `TokenBudget` every iteration, so `max_iterations = 3`
  meant up to 3× the agent's declared budget. The harness's default aggregate bound is the session's effective
  per-run budget — a reflective run spends no more than one run of the agent may. An explicit host value
  replaces it (host code). H7; mutant M7 killed.
- **R6 (FIXED).** The telemetry tap runs inside the round on the session's emit path; a throwing host sink
  would fail (or, on the streaming thread, terminate) the run. Contained and counted (`sink_failures`). H5;
  mutant M6 killed (the uncaught throw ends the test process).
- **R7 (design).** `Telemetry<none>` on the agent with the harness's default telemetry ON: the agent's
  declaration wins (piece off). A sink passed for such an agent is refused rather than silently never called
  (`harness.telemetry_sink_unused`). H5/H10; mutant M11 killed.

### Round 2 — against the working code

- **S1 (FIXED).** The `skills` slot is recognised by contributor name, which host code chooses: a tool-bearing
  provider named "skills" would put undeclared tools in front of the model through the slot documented as
  "advertisement only". Now structural: a skills contribution carrying a tool fails the round
  (`harness.skills_contributed_tools`). H11; mutant M8 killed. (Such tools would still have needed held
  capabilities — this closes a confusion, not an I2 hole.)
- **S2 (FIXED, build).** `harness.hpp` first included `skill_provider.hpp`, whose `SkillsProvider` links
  `agentengine::worktree_store` (WIN32-gated in this tree) — every harness user would have needed that library.
  `HarnessOptions::skills` is now a type-erased `ContextProviderDescriptor`.
- **S3 (checked, holds).** Does the harness change any authority? The session's `CapabilitySet` is built by the
  bridge from `opts.grants` alone; the harness never touches `opts.grants` or `chat_client_grants`. H8: a
  no-grant harness session holds 0 capabilities and an ungranted declared tool is still `capability not held`
  after the gate opens; for the same grants the harness and the bridge hold the identical narrowed set;
  `schedule_wakeup` appears only with the caller's `cap::Schedule`. Mutant M12 (append the ceiling to the
  grants) killed.
- **S4 (checked, holds).** Approval suspension and deciders: `approval_waits_for_human()` is false whenever an
  `ApprovalDecider` (`approve_tools`) or unattended mode is set, so suspension never overrides a host decision.
  The plan decider's `auto_deny` is a denial, not a park — the gate is not bypassed by a human approving a
  pre-plan call (H3 a1 is denied with suspension on). Mutant M4 killed (H4).
- **S5 (checked, holds).** ADR-192 unattended mode asks the `PolicyDecider` about every call that reaches the
  decider and honours `auto_deny` — so in unattended mode the gate also holds back `always_require` tools before
  the plan: stricter, not looser.
- **S6 (residual).** The telemetry piece uses `AgentSession::set_run_event_tap()`, a single slot.
  `rt::agent_session_as_executor_body()` (a workflow node) sets its own tap around each run and resets it to a
  no-op afterwards, and a host calling `set_run_event_tap()` replaces it — either silently ends the harness's
  counting. Disclosed; a multi-tap seam is an `AgentSessionCore` change outside this ADR.
- **S7 (residual).** The plan gate opens once per session and never closes (ADR-167's `GateState`); a second,
  unrelated task in the same session runs ungated. MAF's mode provider switches both ways. Not changed here.
- **S8 (residual).** `always_require` tools are not held by the gate outside unattended mode (the decider is
  never consulted for them). With the approval piece on, a human must approve each such call, so a human is the
  gate; the harness does not raise or lower them.
- **S9 (residual, inherited).** `AgentToolSurface` has no ADR-116 owner tag, so `a.history_provider() =
  std::move(b.history_provider())` (host code) moves `b`'s live harness pieces (todo state, gate) into `a`. Same
  for every ADR-226 session; not reachable from model output.

Other residuals: **R-A** no standing "always approve" (needs a `ResolveInteraction` flag); **R-B** `ask_stream()`
does not reflect (a loop's intermediate answers are not the stream's text); **R-C** a reflective run that parks
for approval returns `run.suspended_for_approval` and `resolve()` finishes *that* run only — the loop does not
resume (MAF's Python loop also returns the pending approval to the caller); **R-D** `Summarize<N, S>` compaction
needs a default-constructible summarizer client (`HistoryProvider<Summarize>` has no constructor taking one);
**R-E** the harness context is fail-closed by design — a skills source that fails to load fails every round,
where a plain composed provider would skip it; **R-F** declarative metadata still has ADR-226 R10 (no ceiling
from YAML).

## 6. Evidence

`tests/core/agent/test_harness_session.cpp` — every claim drives a real `start_run()` (or `resolve_interaction()`)
through the harness against the strict `testing::ScriptedChatClient`, each with a positive control:

- H1 the six default pieces, in order; all-disabled = history only, no harness text, only declared tools; each
  `disable_*` flag removes exactly its piece. H2 todo active (item in live state, list in the next request,
  adaptive before first use); disabled → `todos_add` is an unknown tool. H3 plan gate (R1/R2): `act` denied by
  policy pre-plan, `look` (pure) usable, `plan_ready` refused without a todo, `todos_add` ungated, `act` runs
  after; disabled → runs at once; a host `is_planning_safe` widening honoured. H4 approval: parks, `resolve()`
  completes it; disabled → denied and continues. H5 telemetry counters; MetadataOnly sink sees stripped
  payloads, Full sees payloads; `Telemetry<none>` → off; throwing sink contained (R6). H6 `Window<1>` drops the
  older turn; default keeps it. H7 reflection until satisfied with feedback in the next request; default
  aggregate bound = `TokenBudget<50>` (stops at 60 after 2 calls); explicit bound → 3 iterations; no evaluator
  → refused / single run. H8 I2 (S3). H9 fail-closed (R3) with the `ComposedContextProvider` control. H10
  refusals (plan without todo, name collisions incl. `schedule_wakeup`, zero iterations, unusable sink, a
  `HistoryT` that is not a history). H11 a real `SkillsProvider` advertisement reaches the model and unlocks no
  tool; a non-skills descriptor is refused; a tool-bearing "skills" piece fails the round (S1). H12 I6:
  `bind_harness_session()` over the same metadata makes byte-identical requests and gates the same tools.

Result (Windows, clang 21 via the project's Debug Ninja build): **88/88 checks, `OK`**. ADR-226's bridge test,
unchanged, still **62/62 `OK`** after the bridge's internal seams were added.

Mutation check (`harness.hpp`, each mutant applied, test rebuilt and run, file restored from a copy): see §6a.

`examples/35_harness_agent.cpp` runs a `Writer` agent offline through `make_harness_session<Writer>()` with a
reflection evaluator: `publish` is refused before the plan, runs once after `plan_ready`, and the second
reflection iteration satisfies the evaluator (`ctest -R example_35_harness_agent`).

### 6a. Mutants

Each mutant applied to `harness.hpp`, `test_harness_session` rebuilt and run, the header restored from a
copy (byte-identical, checked with `cmp`): M1 no plan-gate raise (R1), M2 no post-gate `auto_approve` for gated
tools, M3 fail-open assembly (R3), M4 no approval suspension, M5 metadata-only payloads not stripped, M6 sink
exceptions not caught (R6; the uncaught throw terminates the test, rc=3), M7 no reflection token default
(R5), M8 skills may contribute tools (S1), M9 no `schedule_wakeup` collision check (R4), M10 harness
instructions dropped, M11 agent's `Telemetry<none>` ignored (R7), M12 the ceiling appended to the grants
(I2, S3), M13 no history-piece check — **13/13 killed** (1–4 failing checks each). M5, M7 and M13's first
spellings left a variable unused and failed to compile under `-Werror`; warning-clean spellings were then
killed.

## 7. Decision and spec changes

- 002 gains §2.2 ("Running an agent with the harness"), the per-piece default table, and a note under §3 that
  the harness is configuration, not a policy tag. 005/006/014 are unchanged: the harness composes 005 §5
  providers without changing their contract, adds no tool-pipeline step (006), and uses no workflow (014).
- `core/agent_session_bridge.hpp`'s public functions are unchanged; `AgentBundleWith<C, Inner>` is new and
  `AgentBundle<C>` is its `Inner = HistoryProvider<Window<0>>` instance.
