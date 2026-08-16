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

Evidence for **ADR-062 §9.2** and `docs/research/2026-08-16-clang-windows-asan-exception-ptr.md`.

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
