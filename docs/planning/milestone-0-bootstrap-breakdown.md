# Milestone 0 — Bootstrap: work breakdown and kick-off

**Status:** Work breakdown (stage 4 of
[the review-signoff workflow](v1-review-signoff-workflow.md)), written just-in-time as this
milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 0 exit criterion: *"an empty engine target
links against the pinned Quark commit and builds clean on both platforms under the CI matrix."*

## Current state (verified 2026-08-05)

Before breaking down what's left, a real audit of what already exists — this repo is not a blank
slate. `decisions/ADR-001` through `ADR-004` each produced real prove-phase code under
`src/backends/native_jail/` to test a specific design question (ShellRunner grammar/dispatch,
embedded-CPython mediation, caller-aware import gating, the AppContainer+Job-Object Windows
backend). **Per project-owner direction (2026-08-05): that code stays exactly where it is — it's
the evidence those ADRs cite — but it is prove-phase spike code, not the v1 implementation
baseline.** Milestone 0 and everything after it is written fresh against the now-Reviewed RFCs;
nothing here builds on top of the spike.

What's already done, narrowly for M0's own exit bar:

| Item | State |
|---|---|
| Root `CMakeLists.txt`, C++23 standard | Exists |
| Quark submodule, pinned | Exists — pinned to `a3d66c2` |
| `agentengine::core` — an empty (header-only) target linking `quark::quark` | Exists |
| `cmake_minimum_required` matches 021 §5 (CMake ≥ 3.28) | Fixed today — was 3.24, a real spec/code drift caught while writing this breakdown; corrected per CLAUDE.md's "spec wins" rule |
| Compiler-version enforcement (021 §5: MSVC 19.4x, g++ 14+, clang 20+) | **Not done** — no guard exists; anything compiles today regardless of version |
| CI skeleton, Windows + Linux matrix | **Not done** — no `.github/workflows/` directory exists |
| 027 naming-lint stub wired into CI | **Not done** — CI doesn't exist yet for it to wire into |
| Build verified clean on both platforms | **Partially** — buildable locally on Windows (this dev box); Linux has never been exercised, since there's no CI and no local Linux box in this session |

So M0 is closer to done than "start from zero," but the two things its own exit criterion actually
names — the CI matrix and a genuinely cross-platform-verified build — don't exist yet.

## Tasks

Each sized S/M/L/XL, not a point estimate — per the process doc's §4, a numeric estimate for
work whose actual friction (a Linux CI runner's exact toolchain, an untested compiler-version
guard) hasn't been hit yet would be false precision.

1. **Compiler-version guard in root `CMakeLists.txt`** — `message(FATAL_ERROR ...)` if the detected
   compiler is below 021 §5's floor (MSVC 19.4x / g++ 14 / clang 20). **S.**
2. **CI skeleton: Windows + Linux matrix** — a GitHub Actions workflow (`.github/workflows/ci.yml`)
   building `agentengine::core` (and, while true either way, whatever else `AGENTENGINE_BUILD_TESTS`
   already pulls in) on both platforms. Runs even without a remote to push to yet — it's ready the
   moment one exists, and its absence is exactly what's blocking "verified clean on both platforms"
   from being more than a claim. **M** — the Linux leg is the real unknown; the existing
   `AGENTENGINE_BUILD_PYTHON_RUNNER` path is explicitly Windows-only (CMakeLists.txt's own
   `message(FATAL_ERROR ...)` for `NOT WIN32`), so the *default* (non-Python-runner) build is what
   CI needs to prove on Linux — the harder POSIX Python wiring stays out of scope for M0.
3. **027 naming-lint stub** — even a minimal version: a script that walks `include/agentengine/`
   and flags any exported symbol not appearing in 027's vocabulary tables. Wire it into the new CI
   workflow as a required check from day one (per the roadmap's "continuous, not bolted on later"
   framing for 027). **M** — the checking logic is small; deciding how strictly to parse C++ symbol
   names without a full compiler frontend is the actual work.
4. **Verify the Linux build for real** — once CI exists, confirm `agentengine::core` actually
   builds clean there; fix whatever CI surfaces (there's a real chance something Windows-only has
   crept into a file this task doesn't already know is out of scope, e.g. via a stray transitive
   include). **S** if CI's Linux leg is green immediately, **M** if it isn't.

## Handover & kick-off

Milestone 0 starts today, 2026-08-05. No deviation from the roadmap's assumed order — this is
still the first milestone, and nothing above changes Milestone 1's scope or start condition.

The one thing worth flagging forward: this breakdown surfaced a real spec/code drift (CMake
minimum version) just from reading the existing build file against 021 §5. That's a small,
specific instance of the general risk the roadmap's "What this doc is not" section already named —
implementation will keep surfacing gaps spec review alone didn't catch. Worth watching for more of
these as M0's remaining tasks land, not just at milestone boundaries.
