# 021 — Platform Support and Portability

**Status:** Draft · **Depends on:** 008, 009, Quark 019 · **Gate:** §6

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
| macOS / arm64 | **Supported** | `microvm` unavailable (008 Q1) |
| Linux / arm64 | CI-verified | Quark runs its matrix here; budgets not re-baselined |
| Windows / arm64 | Best-effort | — |

**Honesty requirement:** the README's support table is generated from CI results, not written by
hand. A tier is a statement about what CI proves.

## 3. Per-subsystem portability

| Subsystem | Mechanism | Portability risk |
|---|---|---|
| Scheduler, mailbox, timers, cluster | Quark + PAL (`linux_x86_64`, `windows_x86_64` backends exist) | macOS PAL backend is the gap; it is Quark's to fill, upstream |
| Networking / TLS | PAL sockets + a TLS seam backend | Platform TLS stores differ; certificate handling is the usual trap |
| Filesystem / workspaces | PAL file IO + path canonicalization | **High**: case-insensitivity, ADS, long paths, reparse points, `\\?\`, symlinks. Path escape is a security bug, so this is tested as security, not convenience |
| `wasm` profile | wasmtime (WASI 0.3) | **Lowest** — identical semantics on all three |
| `native-jail` profile | AppContainer + Job Object + restricted token · namespaces + seccomp + cgroups v2 · sandbox profile | **Highest**: three separate implementations of one contract (008 §9 G1 is the gate) |
| `microvm` profile | WHP · KVM/mshv | Unavailable on macOS |
| Python interpreter | Per-platform CPython + venv | Package availability differs by platform; pinning is the mitigation |
| Secrets | DPAPI · keyring/file · Keychain | Different capabilities and different failure modes |
| Process/resource limits | Job Objects · cgroups v2 · `setrlimit`+sandbox | Semantics differ; the seam exposes only what all three enforce |

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
- CI matrix: {Windows, Linux, macOS} × {Release, ASan+UBSan} × {MSVC/clang-cl, gcc, clang}, plus TSan
  on Linux, plus the conformance suites, plus the budget gate on the reference machine.
- The machine-safety caps in CONVENTIONS (`-j4`, pinned tests) apply to CI configuration too.

## 6. Promotion gate

- **G1** — the full correctness suite passes on all three Supported platforms.
- **G2** — the 008 parity gate (identical outcome classification across backends) passes per
  platform, with the availability table matching reality.
- **G3** — the path-escape corpus (`..`, symlinks, junctions, `\\?\`, ADS, case tricks, unicode
  normalization) fails to escape a mount on any platform.
- **G4** — one `ae:tool` component produces byte-identical results on all three (009 G1).
- **G5** — the README support table is generated from CI, and a deliberately broken platform job
  changes it.

## 7. Open questions

- **Q1** — macOS: Quark has no macOS PAL backend today. Either it is contributed upstream or macOS
  support is limited to what the engine can do without Quark's PAL-dependent paths. **This is the
  single largest portability risk in the project and it needs an owner.**
- **Q2** — Windows `native-jail`: AppContainer + Job Objects is the plan, but proving it enforces
  the same limits as cgroups v2 (particularly memory and pid caps) is unvalidated.
- **Q3** — Whether to ship prebuilt wasmtime binaries or build from source in CI.
- **Q4** — arm64 budget re-baselining (inherits Quark's open question).
