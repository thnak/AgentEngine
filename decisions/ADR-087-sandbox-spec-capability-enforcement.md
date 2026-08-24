# ADR-087 — `SandboxSpec::capabilities` becomes the real authority behind `mounts`/`net` for every
# mount/net-shaped `SandboxBackend` (KataBackend, LinuxNativeJailBackend, NativeJailBackend), not
# Kata alone

**Status:** Proposed (design → independent red-team → fix → implement → local verification
complete, 2026-08-24; awaiting project-owner Judged sign-off).

**Relates to:** `decisions/ADR-086-kata-backend-slice-2-enforcement.md` §5.1 (the residual this ADR
closes — "`SandboxSpec::capabilities` remains entirely unenforced... Slice 3+ work, not started
here"), `decisions/ADR-071-native-unsandboxed-process-execution-providers.md` (precedent for adding
a purpose-built capability kind instead of overloading an existing one), `decisions/ADR-070-host-configurable-responsibility-boundary.md` (the Delegated Decision Seam shape this design's opt-in
mirrors), `008-Sandbox-and-Isolation.md` §2 ("empty-by-default authority... nothing is reachable
that the spec did not grant"), `007-Capability-and-Trust-Model.md` (`CapabilitySet`, I2/I3). Full
design record, kept as the uncompressed history including the pre-red-team draft and every finding:
`docs/planning/sandbox-spec-capability-enforcement-design-draft.md`.

## 1. The question

KataBackend Slice 2 (ADR-086) named, honestly, that `SandboxSpec::capabilities` remained entirely
unenforced. The obvious next step — enforce it in `KataBackend::create()` alone — was rejected
before any code was written: reading every real `SandboxBackend::create()` confirmed
`LinuxNativeJailBackend` and the Windows `NativeJailBackend` have the IDENTICAL gap, and the only
backend that reads `spec.capabilities` at all is `WasmBackend`, via a structurally different
mechanism (WASI component imports) that doesn't generalize. A Kata-only fix would create a real
per-backend divergence: the same `SandboxSpec` would mean two different things depending on which
backend a host selected via `SandboxProfile<P>` — exactly the kind of drift `008 §2`'s "every
backend must provide, or it is not a backend" framing rejects. Project-owner direction (2026-08-24)
scoped this generalized, not Kata-only.

## 2. The resolved design

Two new capability kinds, `cap::SandboxMount`/`cap::SandboxNetOut` (`trust/capability.hpp`) —
deliberately NOT a reuse of `cap::FsRead`/`FsWrite`/`NetOut`. Those are `mount_id`-keyed, resolved
through a Worktree/`FileSystemAdapter`'s own mount registry (025); `SandboxSpec::MountSpec` names a
raw host filesystem path directly, with no adapter indirection. Conflating the two would be a real
capability-confusion hazard: a caller holding `cap::FsWrite{mount_id="workdir", path_prefix="/src"}`
— scoped to what ONE `FileSystemAdapter` instance exposes — has no relationship to an
attacker-influenced literal host path a `SandboxBackend` would bind-mount; treating them as
interchangeable would let a Worktree-scoped grant silently authorize an unrelated raw host bind
mount. `ADR-071` already established this exact precedent (`NativeExec` as its own kind rather than
overloading `Exec`).

A single free function, `agentengine::authorize_spec(SandboxSpec const&) -> result<void>`
(`sandbox/sandbox.hpp`), called identically, first-thing, by every real mount-granting entry point:
`KataBackend::create()`, `LinuxNativeJailBackend::create()`, `NativeJailBackend::create()` (Windows),
and `NativeJailBackend::create_python_worker()` (Windows — the mediated Python worker's own separate
entry point). Enforcement is scoped to presence of the relevant grant kind
(`CapabilitySet::sandbox_mount_grants()`/`sandbox_net_out_grants()` non-empty), not to whole-
`CapabilitySet` emptiness — every real call site today builds specs with zero grants of either kind,
so behavior for them is byte-for-byte unchanged (ADR-070's Delegated Decision Seam shape: explicit
opt-in, fails closed once engaged, changes nothing for a caller that hasn't opted in).

## 3. Independent red-team pass (2026-08-24, before any implementation existed)

Run against the pre-implementation design draft, not the code — the earliest point a fix is cheapest
— per CLAUDE.md's `design → red-team → prove → judge` requirement for a change touching I2 across
three backends. Full report kept in the design draft's own §8. Findings:

- **FATAL (F1), verified by reasoning against the real `path_prefix_covers()` implementation.** The
  draft's coverage check reused `capability_detail::path_prefix_covers()` (built for, and safe in,
  `find_fs_read`/`find_fs_write`'s own use — reached only after a path already passed through a
  handle-anchored primitive). Applied directly to a raw `MountSpec` host path with no such upstream
  step, a literal `..` defeats it outright: `path_prefix_covers("/srv/allowed",
  "/srv/allowed/../../../etc")` returns `true` (the string comparison and segment-boundary check both
  pass), while the real path that reaches `mount(2)`/`ctr run --mount` afterward resolves to `/etc`.
  No filesystem access needed to construct this — a pure string bug in the mechanism's own central
  claim.
- **BLOCKING (B1), verified by reading every real call site.** The mechanism as drafted was only
  wired into the three-method `SandboxBackend` concept surface. `NativeJailBackend::
  create_python_worker()` — the ACTUAL, live, production mount-granting path for the mediated Python
  interpreter (`MediatedPythonRunner::initialize()` calls it directly, never `create()`) — is a
  structurally separate, non-concept method with its own independent mount-processing loop. As
  drafted, this design would have shipped claiming to cover every mount/net-shaped backend while
  leaving its highest-value target completely unenforced.
- **BLOCKING (B2), argued from both directions in the red-team's own report.** The draft's opt-out
  was `spec.capabilities.size() == 0`. Against it: this fails OPEN the moment a caller holds ANY
  unrelated capability (e.g. `cap::Background`) on a spec that also carries real mounts, since the
  decision would flip on with zero `SandboxMount` grants existing for those specific mounts —
  producing full ambient authority in the common case and a surprising accidental engagement in the
  uncommon one. This does NOT match the `ADR-070` pattern the draft claimed to mirror: every real
  instance of that pattern in this codebase (`NativeExec`) means "unset -> zero surface," not "unset
  -> full pre-existing ambient authority."
- **BLOCKING (B3), verified by reading `KataBackend::create()` vs. both native-jail backends.**
  `KataBackend` already fails closed on any `NetPolicy` beyond `deny_all=true` (ADR-086);
  `LinuxNativeJailBackend`/`NativeJailBackend` silently ignored `NetPolicy` entirely. Left unfixed,
  the very mechanism meant to end per-backend divergence would ship with a second instance of it
  still live for the exact capability kind (`NetOut`) it introduces.
- **MUST-FIX (M1)**, verified against the real field types. `MountSpec::quota_bytes` is a bare
  (non-`optional`) `uint64_t` defaulting to `0` — the draft's "0 = unspecified, accept" rule, copied
  from `cap::FsWrite::quota_bytes` (a real `std::optional<uint64_t>`), has no well-typed meaning on a
  field that cannot distinguish "unspecified" from "an explicit zero-byte request." Inert today (no
  backend enforces `MountSpec::quota_bytes`), but would become a real widening the moment one did.
- **MUST-FIX (M2)**: `host_allowlist` string-matching grammar (case-folding, wildcard handling) was
  left to "verify during implementation" — a design without a falsifiable rule for a security-
  critical string comparison.
- **MUST-FIX (M3)**: the new capability structs carried no I3 ("host-authored, never model-derived")
  comment, unlike the cited `NativeExec` precedent.
- **SHOULD-FIX (S1/S2/S3)**: unbounded `host_path_prefix` blast radius worth documenting (not a code
  change); wrong-polarity vs. no-grant diagnostics should NOT be distinguished (avoids leaking grant
  shape); the draft's "zero existing tests need changes" claim was unverified at draft time (and
  incomplete, per B1) — needed a real audit, not an assumption.

**Overall red-team verdict:** "Not safe to implement as written... needs a real second design pass."
The underlying capability-kind design (§2, distinct `SandboxMount`/`SandboxNetOut` kinds) was found
sound and not reworked; the coverage/opt-in/entry-point mechanics were.

## 4. Fixes (same pass, before implementation began)

- **F1**: `authorize_spec()` rejects any `MountSpec::source`/`guest_path` containing a literal
  `.`/`..` path component outright, before ever reaching a prefix comparison — mirroring
  `KataBackend::create()`'s own existing "reject the injection surface entirely" fix for a literal
  comma in its `--mount` grammar (ADR-086). Symlink-based escape inside an otherwise-correctly-
  granted directory is a disclosed residual, not solved here (§6) — the same class of gap `ADR-071`
  §6 item 3 already accepts, unsolved, for argv validation; a real fix needs a handle-anchored check
  at the point each backend actually opens/mounts the path, separate and larger work.
- **B1**: `authorize_spec()` is now called first-thing in all four real entry points (§2), found by
  enumerating every actual `SandboxSpec`-consuming mount-granting call, not the three-method concept
  surface alone.
- **B2**: enforcement scoped to `sandbox_mount_grants()`/`sandbox_net_out_grants()` presence, never
  to `CapabilitySet::size()`. An unrelated capability on the same spec no longer changes anything
  (proven: `test_sandbox_capability_authorization.cpp` G7).
- **B3**: `LinuxNativeJailBackend::create()` and `NativeJailBackend::create()` (Windows) both gained
  the identical unconditional `NetPolicy` fail-closed check `KataBackend::create()` already had,
  independent of capabilities — `linux_native_jail.net_allowlist_unsupported` /
  `native_jail.net_allowlist_unsupported`.
- **M1**: `cap::SandboxMount` does not carry a `quota_bytes` field in this version — nothing checks
  `MountSpec::quota_bytes` (no backend enforces it yet), so there is nothing to check unsafely. A
  contained follow-on once quota enforcement itself lands, alongside making `MountSpec::quota_bytes`
  itself `std::optional<uint64_t>`.
- **M2**: pinned — exact string match after lowercasing the whole `"host:port:scheme"` entry
  (hostnames case-insensitive by convention); a literal `*` in either side is rejected outright, no
  wildcard support in this version.
- **M3**: `cap::SandboxMount`/`cap::SandboxNetOut` both carry the identical "host-authored, never
  model-derived (I3)" comment `cap::NativeExec` does.
- **S2 adopted**: `authorize_spec()` returns the identical `sandbox.mount_not_authorized` code
  regardless of why coverage failed (no grant / wrong `read_write` polarity / non-covering prefix).

## 5. Falsifiable claims and proof

| # | Claim | Verdict | Basis |
|---|---|---|---|
| P1 | `authorize_spec()` is a no-op (returns `{}` unconditionally) for a spec holding no `SandboxMount`/`SandboxNetOut` grant, regardless of `mounts`/`net` content — the whole backward-compatibility premise. | **CORRECT** | `test_sandbox_capability_authorization.cpp` G1. |
| P2 | A `MountSpec` covered by a matching `SandboxMount` grant (prefix + `read_write` polarity) is authorized; an uncovered one, or one covered only in the wrong `read_write` direction, fails closed with `sandbox.mount_not_authorized`. | **CORRECT** | G2, G3, G4, G4b. |
| P3 | A literal `..` path component defeats the mechanism if unguarded (the exact F1 exploit) is now rejected outright, never lexically accepted. | **CORRECT** | G5, G6. |
| P4 | An unrelated capability (e.g. `cap::Background`) present on a spec with zero `SandboxMount` grants does not engage mount enforcement (the B2 fix). | **CORRECT** | G7. |
| P5 | `NetPolicy` coverage: `deny_all=true` needs no grant; a covered allowlist entry (case-insensitively) is authorized; an uncovered entry, an unrestricted request (`deny_all=false` + empty allowlist), and a literal `*` all fail closed with distinct, correct diagnostic codes. | **CORRECT** | G8, G9, G9b, G10, G11, G12. |
| P6 | Coverage is checked against every held grant, not just the first. | **CORRECT** | G13. |
| P7 | A `BlobRef` mount source is left untouched by `authorize_spec()` (each backend's own pre-existing `blob_mount_unsupported` check handles it downstream) — no spurious pass or fail. | **CORRECT** | G14. |
| P8 | Wiring into all four real entry points introduces zero regressions on any existing Windows-buildable test/target. | **CORRECT** | Full local Windows rebuild (`ninja`, zero errors) + full local `ctest` run, see §7. |

`test_sandbox_capability_authorization.cpp` is pure-logic (no OS dependency) and runs on every
platform in CI, unlike the Linux/Windows-specific backend suites that prove the real end-to-end
wiring.

## 6. Residuals, carried forward explicitly

- **Symlink escape** inside an otherwise-correctly-granted host directory is not detected —
  `authorize_spec()` never touches the filesystem. Same class of gap `ADR-071` §6 item 3 already
  names, unsolved, for a different surface. A real fix needs a handle-anchored, TOCTOU-safe check at
  the point each backend actually opens/mounts the path — separate, larger work.
- **`cap::SandboxMount::host_path_prefix` is an unbounded raw filesystem prefix** with a categorically
  higher blast radius than `FsRead`/`FsWrite`'s adapter-rooted `path_prefix` — a syntactically valid
  grant of `"/"` authorizes the entire host filesystem. Operator-discipline risk, disclosed, not a
  code defect.
- **`quota_bytes` is not carried on `cap::SandboxMount` at all** (M1) — add it, together with
  `MountSpec::quota_bytes` becoming `std::optional<uint64_t>`, once a backend actually enforces
  mount-level quotas (still a REAL GAP per `ADR-086` for every backend today).
- **`ResourceLimits`' remaining unenforced axes** (`cpu_ms`/`pids`/`fds`/`disk_bytes`/`net_bytes`)
  are unrelated to this ADR's scope and remain exactly as named-gap as `ADR-086` and the native-jail
  headers already state.
- **`SandboxSpec::capabilities` is still not the ONLY authority for `KataBackend`'s `NetPolicy`** —
  `KataBackend` still has no CNI/egress-proxy mechanism to actually honor a granted, covered
  `SandboxNetOut` allowlist entry; a covered entry still hits Kata's own pre-existing
  `kata_backend.net_allowlist_unsupported` fail-closed check downstream of `authorize_spec()`. This
  ADR makes the CAPABILITY side of that check real; the MECHANISM side (a real Kata CNI/egress proxy)
  is separate, not-yet-built work.
- **Full CI (Linux + Kata-deployment-gated) verification has not run yet** — see §7; this ADR's own
  local verification is Windows-only (the platform this session had available). Linux/Kata coverage
  relies on GitHub Actions' existing matrix, the same posture prior Kata/native-jail-Linux ADRs in
  this repository's history have used when no local Linux environment was available.

## 7. Verification performed this session

Windows-only (no local Linux/Kata environment available, consistent with prior sessions' own
disclosed limitation for this backend family): full local rebuild (MSVC 19.51, `/std:c++latest`,
`AGENTENGINE_WITH_HTTPS=ON`) — zero new compile errors or warnings from this change across every
touched translation unit; full local `ctest` run — see the accompanying session record for the pass
count. `test_sandbox_capability_authorization.cpp` (pure-logic, platform-independent, 16/16 checks
passing) is the primary proof for §5's claims; Linux (`LinuxNativeJailBackend`, `KataBackend`) and
the Windows-only `create_python_worker()` integration path are exercised by this change's wiring but
not independently re-verified end-to-end this session beyond compilation review — a future session
with Linux/Kata-deployment access should confirm the existing `test_native_jail_*_linux.cpp` and
`test_kata_backend_*_linux.cpp` suites still pass unmodified (they hold zero `SandboxMount`/
`SandboxNetOut` grants today, so `authorize_spec()` should be a no-op for every one of them — an
expectation, not yet independently confirmed against a live Linux/Kata run).
