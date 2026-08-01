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
  # at test-run time. Auto-detected against this project's known dev-box target (ADR-002's
  # "concrete embedding target": a miniconda3 install with numpy/pandas preinstalled) if not given
  # explicitly; override with -DAGENTENGINE_PYTHON_ROOT=<path> to point at any other CPython
  # install (a plain CPython install, a venv with dev headers, etc).
  if(NOT AGENTENGINE_PYTHON_ROOT)
    if(WIN32 AND DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/miniconda3/include/Python.h")
      set(AGENTENGINE_PYTHON_ROOT "$ENV{USERPROFILE}/miniconda3")
    endif()
  endif()
  set(AGENTENGINE_PYTHON_ROOT "${AGENTENGINE_PYTHON_ROOT}" CACHE PATH
    "Root of the CPython install/venv to embed (must contain include/Python.h)")

  if(NOT AGENTENGINE_PYTHON_ROOT OR NOT EXISTS "${AGENTENGINE_PYTHON_ROOT}/include/Python.h")
    message(FATAL_ERROR
      "AGENTENGINE_BUILD_PYTHON_RUNNER is ON but AGENTENGINE_PYTHON_ROOT "
      "('${AGENTENGINE_PYTHON_ROOT}') does not contain include/Python.h. Pass "
      "-DAGENTENGINE_PYTHON_ROOT=<path to a CPython install/venv root>, e.g. a miniconda3 prefix.")
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
  set(AGENTENGINE_PYTHON_SITE_PACKAGES "${AGENTENGINE_PYTHON_ROOT}/Lib/site-packages" CACHE PATH
    "Curated site-packages directory added to sys.path (010 §5 'preinstalled' policy stand-in)" FORCE)
  set(AGENTENGINE_PYTHON_DLL_DIR "${AGENTENGINE_PYTHON_ROOT}" CACHE PATH
    "Directory containing python3<minor>.dll, prepended to PATH for tests" FORCE)
endif()

# ---- Tests --------------------------------------------------------------------
option(AGENTENGINE_BUILD_TESTS "Build AgentEngine tests" ON)
if(AGENTENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
