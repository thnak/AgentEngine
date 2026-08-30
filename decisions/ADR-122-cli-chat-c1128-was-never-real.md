# ADR-122 — `agentengine_cli_chat`'s "pre-existing MSVC C1128" was never real: a stale, orphaned `.vcxproj` artifact, not a code defect

- **Status:** Proposed — root-caused and confirmed fixed with ZERO code changes. `agentengine_cli_chat`
  builds and runs clean, with the `/bigobj` fix CMakeLists.txt already carries from ADR-114. Corrects
  ADR-114 §26 and ADR-119 §2/§4/§5/§6/§7, which both asserted this was a real, pre-existing, unrelated
  MSVC limit — that assertion was accurate to what those passes actually observed, but the observation
  itself was against a stale, disconnected build artifact, not the real, current target.
- **Date:** 2026-08-30.
- **Scope:** No production code, `CMakeLists.txt`, or test file changed. This ADR is a root-cause
  correction of a testing-methodology artifact, plus disclosure fixes in `decisions/ADR-114-task-
  branch-tools-promotion.md`, `decisions/ADR-119-run-command-capability-variant-widening.md`, and
  `decisions/README.md`.
- **Related specs:** `decisions/ADR-114-task-branch-tools-promotion.md` (first observed the failure,
  honestly disclosed as "not investigated further"), `decisions/ADR-119-run-command-capability-variant-
  widening.md` (re-observed the identical failure, used a `git stash` comparison against the SAME stale
  artifact to conclude it was pre-existing and unrelated — a correct conclusion about the artifact, a
  wrong one about the real build).

## 1. The question

ADR-114 and ADR-119 both independently hit `error C1128: number of sections exceeded object file
format limit` building `agentengine_cli_chat` and both concluded, via a `git stash` comparison, that it
was a real, pre-existing, unrelated MSVC limit — "not investigated further" (ADR-114) and out of scope
(ADR-119 §6: "`agentengine_cli_chat`'s own pre-existing `C1128` ... is unrelated to this ADR and remains
unfixed"). Asked to actually fix it: is this a genuine large-translation-unit MSVC limit that needs a
structural fix (splitting the file, `/bigobj` alone no longer sufficing), or something else?

## 2. Findings

**The failure was never reproducible against the REAL, current build target.** `agentengine_cli_chat`
requires BOTH `AGENTENGINE_WITH_HTTPS` and `AGENTENGINE_BUILD_PYTHON_RUNNER` (CMakeLists.txt line 912,
`if(AGENTENGINE_WITH_HTTPS AND AGENTENGINE_BUILD_PYTHON_RUNNER)`). The main Windows build directory this
whole session's work (and, apparently, ADR-114/119's own work before it) used has
`AGENTENGINE_BUILD_PYTHON_RUNNER=ON` but `AGENTENGINE_WITH_HTTPS=OFF` — confirmed directly by reading
`CMakeCache.txt`, not assumed. Under that cache state, CMake's own `add_executable(agentengine_cli_chat
...)` call is never reached, so the target does not exist in the CURRENT solution at all.

**Yet `cmake --build . --target agentengine_cli_chat` "worked" (i.e., genuinely invoked a compiler and
produced the C1128 error) anyway.** This is a real, generalizable Visual-Studio-generator quirk, not
specific to this file: when a target is later excluded from `CMakeLists.txt`'s own conditional logic (by
a cache-variable toggle, not a source edit), CMake does NOT delete or invalidate the target's previously
-generated `.vcxproj` file — it is simply left, orphaned, on disk. `cmake --build --target <name>`
resolves `<name>` to a `.vcxproj` file by NAME, on disk, without first checking whether that target is
still part of the CURRENT solution's live target graph. The result: an orphaned, stale `agentengine_
cli_chat.vcxproj` — generated at some earlier point when this exact directory's cache had
`AGENTENGINE_WITH_HTTPS=ON` — remained fully buildable by direct name, silently detached from the
directory's actual current configuration. Confirmed directly: this stale file predates the `/bigobj`
fix CMakeLists.txt has carried since ADR-114 itself (`grep -ic bigobj` on the stale, on-disk `.vcxproj`
returned zero matches, even though `CMakeLists.txt`'s own `if(MSVC) target_compile_options(
agentengine_cli_chat PRIVATE /bigobj) endif()` block has existed the whole time this ADR line has been
active).

**The `git stash` methodology ADR-114/119 both used was a real, sound comparison — of the wrong thing.**
Comparing "does this same stale `.vcxproj` fail identically on the stashed vs. unstashed source tree"
correctly answers "is this failure caused by MY change" (no), but never asks "is this `.vcxproj` even
the one CMake would generate for the CURRENT configuration" — because both ADR-114 and ADR-119's
authoring passes (and this session's own initial ADR-119 verification, which repeated the identical
mistake against the identical stale file) ran the comparison inside the exact same drifted build
directory, the stale artifact was invisible to that methodology by construction.

**Reconfiguring with the correct cache state makes the real target reappear, correctly built, with
`/bigobj` genuinely applied.** `cmake -DAGENTENGINE_WITH_HTTPS=ON .` in the same directory (which
already had `AGENTENGINE_BUILD_PYTHON_RUNNER=ON`) regenerates a FRESH `agentengine_cli_chat.vcxproj` —
confirmed via `grep -ic bigobj` returning 4 matches (one per Debug/Release configuration section) where
the stale file had zero. A from-scratch rebuild (intermediate directory and `.exe`/`.pdb` deleted first,
to rule out any stale-object-file luck) **compiles and links clean, zero errors** — only the same three
pre-existing, unrelated `skill_provider.hpp` `C4458` warnings and one `xutility` `C4244` warning every
other real build of this file already produces (confirmed identical to warnings seen in this session's
own earlier, correctly-configured `build-https`-directory attempts at other targets). Ran the resulting
`agentengine_cli_chat.exe` directly: starts, writes its conversation-dump banner, and exits cleanly with
the expected "API key not set" message — a genuinely working binary, not merely a successful link.

**No code, no CMakeLists.txt change, no structural fix was needed at all.** The `/bigobj` remedy
ADR-114 already applied was always correct and always sufficient; it was simply never being exercised
by either the C1128-observing pass OR the `git stash` "confirmation" pass, both of which tested a dead
file left behind by an unrelated, earlier cache-state change to this same directory.

## 3. What was built

Nothing. Zero files changed in `include/`, `src/`, `tools/`, `tests/`, or `CMakeLists.txt`. The only
changes are disclosure corrections in `decisions/ADR-114-task-branch-tools-promotion.md`, `decisions/
ADR-119-run-command-capability-variant-widening.md`, and `decisions/README.md`, replacing the "pre-
existing, unrelated, not investigated further" / "remains unfixed" language with a pointer to this
ADR's real root cause and outcome.

## 4. Verification

- `CMakeCache.txt` (main build directory): confirmed `AGENTENGINE_WITH_HTTPS:BOOL=OFF`,
  `AGENTENGINE_BUILD_PYTHON_RUNNER:BOOL=ON` — the drifted state that orphaned the target.
- Stale `agentengine_cli_chat.vcxproj` (pre-reconfigure): confirmed present on disk, confirmed zero
  `/bigobj` occurrences, confirmed it still builds directly (reproducing the exact C1128 both prior
  ADRs observed) despite not being part of the current solution.
- `cmake -DAGENTENGINE_WITH_HTTPS=ON .`: regenerates a fresh `agentengine_cli_chat.vcxproj`, confirmed
  4 `/bigobj` occurrences.
- Full clean rebuild (intermediate directory and output binary deleted first): **zero errors**, only
  pre-existing unrelated warnings.
- `agentengine_cli_chat.exe` run directly: starts and exits cleanly with the expected missing-API-key
  message — a genuinely functional binary.

## 5. What was NOT done

- The main build directory's `AGENTENGINE_WITH_HTTPS` cache variable was left `ON` after this pass
  (needed to keep `agentengine_cli_chat` as a live, buildable target for any future verification) — a
  deliberate, disclosed state change to this one shared build directory, not reverted, since reverting
  it would simply re-orphan the target and reintroduce the exact confusion this ADR closes.
- No attempt was made to audit whether any OTHER target in this same build directory (or others used
  across this session's work) suffers the identical "orphaned-but-still-directly-buildable" artifact —
  scoped specifically to the one claim this ADR was asked to fix, not a general build-hygiene audit.

## 6. Residuals

- **A real, generalizable process gap, worth naming for future verification passes in this repo**: a
  `cmake --build . --target <name>` in a long-lived build directory can silently build a STALE, orphaned
  project file if that directory's own cache state has changed since the target was last a live part of
  the solution — the build succeeding or failing tells you nothing about whether it reflects the CURRENT
  `CMakeLists.txt`/source tree. The safe pattern, established here: before trusting any `--target`
  result as authoritative (especially a "pre-existing, unrelated" conclusion drawn from a `git stash`
  comparison), confirm the relevant cache variables actually enable that target in the CURRENT
  configuration (`grep` the exact `CMakeCache.txt` entries the target's own `if()` guard reads), not
  merely that `cmake --build --target <name>` produces SOME output.
