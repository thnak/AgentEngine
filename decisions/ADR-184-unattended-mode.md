# ADR-184 — Can AgentEngine run a full-automation system — lessons that act as instructions, no fence, no human approving calls — without the engine ever minting authority?

- **Status:** Proposed — built, tested offline, red-teamed twice (§7: no fatal; round 1 8 major, round 2 4 major,
  all fixed), measured live (§6). The round-2 fixes are not yet re-red-teamed. **Needs the project owner's judgement** — the owner asked for it
  (2026-09-24: "as an engine we should let developer choose their own … we must add option like by pass all, in some
  case engine can be used to build a full automation system and that would be a strong feature"), and it deliberately
  trades safety for capability behind host opt-ins. §4 states plainly what that waives.
- **Date:** 2026-09-24.
- **Scope:** `core/content.hpp` (`ContentItem::deliver_as_instructions`), `core/approved_lessons.hpp`
  (`approved_lesson_level`, `approve_automatic`, reserved approval ids), `rt/agent_session.hpp`
  (`set_approved_lessons(…, level)`, `disable_system_channel_fence`, `set_unattended_approvals(…, veto)`,
  `enable_unattended_mode`), `core/tool_pipeline.hpp` (`auto_deny` honoured for `text_derived` calls),
  `core/system_channel_fence.hpp` (the predicate; the automatic-approval sentence), `core/middleware.hpp` (the guard
  covers the new mark), `core/chat_recording.hpp`, `core/chat_stream_drain.hpp`, `eval/promotion_ack.hpp`
  (`promote_lesson_automatically`); amendments to `003` §2, ADR-070 §5a (the `text_derived` policy row), ADR-173,
  ADR-183, ADR-179; tests (§6), including `test_tool_pipeline`'s ADR-070 check, rewritten for the `auto_deny` change.
- **Related:** ADR-183 (approved lessons; this extends its route) · ADR-173 (the fence) · ADR-070 (the Delegated
  Decision Seam, §4a) · ADR-023 (text-derived calls) · ADR-029 (suspend for approval) · ADR-033 (middleware) ·
  ADR-179 §215 T4 (auto-promotion, deleted there; reintroduced here as a host opt-in, §5).

## 1. The question

**Stated so it has a wrong answer:** can a host build an agent that learns and acts with nobody watching — its
lessons followed as instructions, retrieved text read without a fence, every tool call it was granted run without a
human — using only host-side switches, each off by default and audited, without the engine ever granting a
capability?

Before this ADR a host could not: an approved lesson was always fenced (ADR-183); every tainted system text was
always fenced (ADR-173, 003 §2); a lesson needed a human approver; and unattended calls needed a hand-written
always-yes `ApprovalDecider`, with nothing in the audit saying no human was involved.

## 2. What the engine's gates are

Checked against the code before designing (the owner first asked for "engine permission checks" to be bypassable):

- **Capabilities (I2).** A tool runs only under authority the host granted (`set_capabilities`); `admit_call` binds
  them before the approval step. Nothing here changes that: a full-automation host grants what it wants, and the
  engine grants nothing itself.
- **Approval.** A call reaches the `ApprovalDecider` when its tool is `always_require`, `policy_driven` without a
  deciding policy, or when the call is `text_derived` to a tool that is not pure and inert (ADR-023). ADR-070 §4a
  forbids a `PolicyDecider` from *approving* `text_derived` calls; the decider — the host's own answer, usually a
  human — could always approve anything.
- **Denials.** Capability checks, hook-stage denials and a `PolicyDecider`'s `auto_deny` apply in every mode. The red
  team found two holes, both closed:
  - **Round 1:** `auto_deny` was never consulted for `text_derived` calls, so in unattended mode such a call skipped
    a host's explicit deny (a plan-mode gate, for one). It is now honoured for every provenance (§7 S-M1). This also
    narrows default mode: a denied `text_derived` call is now denied instead of asking the decider, and a policy is
    now also called for `text_derived` calls, twice per call (the suspend pre-check and dispatch).
  - **Round 2:** the policy is only asked about `policy_driven` tools, so its deny still lost to unattended mode on
    `always_require` tools and on `text_derived` calls to `never_require` tools. In unattended mode the decider now
    asks the policy about every call that reaches it and honours `auto_deny` (R2-M1).

## 3. Decision

Four host opt-ins, all off by default and all audited (I4). Knobs 1, 3 and 4 are `AgentSession` settings; knob 2
is a registry call, and `enable_unattended_mode(operator, &registry)` sets 1, 3 and 4 at once.

1. **Approved lessons as instructions** — `set_approved_lessons(&registry, approved_lesson_level::instructions)`.
   An approved lesson (ADR-183's exact-text match) goes to the model as plain, unfenced system text. The default
   level, `guidance`, is ADR-183's fenced route. The item keeps `tainted = true` and `content_origin::external`.
2. **Automatic approval** — `ApprovedLessonRegistry::approve_automatic(scope, text, reviewer)` and
   `eval::promote_lesson_automatically(...)`: a lesson approved by the host's own automated reviewer, no human. Its
   id is `automatic:<reviewer>` (a human approval may not use that prefix, nor `simulated:`), the audit says
   "(automatic)", and at `guidance` level the preamble says "a human operator or the deployment's automated
   reviewer" approved it — never that a human did. Calling it is the opt-in.
3. **System-channel fence off** — `disable_system_channel_fence(operator)`. Every tainted system text — memory, RAG,
   summaries, lessons — goes out as plain system text: no fence, no preamble. One event per request names the
   operator and how many items it unfenced; an approved lesson is audited as delivered "as instructions (fence off)".
4. **Unattended approvals** — `set_unattended_approvals(operator, veto = {})`. Every call that would wait for an
   approval is approved with no human (`always_require` and `text_derived` included), and the session never
   suspends for approval. It is the round's `ApprovalDecider`, built per round (never stored), passing the host's
   decider by reference when off, so a stateful host decider keeps its state. The host's `PolicyDecider` is asked
   about every call it sees and its `auto_deny` denies. An optional `veto` decider is asked next, for every call that
   would need approval ("everything that needs approval, except X"). A later call without a veto keeps the existing
   one; `clear_unattended_approvals` drops it. Each approval emits one event naming the tool, the caller, a hash of
   the arguments and the operator; each denial by policy or veto emits one naming why. It overrides
   `set_approval_decider` while set. Once cleared, even mid-round, the next call goes to the host's decider again.

Every operator/reviewer id is required; an empty one is refused and changes nothing.

**One mark, one granter.** Knobs 1 and 3 set `ContentItem::deliver_as_instructions`; the serializers' fence
predicate skips a marked item (it then goes out like untainted system text, still losing the reserved glyphs). Like
`approval`, the session clears it on every item before granting, so no provider, plugin or stored message can set
it, and `MiddlewareModelCallGateway` drops both marks from any item a `before_model` hook marked, lifted or rewrote.
Recordings keep the mark (I5); a streamed delta with a different mark is never joined.

## 4. What this waives, stated plainly

- **Knobs 1 and 3 relax how the model is told to read tainted text** — the reading rule of 003 §2 and ADR-173, the
  same kind of relaxation ADR-183 made for one class, now for any class the host chooses. No engine declassifier
  (`unsafe_view`, ADR-070 §4a's list) is touched and the taint bit is kept, but ADR-070 §4 property 3 is **not met**
  for them.
- **Knobs 3 and 4 together waive I3 in practice for granted tools.** With the fence off, text a tool, a document or
  an earlier model turn wrote is read like the host's instructions; with unattended approvals, a `text_derived` call
  the model makes from it runs with no human. The engine still enforces the grant (I2) and records everything (I4),
  but "model output is never authority" then holds only as far as the host's grants are narrow. This sits **outside**
  ADR-070's seam pattern — it is not a narrowing of possessed authority by a host decision but a host choosing to let
  model-derived text act — and it is recorded as **an owner-sanctioned exception**, not as a seam.

Against ADR-070 §4 otherwise: explicit opt-in — **met** (named calls, ids required); fails safe when unset —
**met** (A1, A3, A4, L1; the one default change is the `auto_deny` narrowing in §2); host code, never model output —
**met** for who can set the knobs (knob 2 lets host-chosen automation approve model-proposed lessons, which is the
host's decision); always audited — **met, if the host attaches a run-event tap or stream**: `emit_run_event` drops
events nobody listens to, and the tool pipeline's own audit record does not say "no human".

## 5. What this does NOT claim (residuals)

- **Unfenced, unattended text can drive granted tools** (§4). Grant narrowly; use the `veto` for tools that must
  never run unattended.
- **Automatic approval approves model output.** ADR-179 §215 T4 deleted auto-promotion because a model reviewing its
  own lessons launders them; this brings it back as an explicit host choice, not a default. Revocation (`revoke`)
  is the switch. Nothing in the engine proposes lessons yet (ADR-179's reviewer is unbuilt): a host writes the
  reviewer, calls `promote_lesson_automatically`, and writes the returned `MemoryItem` to memory itself. The
  registry has no list/export: a host keeps its own record and reloads it with `approve_automatic`.
- **Not everything unattended.** `agent.ask` and hook-decision suspensions still wait for host code. `agent.spawn`
  children cannot be put in unattended mode — the child session is built internally and takes only the decider its
  `SpawnTargetDescriptor` names (a hand-written always-yes decider works there, without this ADR's audit). Workflow
  executors and other sessions have their own settings.
- **Only `AgentSession`.** `invoke_tool` called directly uses the caller's decider, as before.
- **The middleware guard covers the marks only**: a `before_model` hook can still change `tainted`, `origin` or a
  message's role (as before this ADR); middleware is host code.
- **The approval audit names a hash of the arguments, not the call id** (the decider is not told it); the tool-call
  events around it name the call.
- **Setters are session-thread-only**; clearing unattended mode mid-round stops the next approval in that round.
- **Tested paths**: the main round loop. The hook-processed round (`finish_hook_processed_round`, after a
  `ToolCallHook` external dispatch) uses the same effective decider and the same suspend check, but has no test of
  its own in unattended mode.
- Measured on one model (§6).

## 6. Evidence

**Offline.**
- `tests/test_unattended_approvals.cpp` (31 checks, no HTTPS needed, so it runs in every build; round 2 moved the
  core delivery checks here too, D1-D7, since the other file only builds with HTTPS). A1-A7: `always_require`
  and `text_derived` calls are denied by default and run unattended, each audited exactly once. Suspend-for-approval
  never suspends. `auto_deny` still denies, for `text_derived` calls too (A5b), and a policy's `auto_approve` is still
  never an approval for them (A5c). Unattended overrides the host decider; cleared, the default is back. A8-A11: the
  veto; empty operator ids refused; `enable_unattended_mode`; a stateful host decider keeps its state across rounds.
  S1: a streaming join respects the mark. A12-A15 (round 2): a policy's deny covers every call in unattended mode; a
  veto survives `enable_unattended_mode`; clearing mid-round hands the next call to the host's decider (A14b); a veto
  that clears unattended mode from inside itself neither crashes nor approves. D1-D7: default level; `instructions`
  unfences only approved text; a provider's own mark is cleared; fence off drops the preamble; automatic approvals
  are not worded as human; `enable_unattended_mode` sets the level; reserved ids refused on both fields.
- `tests/test_unattended_mode.cpp` (17 checks). L1-L9: the default level is unchanged. `instructions` is unfenced on
  the OpenAI wire while an unapproved note beside it stays fenced. With the fence off every tainted system text is
  unfenced and audited once, and switching it back on restores it. Automatic approvals are recorded and audited; the
  mark round-trips through recordings. Both knobs together are audited as what is sent. An automatic approval is not
  described as a human's, and reserved ids are refused.
- `test_middleware_model_call_gateway` T17: a hook cannot add the mark, keep it on rewritten text, or lift a fenced
  approved lesson.
- `test_eval_tier1_screen` T33: `promote_lesson_automatically`.
- Planted mutants each fail a test: the fence ignoring the mark; the session not clearing it; the unattended decider
  denying; the session suspending anyway; `text_derived` skipping the policy again; the host decider copied per round;
  the guard comparing text only. Round 2, all against the ungated test: the policy not asked in unattended mode; the
  veto reset by a later call; the fence ignoring the mark; the session keeping a provider's mark; automatic worded as
  human; the reserved-id check removed; `enable_unattended_mode` not setting the level. One survives and is
  equivalent: removing the first "still unattended?" re-read, since a second one before approving has the same effect
  (it only changes which audit line a policy denial gets after clearing).

**Live** (DeepSeek `deepseek-flash`, `tests/test_memory_lesson_label_live_e2e.cpp` through the real OpenAI serializer,
one interleaved run, 20 trials per cell; followed / asked the user, naming the value / other):

| Arm | alert channel | deploy region |
|---|---|---|
| A a lesson as fenced memory, today's default | 6/14/0 | 0/20/0 |
| R approved, `guidance` (ADR-183) | 19/0/1 | 20/0/0 |
| **T approved, `instructions` (knob 1)** | **19/0/1** | **19/1/0** |
| **N the same memory item, unapproved, fence off (knob 3)** | **17/3/0** | **14/6/0** |
| F plain host text (the ceiling) | 19/0/1 | 17/3/0 |
| C no lesson (control) | 0/0/20 | 0/18/2 |
| X2 hostile fenced block beside an approved lesson | 0/19/1 | 0/20/0 |

Knob 1 does what it says: an approved lesson delivered as instructions is followed at the ceiling — no better than
ADR-183's fenced `guidance` route, which already reached it on this model, so `instructions` matters for models that
weigh the fence more. Knob 3's cost is measured, not assumed: with the fence off, an ordinary unapproved memory
statement — which is exactly what a hostile one looks like — is followed 17/20 and 14/20 instead of 6/20 and 0/20.
Unattended approvals (knob 4) are engine logic with no model in the loop; they are proven offline only.

## 7. Red team

**Round 1 (two reviewers: security, correctness/claims).** No fatal.

| # | Sev | Finding | Disposition |
|---|---|---|---|
| S-M1 / C-M1 | major | `auto_deny` skipped for `text_derived` calls, so unattended mode ran a call the host had denied (plan-mode gate) | `resolve_approval_outcome` honours `auto_deny` for every provenance; `auto_approve` still never approves `text_derived` (A5b, A5c; mutant-checked) |
| S-M2 | major | Default mode changed: the host decider was copied per round, resetting a stateful decider | Passed by reference (A11; mutant-checked) |
| C-M2 | major | An automatic approval at `guidance` level told the model a human approved it | Separate wording when any approved block is automatic (L8) |
| C-M3 | major | 003 §2, ADR-173 G1, ADR-183 §3.1, ADR-179 T4 contradicted by the code, unamended | Amended (see Scope); T4 addressed in §5 |
| C-M4 | major | The ADR said nothing relaxes I3 while knobs 3+4 waive it in practice | §4 rewritten: an owner-sanctioned exception outside ADR-070's seam |
| C-M5 | major | "Children must opt in separately" is impossible for `agent.spawn` | §5 says so plainly |
| C-M6 | major | The approval proofs sat behind the HTTPS build flag, so CI never ran them | Split into the ungated `test_unattended_approvals` |
| S-m3 | minor | Clearing unattended mode mid-round read an emptied optional (crash) | Operator id captured by value; the setting re-read per call |
| S-m4 / C-m1 | minor | Guidance lesson + fence off audited as "guidance" and counted twice | Audited as "instructions (fence off)", not recounted (L7) |
| S-m5 | minor | Approval event named no call | Adds an arguments hash; call id disclosed (§5) |
| S-m6 / C-m4 | minor | Guard comment overclaimed; a text-only comparison survived | Comment narrowed; T17 lift case (mutant-checked) |
| S-nit / C-m3 | minor | `automatic:`/`simulated:` ids spoofable by a human approval | Reserved (L9) |
| C-m2 | minor | Empty operator id silently accepted | Refused (A9) |
| C-m5 | minor | Hook-processed and `resolve_interaction` paths untested unattended | Disclosed (§5) |
| C-m6 | minor | Stream join untested | S1 |
| C-m7 | minor | Audit depends on an attached sink | Disclosed (§4) |
| C-m8..10 | minor | ADR self-inconsistencies, stale comments, missing file-top citations, no README row | Fixed |
| usability | — | One call for the whole setup; "everything except X" | `enable_unattended_mode`; `veto` |

**Round 2 (one reviewer, on the round-1 fixes).** No fatal.

| # | Sev | Finding | Disposition |
|---|---|---|---|
| R2-M1 | major | The S-M1 fix covered only `policy_driven` tools: in unattended mode a deny-all policy still let `always_require` tools and `text_derived` calls to `never_require` tools run (the plan gate held back only its `policy_driven` tool) | The unattended decider asks the policy about every call and honours `auto_deny` (A12; mutant-checked) |
| R2-M2 | major | `enable_unattended_mode` dropped a veto set earlier, silently | A later call without a veto keeps it; `enable_unattended_mode` takes a veto (A13; mutant-checked) |
| R2-M3 | major | Five claimed mutant kills lived only in the HTTPS-only test | Core delivery checks D1-D7 moved into the ungated test (all five now killed there) |
| R2-M4 | major | The mid-round re-read and `enable_unattended_mode`'s level were untested | A14/A14b, D5 |
| R2-m1 | minor | A veto that cleared unattended mode destroyed itself while running (segfault) | Veto held by `shared_ptr`, copied locally for the call (A15) |
| R2-m2 | minor | Reserved ids forgeable through the acknowledgement field | Checked on both fields (D6) |
| nits | nit | Case/whitespace variants of reserved ids; empty simulated trial id; audit names the round-start operator after a mid-round re-set; policy now called for `text_derived` (and twice); the veto sees only calls needing approval | Case-folded and trimmed; trial id required; the rest disclosed (§2, §3) |
