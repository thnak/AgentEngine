# 017 — Safety and Content Governance

**Status:** Draft · **Depends on:** 003, 006, 007, 016 · **Gate:** §8

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
- **Filters can deny; they cannot grant.** A filter returning `allow` never bypasses a capability
  check or an approval requirement (007 I3).
- Ordered, each independently attributed in the trace with its verdict; a filter that fails
  **fails closed** by default (configurable per filter, with the unsafe setting explicit).
- Intended plug-in points for injection classifiers, PII detectors, jailbreak detectors,
  moderation APIs, and organization-specific policy — none of which the engine implements itself.

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
| Data poisoning of memory/retrieval | Memory writes are capability-gated and attributed (005 §5) |
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
- **G5** — every deny/approve decision in a randomized run is attributable to a specific rule id.

## 9. Open questions

- **Q1** — Whether to ship first-party injection/PII classifiers or only the seam. Shipping them
  creates an expectation of quality we would have to maintain; not shipping them leaves the default
  posture weaker.
- **Q2** — Span-level taint (003 Q3) would make structural separation far more precise.
- **Q3** — Whether `require_approval` from a filter should be able to escalate to a *different*
  approver (a security team) rather than the end user.
- **Q4** — Evaluation of safety controls: an adversarial suite that grows over time needs an owner
  and a cadence (022).
