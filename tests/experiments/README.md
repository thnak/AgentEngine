# tests/experiments — standalone toolchain reproducers

Not part of the test suite. Nothing here is registered with CTest or built by
`cmake --build`, deliberately: these programs exist to characterise a **compiler or runtime**
defect, so they must be buildable with a hand-written command line whose every flag is visible,
not through the project's own build system.

A file lands here when an ADR needs to distinguish "our bug" from "the toolchain's bug", and the
distinguishing evidence is a program small enough that no AgentEngine code can be blamed for it.
It stays here after the question is settled, because an ADR that cites evidence nobody can re-run
is an assertion.

## `exception_ptr_upcast_repro.cpp`

Evidence for `docs/research/2026-08-16-clang-windows-asan-exception-ptr.md`.

Twelve lines of standard C++ — `throw` → `std::current_exception()` → `std::rethrow_exception()` →
`catch (std::runtime_error const&)` → `e.what()`. No coroutine, no AgentEngine header, nothing from
this project.

**Result (2026-08-16, clang 20 from `choco install llvm`, windows-latest runner):** it takes an
AddressSanitizer access violation on the `e.what()` line under Windows, and is clean under
UndefinedBehaviorSanitizer alone. That is what establishes the three `Windows / clang-cl /
ASanUBSan` CI findings as a toolchain defect rather than an AgentEngine one.

### Re-running it

Windows, in an MSVC developer shell, with clang on `PATH`:

```pwsh
$rt = $(clang++ -print-resource-dir)
$env:PATH = "$rt\lib\windows;$env:PATH"     # the sanitizer runtime DLLs
$env:ASAN_OPTIONS  = "detect_leaks=0:halt_on_error=1"
$env:UBSAN_OPTIONS = "halt_on_error=1:print_stacktrace=1"

# The interesting comparison. Note -fms-runtime-lib=, NOT MSVC's /MT --
# clang++ is the GNU-style driver and rejects /MT outright.
clang++ -std=c++23 -g -O1 -fsanitize=address -fms-runtime-lib=static `
  tests/experiments/exception_ptr_upcast_repro.cpp -o repro_asan.exe
clang++ -std=c++23 -g -O1 -fsanitize=undefined -fno-sanitize-recover=undefined -fms-runtime-lib=static `
  tests/experiments/exception_ptr_upcast_repro.cpp -o repro_ubsan.exe

.\repro_asan.exe    # expected: ASan access-violation, nonzero exit
.\repro_ubsan.exe   # expected: prints "what=boom" then "repro: clean", exit 0
```

Linux, as the control — both legs are expected to exit 0:

```sh
clang++ -std=c++23 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  tests/experiments/exception_ptr_upcast_repro.cpp -o repro && ./repro
```

Exit 0 with `repro: clean` means the platform is fine. A nonzero exit is the finding.

## `wait_timeout_vs_qpc.cpp`

Evidence for the wall-clock tolerance in `tests/test_job_object_limits.cpp`.

`test_job_object_limits` asserted that a `WaitForSingleObject(h, 500)` measured with `steady_clock`
reports at least 500 ms elapsed. It failed intermittently in CI on an **uninstrumented** MSVC
Release build (runs `31925631415`, `31939239439`) — so the clang/ASan finding does not cover it,
and the two candidate explanations were "`wait_or_kill()` kills children early" (a containment
defect) and "the assertion asserts something Win32 does not guarantee".

**Result (2026-08-16, MSVC 14.44, Windows 11, 60 waits on a 500 ms deadline):**

| system timer resolution | elapsed − deadline (ms) | under-shoots |
| --- | --- | --- |
| 15.625 ms (default) | +1.154 … +14.155 | 0 / 60 |
| 1 ms (`timeBeginPeriod(1)`) | **−0.007** … +0.849 | **3 / 60** |

The timeout expires against the kernel's tick clock, not QPC. At default granularity the rounding-up
slack hides the divergence; at 1 ms it does not, and ~5% of waits return early by QPC. Timer
resolution is machine-global and any process can raise it, so unrelated software on the runner
decided whether the assertion held — the "passes almost always" signature.

The platform, not the product. The test now allows one tick of slack.

### Re-running it

```pwsh
cl /nologo /O2 /EHsc /std:c++20 tests/experiments/wait_timeout_vs_qpc.cpp /Fe:wait_qpc.exe
.\wait_qpc.exe 60
```

A negative `min` on the second row is the finding. Zero under-shoots on both rows means this machine
never had its timer resolution raised during the run — re-run it with something like a browser or a
media player open, which is precisely the point.

## `appcontainer_profile_race.cpp`

Evidence for `CrossProcessLock` in `src/backends/native_jail/app_container_profile.cpp`.

`NativeJailBackend` uses one machine-global AppContainer profile name (`AgentEngine.NativeJail`), and
`ctest -j 4` runs six Windows native-jail test binaries concurrently — six separate processes calling
`CreateAppContainerProfile` on that one name. Intermittently (~1 run in 40–80) every `create()` in one
test process failed while a concurrent one failed differently:

```
abuse_corpus_windows : CreateAppContainerProfile failed: HRESULT 0x8007000A  (ERROR_BAD_ENVIRONMENT)
backend_windows      : CreateProcessW failed: Win32 error 2                  (ERROR_FILE_NOT_FOUND)
```

**Result (2026-08-16, MSVC 14.44, Windows 11):**

| configuration | unexpected failures |
| --- | --- |
| 1 process × 50 iterations, no lock | 0 |
| 8 processes × 300 iterations, no lock | **149–158 per process (~50%)**, worst `0x80070005` |
| 8 processes × 300 iterations, `lock` | **0**, all 2400 calls |

`CreateAppContainerProfile` is not safe to call concurrently on the same name, and the failures are
*not* `ERROR_ALREADY_EXISTS`, so they bypass the idempotency path the class was built around.
`DeriveAppContainerSidFromAppContainerName` measured 0 failures throughout and needs no protection.

The third row is the same binary under the same contention with only a named mutex added — the
control that makes "0 failures" mean something.

### Re-running it

```pwsh
cl /nologo /O2 /EHsc /std:c++20 tests/experiments/appcontainer_profile_race.cpp `
   /Fe:acrace.exe /link userenv.lib advapi32.lib

# One process proves nothing here — the race needs concurrent processes.
1..8 | ForEach-Object { Start-Process .\acrace.exe -ArgumentList "AgentEngine.RaceProbe",300 }
1..8 | ForEach-Object { Start-Process .\acrace.exe -ArgumentList "AgentEngine.RaceProbe","300","lock" }
```

Exit code is the count of unexpected failures. Nonzero without `lock` and zero with it is the finding.
