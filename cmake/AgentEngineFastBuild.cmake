# AGENTENGINE_FAST_TESTS (issue #115 E4, #120 S2): build a directory's targets without optimization.
#
# Most of a cold build is the optimizer, and it is spent in the code that instantiates the engine's
# header-only templates: tests/, examples/ and the test tools. The worst test TU
# (test_delegation_provenance) measured 130-180 CPU-s under MSVC Release and 25.4 s under /Od.
# Library targets keep their optimized flags, and the build type is unchanged (Release semantics
# otherwise). The cost is coverage: a bug that only MSVC's optimizer exposes in engine templates is
# not seen in a fast build. CI keeps that coverage on the Windows / MSVC / Release leg, which leaves
# the option OFF; the MSVC ASan leg turns it on (.github/workflows/ci.yml).
#
# A macro, not a function: it sets the calling directory's flag variables, which are read at the end
# of that directory for every target in it (and in its subdirectories).
#
# The optimization flags are stripped from the flag variables rather than overridden with a later
# /Od, for the same reason as tests/CMakeLists.txt's NDEBUG block: `/O2 ... /Od` makes cl emit D9025
# on every TU.
macro(agentengine_fast_build_this_directory)
  if(AGENTENGINE_FAST_TESTS)
    foreach(ae_fast_lang C CXX)
      foreach(ae_fast_var CMAKE_${ae_fast_lang}_FLAGS
                          CMAKE_${ae_fast_lang}_FLAGS_RELEASE
                          CMAKE_${ae_fast_lang}_FLAGS_RELWITHDEBINFO
                          CMAKE_${ae_fast_lang}_FLAGS_MINSIZEREL)
        # Every /O... or -O... token: /O2 /Ob2 /Oi /Oy- (cl, clang-cl), -O1 -O3 -Os -Ofast (gcc, clang).
        string(REGEX REPLACE "(^| )[/-]O[^ ]*" "" ${ae_fast_var} "${${ae_fast_var}}")
      endforeach()
    endforeach()
    if(MSVC)
      add_compile_options(/Od)
    else()
      add_compile_options(-O0)
    endif()
    file(RELATIVE_PATH ae_fast_dir "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    message(STATUS "AGENTENGINE_FAST_TESTS: ${ae_fast_dir}/ compiled without optimization")
  endif()
endmacro()
