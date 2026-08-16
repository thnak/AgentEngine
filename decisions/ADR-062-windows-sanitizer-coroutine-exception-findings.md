# ADR-062 — Are the Windows ASan/UBSan exception-`what()` findings real defects?

**Status:** Proposed (2026-08-16, **revised twice the same day**: once after an independent red-team
pass returned 14 findings, then again in **§9** after the experiments §7d proposed were actually
run). **Awaiting Judged — project owner sign-off.**

**The headline, which §1-§8 below do not yet know.** §9's experiments answer the question. A
**12-line standard-C++ program with no coroutine and no AgentEngine header** — `throw` →
`std::current_exception()` → `std::rethrow_exception()` → `catch (std::runtime_error const&)` →
`e.what()` — takes an AddressSanitizer access violation under clang on Windows, and is clean under
UBSan alone. **No AgentEngine code is implicated.** Read §1-§8 as the reasoning that got there,
including two hypotheses of its own that the measurement killed; read §9 for what is actually true.
§9.4 also withdraws §7b, this ADR's own most consequential recommendation.

**Revision note, kept because the reasoning that was wrong is part of the record.** The first draft
of this ADR was materially wrong in ways an independent red-team pass found and this version fixes:
it misidentified the mechanism (claimed all three findings were "an exception propagating out of a
coroutine frame"; two of the three do not involve a coroutine at the fault site at all — §2), it
gave its preferred hypothesis an invented fourth verdict label while giving the competing hypothesis
the cold correct one (§6), it presented the ASan finding as evidence *for* the artifact reading when
it is evidence *against* it (§5 E3), it justified its central decision by citing a project rule that
does not exist (§7b), and it declared the decisive experiments impossible when they are one line of
YAML in a workflow this same branch was editing (§7d). The original file name is kept for link
stability; the title changed because the old one embedded the mechanism error.

**Relates to:** ADR-015 (`windows-shell-fuzz`, whose runtime-PATH recipe fixed this job), ADR-037
(`agentengine::rt::task<T>`), 021 §5, 022 §5 (positive controls mandatory).

**Precondition, and why this ADR exists at all:** the `Windows / clang-cl / ASanUBSan` CI job had
never executed a single test since it was introduced. Every binary ran to its per-test timeout
producing zero output — 0 passed, 41 timed out at 180s each — because clang's sanitizer runtime DLLs
were never on `PATH` at test time (fixed in `9c81069`). ASan and UBSan had therefore **never once run
over this codebase on Windows**. (Correcting the first draft's arithmetic: 41 × 180s ÷ 4 workers ≈ 31
minutes, so the run went red on its own; the 45-minute job cap did *not* fire. The cap fired on
earlier runs of the same job.)

## 1. The question

**Stated so it has a wrong answer:** are the three sanitizer findings that appeared the first time
ASan+UBSan actually executed on Windows defects in AgentEngine's own code, or artifacts of the
sanitizers' interaction with the Windows toolchain?

Both wrong answers cost. "Artifact" when it is a defect ships a memory-safety bug on the primary
platform. "Defect" when it is an artifact rewrites correct, portable code to appease a
platform-specific false positive, and teaches the project that sanitizer output is negotiable.

## 2. The findings, and what they actually have in common

Full configuration, reported completely (the first draft omitted the last two defines, which are
directly load-bearing for finding 3): clang from `choco install llvm` — **version unpinned in
`ci.yml`**, reported by the job as resource dir `.../lib/clang/20/lib/windows` — MSVC STL, static CRT
(`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`), `CMAKE_BUILD_TYPE=RelWithDebInfo`,
`-fsanitize=address,undefined -fno-sanitize-recover=undefined -D_DISABLE_STRING_ANNOTATION
-D_DISABLE_VECTOR_ANNOTATION -g -O1`.

```
tests/test_rt_thread_pool.cpp:126:43: runtime error: upcast of misaligned address 0x00000000001e
    for type 'std::runtime_error', which requires 8 byte alignment
tests/test_rt_task.cpp:114:20: runtime error: upcast of misaligned address 0x5c00300032005c
    for type 'std::runtime_error', which requires 8 byte alignment
==1732==ERROR: AddressSanitizer: access-violation on unknown address 0xffffffffffffffff
SUMMARY: AddressSanitizer: access-violation include/agentengine/core/middleware.hpp:153
    in agentengine::middleware_detail::run_before<0,std::tuple<ThrowingMiddleware>>
```

**The common mechanism, corrected.** The reported line:column values identify the expressions
exactly, and they are not what the first draft said:

| Finding | Expression | Enclosing scope |
|---|---|---|
| `test_rt_task.cpp:114:20` | `what = e.what();` — col 20 is `e` | `int main()` — **not a coroutine** |
| `test_rt_thread_pool.cpp:126:43` | `std::string(e.what())` — col 43 is `e` | plain block — **not a coroutine** |
| `middleware.hpp:153` | `e.what()` in a catch | `run_before` — a coroutine |

All three are **an upcast of a caught exception reference in order to call `std::exception::what()`**.
Two of the three reach that catch via `std::rethrow_exception(...)` of an `std::exception_ptr` that a
coroutine promise captured earlier (`rt/task.hpp` `fault_ = std::current_exception()`); the throw and
the catch are in ordinary, non-coroutine code. So the shared factor is the **`exception_ptr`
round-trip and the derived→base upcast**, not a coroutine frame at the fault site.

This correction matters twice over. It falsifies the first draft's "three tests, one mechanism
(coroutines)" steelman, and it means the reproducer that draft proposed — "throw from a coroutine,
catch after resume" — **could not have reproduced two of the three findings**. §7d is re-aimed
accordingly.

Job result: **161/169**, up from 0/169. The other 5 failures are pre-existing `test_native_jail_*`
AppContainer flakiness (§7c).

## 3. The competing hypotheses

**H1 — a real defect in this project's exception propagation.** `rt::task<T>` and `rt::ThreadPool`
store a fault via `std::current_exception()` and rethrow it later from a different stack context. If
the stored `exception_ptr`, or the object it owns, is mishandled, the caught reference is garbage —
which is exactly what the reported addresses look like. Three independent tests exercise that path
and all three report.

**H2 — a Windows-toolchain artifact.** Not (as the first draft claimed) "the checkers do not model
the SEH ABI". That is false as stated: UBSan's alignment check has no ABI model — it loads the
pointer value the generated code holds and tests the low bits. The only defensible artifact form is
narrower: **the MSVC STL's `exception_ptr` implementation (`__ExceptionPtrCopyException` and
friends) under the *static* CRT, combined with the sanitizers' interceptors, yields a handler
reference the checks read as garbage.**

**H3 — sanitizer-induced corruption (new; the first draft did not consider it).** A variant of H2
that is not benign: the ASan runtime *perturbs* the MSVC exception-object copy such that the caught
reference really is invalid. Under H3 the checks are reporting truthfully, the program is correct,
and the corruption exists only under instrumentation — but it is still real memory corruption on the
primary platform's toolchain, not a false positive to be waved away.

## 4. Red team

**A1. "Two of three files are untouched" proves nothing about defect-vs-artifact.** Conceded.
Provenance separates *this session introduced it* from *it is old*; it is reported in §5 E2 as a
scope check only.

**A2. "It passes on Linux" is worthless if the Linux harness cannot fail.** Answered by §5.1 — and
only partially, see A7.

**A3. Garbage-looking addresses are equally consistent with H1.** Correct. A genuine dangling
exception reference prints nonsense too. E3 is weighted as suggestive, not probative.

**A4. A defect could manifest only under the Windows toolchain.** Unfalsifiable by a Linux run. It is
why C4 reads INCONCLUSIVE.

**A5. `ThrowingMiddleware` was edited in `0569828`.** The third finding is reached through a struct
this session modified (the C4702 fix made its throw conditional on a never-assigned member). That
does not change what is thrown or from where — but that is an argument, not a measurement, and the
measurement (§7d.4) has not been taken.

**A6 (new — attacking the DECISION, which the first draft never did).** The first draft red-teamed
only the evidence for its preferred hypothesis, never §7 itself. The attack it dodged: *a first-ever
sanitizer run that produced a real access violation on the primary platform is the highest-value
lead in this repository, and the decision is to stop looking.* Under H3 that is worse than
conservative — it defers a live memory-corruption question. The answer is not argument: it is §7d's
one-line experiments, which are now scheduled rather than declared impossible.

**A7 (new).** §5.1's positive control exercises **ASan** against a **stack-lifetime** bug. Two of
three findings are **UBSan alignment/upcast**, whose teeth that control never tested — and it ran in
a *different binary* (`run_task_sync.hpp` is included by `test_middleware_model_call_gateway.cpp`,
but by neither `test_rt_task.cpp` nor `test_rt_thread_pool.cpp`). Answered by the second control,
§5.2.

## 5. Executed evidence

**E1 — same source, same checkers, different platform stack.** Column headers stated honestly: this
comparison changes **five** variables at once, not one. The exception ABI is one candidate among
them and not obviously the most likely — the STL implementing `exception_ptr` is the one §2 now
points at.

| Test | Windows: clang 20 · MSVC STL · static CRT · SEH · annotations off | Linux: clang 21.1.8 · libstdc++ · glibc · Itanium |
|---|---|---|
| `test_rt_task` | UBSan misaligned upcast | **exit 0, ALL PASS** |
| `test_rt_thread_pool` | UBSan misaligned upcast | **exit 0, ALL PASS** |
| `test_middleware_model_call_gateway` | ASan access-violation | **exit 0, all checks passed** |

Flags matched to CI where they are platform-independent (`-fsanitize=address,undefined
-fno-sanitize-recover=undefined -g -O1`, `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`).

**E2 — provenance (scope check only, per A1).** Against `04b13a1`: `tests/test_rt_task.cpp`,
`tests/test_rt_thread_pool.cpp`, `include/agentengine/core/middleware.hpp` and
`include/agentengine/rt/task.hpp` are byte-identical. **Two files in the third finding's binary are
not**: `tests/test_middleware_model_call_gateway.cpp` (A5) and `tests/support/run_task_sync.hpp`
(21 lines — the stack-use-after-scope fix in `d40daa8`). The first draft's provenance list omitted
the second, which is exactly the kind of omission that flatters a conclusion.

**E3 — the reported addresses, split by class (the first draft summed these into one paragraph, and
that was its worst analytical error).**

- *The two UBSan reports.* `0x5c00300032005c` decodes as little-endian UTF-16 (`\`, `2`, `0`, `\`) —
  string bytes read as an object pointer; `0x1e` is 30. These are the pointer values actually held
  at the catch site. Consistent with H1, H2 and H3 alike; they do not discriminate.
- *The ASan report.* `access-violation` on Windows is **not** a shadow-memory verdict — it is ASan's
  deadly-signal path: the process took a real `EXCEPTION_ACCESS_VIOLATION` and ASan's handler printed
  it. "unknown address" means *not described by shadow memory*, not *not resolved*, and
  `0xffffffffffffffff` is `(size_t)-1`. **A checker that merely misreads something cannot cause a
  hardware fault.** Finding 3 is therefore evidence *against* the benign reading of H2, and for
  either H1 or H3. The first draft filed it as support for H2. That was backwards.

### 5.1 Positive control #1 — ASan teeth (mandatory, 022 §5)

The Linux harness was tested against a known real bug: the immediately-invoked lambda coroutine whose
closure died before `resume()` (fixed in `d40daa8`), reintroduced into `tests/support/
run_task_sync.hpp` and then reverted.

```
-- reintroduced the known bug --
POSITIVE CONTROL exit=1   (nonzero = harness has teeth)
==406==ERROR: AddressSanitizer: stack-use-after-scope
-- fix restored --
AFTER RESTORE exit=0
```

Scope, stated rather than glossed (A7): this proves **ASan** detects a **stack-lifetime** defect, in
the binary of finding 3 only.

### 5.2 Positive control #2 — UBSan instrumentation is actually emitted at the reported shape

A2/A7's remaining gap: a clean UBSan result means nothing unless the check is emitted at all. The
exact reported shape (`rethrow_exception` → `catch (std::runtime_error const&)` → `e.what()`) was
compiled to LLVM IR with clang 21.1.8:

```
handler.type_mismatch4:                           ; preds = %catch
  call void @__ubsan_handle_type_mismatch_v1_abort(ptr nonnull @4, i64 %42)
```

The alignment/upcast check sits **inside the catch block**, and disappears under
`-fno-sanitize=alignment` (3 handlers → 2). So on Linux the check really is present, really did
execute, and really did pass. This strengthens C2 — and it is the control the first draft should
have run instead of arguing about address shapes.

Still not covered by either control: whether this harness would catch a defect specific to the
Windows toolchain. Nothing available on Linux can establish that.

## 6. Per-claim verdicts

Three labels only, per `decisions/README.md`. The first draft's "PLAUSIBLE, NOT PROVEN" was an
invented fourth label applied to exactly one hypothesis — the one that licensed doing nothing.

| # | Claim | Verdict |
|---|---|---|
| **C0** | **§1's actual question is answered: defect or artifact** | **INCONCLUSIVE** |
| C1 | This session's changes caused the findings | **WRONG** for findings 1-2 (E2, byte-identical). **INCONCLUSIVE** for finding 3 (A5, E2) |
| C2 | The findings reproduce on Linux under the same checkers | **WRONG** — E1, with ASan teeth from §5.1 and UBSan emission confirmed by §5.2 |
| C3 | H2 — a benign Windows-toolchain artifact | **INCONCLUSIVE**, and E3 argues against it for finding 3 specifically |
| C4 | H1 — a real defect manifesting only under the Windows toolchain | **INCONCLUSIVE** |
| C5 | H3 — sanitizer-induced real corruption under instrumentation | **INCONCLUSIVE** (not considered at all by the first draft) |
| C6 | The Linux harness can detect a stack-lifetime defect via ASan | **CORRECT** — §5.1 |
| C7 | The Linux harness emits the UBSan check at the reported shape | **CORRECT** — §5.2 |

C0 is the row the first draft did not have. Without it the table read as two refutations and a
confirmation, which is a comfortable scoreboard over an investigation whose actual result is that the
question is open. C2 is retained but narrowed to what it is — a reachability claim — rather than the
strawman "code defects reachable on Linux", which nobody advanced.

## 7. Decision

**a. Change no production code on this evidence.** C0 is INCONCLUSIVE and C2 is WRONG; there is no
demonstrated defect to fix, and rewriting `rt::task<T>`'s fault path to quiet a checker that
complains on one toolchain would be changing correct code on a guess.

**b. Do not suppress, exclude, or `continue-on-error` the three tests; the job stays red.** Stated
without the appeal to authority the first draft used: it asserted "this project's rule is that a
warning or a sanitizer finding is fixed or red-teamed, never silenced" and cited it as binding.
**That rule does not exist.** `grep -rn "never silenc"` across the repository returns only this ADR;
CONVENTIONS.md §128 requires a warning-clean build under three compilers and says nothing about
sanitizer findings or red CI jobs. Inventing a rule in the sentence that cites it, to justify the
option requiring no work, is exactly the failure mode an ADR is supposed to prevent.

So this is a **proposal, not a derivation**: a green badge over three unexplained sanitizer findings
is worth less than a red one, and §7d's experiments should resolve it quickly enough that the red
window is short. The owner may reasonably decide otherwise; it is their CI.

**c. Out of scope, named:** the 5 `test_native_jail_*` failures in this job are AppContainer
environmental flakiness — the same two tests passed under `clang-cl / Release` and failed under
`MSVC / Release` within one run, on identical code. Different question.

**d. What settles it — available now, not impossible.** The first draft closed with "all require a
Windows clang toolchain, which the machine this was investigated on does not have." The machine
claim is true; the conclusion was not. This branch had just spent two commits editing
`.github/workflows/ci.yml`, **which provisions a Windows clang toolchain on every push**. In
descending order of information per line changed:

1. **Static CRT flip.** `ci.yml` `MultiThreaded` → `MultiThreadedDLL`, re-run. **One line.** §2 now
   points at the MSVC STL's `exception_ptr` under the static CRT; this is the single most
   discriminating experiment available and the first draft did not even list it.
2. **Split the sanitizers.** Configure `-fsanitize=address` and `-fsanitize=undefined` separately,
   and UBSan with `-fno-sanitize=alignment`. Isolates whether ASan's presence is what produces the
   access violation — i.e. tests H3 directly.
3. **A ~12-line standalone reproducer, re-aimed per §2**: `throw std::runtime_error` →
   `std::current_exception()` → `std::rethrow_exception` → `catch (std::runtime_error const&)` →
   `e.what()`, with **no coroutine and no AgentEngine headers**, built clang + `/MT` +
   `-fsanitize=address,undefined`. If ~12 lines reproduce it, the finding is the toolchain's,
   conclusively. (The first draft's coroutine-based reproducer could not have reproduced findings 1-2
   at all.)
4. **Close A5 by measurement:** restore `ThrowingMiddleware` to its pre-`0569828` form and re-run the
   ASanUBSan leg.

## 8. Residual risks

- **C4 and C5 both remain open**, and C5 is the one nobody had named: under H3 there is real memory
  corruption on the primary platform's instrumented builds, which would also mean every future ASan
  run on Windows is reporting through a compromised layer. §7d.2 tests it directly.
- **A standing red job erodes signal.** Every day this is red for known reasons raises the chance a
  *new* finding is ignored. This is the direct cost of §7b and the reason §7d is scheduled rather
  than aspirational.
- **161/169 is a first execution, not a clean bill.** The sanitizers have run once. Findings needing
  particular inputs, timing, or the tests currently failing for other reasons have not been reached.
- **`_DISABLE_STRING_ANNOTATION` / `_DISABLE_VECTOR_ANNOTATION` are in force**, so MSVC STL container
  overflow detection is off in this job — and finding 3 is inside a `std::string` concatenation.
  Whatever ASan would have said about that buffer, it was not asked.

## 9. Addendum (2026-08-16, same day) — the experiments were run, and they settle it

§7d listed four experiments and the first draft had declared them impossible. They were run on the
`adr-062-experiment` branch (workflow `.github/workflows/adr-062-experiment.yml`, temporary,
branch-only, never to be merged). Two rounds; round 1's reproducer legs failed on an error of mine
(`clang++` is the GNU-style driver and rejects MSVC's `/MT`, so all six legs died with "no such file
or directory: '/MT'"; the driver spelling is `-fms-runtime-lib=`). Round 2 fixed it.

### 9.1 The three reporting tests, across CRT and sanitizer axes

| CRT | sanitizers | `test_rt_task` | `test_rt_thread_pool` | `test_middleware_...` |
|---|---|---|---|---|
| MultiThreaded | **undefined only** | **0** | **0** | **0** |
| MultiThreaded | address only | 1 | 1 | 1 |
| MultiThreaded | both | 1 | 1 | 1 |
| MultiThreaded | both, `-fno-sanitize=alignment` | 1 | 1 | 1 |
| MultiThreadedDLL | address only | 1 | 1 | 1 |
| MultiThreadedDLL | both | 1 | 1 | 1 |
| MultiThreadedDLL | both, `-fno-sanitize=alignment` | 1 | 1 | 1 |

(`MultiThreadedDLL / undefined` is absent because it dies at CONFIGURE on an unrelated `lld-link
/failifmismatch` RuntimeLibrary error in a 007 §9 G2 `try_compile` control — an artifact of the flag
combination, not a result.)

**Two hypotheses die here.**

- **The static-CRT hypothesis is refuted.** `MultiThreaded` and `MultiThreadedDLL` are identical in
  every cell. §2's suspicion of MSVC STL's `exception_ptr` *under `/MT` specifically* was wrong.
- **UBSan is not the agent.** Under **UBSan alone every test passes**. The misaligned pointer the
  first draft spent five paragraphs analysing therefore *does not exist* in the program as compiled
  without ASan. Disabling the alignment check does not help either: the ASan faults remain.

### 9.2 The decisive one: a 12-line standard-C++ reproducer

`tests/experiments/exception_ptr_upcast_repro.cpp` — `throw` → `std::current_exception()` →
`std::rethrow_exception()` → `catch (std::runtime_error const&)` → `e.what()`. **No coroutine, no
AgentEngine header, nothing from this project.**

| leg | result |
|---|---|
| `MT / undefined` | **exit 0** — prints `what=boom`, `repro: clean` |
| `MT / address` | **ASan access-violation, `exception_ptr_upcast_repro.cpp:31 in main`** |
| `MT / both` | ASan access-violation |
| `MD / address` | exit `-1073740791` = `0xC0000409` `STATUS_STACK_BUFFER_OVERRUN` |
| `MD / both` | exit `-1073740791` |

Line 31 is `std::printf("what=%s\n", e.what());` — precisely the upcast under test.

**Twelve lines of standard C++ crash under clang's AddressSanitizer on Windows, and are clean under
UBSan alone.** Nothing in AgentEngine is implicated in any way.

### 9.3 Revised verdicts

| # | Claim | Was | Now |
|---|---|---|---|
| C0 | §1's question: defect or artifact | INCONCLUSIVE | **ARTIFACT — toolchain.** §9.2 |
| C2 | Reproduces on Linux under the same checkers | WRONG | **WRONG** (unchanged) |
| C3 | H2, a *benign* Windows artifact | INCONCLUSIVE | **WRONG** — nothing benign about it; ASan induces a real fault (§5 E3 was right that a mere misreading cannot fault) |
| C4 | H1, a real defect in our code, Windows-only | INCONCLUSIVE | **WRONG** — refuted by §9.2; no AgentEngine code is involved |
| C5 | H3, sanitizer-induced corruption | INCONCLUSIVE | **CORRECT** — ASan's presence is necessary and sufficient |
| C6/C7 | Linux harness teeth (ASan) / UBSan emission | CORRECT | unchanged |

The hypothesis that turned out to be right (H3) is the one the first draft did not contain at all; it
was added only because an independent red-team pass insisted the artifact story, as written, could
not produce a hardware fault.

### 9.4 What this changes about the decision

§7a stands, now on evidence rather than absence of it: **no production code changes**, because no
production code is implicated.

**§7b does not stand.** It kept the job red pending an explanation, on the reasoning that a green
badge over unexplained findings is worth less than a red one. The findings are now explained, and the
explanation is that clang's ASan on this platform cannot run *any* program that round-trips an
exception through `std::exception_ptr` — a category that includes essentially every error path in
this codebase. Holding a required job red against a toolchain defect we do not own, indefinitely,
buys nothing and steadily erodes the signal §8 already warned about.

**Recommended, for owner decision (this ADR does not enact it):**
1. Report upstream to LLVM with `exception_ptr_upcast_repro.cpp` as-is; record the issue number here.
   **Not yet done, and worth stating plainly: this ADR has not checked whether it is already a known
   LLVM bug.** That check comes before filing.
2. Until upstream resolves, run the ASanUBSan job with **UBSan only** — measured clean at §9.1, so it
   keeps real coverage instead of none, which is what the job had for its entire existence.
3. Revisit when the toolchain moves. `choco install llvm` is unpinned in `ci.yml` (§2), so this can
   change under us without notice — pinning it is a separate, and probably overdue, decision.

### 9.5 Residual, honestly

- **The mechanism inside ASan is not identified.** "ASan's presence is necessary and sufficient" is
  measured; *why* is not. That is upstream's question, but it means this ADR cannot rule out a
  narrower trigger that some AgentEngine pattern happens to hit more often.
- **`MD / undefined` never built** in either round (§9.1's note), so the UBSan-only row is confirmed
  on the static CRT only.
- **The five `test_native_jail_*` failures are untouched** by all of this (§7c).
