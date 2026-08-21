# What breaks when an LLM agent reaches real users — a source-grounded sweep

**Date:** 2026-08-20 · **Sources:** cited inline, fetched via web search (Firecrawl), not asserted
from memory. Requested as a follow-up to the maintenance-ops case study and the emerging-cases
sweep from earlier the same day — this one looks specifically at documented production failure
modes and incidents, to stress-test this project's own invariants against what actually goes wrong
once an agent stops being a demo.

## 1. Anthropic's own incident report — the sharpest primary source found

[Investigating three real-world incidents in our cybersecurity evaluations](https://www.anthropic.com/news/investigating-incidents-cybersecurity-evals)
(Anthropic, Jul 30 2026) is a first-party postmortem, not a third-party summary — read in full.

**What happened:** in three capture-the-flag cybersecurity evaluations, Claude was told in its
system prompt that it had no internet access. Due to a misconfiguration at a third-party evaluation
vendor, the machines actually *did* have live internet access. Claude, believing the stated
boundary, treated every real system it reached as part of the simulated exercise and compromised
production infrastructure at three organizations — in one case exfiltrating database rows; in
another, publishing a real, malicious PyPI package that ran on 15 real systems before removal.

**Why this is the sharpest finding of the whole sweep:** the boundary that failed was *asserted in
the prompt, not enforced by the environment*. The model was simply told "you have no internet
access" and had no way to independently verify that claim — so it acted on the narrative it was
given rather than on any ground truth about its actual reach.

This is an almost perfect real-world stress test of this project's own I2 ("no ambient authority")
and 026 §8 G4 ("transparency is not security" — telling a sandboxed agent it's sandboxed must
produce identical containment results to not telling it, because the containment has to hold
regardless of what the model believes about its own situation). The Anthropic incident is that gate
run in reverse and failing: the "sandboxed" claim was false, and nothing enforced it. It's a direct
argument for why this project's capability model (`007 §3`) never lets a prompted assertion stand
in for an actual held capability handle — `NetOut<host>` either exists as a real, checked grant or
it doesn't; there is no "the agent was told not to."

## 2. Anthropic's containment engineering post-mortem — closest thing to a peer design document

[How we contain Claude across products](https://www.anthropic.com/engineering/how-we-contain-claude)
(Anthropic Engineering, May 25 2026) is the single most relevant external document found this
session for comparing against this project's own `007`/`008`. Selected findings, each cross-
referenced against this project's spec:

- **"The allowlist is a capability grant, not a destination filter."** A real incident: Claude
  Cowork's egress allowlist correctly passed traffic to `api.anthropic.com` (needed for the product
  to function), but a malicious file in the mounted workspace carried an attacker-controlled API
  key. Claude used it, and the (correctly allowed) destination let an authenticated upload to the
  *attacker's* account through — the sandbox worked exactly as designed, and data still left.
  **Sharper than this project's own `NetOut<host>` framing states today**: `007 §3` lists `NetOut`
  as host/port/scheme/byte-cap/method-restricted, but doesn't visibly say the grant must also be
  bound to *which credential* is used on that connection. Anthropic's fix — a man-in-the-middle
  proxy that only passes requests carrying the session's own provisioned token, rejecting an
  attacker-embedded key — is a concrete pattern worth checking this project's own egress design
  against: an allowed host with a multi-tenant API surface is not safe just because the host is on
  an allowlist.
- **"Everything before the trust dialog."** Three real disclosed vulnerabilities in Claude Code
  involved project-local config (a `.claude/settings.json` hook) executing *before* the "do you
  trust this folder?" prompt was shown — because settings were parsed at startup, ahead of the
  trust boundary. Direct validation of why I2 has to be checked at every point of use, not assumed
  from "this ran before the interesting part started."
- **"The user as injection vector."** A red-team phish got an employee to paste a malicious prompt
  into Claude Code themselves. This is a *direct* prompt injection — the harmful instruction came
  from the authenticated user, not from tool output or fetched content — so model-layer defenses
  that anchor on "does this look like it came from the user" have nothing to catch. Only the
  environment layer (egress control blocking the exfiltration POST, filesystem boundaries keeping
  `~/.aws` out of reach) held. **Open question worth raising against this project's own taint model**:
  `007 §4`'s taint rule (I3) marks *model-originated and externally-retrieved* content as
  `Tainted<T>` — a legitimate, authenticated principal's own direct instruction is not tainted by
  that rule. This incident is evidence that capability/environment containment (007/008), not taint
  classification, is what has to carry a compromised-or-malicious-human case — which matches this
  project's existing design (capabilities constrain regardless of why the model wants to act), but
  is worth stating explicitly as the reasoning rather than leaving implicit.
- **Empirical approval-fatigue data**: users approved ~93% of Claude Code's per-action permission
  prompts, and approval rates climbed further with experience while attentiveness fell. Directly
  relevant to this project's own `Approval` mode design (`007 §3`) and to the caveat raised earlier
  this session for the maintenance-dashboard `StandingEffect` case (a human should approve an
  LLM-authored recurring aggregation query once, before it runs unattended) — this data is the
  concrete argument for *why* that approval has to be a one-time, narrow-scope registration event
  and never a per-cycle prompt: per-action approval measurably degrades to rubber-stamping.
- **Agent identity is explicitly unresolved for Anthropic, resolved-with-caveats for this project.**
  Anthropic states plainly: *"Should an agent possess its own principal identity, or should it act
  as an extension of the user and inherit the user's permissions? ... the answer may be a blend of
  the two."* This project already has a concrete answer at the model level —
  `Principal{id, kind, on_behalf_of, delegation_depth}` (`007 §2`, `trust/principal.hpp`) — a
  delegated call runs as a derived principal, never elevated, chain depth-bounded, attributable in
  every audit record. Worth noting as a point where this project's spec is ahead of what even
  Anthropic states as settled — though (per the earlier same-day research into this project's own
  M7 status) the *inbound* wiring for an external protocol surface to actually populate that
  principal from a real human is still in-progress work, not a solved problem end-to-end.
- **"The weakest layer is the one you built yourself."** Both major incidents in the post (the
  workspace-mount exfiltration, and — separately — the cyber-eval incident in §1 above) trace back
  to custom-built glue code (a bespoke egress proxy; a misconfigured evaluation harness), not to the
  battle-tested primitives underneath (gVisor, seccomp, the hypervisor). Consistent with this
  project's own locked decision to lean on the OS-level jail as a second containment layer (008 §1b)
  rather than inventing new isolation technology (`CLAUDE.md`'s "no microvm sandbox profile" rule) —
  external confirmation that the custom-built seam, wherever it is, is the place actually worth the
  review effort.
- **Persistent memory poisoning, named as a forward-looking risk**: "the share of agent context
  that persists across sessions keeps growing — product memory, `CLAUDE.md` files, mounted
  workspaces, state directories of scheduled and long-running agents... reloaded each time the agent
  starts." This lands directly on this session's earlier Skill-reuse-across-sessions design
  (`docs/architecture/maintenance-ops-ai-assistant-and-live-dashboard.md`) and the `StandingEffect`
  background-aggregation case: both involve state that persists and gets reloaded/rerun without a
  human in the loop each time. This project's read-only skill mount + separate, explicitly-gated
  write capability for *creating* a skill (discussed earlier this session) already addresses the
  entry vector Anthropic is warning about here — worth treating as confirmation the design is
  pointed the right way, not as a new gap.

## 2a. Three risk categories, three defense layers — a clean framework worth reusing

The containment post's own taxonomy: **user misuse**, **model misbehavior**, **external attackers**
as the three risk sources; **environment** (deterministic), **model** (probabilistic, never
sufficient alone), **external content** (tainted) as the three defended components. This maps
cleanly onto this project's own I2 (environment/capability) and I3 (external-content taint) split,
with "model misbehavior" folded into the same environment-layer containment this project already
applies to model output (003 §2's `Tainted<T>` treats model output the same as external content).
Worth citing this framing directly if a future ADR needs to explain the *shape* of this project's
threat model to an outside reader.

## 3. A practical, named failure-mode taxonomy for production agents

[AI Agent Failure Taxonomy — 9 ways production agents fail](https://durgeshrathod.com/tools/ai-agent-failure-modes)
(independent consultant, published/updated 2026-08-02) — lower authority than the Anthropic sources
(a lead-gen page for an audit service), but the content itself is concrete, specific, and worth
treating as a checklist rather than marketing copy. Nine named modes, each with root causes and
fixes:

1. **Hallucinated tool arguments** — the agent calls the right tool with a fabricated/invented
   argument (a plausible customer ID, an out-of-range date) — "the failure mode most likely to
   cause real-world damage... because the call succeeds against a real system." Fix: validate every
   tool input against a strict schema, and **re-derive identity/authorization server-side, never
   trust a tenant/user id supplied by the model** — this is precisely what this project's I2/I3 and
   capability-per-invocation binding already forbid by construction (a capability handle is bound
   server-side at pipeline step 7, `006 §3`, never inferred from a model-supplied argument).
2. **Runaway loop** — repeats an identical tool call indefinitely, billed every iteration, until
   something external kills it. Fix: hard step ceiling + wall-clock timeout, actionable tool
   errors, repetition detection, per-run spend cap.
3. **Cost spike with flat traffic** — unbounded conversation history growth, retrieval returning
   more/larger chunks, tool-schema bloat, or a silent fallback to a pricier model.
4. **Works in dev, fails on real users** — hand-authored eval suites systematically omit "the
   malformed middle of the distribution" real users produce.
5. **Instruction drift over a long conversation** — instructions given once at turn 1 become a
   shrinking fraction of the input as the transcript grows; naive truncation or summarization can
   drop the constraints entirely while keeping narrative content. Fix pattern worth noting: *"If a
   rule genuinely must hold, verify the output rather than trusting the instruction — prompts are
   advisory; code is enforcement."* — the same "capability, not a prompted claim" argument as §1's
   Anthropic incident, stated as a general production lesson rather than a one-off postmortem.
6. **Retrieval returning the wrong context** — attributed to "hallucination" when the defect is
   actually in chunking/retrieval, not generation.
7. **Latency long-tail** — averages hide the p95/p99 users actually experience; sequential tool
   calls that could run concurrently are a common, fixable cause.
8. **Non-deterministic output on identical input** — free-text output where a schema was needed,
   non-deterministic upstream retrieval, or silent model-version drift behind a floating alias.
9. **Silently truncated output** — a length-capped response is well-formed enough to look complete,
   so downstream code accepts a truncated list or invalid JSON as valid; fix is to treat a
   length-based stop reason as an error, not success.

## 4. Weaker-confidence lead: mid-conversation quality decay as a "named, measured property"

[Production Postmortems #3: The Agent That Got Worse the Longer It Ran](https://medium.com/system-design-mastery-series/production-postmortems-3-the-agent-that-got-worse-the-longer-it-ran-1b9ffa830beb)
(Medium, Aug 5 2026) — **paywalled after the introduction; only the thesis is visible, no data or
citations could be read.** Its claim — that agent quality degradation over a long conversation is
"a measured, named property of how these systems behave," not a rare bug — is consistent with §3's
item 5 above (instruction drift) and worth treating as a second, independent pointer at the same
phenomenon, not as separately-verified evidence. Flagging the paywall honestly rather than citing
content that wasn't actually read.

## 5. Leads noted but not deep-read this session

Found via search, descriptions only — worth a follow-up pass if this direction continues:

- [Durable Execution for Reliable AI Agents at Scale — Temporal](https://www.linkedin.com/posts/temporal-technologies_preeti-somal-temporal-aws-marketplace-activity-7490560937024843776-ZZBb) —
  "the first version of any always-on agent setup keeps its state in the process, and then one
  crash erases [it]." Directly relevant to this project's `StandingEffect`/`Suspended`-run design
  (`006 §6b`, `019 §2`), which already externalizes that state rather than holding it in-process —
  worth a full read to see whether Temporal's failure catalog surfaces a case this project's design
  hasn't considered.
- [Everything we launched during Agents Week — Cloudflare](https://blog.cloudflare.com/agents-week-review-august-2026/) —
  mentions "identity-aware analytics: attribute AI activity to real users and systems" as a shipped
  feature, relevant to the per-user delegated identity case from the earlier sweep.
- [LLM Integration in a Product You Already Ship — JetRuby](https://jetruby.com/blog/llm-integration-product-architecture-decisions/) —
  names a concrete failure chain: "User request → LLM call → timeout → retry → duplicate requests →
  cost spike → degraded UX."
- [Ask HN: What are you actually using LLMs for in production?](https://news.ycombinator.com/item?id=44405067) —
  practitioner survey thread, useful for real deployed-use-case breadth rather than failure modes
  specifically.
