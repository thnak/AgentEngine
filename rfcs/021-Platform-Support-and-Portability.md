# 021 — Platform Support and Portability

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 008, 009 (historical: originally also Quark 019 — ADR-037 removed Quark as a dependency) · **Gate:** §6

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
| Windows 11 / x86-64 | **Supported** | The v1 target — primary and, for now, only platform under active implementation; MSVC + clang-cl |
| Linux / x86-64 | **Next** (not yet gated) | Taken up once the Windows implementation reaches a stable state — a sequenced follow-on, not simultaneous v1 work (historical: originally framed as "Quark's own reference target" before ADR-037 removed Quark as a dependency) |
| Linux / arm64 | CI-verified (deferred with Linux/x86-64) | Budgets not re-baselined (historical: "Quark runs its matrix here" before ADR-037) |
| Windows / arm64 | Best-effort | — |
| macOS (any arch) | **Unsupported — not a target** | No macOS PAL backend exists and none is planned; no AgentEngine RFC may claim macOS support. Resolved 2026-08-03 (see OpenQuestions.md); previously listed here as Supported on the reasoning that dropping `microvm` (008 §1) removed the only profile-specific gap — that reasoning never addressed the PAL gap itself, which is the actual blocker (historical: originally reasoned from "Quark has no macOS PAL backend"; unaffected by ADR-037 since `agentengine::pal` never had a macOS backend either) |

**Honesty requirement:** the README's support table is generated from CI results, not written by
hand. A tier is a statement about what CI proves.

## 3. Per-subsystem portability

| Subsystem | Mechanism | Portability risk |
|---|---|---|
| Scheduler, mailbox, timers, cluster | `agentengine::rt::` + PAL (`linux_x86_64`, `windows_x86_64` PAL backends exist) | None for the current target set — the PAL backend AgentEngine needs today (Windows) exists, Linux next (historical: this layer used to be Quark + PAL, with both backends existing upstream in Quark; ADR-037 removed Quark, `rt::` has no cluster mechanism at all) |
| Networking / TLS | PAL sockets + a TLS seam backend | Platform TLS stores differ; certificate handling is the usual trap |
| Filesystem / workspaces | PAL file IO + path canonicalization | **High**: case-insensitivity, ADS, long paths, reparse points, `\\?\`, symlinks. Path escape is a security bug, so this is tested as security, not convenience |
| `wasm` profile | wasmtime (WASI 0.3) | **Lowest** — identical semantics on both targeted OSes |
| `native-jail` profile | AppContainer + Job Object + restricted token · namespaces + seccomp + cgroups v2 · sandbox profile, plus interpreter-level mediation (008 §1b) | **Highest**: three separate OS-level implementations of one contract (008 §9 G1 is the gate); the mediation layer on top is platform-independent engine code |
| Python interpreter | Embedded native CPython, one runtime everywhere (010 §2) | Package availability differs by platform; pinning the `preinstalled` image is the mitigation |
| Secrets | DPAPI (Windows) · keyring/file (Linux) | Different capabilities and different failure modes |
| Process/resource limits | Job Objects (Windows) · cgroups v2 (Linux) | Semantics differ; the seam exposes only what both targeted OSes enforce |

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
- No submodules — AgentEngine is a self-contained repository (historical: Quark used to be pinned
  as a submodule here; `decisions/ADR-037-remove-quark-as-core-runtime.md`, executed 2026-08-13,
  removed that dependency entirely). wasmtime and other backend dependencies stay behind CMake
  options, each independently disable-able (a minimal build is core + `wasm` profile).
- CI matrix (current target set): {Windows} × {Release, ASan+UBSan} × {MSVC, clang-cl}, plus the
  conformance suites and the budget gate on the reference machine. Linux ({gcc, clang} × {Release,
  ASan+UBSan+TSan}) is added when Linux implementation begins (§2). No macOS row — not a target.
- The machine-safety caps in CONVENTIONS (`-j4`, pinned tests) apply to CI configuration too.

## 6. Promotion gate

- **G1** — the full correctness suite passes on every Supported platform in the current target set
  (§2) — Windows now; Linux is added to this gate once its implementation begins.
- **G2** — the 008 parity gate (identical outcome classification across backends) passes per
  platform, with the availability table matching reality.
- **G3** — the path-escape corpus (`..`, symlinks, junctions, `\\?\`, ADS, case tricks, unicode
  normalization) fails to escape a mount on any platform.
- **G4** — one `ae:tool` component produces byte-identical results on every platform in the current
  target set (009 G1).
- **G5** — the README support table is generated from CI, and a deliberately broken platform job
  changes it.

## 7. Open questions

- ~~**Q1** — macOS: Quark has no macOS PAL backend today.~~ **Resolved 2026-08-03 (project-owner
  decision, see OpenQuestions.md): macOS is not a target.** No PAL backend will be contributed
  upstream for it; the target matrix (§2) and every other RFC's platform claims are normalized to
  Windows (current) → Linux (next, once Windows is stable), with no macOS row.
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
- **Q3** — Whether to ship prebuilt wasmtime binaries or build from source in CI. (Separate from
  *which* version(s) to support — 009 §11 Q4 resolved that: pin to exactly one Wasmtime version at a
  time. This question is about how CI obtains that one pinned build, not how many to track.)
  **Informed, not fully resolved, by OQ-7's small prove (OpenQuestions.md, 2026-08-03):** the
  official prebuilt Windows x64 C API release (v47.0.3) downloads, links, and runs correctly with no
  build-from-source step, which is at least evidence against needing to build from source on this
  platform. Whether CI should still build from source anyway (reproducibility, supply-chain
  preference) is a separate question this evidence doesn't settle either way — the prove exercised
  only official-release download/link/run, not a from-source-in-CI path, so it cannot close that
  half of the question.
- **Q4** — arm64 budget re-baselining (historical: originally framed as inheriting Quark's open
  question; now an AgentEngine-owned open question in its own right post-ADR-037).
