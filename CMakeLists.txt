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

# ---- Quark (submodule; never forked or patched in-tree, CLAUDE.md) -----------
# Force Quark's own test/bench suite off before pulling it in as a subdirectory — AgentEngine's
# build must not drag in Quark's CTest targets or benchmarks.
set(QUARK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_BENCH OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/quark)

# ---- Core vocabulary (header-only for now; no .cpp under include/agentengine) --
add_library(agentengine_core INTERFACE)
target_include_directories(agentengine_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(agentengine_core INTERFACE quark::quark)
target_compile_features(agentengine_core INTERFACE cxx_std_23)
add_library(agentengine::core ALIAS agentengine_core)

# ---- Egress proxy (src/sandbox/) -- Windows + Linux ---------------------------------------------
# Implements decisions/ADR-011-first-party-egress-proxy.md (008 §10 Q3, M2 Phase F task F1): the
# host-mediated mechanism NetOut (007 §3) is enforced through. Ordinary OS socket/DNS APIs
# (getaddrinfo/inet_pton/select) plus third_party/quark's already-vendored, cross-platform PAL socket
# primitives (quark::quark, pulled in transitively via agentengine::core) -- no new third-party
# dependency, so this needs no CMake option gate beyond the platform-independent build itself (ws2_32
# is already a PUBLIC link of the quark target on Windows).
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

# ---- native_jail ShellRunner backend (src/backends/native_jail/) --------------
# Implements decisions/ADR-001-shellrunner-grammar-and-dispatch.md — Design A: the
# recursive-descent parser, the pmr-backed AST, the tree-walking evaluator, the fixed builtin
# table, CommandRegistry, and RealFileSystemAdapter. A STATIC library (not header-only) so Sh-S1's
# static "zero references to a process-creation primitive" check (ADR-001 §7 finding 1's fix) can
# be stated at link-target granularity: this library links ONLY against agentengine::core (header
# -only vocabulary + Quark) and the C++ standard library — nothing here can pull in an object that
# defines CreateProcess/_wspawnv/system/LoadLibrary/posix_spawn/exec*/fork by construction of what
# it's linked against, which tests/test_shell_runner_proof.cpp verifies against the actual built
# artifact, not against prose.
add_library(agentengine_shell_runner STATIC
  src/backends/native_jail/shell_parser.cpp
  src/backends/native_jail/shell_dispatch.cpp
  src/backends/native_jail/real_filesystem_adapter.cpp)
target_include_directories(agentengine_shell_runner PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agentengine_shell_runner PUBLIC agentengine::core)
target_link_libraries(agentengine_shell_runner PRIVATE agentengine_warnings)
add_library(agentengine::shell_runner ALIAS agentengine_shell_runner)

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
    src/backends/native_jail/native_jail_backend.cpp)
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

# ---- wasmtime C API import (src/backends/wasm/) -- Windows + Linux -----------------------------
# Milestone 2 Phase D task D1 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md,
# decision 5): wasmtime 47.0.3 as the `wasm` SandboxBackend's one CONVENTIONS.md tier-2 seam-backend
# heavy dependency (009's plugin ABI). This block ONLY imports the dependency and proves it links
# and runs (tests/test_wasmtime_smoke.cpp, gated the same way below) -- no SandboxBackend
# implementation exists under src/backends/wasm/ yet (still README-only by design: that's D3,
# security-critical, goes through design->red-team->prove->judge per CLAUDE.md before it is real
# code, same posture as job_object_limits.hpp/cgroup_limits.hpp before ADR-004's build-and-measure
# pass). Off by default, matching AGENTENGINE_BUILD_PYTHON_RUNNER's own posture for the other
# tier-2 heavy dependency this project has.
option(AGENTENGINE_WITH_WASM
  "Import the wasmtime C API (009's plugin ABI dependency) -- no SandboxBackend implementation yet, see D3"
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
  # own AgentEngineWarnings module should be answerable for -- both forced off, matching
  # QUARK_BUILD_TESTS/QUARK_BUILD_BENCH's own precedent for Quark above.
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
  # whatever the build machine happens to have installed. AgentEngine now owns this file's rotation
  # over time (ADR-013 section 9's named residual) -- bundle snapshot dated 2026-07-16 (curl.se's
  # own header line), fetched and hand-verified during this task.
  option(AGENTENGINE_VENDOR_CA_BUNDLE
    "Fetch a pinned, checksummed CA root bundle instead of using a local PEM file"
    ON)
  if(AGENTENGINE_VENDOR_CA_BUNDLE)
    set(AE_CA_BUNDLE_PATH "${CMAKE_CURRENT_BINARY_DIR}/vendored_ca_bundle/cacert.pem")
    file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${AE_CA_BUNDLE_PATH}"
      EXPECTED_HASH SHA256=3ff344e30b9b1ed2971044eabb438a08f2e2245ddb5f8ab1a3ad8b63ab4eaf91)
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
endif()

# ---- Cross-process capability tokens (src/trust/) -- Windows only ------------------------------
# Implements decisions/ADR-005-capability-bearer-tokens-cross-process.md (resolves OQ-3): both
# competing designs it prove-phases -- Design A (trust/capability_token.hpp, a self-verifying
# macaroon-style bearer token) and Design B (trust/capability_registry.hpp, the status-quo
# host-side-registry alternative it is measured against). HMAC-SHA256 and randomness come from
# Windows CNG/BCrypt, a system API (bcrypt.lib) -- not a third-party dependency, same posture as
# job_object_limits.cpp's direct use of Job Objects, so this needs no CMake option gate beyond the
# platform check.
if(WIN32)
  add_library(agentengine_capability_token STATIC
    src/trust/capability_token.cpp
    src/trust/capability_registry.cpp)
  target_link_libraries(agentengine_capability_token PUBLIC agentengine::core)
  target_link_libraries(agentengine_capability_token PRIVATE agentengine_warnings bcrypt)
  add_library(agentengine::capability_token ALIAS agentengine_capability_token)
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

# ---- Tests --------------------------------------------------------------------
option(AGENTENGINE_BUILD_TESTS "Build AgentEngine tests" ON)
if(AGENTENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
