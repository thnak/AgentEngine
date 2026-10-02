# AGENTENGINE_TEST_PCH: one precompiled header (tests/support/test_pch.hpp) shared by the plain test
# executables, to cut the parse/instantiate cost every test TU pays again for the same engine headers.
#
# Why now, when ADR-199 and ADR-200 set precompiled headers aside: those measured optimized builds, where
# code generation dominated and a PCH does not touch code generation. Under AGENTENGINE_FAST_TESTS the
# tests compile at /Od / -O0, and a clang -ftime-trace of that build (2026-10-02) put 85% of compile CPU in
# the frontend (913 of 1,077 CPU-s), which is exactly the part a PCH removes.
#
# Measured 2026-10-02, cold dev build at -j4, ON vs OFF:
#   - MSVC 4:33 -> 2:31 (test compile CPU 831 -> 351 s);
#   - clang 21: 205 -> 130 s;
#   - gcc 15: 258 -> 245 s. gcc's PCH is a 158 MB file loaded per TU and holds no instantiations, so
#     it saves ~35% per test TU but little overall.
# The trade-off: editing a header in the PCH rebuilds every reusing test, not just its includers. Measured
# on MSVC, a code edit to core/tool_pipeline.hpp rebuilds 356 TUs in 124 s, against 183 TUs in 165 s
# without the PCH, so it still comes out ahead.
#
# Build speed only. OFF by default; the `dev` preset turns it on. CI legs leave it OFF, so a test that
# leans on the PCH instead of including what it uses still fails there.
#
# Shared with REUSE_FROM, so it only applies to targets built with the PCH target's own flags. Skipped:
#   - targets with their own compile definitions (a macro the PCH was not built with: C4605 under /WX
#     on MSVC, a hard error on clang);
#   - the folders listed in AE_TEST_PCH_SKIP_FOLDERS: compile-fail gates (their verdict must not depend on
#     what a PCH dragged in), and folders whose TUs #define macros before their first #include.
option(AGENTENGINE_TEST_PCH "Share one precompiled header across the plain test executables (dev builds)" OFF)

set(AE_TEST_PCH_SKIP_FOLDERS compile_fail python experiments helpers)

# Called once from tests/CMakeLists.txt, after the directory-wide flags are final.
function(agentengine_define_test_pch)
  if(NOT AGENTENGINE_TEST_PCH)
    return()
  endif()
  add_library(agentengine_test_pch STATIC "${PROJECT_SOURCE_DIR}/tests/support/test_pch_anchor.cpp")
  target_link_libraries(agentengine_test_pch PRIVATE agentengine::core agentengine_warnings)
  target_precompile_headers(agentengine_test_pch PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/test_pch.hpp")
  message(STATUS "AGENTENGINE_TEST_PCH: plain test executables reuse tests/support/test_pch.hpp")
endfunction()

# Called by agentengine_add_test_folder() for each folder it adds.
function(agentengine_apply_test_pch folder)
  if(NOT AGENTENGINE_TEST_PCH)
    return()
  endif()
  foreach(skip IN LISTS AE_TEST_PCH_SKIP_FOLDERS)
    if(folder STREQUAL skip OR folder MATCHES "^${skip}/")
      return()
    endif()
  endforeach()
  get_property(targets DIRECTORY ${folder} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(t IN LISTS targets)
    get_target_property(type ${t} TYPE)
    get_target_property(defs ${t} COMPILE_DEFINITIONS)
    if(type STREQUAL "EXECUTABLE" AND NOT defs)
      target_precompile_headers(${t} REUSE_FROM agentengine_test_pch)
    endif()
  endforeach()
endfunction()
