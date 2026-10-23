# ADR-244 §2.2 / G-C8 ("no backend by linkage, platform or null"): a host registers each SandboxBackend it chose;
# nothing in the engine, the tools or the examples may build a registry for it from whatever this build contained.
# `build_default_sandbox_registry` was that function, and it is deleted. This scan fails if the identifier is used
# again outside a comment (the scan root's own fixture, tests/fixtures/default_registry_scan/bad, is the control that
# proves it can fail).
#
# Run as: cmake -DSCAN_ROOTS="<dir>;<dir>" -P cmake/check_no_default_sandbox_registry.cmake
if(NOT SCAN_ROOTS)
  message(FATAL_ERROR "SCAN_ROOTS is required")
endif()

set(candidates "")
foreach(root IN LISTS SCAN_ROOTS)
  file(GLOB_RECURSE found "${root}/*.cpp" "${root}/*.hpp" "${root}/*.h" "${root}/CMakeLists.txt")
  list(APPEND candidates ${found})
endforeach()

set(violations "")
set(scanned 0)
foreach(f IN LISTS candidates)
  math(EXPR scanned "${scanned} + 1")
  file(STRINGS "${f}" hits REGEX "build_default_sandbox_registry")
  foreach(line IN LISTS hits)
    # A mention in a comment (// or #) is documentation of the removal, not a use.
    if(NOT line MATCHES "^[ \t]*(//|#|\\*)")
      list(APPEND violations "${f}: ${line}")
    endif()
  endforeach()
endforeach()

if(violations)
  list(JOIN violations "\n  " joined)
  message(FATAL_ERROR "build_default_sandbox_registry is used again (ADR-244 G-C8: the host registers its backends):\n  ${joined}")
endif()
if(scanned EQUAL 0)
  message(FATAL_ERROR "check_no_default_sandbox_registry scanned no file -- the scan matched nothing")
endif()
message(STATUS "check_no_default_sandbox_registry: ok (${scanned} files)")
