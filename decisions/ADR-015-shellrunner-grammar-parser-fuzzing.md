# ADR-015 — How does `MediatedShellRunner`'s grammar parser get continuous, corpus-driven fuzzing under ASan/UBSan, gated in CI, given 010 §9 G8 requires it before this parser can be called Proven against attacker-controlled input?

## 1. The question

`010-Python-Code-Interpreter.md` §9's promotion gate G8 names a claim ADR-001's own prove phase
explicitly stopped short of: "continuous corpus-driven fuzzing (libFuzzer-class) against the grammar
parser under ASan/UBSan, finds zero crashes/hangs/OOB reads over a declared minimum run; a
deliberately reintroduced known-bug class is caught by the harness." ADR-001 §8.5-8.6 ran a real
*fixed adversarial corpus* under clang ASan+UBSan (a 10 MB crafted-input suite: deep nesting, a
100k-stage pipeline, deeply nested `${...}`, unterminated quotes) against the untouched spike's own
`shell_parser.cpp` — genuine evidence, but a fixed corpus checked once, not the mutation-driven,
continuously-widening exploration G8's own wording asks for, and it was never run at all against
`mediated_shell_parser.cpp` (the genuinely new E3 parser, decision 4's clean-room reimplementation).

The question this ADR answers, stated so it has a wrong answer: **what does "continuous, corpus-
driven fuzzing... gated in CI" concretely mean for this repository's actual toolchain and CI budget,
and does a real libFuzzer harness against `mediated_shell_parser.cpp`'s `parse()` — proven with a
genuine, deliberately reintroduced crash — satisfy G8**, or does this Windows-first, MSVC-primary
codebase lack the toolchain to build one at all?

## 2. Background this design must respect

- **`parse()`'s own shape is already fuzz-shaped by construction** (`mediated_shell_parser.hpp`'s
  header comment, E3): a pure `bytes -> result<ScriptNode>` function with no dependency on
  `FileSystemAdapter`, `CommandRegistry`, `ExecState`, or `EffectContext` reachable from inside it —
  no fakes needed, no global mutable state to reset between fuzz iterations (the arena is fully owned
  by the returned `ParsedScript` and destructs at the end of each call). This is the same property
  ADR-001 §3's own harness note anticipated ("a libFuzzer harness is a five-line function").
- **The safety knobs under test are named, not incidental**: `kMaxSourceBytes` (1 MiB),
  `kMaxTokens` (50,000), `kMaxNestingDepth` (32, shared across `if`/`for` via `DepthGuard`),
  `kBytesPerNodeUpperBound`/`kArenaBytes` — `mediated_shell_grammar.hpp`'s own constants, carried
  forward from ADR-001 findings 12/13 as a design, reimplemented fresh in E3 (decision 4). A fuzzing
  harness's whole point here is proving these bounds actually hold under attacker-chosen bytes, not
  merely under the fixed probes E3's own smoke test already checked.
- **This repo already has real, working ASan/UBSan-under-clang precedent** — ADR-001 §8.5's own
  invocation (`-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1`, `ASAN_OPTIONS=
  detect_leaks=0:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`) and
  `.github/workflows/ci.yml`'s existing `windows-clang-cl` matrix job (`ASanUBSan` leg, `choco
  install llvm`). What does not already exist anywhere in this repo is a libFuzzer *target* (`-
  fsanitize=fuzzer`) — an actual continuous, mutation-driven harness, as opposed to a fixed-corpus
  batch run.
- **021 §5's toolchain floor**: MSVC has no UBSan-equivalent and no libFuzzer at all (documented
  gap this repo already carries across ADR-001/005/006/007/009 — "UBSan not attempted, no clang
  toolchain"). A real libFuzzer harness is therefore necessarily Clang-only, matching the existing
  `windows-clang-cl` CI job's own toolchain choice, not a new dependency class for this project.
- **decision 5** (this milestone's breakdown doc): G8 is its own ADR-track item, going through
  design→red-team→prove→judge and producing an ADR before landing — unlike E2/E3/E4's "ordinary
  task" treatment — because a fuzzing harness's own correctness (does it actually instrument the
  right code, does it actually crash on a real bug) is exactly the kind of claim that reads
  plausible and turns out wrong without executed proof.
- **CLAUDE.md's Machine Safety section**: no `hardware_concurrency()` threads, builds capped at
  `-j4`. libFuzzer defaults to a single fuzzing process with no `-jobs`/`-workers` flag passed —
  this ADR never passes either, so every run below is single-process by construction, not by
  discipline alone.

## 3. The competing designs

### Design A (accepted) — a real libFuzzer target against `mediated_shell_parser::parse()`, gated behind a Clang-only CMake option, run for a declared minimum duration each CI push

A new `tests/fuzz/mediated_shell_parser_fuzz.cpp` (`extern "C" int LLVMFuzzerTestOneInput(...)` calling
`parse()` directly on the raw fuzzer-supplied bytes) plus a checked-in seed corpus
(`tests/fuzz/corpus/mediated_shell_parser/`: six hand-picked scripts covering pipelines, `if`/`for`,
quoting/variable expansion, redirects, and a known-malformed/edge-case probe). A new CMake option,
`AGENTENGINE_BUILD_SHELL_FUZZER` (default OFF, matching `AGENTENGINE_WITH_WASM`/`AGENTENGINE_BUILD_
PYTHON_RUNNER`'s own opt-in posture), fails configure with a clear message unless the compiler is
Clang; when on, builds one executable (`agentengine_mediated_shell_parser_fuzz`) linking
`agentengine::mediated_shell_runner`, with `-fsanitize=fuzzer` added only to that target (never the
whole build, since libFuzzer's own `main()` must never coexist with an ordinary test binary's). The
rest of the tree (the library under test) is instrumented via an ordinary `-fsanitize=address,
undefined` in `CMAKE_CXX_FLAGS`, matching ADR-001's own invocation. A new CI job,
`windows-shell-fuzz` (`.github/workflows/ci.yml`), builds this one target and runs it for a declared
120 seconds against the checked-in seed corpus on every push/PR, failing the job on any nonzero exit
(crash, ASan/UBSan finding, or libFuzzer-detected hang).

**Steelman.** This is the only design that is actually "continuous, corpus-driven fuzzing" in G8's
own sense — mutation-based exploration that widens its own coverage over the run, not a fixed
probe list re-checked byte-identically every time (ADR-001's own §8.5 corpus, valuable as it is, is
that latter shape). Reuses every piece of toolchain this repo already has proven working (Clang,
ASan, UBSan, the existing CI matrix's LLVM install step) — no new dependency, no new compiler, no
new CI infrastructure class. `parse()`'s pure-function shape means the harness is genuinely five
lines, not a fragile scaffold requiring fakes.

### Design B (rejected) — extend ADR-001's fixed adversarial corpus instead of building a real fuzzer

Add more hand-picked adversarial cases to `test_shell_parser_adversarial.cpp`-style fixed-corpus
tests, run under ASan+UBSan in CI (already possible today, zero new CMake plumbing).

**Steelman.** Zero new toolchain surface, zero new CI job, zero libFuzzer-on-Windows CRT/ABI
integration risk (§6 below shows this risk was real, not hypothetical). Every case is deterministic
and human-reviewable — a fixed corpus never produces a flaky, hard-to-reproduce finding the way
mutation-based fuzzing occasionally can.

**Rejected because:** this is not what G8 asks for, read plainly — "continuous corpus-driven
fuzzing" and "a deliberately reintroduced known-bug class is caught by the harness" describe a
mutation-based harness with a positive control proving it has real teeth, not an enumerable,
human-authored list. A fixed corpus only ever finds what its author already thought to write down;
it structurally cannot find the shape of bug a fuzzer's own mutators (byte flips, cross-over,
dictionary-driven token insertion) are specifically good at surfacing. Design B would be relabeling
work this repo already did (ADR-001 §8.5) as satisfying a gate whose own text asks for something
categorically different.

### Design C (rejected) — AFL++ instead of libFuzzer

AFL++'s persistent-mode fuzzing, typically faster per-exec than libFuzzer on Linux, with a mature
crash-triage toolchain.

**Steelman.** Generally regarded as the stronger fuzzer for CPU-bound targets on Linux; this
project's own roadmap already treats Linux as "Next" (021 §2), so investing in AFL++ now could pay
off once Linux parity work begins.

**Rejected because:** AFL++'s Windows support is materially weaker than libFuzzer's (WinAFL exists
but is a separate, less-maintained fork with its own DynamoRIO dependency) — this repo's Supported
platform today is Windows only (021 §2), and G8's gate must be satisfiable on the platform this
project actually ships on now, not the one it plans to reach later. libFuzzer, being part of the
same LLVM/compiler-rt distribution this repo already vendors for ASan/UBSan, has zero additional
dependency surface; AFL++ would be a second, unrelated fuzzing toolchain to vet, pin, and maintain.

## 4. Falsifiable claims (Design A)

- **C1 (harness builds and runs).** `agentengine_mediated_shell_parser_fuzz.exe` builds cleanly
  under Clang with `-fsanitize=fuzzer,address,undefined` and executes against the seed corpus without
  crashing on any of the seed files themselves. *Disproof: build failure, or a seed file (none of
  which is intended to be a crasher) crashes the harness.*
- **C2 (declared minimum run, clean).** A 120-second corpus-driven fuzzing run against the current,
  correct `mediated_shell_parser.cpp` finds zero crashes, zero hangs, zero ASan/UBSan findings.
  *Disproof: any nonzero exit, any sanitizer report, any run that does not complete within its own
  declared time budget (a hang).*
- **C3 (deliberately reintroduced known-bug class is caught).** With ADR-001 finding 12's
  nesting-depth guard (`kMaxNestingDepth` check in `parse_if`) temporarily disabled, the harness
  finds a real crash — proving the harness actually instruments and exercises the code path this
  guard protects, not merely that it runs without error regardless of what the target code does.
  *Disproof: the bugged build runs clean for the same declared budget — the harness would then be
  vacuous, passing regardless of the target's own correctness.*
- **C4 (the fix, once restored, is clean again — no regression from the experiment itself).** After
  restoring the guard, the harness returns to C2's clean result, and the specific deep-nesting input
  that crashed the bugged build is now cleanly rejected (`shell.nesting_too_deep`), not merely
  "doesn't crash by accident." *Disproof: the restored build still crashes, or takes materially
  longer / behaves differently on the same input than before the experiment.*
- **C5 (default build posture unaffected).** With `AGENTENGINE_BUILD_SHELL_FUZZER` off (the
  default), the whole project configures and builds exactly as before — no new dependency, no new
  compiler requirement, for any build that does not opt in. *Disproof: a default-configuration
  build fails, or gains a new mandatory dependency.*
- **C6 (CI job is real, not a stub).** The new `windows-shell-fuzz` CI job actually builds the
  target and actually runs the fuzzer for the declared duration against the checked-in corpus, and
  would fail the job on a crash (verified by the same nonzero-exit mechanism C3's local reproduction
  used, not merely asserted from reading the YAML). *Disproof: the job step's exit-code check is
  wrong, or the job silently skips the actual fuzz run.*

## 5. The red-team attack

- **R-C1 (Windows libFuzzer ABI is not a solved problem — it broke twice before it worked).** The
  first build attempt failed at link time: `lld-link: error: /failifmismatch: mismatch detected for
  'RuntimeLibrary': clang_rt.fuzzer-x86_64.lib(...) has value MT_StaticRelease` against the fuzz
  target's own `MD_DynamicRelease` — LLVM's shipped `clang_rt.fuzzer-x86_64.lib` on this machine is
  built against the **static** CRT, while this project's default Windows build links dynamically.
  Fixed by setting `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` for this dedicated build tree (relying
  on CMP0091 defaulting NEW under this repo's `cmake_minimum_required(VERSION 3.28)`, not by editing
  the shared root build's own CRT posture). The SECOND attempt then failed differently:
  `/failifmismatch: mismatch detected for 'annotate_string': clang_rt.fuzzer-x86_64.lib(...) has
  value 0` against the harness TU's own `value 1` — MSVC STL's ASan container-annotation feature
  (`std::string`/`std::vector` bounds instrumentation), which the vendored libFuzzer runtime was
  built without. Fixed by adding `-D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION` to
  `CMAKE_CXX_FLAGS`. Neither of these is a design flaw in Design A's own architecture — both are
  genuine, undocumented-until-hit ABI mismatches specific to combining MSVC STL + Clang's ASan +
  libFuzzer on Windows, exactly the kind of thing a red-team pass exists to surface rather than
  assume away. **Real risk this leaves**: a future LLVM/toolchain upgrade could reintroduce either
  mismatch in a different shape; C1/C6 (the CI job actually building and running) is the ongoing
  check that would catch a regression here, not a one-time proof.
- **R-C2 (does "declared minimum run" actually mean anything, or is 120s an arbitrary number no
  one will ever revisit)?** Measured, not assumed: the real run in §6 executed 22,732-23,517 units
  in 121 seconds (~187-194 exec/s) against this specific small grammar — coverage (`cov: 14 ft: 26`)
  plateaus within the first ~2,000 executions and stays flat for the remainder, meaning 120s already
  runs the corpus-driven exploration well past its own point of diminishing returns for this
  grammar's size. 120s was chosen as a CI-budget-reasonable number (comparable to the existing
  `test_native_jail_*` suite's own multi-second-per-test budget), not derived from a formal coverage
  target — named as a real, if minor, arbitrary-constant residual (§9), not hidden.
- **R-C3 (is the "known-bug class" reintroduction actually representative, or a strawman a fuzzer
  would trivially find regardless of harness quality)?** The nesting-depth guard is ADR-001 finding
  12's own real, previously-identified bug class (unbounded recursion depth in a recursive-descent
  parser → native stack overflow), not an invented one — the same class this project's own E3 work
  already carried forward as a named safety knob. It was deliberately chosen over reintroducing E3's
  OWN two real historical bugs (the `CreateFileW`-can't-create-directories bug, the parser's
  keyword-swallowing bug) because **both of those are silent wrong-output bugs, not crashes** — a
  fuzzer with no semantic oracle (this harness has none; it only detects crashes/hangs/sanitizer
  findings) structurally cannot "catch" a bug that produces a plausible-looking wrong answer instead
  of a fault. This is a real, named boundary of what this harness proves (§9), not glossed over.
- **R-C4 (would the fuzzer have found the reintroduced bug on its own, via mutation, or did the
  seed corpus do all the work)?** Checked directly rather than assumed: the deep-nesting regression
  seed (`seed_deep_nesting_regression.txt`, 5,000 repetitions of `"if echo x then "`, 75,000 bytes)
  was added to the corpus specifically so the crash is found during libFuzzer's initial "INITED"
  pass over the seed corpus itself (confirmed in §6's log: `32 files found` immediately followed by
  `==5948== ERROR: libFuzzer: deadly signal`, before any mutation round completed) — not left to
  chance mutation, which coverage-guided fuzzing has no strong incentive to construct on its own
  (each additional nesting level re-executes the same code path already covered, so pure
  coverage-feedback does not specifically reward depth). This is a deliberate, honestly-stated
  choice: the corpus does the discovery work here, and the harness's claim is narrower than "would
  find this bug from an empty corpus" — it is "given a corpus containing the shape of input this bug
  class needs, the harness correctly detects and reports the resulting crash," which is what C3
  actually tests.
- **R-C5 (does disabling the guard and rebuilding risk leaving the tree in a broken state)?** The
  edit was applied via `Edit`, rebuilt, exercised, then reverted via the identical `Edit` call
  restoring the exact original text — confirmed via `git diff --stat` on the file showing zero
  changes before staging/committing anything from this ADR's work, so the reintroduced bug never
  reached a commit.

## 6. Executed evidence

**Toolchain.** `C:\Program Files\LLVM\bin\clang++.exe`, `clang version 22.1.5`, target
`x86_64-pc-windows-msvc`; `clang_rt.fuzzer-x86_64.lib`/`clang_rt.fuzzer_no_main-x86_64.lib` and the
`fuzzer` C++ headers confirmed present under `lib\clang\22\`. A dedicated tree, `build-fuzz`
(Ninja, `RelWithDebInfo`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, `CMAKE_CXX_FLAGS=
"-fsanitize=address,undefined -fno-sanitize-recover=undefined -D_DISABLE_STRING_ANNOTATION
-D_DISABLE_VECTOR_ANNOTATION -g -O1"`, `AGENTENGINE_BUILD_SHELL_FUZZER=ON`,
`AGENTENGINE_BUILD_TESTS=OFF`), separate from every other build tree this project uses, so this
work never touched the default/build-py/build-linux trees' own configuration.

**C1.** Build succeeded (`ninja` 20/20 targets, after the two ABI fixes in §5 R-C1). The harness ran
clean against all six original seed files with zero crashes (`Done 1932 runs in 11 second(s)` on a
10-second smoke pass, `stat::new_units_added: 38`, zero sanitizer output).

**C2 (declared minimum run, current/correct parser).**
```
$env:ASAN_OPTIONS = "detect_leaks=0:halt_on_error=1"
$env:UBSAN_OPTIONS = "halt_on_error=1:print_stacktrace=1"
.\agentengine_mediated_shell_parser_fuzz.exe -max_total_time=120 -print_final_stats=1 corpus_out
...
Done 22732 runs in 121 second(s)
stat::number_of_executed_units: 22732
stat::average_exec_per_sec:     187
stat::new_units_added:          38
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              327
```
Exit code 0. Zero crashes, zero sanitizer findings, zero hangs (`slowest_unit_time_sec: 0`).

**C3 (reintroduced bug, caught).** `mediated_shell_parser.cpp`'s `parse_if` had its
`kMaxNestingDepth` guard commented out (§5 R-C5's exact edit); the fuzz target alone was rebuilt
(`cmake --build build-fuzz --target agentengine_mediated_shell_parser_fuzz`, clean build, no other
target touched). Single-input replay of the deep-nesting regression seed:
```
Running: seed_deep_nesting_regression.txt
```
Exit code `-1073741819` (`0xC0000005`, `STATUS_ACCESS_VIOLATION` — a real native stack overflow, the
expected shape for unbounded recursive-descent recursion with the depth guard removed). Real
corpus-driven fuzzing mode (not single-input replay) against the same bugged build reproduced the
identical fault through libFuzzer's own crash-detection path, not merely the OS's:
```
INFO:       32 files found in corpus_out_bugged
INFO: seed corpus: files: 32 min: 1b max: 75000b total: 75930b rss: 82Mb
==5948== ERROR: libFuzzer: deadly signal
```
(Process terminated immediately after this line — a genuine stack-overflow crash leaves too little
stack for libFuzzer's own signal handler to print a full backtrace, a known real-world limitation
of deep-stack-overflow reporting specifically, not a harness malfunction; the crash itself, and
libFuzzer's own `ERROR: libFuzzer: deadly signal` classification of it, is the executed proof.)

**C4 (fix restored, clean again).** The guard was restored via the identical `Edit` reverting the
exact prior text; `git diff --stat` on the file confirmed zero net change before this ADR's own
commit. Fuzz target rebuilt clean. Single-input replay of the same deep-nesting seed: `Executed ...
in 11 ms`, exit 0, `NOTE: fuzzing was not performed, you have only executed the target code on a
fixed set of inputs` (libFuzzer's own standard single-input-mode message — confirms it ran the real
target, not a no-op). A final, full 120-second declared run with the regression seed now
permanently part of the checked-in corpus:
```
Done 23517 runs in 121 second(s)
stat::number_of_executed_units: 23517
stat::average_exec_per_sec:     194
stat::new_units_added:          94
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              343
```
Exit code 0. Zero crashes, zero sanitizer findings.

**C5.** Default-configuration builds (`build`, `build-py`) were never reconfigured or rebuilt during
this ADR's work — `build-fuzz` is a wholly separate CMake tree, and `AGENTENGINE_BUILD_SHELL_FUZZER`
defaults OFF, so the new CMake block in the root `CMakeLists.txt` is inert for every build that does
not pass it explicitly (verified by direct reading of the new `if(AGENTENGINE_BUILD_SHELL_FUZZER)`
guard, matching every other opt-in heavy-dependency block in this file).

**C6.** The new `.github/workflows/ci.yml` `windows-shell-fuzz` job was written to reproduce the
exact same configure/build/run sequence executed locally above (same `CMAKE_CXX_FLAGS`, same
`CMAKE_MSVC_RUNTIME_LIBRARY`, same 120-second budget, same `ASAN_OPTIONS`/`UBSAN_OPTIONS`), with an
explicit `if ($LASTEXITCODE -ne 0) { throw ... }` step so a libFuzzer crash fails the job — the same
nonzero-exit mechanism C3's local reproduction produced, not a separately-invented check. The job's
own YAML was reviewed against the working local command sequence line by line rather than written
from memory of what "should" work; it was not executed inside an actual GitHub Actions runner as
part of this ADR (that would require pushing to a remote this local repository does not have
configured) — named as a residual (§9), not silently claimed as run.

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — harness builds and runs clean against seeds | **CORRECT** | §6, two real ABI mismatches found and fixed (§5 R-C1), 10s smoke pass clean |
| C2 — declared 120s run, clean on the correct parser | **CORRECT** | §6, 22,732 executions, 0 crashes/findings/hangs |
| C3 — reintroduced known-bug class is caught | **CORRECT** | §6, real `STATUS_ACCESS_VIOLATION` crash both via direct replay and via libFuzzer's own corpus-driven crash detection |
| C4 — fix restored, clean again, no regression from the experiment | **CORRECT** | §6, `git diff` confirms exact revert; final 120s run 23,517 executions, 0 findings |
| C5 — default build posture unaffected | **CORRECT** | Direct source inspection: `AGENTENGINE_BUILD_SHELL_FUZZER` defaults OFF, guards a wholly separate CMake block; no default tree was touched |
| C6 — CI job is real, not a stub | **CORRECT** (configuration only) | Line-by-line matches the working local sequence and fails on the same nonzero-exit signal; **not executed on an actual CI runner this pass** (named residual, §9) |

## 8. The decision

**Accepted: Design A.** `MediatedShellRunner`'s grammar parser gets a real libFuzzer harness
(`tests/fuzz/mediated_shell_parser_fuzz.cpp`) against the pure `parse()` entry point, built via a
new Clang-only, default-off CMake option (`AGENTENGINE_BUILD_SHELL_FUZZER`), seeded from a checked-in
corpus (`tests/fuzz/corpus/mediated_shell_parser/`, six representative scripts plus the deep-nesting
regression case this ADR's own positive control produced), and run for a declared 120-second minimum
in a new `windows-shell-fuzz` CI job on every push/PR. The harness's teeth were proven directly, not
assumed: a real, previously-identified bug class (ADR-001 finding 12's nesting-depth guard) was
deliberately reintroduced, produced a real native-stack-overflow crash the harness correctly
detected and reported, and the fix was restored and reconfirmed clean. AFL++ (Design C) and
extending the existing fixed-corpus approach instead (Design B) were both considered and rejected —
the former for Windows-support weakness on this project's only currently-Supported platform, the
latter for not being what G8's own text asks for.

## 9. Residual risks and deferred gates

- **The CI job was written and reviewed, not executed on an actual GitHub Actions runner this
  pass** (§6 C6) — this repository has no configured remote to push to and trigger a real run
  against. The job's steps reproduce the exact locally-executed sequence line for line, but a real
  CI-environment difference (a different `choco install llvm` version pulling a different
  `clang_rt.fuzzer` build, a runner-image PATH quirk) could still surface something this pass did
  not. Named here as the concrete next check, not silently assumed clean.
- **This harness has no semantic oracle** (§5 R-C3) — it can only ever catch crashes, hangs, and
  sanitizer-detectable undefined behavior, never a bug that produces plausible-but-wrong output (the
  exact shape of both of E3's own real historical bugs — the `CreateFileW`/mkdir bug and the
  parser's keyword-swallowing bug). G8's own text ("finds zero crashes/hangs/OOB reads") already
  scopes the gate to this class specifically, so this is not a shortfall against the gate as written
  — but it is a real limit on what "the fuzzer is clean" is allowed to be read as proving.
- **No corpus persistence across CI runs.** Each `windows-shell-fuzz` run starts from the same
  checked-in seed corpus and discards whatever new coverage-widening inputs it discovers during its
  120 seconds (libFuzzer's own `-print_final_stats=1` shows `new_units_added: 38-94` per run in this
  pass's local measurements) — a genuinely continuous OSS-Fuzz-style setup would persist and grow
  the corpus across runs (as a CI cache/artifact), compounding coverage over calendar time instead of
  re-discovering the same ~25 features from scratch every push. Not built this pass; a real,
  deliberately scoped-out piece of infrastructure, not an oversight.
- **120 seconds is a CI-budget-reasonable constant, not a formally derived coverage target** (§5
  R-C2) — coverage on this small grammar plateaus well before that budget is spent, which is
  reassuring for THIS grammar's current size but is not a general argument that 120s remains
  sufficient if the grammar grows materially (e.g. Phase F/G work adding new builtins or syntax).
- **Linux is out of scope** (matching `agentengine_mediated_shell_runner`'s own Windows-only build
  gate, `CMakeLists.txt`) — this ADR's harness, corpus, and CI job are all Windows-specific; a Linux
  libFuzzer leg is a real, separate follow-on once `MediatedShellRunner` itself gains Linux parity
  (021 §2's own "Next" framing for that platform generally).
- **Only `parse()` is fuzzed, not the full `dispatch`/`evaluate` execution path.** This matches G8's
  own scope ("the grammar parser") precisely — `dispatch_command`/`evaluate_pipeline` depend on
  `FileSystemAdapter`/`CommandRegistry`/`EffectContext`, which a fuzz harness would need to fake
  (introducing exactly the "no fakes" property this ADR's §2 background named as `parse()`'s own
  advantage) — named as intentionally out of this ADR's scope, not a gap in it.
