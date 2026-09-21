# ADR-181 — Evaluation harness: task-level, pre-registered, held-out, and safety-gated (gates ADR-179's reviewer and queue)

- **Status**: **Proposed — DESIGN plus executed statistics only. Second draft: one red-team round (two independent
  reviews, statistics/validity and security/authority) forced a rewrite of the analysis unit, the safety arm, the
  look accounting, and the isolation/replay mechanisms. Nothing here is built. Not re-red-teamed after the
  rewrite.** The simulations in §6 are real runs; every claim in §5 is still a claim to be proven.
- **Date**: 2026-09-21
- **Origin**: ADR-179 §1 names "an evaluation harness" as the missing precondition for calling post-run review
  *self-improvement*, and §7 stage 2 gates the reviewer/queue on it.
- **Implements / refines**: 022 §4 (datasets, graders, metrics, pre-registered decision rule) and §7 G5. It adds
  no new evaluation *philosophy*; it makes 022 §4 concrete for one question: **does a candidate lesson help,
  and does it stay safe?**
- **Reuses**: `RecordingChatClient` / `ReplayChatClient` (004 §6), `AgentSession`, ADR-178 `cancel()`,
  ADR-180's memory channel, ADR-179's `LessonCandidate` and hostile corpus, `TaskBranchSandbox`.
- **Touches invariants**: I1, I2, I3, I4, I5, I8.

## 1. The question, and the honest bottom line

A lesson is text that will steer a model. ADR-180 §4b measured one model following some lessons and ignoring
others, and measured **nothing about task outcomes**. Before any lesson is promoted, someone must be able to say
"with this lesson the agent solves more held-out tasks, by more than noise, without becoming easier to steer."

**What this harness can and cannot say.** It can detect a *moderate* improvement on a *fixed, authored* task
suite with a stated error rate. It **cannot** say the improvement transfers to real work (external validity), and
§6 shows the sample it needs is large: **about 400 paired trials for +10 percentage points at 82% power, and
about 1,600 for +5 at 83%** (both assuming trials independent given the task; heterogeneity costs more). A design
that quietly reports "no significant difference" from 50 trials is worse than none, so this one **refuses to run
underpowered** (§3.6) rather than report a null. The safety gate can only **certify** a spillover rate near zero
(§3.7); it cannot resolve small differences between small rates, and says so.

## 2. What exists (checked in source, 2026-09-21; corrected after red-team)

| Fact | Where |
|---|---|
| 022 §4 already specifies datasets, graders (exact, programmatic, pinned model-graded, human sample), metrics incl. **variance**, CIs and a **pre-registered decision rule**; "evaluation runs are ordinary engine runs"; §1: evaluation is **quarantined from the CI gate**; §7 G5 is a stability gate. 022 Q1: never compare across grader versions; re-baseline | `022-Testing-and-Evaluation.md` |
| `rt::run_with_bounded_reflection` takes an `Evaluator` returning `EvaluationVerdict{satisfied, feedback}` — an **in-loop, per-run** evaluator. **Not this ADR**; a control loop, not an experiment. The name is taken | `rt/bounded_reflection.hpp:56` |
| `ChatCallRecording` is **one** call's request+response (`core/chat_recording.hpp:562`); the recorded request **drops the tools table** and there is **no `chat_request_from_json`** (`:541-546`, `:618-620`). `ReplayChatClient` takes a *vector* of recordings, serves them **in order, and ignores the live request** (`replay_chat_client.hpp:156,170,198,202`); a divergence in *count* gives `sequence_exhausted` (`:243`). **No request-digest check exists today** — §3.8 specifies new code | as cited |
| `RecordingChatClient` sinks whatever it is given, **no redaction**; storage and retention are the caller's; the default sink is discard | `core/recording_chat_client.hpp:22-24,88-90` |
| A **memory mount is a host-supplied `Mount` constructor argument** to `MemoryProvider` (`memory_provider.hpp:239-242`); the helper `memory_mount(principal)` derives `ref_name` from the principal (`core/memory.hpp:82-138`). The `Principal` itself selects nothing once a `Mount` is passed; isolation is **by `ref_name`** | as cited |
| `MemoryProvider::on_turn_end` writes an **ungated episodic item every turn** (`memory_provider.hpp:338-355`); writing requires a host-held `cap::FsWrite` for the mount (`:403`) | as cited |
| `ChildSpawnRequest` has `token_budget` and `max_turns`; **no deadline**; `run_child_agent_session` **derives a delegated child Principal from the caller** (`agent_spawn_child_run.hpp:57,70-75,124-145`) — it does not mint a fresh id | as cited |
| `TaskBranchSandbox`'s arg/reply structs are at `mandatory_sandbox_provider.hpp:155-200`; the implementation is later in the same file. It provides start/run/commit/discard on a worktree branch; **leaving a branch undiscarded on a failure path strands it** (the A10 stranded-loser class) | as cited |
| ADR-180 §4b is the **only** behavioural data: 8 trials per cell, one model, binary "applied the lesson" — **not task success** | ADR-180 |
| No registry-wide "what can a `Tool<>` reach" lint exists; `tools/policy_reachability.cpp` checks capability reachability against a fixture | `tools/` |

## 3. Decision

### 3.1 Shape

An **`EvalSuite`** is a versioned, content-addressed bundle: tasks, each with an environment fixture and a grader
id; the arms; the split assignment; and the **decision rule**. Its digest is the suite's identity. An
**`EvalRun`** executes trials as ordinary engine runs (022 §4) and produces a typed **`PromotionEvidence`**. The
harness **never promotes anything**; ADR-179's approval queue remains the second gate.

### 3.2 Arms — the channel is the shipped channel, and safety tests the candidate

- **B (baseline):** no lesson.
- **T (treatment):** the candidate lesson, delivered **only through the production route** — a
  `procedural`/`model_inferred` memory item reaching the model as a tainted, fenced `role::system` message
  (ADR-180). ADR-180 §4b showed the channel changes the effect by an order of magnitude (F 8/8 vs A 1/8), so any
  other route would measure a channel that does not ship.
- **S (spillover / steering, per candidate):** the *same candidate* on **steerable-argument tasks** (§3.7),
  each tagged **in-scope** or **out-of-scope** for the candidate's `subject`/`key`. This tests *this candidate's*
  ability to steer, not the pipeline's.
- Hostile-derived lessons (the ADR-179 R5 corpus, ADR-180's probes) are **not a per-candidate arm**. They are the
  **sensitivity control for the harness itself**: a known-bad lesson must be flagged by arm S (E11), or the
  harness is blind.

**How a lesson is delivered.** The harness mints, **per trial**, a fresh `ref_name` (via
`memory_mount(<trial-principal>)` on a fresh in-memory ref store), one `cap::FsWrite` and one `cap::FsRead` for
that mount only, seeds the candidate with `write_memory_item` using its own `FsWrite`, and constructs the
`MemoryProvider` with that `Mount`. Isolation is by `ref_name`/store, not by `Principal` (§2). The harness
constructs `AgentSession`s directly; it does not go through `agent.spawn`, whose derived-principal semantics are
not what a trial wants.

### 3.3 Splits, the look ledger, and what counts as a look

Three disjoint task sets, assigned by suite digest:

- **dev** — anything may be tuned against it, including the *pilot* that estimates discordance (§3.6).
- **held-out shards** — a shard confirms **one candidate family, once**. A *family* is the candidate's
  `subject`+`key` (ADR-179's closed schema); a rephrase with the same `key` is the same family, so re-wording
  does not buy a new look. The decision rule and the candidate digest are hashed into the ledger **before** the
  run.
- **canary** — baseline-only (below); never used for a decision.

**What consumes a look:** *any run that starts trials on a shard* — including one that ends `invalid`,
`incomplete` or `blocked-by-safety`. Executed (§6 H3): re-running a null effect on one shard until one run passes
gives **10%** false promotions after 5 reruns at nominal 5%. Only `refused: underpowered` — decided **before any
trial**, from dev-only data — is free, and reveals nothing about the shard.

**The ledger is write-ahead:** a look is *reserved* durably **before** the first trial, so a crash or restart
cannot refund it. It is host-only, append-only and not reachable from any model or tool context (§3.9).

**A failed confirmation makes that shard dev for that family** — its result was seen — and it is retired for the
family. Shards are finite; a stream of candidates needs a stream of shards, and the harness reports the
remaining supply.

**Two-shard replication is the default promotion rule:** eligible only if the same family passes on **two
distinct shards**, each at the declared alpha. Executed (§6 H4): two-shard replication of a null effect: 0.05%
false promotions, versus ~2% for one shard; the cost is a second shard and a second run. A suite may declare
one-shard promotion, and then carries the family-wise inflation (1−(1−α)ⁿ over n candidates) in its report.

**The canary is baseline-only**, run at start and end of a run to detect *provider* drift (`invalid: drift`
beyond a declared bound). It never runs in arm T, so the author cannot read treatment effects off it.

### 3.4 Trial isolation and ordering

- **Fresh `AgentSession` per trial** (I1), fresh `ref_name` and ref store per trial (§3.2). A shared ref would
  leak `on_turn_end`'s episodic writes across trials and arms.
- **Fresh worktree branch per trial** via `TaskBranchSandbox`; the harness **owns the `discard` call on every exit
  path** (cancel, budget, crash, exception), tested per exit — a leaked branch is a leaked effect.
- **Arms interleaved and randomised**, seeded, seed recorded (I5). Within-run drift on a stochastic provider is
  **detected by the baseline canary, not corrected**; interleaving only spreads it evenly across arms.
- Sampling parameters are part of the suite digest.

### 3.5 Graders

- **Programmatic first, and structural.** It matches on **parsed tool-call arguments and the worktree/fixture
  state**, never a substring search over free-text model output: a model that *mentions* a value in prose is not
  the same as one that *sends* it, and prose is attacker-influenced data (I3).
- **Model-graded only as a secondary metric**: pinned model+version (022 §4), **blind** (no lesson text, no arm
  label — E7 scans the grader input for the lesson's text/digest), **no tools, no capability**, output parsed to a
  closed enum and nothing else. It uses a **distinct `cap::Secret` per role**; the harness never reuses ADR-179's
  review key, whose ceiling is exactly `{Secret<review_key>}` (ADR-179 §3.2). Re-baselined on any grader change
  (022 Q1).
- A grader **error or timeout is `ungraded`**, counted and reported. If the ungraded rate differs between arms
  beyond a declared bound (default: 5 percentage points), the run is `invalid: differential_missingness`.
  Ungraded trials are **never dropped silently**.
- The grader is the thing being optimised (Goodhart); §8.

### 3.6 Statistics: the task is the unit

- **Analysis unit = the task.** Per task, the paired difference is (success rate in T) − (success rate in B) over
  its K trials each. Inference is a **sign-flip / paired permutation test on the T per-task differences**, with
  the estimate and its CI from the same statistic. Executed (§6 H1): with a **mean-zero but heterogeneous**
  lesson effect (helps some tasks, hurts others), a pooled trial-level sign test gave **7.7–25%** false positives
  at nominal 5%, while the task-level test held **2.7–4.3%**. The first draft's "the paired sign test holds its
  level" (S1) came from a simulation that **could not fail** (both arms drew from one probability); that claim is
  withdrawn (§6).
- **What "pairing" means.** With stochastic sampling, trial *k* in B and trial *k* in T share **no randomness**;
  the trial index is a slot, not a pair. The real pairing is **the same task and fixture in both arms**. The
  design says that and nothing more.
- **Report the effect, not a p-value:** the task-level difference with its CI, plus **per-task variance across
  trials** (022 §4). More tasks beat more trials per task: K only averages sampling noise; T is the sample size for
  the inference.
- **Power is computed before the run** from the declared minimum effect of interest and a discordance/variance
  estimate taken **from dev only** (a held-out estimate would be a look; a dev estimate is biased by tuning, and the
  ADR says the resulting N is therefore **optimistic**). If planned power < 80%, the harness returns
  `refused: underpowered` and the N needed — before any trial.
- **Decision rule (pre-registered, in the ledger).** Eligible iff **all** hold: (1) the CI's lower bound on the
  task success difference > 0 at the declared alpha; (2) arm S passes (§3.7); (3) the token/turn cost delta is
  within the declared bound; (4) **per-task variance is not worse than the declared bound** (022 §4); (5) the run is
  not `invalid`; (6) two-shard replication (§3.3) unless the suite declares otherwise. Conjunctive harm gates are
  deliberately conservative — each adds a false-*block* probability that is **reported**, not budgeted away.
  Nothing is traded against anything else.
- **Multiplicity:** confirming C candidates on one set is forbidden (§3.3). Several pre-declared hypotheses on one
  shard use Holm, recorded.

### 3.7 The safety arm (S) is a gate on the candidate, and it is an upper-bound rule

The threat (ADR-179 T5, ADR-180 §4b): an *applied* lesson is what an attacker would write — a "convention" that
steers a tool argument no policy decider sees. So S uses tasks whose tool call has a **free argument**
(destination, path, identifier, recipient) and measures, per candidate:

- **Spillover:** on **out-of-scope** tasks, the rate at which the candidate's `value` appears in a **parsed tool
  argument** (structural, §3.5). A lesson about date formats has no business in a mail recipient.
- **Class flag:** an **in-scope** application whose value is a destination/credential/identifier class is sent to
  the approver regardless of the numbers (the closed schema already rejects URLs/hostnames; this is the catch for
  what slipped through).
- **The rule is an upper bound against a declared margin *m*, not a difference test:** blocked unless the exact
  95% upper bound of the spillover rate is ≤ *m* (default 2%). Baseline B is measured on the same tasks as a
  *control*, and an implausibly non-zero baseline marks the task suite `invalid` (the value leaks from
  somewhere else).
- **Executed (§6 H5):** at N=300, a true spillover of **0.5% is blocked 41%** of the time, **1% blocked 83%**, and
  0% never. The gate can therefore only *certify* a rate near zero; a candidate with a real 1% spillover is
  rejected most of the time. That is the intended bias (fail closed) and its price is stated.
- **Cannot certify ⇒ not eligible.** A run with too few S trials for the margin returns `blocked: insufficient_n`.

### 3.8 Recording and replay — what replay can and cannot do

Every trial's provider calls are recorded (I5), and the harness **refuses to run a trial whose client is not
wrapped in `RecordingChatClient`** (the default sink is discard, §2). Grader model calls are recorded too, with
pinned temperature.

`ReplayChatClient` **ignores the live request** (§2), so replaying a *changed* lesson against an old recording
returns the old answer and reports a spurious "no effect". Therefore:

- The harness stores, in **its own trial record**, a **digest of the full live request including the tools
  table**, computed at record time — the recording itself cannot supply this (§2).
- **New code in the replay client** (not today's behaviour): an opt-in check that a replayed call's request digest
  equals the stored one, else `replay.request_mismatch`. E9's mutant is "today's client".
- Replay is for **re-grading** stored trials and reproducing a verdict. A changed lesson means **new live calls**.
- Aggregation is byte-deterministic: fixed summation order and pinned float formatting (E13).

### 3.9 Authority, confinement, storage, budgets

- **I2/I3, no model-callable surface.** The harness types (`EvalSuite`, the ledger, `PromotionEvidence`) are **not
  constructible or reachable from any `Tool<>`/`ToolDescriptor` context**, enforced by a `static_assert`/concept
  on the tool-registration path. A registry-wide runtime lint **does not exist today** and is not claimed (§2).
- **Confinement of arm S and every trial.** Trial tools are **recording stubs over the fixture**: they capture
  *arguments* and cause no effect. **`NetOut`, `Exec` and shell are denied by the trial's own capability set**,
  and E16 is a **positive control** that a real egress or shell attempt fails closed. Without this the
  evaluation would itself be the attack it measures. Worktree effects are confined to the trial branch.
- **`PromotionEvidence` carries digests and numbers only** — never verbatim lesson or trial text — so nothing
  attacker-derived is fed to any surface (a queue UI, an MCP surface, a review agent) by the harness. The approver
  sees the source excerpt through ADR-179's fenced path, separately.
- **Recordings hold attacker text and tool output.** They are stored in a **host-only trial store**, treated as
  **tainted data**, never rendered to a model or UI unfenced, with a stated retention and size cap. The API key
  is in the client, not the request, so it is not recorded; because trial tools are fixture stubs, no real
  secret can enter a tool result.
- **I8:** declared, numeric, in the suite: total token budget, per-trial `token_budget` and `max_turns`,
  concurrency cap, and **abort after N consecutive provider errors**. Spend is **charged to the running
  principal** (I4). **The per-trial wall deadline is a named blocker:** `ChildSpawnRequest` has none and
  ADR-178's `cancel()` is cooperative; until a deadline exists a **watchdog thread plus an external process
  memory cap** (`tests/support/memory_cap.hpp`, this repo's rule for hostile/planted runs) is mandatory. Exhausting
  any budget yields `incomplete` — **never a partial verdict** — and still consumes the look (§3.3).
- **Cadence (022 §1):** evaluation is **not in the CI gate**. The *deterministic parts* — analysis, splits,
  ledger, digests, missingness, budgets, replay refusal — are unit-tested with mock clients **and are**.

## 4. Competing designs (steelmanned)

- **Approval only, no harness.** ADR-179 already requires it. Rejected as the *whole* answer: a human reading
  "deploys go to X" cannot see whether it helps anything, and approval fatigue is itself the attack surface. Kept
  as the second gate.
- **LLM-as-judge per run (the reflection `Evaluator`).** Answers "was this run good?", not "does this lesson help
  across tasks?"; no control arm, no error rate. Kept for the *grader* role only, blinded.
- **A/B in production.** The only externally valid design, and it ships an unproven lesson to real users and real
  tools to find out. Rejected as a first gate; a possible later stage after this one passes.
- **Replay-only evaluation.** Cheap and deterministic but cannot evaluate a changed request (§3.8).
- **Same-set selection plus a multiplicity correction.** Simpler than shards; spends the set's power on the
  selection and, as §6 S2 shows, is the design that inflates. Held-out shards cost only task-authoring effort.
- **Pooled trials as the sample.** Higher nominal N, but the wrong unit under heterogeneous effects (§3.6, H1).

## 5. Falsifiable claims (each needs a control and a planted mutant)

| # | Claim | Mutant |
|---|---|---|
| E1 | Inference is at the **task** level; a mean-zero heterogeneous effect gives ≤ α false positives (upper confidence bound over ≥ 1000 planted runs, reported as a bound, not a point) **and the pooled-trial mutant exceeds α** (positive control) | Pooled trials |
| E2 | dev, shards and canary task ids are disjoint by construction; candidate-selection APIs cannot read a shard's results | Overlap allowed |
| E3 | A second confirmation of a failed **family** (same `subject`+`key`, re-worded) on a spent shard is refused; the look is **reserved before the first trial** and survives a crash | Ledger consulted late; digest instead of family |
| E4 | Any run that started trials — including `invalid`/`incomplete`/`blocked` — consumes a look; only pre-trial `refused: underpowered` is free | Reruns free |
| E5 | The decision rule and candidate digest are hashed into the ledger before any trial; a later edit is detected | Hash after |
| E6 | A trial's memory writes are invisible to the next trial; **positive control:** two trials with the *same* `ref_name` leak; a *fresh Principal with a shared `Mount`* also leaks (proving isolation is by ref, §2) | Shared ref |
| E7 | Grader input contains neither the lesson text/digest nor the arm label; **mechanism:** a scan of the assembled grader input; **control:** planting the lesson text trips it | No scan |
| E8 | Differential ungraded rate beyond the bound ⇒ `invalid`; ungraded trials are counted, never dropped | Silent drop |
| E9 | Replay with a mismatched full-request digest (including tools) is refused; **the unmodified client passes the mismatch** (control that this is new behaviour) | Digest omits tools |
| E10 | Budget exhaustion ⇒ `incomplete`, no verdict, look consumed; N consecutive provider errors aborts the suite | Partial verdict |
| E11 | Arm S blocks a candidate whose value spills into an out-of-scope argument **even when task success rose**; the **known-bad hostile corpus is flagged** (harness sensitivity); the report gives the upper bound; too-few-trials ⇒ `blocked: insufficient_n` | Offsetting allowed; corpus not flagged |
| E12 | The harness types are not constructible from any tool context (`static_assert`/concept; a compile-fail test) | A tool wired in |
| E13 | Same recordings + same suite ⇒ byte-identical verdict | Nondeterministic aggregation |
| E14 | Arm order is seeded, randomised and recorded; **baseline-only** canary drift beyond the bound ⇒ `invalid: drift`; no treatment-arm canary result is exposed | Treatment canary visible |
| E15 | **Sensitivity control:** a scripted client whose success depends on the lesson is detected at the planned power. **Null control:** a lesson-independent client is promoted ≤ α (as an upper bound) | No controls |
| E16 | A trial's tools cannot egress: a real network/shell attempt fails closed (`NetOut`/`Exec` denied by construction); trial tools cause no effect outside the fixture | Real tool in the trial |
| E17 | Structural grading: a value mentioned only in **prose** does not count as spillover; the same value in a parsed tool-call argument does | Substring match |
| E18 | Every trial exit path (cancel, budget, crash, exception) discards its worktree branch; none stranded | Missing discard |
| E19 | A run without `RecordingChatClient` around the trial client is refused | Discard sink accepted |
| E20 | The model grader has no tools, no capability, uses its own `Secret`, and its output is parsed to an enum only | Reuses review key |

**Not claimed:** that any lesson improves real-world performance (§1, §8).

## 6. Executed evidence (offline simulation, 2026-09-21)

Scripts: `sim181.py`, `sim181b.py`, `sim181c.py` (scratchpad; to be committed under `tests/` when E1/E15 are
built). Synthetic binary outcomes; α = 0.05; 300–600 replications per cell. The statistics reviewer **independently
re-ran** these and reproduced every number.

| | Result |
|---|---|
| **S1** *(withdrawn as evidence)* paired test, no effect, task-difficulty variation | FP 1.3–4.7%. **Cannot fail:** both arms share one probability, so they are exchangeable. It supports nothing about the deployed design; the first draft's conclusion from it is retracted |
| **S1b** unpaired z-test, different task draws per arm | 20.0% / 32.8% FP. **A strawman** — no one would run that design — kept only to show why the two arms must share tasks |
| **H1** *mean-zero heterogeneous lesson effect* (logit effect ~ N(0,s)), pooled trial-level sign test vs **task-level** sign-flip | pooled: **3.7–9.3% (s=1), 4.3–25.3% (s=2)**, worst at few tasks × many trials; **task-level 2.7–4.3% throughout** |
| **S2** no effect; best of C candidates chosen and tested on the **same** set, N=100 | 2.5% (C=1) → 9.5% (5) → 11.0% (10) → **17.2%** (20). The "dev-select, confirm once on fresh data ≈ 2%" column is **guaranteed by construction** (a null draw) and is not evidence of protection against a *real* winner's curse or dev→shard shift, which was **not simulated** |
| **S3** no effect; L looks at one held-out set | 1.5% (1) → 5.2% (3) → **15.5%** (10) |
| **H3** reruns after `invalid`/`incomplete` on one shard, accept first p<.05 | 2.0% (1) → 6.0% (3) → **10.2%** (5) |
| **H4** two-shard replication, null effect (2000 runs) | **0.05%** |
| **S4 / H2** power, base 60%, independent trials | +10pp: 12/28/53/**82**% at N=50/100/200/400 · +5pp: 4/9/18/27% at N=50–400, **49/72/83%** at N=800/1200/1600 |
| **S4b** power at +10pp, N=100, task heterogeneity | 28% → 22% (100×1); 24% → 21% (20×5) |
| **H5** safety gate: block iff exact 95% upper bound of T's rate > 2%, N=300 | true 0%: blocked **0%** · 0.5%: **41%** · 1%: **83%** · 2%: 98% · 4%: 100% |

**Limits of the evidence:** synthetic; binary outcomes; independent given the task; **no model was called**; the
real discordance rate and noise floor of an actual task suite are unknown until a pilot; the winner's curse and
dev→shard shift are unsimulated.

## 7. Red-team dispositions

**Round 1** (this ADR's first draft): two independent hostile reviews.

| # | Sev | Finding | Disposition |
|---|---|---|---|
| ST1 | fatal | S1 cannot fail; the pooled sign test is the wrong estimand under heterogeneous effects (**verified**: 25% at s=2, 10×10) | **§3.6** task-level analysis; H1; E1; S1 withdrawn |
| ST2 | serious | "Pair by (task, trial index)" is not real pairing — no shared randomness across arms; S1b is a strawman | §3.6 says so; S1b relabelled |
| ST3 | fatal | Arm P tested the pipeline, not the candidate; B's rate ≈ 0 so the gate is vacuous or always-blocks | **§3.2/§3.7** arm S is per candidate, scope-tagged; hostile corpus becomes the harness's sensitivity control (E11) |
| ST4 | serious | Safety arm power/false-block unaddressed; rule of three answers a different question | §3.7 upper-bound-vs-margin rule; **H5** |
| ST5 | serious | Reruns after invalid/incomplete/refused launder selection (**verified**: 10.2% after 5) | **§3.3** any started run consumes a look; write-ahead ledger; H3; E3/E4 |
| ST6 | serious | Shard-shopping via a rephrase; failed shard becomes dev; stream of candidates inflates family-wise error; canary visible | §3.3 family = `subject`+`key`; retired for the family; **two-shard replication** (H4); baseline-only canary; S2's fresh-data column downgraded |
| ST7 | serious | Discordance pilot leaks or biases; "~1,000+ for +5pp" had no data | §3.6 dev-only pilot, N declared **optimistic**; **H2** gives 1,400–1,600 |
| ST8 | serious | Variance missing from the decision rule; conjunctive gates' false-block unbudgeted | §3.6 rule (4); false-block reported |
| ST9/10 | minor | E15 not an upper bound; E7 no mechanism; E12/E14 gaps; canary/missingness defaults | E1/E7/E12/E14/E15 rewritten; default 5 pp missingness bound; drift **detected, not corrected** |
| SE1 | fatal | Lesson delivery undefined; "fresh Principal ⇒ fresh mount" **false** — a mount is a constructor argument | **§2/§3.2** isolation by `ref_name`; harness mints per-trial `FsRead`/`FsWrite`; E6 gets a control that fails it |
| SE2 | fatal | The request-digest check does not exist; the recording drops the tools table | **§2/§3.8** stated as new code; digest over the full live request stored in the harness's own record; E9 |
| SE3 | serious | Recordings hold attacker text and tool output, no redaction, no storage/retention | §3.9 host-only tainted trial store, retention and size cap; fixture tools bound what can appear |
| SE4 | fatal | Arm P had no confinement; the eval could cause real effects | **§3.9** recording-stub tools, `NetOut`/`Exec` denied, E16 positive control |
| SE5 | serious | Grader reads data as if authoritative; model grader I3; key reuse widens I2 | §3.5 structural grading, no-tool blind grader, distinct `Secret`; E17, E20 |
| SE6 | serious | E12's registry lint is not implementable today | §3.9/E12 restated as a compile-time constraint; the lint is admitted unbuilt |
| SE7 | serious | `PromotionEvidence` will contain attacker text once a queue displays it | §3.9 digests and numbers only |
| SE8 | serious | I8: no deadline, no numeric/memory caps, no error abort, spend not attributed | §3.9 watchdog + process memory cap, abort on consecutive errors, charged to the principal; **deadline named as a blocker** |
| SE9 | minor | Recording wrapper not mandatory; grader nondeterminism; float aggregation | §3.8 refuse without `RecordingChatClient`; grader recorded; E13, E19 |
| SE10/11 | minor | §2 citations imprecise; `run_child_agent_session` derives, not mints; branch discard on failure paths; ledger unauthenticated, crash refunds a look | §2 corrected; §3.2 builds sessions directly; E18; write-ahead host-only ledger (E3) |

## 8. Residuals

- **External validity.** A suite passing says nothing about production tasks. Task authorship (who writes them, how
  representative) is the dominant unaddressed risk.
- **Goodhart.** Dev tasks and graders get optimised against; a baseline canary detects only gross provider drift.
- **Cost.** ~400 pairs = 800 model runs per candidate at +10pp; ~1,600 pairs at +5pp; times two shards for
  replication; times the safety arm's ~300 runs. Budgets bound it; they do not make it cheap.
- **Winner's curse and dev→shard shift** are not simulated; a real winner is likely to look weaker on a shard.
- **The family definition** (`subject`+`key`) is only as good as ADR-179's closed schema; two lessons with
  different keys that steer the same behaviour are treated as different families.
- **Accumulation and interaction** of several lessons are not evaluated (ADR-179 §8 already names the gap).
- **One model, one provider** is all ADR-180 measured; the harness is parameterised by model but no cross-model
  claim exists.
- **No pilot run**: real discordance, noise floor and required N are unmeasured, and the dev-only power estimate
  is optimistic by construction.
- **The per-trial deadline is unenforceable today** (§3.9): a watchdog and process memory cap stand in.
- **Not re-red-teamed after this rewrite.** Round 2 should attack §3.7's scope tagging (who decides in/out of
  scope, and can a candidate be shaped to fall on the wrong side), and the two-shard rule's supply arithmetic.
- **Names needing `tools/naming_lint.py`:** `EvalSuite`, `EvalRun`, `PromotionEvidence`, `LookLedger`.
  `EvaluationVerdict` is taken by the reflection loop and must not be reused.
- 022 §4/§7 amendment (a pointer to this ADR) is still to write.
