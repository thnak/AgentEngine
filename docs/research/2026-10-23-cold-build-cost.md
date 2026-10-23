# Cold-cache build cost of AgentEngine (measured 2026-10-23)

**Question.** What does a build cost when no compiler-cache entry hits, so that CI and local-build decisions rest on
numbers? The trigger was GitHub Actions showing a 33% sccache hit rate and Windows jobs of 25-52 minutes after a
header change.

**Status.** One measured run per configuration on one machine, plus the CI timings we observed. The 2-core GitHub
projection is an estimate, not a measurement: no GitHub runner was benchmarked. Treat the numbers as an order of
magnitude and re-measure before a decision that depends on the second digit.

## What was measured

| Item | Value |
|---|---|
| Commit | `7b2f751fe5c11767bb740ae86bbb22aa18bfcff4` (`origin/main` at the merge of PR #221) |
| Machine | `thnak-remote`: Xeon E5-2680 v4 @ 2.4 GHz (Broadwell, 2016), 14 physical cores / 28 threads, 62 GB RAM, idle (load about 0) |
| Targets | everything: library, tests, examples, tools (971 ninja steps, 550 of them compiles). No test was run |
| Cold guarantee | no compiler launcher in the CMake cache, `CCACHE_DISABLE=1`, and `ninja -t commands` showed 0 lines containing `ccache` |
| Flags | `-G Ninja -DCMAKE_BUILD_TYPE=Release -DAGENTENGINE_WITH_HTTPS=ON -DCMAKE_LINKER_TYPE=MOLD`, gcc and clang |

## Results

| Run | Configure | Build wall | User CPU | Sys CPU | Average parallelism | Largest single process RSS | Lowest MemAvailable |
|---|---|---|---|---|---|---|---|
| gcc, `-j28` | 92.8 s | 6 min 29 s | 8,690 s | 262 s | 23.0x | 1.7 GB | 45.0 GB of 62.7 GB |
| clang, `-j28` | 206 s | 6 min 20 s | 8,834 s | 182 s | 23.7x | 0.7 GB | 56.1 GB of 62.7 GB |
| gcc, `-j8` | 69.7 s | 11 min 59 s | 5,134 s | 167 s | 7.4x | n/a | 54.9 GB of 62.7 GB |

All three exited 0 with no failed steps. Memory was never close to a limit.

From `.ninja_log` of the gcc `-j28` run:

- 550 compile steps took 8,974 s in total; the other 421 steps (links, the mbedTLS archives and similar) took 32 s, and
  the slowest of them was 0.1 s (mold). Compilation is 99.6% of the CPU cost.
- The median compile step is 13.2 s. The critical path is about 110 s, so more cores keep helping up to roughly 25x
  parallelism.
- The 20 slowest compiles are 50-110 s each and are almost all tests, tools and examples, not the library:
  `tools/agentengine_test_driver.cpp` (110 s), `tools/agentengine_scenario_runner.cpp` (99 s),
  `tests/eval/test_eval_tier1_screen_live_e2e.cpp` (94 s), `tests/testing/test_agentengine_test_driver.cpp` (89 s),
  `tests/core/context/test_session_builder.cpp` (71 s), `tests/core/agent/test_harness_session.cpp` (70 s). The only
  `src/` file in the top 20 is `src/rt/agent_session_core.cpp` (52 s). Clang's top five are the same files at 68-99 s.

## Reading the numbers

- **Hyperthreading inflates CPU time.** The 28 threads are 14 physical cores, so each step costs about 1.7x more CPU
  time at `-j28` than at `-j8` (8,690 s against 5,134 s of user CPU). The least-contended cost of the whole build is
  therefore about 5,300 s of one core, not about 9,000 s.
- **gcc and clang cost the same here** (within 2% in wall time and CPU). Clang's slower configure (206 s against 93 s)
  was not investigated.

## Projection for a 2-core GitHub runner (estimate)

Assumption: perfect scaling over 2 vCPUs, ninja always busy, no cache hits.

- From 5,300 s of single-core CPU: about 2,650 s, or 44 minutes, plus about 1.5 minutes to configure, so roughly
  46 minutes for the build alone.
- From the inflated 9,000 s: about 75 minutes. That counts SMT contention that a 2-vCPU runner also suffers, so it is
  the pessimistic end rather than a bound.
- A hosted runner's 2 vCPUs are likely two hyperthreads of one core, so parallel efficiency is worse than the
  assumption. The expected range is **45 to 75 minutes, midpoint about 55-60**.

This agrees with what CI showed on cold-cache runs of PR #222 (2026-10-23): Windows jobs of 25 to 52 minutes
(clang-cl Release 25 min, MSVC ASan 33 min, MSVC Release 49 min, clang-cl ASan and UBSan 52 min) against 18 minutes
for the Linux gcc job that also builds and runs the tests. The sccache hit rate was 33% on that PR by the owner's reading of the run, and 23% to 39% across the jobs of the earlier PR #220 run.

## What it implies

- The cost is template-heavy compilation spread over 550 files, not linking or any single file, so a faster linker or
  more cores will not change the shape: more cores help only until about 25x parallelism.
- The most expensive files are tests, tools and examples. A CI job that builds only the targets a change affects, or
  one that skips the live-network test and tool targets, would cut the cold cost without touching the library. This
  note does not size that saving, because the build of a target subset was not measured.
- Any header near the root of the include graph (for example `core/bound.hpp`, `trust/capability.hpp`) invalidates most
  of the cache, which is why a header-split PR is slow once and then fast. The project already treats `core/bound.hpp`
  as frozen for this reason.

## Not measured

Warm-cache and partial rebuilds, a build of a target subset, other optimisation levels, the test run, and any real
GitHub runner. During the run the server was briefly unreachable over SSH for a few minutes; the build was unaffected,
but the memory watchdog the measurement planned was never installed (memory did not come close to a limit).

## Reproduce

On the build server, from a source tree exported with `git archive <commit>` into an empty directory:

```
export CCACHE_DISABLE=1
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
      -DAGENTENGINE_WITH_HTTPS=ON -DCMAKE_LINKER_TYPE=MOLD
/usr/bin/time -v ninja -C build -j28 -k 0
```

For clang use `clang` and `clang++`. Per-step times come from `build/.ninja_log`: duration is (end - start) in
milliseconds, and compile steps are the entries whose output ends in `.o`. Memory was sampled from `/proc/meminfo`
(`MemAvailable`) every 10 s.
