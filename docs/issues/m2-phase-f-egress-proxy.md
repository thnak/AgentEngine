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
- **F2.** (done) Policy-reachability tool (007 §9 G6) — new CI tooling enumerating `{capability
  kind, tool, taint level}` against whatever mechanical enforcement A3 (`CapabilitySet::subsumes`,
  ADR-009) actually implements. ADR-009 §9 already flagged this ADR's own `subsumes()` as exactly what
  such an enumerator would need to walk. **Size: L.**

  `include/agentengine/trust/policy_reachability.hpp`'s `enumerate_policy_reachability()` walks the
  real `CapabilitySet::contains()` per `{agent, tool, capability kind, taint}` cell;
  `tools/policy_reachability.cpp` is the CI-runnable tool, run against
  `tools/policy_reachability_fixture.hpp`'s reference set (exits 0 clean). Two RFC-vs-code gaps named
  explicitly rather than assumed: no 007 §5 declarative rule set exists yet (decision 4), so this
  enumerates the mechanical ceiling-vs-requirement shape instead; "taint level" has no graded
  vocabulary in code (a single bool, ADR-007), so taint is enumerated as that boolean, and the walk
  itself proves admission is taint-invariant today. The exit criterion's own demanded positive control
  (an over-broad grant a manual per-tool review would miss) is proven by
  `tests/test_policy_reachability.cpp`, not exercised by the CLI's own default run (see the breakdown
  doc's F2 entry for why). Verified on Windows and a fresh Linux container; see
  `docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`'s F2 entry for the full
  design/evidence writeup.

## Exit criteria

- A first-party egress proxy exists, is the default, and is proven to actually contain 008 §7's
  named abuse cases it's in scope for (SSRF to link-local/metadata endpoints, DNS-rebinding around an
  allowlist) with a positive control per 008 §9 G2's own pattern. **Met (F1).**
- The proxy is wired into at least one real consumer end-to-end, not proven in isolation only.
  **Met (F1 — `wasm`'s `http-request`.)**
- A CI tool enumerates the declared capability/tool/taint surface against the real enforcement
  mechanism. **Met (F2).**

**Phase F is complete** — F1 and F2 both done. With F2 done, Milestone 2 itself is complete (see the
breakdown doc's F2 entry).
