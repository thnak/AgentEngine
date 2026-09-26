# ADR-201 — The tool-call pipeline's function bodies compiled once in `src/`, not in every file that includes it

- **Status**: **Proposed — design + implementation + proof (2026-09-26); red team round 1 (same day): no BLOCKER or
  MAJOR, 2 MINOR (one disclosed, one fixed) (§8).**
- **Date**: 2026-09-26
- **Origin**: GitHub issue #120 (S5), the structure audit after issue #115 closed.
- **Touches**: `include/agentengine/core/tool_pipeline.hpp` (declarations), `src/core/tool_pipeline.cpp` (new),
  `CMakeLists.txt` (`agentengine_tool_pipeline`, linked through `agentengine::core` and by `agentengine_rt_session`),
  `tests/CMakeLists.txt` (the two `try_compile` gates that already list `agent_session_core.cpp`).

## 1. The question

`core/tool_pipeline.hpp` implements 006 §3's invocation pipeline: `admit_call()`, `run_admitted_call()`,
`invoke_tool()`, `background_task()`, `partition_batch()` and the approval and audit helpers. Every function is an
ordinary non-template `inline` function defined in the header. The #120 audit found 206 including files and 33 commits
to the header since 2026-08-01 — the most-changed header by churn × fan-out, about 6,800 file recompiles in that time
(estimated from current fan-out). Every one of those files parsed and, under `/O2`, optimized the pipeline again.

This is the I2 enforcement path: `admit_call()` is where possession of a capability is checked. Can the bodies be
compiled once without changing what the pipeline does?

## 2. Designs considered

- **A. Define the functions out of line in `src/core/tool_pipeline.cpp` (chosen).** The same split as ADR-200: the
  functions are not templates, so this is an ordinary header/source split.
- **B. Leave it.** Every edit to the pipeline keeps recompiling about 206 files.

## 3. Decision

Design A.

- **What moves.** Every non-template `inline` function whose body has 6 or more lines between its opening-brace line
  and its closing brace, and that is not `constexpr`: 13 functions, 466 lines counting each opening-brace line through
  its closing brace (440 lines of interior), in `agentengine` and
  `agentengine::tool_pipeline_detail`. Bodies are moved verbatim by a script; the `.cpp` reproduces the namespaces.
  Declarations drop `inline` and keep `[[nodiscard]]`, `noexcept` and default arguments.
- **What stays in the header.** Every type (`ToolCallRequest`, `IdempotencyKey`, `ToolInvocationAudit`,
  `AdmittedCall`, `AdmittedCallOutcome`, `ConcurrencyClass`, ...), every declaration with its comment, and the short
  functions (`malformed_arguments_error()`, `derive_idempotency_key()`, `make_denial_result()`,
  `enforce_hook_rewritten_tool_call_provenance()`).
- **Link.** `agentengine_tool_pipeline` is a static library linked through `agentengine::core`'s interface.
  `agentengine_rt_session` (ADR-199's compiled turn loop, which calls the pipeline and takes the include path directly
  rather than linking `agentengine::core`) links it `PUBLIC`, so a single-pass linker sees it after the session
  library. Every other compiled library that calls the pipeline already links `agentengine::core`.
- **Compile-fail gates.** The ADR-096 C2 and ADR-195 §3.8 gates compile `agent_session.hpp` into isolated
  mini-projects with an explicit source list, so that a missing link dependency can never pass for "correctly
  rejected". `agent_session_core.cpp` now calls the moved functions, so both lists gain `src/core/tool_pipeline.cpp`.

## 4. Behaviour — what is and is not the same

The claim is that no observable behaviour changes. What differs, precisely:

1. The moved functions are no longer `inline`, so a caller in another file can no longer inline them. Each runs once
   per tool call or per batch, not per token.
2. The bodies are compiled with the library's flags. `tests/` strips `NDEBUG` and `AGENTENGINE_FAST_TESTS` builds it
   without optimization. The header and every project header it includes contain no `assert` and no `#if` (only
   `static_assert`s on template parameters), so neither setting can make the library's and a test's view of a type
   differ. The same property ADR-199 §4.5 and ADR-200 §4.2 record.
3. The admission (I2) and approval checks moved with everything else and are unchanged; see §5 and §8.

## 5. Evidence

- **Moved verbatim.** A normalized line diff (comments, blank lines and whitespace dropped) between the old header and
  the new header plus `src/core/tool_pipeline.cpp`: every line that exists on only one side is a signature (the old
  `inline` definition line, the new declaration, the new definition, and parameter lines that now appear in both) or
  the new file's `#include` and namespace lines. No body line differs.
- **Builds.** Full MSVC dev-preset build, `/W4 /WX`: no warnings; configure accepts every `try_compile` gate. The new
  source compiles clean under g++-14 `-O3` and `-O0` with `-DNDEBUG -Wall -Wextra -Werror`.
- **Tests.** Full non-live suite: 321/321 pass (2 skipped as always; the 11 Docker-daemon tests and
  `test_external_skill_discovery` excluded, as in ADR-200 §5).

## 6. Measurements

Same method as ADR-200 §6: the `release` preset's own compile commands (`/O2 /Ob2 /DNDEBUG`), one file at a time, each
twice with the faster kept; "before" swaps in the header from `main`.

| Translation unit | CPU before → after |
|---|---|
| `test_approval_decider_principal.cpp` (calls the pipeline) | 8.3 → 7.1 s (−14 %) |
| `test_effect_reexecution.cpp` (calls the pipeline) | 6.6 → 5.3 s (−20 %) |
| `examples/02_add_tools.cpp` | 11.4 → 11.4 s |
| `test_rt_agent_session_stream_retry.cpp` | 19.9 → 19.7 s |
| `test_json_value.cpp` (control) | 2.5 → 2.5 s |

`src/core/tool_pipeline.cpp` costs 2.6 s wall / 5.4 s CPU once per build. The cold-build saving is modest: an unused
`inline` function is never code-generated, so only files that call the pipeline directly save its code generation, and
the agent-session files already reach it through ADR-199's compiled core.

The main saving is the edit loop, and this is the most-edited header. Measured on the same machine, dev preset, `-j4`,
by touching a file and running the real incremental build:

| Edit | Compiles | Links | Wall |
|---|---|---|---|
| A pipeline body, before (the header) | 166 | 329 | 139 s |
| A pipeline body, after (`tool_pipeline.cpp`) | 1 | 326 | 58 s |

## 7. Residuals

- Most of the remaining 58 s is relinking about 326 test executables, because every one links the changed static
  library. That is a property of the one-executable-per-test layout, not of this change; it is recorded here as a
  lead for later work, not addressed.

- The header still includes everything it did, because including files rely on it for transitive includes (the same
  residual as ADR-199 §7 and ADR-200 §7).
- **A host can replace a moved function at link time (red team M1, disclosed).** A static library contributes an object
  file only for symbols still unresolved. A host that defines, say, `agentengine::admit_call` or `invoke_tool` with the
  exact signature in its own source, and whose other linked objects reference no other function from
  `tool_pipeline.cpp`, links cleanly and runs its own definition, on MSVC and GNU ld alike (probed: the host's version
  ran). If any linked object references another moved function, the link fails with a duplicate symbol. Before this
  change the same override already linked silently under GNU ld and was rejected by MSVC (LNK2005), so the new
  exposure is MSVC-only. It needs deliberate host code with the exact signature; host code is trusted (CLAUDE.md, I2/I3
  concern model output and ambient authority, not the host's own link line), so this is outside the invariants. ADR-199
  and ADR-200 have the same property.

## 8. Red team round 1 (2026-09-26)

An independent pass with its own tools, and a positive control for every probe that came back clean:

- **Verbatim.** Its own brace-, string- and comment-aware extractor matched all 13 definitions; each body is
  byte-identical, none missing or duplicated. Control: a changed literal was flagged.
- **Header and signatures.** The old header with each moved body replaced by `;` and `inline` dropped is token-identical
  to the new one apart from the banner, so `[[nodiscard]]`, `noexcept`, default arguments and namespaces are unchanged;
  each definition matches its declaration. Controls: a dropped default and a dropped `noexcept` were flagged.
- **Name lookup.** No moved body uses a name declared later in the header; the `.cpp` includes nothing else. Under
  g++-14 `-O0`, the old bodies (force-emitted) and the new object have identical call targets for 4,022 shared symbols
  across 9,489 relocations; the differences are artefacts of no longer being inline (constructor aliases, a lambda's
  typeinfo). Control: a later `operator!=` added at the end of the header showed up as a new call.
- **Configuration.** None of the 16 project headers pulled in has `#if`, `assert` or a macro test (control: the same
  grep finds them where they exist), so no consumer macro can change a type's layout.
- **Link.** `dumpbin` over the 14 built libraries: only `agentengine_rt_session` and `agentengine_native_jail_backend`
  reference moved functions, and both reach the new library. A Linux CMake/Ninja mock of the real link graph emits the
  new library after `rt_session`; control: without `rt_session`'s `PUBLIC` link, `ld` fails with `undefined reference`.
  Only the two gate lists this ADR edits reach the header, on both platforms, and each negative gate shares its list
  with a positive control that must link.
- **I2.** `admit_call`, `run_admitted_call`, `invoke_tool`, `background_task` and the approval functions are identical
  by token and by call target.
- **MINOR — link-time override (disclosed, §7).**
- **MINOR — bookkeeping (fixed).** The ADR index row was missing, and the `.cpp` banner mentioned function templates
  the header does not have.
