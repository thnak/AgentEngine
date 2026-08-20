# ADR-068 — When a secret leaks into ordinary content at runtime, does AgentEngine own detection and audit durability itself, or delegate both to the host through a declared seam?

**Status:** Design — question, competing designs, and decision recorded from a design→red-team→judge
pass. **NOT Proposed, NOT Judged**: no implementation exists, no executed evidence, no prove phase
has run (§5). Independent of `decisions/ADR-066-context-provider-attribution-provenance.md` and
`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md` — builds only on already-shipped
`trust/secret.hpp` (018 §4).

**Relates to:** `018-Identity-Authorization-and-Secrets.md` §4 (`SecretRef`/`SecretStore`/
`SecretLease`, extended here with a mint-at-runtime path), `005-Sessions-State-and-Memory.md` §1
(redaction as a legal, audited history-rewrite category — the first real consumer of that category
triggered by the agent itself), `decisions/ADR-033-middleware-model-call-chain.md`
(`MiddlewareTraceHook`, the host-injected-callback idiom this design's audit fix reuses). Design
draft: `docs/planning/runtime-secret-quarantine-design-draft.md` (workflow-reviewed 2026-08-20, 4
findings including 1 FATAL, all applied). External grounding: HashiCorp Vault's Transform
tokenization ([docs](https://developer.hashicorp.com/vault/docs/secrets/transform/tokenization)) vs.
Presidio's Encrypt/Decrypt operator
([docs](https://data-privacy-stack.github.io/presidio/anonymizer/)) — this design follows Vault's
shape (opaque token, no information recoverable without a store round trip), not Presidio's
(reversible-by-algorithm ciphertext embedded in the replacement text).

## 1. The question

**Stated so it has a wrong answer:** `trust/secret.hpp` already gives AgentEngine a capability-gated,
zeroizing, declare-then-resolve secret path for secrets the engine always knew about (operator-
declared config). A secret that shows up incidentally inside ordinary content (a pasted API key, a
tool result echoing a leaked credential) is a different problem — nothing detects it today. Should
AgentEngine SHIP a detection heuristic and own the resulting audit trail itself, or define the seam
and delegate both detection and audit durability to the host, the same way it already delegates
session storage (005 §2: "AgentEngine adds no storage engine of its own")?

## 2. The competing designs

**Design A — engine-owned detection + in-process audit.** AgentEngine ships a regex/NER-based
`scan()` and records quarantine events in the existing `invoke_tool()` step-10 audit record.
Steelman: self-contained — a deployment gets basic protection with zero host wiring.

**Design B (chosen) — host-injected `SecretDetector` + host-injected `QuarantineAuditHook`.**
AgentEngine owns only: the seam (where detection is called, what happens to a positive match), a
content-addressed `QuarantineSecretStore` (mint-at-runtime, satisfying the existing `SecretStore`
concept), and a narrow structural guarantee on the audit event type (it can never carry the secret's
own bytes — no field capable of holding them, mirroring `SecretLease`'s "no `std::string` conversion"
trick). Both detection heuristics and audit durability are the host's decision, delivered through
`nullptr`-by-default callbacks — the same idiom this codebase already uses for `ApprovalDecider`,
`MiddlewareTraceHook`, and `WorkflowSupervisor::CheckpointHook`. Steelman: avoids duplicating a whole
existing product category (PII/secret detection — nine surveyed systems in `docs/research/2026-08-
07-sensitive-input-and-secret-leakage-handling.md`) for no benefit over calling out to one a real
deployment already has an opinion about; avoids the audit-durability FATAL hole entirely by
construction, since the engine makes no durability claim it can't back.

## 3. Falsifiable claims

| Claim | Disproving experiment |
|---|---|
| Content-addressed naming (`quarantine()`, keyed by a hash of the detected bytes) deduplicates the same secret appearing via two independent, non-communicating `ContextProvider`s. | Stub two `ContextProvider`s each independently exposing byte-identical secret content; assert exactly one `SecretRef` is minted across both. |
| The Vault-style token carries no recoverable information without a `SecretStore::resolve()` round trip. | Capture a full assembled transcript containing a quarantined reference; assert no algorithm recovers the original value from the transcript alone. |
| The `QuarantineAuditHook` fires exactly once per `quarantine()` call, synchronously, before return, and its event type structurally cannot carry the secret's own bytes. | Attempt to construct a `QuarantineAuditEvent` containing the raw secret; assert this does not type-check / is not expressible. |
| An unconfigured `SecretDetector` (host wires nothing) fails open to no-quarantine behavior, not a crash or silent corruption. | Run the full pipeline with `SecretDetector == nullptr`; assert identical behavior to a run with no secret content at all. |

## 4. The red-team attack (text-level, not code-level), and the FATAL finding

Same three-lens workflow review as ADR-066/067. Four findings, one FATAL:

1. **FATAL, fixed by delegation, not by building a subsystem.** The original design left audit
   durability implicit, leaning on `invoke_tool()`'s existing step-10 record — which
   `tool_pipeline.hpp`'s own top comment documents as "a minimal in-memory struct; 016's full span/
   telemetry shape is out of scope." This made the design's own abuse-surface question (can a
   manipulated model quarantine non-secret content to launder it out of the transcript, undetected)
   answer itself: no attacker technique is even needed, since nothing guarantees the record survives
   process end, a crash, or is visible to an operator at all. **Fixed**: delegate durability entirely
   to a host-injected `QuarantineAuditHook`, matching `MiddlewareTraceHook`'s existing shape — the
   engine's guarantee narrows to "the hook fires, with metadata that structurally excludes the secret
   value," and durability becomes the host's problem, the same way `SessionStore`'s actual backing
   store already is (005 §2).
2. **Must-fix**: citations to `tool_optimizer_provider.hpp` (as precedent for both the detection style
   and the zero-capability-tool trust shape) pointed at a file that exists only on a still-open,
   unmerged branch (PR18) — corrected; the zero-capability-tool trust shape needs no external
   precedent, it follows directly from 006's own `EffectClass`/`Tool` vocabulary.
3. **Must-fix**: detection was described as riding 017 §4's `ae:filter` `input`/`tool_result`
   points — those points are declared vocabulary with zero implementation. Corrected: detection is
   its own seam (`SecretDetector`, host-injected, §2 above), not dependent on unbuilt infrastructure.
4. **Worth-noting, resolved**: the auto-grant scope question (does quarantining the extracting
   principal's own secret grant `cap::Secret` for the current run, or the whole session) — resolved
   to run-scope only. Session-wide-by-default was rejected: inferring a wider grant lifetime from
   what the user "probably expects" sits close to the I2 ambient-authority shape this project exists
   to prevent, even though the grant itself stays named and explicit.

## 5. Executed evidence

**None.** No implementation exists — not `QuarantineSecretStore`, not `SecretDetector`, not
`QuarantineAuditHook`, not the `quarantine_secret` tool. The FATAL finding was found and fixed at the
design level; a real prove phase (implementation, adversarial testing against compiled code,
sanitizers) has not run.

## 6. Per-claim verdicts

Every claim in §3: **INCONCLUSIVE — no executed evidence exists to decide it.**

## 7. The decision

**Design B is adopted as the target for the future prove phase.** It binds:
- `018-Identity-Authorization-and-Secrets.md` §4 — extends `SecretStore`'s vocabulary with a
  mint-at-runtime path (`QuarantineSecretStore`), never replacing the existing declare-then-resolve
  path.
- `005-Sessions-State-and-Memory.md` §1 — the `quarantine_secret` tool is the first real, agent-
  triggered consumer of the redaction history-rewrite category already named there.
- Does NOT bind 017 §4 — detection is explicitly its own seam, not a consumer of `ae:filter`'s
  unimplemented points.

**Explicitly out of scope, named rather than left implied:**
- Detection quality/false-negative rate — entirely the host's `SecretDetector` implementation's
  problem; AgentEngine ships no default.
- Audit durability — entirely the host's `QuarantineAuditHook` implementation's problem.
- Whether a THIRD-PARTY-sourced secret (no auto-grant) should be destroyed outright rather than kept
  retrievable — left open, current design keeps it retrievable (matching Vault's default), flagged as
  possibly wrong for content nobody actually owns.

**Residual risks:**
- The entire §5/§6 evidence gap.
- A deployment that wires neither `SecretDetector` nor `QuarantineAuditHook` gets NO protection and NO
  audit trail at all — a real, named limitation of the delegation design, not a hidden one. Whether
  this is an acceptable default (fail-open on missing host configuration) versus something that
  should fail closed (refuse to run without both configured) is not decided by this ADR.
