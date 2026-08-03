# 018 — Identity, Authorization and Secrets

**Status:** Draft · **Depends on:** 007, 011, 012 · **Gate:** §7

## Goal

Establish who a request is from, what authority it carries across process and organization
boundaries, and how credentials are stored, resolved, and kept out of everything that persists.

## 1. Inbound identity

Every externally-originated request establishes a **principal** (007 §2) before any agent code runs.

| Surface | Mechanism |
|---|---|
| HTTP / AG-UI / OpenAI-compatible | OAuth 2.1 bearer / OIDC, mTLS, or API key (dev only) |
| A2A | The scheme declared in the Agent Card (API key, OAuth2, mTLS, OIDC) — 012 §4 |
| MCP (as server) | The MCP authorization spec — 011 |
| Embedded / in-process | Host-supplied principal; the host is trusted (007 §1) |
| Local CLI | Local user identity |

**Rules:**

- **Anonymous is a principal**, with its own (usually minimal) capability set — not a bypass.
- **Token audience is validated.** A token minted for another service is rejected. This is the
  confused-deputy control and it is not optional.
- **No token passthrough.** A token received inbound is never forwarded to an upstream service.
  Outbound calls use credentials issued for *this* engine, with delegation expressed as
  `on_behalf_of` (007 §2), never by relaying someone else's bearer token.

## 2. Authorization

Authorization happens in two places, and both are required:

1. **Admission** — may this principal start this run on this session/agent at all?
2. **Effect** — may this principal, through this agent, perform this specific effect? (007 §5)

Admission alone is insufficient (it cannot see arguments); effect checks alone are insufficient
(they run too late to reject a whole request cheaply). Session ownership is checked at the actor
boundary (005 §1), not by a later filter.

## 3. Outbound credentials

- Resolved through the **secret seam** at the point of use, never read into a config struct at
  startup and never passed as a message field.
- Scoped per `{provider|server|peer, principal-or-service}` so a credential's blast radius is
  bounded and revocation is targeted.
- **Rotation without restart** is a requirement: a rotated credential takes effect on the next
  resolution, and in-flight calls are not broken by rotation.

## 4. The secret seam

```cpp
struct SecretStore {                                  // concept
    ae::task<result<SecretLease>> resolve(SecretRef, EffectContext&);
};
```

- **`SecretRef` is a name, not a value**, and is what appears in configuration, documents (015 §5),
  and plugin manifests (009 §3).
- **`SecretLease` is short-lived, non-copyable, and redacted by construction**: its formatter, its
  serializer, and its debug output all print `***`. Making the *type* unprintable is the mechanism;
  remembering not to log it is not.
- Backends: environment (dev), OS keychain / DPAPI / Keychain, file with restrictive permissions,
  and external managers (Vault, cloud KMS) as seam backends.
- **Never in**: messages, history, checkpoints, recordings, telemetry, audit payloads, error strings,
  or plugin memory. Plugins receive resolved values only for a granted `Secret<name>` capability,
  per invocation (009 §5).

## 5. MCP and A2A specifics

- **MCP (2026-07-28)** hardened authorization in ways we must honour as a client: validate a present
  `iss` per RFC 9207 before redeeming an authorization code; key persisted client credentials by
  issuer and never reuse them across authorization servers, re-registering when the AS changes;
  specify an appropriate `application_type` during registration; and prefer **Client ID Metadata
  Documents** over Dynamic Client Registration, which is now deprecated. Exact requirements are
  enumerated in 011.
- **A2A** declares schemes in the Agent Card; `AUTH_REQUIRED` is the sanctioned mid-task credential
  request (012 §4).

## 6. Multi-tenancy

- **Tenant is a first-class dimension** of principal, session, memory scope, sandbox, quota, and
  audit — not a filter applied at query time.
- Cross-tenant access is denied at the actor boundary; a cross-tenant leak is a release-blocking
  defect class, tested with a dedicated suite.
- Resource limits and cost budgets are per tenant (Quark 022), so one tenant cannot starve another.

## 7. Promotion gate

- **G1** — negative suite: wrong audience, expired, wrong issuer, replayed, and token-passthrough
  attempts are all rejected, each with the correct classification.
- **G2 (secret hygiene)** — a canary secret is planted and every persisted artifact (logs, traces,
  audit, checkpoints, recordings, crash dumps, plugin memory dumps) is scanned; zero occurrences.
- **G3** — credential rotation takes effect without restart and without breaking in-flight calls.
- **G4** — cross-tenant access attempts across every surface (session, memory, artifact, sandbox
  workspace, audit query) are denied and recorded.
- **G5** — delegation chains attenuate: a derived principal never holds a claim its parent lacked.

## 8. Open questions

- **Q1** — Workload identity for agent-to-agent trust (SPIFFE-style) versus OAuth-only. OAuth covers
  human-delegated flows well and service-to-service less well.
- ~~**Q2** — Capability bearer tokens (007 Q1) would let the `remote` sandbox profile and remote
  plugins carry attenuated authority without a bespoke protocol.~~ **Resolved — see 007 §10 Q1 and
  `decisions/ADR-005-capability-bearer-tokens-cross-process.md`.**
- **Q3** — Whether the audit log needs its own signing identity so it can be verified by a third
  party.
- **Q4** — Agent identity in a registry sense: if agents are published and discovered, who vouches
  for the binding between an agent id and its operator?
