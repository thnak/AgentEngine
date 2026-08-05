M2 Phase F — Cross-cutting ADR-track tasks (008 §10 Q3, 007 §9 G6)

## Context

The milestone-2 breakdown doc flags two items that go through the full design→red-team→prove→judge
cycle rather than an ordinary task, each producing its own ADR: the first-party egress proxy (008
§10 Q3) and the policy-reachability tool (007 §9 G6). Phase F is these two, tracked separately from
the sequential A–E phases because both are cross-cutting rather than owned by one earlier phase.

## Tasks

- **F1.** (done) First-party egress proxy. `decisions/ADR-011-first-party-egress-proxy.md` — see that
  ADR and the breakdown doc's own F1 entry for the full design/red-team/evidence writeup. Summary:
  `NetEgressBackend` concept + `HostEgressProxy` default
  (`include/agentengine/sandbox/net_egress_proxy.hpp`, `src/sandbox/net_egress_proxy.cpp`), wired into
  `wasm`'s `http-request` host import (`src/backends/wasm/wasm_backend.cpp`). Resolve-once-connect-to-
  verified-IPv4-literal (closes DNS-rebinding by construction), a real blocked-range table checked on
  the binary address (immune to encoding-bypass classes), CRLF-injection gate before any network
  activity, byte cap enforced mid-stream (declared cap or a 16 MiB hard ceiling), no redirect-
  following. Plain HTTP only and IPv4 only this milestone — both named, not silently assumed; HTTPS
  needs a follow-up ADR once a general-purpose TLS client exists. **Size: XL.**
- **F2.** (not started) Policy-reachability tool (007 §9 G6) — new CI tooling enumerating `{capability
  kind, tool, taint level}` against whatever mechanical enforcement A3 (`CapabilitySet::subsumes`,
  ADR-009) actually implements. ADR-009 §9 already flagged this ADR's own `subsumes()` as exactly what
  such an enumerator would need to walk. **Size: L.**

## Exit criteria

- A first-party egress proxy exists, is the default, and is proven to actually contain 008 §7's
  named abuse cases it's in scope for (SSRF to link-local/metadata endpoints, DNS-rebinding around an
  allowlist) with a positive control per 008 §9 G2's own pattern. **Met (F1).**
- The proxy is wired into at least one real consumer end-to-end, not proven in isolation only.
  **Met (F1 — `wasm`'s `http-request`.)**
- A CI tool enumerates the declared capability/tool/taint surface against the real enforcement
  mechanism. **Not yet met — F2 pending.**

Phase F is **not yet complete** — F1 done, F2 remaining.
