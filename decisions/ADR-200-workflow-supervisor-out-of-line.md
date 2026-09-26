# ADR-200 — `rt::WorkflowSupervisor`'s member bodies compiled once in `src/`, not in every file that includes it

- **Status**: **Proposed — design + implementation + proof (2026-09-26); red team round 1 (same day): no BLOCKER or
  MAJOR, 2 MINOR, both fixed (§8).**
- **Date**: 2026-09-26
- **Origin**: GitHub issue #120 (S1), the structure audit after issue #115 closed.
- **Touches**: `include/agentengine/rt/workflow_supervisor.hpp` (declarations), `src/rt/workflow_supervisor.cpp` (new),
  `CMakeLists.txt` (`agentengine_rt_workflow`, linked through `agentengine::core`).

## 1. The question

`rt::WorkflowSupervisor` is not a template, but all 2,660 lines of it were in a header, with every member function
defined in the class. Every file that included it compiled and optimized the whole superstep loop again: `execute()`
(a 505-line coroutine), `resume_workflow_locked()` (182 lines), `route_from()` (132 lines) and 30 more. The #120 audit
counted 46 including files (28 tests, 18 examples), measured about 1.9 s of MSVC `/O2` code generation per file on top
of about 0.5 s of parsing, and 30 commits to the header since 2026-08-01, each of which recompiled all 46.

Can the bodies be compiled once, without changing what a supervisor does?

## 2. Designs considered

- **A. Define the members out of line in `src/rt/workflow_supervisor.cpp` (chosen).** The class is not a template, so
  this is an ordinary header/source split. Nothing needs virtual hooks, unlike ADR-199's `AgentSessionCore`.
- **B. Keep the header, add precompiled headers or unity builds.** They cut parsing, not code generation, and #115
  measured the optimizer as the cost. Not a substitute.
- **C. Leave it.** The file is the third most-changed header; every edit keeps costing 46 recompiles.

## 3. Decision

Design A.

- **What moves.** Every member function whose body has 6 or more lines between its opening-brace line and its
  closing brace (so a 6-line function counting both braces stays) and that is neither `constexpr` nor a template:
  33 functions, 1,381 body lines. Bodies are moved verbatim by a script. Default arguments stay on the declarations;
  definitions qualify nested types (`WorkflowSupervisor::route_result`, `::InteractionAsk`, `::EdgeFailurePolicy`).
- **What stays in the header.** The class and every declaration, with every comment at its declaration (the comments
  are where the rules are written down, so a reader of the header still sees them); accessors and other bodies under 6
  lines; `max_nesting_depth()` (`constexpr`); the constructor (an initializer list, one line); `drive<T>()` (a
  template); the nested `ScopedForwardedEventSink`; and the free function templates `save_workflow_checkpoint` and
  `load_workflow_checkpoint` (they take any `SessionStore`).
- **Link.** `agentengine_rt_workflow` is a static library linked through `agentengine::core`'s interface, like
  `agentengine_rt_file_log` (ADR-195 E32) and `agentengine_rt_session` (ADR-199). No consumer needs a CMake change.

## 4. Behaviour — what is and is not the same

The claim is that no observable behaviour changes. What differs, precisely:

1. The moved members are no longer implicitly `inline`. A caller in another file can no longer inline them. They are
   per-run or per-superstep operations (entry points, routing, fan-in bookkeeping), not per-token work, so the cost is
   a direct call where there was sometimes an inlined body.
2. The bodies are compiled with the library's flags. In Release, `NDEBUG` is defined for `src/` and stripped for
   `tests/`, and `AGENTENGINE_FAST_TESTS` builds `tests/` without optimization. The header contains no `assert` and no
   `#if`, so neither can make the library's and a test's view of the class differ. This is the same property ADR-199
   §4.5 records for `agentengine_rt_session`.
3. The ADR-169 admission path (`admit_caller()`, `deny_admission()`, `propagate_admission_to_children()`) moved with
   everything else and is unchanged; `admit_caller()` itself stayed inline (5 lines).

## 5. Evidence

- **Moved verbatim.** A normalized line diff (comments, blank lines and whitespace dropped) between the old header and
  the new header plus `src/rt/workflow_supervisor.cpp`: 25 lines exist only in the old file and 81 only in the new ones,
  and every one of them is a signature (the in-class definition line, the new declaration, the qualified definition) or
  the new file's `#include` and namespace lines. No body line differs.
- **Builds.** Full MSVC dev-preset build (`/O2` library, `/Od` tests), `/W4 /WX`: no warnings. The new source also
  compiles clean under g++-14 `-O3 -DNDEBUG -Wall -Wextra -Werror` (checked separately, because moving
  `FileAppendLogStore::append()` into a `.cpp` in #121 exposed a gcc-only `-Wfree-nonheap-object` false positive).
- **Tests.** Full non-live suite: 321/321 pass (2 skipped as always: the `*_no_process_creation` checks; the 11
  Docker-daemon tests and `test_external_skill_discovery`, which needs a symlinked skill on the machine, excluded).

## 6. Measurements

Single translation units, MSVC 14.51, the `release` preset's own compile commands (`/O2 /Ob2 /DNDEBUG`), compiled
serially, one at a time, each twice with the faster run kept, same machine and session. "Before" is the same command
with the header from `main`. CPU is the compiler process's total CPU time (the back end is multi-threaded).

| Translation unit | Wall before → after | CPU before → after |
|---|---|---|
| `tests/workflow/test_rt_workflow_supervisor.cpp` | 6.3 → 4.6 s | 17.1 → 11.2 s (−35 %) |
| `tests/workflow/test_rt_workflow_sub_workflow.cpp` | 5.8 → 3.9 s | 15.8 → 8.6 s (−46 %) |
| `examples/04_first_workflow.cpp` | 5.2 → 3.6 s | 13.3 → 6.7 s (−50 %) |
| `tests/core/json/test_json_value.cpp` (control: no supervisor) | 1.6 → 1.6 s | 2.5 → 2.4 s |

`src/rt/workflow_supervisor.cpp` itself costs 4.9 s wall / 11.6 s CPU, once per build. Against about 6 CPU-s saved in
each of the ~46 including files, a cold build saves on the order of 250 CPU-s under `/O2`, and an edit to a moved body
recompiles one file instead of 46. The control file does not move, so the difference is not machine noise.

## 7. Residuals

- The header still includes everything it did, because including files rely on it for transitive includes. Trimming
  them is further front-end time, not done here (the same residual as ADR-199 §7).

## 8. Red team round 1 (2026-09-26)

An independent pass, with its own tools, and a positive control for every probe that came back clean:

- **Verbatim.** Its own brace- and string-aware extractor matched all 33 definitions (none missing or duplicated): each
  body is byte-identical to the old in-class body minus the 4-space indent. Control: one changed literal was flagged.
- **Header.** The old class with each of the 33 bodies replaced by `;` is token-identical to the new class, comments
  included, so attributes, `static`, default arguments, access and friends are unchanged. A TU including only the
  header has exactly 33 undefined `WorkflowSupervisor::` symbols under g++. Control: a flipped default was flagged.
- **Name lookup (the main risk of moving a body out of a class).** Out of line, a body also sees namespace-scope names
  declared after the class. The header declares only `save_workflow_checkpoint` and `load_workflow_checkpoint` after
  the class, and no moved body uses them; the `.cpp` includes nothing else. Empirically: the old inline bodies (g++-14
  `-O0`, every member emitted) and the new `.cpp` have identical call targets for all 83 shared symbols across 2,436
  relocations; the only differences are artefacts of no longer being inline (constructor aliases, local lambda
  typeinfo, lambda numbering in internal-linkage names). Control: a later, better-matching `failure_marker` overload
  added after the class was detected in `resume_workflow_locked`.
- **Configuration.** No moved body has a `static` local, `thread_local`, `assert`, `#if`, `__FILE__` or
  `source_location`; neither the header nor the 56 project headers it pulls in has `#if` or `assert`. Nothing uses a
  moved member in a constant expression, `decltype` or by address.
- **Link.** All 46 test and example targets that reach the header link `agentengine::core`; no `try_compile` gate, tool
  or bench source reaches it. A Linux link and run (library `-O2 -DNDEBUG`, tests `-O0` without NDEBUG) passed seven
  workflow tests, admission included.
- **Security (ADR-169).** `admit_caller`, `set_principal` and `set_require_caller` stayed inline and unchanged;
  `deny_admission` and `propagate_admission_to_children` are identical by token and by call target. One new
  possibility: a host `.cpp` could define a moved member itself. That fails at link with a duplicate symbol unless
  the host replaces every moved member the linked objects reference (corrected by ADR-201 §7, whose red team probed the
  same property: with no other reference into the object, the host's definition silently wins). Host code is trusted, so this is outside the I2/I3 model.
- **MINOR — stale evidence (fixed).** The first build and test run predated the 3-line header banner. Rebuilt and
  re-ran; §5 reports the re-run.
- **MINOR — unused link dependencies (fixed).** The library linked `agentengine_rt_file_log` and `ws2_32` PUBLIC, but
  its object references no symbol from either. Both removed.
- **Wording (fixed).** §3 now says how body lines are counted.
