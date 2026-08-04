# 017 — Safety and Content Governance

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 006, 007, 016 · **Gate:** §8

## Goal

Defend against the attack class that is specific to agent systems — **the model is steered by
content it reads, and then acts** — plus the ordinary content-governance obligations of an
application that produces text. The engine provides mechanisms and enforcement points; the policy
is the operator's.

## 1. Prompt injection: the honest framing

**There is no known reliable defence at the model layer.** Any design that relies on the model
noticing it is being manipulated is a design that fails. So the position taken here is:

> **Assume the model will be successfully manipulated. Design so that a manipulated model cannot
> do anything the principal was not already authorized to do, cannot do it unnoticed, and cannot do
> it without a human in the loop where policy demands one.**

That is why the load-bearing defences live in 007 (capabilities), 008 (isolation), and 006
(approval), and why this RFC's own mechanisms are *layered mitigations*, not the primary control.

## 2. Defence layers

| Layer | Mechanism | Spec |
|---|---|---|
| **Authority** | Capability attenuation; empty-by-default; model output can never grant | 007 |
| **Isolation** | Untrusted code and content processed inside a boundary | 008, 009 |
| **Approval** | Policy-driven human confirmation over exact validated arguments | 006 §4 |
| **Provenance** | Taint tracking, transitive, type-enforced | 003 §2 |
| **Structural** | Prompt-level separation of instructions from data | §3 |
| **Detection** | Heuristic and model-based filters on ingress and egress | §4 |
| **Containment** | Egress allowlists, output constraints, rate limits | 007, 008, Quark 022 |
| **Attribution** | Every effect auditable; anomaly visible | 007 §8, 016 |

**Layers are independent.** Detection failing must not enable an effect that authority and approval
would have blocked.

## 3. Structural separation

- Tool results, retrieved documents, MCP resources, and remote agent output are rendered into the
  prompt **delimited and provenance-marked**: `<external source="web_search" trust="untrusted">…`.
- **Instructions never come from tainted content.** A tool result that contains "system: you are
  now…" is data inside a delimiter, and the assembled context says so.
- **Nested delimiters are escaped**, so external content cannot forge a closing marker. This is
  string-level injection defence and it is tested with a corpus of forgery attempts.
- Where the provider supports role/segment separation natively, use it in addition — never instead.

## 4. Filters

A **filter** is a `ae:filter` plugin (009) or a native component, invoked at declared points:
`input`, `pre_model`, `post_model`, `tool_args`, `tool_result`, `output`.

- Verdicts: `allow` · `annotate` · `redact` · `require_approval` · `deny`.
- **A `require_approval` verdict may name an approver class** (`end_user` by default, or an
  operator-declared class such as `security_team`) — the same policy-driven mechanism 006 §4 already
  uses for tool approval, extended by one field rather than a new one (§9 Q3, resolved).
- **Filters can deny; they cannot grant.** A filter returning `allow` never bypasses a capability
  check or an approval requirement (007 I3).
- Ordered, each independently attributed in the trace with its verdict; a filter that fails
  **fails closed** by default (configurable per filter, with the unsafe setting explicit).
- Intended plug-in points for injection classifiers, jailbreak detectors, moderation APIs, and
  organization-specific policy — none of which the engine implements itself (§9 Q1, resolved: these
  are inherently heuristic/fuzzy, and shipping one creates an open-ended quality-maintenance
  commitment this RFC's own layering (§1) says isn't load-bearing anyway). **PII detection is the
  one exception**: a first-party filter for pattern-shaped PII (credit-card numbers via Luhn check,
  SSN-shaped strings, email addresses, API-key-shaped tokens) ships, because it is deterministic and
  testable to the same "total and deterministic" bar 007 §5 already holds policy evaluation to —
  shape-matching is not a fuzzy classification quality bar the way injection/jailbreak detection is.

## 5. Content governance

- **PII**: detection and redaction at declared points; redaction propagates to history, telemetry,
  recordings, and checkpoints (005 §6). Redacting only the display copy is theatre.
- **Output constraints**: schemas, length, format, and forbidden-content policies applied before a
  response leaves the engine.
- **Data-residency and retention** are configuration (020): what may be persisted, where, for how
  long, and what must be purged on request.
- **Provenance for generated artifacts** — digests and origin recorded (010 §4), so a file that
  entered the world through an agent is traceable to the run that produced it.

## 6. Specific attack classes handled

| Attack | Primary control |
|---|---|
| Direct prompt injection (user) | Authority: the user cannot exceed their own principal |
| Indirect injection (documents, web, tool results) | Taint + structural separation + capabilities + approval |
| Tool poisoning / rug pull (server changes descriptions) | Digest pinning + re-approval on change (007 §7, 011) |
| Confused deputy (agent used to reach what caller cannot) | Principal propagation + derived-principal attenuation (007 §2) |
| Exfiltration via tool arguments or URLs | Egress allowlist + host-mediated egress + audit (008 §4) |
| Data poisoning of memory/retrieval | Memory writes are capability-gated and attributed (005 §5, 029 §3–4); model-inferred items can never satisfy a policy predicate requiring user assertion (029 §6) |
| Approval-then-substitute | Approval bound to an argument hash (006 §4) |
| Sandbox escape | 008 hostile suite; release-blocking defect class |
| Cross-principal leakage via shared memory index | Principal-scoped memory; release-blocking defect class |
| Model-generated code exfiltrating workspace data | Empty-by-default egress from the sandbox |

## 7. Operator surface

Safety must be *inspectable and adjustable without a rebuild*: policy documents (007 §5), filter
configuration, approval rules, and egress allowlists are configuration; every decision is traced
with the rule that fired; a dry-run mode reports what *would* be denied before enforcement is
enabled — because a policy that cannot be trialled will be deployed with enforcement off.

## 8. Promotion gate

- **G1** — an injection corpus (indirect via tool results, retrieved documents, filenames, code
  comments, image alt-text, MCP resource contents) fails to produce any effect outside the run's
  capability set, on every profile. The measured outcome is *effects prevented*, not
  *injections detected* — the latter is a weaker claim and this gate refuses it.
- **G2** — delimiter forgery corpus cannot break out of the external-content envelope.
- **G3** — a filter returning `allow` cannot bypass a capability check or an approval; proven with a
  deliberately permissive filter.
- **G4** — redaction propagates to history, telemetry, recordings, and checkpoints (canary scan).
- **G5** — every deny/approve decision in a randomized run is attributable to a specific rule id,
  carried by the trace span each filter/policy evaluation already emits (§4, 016 §2). This
  attribution guarantee rides on telemetry's best-effort semantics (016 §5), not audit's no-loss
  one: a denial by definition produces no effect, so 007 §8's audit stream — scoped to effects — never
  records it; the trace span is the only stream that does.
- **G6 (§4, §9 Q1/Q3)** — the first-party PII shape-detector has zero false negatives over a
  reference corpus of valid credit-card/SSN/email/API-key-shaped strings (shape-matching, not
  fuzzy classification, so zero is the right bar, not a statistical target); a `require_approval`
  verdict naming a non-default approver class is routed to that class and never silently falls back
  to `end_user`.

**Invariant-touching, ADR-track:** tying G5's attribution guarantee to telemetry rather than audit
(2026-08-04) settles a textual contradiction with 016 §7 G4's audit no-loss invariant, but it is a
decision about where a security-relevant record actually lives, not a cosmetic rewording. Per this
project's review workflow (`docs/planning/v1-review-signoff-workflow.md` §3), a change that touches
an attribution guarantee this directly doesn't clear on the textual fix alone: it still owes the
full design→red-team→prove→judge cycle and an ADR under `decisions/` before G5 is more than
Draft-consistent, the same posture 004 §5 flags for its own invariant-touching change.

## 9. Open questions

- ~~**Q1** — Whether to ship first-party injection/PII classifiers or only the seam. Shipping them
  creates an expectation of quality we would have to maintain; not shipping them leaves the default
  posture weaker.~~ **Resolved, split by determinism, not by importance (2026-08-04):** the
  dilemma dissolves once "classifier" is recognized as two different kinds of mechanism. **Injection/
  jailbreak/moderation detection is inherently heuristic** — its accuracy is a moving target against
  evolving attacks, which is exactly the open-ended quality commitment the question worried about,
  and §1 already establishes it isn't load-bearing ("assume the model will be successfully
  manipulated"; detection is a layered mitigation, not the primary control). Seam-only, unchanged.
  **PII detection of structured shapes is deterministic** — a credit-card number, SSN-shaped string,
  email address, or API-key-shaped token is a pattern match, testable to 100% precision on shape the
  same way 007 §5 already requires policy evaluation to be "total and deterministic." That has none
  of the open-ended-quality-bar cost the question was actually worried about, so it ships first-party.
  Full text: §4.
- ~~**Q2** — Span-level taint (003 Q3) would make structural separation far more precise.~~
  **Resolved, No (OQ-5, 2026-08-04):** see 003 §8 Q3 / 007 §10 Q2 — rejected because it would
  downgrade I3's static (compile-fail) guarantee to a runtime one across every text-touching path,
  for a precision gain rather than a security fix. §3's structural separation already gets most of
  the practical benefit for free: ingested content is delimited into separate, independently-tainted
  items *during assembly*, so the coarse "mixing" scenario mostly only arises in model-*generated*
  output that paraphrases tainted material inline — a narrower case than the general ingestion path
  this question was framed against, and one `Citation` (003 §1 Q1) can address at the display layer
  without touching the taint type itself.
- ~~**Q3** — Whether `require_approval` from a filter should be able to escalate to a *different*
  approver (a security team) rather than the end user.~~ **Resolved, Yes (2026-08-04):** routing a
  filter-triggered hold back to the same principal whose session produced the flagged content is a
  weaker check than an independent reviewer for exactly the cases this matters most — if the content
  was sophisticated enough to trip a filter, the engaged end user is not obviously a stronger
  independent check than a dedicated reviewer, and in a direct-injection case the end user may be the
  attack's own source. §4's `require_approval` verdict now carries an optional approver-class field
  (`end_user` default, operator-declared classes such as `security_team` available), reusing 006 §4's
  existing policy-driven approval mechanism rather than adding a second one. Full text: §4.
- ~~**Q4** — Evaluation of safety controls: an adversarial suite that grows over time needs an owner
  and a cadence (022).~~ **Resolved (OQ-10, 2026-08-04):** see 022 §5 for the full resolution —
  owner and cadence are inherited from the design→red-team→prove→judge cycle (024 §4.2) this
  project already runs for every contested 007/008/017/018 change, not invented as a new standing
  commitment.
