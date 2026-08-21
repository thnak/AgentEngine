# 007 — Capability and Trust Model

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 006, 008, 018 · **Gate:** §9

## Goal

Define who may do what, on whose behalf, and how that authority is represented, narrowed, checked,
and audited. This RFC owns invariants **I2** (no ambient authority), **I3** (model output is data,
never authority), and **I4** (every effect is attributable). Everything the engine does that
touches the world outside the process passes through here.

## 1. Threat model

Stated plainly, because a security design without one is decoration.

**Assumed hostile:**
- Model output — text, tool arguments, and generated code. Models are steerable by anything in
  their context.
- Any content retrieved from outside the trust boundary: tool results, web pages, documents, MCP
  resources, memory/retrieval hits, remote agent responses. This is the prompt-injection surface.
- Third-party plugins and MCP servers, including ones that were benign yesterday (rug-pull and
  tool-poisoning are in scope).
- Code executed in a sandbox, unconditionally.

**Assumed trusted:**
- The host process, its configuration, and first-party native code.
- The operator who writes policy.
- The OS kernel / hypervisor providing the isolation primitive of the selected profile (008).

**Explicit non-goals:** we do not defend against a compromised host process, a malicious operator,
side-channel extraction across hardware boundaries, or a model provider acting maliciously with
data already sent to it.

## 2. Principals

```
Principal = { id, kind ∈ {human, service, agent, anonymous}, claims[], issuer, expiry }
```

- Every run executes **on behalf of exactly one principal**, established at admission (018) and
  immutable for the run's lifetime.
- A **sub-agent or delegated call runs as a derived principal** carrying `on_behalf_of`, never as
  the host and never with elevated claims. Delegation chains are recorded and depth-bounded.
- The principal is present in every span, every audit record, and every outbound protocol request.

## 3. Capabilities

A capability is an **unforgeable handle authorizing one class of effect**, held by the host, passed
explicitly, and revocable.

| Capability | Grants | Parameters |
|---|---|---|
| `FsRead<mount>` | Read under a mount | mount id, path prefix, size cap |
| `FsWrite<mount>` | Write under a mount | + quota, file-count cap |
| `NetOut<host>` | Outbound connections | host/port/scheme allowlist, byte cap, method restrictions |
| `NetListen` | Inbound listening | rarely granted; ports |
| `Secret<name>` | Resolve one named secret at point of use | name, TTL |
| `ToolCall<name>` | Invoke a named tool from inside sandboxed code | tool name |
| `Exec<profile>` | Create a sandbox of a profile | profile, resource limits |
| `Clock` | Real time (vs. virtual) | resolution |
| `Entropy` | CSPRNG | — |
| `Env<key>` | Read one environment variable | key |
| `AgentCall<agent>` | Invoke another agent | agent id, depth budget |
| `Schedule<max_horizon, max_active>` | Register a durable wake condition that outlives the current turn (006 §6b, 019 §2) | max horizon into the future, max concurrently-active registrations |
| `Background<max_concurrent>` | Detach a `Backgroundable` tool call (006 §5) from the turn that invoked it (006 §6b) | max concurrently-running detached calls |

**The depth budget is proven, in-process only, by `decisions/ADR-006-agent-spawn-depth-budget-bound.md`**
(`trust/spawn_budget.hpp`) — resolves the depth half of 026 §9 Q1 / OQ-14 (OpenQuestions.md).

**Properties:**

1. **Empty by default.** A sandbox, a plugin instance, or a tool invocation with no explicit grants
   has *none* of these. There is no API that grants "all". (CONVENTIONS §Security.)
2. **Attenuation only.** Any derivation — sub-agent, nested sandbox, plugin calling a tool — may
   produce a **subset**, never a superset. Attenuation is the only operation; there is no `widen`.
3. **Per-invocation binding.** Handles materialize at step 7 of the tool pipeline (006 §3) and are
   revoked at step 10. Retention across calls is a defect class with a dedicated test.
4. **Unforgeable in-process** (handle types with private construction) and **unforgeable across the
   sandbox boundary** (008: the boundary itself grants nothing the host did not hand it).
5. **Parameterized, not boolean.** `NetOut` without an allowlist is not a capability, it is a hole.

## 4. The taint rule (I3)

The mechanism, not just the principle:

- Model-originated and externally-retrieved content is **`Tainted<T>`** (003 §2), transitively.
- **No capability-granting or policy-deciding API accepts a tainted value.** This is enforced by
  the type system: `Tainted<std::string>` has no implicit conversion to the `std::string_view` those
  APIs take.
- Converting tainted → trusted requires an explicit, named, logged **declassifier**: a schema
  validation, a strict enum/allowlist match, a canonicalized-path check against a mount, or an
  operator approval. A declassifier that does no checking is a review-blocking defect.
- **Concretely forbidden:** deriving a mount from a model-supplied path; deriving an allowed host
  from a model-supplied URL; deriving an approval decision from model-supplied text ("the user said
  it's fine"); selecting a policy by a name the model chose.

**Amendment (2026-08-10, ADR-023 — `call_provenance` and the text-derived declassifier).** A tool
call can arrive two ways that look identical once parsed but carry different trust: a
**`vendor_structured`** call is a real field in a provider's own wire response (`tool_calls[]`/
`tool_use`) — the serving layer itself committed to "the model invoked a tool," today's only path,
unchanged trust posture. A **`text_derived`** call is pattern-matched by the host out of free-text
`content` that leaked raw, non-JSON-field response-format tokens (OpenAI Harmony, DeepSeek/Hermes/
Qwen tool-call delimiters — `docs/research/2026-08-10-provider-and-harmony-adapter-landscape.md`).
The second case is exactly this section's own rule in a new shape: **model-supplied text, re-parsed
heuristically, is never itself an authorization decision** — a `text_derived` call is `Tainted`
regardless of how confidently it parsed.

ADR-023's red-team pass (§4b) found the naive declassifier unsafe in a specific way worth stating
here, not just in the ADR: schema-validating a `text_derived` call's arguments and then treating it
as ordinary is **not** a valid declassifier under this section's own list, because a schema only
bounds *shape*, not *intent* — an attacker who can choose arguments freely within a valid schema
is not constrained by validation alone, and (worse) the resulting call is structurally
indistinguishable from a `vendor_structured` one to any policy rule that doesn't itself check
provenance. The declassifier this RFC actually admits is a **strict allowlist match against the
target tool's own declared shape**, not its arguments: a `text_derived` call auto-declassifies only
when the tool's entire capability ceiling is provably inert (read-only/informational — no
`FsWrite`/`NetOut`/`NetListen`/`Secret`/`EnvWrite`/`Exec`/`ToolCall`/`RunnerCall`/`AgentCall`/
`Schedule`/`Background`/`Elicit`; see `trust::is_inert_for_text_derived_declassification`,
`trust/capability.hpp`) **and** the tool is declared `effect_class::pure` (019 §3). Every other
`text_derived` call falls to the other admitted declassifier, **operator approval** — unconditionally,
even overriding a tool's own `Approval<never_require>` declaration, because that declaration was
authored for the trusted, vendor-structured path, not for text reconstructed from a raw leak.
Enforced at exactly one point, `core/tool_pipeline.hpp`'s `invoke_tool` step 5 — see that function's
own comment for the mechanism, and `tests/test_tool_pipeline.cpp`'s `ADR-023 P2-T2` case for the
regression test encoding the confused-deputy scenario this amendment exists to close.

**Amendment (`decisions/ADR-070-host-configurable-responsibility-boundary.md`).** That ADR names a
formal, bounded pattern — the "Delegated Decision Seam" — for shifting part of AgentEngine's own
enforcement responsibility to the consumer dev (capability/sandbox defaults, approval grading,
`ContextProvider`/`Workflow` configuration), explicitly so that request does not become an argument
for loosening THIS section's own mechanism. ADR-070 §4a checks its own new seams against, and leaves
untouched: the `text_derived` closed declassifier allowlist above (a host-supplied `PolicyDecider`
is structurally barred from this path, proven by a tripwire test — ADR-070 §5a, §6 point 2),
`Tainted<T>`'s single, ungeneralized `unsafe_view()` escape hatch, and every other item this
section's own text already establishes. Read ADR-070 before proposing a new host-facing seam
anywhere in the capability/taint/approval surface — it is the checklist, not a precedent for
skipping this section's own rules.

## 5. Policy

Policy is **declarative, versioned configuration** — not code, not model-interpreted, not editable
by an agent at runtime.

```
rule {
  match  { tool: "fs_write", capability: FsWrite, path_under: "${workspace}" }
  decide auto_approve
}
rule {
  match  { capability: NetOut, host_not_in: allowlist }
  decide deny
}
rule {
  match  { tool: "*", taint: high, capability: FsWrite }
  decide require_approval
}
```

- **Default deny** for anything not matched.
- Evaluation is **total and deterministic** — no regex catastrophes, no network lookups, no model
  in the loop. Same inputs, same decision, every time (an I5 requirement).
- Policy decisions are recorded with the rule id that fired, so "why was this allowed?" has an
  answer that is not archaeology.

## 6. Trust tiers for code

| Tier | Examples | Isolation | Capability posture |
|---|---|---|---|
| **T0 Host** | Engine core, first-party native tools | none (is the host) | full; audited by code review |
| **T1 Vetted plugin** | Signed first-party WASM components | Component sandbox | declared in manifest, operator-approved |
| **T2 Third-party plugin / MCP server** | Community components, external servers | Component sandbox / process+network | least privilege, explicit grants, revocable |
| **T3 Model-generated code** | Code interpreter, CodeAct | Sandbox profile (008) | empty by default; grants are per-execution |

**Rule:** a tier may never obtain the authority of a lower-numbered tier. In particular, T3 calling
back into a tool (`ToolCall` capability) executes that tool with **T3's** capability set, not the
agent's — otherwise the sandbox is decorative.

## 7. Supply chain

Third-party code is a trust decision made *before* runtime:

- **Plugins are signed**; signature and publisher are verified at load, and the manifest's declared
  capabilities are shown to the operator for approval (009).
- **Pinning by digest** is the default for plugins and MCP servers; "latest" is opt-in.
- **Change detection:** an MCP server whose tool list, descriptions, or schemas change is
  re-approved rather than silently trusted — this is the tool-poisoning defense, and it is cheap
  because 011 already caches list results by digest.
- **A revoked plugin or server is unloadable at runtime**, and in-flight uses are canceled.

**Registry presence is not a trust signal.** The official MCP Registry is in preview and states that
consumers *"should assume minimal-to-no moderation"* — it explicitly does not remove servers with
known security vulnerabilities. Publication proves namespace ownership, nothing more. No code path
in this engine may treat presence in any registry, index, or marketplace as evidence of
trustworthiness; the controls above carry the full weight
([research](docs/research/2026-mcp-ecosystem.md) §1, 011 §9).

## 8. Audit

Every effect writes an audit record: `{timestamp, run_id, session_id, agent_id+version, principal,
delegation chain, effect kind, target, capability used, policy rule id, decision, result, bytes,
duration, trace/span id}`.

- **Append-only, tamper-evident** (hash-chained), on a separate seam from telemetry so that dropping
  metrics under load never drops audit.
- **Secrets and tainted payloads are never recorded in plaintext** — digests and lengths only
  (018).
- The audit stream is the ground truth for I4; a telemetry-only "audit" is not one.

## 9. Promotion gate

- **G1 (ambient authority)** — a scan/test proves no capability-granting API is reachable without a
  handle parameter; a negative control (an intentionally ambient API) is detected by the same check.
- **G2 (taint)** — a compile-fail test suite: each of the §4 "concretely forbidden" derivations
  fails to compile or fails at the declassifier, and the positive controls compile.
- **G3 (attenuation)** — fuzzed derivation chains never produce a capability outside the parent's
  set; a deliberately buggy `widen` implant is caught by the same property test.
- **G4 (revocation)** — a retained handle used after its invocation fails closed; an unloaded
  plugin's in-flight call is canceled and cannot complete an effect.
- **G5 (attribution)** — under a randomized workload with injected failures, every effect in the
  audit log reconciles 1:1 with an emitted span, with zero unattributed effects.
- **G6 (policy reachability, §10 Q3)** — a reachability tool enumerates every `{capability kind,
  tool, taint level}` combination against a reference policy set; every combination's reached
  decision matches a hand-computed oracle, and every rule in the set is exercised by at least one
  combination (dead-rule detection).

## 10. Open questions

- ~~**Q1** — Whether capabilities should be representable as **bearer tokens** (macaroon-style, with
  caveats) so they can cross a process boundary to a remote sandbox without a bespoke protocol.
  Attractive for the `remote` profile (008); adds a crypto dependency and a revocation problem.~~
  **Resolved by `decisions/ADR-005-capability-bearer-tokens-cross-process.md`: yes, for the
  `ExpiresAt`/`PathPrefix` caveat classes proven there** — accepted narrowly, not as a blanket
  replacement for host-side state. The revocation problem this question anticipated is real and
  confirmed (a minted token is valid until its own caveats lapse, with no way to unmint it early);
  the ADR's answer is to use a host-side registry (also proven there) instead of a bearer token for
  any capability that needs immediate revocation, rather than solving revocation inside the token
  mechanism itself. Performance was measured **inconclusive**, not favorable — see the ADR §9 for
  what a follow-up measurement needs before that claim can be settled either way. The executed,
  red-teamed, ASan-clean implementation is real code at `include/agentengine/trust/capability_token.hpp`
  and `src/trust/capability_token.cpp` (macaroon-style HMAC-chain, narrow caveats).
- ~~**Q2** — Span-level taint (003 Q3) would make declassification far more precise.~~ **Resolved,
  No (OQ-5, 2026-08-04):** see 003 §8 Q3 and `decisions/ADR-007-span-level-taint-vs-per-item.md`.
  The decisive reason lives here, not there: G2's compile-fail proof that `Tainted<T>` has no
  implicit conversion is a whole-value, compile-time property. Span-level taint would need a
  runtime interval check instead, weakening I3's static half for every text-touching path in
  exchange for less over-tainting friction — a precision gain, not a security fix, since per-item
  taint never under-taints; the ADR's prove also found a naive span-aware `concat` under-taints on
  its first implementation attempt, a security-relevant bug class per-item taint structurally
  cannot have. Kept per-item.
- ~~**Q3** — Whether policy should have a formal semantics + solver (decidable, verifiable) rather
  than ordered rules with default-deny.~~ **Resolved, No solver — the existing rule shape already
  gives verifiability for free
  (2026-08-04):** a solver's value is answering "can any rule combination reach capability X?", but
  §5's rule language is already a finite match over a bounded set of fields (`tool`, `capability`,
  `path_under`, `host_not_in`, `taint`, ...) with no recursion and no unbounded quantifiers — that is
  already decidable by **exhaustive enumeration**, not general theorem proving. Building a
  formal-semantics stack to answer a question brute-force search over a finite rule set already
  answers would add a second language (with its own soundness-relative-to-the-runtime proof
  obligation — the solver's model of the system is not the system) for a property already reachable
  the way this RFC proves everything else (G1–G5: scan, fuzz, enumerate — never a solver). Concretely
  adopted instead: a **reachability tool**, run at CI time, that enumerates every `{capability kind,
  tool, taint level}` combination against the full deployed rule set and reports the decision each
  reaches — surfacing dead rules (never matched) and unintended default-deny fallthrough (a
  combination nobody wrote a rule for, which may or may not be intentional) as findings an operator
  reviews, not proofs a solver asserts. Folded into the promotion gate as new **G6**: no
  `{capability, tool, taint}` combination in a reference policy set reaches a decision other than the
  one a hand-computed oracle expects, and every rule in the set is exercised by at least one
  combination (dead-rule detection).
- ~~**Q4** — Agent identity standards: OAuth/OIDC covers principals today, but workload-identity
  (SPIFFE-style) for agent-to-agent trust is unresolved (018).~~ **Resolved, not either/or — SPIFFE
  is an implementation of an already-listed A2A scheme, not a competing one (2026-08-04):** 018 §1
  already lists `mTLS` among A2A's Agent-Card-declared auth
  schemes; SPIFFE-style workload identity (short-lived X.509-SVID, automatically rotated, verified as
  mTLS) is one way to *implement* that existing scheme, not a fourth mechanism needing new plumbing.
  OAuth/OIDC stays the default for calls to/from arbitrary A2A peers — it needs no prior trust-domain
  federation and is what a publicly reachable agent card can assume its caller has. SPIFFE-backed
  mTLS is adopted as the fit for the case OAuth serves worst (the original framing: "service-to-service
  less well" — client-credentials grants typically mean a shared AS and long-lived static secrets):
  service-to-service calls within an operator-controlled trust domain (their own multi-deployment
  estate, or a federated partner via SPIFFE trust-domain federation), and 020 §3b's triggered runs
  with no calling human to derive a principal from. This mirrored a choice already made one layer
  down — Quark 020 §1's `NodeAuthority` seam named SPIFFE IDs as a pluggable cluster-admission
  backend alongside a shared CA — rather than inventing a different answer for the principal layer
  (historical: that "layer down" no longer exists in AgentEngine — ADR-037 removed Quark, and
  AgentEngine has no multi-node cluster story at all now; the reasoning above stands on its own for
  the principal layer, it just no longer mirrors a live lower layer).
  X42 (OpenQuestions.md OQ-9), if adopted, would be a governance layer *above* this (cross-
  organizational trust federation), not a replacement for it. Full text and the identical resolution
  for the 018-side duplicate: 018 §8 Q1.
