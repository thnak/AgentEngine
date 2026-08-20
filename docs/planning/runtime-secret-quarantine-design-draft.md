# Design draft: runtime secret quarantine (mint-at-runtime `SecretStore`)

**Status:** Design draft, workflow-reviewed 2026-08-20 (fixes applied below, see §6; 1 FATAL finding
closed by delegating durability to a host-injected hook rather than building a durable subsystem in
the engine — see §3). No code written. Independent of the other three drafts in this batch
(provenance, turn-middleware/compaction, content-triggered replay) — builds only on already-shipped
`trust/secret.hpp` (018 §4). Detection (§2a) is now its OWN host-provided seam, explicitly NOT
claimed as riding 017 §4's `ae:filter` points (those are unimplemented — see §2a). Grounded in
`docs/research/2026-08-20-compaction-provenance-chaining-prior-art.md` is NOT a dependency of this
draft; the relevant external grounding here is HashiCorp Vault's Transform tokenization
([docs](https://developer.hashicorp.com/vault/docs/secrets/transform/tokenization)) versus
Presidio's Encrypt/Decrypt operator
([docs](https://data-privacy-stack.github.io/presidio/anonymizer/)) — this draft follows Vault's
shape, not Presidio's (§1 below).

## 1. The gap, precisely

`trust/secret.hpp` (`SecretRef`/`SecretStore`/`SecretLease`, 018 §4) already gives AgentEngine a
capability-gated, zeroizing, unprintable secret-resolution path — but only in the **declare-then-
resolve** direction: an operator names a secret in config, a `SecretSource` (env/file) resolves it
at the point of use. This protects secrets the ENGINE always knew about.

It does nothing for a secret that shows up **incidentally inside ordinary content** — a user pastes
an API key into a chat message, a tool's stdout echoes a leaked credential, a web-search result
contains someone's exposed token. Nothing in this codebase detects that today (`ae:filter`,
017 §4, is declared vocabulary with zero implementation — confirmed by grep, zero hits under
`include/agentengine`).

**Why not just reuse `SecretStore` as-is:** `SecretRef{name}` today is a name the OPERATOR chose
ahead of time; there is no path for a name+value pair to be minted at runtime from detected content.
The one type with a `set()`-shaped write path, `InMemorySecretStore`, is explicitly test-only by its
own comment ("production code constructs `AgentEngineSecretStore`... never this").

## 2. The mechanism

Three new pieces, each mapping onto something already in this codebase's vocabulary:

### 2a. Detection — a host-injected seam, not an engine-shipped heuristic

Corrected from an earlier version of this draft, which claimed this rides 017 §4's `ae:filter`
`input`/`tool_result` points — those points are declared vocabulary with zero implementation
(confirmed: zero hits for filter/Filter under `include/agentengine`), and treating unimplemented
infrastructure as available overstated what exists today. It also cited `tool_optimizer_provider.hpp`
(`search_tools`) as precedent for a "cheap, deliberately non-semantic" detection style — that file is
real but lives only on the still-open, unmerged `tool-optimizer-provider-issue-15` branch (PR18), not
this repo's current default branch; citing it as shipped precedent overstated what's on this branch.

**Fix, and a deliberate scope reduction consistent with this project's own "AgentEngine adds no
storage engine of its own" posture (005 §2) applied to detection instead of storage**: detection is
not engine-owned logic at all. It is a host-injected callback, the same idiom this codebase already
uses repeatedly for exactly this shape of decision (`ApprovalDecider`, `MiddlewareTraceHook`,
`WorkflowSupervisor::CheckpointHook`) —

```cpp
class SecretDetector {
public:
    virtual ~SecretDetector() = default;
    // Host-provided. AgentEngine ships no regex/NER of its own -- a real deployment already has an
    // opinion here (Presidio, a vendor DLP scanner, a simple in-house regex list) and re-implementing
    // PII/secret detection inside the engine would duplicate a whole existing product category
    // (docs/research/2026-08-07-sensitive-input-and-secret-leakage-handling.md surveys nine of them)
    // for no benefit over just calling out to one.
    virtual std::vector<DetectedSpan> scan(std::string_view content) = 0;
};
```

This mirrors `SecretSource`'s own existing adapter shape exactly (`trust/secret.hpp:118-124`,
"Adapters... all model this one interface") rather than inventing a second extension convention.
AgentEngine's own contribution is the SEAM (where `scan()` is called, what happens to its output —
§2b-§2e), not the detection heuristic itself. A deployment with no detector configured gets no
quarantine behavior at all (fails open on detection, same as any optional host-injected hook
defaulting to `nullptr` elsewhere in this codebase) — this is a real, named limitation, not hidden.

Runs **per content item, independently** — no cross-contributor visibility needed (see §4 for why
this matters).

### 2b. Mint-at-runtime store, content-addressed

```cpp
class QuarantineSecretStore {
public:
    // Detects & extracts; if already quarantined (same value seen before), returns the EXISTING
    // ref instead of minting a duplicate -- name is a content hash of the detected bytes, not a
    // random id, so the same secret appearing via two different ContextProviders (e.g. live
    // history AND a memory recall of the same leaked value) collapses to ONE SecretRef without
    // either provider needing to know about the other. Mirrors this project's own content-
    // addressing precedent (025-Worktree-and-Virtual-Filesystem.md), applied to secrets instead of
    // files.
    [[nodiscard]] SecretRef quarantine(std::string_view detected_value, EffectContext& ctx);
};
```

Backing storage reuses `SecretStore`'s existing shape (`resolve(ref, ctx) -> result<SecretLease>`,
`trust/secret.hpp:276-278`) — `QuarantineSecretStore` satisfies the same `SecretStore` concept, so
every existing capability-gate/zeroize/unprintable guarantee applies unchanged; only the write path
(`quarantine()`) is new.

### 2c. Auto-grant, scoped to the extracting principal's run

Per `require_secret_capability` (`trust/secret.hpp:245-255`), resolving a `SecretRef` needs a
granted `cap::Secret{ref.name}` in `EffectContext::capabilities` — nothing grants this
automatically today, by design (I2). For a user who just typed their own key inline and expects the
agent to use it this turn, **the act of producing the value is itself the authorization event** —
not ambient trust, an explicit act by the principal. `quarantine()` mints the ref AND grants a
`cap::Secret` scoped to the current run/session for it, in the same step. A secret detected in a
THIRD-PARTY source (a tool result, a memory recall — not the user's own input) does **not** get an
auto-grant; it is quarantined but stays unresolvable without a separate, explicit operator-level
grant — the extraction event alone is not an authorization event when the principal didn't produce
the value themselves.

### 2d. Replacement text

The detected span is replaced in-place with a reference marker naming the `SecretRef` (matching the
user's own phrasing: *"API key của bạn là: có thể trích từ API Storage"*) — never the raw value,
never an embedded ciphertext (unlike Presidio's Encrypt operator, §0 above) — an attacker who
captures the transcript alone recovers nothing; the token is opaque without a `SecretStore` round
trip, matching Vault's tokenization guarantee exactly.

### 2e. The `quarantine_secret` tool — the agent-initiated path

A zero-capability management tool — `Tool<T, EffectClass<pure>>`, grants nothing new, only triggers
2b/2c/2d on content the model itself flags. (An earlier version of this draft cited `search_tools`/
`mount_tool` from PR18's `ToolOptimizerProvider`, `tool_optimizer_provider.hpp`, as the precedent for
this trust shape — that file is real but lives only on the still-open, unmerged
`tool-optimizer-provider-issue-15` branch, not this repo's current default branch. The trust shape
itself — a `Tool` conformer declared with `EffectClass<pure>` and no `Capabilities<...>` — needs no
external precedent to justify; it follows directly from 006's own `EffectClass`/`Tool` vocabulary,
which is real, merged, and does not depend on PR18 landing.) This is the "loop back when
the agent notices something suspicious" case, and it needs **no new turn-replay mechanism**: the
model calls this tool mid-round, gets back the redacted replacement text, and continues the SAME
round with clean data — an ordinary step in `AgentSession::run_rounds()`'s already-existing
multi-round tool-call loop. `005-Sessions-State-and-Memory.md` §1 already names redaction as a
legal, audited history-rewrite category ("compaction, redaction, fork — never an incidental
mutation"); this is the first real consumer of that category triggered by the agent itself rather
than an operator/filter.

**What this tool does NOT cover**: a secret already quoted in a model response that already
committed to history before anyone noticed. That is `content-triggered-response-replay-design-
draft.md`'s scope (a separate, heavier mechanism this draft does not need and does not depend on).

## 3. Retention tension with 005 §4's compaction invariant, and the audit-durability FATAL fix

005 §4 states compaction "never silently deletes — the pre-compaction history remains in the
durable log." Applied verbatim to secret quarantine, this would be **wrong**: the whole point is
that the plaintext must not persist anywhere outside the capability-gated `QuarantineSecretStore`
— retaining it in an audit/durable-log copy defeats the mechanism. The audit span (005 §1's
"explicit, audited operation") records **metadata only** — trigger, ref name, timestamp, triggering
principal/tool — never the value. The value lives exactly once, in `QuarantineSecretStore`, gated
like any other `SecretStore`. This part of the original proposal survives review unchanged.

**What did not survive review**: this draft originally left WHERE that metadata record lives
unspecified, implicitly leaning on `invoke_tool()`'s existing step-10 audit record. The review
workflow found that record is documented, by `tool_pipeline.hpp`'s own top comment, as "a minimal
in-memory struct; 016's full span/telemetry shape is out of scope" — not durable. This made §5's own
abuse-surface question (can a manipulated model quarantine non-secret content to launder it out of
the transcript, undetected) answer itself: no attacker suppression trick is even needed, because
nothing guarantees the metadata record survives process end, a crash, or is visible to an operator
at all today. FATAL.

**Fix, and the same delegation shape this project already uses repeatedly rather than a new
in-engine durable subsystem**: AgentEngine does not own audit durability any more than it owns
session storage (005 §2: "AgentEngine adds no storage engine of its own") or secret-detection
heuristics (§2a's own fix, above). `quarantine()` fires a `QuarantineAuditHook` — an optional,
host-injected callback (`nullptr` by default), same shape as `MiddlewareTraceHook` (`middleware.hpp`,
ADR-033 §4) and `WorkflowSupervisor::CheckpointHook` — carrying exactly the metadata named above and
nothing else. AgentEngine's guarantee is narrow and structural: the hook fires, synchronously, before
`quarantine()` returns, every time, with a value that can never be the secret's own bytes (the
`QuarantineAuditEvent` type carries no field capable of holding it — matching `SecretLease`'s own "no
`std::string` conversion" trick, `trust/secret.hpp:210-229`, applied to the audit event type instead
of the secret type). What the HOST does with that event — write it to its own durable log, forward
it to a SIEM, alert an operator in real time — is entirely the host's decision and outside this
draft's scope, the same way `ApprovalDecider`'s actual approval POLICY is the host's decision and not
this codebase's. A deployment that wires no hook gets no durable record — a real, named limitation
(matching `MiddlewareTraceHook`'s own "nothing in this codebase yet exists to feed it into" residual,
ADR-033 §5), not a silent gap.

## 4. Why this does NOT need the chain/terminal-stage mechanism

Because naming is content-addressed (2b), duplicate detections across independent, non-communicating
`ContextProvider`s collapse to one `SecretRef` for free — no contributor needs to see another's
output. This draft is a pure `ae:filter`-point transform (017 §4), fully expressible under today's
fan-out `assemble_context()` with zero changes to it. It ships independently of
`context-provider-provenance-design-draft.md` and `context-turn-middleware-and-compaction-design-
draft.md`.

## 5. Open questions for the review workflow

- **Abuse surface**: can a manipulated model call `quarantine_secret` on content that ISN'T actually
  a secret, to strip inconvenient legitimate content from its own transcript? The zero-capability
  trust shape (2e) means the tool itself grants nothing, but the CONTENT MUTATION still happens.
  Does §3's metadata-only audit span give an operator enough signal to catch this after the fact?
- **False-negative risk of a cheap, non-semantic detector** (2a) — how much is "good enough," given
  017 §1's own honest framing that heuristic content-layer defenses are mitigations, not the primary
  control (007 capabilities/006 approval carry that load)?
- **Resolved: keep the default auto-grant scoped to the current run**, as originally drafted — do
  not widen to session scope by default. Widening a capability grant's temporal scope based on an
  inference about what the user "expects" (rather than an explicit, per-use act) sits close to the
  ambient-authority shape I2 exists to prevent, even though the grant itself stays named and
  explicit. Content-addressed dedup (§2b) does not resolve this tension — it addresses VALUE
  duplication, not GRANT lifetime, and the two are independent questions. If a real deployment needs
  the same secret reusable across multiple runs in one session, that must be its own explicit,
  separately-approved grant action, not a silent default change.
- **Does quarantining a THIRD-PARTY-sourced secret (no auto-grant) still need to exist as a
  capability-resolvable ref at all**, or should it just be destroyed outright (true redaction, zero
  retention) since nobody is authorized to use it? This draft currently keeps it retrievable
  (matching Vault's default), which may be wrong for content nobody actually owns.

## Red-team findings (workflow review)

Reconciled from three independent reviews (connectivity/orphan audit, feature-advocate,
safety-advocate) run against all four drafts, cross-checked against real code
(`tool_pipeline.hpp`, 017 §4, `OpenQuestions.md`).

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | §3/§5's accountability argument leans entirely on the audit span, but `tool_pipeline.hpp`'s own top comment states step 10's audit record is "a minimal in-memory struct; 016's full span/telemetry shape is out of scope." §5's own open question ("can a manipulated model call `quarantine_secret` on non-secret content to launder it out of the transcript, and does the metadata-only audit span catch it after the fact") answers itself: no attacker suppression trick is even needed, because nothing guarantees the metadata record survives process end, a crash, or is visible to an operator at all today. | FATAL | This draft cannot ship independently of a durable sink for `quarantine_secret` specifically — route its audit record through the checkpoint/history log (redaction-as-history-rewrite, 005 §1, which §2e already gestures at) rather than the in-memory-only span, or explicitly block this draft on 016. |
| 2 | §2a's and §2e's precedent for "cheap, non-semantic" detection style and the zero-capability-tool trust shape both cite `tool_optimizer_provider.hpp` (`search_tools`, `mount_tool`, "PR18") with specific line numbers — this file does not exist anywhere in the repo (confirmed by grep across `include/`, `src/`, `tests/`, and every `.md` file; the only hits are inside this draft and the provenance draft). The zero-capability-tool trust shape argument is otherwise unbacked by real precedent. | Must-fix | Find a real precedent for the detection-heuristic style and the zero-capability-tool trust shape, or drop the claim that this reuses existing precedent — as written, §2a's detection-style argument has no real backing left once the citation is removed. |
| 3 | §2a places detection "at the `ae:filter` `input`/`tool_result` points (017 §4) already declared for exactly this class of check" — those points are declared-but-unimplemented (zero hits for filter/Filter under `include/agentengine`, confirmed). The turn-middleware draft names this exact gap explicitly as an open question (its own §7); this draft, despite declaring itself "independent of the other three drafts," never acknowledges the gap, treating unimplemented infrastructure as available. | Must-fix | State explicitly that `ae:filter`'s `input`/`tool_result` points do not exist yet and that this draft's detection mechanism needs its own insertion point (or depends on the turn-middleware draft's future filter-wiring work) until they do — remove the implied "already declared for exactly this" framing, which currently reads as though the seam is ready to use. |
| 4 | §5's own Q3 leaves the auto-grant scope (2c) genuinely unresolved between "current run" and session-wide. Widening a capability grant's temporal scope based on an inference about what the user "expects" (rather than an explicit, per-use act) sits close to the ambient-authority shape I2 exists to prevent, even though the grant itself stays named and explicit — content-addressed dedup (raised as a counterargument) addresses value duplication, not grant lifetime, so it doesn't actually resolve this tension. | Worth-noting (resolved: keep run-scope default) | Keep the default auto-grant scoped to the current run, as currently drafted — do not widen to session scope by default. If a real deployment needs cross-run reuse of the same secret within a session, that should be its own explicit, separately-approved grant action, not a silent default change. Document this reasoning in §5 to close Q3 rather than leaving it open. |

**Applied**: finding 1 (FATAL — §3 now delegates audit durability to a host-injected
`QuarantineAuditHook`, matching `MiddlewareTraceHook`/`WorkflowSupervisor::CheckpointHook`), 2 (§2e
citation corrected), 3 (§2a rebuilt as a host-injected `SecretDetector` seam, no longer claims to
ride the unimplemented `ae:filter` points), 4 (§5 Q3 resolved to run-scope) are all incorporated
above. Net effect of 1 and 3 together: this draft now asks the ENGINE to own less — a seam plus a
narrow structural guarantee (the event fires, it can never carry the secret's own bytes), not a
detection heuristic or a durable-storage subsystem — consistent with this codebase's existing
"AgentEngine adds no storage engine of its own" posture (005 §2) applied one layer further than
before.
