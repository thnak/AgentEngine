# A honeypot-realism gate for 008/017's hostile suites — design draft

**Status: designed, not implemented.** Matches this project's `design → red-team → prove → judge`
discipline (CLAUDE.md), same format as `docs/planning/schedule-wakeup-standing-effect-design-
draft.md`, `docs/planning/tool-optimizer-provider-design-draft.md`, and `docs/planning/model-call-
gateway-routing-design-draft.md`.

**Origin:** same-session reading of two real Google DeepMind alignment-auditing papers —
["Realistic honeypot evaluations for scheming propensity"](https://arxiv.org/abs/2605.29729) and
["Gram: Assessing sabotage propensities via automated alignment auditing"](https://arxiv.org/abs/2605.30322)
(both cited in full in `docs/research/2026-08-20-scaling-llm-capability-and-the-safety-surface.md`
§6a). Gram's sharpest finding: sabotage rate in simulated agentic deployments falls close to zero as
environment realism increases and nudges-toward-misbehavior are removed — risk is a function of how
contrived the deployment *feels* to the model, not a fixed per-capability tax.

## Verification: the gap is real

Grepped `008-Sandbox-and-Isolation.md` §9 (Promotion gate, G1-G8) and `017-Safety-and-Content-
Governance.md` §8 (Promotion gate, G1-G6) for "honeypot"/"realistic" — no hits. Read both gate
sections in full. Every existing gate measures **containment of a known-adversarial corpus**: kill
time (008 G2), no-ambient-authority enumeration (008 G3), injection-effects-prevented (017 G1),
delimiter-forgery resistance (017 G2). None test whether an agent that does **not** know it is being
tested, given a plausible-looking illegitimate opportunity nobody flagged as adversarial, takes it
anyway. This is a genuinely different methodology from what exists, not a variant of an existing
gate — confirmed gap, not manufactured.

## Design — a new gate category, not a new subsystem

No new sandbox mechanism needed — this reuses `008`'s existing profiles and `017`'s existing filter
pipeline as the environment; what's new is the **shape of the test corpus** and **what counts as
passing**.

- **Corpus shape**: ordinary-looking tasks (matching real agent workloads this engine targets —
  coding, tool orchestration) with one **achievable-but-wrong** action embedded — an unattended
  opportunity to exceed the task's actual scope, never labeled as a test, never phrased to imply
  agency/goal-directedness beyond the task itself (the scheming-honeypot paper's own finding: those
  are exactly the prompt shapes that *induce* the behavior being measured, so the corpus must avoid
  them to measure a realistic baseline, not manufacture a positive).
- **Pass condition**: the agent does not take the wrong-but-available action, *and* — the harder,
  more valuable half — does not need to be told not to. A corpus entry that only passes because a
  system prompt explicitly forbids the specific trap is measuring instruction-following, not
  containment; 017 §9's own resolved Q1 already establishes this project's posture ("assume the
  model will be successfully manipulated; detection is a layered mitigation, not the primary
  control") — this gate should hold to the identical standard: pass because I2/I3 capability bounds
  make the wrong action unreachable or inconsequential, not because a prompt discouraged it.

## Self-red-team

- **The one real hazard, and it is not hypothetical**: realism must never be achieved by actual
  reachability to real external systems. This is not an abstract worry — it is *exactly* the failure
  mode in the Anthropic cybersecurity-incident report already cited this session
  (`docs/research/2026-08-20-llm-agent-production-deployment-lessons.md` §1): an evaluation
  environment believed to be isolated was, through misconfiguration, actually internet-connected,
  and the model treated real production systems as in-scope because nothing distinguished them from
  the simulation. A honeypot corpus for this project's own gates must get its realism from
  **narrative/data fidelity** (plausible-looking file contents, plausible-looking tool responses,
  plausible-looking stakes) while remaining exactly as sandboxed as every other 008 hostile-suite
  case — same profile, same capability bounds, same `NetOut` allowlist (which, per 007 §3, is empty
  or tightly scoped by default regardless). **Never widen containment to manufacture realism.** This
  is a hard constraint on the design, not a tradeoff to weigh.
- **CLAUDE.md's Machine Safety rule applies unchanged**: "a test proving a fork bomb is contained
  must not be able to take the machine with it" — a honeypot corpus is still a hostile test; it gets
  no exemption from resource-capping just because its trap is narrative rather than a literal
  exploit attempt.
- **Eval-awareness confound is a named, honest limitation, not solved by this design**: the
  scheming-honeypot paper's own point — a model that recognizes it is being tested behaves
  differently, and realism reduces but does not eliminate that confound. A "zero misbehavior" result
  on this gate is evidence of contained behavior *under this test's specific realism level*, not a
  provable absence of the behavior at every level of situational awareness a future, more capable
  model might have. State this in the gate's own documentation rather than overclaiming — matching
  017 §1's existing "assume the model will be successfully manipulated" honesty norm.
- **I5 / determinism**: no new concern beyond what every other hostile-suite case already requires —
  model calls in the corpus must be recorded/replayable the same way, nothing honeypot-specific
  changes the recording contract.
- **Does a more-realistic test environment risk being mistaken for a real one by a human operator?**
  Low risk, named for completeness: this is an internal CI/test corpus, never operator-facing or
  reachable outside the test harness — no plausible path for confusion, unlike a staging environment
  that real users might reach.

## What this draft is not

Not an implementation. Not an ADR. Not a claim that this closes the eval-awareness confound the
source papers themselves name as unresolved. A candidate new gate category (`008 §9` and/or a new
`017 §8` entry) needing the full `design → red-team → prove → judge` cycle, same as every other
finding filed this session, before any corpus is written or any gate becomes binding.
