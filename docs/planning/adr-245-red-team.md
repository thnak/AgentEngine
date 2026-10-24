# ADR-245 red-team log

Work log for [ADR-245](../../decisions/ADR-245-sandbox-backends-in-a-separate-repository.md). The ADR's §8
summarises; this file keeps each round's findings and where each was folded in. Rounds 1–3 were one
independent agent across three revisions; round 4 was a fresh agent that had not seen the earlier
rounds. Every cited location was re-checked in the code before a finding was folded in.


Every cited location was re-checked before folding in.

**Round 1** (revision 1): not ready, 3 FATAL, 8 MAJOR, 3 MINOR. F1 `agent.*` loop and HandleRelay
checks were in moved files → §2.2 items 5–6. F2 `authorize_spec` runs inside backends → §2.2 item 1.
F3 descriptor hand-off left path resolution to the backend → §2.3. F4 read-write mounts → `StagingRoot`.
F5 facet forwarded authority → bytes only. F6 runtime version integer → link-time tag. F7 cross-repo
CI → non-required manifest check. F8 lost tests → kit, test host, G-S1 manifest. F9 PDF tool → moves.
F10 ADR-243 order → §2.7. F11 surfaces → deleted. F12–F14 figures, memory caps, gate controls.

**Round 2** (revision 2): not ready, 3 FATAL, 6 MAJOR, 2 MINOR.
- **R1 (FATAL)** handle passing into the jail is already falsified under AppContainer and would hand
  a guest raw sockets → HandleRelay stays wholly an engine RPC relay (§2.2 item 6), G-S6.
- **R2 (FATAL)** `SandboxSpec` carries `CapabilitySet`; `WasmBackend` decides capabilities; G-S2 was
  vacuous → `BackendSpec` and `backend_api/`, scope rule exempting engine components, G-S2 as a
  transitive-include lint.
- **R3 (FATAL)** interpreter start re-authorizes inside the backend → wrapper authorizes at facet start.
- **R4** stripping `EffectContext` dropped stop, deadline and attribution; `cap::Exec` and hostname
  pinning left in backends → `ExecControl`, wrapper decisions 2–3, G-S7.
- **R5** today's opens follow links; no Windows reparse, link-count or stream checks; FIFOs block →
  new primitives (§2.3), G-S3 extended with a live writer.
- **R6** a staging path is a race; Kata's staging needs root; Windows has no quota → `StagingRoot`
  handle, `fstatfs` check, disclosed residual.
- **R7** live read-write consumers not inventoried → §2.3 inventory.
- **R8** ordering cycle with ADR-243 §5 → §2.7.
- **R9** the ABI hash missed build options → §2.5, G-S8.
- **R10** the test host was a second guest implementation → one installed guest library (§2.4
  item 3), G-S5's hash check, G-S12.
- **R11** gate gaps → G-S6, G-S7, G-S12; G-S2 and G-S4 strengthened.

**Round 3** (revision 3): no FATAL; 6 MAJOR, 3 MINOR. Round 2's fixes for spec authorization,
`BackendSpec`/`ExecControl` and handle passing were found to hold against the code.
- **M1** the engine-resident exemption was unenforced and unnecessary for `NoSandboxBackend` → only
  `WasmBackend`, behind a private-constructor key, G-S13.
- **M2** the guest library is a Windows-only native executable; import gating was misattributed →
  Python source plus per-platform shim, Linux transport in the contract slice, source hash (§2.4
  item 3), G-S5.
- **M3** relay ids outlive an authority change → per-operation re-check and close on change, G-S14.
- **M4** relayed writes bypassed harvest → relayed file I/O targets the `StagingRoot`.
- **M5** `StagingRoot` verification insufficient; `RESOLVE_NO_XDEV` missing; paths at the OCI and ACL
  boundary → dedication checks, `RESOLVE_NO_XDEV`, handle-derived paths, G-S4 negatives.
- **M6** gate gaps → G-S13–G-S15; G-S1 excludes smoke tests; G-S6 names its enumeration method.
- **m1–m3** 8.3 and trailing dot/space collisions; bounded Linux read and `/proc`; ADR-242 §6 order
  and `AnySandboxBackend` timing named in §5.

**Round 4** (revision 4, fresh reviewer): no FATAL; 10 MAJOR, 8 MINOR. Folded into revision 5.
- **M1** under Kata the backend's nft/tc rules enforce the allowlist and `net_bytes`
  (`egress_policy.hpp:12-23`), so I2/I8 at the microvm edge would leave the engine → every guest's
  only network path is the engine's egress proxy; the backend enforces only "proxy or drop" (§2.2
  item 3, G-S16).
- **M2** the authority-bound rebuild covered only the VM; a persistent native-jail interpreter
  outlives a principal change; pooled backends may leave residue → the wrapper applies ADR-242 R3
  rule 1 to every persistent facet; kit residue gate (G-S17).
- **M3** harvest can be hung by leases and oplocks, and walks are unbounded → requirement H3, G-S3
  cases.
- **M4** deletions, renames, empty directories and mode bits unspecified → requirement H4 (diff
  against the copy-in baseline).
- **M5** no crash consistency; staging never swept → requirement H5, G-S18.
- **M6** concurrency and tenancy absent; Kata looks handles up by string with no owner check
  (`kata_backend.cpp:843-845`) → wrapper binds handles to owner (§2.2 item 8), G-S19.
- **M7** `Strict` is decided by a backend-declared strength; `register_backend` defaults to
  `eligible` (`sandbox_backend_registry.hpp:126`) → `backend_api/` registrations are `named_only`
  unless the developer states otherwise (§2.2 item 9).
- **M8** Kata needs hostnames (`/etc/hosts`, SNI), the image and carried env; `/proc/self/fd` is the
  wrong process for Kata → `BackendSpec` fields (§2.2); bind-source mechanism delegated to the
  contract-slice ADR as requirement H2.
- **M9** dedication checks contradict per-backend reality; export method unverifiable → checks scoped
  per backend (H1), export method backend-enforced and kit-tested.
- **M10** `quiesce()` underspecified; between-run pause and blackhole missing from the contract →
  requirement H6, G-S15 extended.
- **m1** G-S14 contradicted ADR-242 R3 rule 1 → authority is the run's; ids closed between runs.
- **m2** "never sees" overstated; key is accident prevention → restated "never decides".
- **m3** no gate that the engine calls backends only through the wrapper → G-S2 extended.
- **m4** MSVC layout options → in the ABI tag via `detect_mismatch`.
- **m5** guest-library hash and guest-side import gate → host-computed hash; the guest gate is not a
  boundary under `microvm`.
- **m6** I5 → `ExecOutcome` and harvested change sets recorded.
- **m7** hosted backends send worktree content off-host → the control-plane destination is a declared
  `NetOut` grant.
- **m8** coherence with ADR-242 §2, R2, CLAUDE.md, unjudged ADR-243, gate requiredness, signing key,
  PDF tools → §5 amendments and §2.6.
