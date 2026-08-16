# ADR-062 — Are the Windows ASan/UBSan coroutine-exception findings real defects?

**Status:** Proposed (2026-08-16). Designed, self-red-teamed (§4), evidence executed (§5) with a
mandatory positive control (§5.1). **Awaiting Judged — project owner sign-off.** The decision in §7
deliberately changes no production code; it is a decision about what the evidence licenses, and
what it does not.

**Relates to:** ADR-015 (the `windows-shell-fuzz` job whose runtime-PATH recipe fixed this job),
ADR-037 (`agentengine::rt::task<T>`, the coroutine type under suspicion), 021 §5 (Platform Support:
Windows/MSVC, Linux/gcc, clang), 022 §5 (positive controls are mandatory for security claims).

**Precondition, and the reason this ADR exists at all:** the `Windows / clang-cl / ASanUBSan` CI job
had never executed a single test since it was introduced. Its Test step ran every binary to its full
timeout producing zero output — 0 passed, 41 timed out at 180s each before the 45-minute job cap
killed the run — because clang's sanitizer runtime DLLs were never placed on `PATH` at test time
(fixed in `9c81069`, mirroring the recipe `windows-shell-fuzz` already used). ASan and UBSan had
therefore **never once run over this codebase on Windows**. The findings below are the first output
they have ever produced, which is precisely why they must not be dismissed casually — and precisely
why they must not be "fixed" casually either.

## 1. The question

**Stated so it has a wrong answer:** are the three sanitizer findings that appeared the first time
ASan+UBSan actually executed on Windows — all three of them an exception propagating out of a
coroutine frame — defects in AgentEngine's own code, or artifacts of the sanitizers' interaction
with MSVC's SEH-based exception ABI?

The wrong answer in either direction has a real cost. Answer "artifact" when it is a defect and a
memory-safety bug in the coroutine substrate ships. Answer "defect" when it is an artifact and the
project rewrites correct, portable exception handling to appease a platform-specific false positive
— and, worse, learns that sanitizer output is negotiable.

## 2. The findings, verbatim

From run `31920201687`, job `Windows / clang-cl / ASanUBSan` (clang 20, `-fsanitize=address,undefined
-fno-sanitize-recover=undefined -g -O1`, static CRT, `RelWithDebInfo`):

```
tests/test_rt_thread_pool.cpp:126:43: runtime error: upcast of misaligned address 0x00000000001e
    for type 'std::runtime_error', which requires 8 byte alignment
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior test_rt_thread_pool.cpp:126:43

tests/test_rt_task.cpp:114:20: runtime error: upcast of misaligned address 0x5c00300032005c
    for type 'std::runtime_error', which requires 8 byte alignment
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior test_rt_task.cpp:114:20

==1732==ERROR: AddressSanitizer: access-violation on unknown address 0xffffffffffffffff
SUMMARY: AddressSanitizer: access-violation include/agentengine/core/middleware.hpp:153
    in agentengine::middleware_detail::run_before<0,std::tuple<ThrowingMiddleware>>
```

Job result: **161/169 passed**, up from 0/169. The other 5 failures are the pre-existing
`test_native_jail_*` AppContainer flakiness, out of scope here (§7c).

## 3. The competing hypotheses, each steelmanned

**H1 — Real defect in coroutine exception propagation.** `rt::task<T>`'s promise stores a fault via
`unhandled_exception()` into an `std::exception_ptr` and rethrows it later from a different stack
context (`rt/task.hpp`). Exception objects crossing a coroutine frame boundary is exactly the kind of
lifetime question that gets silently wrong, and *all three* findings sit on that one path. The
steelman is strong: three independent tests, one mechanism, and the reported addresses are garbage
— which is what a dangling or mis-derived exception-object pointer looks like. A codebase whose
sanitizers have never run is exactly where such a bug survives.

**H2 — Sanitizer/SEH/coroutine interaction, not a code defect.** Windows uses MSVC's SEH-based
exception ABI; the catch machinery hands the handler a pointer derived through structures ASan and
UBSan do not model the way they model the Itanium ABI. UBSan's `-fsanitize=alignment` upcast check
firing on an address that is plainly not an object pointer, and ASan reporting an "access-violation
on unknown address `0xffffffffffffffff`" (a sentinel, not an address it resolved), are both
signatures of the checker misreading the platform's unwind machinery rather than of the program
touching bad memory.

## 4. Red team — attacking the answer this ADR wants to give

The comfortable answer is H2, because H2 requires no work. So the attack is aimed there.

**A1. "Two of three files are untouched" proves nothing about whether the bug is old.** Correct, and
conceded. Provenance separates *"this session introduced it"* from *"it is a defect"*; it does not
separate *"defect"* from *"artifact"*. It is reported in §5 as what it is — a scope check, not
evidence for H2.

**A2. "It passes on Linux" is worthless if the Linux harness cannot fail.** This is the strongest
attack and it demanded an executed positive control, not an assurance. See §5.1.

**A3. Garbage-looking addresses are consistent with H1 too.** Also correct. A genuine dangling
exception pointer would *also* print nonsense. The address shapes are suggestive, not probative, and
are weighted accordingly below.

**A4. A latent defect could manifest only under the Windows ABI.** Unfalsifiable by a Linux run, and
it is the reason §6 records the verdict it does rather than a clean acquittal.

**A5. `ThrowingMiddleware` was edited in this branch (commit `0569828`).** True, and the honest
residual: the third finding is reached through a struct this session modified. The edit made the
throw conditional on a never-assigned member to clear MSVC C4702; it does not change what is thrown,
from where, or through what. That is an argument, not a measurement, and it is recorded as such.

## 5. Executed evidence

**E1 — Same source, same sanitizers, different exception ABI.** All three tests built with
clang++ 21.1.8 on Linux, flags matched to CI (`-fsanitize=address,undefined
-fno-sanitize-recover=undefined -g -O1`, `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`):

| Test | Windows (SEH ABI) | Linux (Itanium ABI) |
|---|---|---|
| `test_rt_task` | UBSan: misaligned upcast | **exit 0, ALL PASS, no diagnostics** |
| `test_rt_thread_pool` | UBSan: misaligned upcast | **exit 0, ALL PASS, no diagnostics** |
| `test_middleware_model_call_gateway` | ASan: access-violation | **exit 0, all checks passed** |

**E2 — Provenance (scope check, per A1).** Against `04b13a1`, the last commit before this session's
work: `tests/test_rt_task.cpp`, `tests/test_rt_thread_pool.cpp`, `include/agentengine/core/
middleware.hpp` and `include/agentengine/rt/task.hpp` are all byte-identical.
`tests/test_middleware_model_call_gateway.cpp` is not (see A5).

**E3 — Address shapes (weak, per A3).** `0x5c00300032005c` decodes as little-endian UTF-16 text
(`\`, `2`, `0`, `\`) — string bytes interpreted as an object pointer. `0xffffffffffffffff` is
`INVALID_HANDLE_VALUE`/`(size_t)-1`, a sentinel ASan reports when it has no resolved address.

### 5.1 Positive control (mandatory — 022 §5, and A2's answer)

A harness that reports nothing because it is inert would produce E1's table exactly. So the Linux
harness was tested against a **known, real bug of the same family**: the immediately-invoked lambda
coroutine whose closure temporary died before `resume()` — the stack-use-after-scope fixed in
`d40daa8`. It was reintroduced into `tests/support/run_task_sync.hpp`, run under the identical clang
Linux ASan+UBSan configuration, then reverted:

```
-- reintroduced the known bug --
POSITIVE CONTROL exit=1   (nonzero = harness has teeth)
==406==ERROR: AddressSanitizer: stack-use-after-scope on address 0x6d35122decc8
SUMMARY: AddressSanitizer: stack-use-after-scope
-- fix restored --
AFTER RESTORE exit=0      (0 = clean)
```

The harness detects a real coroutine-lifetime defect and goes quiet when it is fixed. E1's clean
results are therefore informative rather than vacuous. (`git status` confirmed clean afterwards — the
probe left no residue.)

**What the positive control does NOT establish:** that this harness would catch an
*exception-ABI-specific* defect. It proves teeth for coroutine frame/stack lifetime, the nearest
real bug available. A2 is answered; A4 is not.

## 6. Per-claim verdicts

| # | Claim | Verdict |
|---|---|---|
| C1 | This session's changes caused the findings | **WRONG** for two of three (E2: byte-identical files). **INCONCLUSIVE** for the third (A5) |
| C2 | The findings are code defects reachable on Linux | **WRONG** — E1, with teeth proven by §5.1 |
| C3 | The findings are Windows sanitizer/SEH artifacts (H2) | **PLAUSIBLE, NOT PROVEN.** E1+E3 point here; A3 and A4 keep it short of proof |
| C4 | The findings are real defects manifesting only under the SEH ABI (H1, narrowed) | **INCONCLUSIVE** — not excluded by any evidence gathered |
| C5 | The Linux ASan+UBSan harness can detect coroutine-lifetime defects | **CORRECT** — §5.1, executed both directions |

C3 and C4 are not complements dressed as one answer: the evidence discriminates *code defect
reachable anywhere* (refuted) from *everything else* (unresolved). Recording C4 as INCONCLUSIVE
rather than folding it into C3 is the point of §4's A4.

## 7. Decision

**a. Change no production code on this evidence.** C2 is refuted and C4 is inconclusive; rewriting
`rt::task<T>`'s exception path to quiet a checker that only complains under one ABI, with no
demonstrated defect, would be changing correct code on a guess.

**b. Do not suppress, exclude, or `--exclude-regex` the three tests, and do not mark the job
continue-on-error.** The job must stay red while this is unresolved. This project's rule is that a
warning or a sanitizer finding is fixed or red-teamed, never silenced — and a job that reports a
comfortable green while three sanitizer findings stand is worse than no job. The cost is accepted
explicitly: a standing red job is a known, named debt carried in the open, not an oversight.

**c. Out of scope, named:** the 5 `test_native_jail_*` failures in the same job are AppContainer
environmental flakiness — the same two tests passed on `clang-cl / Release` and failed on
`MSVC / Release` within this very run, on identical code. Different question, different ADR.

**d. What would settle it.** In rough order of decisiveness:
   1. A minimal standalone reproducer — throw from a coroutine, catch after resume — built with
      clang on Windows. If ~30 lines with no AgentEngine headers reproduces it, the finding is the
      toolchain's, conclusively.
   2. The same reproducer against a newer clang; if it is a known LLVM issue, an upstream bug
      reference retires this ADR.
   3. `-fsanitize=address` and `-fsanitize=undefined` separately, and UBSan with `alignment`
      disabled but the rest live — isolating which check fires narrows the mechanism.
   4. Re-running the third finding with `ThrowingMiddleware` restored to its pre-`0569828` form,
      closing A5 by measurement instead of argument.

None require a decision from this ADR; all require a Windows clang toolchain, which the machine this
was investigated on does not have.

## 8. Residual risks

- **A real ABI-specific defect stays open (C4).** If H1-narrowed is true, a memory-safety bug in the
  coroutine substrate is live on the project's primary platform. §7d item 1 is the cheapest way to
  close it and should be done before Milestone 8 leans harder on `rt::task<T>`.
- **A standing red job erodes signal.** Every day ASanUBSan is red for known reasons, the chance of a
  *new* finding being ignored rises. This is the direct cost of §7b and the reason §7d exists.
- **161/169 is a first execution, not a clean bill.** ASan and UBSan have now run once. Findings that
  need specific inputs, timing, or the tests currently failing for other reasons have not been
  reached.
