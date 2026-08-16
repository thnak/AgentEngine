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

## Upstream: known since 2016, and fixed in October 2025

Checked 2026-08-16. **This is a long-standing, documented AddressSanitizer limitation on Windows,
not a novel finding, and it has an upstream fix.** Nothing should be filed.

- **google/sanitizers#749**, opened 2016-12-04, now closed: "Asan doesn't work with exceptions on
  Windows". The reporter's case is this reproducer minus the `exception_ptr` hop --
  `try { throw std::exception("test"); } catch (const std::exception& ex) { puts(ex.what()); }` --
  producing "ERROR: AddressSanitizer: access-violation on unknown address". Same diagnostic, same
  place, ten years earlier.

- **llvm/llvm-project#159618**, merged into `main` 2025-10-17, explicitly a mitigation for #749.
  It states the mechanism directly: **"ASan's instrumentation is incompatible with Window's
  assumptions for instantiating catch-block's parameters"**, and the fix is to stop instrumenting
  catch-block parameters on Windows -- "strictly better than today's status quo, where the runtime
  generates false positives".

**This identifies the mechanism**, which the measurements above deliberately left open. The corrupt
pointer is the **catch-block parameter itself** (`e` in `catch (std::runtime_error const& e)`), not
the exception object or the `exception_ptr`. That explains every observation at once: why UBSan
alone is clean (no ASan instrumentation of the catch parameter, so nothing to be inconsistent with),
why the value reads as unrelated bytes (an uninstantiated parameter slot), and why the static/DLL CRT
choice is irrelevant (the incompatibility is in instrumentation, not in the CRT).

It also settles the "real defect vs artifact" question in favour of a **false positive**: the
program is correct; ASan's instrumentation of the catch parameter is what breaks it.

### Which versions have the fix

| | |
|---|---|
| Fix merged to LLVM `main` | 2025-10-17 |
| LLVM 21.1.0 released | ~2025-09 — **before** the merge; backport to 21.1.x not verified |
| LLVM 22.1.0 released | 2026-02-24 — **first stable release containing the fix** |
| **What our CI actually ran** | **clang 20.1.8** (from the job's own `clang++ --version`) |

clang 20.1.8 on a 2026 runner is not what `choco install llvm` would fetch fresh; the
`windows-latest` image ships LLVM preinstalled and `choco install llvm -y` is a no-op when the
package is already present. So the job has been silently pinned to the image's version all along --
which is also why `ci.yml` not pinning clang (noted above) mattered more than it looked.

## Open

1. **Verify that clang 22.1+ actually clears it**, by pinning the CI install and re-running the
   matrix in this document. Expected clean; not yet measured, and this file does not claim it is.
2. Whether the fix was backported to any 21.1.x point release -- unverified, and only worth knowing
   if pinning to 22.1+ turns out to be awkward.
