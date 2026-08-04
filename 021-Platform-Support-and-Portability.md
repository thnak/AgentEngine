# 021 — Platform Support and Portability

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 008, 009, Quark 019 · **Gate:** §6

## Goal

State exactly what "cross-platform" means here — per platform, per subsystem — and make the claim
falsifiable rather than aspirational.

## 1. Support tiers

| Tier | Meaning |
|---|---|
| **Supported** | Full correctness suite + conformance suites + budget gate run in CI on every push; all profiles that the platform table claims are available and gated |
| **CI-verified** | Correctness suite runs in CI; budgets not baselined; some profiles unavailable |
| **Best-effort** | Builds and runs; not gated in CI |
| **Unsupported** | No claim |

## 2. Target matrix (intent for v1)

| Platform | Tier (intent) | Notes |
|---|---|---|
| Windows 11 / x86-64 | **Supported** | Primary development platform; MSVC + clang-cl |
| Linux / x86-64 | **Supported** | Quark's own reference target |
| Linux / arm64 | CI-verified | Quark runs its matrix here; budgets not re-baselined |
| Windows / arm64 | Best-effort | — |
| macOS (any) | **Unsupported — no claim** | Dropped entirely, not downgraded (§7 Q1, resolved 2026-08-04): Quark has no macOS PAL backend, nobody has claimed the work of contributing one upstream, and every macOS line in this project was an aspiration this RFC's own §2's "Honesty requirement" doesn't let it keep making. If a macOS PAL backend is ever contributed to Quark, re-adding macOS here is a new decision, not a reinstatement — it starts at Best-effort like any newly-claimed platform, proven by CI before it moves up. |

**Honesty requirement:** the README's support table is generated from CI results, not written by
hand. A tier is a statement about what CI proves.

## 3. Per-subsystem portability

| Subsystem | Mechanism | Portability risk |
|---|---|---|
| Scheduler, mailbox, timers, cluster | Quark + PAL (`linux_x86_64`, `windows_x86_64` backends exist) | None on the two targeted platforms; a macOS PAL backend would be Quark's to contribute upstream if macOS is ever re-added as a target (§2) |
| Networking / TLS | PAL sockets + a TLS seam backend | Platform TLS stores differ; certificate handling is the usual trap |
| Filesystem / workspaces | PAL file IO + path canonicalization | **High**: case-insensitivity, ADS, long paths, reparse points, `\\?\`, symlinks. Path escape is a security bug, so this is tested as security, not convenience |
| `wasm` profile | wasmtime (WASI 0.3) | **Lowest** — identical semantics on both |
| `native-jail` profile | AppContainer + Job Object + restricted token · namespaces + seccomp + cgroups v2, plus interpreter-level mediation (008 §1b) | **Highest**: two separate OS-level implementations of one contract (008 §9 G1 is the gate); the mediation layer on top is platform-independent engine code |
| Python interpreter | Embedded native CPython, one runtime everywhere (010 §2) | Package availability differs by platform; pinning the `preinstalled` image is the mitigation |
| Secrets | DPAPI · keyring/file | Different capabilities and different failure modes |
| Process/resource limits | Job Objects · cgroups v2 | Semantics differ; the seam exposes only what both enforce |

## 4. Portability rules

- **No `#ifdef` outside a seam.** Platform code lives in a PAL or backend implementation, never
  inline in the core (CONVENTIONS).
- **Paths are a type, not a string.** Canonicalization, mount-relative resolution, and escape
  checks belong to that type, so no caller can forget them.
- **Line endings, encodings, and locale** are explicit at every boundary; UTF-8 internally,
  everywhere, always.
- **Time is monotonic** for deadlines and wall-clock only where semantically required (reminders,
  audit timestamps).
- **A feature unavailable on a platform is absent, not silently degraded** — the profile-downgrade
  visibility rule (008 §3) is the general form of this.

## 5. Build

- CMake ≥ 3.28, C++23. Compilers: MSVC 19.4x, g++ 14+, clang 20+ (clang-cl on Windows).
- Quark as a submodule; wasmtime and other backend dependencies behind CMake options, each
  independently disable-able (a minimal build is core + `wasm` profile).
- CI matrix: {Windows, Linux} × {Release, ASan+UBSan} × {MSVC/clang-cl, gcc, clang}, plus TSan
  on Linux, plus the conformance suites, plus the budget gate on the reference machine.
- The machine-safety caps in CONVENTIONS (`-j4`, pinned tests) apply to CI configuration too.

## 6. Promotion gate

- **G1** — the full correctness suite passes on both Supported platforms.
- **G2** — the 008 parity gate (identical outcome classification across backends) passes per
  platform, with the availability table matching reality.
- **G3** — the path-escape corpus (`..`, symlinks, junctions, `\\?\`, ADS, case tricks, unicode
  normalization) fails to escape a mount on either platform.
- **G4** — one `ae:tool` component produces byte-identical results on both (009 G1).
- **G5** — the README support table is generated from CI, and a deliberately broken platform job
  changes it.

## 7. Open questions

- ~~**Q1** — macOS: Quark has no macOS PAL backend today. Either it is contributed upstream or macOS
  support is limited to what the engine can do without Quark's PAL-dependent paths. This is the
  single largest portability risk in the project and it needs an owner.~~ **Resolved (2026-08-04):**
  macOS is dropped as a target platform entirely — not contributed upstream, not downgraded to a
  lower tier. No macOS PAL backend exists, nobody has claimed the work, and carrying an
  aspirational Supported claim (§2 as it stood) with no CI behind it on any platform violated this
  RFC's own honesty requirement. Windows and Linux are the v1 target set (§2); this removes the
  project's single largest portability risk by removing its scope rather than by resourcing it. If
  a macOS PAL backend is contributed to Quark later, re-adding macOS is a fresh decision that starts
  at Best-effort, not a reinstatement of the old claim. Downstream effect: 008 §1's `no microvm
  profile` reasoning, which had cited the macOS gap, was re-grounded in a Windows-hosting finding
  instead (`docs/research/2026-microvm-windows-portability.md`) so the decision doesn't lose its
  rationale now that macOS is moot rather than missing.
- **Q2** — Windows `native-jail`: AppContainer + Job Objects is the plan, but proving it enforces
  the same limits as cgroups v2 is unvalidated — **partially answered for the Windows half**
  (`decisions/ADR-004-appcontainer-native-jail-windows-backend.md` §10, 2026-08-02): memory and
  pid caps are measured precise and reliable (matches expectation); CPU-time caps
  (`JOB_OBJECT_LIMIT_JOB_TIME`) are measured *unreliable* — fired in only 3/11 runs, 1.38x-8.22x
  overrun when it did — so `wall_ms`, not `cpu_ms`, is the dependable enforcement point on this
  backend (008 §2). Still open: no Linux cgroups v2 `native-jail` backend exists yet to give this
  question its actual comparison point, so it remains unvalidated in the sense this question
  asks — the Windows measurement is new information, not a close. Also open: why
  `JOB_OBJECT_LIMIT_JOB_TIME` behaves this way is unexplored (root cause, not just measurement),
  and LPAC (vs. plain AppContainer) was reasoned about but not tested against the read-leak finding
  in the same ADR (008 §7).
- ~~**Q3** — Whether to ship prebuilt wasmtime binaries or build from source in CI. (Separate from
  *which* version(s) to support — 009 §11 Q4 resolved that: pin to exactly one Wasmtime version at a
  time. This question is about how CI obtains that one pinned build, not how many to track.)~~
  **Resolved, prebuilt, checksum/signature-verified (2026-08-04):** the same supply-chain-trust
  pattern this project has now applied repeatedly (009 §3's plugin signing, 012 §4a's card signing,
  015 §8 Q3's document signing) — verify an externally-produced artifact's integrity via digest or
  signature, never re-derive the same assurance by rebuilding it yourself. Building Wasmtime from
  source on every CI run adds real time/resource cost for a dependency already pinned to one exact
  version (009 §11 Q4) — source-building doesn't even buy version flexibility, since the pin is exact
  either way, only a slower path to the identical artifact. Prebuilt binaries are fetched once,
  checksum-verified against the upstream release, and cached.
- **Q4** — arm64 budget re-baselining (inherits Quark's open question).
