# A compiler cache for local builds (Windows / MSVC)

CI has used [sccache](https://github.com/mozilla/sccache) since 2026-09-04 (`.github/workflows/ci.yml`, top
comment). A local build gets nothing unless you set it up: there is no compiler launcher by default, so every
rebuild after a branch switch, a reverted experiment, or a change to a widely included header (for example
`include/agentengine/rt/agent_session.hpp`, which most tests pull in) recompiles from scratch.

**Measured 2026-09-21** on a Windows 11 dev box, MSVC 14.51, Ninja, Debug, tests and examples on
(917 targets, 520 compiles):

| | Wall time | sccache |
|---|---|---|
| Cold cache | 480 s | 520 misses, 0 non-cacheable |
| Warm cache, all outputs deleted | 84 s | **520 hits, 0 misses (100 %)** |

About 5.7x. The 84 s that remains is mostly linking, which a compiler cache cannot help with. It does
nothing for the first compile of code you have just written; it pays when the same translation unit,
with the same inputs, has been compiled before.

## Set up

1. Install sccache. CI uses 0.17.0, and so does this measurement:

   ```
   winget install --id Mozilla.sccache -e
   ```

2. Configure a **separate** build directory (`build-*/` is already gitignored), from a shell with the MSVC
   environment set up. Keep your existing `build/` as it is.

   ```
   cmake -S . -B build-cache -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
     -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache ^
     -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded ^
     -DAGENTENGINE_WITH_HTTPS=ON -DAGENTENGINE_WITH_NATIVE_PROCESS=ON ^
     -DAGENTENGINE_BUILD_TESTS=ON -DAGENTENGINE_BUILD_EXAMPLES=ON
   cmake --build build-cache -j4
   ```

   Add whichever other `-DAGENTENGINE_*` options your normal build uses. `-j4` is the project's limit
   (`CONVENTIONS.md`, "Build & test -- machine safety").

3. Check that it is really caching, not merely configured to:

   ```
   sccache --zero-stats
   cmake --build build-cache -j4
   sccache --show-stats
   ```

   You want a high "Cache hits" count on a second build and **"Non-cacheable compilations" at 0**. A launcher
   that is set but whose every compile is non-cacheable looks identical to a working one until you read the
   stats; that is exactly how a whole CI leg once ran with sccache installed and zero hits (issue #73,
   `CMakeLists.txt`).

## The one flag that matters: `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`

sccache cannot cache MSVC's `/Zi` output: a shared, incrementally written `.pdb` is not a pure function of
one translation unit's inputs. And putting `/Z7` in `CMAKE_CXX_FLAGS_DEBUG` is **not** enough. This project's
CMake policy CMP0141 is on, so CMake's default debug format is `ProgramDatabase` and it appends `-Zi`
*after* your flags:

```
cl : Command line warning D9025 : overriding '/Z7' with '/Zi'
fatal error C1041: cannot open program database '...mbedcrypto.pdb'; if multiple CL.EXE write to the same .PDB file, please use /FS
```

The second line is what a parallel build of the vendored mbedtls does with it. Setting
`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` is the CMake-native way to say "embed it", and it reaches the
dependencies too. You can confirm on a configured tree with no compile at all:

```
grep -c -- "-Zi" build-cache/build.ninja     # expect 0
grep -c -- "-Z7" build-cache/build.ninja     # expect a large number
```

Only the Windows MSVC toolchain has been measured. On Linux the same `*_COMPILER_LAUNCHER` variables apply
and nothing here about `.pdb` files does; CI's Linux leg is the reference for that.

## Housekeeping

- The cache is on disk outside the repository (`sccache --show-stats` prints "Cache location"), so deleting
  `build-cache/` does not lose it. The default size limit is generous; `sccache --show-stats` reports the
  current size.
- `sccache --stop-server` stops the background process. Changing compiler version or flags simply misses
  and repopulates; it never serves a stale object.
- A cache hides nothing: if a check depends on a fresh compile (a warning-as-error you just fixed), a hit
  still reports what the original compile produced for identical inputs, which is the point.
