# AgentEngine — root build. See CONVENTIONS.md for the binding coding contract (layout, dependency
# tiers, machine-safety build rules) and AgentEngineSpecification.md for the design this implements.

cmake_minimum_required(VERSION 3.24)
project(AgentEngine LANGUAGES CXX)

# ---- Standard ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AgentEngineWarnings)

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
