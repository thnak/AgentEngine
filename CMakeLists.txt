# AgentEngine — root build. See CONVENTIONS.md for the binding coding contract (layout, dependency
# tiers, machine-safety build rules) and AgentEngineSpecification.md for the design this implements.

cmake_minimum_required(VERSION 3.28) # 021 §5
project(AgentEngine LANGUAGES CXX)

# ---- Standard ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AgentEngineWarnings)
include(AgentEngineCompilerGuard) # 021 §5 compiler-version floor

# ---- Windows toolchain wiring -------------------------------------------------
# Mirrors third_party/quark/CMakeLists.txt's pattern: NOMINMAX/WIN32_LEAN_AND_MEAN project-wide so
# any TU that pulls in <windows.h> (directly or transitively, e.g. via a future sandbox backend)
# gets the lean, min/max-free definition before it does.
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# C++ exceptions must be ON for every MSVC-like frontend, stated here rather than inherited.
# `src/backends/native_jail/shell_parser.cpp` really does `catch (std::bad_alloc const&)` -- that is
# how ADR-001's arena-exhaustion path reports `shell.arena_exhausted` instead of terminating -- so a
# build without exceptions is not a valid configuration of this project, it is a broken one.
#
# It was inherited before, and only by accident: the Visual Studio generator injects /EHsc into its
# own default flags, so the MSVC leg compiled. Ninja + clang-cl gets no such default and failed with
# "cannot use 'try' with exceptions disabled". `MSVC` is true for clang-cl as well (CMake sets it for
# any MSVC-like frontend), so one guard covers both, and stating it here makes the property
# generator-independent rather than a property of which generator CI happens to use.
if(MSVC)
  add_compile_options(/EHsc)
endif()

# ---- Core vocabulary (header-only for now; no .cpp under include/agentengine) --
add_library(agentengine_core INTERFACE)
target_include_directories(agentengine_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(agentengine_core INTERFACE cxx_std_23)
# ADR-037: the Quark actor-engine submodule (third_party/quark) is fully removed -- agentengine::pal
# (include/agentengine/pal/net.hpp) is a small, standalone, vendored replacement for the one slice of
# Quark's own PAL this project ever used (portable non-blocking sockets), so ws2_32 -- previously
# inherited transitively via quark::quark's own PUBLIC link on Windows -- is linked directly here now.
if(WIN32)
  target_link_libraries(agentengine_core INTERFACE ws2_32)
endif()
add_library(agentengine::core ALIAS agentengine_core)

# ---- Egress proxy (src/sandbox/) -- Windows + Linux ---------------------------------------------
# Implements decisions/ADR-011-first-party-egress-proxy.md (008 §10 Q3, M2 Phase F task F1): the
# host-mediated mechanism NetOut (007 §3) is enforced through. Ordinary OS socket/DNS APIs
# (getaddrinfo/inet_pton/select) plus agentengine::pal's own vendored, cross-platform socket
# primitives (include/agentengine/pal/net.hpp, pulled in transitively via agentengine::core) -- no
# third-party dependency at all now, so this needs no CMake option gate beyond the platform-
# independent build itself (ws2_32 is linked directly on agentengine::core on Windows, above).
add_library(agentengine_net_egress_proxy STATIC
  src/sandbox/net_egress_proxy.cpp)
target_link_libraries(agentengine_net_egress_proxy PUBLIC agentengine::core)
target_link_libraries(agentengine_net_egress_proxy PRIVATE agentengine_warnings)
add_library(agentengine::net_egress_proxy ALIAS agentengine_net_egress_proxy)

# ---- Policy-reachability CI tool (tools/) -- 007 §9 gate G6, M2 Phase F task F2 ------------------
# Enumerates {capability kind, tool, taint} against the real mechanical enforcement
# decisions/ADR-009-capability-set-enforcement-mechanism.md built (CapabilitySet::contains(),
# trust/capability.hpp) via include/agentengine/trust/policy_reachability.hpp's enumerator -- see
# that header's own top comment for the RFC-vs-code scope note (no declarative rule set exists yet,
# decision 4; taint is enumerated as the boolean the mechanism actually has, not 007 §5's
# aspirational graded levels). Header-only logic (agentengine::core is enough); no new dependency.
add_executable(agentengine_policy_reachability tools/policy_reachability.cpp)
target_link_libraries(agentengine_policy_reachability PRIVATE agentengine::core agentengine_warnings)

# ---- MCP conformance client (tools/) -- ADR-023 §10 Tier 1, 011 §10 G2 --------------------------
# The binary `conformance client --command` spawns. See tools/mcp_conformance_client.cpp's own top
# comment for why the CLIENT role needs no listener and no fixture, and
# docs/research/2026-08-15-mcp-conformance-harness.md for the empirically confirmed harness contract.
# Links the real egress proxy (ADR-011/ADR-016) rather than a test double -- the point of this target
# is that the conformance number it produces is attributable to engine code.
add_executable(agentengine_mcp_conformance_client tools/mcp_conformance_client.cpp)
target_link_libraries(agentengine_mcp_conformance_client
  PRIVATE agentengine::core agentengine::net_egress_proxy agentengine_warnings)

# ---- native_jail ShellRunner backend (src/backends/native_jail/) --------------
# Implements decisions/ADR-001-shellrunner-grammar-and-dispatch.md — Design A: the
# recursive-descent parser, the pmr-backed AST, the tree-walking evaluator, the fixed builtin
# table, CommandRegistry, and RealFileSystemAdapter. A STATIC library (not header-only) so Sh-S1's
# static "zero references to a process-creation primitive" check (ADR-001 §7 finding 1's fix) can
# be stated at link-target granularity: this library links ONLY against agentengine::core (header
# -only vocabulary, no Quark dependency since ADR-037) and the C++ standard library -- nothing here
# can pull in an object that defines CreateProcess/_wspawnv/system/LoadLibrary/posix_spawn/exec*/
# fork by construction of what it's linked against, which tests/test_shell_runner_proof.cpp
# verifies against the actual built artifact, not against prose.
add_library(agentengine_shell_runner STATIC
  src/backends/native_jail/shell_parser.cpp
  src/backends/native_jail/shell_dispatch.cpp
  src/backends/native_jail/real_filesystem_adapter.cpp)
target_include_directories(agentengine_shell_runner PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agentengine_shell_runner PUBLIC agentengine::core)
target_link_libraries(agentengine_shell_runner PRIVATE agentengine_warnings)
add_library(agentengine::shell_runner ALIAS agentengine_shell_runner)

# ---- Milestone 3 Phase E3's real ShellRunner (mediated_shell_*.{hpp,cpp}) -- Windows only this
# pass (matches ADR-001's own Windows-only scope) -- a genuinely new set of translation units per
# decision 4, sharing NO source with agentengine_shell_runner above (that target's sources stay in
# place, untouched, as ADR-001's cited prove-phase evidence). Built directly on
# core/worktree_mount_fs.hpp's open_within_mount_root (agentengine::worktree_store) -- that
# primitive's own header names itself as exactly what a future FileSystemAdapter is expected to
# call, so reusing it here is the documented intent, not a decision-4 violation (only
# real_filesystem_adapter.{hpp,cpp} is off-limits to reuse).
if(WIN32)
  add_library(agentengine_mediated_shell_runner STATIC
    src/backends/native_jail/mediated_shell_parser.cpp
    src/backends/native_jail/mediated_shell_dispatch.cpp
    src/backends/native_jail/mediated_filesystem_adapter.cpp)
  target_include_directories(agentengine_mediated_shell_runner PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_mediated_shell_runner PUBLIC agentengine::core agentengine::worktree_store)
  target_link_libraries(agentengine_mediated_shell_runner PRIVATE agentengine_warnings)
  add_library(agentengine::mediated_shell_runner ALIAS agentengine_mediated_shell_runner)
endif()

# Linux half of the same target (2026-08-28, ADR-103, the SandboxToolProvider Linux-parity pass) --
# `mediated_shell_parser.cpp`/`mediated_shell_dispatch.cpp` are unchanged, already-portable C++ (no
# Windows API dependency of their own, confirmed by direct reading before this pass); only
# `mediated_filesystem_adapter.cpp` (genuinely Win32 API-dependent -- CreateFileW/ReadFile/WriteFile/
# SetFileInformationByHandle/GetFileInformationByHandleEx/GetFinalPathNameByHandleW) needed a real
# Linux twin, `mediated_filesystem_adapter_posix.cpp`, built on `worktree_mount_fs_posix.hpp`'s
# already-Judged `open_within_mount_root` (ADR-014 Phase C4) exactly the way the Windows branch above
# is built on its own Phase C2 twin. Same target name, same public surface
# (`agentengine::mediated_shell_runner`, `MediatedFileSystemAdapter`/`SessionShellSandbox`/
# `SandboxToolProvider` all unchanged) on both platforms, mirroring
# `agentengine_worktree_store`'s own WIN32/`NOT WIN32` split just above in this file.
if(NOT WIN32)
  add_library(agentengine_mediated_shell_runner STATIC
    src/backends/native_jail/mediated_shell_parser.cpp
    src/backends/native_jail/mediated_shell_dispatch.cpp
    src/backends/native_jail/mediated_filesystem_adapter_posix.cpp)
  target_include_directories(agentengine_mediated_shell_runner PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_mediated_shell_runner PUBLIC agentengine::core agentengine::worktree_store)
  target_link_libraries(agentengine_mediated_shell_runner PRIVATE agentengine_warnings)
  add_library(agentengine::mediated_shell_runner ALIAS agentengine_mediated_shell_runner)
endif()

# ---- Milestone 3 Phase E5 / decisions/ADR-015: libFuzzer harness for MediatedShellRunner's grammar
# parser (010 SS9 G8). Off by default (matches AGENTENGINE_WITH_WASM/AGENTENGINE_BUILD_PYTHON_
# RUNNER's own posture for an opt-in, toolchain-specific target) -- only buildable with a real Clang
# front end (covers both upstream clang++ and clang-cl): libFuzzer's runtime (-fsanitize=fuzzer) has
# no MSVC-native equivalent, the same documented gap this repo already carries for UBSan
# (ADR-001/005/006/007/009's own "no clang toolchain" notes). The whole tree must ALSO be configured
# with -fsanitize=address,undefined in CMAKE_CXX_FLAGS for the target under test
# (agentengine_mediated_shell_runner) to be instrumented -- this option only adds libFuzzer's own
# `-fsanitize=fuzzer` on top, scoped to the fuzz executable alone, never the whole build (a build
# that happens to link the fuzz target's translation unit into anything else would pull in
# libFuzzer's own main(), which must never coexist with an ordinary test binary's).
option(AGENTENGINE_BUILD_SHELL_FUZZER
  "Build the libFuzzer harness against MediatedShellRunner's grammar parser (010 SS9 G8) -- Clang only"
  OFF)

if(AGENTENGINE_BUILD_SHELL_FUZZER)
  if(NOT WIN32)
    message(FATAL_ERROR "AGENTENGINE_BUILD_SHELL_FUZZER requires Windows (matches agentengine_mediated_shell_runner's own scope)")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "AGENTENGINE_BUILD_SHELL_FUZZER requires Clang (libFuzzer has no MSVC-native equivalent) -- got ${CMAKE_CXX_COMPILER_ID}")
  endif()
  add_executable(agentengine_mediated_shell_parser_fuzz
    tests/fuzz/mediated_shell_parser_fuzz.cpp)
  target_include_directories(agentengine_mediated_shell_parser_fuzz PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_mediated_shell_parser_fuzz PRIVATE
    agentengine::mediated_shell_runner)
  target_compile_options(agentengine_mediated_shell_parser_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(agentengine_mediated_shell_parser_fuzz PRIVATE -fsanitize=fuzzer)
endif()

# ---- native_jail Job Object resource-limit layer (src/backends/native_jail/) -- Windows only -----
# Implements decisions/ADR-004-appcontainer-native-jail-windows-backend.md §3 steps 2-3 (build-and-
# measure follow-up named in that ADR's §9): the Windows Job Object half of 008 §2's ResourceLimits
# contract (memory_bytes/pids/cpu_ms, plus a host-side wall-clock watch). Pure Win32 API, no
# third-party dependency, so unlike the PythonRunner backend below this needs no CMake option gate
# beyond the platform check itself -- it is unconditionally available wherever WIN32 is true.
if(WIN32)
  add_library(agentengine_job_object_limits STATIC
    src/backends/native_jail/job_object_limits.cpp)
  target_include_directories(agentengine_job_object_limits PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_job_object_limits PUBLIC agentengine::core)
  target_link_libraries(agentengine_job_object_limits PRIVATE agentengine_warnings)
  add_library(agentengine::job_object_limits ALIAS agentengine_job_object_limits)
endif()

# ---- native_jail SandboxBackend, Windows half (src/backends/native_jail/) -- Windows only -------
# Implements agentengine::SandboxBackend (008 SS1b, SS2, SS3) via AppContainer (app_container_profile.*)
# + the Job Object layer above -- Milestone 2 Phase C task C2. Pure Win32 API (userenv.dll's
# AppContainer profile functions, advapi32.dll's ACL functions) -- no third-party dependency.
if(WIN32)
  add_library(agentengine_native_jail_backend STATIC
    src/backends/native_jail/app_container_profile.cpp
    src/backends/native_jail/native_jail_backend.cpp
    src/backends/native_jail/jailed_worker_rpc.cpp)
  target_include_directories(agentengine_native_jail_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_native_jail_backend PUBLIC
    agentengine::core agentengine::job_object_limits)
  target_link_libraries(agentengine_native_jail_backend PRIVATE
    agentengine_warnings userenv advapi32)
  add_library(agentengine::native_jail_backend ALIAS agentengine_native_jail_backend)
endif()

# ---- native_jail SandboxBackend, Linux half (src/backends/native_jail/) -- Linux only -----------
# Implements agentengine::SandboxBackend (008 SS1b, SS2, SS3) via namespaces (clone()) + cgroups v2
# (cgroup_limits.*) + seccomp-BPF (seccomp_filter.*) -- Milestone 2 Phase C task C2's own follow-up
# (021 SS2 sequencing). Pure Linux syscall API -- no third-party dependency (no libseccomp).
if(NOT WIN32)
  add_library(agentengine_linux_native_jail_backend STATIC
    src/backends/native_jail/cgroup_limits.cpp
    src/backends/native_jail/seccomp_filter.cpp
    src/backends/native_jail/linux_native_jail_backend.cpp)
  target_include_directories(agentengine_linux_native_jail_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_linux_native_jail_backend PUBLIC agentengine::core)
  target_link_libraries(agentengine_linux_native_jail_backend PRIVATE agentengine_warnings)
  add_library(agentengine::linux_native_jail_backend ALIAS agentengine_linux_native_jail_backend)
endif()

# ---- KataBackend (src/backends/kata/) -- OPTIONAL, OFF by default, Linux only -------------------
# Implements agentengine::SandboxBackend (008 §2-3) via Kata Containers -- docs/planning/microvm-
# first-party-backend-design-draft.md Design A, narrowed per that draft's own §7 C2 finding (ONE
# pinned runtime class -- cloud-hypervisor/`kata-clh`, not QEMU -- to bound the CVE-tracking surface
# the way `decisions/ADR-013-...mbedtls-vendoring.md` bounds mbedTLS's). Reopen condition (i)
# (`native-jail`'s own 008 §9 gate reaching Judged on both platforms) was independently re-verified
# met on 2026-08-23 before this option existed -- see `decisions/ADR-004-...md`/`ADR-083-...md`.
#
# No new C++ dependency is vendored here: this backend shells out to the `ctr` CLI already needed to
# operate any Kata/containerd deployment at all (kata_backend.hpp's own header comment has the full
# mechanism and residuals -- SLICE 1 only, `SandboxSpec::capabilities`/`mounts`/`limits`/`net` are
# NOT yet enforced, named honestly there rather than silently assumed). OFF by default so a host that
# never wants the KVM/Kata/containerd runtime dependency doesn't pay for it -- no new required
# toolchain for the v1 completeness target (`native-jail`), mirroring
# `AGENTENGINE_BUILD_PYTHON_RUNNER`'s own posture for this project's other opt-in heavy dependency.
option(AGENTENGINE_BUILD_KATA_BACKEND
  "Build the first-party KataBackend (Linux only; requires a running containerd + Kata Containers deployment with a kata-clh runtime class already configured -- see src/backends/kata/kata_backend.hpp)"
  OFF)

if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  add_library(agentengine_kata_backend STATIC
    src/backends/kata/kata_backend.cpp)
  target_include_directories(agentengine_kata_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_kata_backend PUBLIC agentengine::core)
  # SLICE 10 (2026-08-24, ADR-093): reuses agentengine::net_egress_proxy's resolve_and_validate --
  # the same SSRF-safe (loopback/link-local/RFC1918/CGNAT/multicast/reserved/metadata-address
  # blocking) resolver HostEgressProxy itself uses -- to pin NetPolicy::allowlist hostnames to
  # verified IPv4 literals at create() time, rather than re-implementing that vetted logic here.
  target_link_libraries(agentengine_kata_backend PUBLIC agentengine::net_egress_proxy)
  target_link_libraries(agentengine_kata_backend PRIVATE agentengine_warnings)
  add_library(agentengine::kata_backend ALIAS agentengine_kata_backend)
elseif(WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  message(FATAL_ERROR
    "AGENTENGINE_BUILD_KATA_BACKEND=ON is Linux-only (kata_backend.hpp's own §1 platform split) -- "
    "not buildable on WIN32.")
endif()

# ---- native_process backend (src/backends/native_process/) -- OPTIONAL, OFF by default ---------
# Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md: real, UNSANDBOXED
# host process spawning for NativeShellProvider/NativeBashProvider/NativePythonProvider/
# NativeNodeProvider (ADR-070's "host-configurable responsibility boundary" made concrete for a new
# capability class, not a variant of the existing native_jail sandbox). No third-party dependency
# (Win32 CreateProcessW + the ALREADY-BUILT job_object_limits.hpp/worktree_store primitives, reused
# not duplicated) -- this option exists NOT for CONVENTIONS.md's usual "heavy dependency" reason but
# because real process creation off a host-granted allowlist is a genuinely new, security-relevant
# capability class a consumer dev must deliberately opt into building, matching
# AGENTENGINE_BUILD_PYTHON_RUNNER/AGENTENGINE_WITH_WASM's own "off by default, one option per new
# capability surface" posture. Deliberately never linked into, or reachable from,
# agentengine_shell_runner/agentengine_python_runner -- those targets' own ADR-001 Sh-S1 static
# "no process-creation symbol" proof must stay meaningful independent of whether this option is on.
option(AGENTENGINE_WITH_NATIVE_PROCESS
  "Build the native_process backend (real, unsandboxed host process spawning for the Native*Provider family, ADR-071)"
  OFF)

if(AGENTENGINE_WITH_NATIVE_PROCESS)
  if(WIN32)
    add_library(agentengine_native_process_backend STATIC
      src/backends/native_process/native_process_spawn.cpp
      src/backends/native_process/native_path_scan.cpp
      src/backends/native_process/native_worktree_bridge.cpp)
    target_include_directories(agentengine_native_process_backend PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(agentengine_native_process_backend PUBLIC
      agentengine::core agentengine::job_object_limits agentengine::worktree_store)
    target_link_libraries(agentengine_native_process_backend PRIVATE agentengine_warnings)
    add_library(agentengine::native_process_backend ALIAS agentengine_native_process_backend)
  else()
    message(WARNING "AGENTENGINE_WITH_NATIVE_PROCESS: no POSIX spawn implementation yet "
      "(decisions/ADR-071-native-unsandboxed-process-execution-providers.md, design-only for "
      "non-Windows -- matching this project's existing 'no Linux env available to build/verify "
      "against' posture for other Milestone-3+ Linux gaps). Only native_path_scan.cpp (portable) "
      "is built; there is no native_process_spawn/native_worktree_bridge target on this platform.")
    add_library(agentengine_native_process_backend STATIC
      src/backends/native_process/native_path_scan.cpp)
    target_include_directories(agentengine_native_process_backend PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(agentengine_native_process_backend PUBLIC agentengine::core)
    target_link_libraries(agentengine_native_process_backend PRIVATE agentengine_warnings)
    add_library(agentengine::native_process_backend ALIAS agentengine_native_process_backend)
  endif()
endif()

# ---- wasmtime C API import (src/backends/wasm/) -- Windows + Linux -----------------------------
# Milestone 2 Phase D task D1 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md,
# decision 5): wasmtime 47.0.3 as the `wasm` SandboxBackend's one CONVENTIONS.md tier-2 seam-backend
# heavy dependency (009's plugin ABI). This block imports the dependency (tests/
# test_wasmtime_smoke.cpp, gated the same way below) and, when this option is on, builds the real
# `SandboxBackend` (`agentengine::wasm_backend`, D3, decisions/ADR-010-*.md) plus its tool-pipeline
# bridge (decisions/ADR-040-wasm-tool-pipeline-bridge.md) -- each landed only after its own
# design->red-team->prove->judge pass per CLAUDE.md, same posture as job_object_limits.hpp/
# cgroup_limits.hpp before ADR-004's build-and-measure pass. Off by default, matching
# AGENTENGINE_BUILD_PYTHON_RUNNER's own posture for the other tier-2 heavy dependency this project
# has.
option(AGENTENGINE_WITH_WASM
  "Import the wasmtime C API and build the real wasm SandboxBackend + tool-pipeline bridge (009's plugin ABI, ADR-010/ADR-040)"
  OFF)

if(AGENTENGINE_WITH_WASM)
  # Default (AGENTENGINE_VENDOR_WASMTIME=ON): a pinned, checksummed wasmtime C API release is
  # fetched directly from the project's official GitHub releases (both archives' SHA256 verified by
  # hand against a fresh download during this task, matching AGENTENGINE_VENDOR_PYTHON's own
  # provenance posture above) -- never whatever wasmtime happens to be on the host, for the same
  # reason the vendored-CPython comment above gives.
  #
  # Override: pass -DAGENTENGINE_VENDOR_WASMTIME=OFF and -DAGENTENGINE_WASMTIME_ROOT=<path to an
  # already-extracted wasmtime C API release> to point at any other install instead.
  option(AGENTENGINE_VENDOR_WASMTIME
    "Fetch a pinned, checksummed wasmtime C API release instead of using a local install"
    ON)

  set(AE_VENDORED_WASMTIME_VERSION "47.0.3")  # OQ-7, decision 5

  if(AGENTENGINE_VENDOR_WASMTIME)
    include(FetchContent)
    if(WIN32)
      FetchContent_Declare(ae_vendored_wasmtime
        URL "https://github.com/bytecodealliance/wasmtime/releases/download/v${AE_VENDORED_WASMTIME_VERSION}/wasmtime-v${AE_VENDORED_WASMTIME_VERSION}-x86_64-windows-c-api.zip"
        URL_HASH SHA256=952dfbc864295eccc89caa92bc303686017ca4ea205a1f21878809f374bb77c5
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(ae_vendored_wasmtime)
      set(AGENTENGINE_WASMTIME_ROOT "${ae_vendored_wasmtime_SOURCE_DIR}" CACHE PATH
        "Root of the vendored wasmtime C API install" FORCE)
    elseif(UNIX)
      FetchContent_Declare(ae_vendored_wasmtime
        URL "https://github.com/bytecodealliance/wasmtime/releases/download/v${AE_VENDORED_WASMTIME_VERSION}/wasmtime-v${AE_VENDORED_WASMTIME_VERSION}-x86_64-linux-c-api.tar.xz"
        URL_HASH SHA256=aaa3621f2a3d8393696702897f8f78a1cc504437d500701496d560125aefd732
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(ae_vendored_wasmtime)
      set(AGENTENGINE_WASMTIME_ROOT "${ae_vendored_wasmtime_SOURCE_DIR}" CACHE PATH
        "Root of the vendored wasmtime C API install" FORCE)
    else()
      message(FATAL_ERROR
        "AGENTENGINE_VENDOR_WASMTIME=ON has no fetch wired up on this platform (decision 5: Windows "
        "+ Linux only, matching 021 SS2's target set). Pass -DAGENTENGINE_VENDOR_WASMTIME=OFF and "
        "-DAGENTENGINE_WASMTIME_ROOT=<path to an extracted wasmtime C API release> instead.")
    endif()
  endif()

  set(AGENTENGINE_WASMTIME_ROOT "${AGENTENGINE_WASMTIME_ROOT}" CACHE PATH
    "Root of the wasmtime C API install (must contain include/wasm.h and lib/)")

  if(NOT AGENTENGINE_WASMTIME_ROOT OR NOT EXISTS "${AGENTENGINE_WASMTIME_ROOT}/include/wasm.h")
    message(FATAL_ERROR
      "AGENTENGINE_WITH_WASM is ON but AGENTENGINE_WASMTIME_ROOT "
      "('${AGENTENGINE_WASMTIME_ROOT}') does not contain include/wasm.h. Either leave "
      "AGENTENGINE_VENDOR_WASMTIME=ON (the default) or pass -DAGENTENGINE_WASMTIME_ROOT=<path to an "
      "extracted wasmtime C API release> with -DAGENTENGINE_VENDOR_WASMTIME=OFF.")
  endif()

  # The full-featured build under lib/ (not min/lib/'s reduced-feature Pulley-only build) --
  # include/wasmtime/conf.h in the fetched release confirms WASMTIME_FEATURE_COMPONENT_MODEL is
  # only compiled into this one, and 009's plugin ABI is Component-Model-shaped (CLAUDE.md's locked
  # decision), so lib/ is the only variant this project can use.
  #
  # Windows links the DLL (+ import lib); Linux links the static archive instead of the .so --
  # MEASURED FINDING (M2 Phase D task D1): the released libwasmtime.so has no embedded SONAME. Per
  # glibc ld.so's documented behavior, a DT_NEEDED entry containing a slash is treated as a literal
  # (relative-to-CWD, if not absolute) pathname and bypasses RPATH/RUNPATH/LD_LIBRARY_PATH search
  # entirely -- confirmed directly: a test executable linked against the .so recorded DT_NEEDED as
  # the literal build-relative path "_deps/ae_vendored_wasmtime-src/lib/libwasmtime.so", so it only
  # ran when launched with that exact relative path resolvable from the current CWD (an accident of
  # directory choice, not a fix), and failed under `ctest` (whose test CWD is the test's own binary
  # directory, not the top-level build directory) with "cannot open shared object file". The static
  # archive has no such problem -- it becomes part of the final binary, nothing to locate at run
  # time. Needs `pthread`/`dl`/`m` on the final link (measured directly against this release, Rust's
  # usual static-lib convention) -- declared as this imported target's own INTERFACE_LINK_LIBRARIES
  # so `target_link_libraries(... agentengine::wasmtime_vendor)` alone is enough for a consumer.
  if(WIN32)
    add_library(agentengine_wasmtime_vendor SHARED IMPORTED GLOBAL)
    target_include_directories(agentengine_wasmtime_vendor INTERFACE
      "${AGENTENGINE_WASMTIME_ROOT}/include")
    set_target_properties(agentengine_wasmtime_vendor PROPERTIES
      IMPORTED_LOCATION "${AGENTENGINE_WASMTIME_ROOT}/lib/wasmtime.dll"
      IMPORTED_IMPLIB "${AGENTENGINE_WASMTIME_ROOT}/lib/wasmtime.dll.lib")
    # Windows has no rpath; wasmtime.dll must be reachable via PATH at test-run time, wired via
    # each test's ENVIRONMENT property in tests/CMakeLists.txt (same posture as
    # AGENTENGINE_PYTHON_DLL_DIR above).
    set(AGENTENGINE_WASMTIME_DLL_DIR "${AGENTENGINE_WASMTIME_ROOT}/lib" CACHE PATH
      "Directory containing wasmtime.dll, prepended to PATH for tests" FORCE)
  else()
    add_library(agentengine_wasmtime_vendor STATIC IMPORTED GLOBAL)
    target_include_directories(agentengine_wasmtime_vendor INTERFACE
      "${AGENTENGINE_WASMTIME_ROOT}/include")
    set_target_properties(agentengine_wasmtime_vendor PROPERTIES
      IMPORTED_LOCATION "${AGENTENGINE_WASMTIME_ROOT}/lib/libwasmtime.a"
      INTERFACE_LINK_LIBRARIES "pthread;dl;m")
  endif()
  add_library(agentengine::wasmtime_vendor ALIAS agentengine_wasmtime_vendor)

  # ---- wasm SandboxBackend (src/backends/wasm/) -- Windows + Linux -----------------------------
  # Milestone 2 Phase D task D3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
  # the first real implementation under src/backends/wasm/ -- security-critical, built through
  # design->red-team->prove->judge per CLAUDE.md and landed as
  # decisions/ADR-010-wasm-component-host-manifest-capability-binding.md before this target existed.
  add_library(agentengine_wasm_backend STATIC
    src/backends/wasm/wasm_backend.cpp)
  target_include_directories(agentengine_wasm_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_wasm_backend PUBLIC
    agentengine::core agentengine::wasmtime_vendor agentengine::net_egress_proxy)
  target_link_libraries(agentengine_wasm_backend PRIVATE agentengine_warnings)
  add_library(agentengine::wasm_backend ALIAS agentengine_wasm_backend)
endif()

# ---- Default sandbox-backend registry wiring (src/sandbox/) -------------------------------------
# docs/planning/default-sandbox-registry-wiring-design-draft.md: build_default_sandbox_registry(),
# real host-callable code that registers whichever SandboxBackend conformers this build was
# actually compiled with into a SandboxBackendRegistry (include/agentengine/sandbox/
# sandbox_backend_registry.hpp) -- the "how does a backend get into the registry at all" half of
# the gap the prior sandbox-backend-registry-design-draft.md named and deliberately deferred.
# Always built (the native-jail leg has no off switch); the wasm/kata legs are compiled in only
# when their own options are already ON, via AGENTENGINE_HAVE_WASM_BACKEND/
# AGENTENGINE_HAVE_KATA_BACKEND (PRIVATE -- only this target's own .cpp reads them).
add_library(agentengine_default_sandbox_registry STATIC
  src/sandbox/default_sandbox_registry.cpp)
target_include_directories(agentengine_default_sandbox_registry PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::core)
target_link_libraries(agentengine_default_sandbox_registry PRIVATE agentengine_warnings)
if(WIN32)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::native_jail_backend)
elseif(UNIX)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::linux_native_jail_backend)
endif()
if(AGENTENGINE_WITH_WASM)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::wasm_backend)
  # PUBLIC, not PRIVATE (self-caught correction after the correctness red-team's PRIVATE suggestion
  # -- true for the .cpp alone, but test_default_sandbox_registry.cpp also needs to see this macro
  # to conditionally compile its own wasm/kata assertions; PRIVATE would silently skip those checks
  # even when the backend is actually built in).
  target_compile_definitions(agentengine_default_sandbox_registry PUBLIC AGENTENGINE_HAVE_WASM_BACKEND)
endif()
if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::kata_backend)
  target_compile_definitions(agentengine_default_sandbox_registry PUBLIC AGENTENGINE_HAVE_KATA_BACKEND)
endif()
add_library(agentengine::default_sandbox_registry ALIAS agentengine_default_sandbox_registry)

# ---- HTTPS/TLS client for the egress proxy (src/sandbox/) -- Windows + Linux -------------------
# decisions/ADR-013-https-egress-tls-client.md (M2 residual, ADR-011 section 9's named follow-up):
# mbedTLS as the egress proxy's one CONVENTIONS.md tier-2 heavy dependency for ordinary outbound
# HTTPS (system-CA-style validation + real hostname verification -- NOT Quark's SecureTransport,
# built for node-identity cluster mTLS, ADR-011 section 3 Design C's own rejection reasoning). Off
# by default, matching AGENTENGINE_WITH_WASM/AGENTENGINE_BUILD_PYTHON_RUNNER's posture for this
# project's other two tier-2 dependencies.
option(AGENTENGINE_WITH_HTTPS
  "Vendor mbedTLS + a pinned CA bundle for the egress proxy's https:// support (ADR-013)"
  OFF)

if(AGENTENGINE_WITH_HTTPS)
  # Default (AGENTENGINE_VENDOR_MBEDTLS=ON): a pinned, checksummed mbedTLS source release built as
  # an ordinary CMake subproject -- NOT Quark's own QUARK_WITH_MBEDTLS (a vcpkg-resolved
  # "version>=3.6.2" floor with no archive checksum, a materially weaker pinning discipline than
  # this project's own wasmtime/CPython precedent above, ADR-013 section 3/6). The LTS 3.6.x line,
  # matching Quark's own floor, not the newer 4.x major (avoids an unfamiliar API surface for a
  # security-critical dependency).
  #
  # Override: pass -DAGENTENGINE_VENDOR_MBEDTLS=OFF and -DAGENTENGINE_MBEDTLS_ROOT=<path to an
  # already-extracted mbedTLS source release, containing its own top-level CMakeLists.txt> instead.
  option(AGENTENGINE_VENDOR_MBEDTLS
    "Fetch a pinned, checksummed mbedTLS source release instead of using a local copy"
    ON)

  set(AE_VENDORED_MBEDTLS_VERSION "3.6.7")

  # mbedTLS's own CMakeLists.txt auto-detects MBEDTLS_AS_SUBPROJECT and defaults ENABLE_TESTING to
  # OFF under add_subdirectory() -- ENABLE_PROGRAMS defaults ON regardless, and
  # MBEDTLS_FATAL_WARNINGS (also ON by default) is a third-party library's own warning posture
  # under whatever compiler flags it happens to be built with here, not something this project's
  # own AgentEngineWarnings module should be answerable for -- both forced off.
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

  if(AGENTENGINE_VENDOR_MBEDTLS)
    include(FetchContent)
    FetchContent_Declare(ae_vendored_mbedtls
      URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${AE_VENDORED_MBEDTLS_VERSION}/mbedtls-${AE_VENDORED_MBEDTLS_VERSION}.tar.bz2"
      URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(ae_vendored_mbedtls)
  else()
    set(AGENTENGINE_MBEDTLS_ROOT "${AGENTENGINE_MBEDTLS_ROOT}" CACHE PATH
      "Root of a local mbedTLS source copy (must contain its own top-level CMakeLists.txt)")
    if(NOT AGENTENGINE_MBEDTLS_ROOT OR NOT EXISTS "${AGENTENGINE_MBEDTLS_ROOT}/CMakeLists.txt")
      message(FATAL_ERROR
        "AGENTENGINE_WITH_HTTPS is ON, AGENTENGINE_VENDOR_MBEDTLS is OFF, but AGENTENGINE_MBEDTLS_ROOT "
        "('${AGENTENGINE_MBEDTLS_ROOT}') does not contain a CMakeLists.txt. Either leave "
        "AGENTENGINE_VENDOR_MBEDTLS=ON (the default) or pass a real path.")
    endif()
    add_subdirectory("${AGENTENGINE_MBEDTLS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls_local" EXCLUDE_FROM_ALL)
  endif()

  # A pinned, checksummed CA root bundle (curl.se's well-known, regularly-refreshed distribution of
  # Mozilla's own root program) -- mbedTLS has no OS-trust-store integration of its own (unlike
  # OpenSSL/Schannel); vendoring one keeps this project's own "no host-machine-dependent build
  # state" posture (matching AGENTENGINE_VENDOR_PYTHON's own comment above) rather than trusting
  # whatever the build machine happens to have installed. AgentEngine owns this file's rotation over
  # time (ADR-013 section 9's named residual) -- bundle snapshot dated 2026-08-13 (curl.se's own
  # header line: "Certificate data from Mozilla as of: Thu Aug 13 03:12:01 2026 GMT"), re-fetched and
  # hand-verified (downloaded independently, content inspected, hash computed locally before pinning)
  # after the prior 2026-07-16 pin went stale from ordinary upstream CA-bundle rotation.
  option(AGENTENGINE_VENDOR_CA_BUNDLE
    "Fetch a pinned, checksummed CA root bundle instead of using a local PEM file"
    ON)
  if(AGENTENGINE_VENDOR_CA_BUNDLE)
    set(AE_CA_BUNDLE_PATH "${CMAKE_CURRENT_BINARY_DIR}/vendored_ca_bundle/cacert.pem")
    file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${AE_CA_BUNDLE_PATH}"
      EXPECTED_HASH SHA256=f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9)
  else()
    set(AE_CA_BUNDLE_PATH "${AGENTENGINE_CA_BUNDLE_PATH}" CACHE FILEPATH
      "Path to a local PEM CA bundle to embed instead of the vendored one")
    if(NOT AE_CA_BUNDLE_PATH OR NOT EXISTS "${AE_CA_BUNDLE_PATH}")
      message(FATAL_ERROR
        "AGENTENGINE_VENDOR_CA_BUNDLE is OFF but AGENTENGINE_CA_BUNDLE_PATH ('${AE_CA_BUNDLE_PATH}') "
        "does not exist.")
    endif()
  endif()

  # Embedded into the binary at compile time (a generated .cpp, see cmake/ca_bundle_embed.cpp.in),
  # not loaded from a file path at run time -- ADR-013's own choice: a run-time path would need to
  # keep resolving correctly wherever the build tree or installed binary ends up, which a compiled-
  # in constant sidesteps entirely.
  file(READ "${AE_CA_BUNDLE_PATH}" AE_CA_BUNDLE_CONTENT)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ca_bundle_embed.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle_embed.cpp" @ONLY)

  # ---- TLS client (src/sandbox/) -- Windows + Linux ---------------------------------------------
  add_library(agentengine_tls_client STATIC
    src/sandbox/tls_client.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle_embed.cpp")
  target_link_libraries(agentengine_tls_client PUBLIC agentengine::core)
  target_link_libraries(agentengine_tls_client PRIVATE
    agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_library(agentengine::tls_client ALIAS agentengine_tls_client)

  # Extends the already-defined agentengine::net_egress_proxy target (above) rather than moving its
  # definition down here -- AGENTENGINE_WITH_HTTPS is resolved after that target already exists,
  # and CMake target_link_libraries()/target_compile_definitions() calls accumulate across a file.
  target_link_libraries(agentengine_net_egress_proxy PRIVATE agentengine::tls_client)
  target_compile_definitions(agentengine_net_egress_proxy PUBLIC AGENTENGINE_WITH_HTTPS)

  # ---- Milestone 5 Phase C: host-initiated provider HTTP client (src/sandbox/) -------------------
  # Reuses agentengine::net_egress_proxy's resolve_and_validate/perform_https_exchange directly
  # (provider_http_client.hpp's own top comment) -- HTTPS only, so this only exists under the same
  # AGENTENGINE_WITH_HTTPS gate net_egress_proxy's own HTTPS half is behind.
  add_library(agentengine_provider_http_client STATIC src/sandbox/provider_http_client.cpp)
  target_link_libraries(agentengine_provider_http_client PUBLIC agentengine::net_egress_proxy)
  target_link_libraries(agentengine_provider_http_client PRIVATE agentengine_warnings)
  add_library(agentengine::provider_http_client ALIAS agentengine_provider_http_client)
endif()

# ---- Cross-process capability tokens (src/trust/) -- Windows only ------------------------------
# Implements decisions/ADR-005-capability-bearer-tokens-cross-process.md (resolves OQ-3): both
# competing designs it prove-phases -- Design A (trust/capability_token.hpp, a self-verifying
# macaroon-style bearer token) and Design B (trust/capability_registry.hpp, the status-quo
# host-side-registry alternative it is measured against). HMAC-SHA256 and randomness come from
# Windows CNG/BCrypt, a system API (bcrypt.lib) -- not a third-party dependency, same posture as
# job_object_limits.cpp's direct use of Job Objects, so this needs no CMake option gate beyond the
# platform check. trust/hmac.cpp (Milestone 7, ADR-021) extracted the shared HMAC-SHA256/constant-
# time-compare primitive so bearer_token.cpp (ADR-021's own inbound-auth token, a DIFFERENT concept
# from this file's cross-process capability token) reuses the identical audited implementation
# rather than a second one.
if(WIN32)
  add_library(agentengine_capability_token STATIC
    src/trust/hmac.cpp
    src/trust/capability_token.cpp
    src/trust/capability_registry.cpp
    src/trust/bearer_token.cpp)
  target_link_libraries(agentengine_capability_token PUBLIC agentengine::core)
  target_link_libraries(agentengine_capability_token PRIVATE agentengine_warnings bcrypt)
  add_library(agentengine::capability_token ALIAS agentengine_capability_token)
endif()

# ---- Worktree content-addressed object store (src/core/) ---------------------------------------
# Implements 025-Worktree-and-Virtual-Filesystem.md §2, Milestone 3 Phase A1
# (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md): SHA-256 content digests
# for Blob/Tree objects via Windows CNG/BCrypt, a system API -- not a third-party dependency, same
# posture as ADR-005's capability_token.cpp. The Linux SHA-256 provider (worktree_digest_posix.cpp,
# below) closes the breakdown doc's own decision-2 gap with a self-contained, dependency-free FIPS
# 180-4 implementation rather than pulling in a new mandatory third-party dependency -- see that
# file's own top comment for why (mbedTLS is vendored but off by default behind
# AGENTENGINE_WITH_HTTPS, and OpenSSL/AF_ALG were both considered and rejected).
if(WIN32)
  add_library(agentengine_worktree_store STATIC
    src/core/worktree_digest.cpp
    src/core/worktree_mount_fs.cpp)
  target_link_libraries(agentengine_worktree_store PUBLIC agentengine::core)
  target_link_libraries(agentengine_worktree_store PRIVATE agentengine_warnings bcrypt)
  add_library(agentengine::worktree_store ALIAS agentengine_worktree_store)
endif()

# ---- Worktree mount path-escape primitive (src/core/) -- Linux half, Milestone 3 Phase C4 --------
# Implements 025 §5's OS-level path-escape corpus, Linux parity for `worktree_mount_fs.hpp`'s
# Windows-only `open_within_mount_root` (Phase C2, ADR-014, Judged) -- an ordinary follow-on task
# per the breakdown doc's decision 6, not a second ADR. worktree_digest_posix.cpp supplies this
# branch's compute_digest -- the decision-2 gap the comment above used to describe as "still open"
# is closed as of this pass.
if(NOT WIN32)
  add_library(agentengine_worktree_store STATIC
    src/core/worktree_digest_posix.cpp
    src/core/worktree_mount_fs_posix.cpp)
  target_link_libraries(agentengine_worktree_store PUBLIC agentengine::core)
  target_link_libraries(agentengine_worktree_store PRIVATE agentengine_warnings)
  add_library(agentengine::worktree_store ALIAS agentengine_worktree_store)
endif()

# ---- RealIoFileSystem's platform-specific verified I/O (src/sandbox/) --------------------------
# Implements decisions/ADR-104-real-io-filesystem-linux-parity.md: `RealIoFileSystem::write_verified()`/
# `read_verified()` (sandbox/real_io_filesystem.hpp) are the only two methods on that class that need
# a real, verified OS handle -- split out-of-line per platform, mirroring `agentengine_worktree_store`'s
# own WIN32/`NOT WIN32` split just above. Every other method on `RealIoFileSystem` stays inline in the
# header (pure std::filesystem/Ledger/AsyncMutex, no OS-specific symbol).
if(WIN32)
  add_library(agentengine_sandbox_io STATIC src/sandbox/real_io_filesystem.cpp)
  target_link_libraries(agentengine_sandbox_io PUBLIC agentengine::core agentengine::worktree_store)
  target_link_libraries(agentengine_sandbox_io PRIVATE agentengine_warnings)
  add_library(agentengine::sandbox_io ALIAS agentengine_sandbox_io)
endif()
if(NOT WIN32)
  add_library(agentengine_sandbox_io STATIC src/sandbox/real_io_filesystem_posix.cpp)
  target_link_libraries(agentengine_sandbox_io PUBLIC agentengine::core agentengine::worktree_store)
  target_link_libraries(agentengine_sandbox_io PRIVATE agentengine_warnings)
  add_library(agentengine::sandbox_io ALIAS agentengine_sandbox_io)
endif()

# HandleRelay (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md,
# ADR-081 Slice 2): dispatch_worker_query's "open"/"listdir" handlers call
# open_within_mount_root/list_within_mount_root/mount_root_usage (agentengine::worktree_store), and
# its "connect_authorize" handler calls resolve_and_validate (agentengine::net_egress_proxy) -- both
# targets already exist (defined above); this extra target_link_libraries call, not a change to the
# native_jail_backend target's own original definition, is deliberately placed here so it comes after
# both dependencies are declared.
if(WIN32)
  target_link_libraries(agentengine_native_jail_backend PUBLIC
    agentengine::worktree_store agentengine::net_egress_proxy)
endif()

# ---- native_jail PythonRunner backend (src/backends/native_jail/) -- OPTIONAL, OFF by default ---
# Implements decisions/ADR-002-pythonrunner-embedding-and-mediation.md (prove phase): the embedded
# CPython interpreter, Layer 0's sys.modules sweep, and the sys.meta_path allowlist finder
# (python_lockdown.{hpp,cpp}), consumed by python_runner.hpp. CPython is a genuine heavy dependency
# (CONVENTIONS.md tier 2 — "seam backends may take a heavy dependency... one dependency per
# backend, behind a CMake option, never linked into a build that does not select that backend") --
# this target is built ONLY when AGENTENGINE_BUILD_PYTHON_RUNNER is explicitly turned ON, and links
# against the configured CPython install, never against agentengine_core or
# agentengine_shell_runner (that library's own Sh-S1 static "no process-creation symbol" check,
# ADR-001, must stay meaningful independent of whether this option is on).
#
# ADR-002's own §5.5.6 decision ("one process per session") means this library assumes at most one
# PythonLockdownInterpreter is ever alive in one process -- see python_lockdown.hpp's file header.
option(AGENTENGINE_BUILD_PYTHON_RUNNER
  "Build the native_jail PythonRunner backend (embeds a real CPython interpreter; requires a CPython dev install — see AGENTENGINE_PYTHON_ROOT)"
  OFF)

if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  # AGENTENGINE_PYTHON_ROOT: root of the CPython install/venv to embed against. Must contain
  # include/Python.h and (on Windows) libs/python3<minor>.lib next to a python3<minor>.dll findable
  # at test-run time.
  #
  # Default (AGENTENGINE_VENDOR_PYTHON=ON, the default): a pinned, checksummed CPython build is
  # fetched via the official "python" NuGet package (published by the Python Software Foundation,
  # api.nuget.org/v3-flatcontainer/python/... -- the same embeddable-with-dev-headers distribution
  # python.org's own embedding docs point to) and numpy/pandas are pip-installed at pinned versions
  # into a vendored site-packages directory under the build tree. This is deliberately NOT the host
  # machine's Python: no build depends on what happens to be installed where, matching
  # decisions/ADR-004-appcontainer-native-jail-windows-backend.md §3 step 1 and §5.4's finding that
  # ACL-granting a system-owned interpreter needs elevation a vendored, build-owned one does not.
  # Version pins match the target ADR-002/ADR-003 already proved against (CPython 3.13.5, numpy
  # 2.3.3, pandas 2.3.3) rather than floating to whatever is newest.
  #
  # Override: pass -DAGENTENGINE_VENDOR_PYTHON=OFF and -DAGENTENGINE_PYTHON_ROOT=<path> to point at
  # any other CPython install (a system install, a venv, a different pinned version for CI matrix
  # testing) instead.
  option(AGENTENGINE_VENDOR_PYTHON
    "Fetch a pinned, checksummed CPython + numpy/pandas instead of using a host-machine install"
    ON)

  set(AE_VENDORED_PYTHON_VERSION "3.13.5")
  set(AE_VENDORED_PYTHON_NUPKG_SHA256
    "fc496ae8aa3f7318704e24c4885c0cc0e3e6060c6e8af28aaa266226c01d96a1")
  set(AE_VENDORED_NUMPY_VERSION "2.3.3")
  set(AE_VENDORED_PANDAS_VERSION "2.3.3")

  if(AGENTENGINE_VENDOR_PYTHON)
    if(NOT WIN32)
      message(FATAL_ERROR
        "AGENTENGINE_VENDOR_PYTHON=ON only has a Windows fetch wired up this pass (the NuGet "
        "'python' package is Windows-only). Pass -DAGENTENGINE_VENDOR_PYTHON=OFF and "
        "-DAGENTENGINE_PYTHON_ROOT=<path> on other platforms (see ADR-002 §9's own POSIX gap).")
    endif()

    include(FetchContent)
    FetchContent_Declare(ae_vendored_python
      URL "https://api.nuget.org/v3-flatcontainer/python/${AE_VENDORED_PYTHON_VERSION}/python.${AE_VENDORED_PYTHON_VERSION}.nupkg"
      URL_HASH SHA256=${AE_VENDORED_PYTHON_NUPKG_SHA256}
      DOWNLOAD_NAME "python-${AE_VENDORED_PYTHON_VERSION}.zip" # nupkg IS a zip; the extension just
                                                                # needs to say so for the extractor.
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(ae_vendored_python)
    set(AGENTENGINE_PYTHON_ROOT "${ae_vendored_python_SOURCE_DIR}/tools" CACHE PATH
      "Root of the vendored CPython install" FORCE)

    # Pin numpy/pandas into a vendored, build-owned site-packages directory -- never the vendored
    # interpreter's own Lib/site-packages, so `python -m pip install` above and this stay clearly
    # separated (one is the interpreter, one is third-party packages, matching 010 §5's tiering).
    # Guarded by a stamp file recording exactly which pins were last installed, so a routine
    # reconfigure doesn't re-hit PyPI every time -- only a version-pin change here does.
    set(AE_VENDORED_SITE_PACKAGES "${CMAKE_BINARY_DIR}/vendored_python_packages")
    set(AE_VENDORED_PACKAGES_STAMP "${AE_VENDORED_SITE_PACKAGES}.stamp")
    set(AE_VENDORED_PACKAGES_PIN "numpy==${AE_VENDORED_NUMPY_VERSION};pandas==${AE_VENDORED_PANDAS_VERSION}")
    set(AE_NEED_PACKAGE_INSTALL TRUE)
    if(EXISTS "${AE_VENDORED_PACKAGES_STAMP}")
      file(READ "${AE_VENDORED_PACKAGES_STAMP}" AE_LAST_PACKAGES_PIN)
      if(AE_LAST_PACKAGES_PIN STREQUAL AE_VENDORED_PACKAGES_PIN)
        set(AE_NEED_PACKAGE_INSTALL FALSE)
      endif()
    endif()
    if(AE_NEED_PACKAGE_INSTALL)
      message(STATUS "Vendoring numpy==${AE_VENDORED_NUMPY_VERSION} pandas==${AE_VENDORED_PANDAS_VERSION} "
        "into ${AE_VENDORED_SITE_PACKAGES} (pinned, one-time per version change)")
      execute_process(
        COMMAND "${AGENTENGINE_PYTHON_ROOT}/python.exe" -m pip install --no-input
          --target "${AE_VENDORED_SITE_PACKAGES}"
          "numpy==${AE_VENDORED_NUMPY_VERSION}" "pandas==${AE_VENDORED_PANDAS_VERSION}"
        RESULT_VARIABLE AE_PIP_RESULT)
      if(NOT AE_PIP_RESULT EQUAL 0)
        message(FATAL_ERROR "Vendoring numpy/pandas via pip failed (exit ${AE_PIP_RESULT})")
      endif()
      file(WRITE "${AE_VENDORED_PACKAGES_STAMP}" "${AE_VENDORED_PACKAGES_PIN}")
    endif()
    set(AGENTENGINE_PYTHON_SITE_PACKAGES_OVERRIDE "${AE_VENDORED_SITE_PACKAGES}")
  endif()

  set(AGENTENGINE_PYTHON_ROOT "${AGENTENGINE_PYTHON_ROOT}" CACHE PATH
    "Root of the CPython install/venv to embed (must contain include/Python.h)")

  if(NOT AGENTENGINE_PYTHON_ROOT OR NOT EXISTS "${AGENTENGINE_PYTHON_ROOT}/include/Python.h")
    message(FATAL_ERROR
      "AGENTENGINE_BUILD_PYTHON_RUNNER is ON but AGENTENGINE_PYTHON_ROOT "
      "('${AGENTENGINE_PYTHON_ROOT}') does not contain include/Python.h. Either leave "
      "AGENTENGINE_VENDOR_PYTHON=ON (the default) or pass -DAGENTENGINE_PYTHON_ROOT=<path to a "
      "CPython install/venv root> with -DAGENTENGINE_VENDOR_PYTHON=OFF.")
  endif()

  if(NOT WIN32)
    # POSIX wiring (python3-config/pkg-config --embed, .so soname handling) is NOT ATTEMPTED this
    # pass (decisions/ADR-002-pythonrunner-embedding-and-mediation.md §8/§9) -- only the Windows
    # miniconda-style include/libs layout named as this ADR's concrete embedding target is wired
    # up. Recorded here as an explicit, honest gap rather than a silently-wrong guess at flags.
    message(FATAL_ERROR
      "AGENTENGINE_BUILD_PYTHON_RUNNER: only the Windows (miniconda-style include/libs) layout is "
      "wired up this pass; POSIX python3-config/pkg-config wiring is NOT ATTEMPTED (see ADR-002 §9).")
  endif()

  find_library(AE_PYTHON_IMPORT_LIB
    NAMES python313 python3
    PATHS "${AGENTENGINE_PYTHON_ROOT}/libs"
    NO_DEFAULT_PATH)
  if(NOT AE_PYTHON_IMPORT_LIB)
    message(FATAL_ERROR
      "Could not find python313.lib/python3.lib under ${AGENTENGINE_PYTHON_ROOT}/libs")
  endif()

  add_library(agentengine_python_runner STATIC
    src/backends/native_jail/python_lockdown.cpp)
  target_include_directories(agentengine_python_runner PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_include_directories(agentengine_python_runner PRIVATE
    "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(agentengine_python_runner PUBLIC agentengine::core)
  target_link_libraries(agentengine_python_runner PRIVATE "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  add_library(agentengine::python_runner ALIAS agentengine_python_runner)

  # ---- Jailed Python worker (python_worker_main.cpp/python_worker_mediation.cpp) -- the ADR
  # superseding native_jail_backend.hpp's former "Correction (2026-08-23)" comment: real CPython
  # execution now happens in THIS separate, AppContainer+Job-Object-jailed executable, spawned by
  # NativeJailBackend::create_python_worker() (native_jail_backend.cpp), never in the host process
  # that embeds agentengine_mediated_python_runner. This is the ONLY target in this CMakeLists.txt
  # that still needs Python.h/AE_PYTHON_IMPORT_LIB for the mediated-runner subsystem -- see
  # python_worker_mediation.hpp's own file header for exactly what moved here and why.
  add_executable(agentengine_python_worker
    src/backends/native_jail/python_worker_main.cpp
    src/backends/native_jail/python_worker_mediation.cpp
    src/backends/native_jail/jailed_worker_rpc.cpp)
  target_include_directories(agentengine_python_worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(agentengine_python_worker PRIVATE
    agentengine::core "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  # python313.dll (or whatever this vendored/configured CPython's runtime DLL is actually named) must
  # sit next to the worker's own .exe: this process is spawned CREATE_SUSPENDED inside an AppContainer
  # with a FIXED, minimal environment block (native_jail_backend.cpp's build_minimal_environment_block(),
  # unchanged, byte-for-byte, by this design) that does NOT inherit the launching process's own PATH --
  # so "next to the exe" (Windows' own default DLL search order) is the only reliable way for this
  # specific child to find it, not an environment-block special case that would touch the untouched
  # per_exec exec() path's own environment-building function.
  file(GLOB AE_PYTHON_RUNTIME_DLLS "${AGENTENGINE_PYTHON_ROOT}/python3*.dll")
  foreach(AE_ONE_PYTHON_DLL IN LISTS AE_PYTHON_RUNTIME_DLLS)
    add_custom_command(TARGET agentengine_python_worker POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${AE_ONE_PYTHON_DLL}" "$<TARGET_FILE_DIR:agentengine_python_worker>"
      COMMENT "Copying ${AE_ONE_PYTHON_DLL} next to agentengine_python_worker.exe")
  endforeach()

  # ---- Milestone 3 Phase E2's real PythonRunner (mediated_python_runner.{hpp,cpp}) -- REBUILT by
  # the jailed-worker design as an IPC CLIENT: no CPython dependency of its own any more (see that
  # file's own header comment for exactly what moved into agentengine_python_worker above). Links
  # agentengine::native_jail_backend instead of a Python include dir/import lib.
  add_library(agentengine_mediated_python_runner STATIC
    src/backends/native_jail/mediated_python_runner.cpp)
  target_include_directories(agentengine_mediated_python_runner PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(agentengine_mediated_python_runner PUBLIC
    agentengine::core agentengine::native_jail_backend)
  target_link_libraries(agentengine_mediated_python_runner PRIVATE agentengine_warnings)
  # Fixed, host-controlled, build-time path to the worker binary (I2 -- never resolved at runtime from
  # anything guest/model-influenced, e.g. GetModuleFileNameW-relative-to-whatever-process-happens-to-
  # embed-this-runner). PUBLIC: every consumer of agentengine::mediated_python_runner (cli_chat, every
  # test that constructs a MediatedPythonRunner) needs this macro defined identically, and none of them
  # should have to know or repeat the exact target-file generator expression themselves.
  target_compile_definitions(agentengine_mediated_python_runner PUBLIC
    AE_PYTHON_WORKER_EXE_PATH="$<TARGET_FILE:agentengine_python_worker>")
  add_dependencies(agentengine_mediated_python_runner agentengine_python_worker)
  add_library(agentengine::mediated_python_runner ALIAS agentengine_mediated_python_runner)

  # Runtime inputs the tests need: the CPython "home" (PyConfig.home -> stdlib search path), the
  # curated site-packages directory (numpy/pandas live there, added to sys.path by the lockdown
  # config rather than by running site.py -- see python_lockdown.cpp's file header), and the DLL
  # directory (Windows has no rpath; python3<minor>.dll must be reachable via PATH at test-run time,
  # wired below via each test's ENVIRONMENT property in tests/CMakeLists.txt).
  set(AGENTENGINE_PYTHON_HOME "${AGENTENGINE_PYTHON_ROOT}" CACHE PATH
    "CPython 'home' passed to PyConfig.home (Py_DecodeLocale'd) at runtime" FORCE)
  # Vendored case: numpy/pandas were pip --target-installed into a build-owned directory (above),
  # deliberately NOT the interpreter's own Lib/site-packages -- use that. Non-vendored case:
  # unchanged, the host install/venv's own site-packages (010 §5 "preinstalled" policy stand-in).
  if(AGENTENGINE_PYTHON_SITE_PACKAGES_OVERRIDE)
    set(AGENTENGINE_PYTHON_SITE_PACKAGES "${AGENTENGINE_PYTHON_SITE_PACKAGES_OVERRIDE}" CACHE PATH
      "Curated site-packages directory added to sys.path (010 §5 'preinstalled' policy stand-in)" FORCE)
  else()
    set(AGENTENGINE_PYTHON_SITE_PACKAGES "${AGENTENGINE_PYTHON_ROOT}/Lib/site-packages" CACHE PATH
      "Curated site-packages directory added to sys.path (010 §5 'preinstalled' policy stand-in)" FORCE)
  endif()
  set(AGENTENGINE_PYTHON_DLL_DIR "${AGENTENGINE_PYTHON_ROOT}" CACHE PATH
    "Directory containing python3<minor>.dll, prepended to PATH for tests" FORCE)
endif()

# ---- CLI chat (interactive validation of 009 §8's skill mechanism against a real model + real code
# execution) -- only buildable when both a real chat backend (HTTPS) and a real Python embed
# (AGENTENGINE_BUILD_PYTHON_RUNNER) are available; needs python3<minor>.dll reachable via PATH at run
# time (Windows has no rpath), same AGENTENGINE_PYTHON_DLL_DIR every Python-runner test already uses.
if(AGENTENGINE_WITH_HTTPS AND AGENTENGINE_BUILD_PYTHON_RUNNER)
  add_executable(agentengine_cli_chat tools/cli_chat.cpp)
  target_link_libraries(agentengine_cli_chat PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::mediated_python_runner
    agentengine::mediated_shell_runner agentengine::worktree_store agentengine::sandbox_io
    agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  target_compile_definitions(agentengine_cli_chat PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  # ADR-102 Phase 5: wiring MandatorySandboxProvider/SandboxRuntime/Ledger<> (real, template-heavy
  # code) into this already template-heavy file pushed its one .obj past MSVC's COFF section-count
  # limit (C1128) -- a mechanical compiler limit, not a real code defect. /bigobj is the documented
  # MSVC fix; scoped to this one target, not project-wide, since nothing else needs it.
  if(MSVC)
    target_compile_options(agentengine_cli_chat PRIVATE /bigobj)
  endif()
endif()

# ---- ADR-102 Phase 5 slice: ComposedContextProvider<Ms...> real production consumer -------------
# tools/sandboxed_shell_chat.cpp -- the first real, user-reachable CLI wiring of
# ComposedContextProvider<Ms...> anywhere in this codebase, composing SandboxToolProvider (ADR-096)
# and MandatorySandboxProvider<DockerExecutionSurface> (ADR-102 Phases 1-4) into ONE session. Needs no
# Python embed (unlike agentengine_cli_chat above), so gated only on AGENTENGINE_WITH_HTTPS (real
# OpenAI network calls) and WIN32. NOT for the reason previously stated here (SandboxToolProvider's
# own platform scope -- stale as of ADR-103, see decisions/ADR-105-sandbox-tool-provider-composed-
# linux-parity.md): the REAL Linux blocker, found empirically while investigating that stale claim, is
# unrelated -- this file's use of QuarantineSecretStore (trust/secret_quarantine.hpp) pulls in
# agentengine::trust::hmac_sha256, which has NO Linux implementation at all (only built inside
# agentengine_capability_token below, itself WIN32-only per ADR-005's own deliberate BCrypt/CNG scope).
# A real, disclosed, separate residual -- not attempted here (see ADR-105 §7).
if(AGENTENGINE_WITH_HTTPS AND WIN32)
  add_executable(agentengine_sandboxed_shell_chat tools/sandboxed_shell_chat.cpp)
  target_link_libraries(agentengine_sandboxed_shell_chat PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::mediated_shell_runner
    agentengine::worktree_store agentengine::sandbox_io agentengine_warnings MbedTLS::mbedtls
    MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# ---- Standalone batch inference (tools/batch_infer.cpp) -- submits N independent prompts to
# OpenRouter's real Batch API (beta) and polls to completion. See that file's own top comment for
# why this is scoped OUTSIDE docs/planning/batch-inference-coalescing-gap.md's standing "do not wire
# vendor batch mode into AgentSession without an ADR" directive: no AgentSession/StandingEffect/
# workflow dependency at all, so unlike agentengine_cli_chat above it needs no Python embed.
if(AGENTENGINE_WITH_HTTPS)
  add_executable(agentengine_batch_infer tools/batch_infer.cpp)
  target_link_libraries(agentengine_batch_infer PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# ---- Examples -------------------------------------------------------------------
# "Get started" examples (mirrors Microsoft Agent Framework's samples/01-get-started progression) --
# small, offline, single-file programs, one AgentEngine concept each. Needs only agentengine::core;
# no HTTPS/Python runner prerequisites, unlike agentengine_cli_chat above.
option(AGENTENGINE_BUILD_EXAMPLES "Build AgentEngine get-started examples" ON)
if(AGENTENGINE_BUILD_EXAMPLES)
  enable_testing()
  add_subdirectory(examples)
endif()

# ---- Tests --------------------------------------------------------------------
option(AGENTENGINE_BUILD_TESTS "Build AgentEngine tests" ON)
if(AGENTENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
