# ADR-098 — Should the four real `SandboxBackend` conformers get a real, tested factory that registers them into a `SandboxBackendRegistry`?

**Status:** Proposed — design and red-team phases complete (three independent rounds, 2026-08-25);
**implementation is done and proven** (2026-08-25, post-design): `build_default_sandbox_registry()`
(`src/sandbox/default_sandbox_registry.hpp`/`.cpp`, new `agentengine_default_sandbox_registry` CMake
target) is real code. Verified on this session's real environment two ways: the default build
(Windows, `AGENTENGINE_WITH_WASM=OFF`, `AGENTENGINE_BUILD_KATA_BACKEND=OFF`) — full rebuild (253
targets) and full `ctest` (253/253) green — and, per explicit project-owner go-ahead this session, a
second full pass with `-DAGENTENGINE_WITH_WASM=ON` (a real, first-time-this-session network fetch of
the pinned wasmtime release) — full rebuild (256 targets) and full `ctest` (256/256) green,
confirming the wasm registration branch actually compiles and behaves as designed, not just by code
inspection. Still awaiting the project owner's own `Judged` sign-off; this session cannot self-Judge
a change to security-critical sandbox-backend selection code per CLAUDE.md.

**Relates to:** `docs/planning/default-sandbox-registry-wiring-design-draft.md` (the full record —
two revisions, three red-team rounds, summarized not duplicated below);
`docs/planning/sandbox-backend-registry-design-draft.md` (the prior, already-implemented,
already-tested registry mechanism itself — `SandboxBackendRegistry`, `register_backend()`,
`register_hardware_isolation_backend()`, `resolve_strict()`/`resolve_named()` — this ADR's own
direct dependency, unmodified by this ADR); `sandbox.hpp`'s `SandboxBackend` concept (008 §2a),
already conformed to by all four real backends via `static_assert` before this ADR.

## 1. The question

**Stated so it has a wrong answer:** `SandboxBackendRegistry` is real, tested infrastructure — but
grepping the whole tree (excluding tests) found `register_backend()` was, before this ADR, called
from nowhere except test files. Is that gap best closed by writing a real, host-callable factory
function that registers the compiled-in backends (this ADR's scope), or does closing it properly
require wiring a real execution path (`AgentSession`, `SandboxToolProvider`) to actually consume a
resolved backend? The user considered both and explicitly chose the narrower scope for this pass —
recorded here so a future reader doesn't mistake this ADR for having done the larger thing.

## 2. What a full read of the real code found

Two real constraints the naive version of "just write a registration header" gets wrong, found by
reading the actual `CMakeLists.txt` and backend headers directly rather than assuming from the
concept's shape:

1. **The four `SandboxBackend` conformers are separate, independently opt-in CMake targets, not
   just platform-split.** `agentengine_wasm_backend` is gated behind `AGENTENGINE_WITH_WASM` (OFF by
   default, pulls a real vendored wasmtime dependency); `agentengine_kata_backend` is gated behind
   `AGENTENGINE_BUILD_KATA_BACKEND` (OFF by default, Linux-only, needs a live containerd+Kata
   deployment). A header that unconditionally `#include`s all four backend headers fails to compile
   under this project's own default build configuration. This needed its own CMake target with
   conditional linking and matching `#ifdef` feature-detection macros
   (`AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND`, the first of their kind in this
   codebase — everything else gates whole translation units out of the build graph rather than
   `#ifdef`-ing inside one shared file).
2. **No `include/agentengine/**` header ever includes anything under `backends/`.** Backend
   conformers' own headers live under `src/backends/*/`, reached via `#include "backends/..."`
   relative to a `src/`-rooted include path, never from the public `include/agentengine/` surface.
   This factory follows the same convention: it lives under `src/sandbox/`, not
   `include/agentengine/sandbox/`.

**A larger, adjacent gap this ADR does NOT close, named honestly rather than assumed fixed** (the
prior registry draft's own §0 named this first): grepping the whole tree (excluding
`tests/`/`.claude/worktrees/`) for real, non-test `register_agent<A>()` call sites finds exactly
one, `tools/policy_reachability_fixture.hpp` — a CI policy-check dev tool
(`register_agent<EchoAgent>()`, no registry argument). `tools/cli_chat.cpp`, the closest thing to a
real host in this tree, never touches `Agent<>`/`register_agent<A>()` at all — it wires tools
directly. So `build_default_sandbox_registry()` has, as of this writing, **no real production
caller**. This ADR makes backend *selection* (getting real, compiled-in backends into a registry)
mechanical and tested; it does not make backend *consumption* real for any execution path — that
remains exactly as open as the prior registry ADR left it, a separate and larger piece of work the
project owner explicitly declined to fold into this pass.

## 3. What changed

**`build_default_sandbox_registry()`** (`src/sandbox/default_sandbox_registry.hpp`, declaration
only — callers never see `NativeJailBackend`/`WasmBackend`/`KataBackend` directly;
`src/sandbox/default_sandbox_registry.cpp`, the real registration list) constructs a fresh
`SandboxBackendRegistry` and registers:

- **native-jail** (`NativeJailBackend` on Windows, `LinuxNativeJailBackend` on Linux) — always,
  `strict_eligible` (the default), no build option gates this leg.
- **wasm** (`WasmBackend`) — only when `AGENTENGINE_HAVE_WASM_BACKEND` is defined (i.e.
  `AGENTENGINE_WITH_WASM` was ON at configure time), `strict_eligible`. A compile-time
  `static_assert` in the `.cpp` enforces `WasmBackend::traits.strength < NativeJailBackend::traits.strength`
  (or the Linux equivalent) so "wasm never outranks native-jail in `Strict` resolution" is a
  structurally-enforced fact, not just true by coincidence today.
- **kata** (`KataBackend`) — only when `AGENTENGINE_HAVE_KATA_BACKEND` is defined (Linux +
  `AGENTENGINE_BUILD_KATA_BACKEND=ON`), registered via `register_hardware_isolation_backend()` —
  never `register_backend(..., strict_eligibility::named_only)` directly (§5's security finding).

Every backend uses its own built-in constructor defaults — this ADR adds no config-driven override
surface (e.g. an env var for `KataBackend`'s `image`/CNI paths). New CMake target
`agentengine_default_sandbox_registry` (`CMakeLists.txt`, placed after all four backend option
blocks so every `agentengine::*_backend` alias it conditionally links against already exists),
linking the current-platform native-jail target unconditionally and the wasm/kata targets only
under their own existing option guards, with the two feature-detection macros `PUBLIC` (so both the
`.cpp` and the new test target see them — see §5's self-caught correction).

## 4. What does NOT stay / was not attempted

No production call site was added anywhere (§2's "no real host" finding — this ADR does not invent
one, e.g. by retrofitting `tools/cli_chat.cpp` to use `Agent<>`/`register_agent<A>()`, which the
project owner explicitly declined as out of scope). No config-driven backend-constructor override.
No change to `SandboxBackendRegistry`/`register_backend()`/`resolve_strict()`/`resolve_named()`
themselves — this ADR is purely a new caller of already-settled, already-Judged-track machinery.

## 5. Verification performed, including two real self-caught corrections

- **Three independent red-team passes** (correctness/CMake plumbing, security I2/I3,
  scope/overclaim) against the real current code, not the draft's paraphrase, before implementation.
  Real findings, all fixed before/during implementation:
  - **Correctness, MUST-FIX**: `WasmBackend` lives in `agentengine::wasm`, not top-level
    `agentengine` — the first draft's sample code would not have compiled. Fixed:
    `wasm::WasmBackend`, confirmed by a real build against the real type (§ below).
  - **Security, real finding**: the first draft called `register_backend("kata", ...,
    strict_eligibility::named_only)` directly instead of the dedicated
    `register_hardware_isolation_backend()` entry point `KataBackend`'s own header contract names
    for exactly this case — bypassing a type-level guarantee the codebase built specifically to
    prevent a dropped-argument landmine (a future edit silently making a stronger, hardware-VM
    backend `Strict`-eligible process-wide). Fixed: uses `register_hardware_isolation_backend()`.
  - **Correctness, should-fix**: the Windows wasm-test verification plan initially named no
    `wasmtime.dll` `PATH` wiring (every existing wasm-linked test needs
    `set_tests_properties(... ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")`) —
    fixed in `tests/CMakeLists.txt`.
  - **Scope/overclaim**: independently re-verified every honesty claim in §2 (register_agent call
    sites, `SandboxBackendRegistry` being real/tested, this ADR's bounded verification scope)
    against the real tree — all confirmed true, no overclaim found.
- **Self-caught during implementation** (not from red-team): the correctness red-team's own
  should-fix suggestion — make the two feature-detection macros `PRIVATE`, since only
  `default_sandbox_registry.cpp` itself reads them — turned out to be wrong once the real test file
  was written: `tests/test_default_sandbox_registry.cpp` also needs to see
  `AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND` to conditionally compile its own
  wasm/kata assertions, and `PRIVATE` would have silently skipped those checks even when the
  backends were actually built in. Caught by re-reading the diagnostics before running the build,
  not by the red-team or the user; reverted to `PUBLIC` with the corrected rationale recorded in
  both the CMake comment and the design draft.
- **Self-caught, unrelated to this ADR's own new code**: running `tools/naming_lint.py` as part of
  this ADR's own verification surfaced that ADR-097 (already committed, prior to this ADR) had left
  three new exported names (`EmitFn`, `DrainedCompletion`, `StandingEffectRegistry`) without the
  required `ae-naming-lint: allow` suppression comments — a real gap in that already-landed work
  this session had not checked. Fixed as part of this ADR's own verification pass (three one-line
  comment additions, `include/agentengine/rt/agent_session_trust.hpp` and
  `include/agentengine/rt/standing_effect_registry.hpp`), confirmed via a clean `naming_lint.py` run
  (0 new findings; the 3 remaining are pre-existing and unrelated to either ADR).
- **Full default-config build+test** (Windows, `AGENTENGINE_WITH_WASM=OFF`,
  `AGENTENGINE_BUILD_KATA_BACKEND=OFF` — this project's actual defaults): rebuild clean, `ctest`
  253/253.
- **Full wasm-enabled build+test** (`-DAGENTENGINE_WITH_WASM=ON`, a real network fetch of the
  pinned/checksummed wasmtime release, done with explicit project-owner go-ahead this session):
  rebuild clean (confirms `wasm::WasmBackend` qualification and the `static_assert` both compile for
  real against the real type), `ctest` 256/256 — including `test_default_sandbox_registry`'s
  wasm-gated assertions, confirmed to have actually run (not silently skipped) by inspecting the
  real compile command in `compile_commands.json` for `-DAGENTENGINE_HAVE_WASM_BACKEND`.
- Build config reverted to the project default (`AGENTENGINE_WITH_WASM=OFF`) after verification, so
  the local dev build folder matches its normal state going forward.

## 6. Residuals, named not silently assumed complete

- **No production consumer** (§2) — `build_default_sandbox_registry()` is real, tested, reusable
  infrastructure with no real caller anywhere in this tree yet. A future host that wires a real
  execution path to actually call `resolve_strict()`/`resolve_named()` and use what comes back is a
  separate, larger, higher-risk piece of work this ADR explicitly does not attempt.
- **Kata leg is code-inspection-verified only, not build-verified this pass** — this Windows dev box
  cannot build `AGENTENGINE_BUILD_KATA_BACKEND` at all (Linux-only, needs a live containerd+Kata
  deployment). Checked by eye against the exact CMake gating the real `agentengine_kata_backend`
  target already uses; stated plainly as **compiled by no one this pass**, not equivalent to
  verified. A future session with real Linux+Kata CI should build-verify it before treating it as
  settled.
- **No config-driven override of backend constructor arguments** — every backend uses its own
  built-in defaults; a real "read a config file/env var to override `KataBackend`'s image/CNI paths"
  path is future work.
- `AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND` are new feature-detection macros —
  the first of their kind in this codebase. If a future backend needs the same treatment, reuse this
  pattern rather than inventing a third one.
