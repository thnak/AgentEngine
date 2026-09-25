# ADR-182 §12 R11: test-only code must not leak into the engine. Fails if any file under src/ or any
# public header outside include/agentengine/testing/ includes "agentengine/testing/...".
# Run as: cmake -DSOURCE_DIR=<repo root> -P cmake/check_testing_includes.cmake
if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE candidates
  "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.hpp" "${SOURCE_DIR}/src/*.h"
  "${SOURCE_DIR}/include/agentengine/*.hpp")

set(violations "")
foreach(f IN LISTS candidates)
  if(f MATCHES "/include/agentengine/testing/")
    continue()
  endif()
  file(STRINGS "${f}" hits REGEX "#[ \t]*include[ \t]*[<\"]agentengine/testing/")
  if(hits)
    list(APPEND violations "${f}")
  endif()
endforeach()

if(violations)
  list(JOIN violations "\n  " joined)
  message(FATAL_ERROR "agentengine/testing/ included from engine code (ADR-182 §12 R11):\n  ${joined}")
endif()
message(STATUS "check_testing_includes: ok")
