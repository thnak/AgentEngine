# clang/Windows: AddressSanitizer faults on `what()` after an `std::exception_ptr` round-trip

**Date:** 2026-08-16
**Status of the claim:** measured directly on GitHub-hosted `windows-latest` runners, twice, with a
standalone reproducer. **Not yet checked against upstream LLVM issues** — see "Open" below.
**Consumed by:** `decisions/ADR-062-windows-sanitizer-coroutine-exception-findings.md` §9.

CLAUDE.md requires external claims to be dated and cited rather than asserted from memory. This is
an external claim — about a compiler and its runtime, not about AgentEngine — so it lives here, and
ADR-062 cites it rather than restating it.

## The claim

Under clang on Windows with `-fsanitize=address`, a program that

1. throws an exception,
2. captures it with `std::current_exception()`,
3. rethrows it with `std::rethrow_exception()`,
4. catches it by reference to a **derived** type, and
5. calls `std::exception::what()` on it

takes an access violation at step 5. The same program is clean under
`-fsanitize=undefined` alone, and clean under both sanitizers on Linux.

## Reproducer

`tests/experiments/exception_ptr_upcast_repro.cpp` (in this repository). 36 lines including comment
banner; the executable part is twelve. **No coroutine, no AgentEngine header, no project code.**

## Measurements

Environment: GitHub `windows-latest` (Windows Server 2025, image `20260729.566`), clang installed by
`choco install llvm` — **version not pinned by the workflow**, reported by the job as resource dir
`C:\Program Files\LLVM\lib\clang\20\lib\windows`. Linux control: clang 21.1.8, WSL Ubuntu.
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

### The reproducer (run `31922713398`)

| CRT | sanitizers | result |
|---|---|---|
| `-fms-runtime-lib=static` | undefined only | **exit 0** — prints `what=boom`, `repro: clean` |
| `-fms-runtime-lib=static` | address | **ASan access-violation**, `exception_ptr_upcast_repro.cpp:31 in main` |
| `-fms-runtime-lib=static` | address+undefined | ASan access-violation |
| `-fms-runtime-lib=dll` | address | exit `-1073740791` = `0xC0000409` `STATUS_STACK_BUFFER_OVERRUN` |
| `-fms-runtime-lib=dll` | address+undefined | exit `-1073740791` |
| `-fms-runtime-lib=dll` | undefined only | build failed (unrelated: see "Instrument errors") |

Line 31 is `std::printf("what=%s\n", e.what());`.

### Real code, same shape (run `31922513049`)

Three AgentEngine tests that route an exception through `std::exception_ptr` and then call `what()`
in a catch block, across the same axes:

| CRT | sanitizers | `test_rt_task` | `test_rt_thread_pool` | `test_middleware_model_call_gateway` |
|---|---|---|---|---|
| MultiThreaded | **undefined only** | **0** | **0** | **0** |
| MultiThreaded | address | 1 | 1 | 1 |
| MultiThreaded | address+undefined | 1 | 1 | 1 |
| MultiThreaded | address+undefined, `-fno-sanitize=alignment` | 1 | 1 | 1 |
| MultiThreadedDLL | address | 1 | 1 | 1 |
| MultiThreadedDLL | address+undefined | 1 | 1 | 1 |
| MultiThreadedDLL | address+undefined, `-fno-sanitize=alignment` | 1 | 1 | 1 |

Representative diagnostics from the address+undefined legs:

```
test_rt_task.cpp:114:20: runtime error: upcast of misaligned address 0x63005c00620069
    for type 'std::runtime_error', which requires 8 byte alignment
==7992==ERROR: AddressSanitizer: access-violation on unknown address 0xffffffffffffffff
SUMMARY: AddressSanitizer: access-violation test_rt_thread_pool.cpp:126 in main
```

`0x63005c00620069` and `0x5c00300032005c` (seen on an earlier run) decode as little-endian UTF-16
text — string bytes being read as an object pointer.

## What the measurements establish

- **ASan's presence is necessary and sufficient.** Every configuration containing
  `-fsanitize=address` faults; the configuration with only `-fsanitize=undefined` passes.
- **The static/DLL CRT choice is irrelevant.** Identical outcomes in every paired cell. A hypothesis
  that the MSVC STL's `exception_ptr` under `/MT` was responsible is **refuted**.
- **UBSan's alignment check is a symptom, not the cause.** Disabling it (`-fno-sanitize=alignment`)
  removes the `upcast of misaligned address` reports and changes nothing else — the ASan faults
  remain. And with UBSan alone the misaligned pointer is not observed at all, i.e. it does not exist
  in the program as compiled without ASan.
- **It is not AgentEngine-specific.** Twelve lines of standard C++ reproduce it.

## What they do NOT establish

- **The mechanism inside ASan is unidentified.** "Necessary and sufficient" is measured; *why* is
  not. A narrower trigger that this shape merely happens to hit is not excluded.
- **The clang version is not pinned**, so "clang 20" here means whatever `choco install llvm`
  resolved to on 2026-08-16. Pinning it in CI is a separate, and arguably overdue, decision.
- **The DLL-CRT + UBSan-only cell was never measured** (it fails to configure for an unrelated
  reason), so the clean UBSan-only result is confirmed on the static CRT only.

## Instrument errors, recorded rather than quietly re-run

- Round 1's six reproducer legs all failed to build with `clang++: error: no such file or directory:
  '/MT'`. `clang++` is the GNU-style driver and does not accept MSVC's `/MT`/`/MD`; the spelling is
  `-fms-runtime-lib=static|dll`. Round 2 fixed it.
- `MultiThreadedDLL` + UBSan-only fails at CMake configure time — `lld-link: error: /failifmismatch:
  mismatch detected for 'RuntimeLibrary'` inside a 007 §9 G2 `try_compile` control. An artifact of
  that flag combination, not a result about exceptions.

## Open

1. **Check whether this is a known LLVM issue before filing anything.** Not done. It is the next
   step, and ADR-062 §9.4 says so rather than implying an upstream report already exists.
2. If unknown upstream, file with `exception_ptr_upcast_repro.cpp` verbatim.
3. Re-measure when the toolchain moves; the unpinned clang makes that silent.
