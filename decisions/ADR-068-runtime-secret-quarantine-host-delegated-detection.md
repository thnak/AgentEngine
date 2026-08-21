# ADR-068 — When a secret leaks into ordinary content at runtime, does AgentEngine own detection and audit durability itself, or delegate both to the host through a declared seam?

**Status:** Proposed (design → red-team → prove phases complete for Design B; awaiting explicit user
"Judged"). Implemented: `include/agentengine/trust/secret_quarantine.hpp`
(`QuarantineSecretStore`/`SecretDetector`/`QuarantineAuditHook`/`scan_and_quarantine`/
`QuarantineSecretTool`/`QuarantineToolProvider`), proven by `tests/test_secret_quarantine.cpp`
(24/24 checks) and, wired into a real `rt::AgentSession` round via `QuarantineToolProvider`,
`tests/test_rt_agent_session_quarantine_tool.cpp` (5/5 checks, 2026-08-20 — see §7's residual-risk
update), real Windows/MSVC build — see §5/§6 for the updated evidence and verdicts; this ADR's
original §5/§6, written before implementation, are superseded by that section, not deleted). A real,
mid-implementation finding corrected the original design (recorded in the header's own top comment and §7 below):
"auto-grant in the same step" is not implementable against the real `CapabilitySet` (empty by
construction, no incremental-grant method) and would have been I3-unsafe for the agent-initiated
path regardless (a model-supplied tool argument cannot mint its own authority) — fixed by splitting
grant-eligibility bookkeeping from any actual grant, which stays entirely host-driven. Independent of
`decisions/ADR-066-context-provider-attribution-provenance.md` and `decisions/ADR-067-middleware-
turn-point-pre-model-enforcement.md` — builds only on already-shipped `trust/secret.hpp` (018 §4).
**Forward reference:** `decisions/ADR-070-host-configurable-responsibility-boundary.md` names this
ADR's `SecretDetector`/`QuarantineAuditHook` shape (host-injected, `nullptr`-by-default, structurally
barred from ever reaching a capability-granting position) as the prior-art precedent for its own
formally-named "Delegated Decision Seam" pattern.

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

**Note on §2's own text below**: written before implementation; it still describes the ORIGINAL
"quarantine() grants in the same step" mechanism for the capability-scope part of Design B. §5
records why that specific mechanism doesn't survive contact with the real `CapabilitySet` API and I3,
and what replaced it (grant-eligibility bookkeeping, entirely host-driven). Kept here unedited,
as the record of what was proposed, rather than silently rewritten to match what shipped.

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

## 5. Executed evidence (superseding this ADR's original, pre-implementation §5)

Implemented: `include/agentengine/trust/secret_quarantine.hpp`. Real, mid-implementation finding
(not anticipated by the design draft, recorded in the header's own top comment): the original
"auto-grant in the same step" design is not implementable at all against the real `CapabilitySet`
(`trust/capability.hpp`) — it is empty by construction with no public method to add a single
capability incrementally, only `grant_root()`, which REPLACES the whole set and is, by that type's
own comment, "never reachable from anything derived from model output" (I3). A sharper problem
independent of that API gap: `QuarantineSecretTool::invoke`'s `args.text` is model-supplied, so even
if an incremental-grant API existed, granting from it would let a manipulated model mint itself an
arbitrary resolvable secret reference — model output becoming authority, exactly I3's prohibition.

**Fixed by splitting the concern**: `quarantine()` never mutates any capability set. Extraction
`trigger` (`verified_user_content` vs. `agent_initiated`) is pure bookkeeping, read only by
`grant_eligible_ref_names()` — a HOST-ONLY query with no path from any `Tool::invoke()` body — naming
which refs a host MAY fold into a real grant at its own next `CapabilitySet::grant_root()` call.
`agent_initiated` refs (minted through `QuarantineSecretTool`) are NEVER grant-eligible, structurally,
regardless of what a model passes as the argument.

Windows/MSVC build, `tests/test_secret_quarantine.cpp`, **24/24 checks passed**, `ctest` clean
(`test_secret_store`/`test_secret_quarantine`, 2/2). Commands: `cmake -S . -B build`, `cmake --build
build --target test_secret_quarantine --config Debug`, `ctest --test-dir build -C Debug -R secret
--output-on-failure`.

## 6. Per-claim verdicts (superseding this ADR's original, pre-implementation §6)

| Claim (§3) | Verdict |
|---|---|
| Content-addressed naming deduplicates the same secret across two independent callers. | **CORRECT** — `test_content_addressed_dedup`: two `quarantine()` calls with byte-identical input yield the same `SecretRef.name`; a different value yields a different name. |
| The token carries no recoverable information without a `SecretStore::resolve()` round trip. | **CORRECT, narrower than originally stated** — the disproving experiment as written (capture a full transcript, assert no algorithm recovers the value) was not run as a generic claim over "any algorithm"; what IS proven (`test_scan_and_quarantine_redacts_correctly`, `test_agent_initiated_tool_never_grants_and_redacts_verbatim`) is that the ONLY string ever returned to a caller is the SHA-256-derived ref name (via the existing, audited `hmac_sha256` primitive) — the original bytes never appear in any return value, redacted text, or reply. Real cryptanalysis of the digest itself is out of this ADR's scope (inherited from `trust::hmac_sha256`'s own, separate audit, ADR-021/ADR-005). |
| The `QuarantineAuditHook` fires exactly once per `quarantine()` call and its event type structurally cannot carry the secret's own bytes. | **CORRECT** — `test_audit_hook_fires_with_no_value`: exactly one event per call, correct `trigger`/`principal_id`/`ref_name`; `QuarantineAuditEvent` is a 3-field aggregate (`ref_name`, `trigger`, `principal_id`) with no field capable of holding arbitrary secret bytes, checked via a field-count-via-aggregate-init compile-time proof. |
| An unconfigured `SecretDetector` fails open to no-quarantine behavior. | **INCONCLUSIVE, claim reframed during implementation.** `scan_and_quarantine()` takes `SecretDetector&` (a required reference), not a nullable pointer — "unconfigured" is realized by the host simply never calling that function, which trivially returns content unchanged (not tested as a distinct null-check path, because there is no null-check path in the real API). The narrower, actually-implemented claim — "content the host never scans is never quarantined" — is trivially true by construction, not separately tested. |
| **New, found during implementation, not in the original §3**: `agent_initiated` refs are never grant-eligible, regardless of the tool argument's content. | **CORRECT** — `test_grant_eligibility_excludes_agent_initiated` and `test_agent_initiated_tool_never_grants_and_redacts_verbatim`: `grant_eligible_ref_names()` excludes every ref minted via `QuarantineSecretTool`, even immediately after quarantining real secret-shaped text through it. |

## 7. The decision

**Design B, as corrected during implementation (§5), is adopted and implemented.** It binds:
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
- A deployment that wires neither `SecretDetector` nor `QuarantineAuditHook` gets NO protection and NO
  audit trail at all — a real, named limitation of the delegation design, not a hidden one. Whether
  this is an acceptable default (fail-open on missing host configuration) versus something that
  should fail closed (refuse to run without both configured) is not decided by this ADR.
- **WIRED, real end-to-end evidence (2026-08-20)**: `QuarantineToolProvider`
  (`trust/secret_quarantine.hpp`, new) is a real `ContextProvider` conformer wrapping
  `QuarantineSecretStore`, contributing exactly one tool (`quarantine_secret`) — it occupies
  `AgentSession`'s `HistoryProviderT` slot directly, or composes alongside other contributors via
  `ComposedContextProvider`, with zero changes to `agent_session.hpp`.
  `tests/test_rt_agent_session_quarantine_tool.cpp`, 5/5 checks, drives a REAL round where the model
  calls `quarantine_secret` as an ordinary tool through the actual `ToolTable`/`invoke_tool()`
  pipeline (not a hand-built `EffectContext` calling the closure directly, the standalone unit test's
  own shape): the raw secret never appears anywhere in durable session history (recursing into the
  `ToolResult`'s own nested content — a real bug in this test's FIRST version, which only scanned
  top-level `ContentItem`s and so found neither leak nor marker, silently proving nothing until
  fixed), the `[quarantined secret: ...]` marker does appear, and `grant_eligible_ref_names()` stays
  empty end-to-end, confirming the I3 fix holds through the real pipeline, not just the isolated
  store. **A real, load-bearing constraint found while wiring this**: `AgentSession::
  history_provider()` cannot be reassigned after construction (a plain, default-constructed value
  member, the same limitation `history_and_skills_provider.hpp`'s own top comment already documents
  for a different reason) — and `QuarantineSecretStore` holds a `std::mutex`, making it neither copy-
  nor move-assignable regardless — so this test exercises the provider's DEFAULT (`nullptr`) audit
  hook; injecting a custom one requires a host-level construction API this ADR does not add. Full-tree
  rebuild: zero compile errors; full `ctest`: 187/197 (same 10 pre-existing, unrelated not-run targets
  as every other ADR in this batch).
- `hmac_sha256`'s real cryptanalytic strength is inherited, not re-proven here (§6).
- Whether a THIRD-PARTY-sourced secret (no grant-eligibility at all — not just no auto-grant) should
  be destroyed outright rather than kept retrievable — still open, unchanged by implementation.
- Windows-only, transitively, via `trust::hmac_sha256` (§2's implementation note) — not yet portable.
