# ADR-243 red-team log

Work log for [ADR-243](../../decisions/ADR-243-layer-and-developer-api.md). The ADR's §7 summarises; this
file keeps each round's findings and where each was folded in.

Every cited location was re-checked before folding in.

**Round 1** (2026-10-20): F1 layer order (`server/` below `engine/`, `AgentSpec` at the top) → `agent/`
holds `AgentSpec`/`Session`, `engine/` L5, `server/` L6. M1 L1 cycles → L1 order, `FilesystemAdapter`
to `vocab/`, `agent_spawn_capability` to `agent/`. M2 backends and code mode → `ToolDispatch`, backend
sub-map. M3 erasure dropped facets → facet list and tests. M4 store-backed state → in-memory `AnyState`.
M5 authority underspecified. M6 server sketch incomplete. M7 gate loopholes. Minor: seam table, wording,
order, citation rewrite.

**Round 2** (2026-10-24, fresh reviewer): not ready, 2 FATAL, 8 MAJOR.
- **F1** "narrowed and audited" contradicted ADR-244 §2.3 and the code, which refuse; `require_authority`
  is gone; `single_trusted_caller(caps)` did not match ADR-244's type → §2.2 restated on ADR-244.
- **F2** `result<T&>` cannot compile; clear was unspecified → `reference_wrapper`, `reset()` (§2.3–§2.4).
- **M1** one shared model client per agent mixes per-session state across tenants → factories (§2.2),
  G-A5's two-tenant test, alternative C.
- **M2** facet list incomplete; no runtime composer → five more facets and a runtime composer (revised in round 3: the existing `ContextProviderDescriptor`).
- **M3** G-A1 unfalsifiable (no `.clang-format`, trivial `grants`, scripted model, no in-repo backend)
  → G-A1 rewritten.
- **M4** G-A2 and G-A3 could not fail; cost mislabelled → thresholds, planted controls, frame
  allocation stated.
- **M5** missing I6, R6 and G-R5 gates; ADR-244 §6's delegations unanswered → G-A7–G-A9; presets
  carry the narrowing defaults; the PDF tool's backend follows ADR-245.
- **M6** "produced only by the bridge" unbuildable and killed the host-socket path; seam wording
  contradicted R7 → `PrincipalVerifier`, audited guarantee, R7 wording.
- **M7** ADR-243 and ADR-245 blocked each other → ADR-245's amendments folded in (§2.1, §2.3, G-A1,
  §5).
- **M8** order interleaved ADR-242 §6 without amending it; open-ended "1b first"; no concurrency
  protocol; archive citations → §5's order, infrastructure-bound 1b items no longer block this ADR,
  move map and rebase script, `PATHS.md`.
- **Minor:** figures re-measured; the `testing/` rule kept as a lint; `detail/` placement; telemetry
  per ADR-244; README status.

**Round 3** (2026-10-24, same reviewer as round 2): no FATAL; 7 MAJOR, 5 MINOR. Round 2's F1 and F2
fixes hold; the layer map has no new cycle.
- **M1** the factories said nothing about what stays shared, nor their signature; "impossible" was
  false → the scope table (per session, per tenant, engine-shared), `SessionContext`/`TenantContext`,
  per-tenant secret resolution, G-A5's tenant-recall test.
- **M2** the context holder already exists and aliases on copy; name and budget facets missing;
  attribution claim contradicted the single-provider path → build on `ContextProviderDescriptor`, deep
  `clone()`, refusal of a moved provider, the stated behaviour change.
- **M3** the sketch broke ADR-244 (no `Expiry`, no cap limits, a tenant field that does not exist) →
  rewritten on ADR-244's types; the tenant comes from `tenancy::per_claim`.
- **M4** G-A7 compared function objects → a canonical description, C++-only seams listed.
- **M5** G-A8 measured idle sessions, which never start the detached threads → mid-stream and MCP
  background tasks, Linux, baseline delta.
- **M6** `StagingRoot` in `worktree/` would pull `trust/` into `backend_api/` → handle type in
  `vocab/`, H2 primitives in `runtime/pal/`.
- **M7** no amendments section; silent dependency on ADR-245 → §5b with the fallback.
- **Minor:** G-A2 three-run medians; G-A3 relative to the direct call, no waiver; G-A1 behavioural
  `grants` check, linted test-only model hook, scoped column limit, no shell over `none()`; the
  time-limit row is not Kata-bound; slice 2 takes `AnySandboxBackend`; verifier wording; G-A9 proves
  `VerifiedPrincipal` cannot be built outside a verifier.
