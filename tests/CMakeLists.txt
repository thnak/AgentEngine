# Correctness gate (CONVENTIONS.md: "tests/ — correctness gate, one per load-bearing invariant").
# smoke_vocabulary proves the abstract-vocabulary headers under include/agentengine/ actually
# compile together and that the declared concepts (ChatClient, Runner, SandboxBackend) are
# satisfiable by a conforming type.

# Every test target: AgentSession-heavy test TUs exceed MSVC's default object-file section limit under ASan
# (C1128; PR #103 CI). /bigobj only changes the object format, so it is safe to apply to all of them.
if(MSVC)
  add_compile_options(/bigobj)
endif()

# ---- Compile-fail gates: tests/compile_fail/ (issue #120 S9) ---------------------------------
# The compile-fail proofs (007 §9 G1/G2, ADR-009, ADR-012, ADR-071, ADR-096 C2, ADR-102 §3 C1,
# ADR-158, ADR-176 §14, ADR-195 §3.8, 014 §1, Milestone 3 A3) used to be configure-time try_compile()
# calls here. They are now build targets and ctest entries, registered by the compile_fail folder
# below: positive controls build with ALL, negatives run as `ctest -L compile_fail`
# (cmake/AgentEngineCompileFail.cmake).

# ---- Keep assert() live in every configuration, Release included --------------------------------
# 13 files here (195 assert() calls) use bare <cassert> rather than the AE_CHECK/g_failures harness:
# smoke_vocabulary, test_job_object_limits, test_native_jail_runner_stubs, test_python_audit_hook,
# test_python_caller_gated_{benchmark,import}, test_python_embed_smoke, test_python_layer0_sweep,
# test_python_meta_path_finder, test_python_numpy_pandas_import,
# test_python_trusted_loader_proxy_heap_type_negative_control, test_real_filesystem_adapter,
# test_recorded_chat_client.
#
# CMake's Release/RelWithDebInfo flags carry -DNDEBUG, which turns every one of those into a no-op --
# so the CI jobs that build Release (Windows/MSVC, Windows/clang-cl, Linux/gcc-14) were running
# bodies that cannot fail. That is the exact thing CLAUDE.md forbids ("a test that cannot fail proves
# nothing"), and it covered real security proofs: the caller-gated import gate (45 asserts), the
# audit hook, the Job-object limits, and a file whose name is literally
# ..._negative_control.cpp. gcc's -Wunused-variable is what exposed it -- four locals in
# test_recorded_chat_client.cpp had no remaining reader once the asserts were preprocessed away.
#
# Dropping NDEBUG for tests/ only is the narrow fix: production targets keep their Release semantics
# untouched, and the assertions become load-bearing everywhere they are built.
#
# The define is STRIPPED from this directory's config flags rather than countered with a
# `/UNDEBUG`-`-UNDEBUG` compile option. Both undefine it, but appending the counter-flag makes cl
# itself emit `command line warning D9025: overriding '/DNDEBUG' with '/UNDEBUG'` on every single
# translation unit -- trading 195 dead assertions for a few thousand lines of new warning, when the
# rule here is that warnings get fixed rather than accumulated. Editing the variables at directory
# scope leaves no contradictory pair on the command line for cl to complain about.
# Three places NDEBUG can arrive from, and an earlier version of this block only handled the first:
#   1. CMAKE_<LANG>_FLAGS_<CONFIG>          -- CMake's own Release/RelWithDebInfo/MinSizeRel defaults
#   2. CMAKE_<LANG>_FLAGS                   -- seeded by the CXXFLAGS/CFLAGS environment variables and
#                                              by -DCMAKE_CXX_FLAGS=..., both entirely ordinary in a
#                                              packaging or toolchain-file build
#   3. the directory COMPILE_DEFINITIONS property -- an add_compile_definitions(NDEBUG) above us
# Missing (2) meant a perfectly standard configuration silently restored all 195 dead assertions,
# which is precisely the failure this block exists to prevent, so all three are handled.
foreach(ae_lang C CXX)
  foreach(ae_flags_var CMAKE_${ae_lang}_FLAGS
                        CMAKE_${ae_lang}_FLAGS_RELEASE
                        CMAKE_${ae_lang}_FLAGS_RELWITHDEBINFO
                        CMAKE_${ae_lang}_FLAGS_MINSIZEREL)
    string(REPLACE "/DNDEBUG" "" ${ae_flags_var} "${${ae_flags_var}}")
    string(REPLACE "-DNDEBUG" "" ${ae_flags_var} "${${ae_flags_var}}")
  endforeach()
endforeach()
get_directory_property(ae_dir_defs COMPILE_DEFINITIONS)
if(ae_dir_defs)
  list(REMOVE_ITEM ae_dir_defs NDEBUG)
  set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${ae_dir_defs}")
endif()

# AGENTENGINE_FAST_TESTS: optimization off for every test target (cmake/AgentEngineFastBuild.cmake).
agentengine_fast_build_this_directory()

# ---- Test folders (issue #120 S11) -------------------------------------------------------------
# The tests live in subfolders that mirror include/agentengine/ and src/. Each folder has its own
# CMakeLists.txt, added below by agentengine_add_test_folder() (cmake/AgentEngineTests.cmake), which
# also gives every test in it the folder's name as a ctest label (`ctest -L rt/agent_session`) and
# keeps its working directory at this directory's binary dir, where it was before the split.
#
# Everything above that edits directory-scope flags (the NDEBUG strip, AGENTENGINE_FAST_TESTS,
# /bigobj) must stay above the add_subdirectory() calls: a subdirectory copies its parent's variables
# and directory properties when it is added, not later.
include(AgentEngineTests)
set(AE_TESTS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- nlohmann/json, a test-only dependency ------------------------------------------------------
# First used by test_recorded_chat_client (core/chat/). Declared here, before the folders are
# added, so any folder's tests can link nlohmann_json::nlohmann_json. It is test-only per
# CONVENTIONS.md's dependency-tier discipline: FetchContent-ed here (not in the root CMakeLists.txt)
# and linked only to test targets — never to agentengine_core or anything under
# include/agentengine/core.
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

if(WIN32)
  # Deliberately hostile test-only helper process (tests/helpers/hostile_child.cpp) the Job Object
  # tests launch and try to contain -- never a product target, never linked into anything else.
  #
  # Defined in its own directory scope (helpers/CMakeLists.txt) rather than here, because it must be
  # built WITHOUT sanitizer instrumentation and CMAKE_CXX_FLAGS is global. An instrumented child
  # imports the sanitizer runtime DLL from Program Files, which a zero-capability AppContainer has no
  # ACL grant for -- so it never reaches main and six containment tests fail while create() succeeds.
  # That subdirectory carries the full reasoning, including why granting the ACL would be the wrong
  # fix.
  add_subdirectory(helpers)
endif()

# Every test executable is still written to <build>/tests/, as it was when this was one directory:
# CI, scripts and people run them from there. Set after add_subdirectory(helpers) above, whose
# hostile-child executables keep their own <build>/tests/helpers/ location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

foreach(ae_test_folder IN ITEMS
  rt
  rt/project
  rt/agent_spawn
  rt/agent_session
  workflow
  memory
  eval
  core/rag
  core/json
  core/ledger
  core/chat
  core/context
  core/tools
  core/agent
  core/skills
  trust
  sandbox
  sandbox/execution_surface
  backends/kata
  backends/native_jail
  backends/native_process
  python
  tools
  plugin
  worktree
  protocol/mcp
  protocol/a2a
  protocol/agui
  protocol/openai
  protocol/anthropic
  testing
  # Last: its negatives name targets from the folders above as their positive controls.
  compile_fail)
  agentengine_add_test_folder(${ae_test_folder})
endforeach()

# ADR-182 §12 R11: engine code must not include agentengine/testing/.
add_test(NAME lint_testing_includes
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/check_testing_includes.cmake)

# ---- ADR-182 §16: one ctest per checked-in scenario, replayed with the scripted model (no network,
# no Claude). Scenarios are exported by the test driver (scenario_export), usually by a Claude tester
# sub-agent -- including from live-model runs, whose observed answers become the script.
file(GLOB AE_SCENARIO_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/scenarios/*.json)
foreach(scenario IN LISTS AE_SCENARIO_FILES)
  get_filename_component(scenario_name ${scenario} NAME_WE)
  add_test(NAME scenario_${scenario_name} COMMAND agentengine_scenario_runner
         --fixtures-root ${PROJECT_SOURCE_DIR}/tests/fixtures/test_driver ${scenario})
  set_tests_properties(scenario_${scenario_name} PROPERTIES LABELS "scenario" TIMEOUT 120)
endforeach()
