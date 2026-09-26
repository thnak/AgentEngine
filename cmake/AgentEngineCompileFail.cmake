# Compile-fail gates for tests/compile_fail/ (issue #120 S9).
#
# A compile-fail gate proves that some code shape the engine forbids -- an implicit Tainted<T>
# conversion (I3), a directly constructed CapabilitySet (I2), a mismatched workflow edge, ... -- does
# NOT compile. Each gate is a pair: the NEGATIVE file must be rejected by the compiler, and a POSITIVE
# CONTROL of the same shape must compile and link, because a fail-only check cannot tell "correctly
# rejected" from "nothing here compiles at all".
#
# These used to be configure-time try_compile() calls. That cost ~48 s of every fresh configure (they
# ran serially), and their results were cached in CMakeCache.txt, so an existing build directory never
# re-ran them when a header changed: a header edit that broke a security gate went unnoticed until
# someone configured from scratch. Now:
#   - a positive control is an ordinary executable in the ALL build (agentengine_compile_fail_control).
#     If it stops compiling or linking, the build fails -- what configure's FATAL_ERROR used to do.
#   - a negative is an EXCLUDE_FROM_ALL executable plus a ctest entry that builds it and passes only if
#     the COMPILER rejected it (agentengine_compile_fail_test). Both re-run whenever a header changes,
#     because they are ordinary build steps with ordinary dependency tracking.
#
# ---- Failing for the right reason (ADR-096 C2, ADR-195 §3.8) -------------------------------------
# try_compile() ran in an isolated project that could not see this build's targets, so the gates that
# need compiled code listed the library sources by hand -- deliberately for the negatives too, so that
# "a permanently-missing link dependency can never masquerade as correctly rejected." try_compile only
# knew "the build failed", so a missing dependency would otherwise have read as a pass. That property
# is kept, and made stronger, by four things together:
#   1. The negative links the SAME real libraries as its positive control (agentengine::worktree_store,
#      agentengine::mediated_shell_runner, ...), and is an executable, so it is linked the same way too.
#   2. The negative's test requires a ctest fixture whose setup test builds the positive control
#      (CONTROL). ctest runs it first, and if it fails the negative is not run and counts as failed. So
#      when the negative's build starts, every library it depends on is already built, and the only
#      step left that can fail is the negative's own translation unit (and, if that compiled, its link).
#   3. PASS_REGULAR_EXPRESSION requires a compiler diagnostic that names the rejection itself, on the
#      diagnostic's own line: `error( C[0-9]+)?:[^\n]*<EXPECT>`. cl prints `error C2664: ...`, clang-cl,
#      gcc and clang print `error: ...`. EXPECT is the static_assert message where the gate is a
#      static_assert, and otherwise the name of the type or function being rejected.
#   4. FAIL_REGULAR_EXPRESSION rejects a link step or a missing file, whatever else the output says:
#      the negative reaching `Linking CXX executable` (it compiled), linker diagnostics (LNKnnnn,
#      `undefined reference`, `ld returned`, `ld: `), and a missing source or header (C1083,
#      `cannot open source/include file`, `No such file or directory`). Also a warning promoted to an
#      error (C2220, gcc's `[-Werror=...]` tag), though the negative is built without agentengine_warnings.
# A PASS regex ignores the exit code, which is why (4) matches the link step: a negative that compiled
# fails even if some unrelated line of the output happened to look like a diagnostic.
#
# ---- Flags -------------------------------------------------------------------------------------------
# try_compile() used the parent's CMAKE_CXX_FLAGS and its own default-configuration flags, and none of
# the directory's compile options. These targets are built with tests/'s flags: the build type's flags
# with NDEBUG stripped, /Od or -O0 under AGENTENGINE_FAST_TESTS, /bigobj and /EHsc. No gate's verdict
# depends on them: every rejection is an overload-resolution, access-control, deleted-function,
# constraint or static_assert error, all decided before optimization, and none of the headers involved
# tests NDEBUG (include/ and src/ have no `NDEBUG` at all). The positive controls additionally link
# agentengine_warnings (/W4 /WX, -Wall -Wextra -Werror) like every other test target; the negatives do
# not, so a warning can never be what rejects them.
#
# ---- Running --------------------------------------------------------------------------------------
# `ctest -L compile_fail` runs them all. Each runs `cmake --build` in this build directory, so they hold
# RESOURCE_LOCK agentengine_build_dir: two of them never run the build tool here at once. Run them after
# the build, as CI does; the fixture setup builds what they need if it is stale.

# agentengine_compile_fail_control(<target> SOURCE <src> [LIBS <lib>...] [INCLUDE_DIRS <dir>...])
#
# A positive control: an ordinary executable, built by ALL, never run. LIBS are linked PRIVATE after
# agentengine_warnings. The caller keeps a comment saying what it proves.
function(agentengine_compile_fail_control target)
  cmake_parse_arguments(PARSE_ARGV 1 AE_CF "" "SOURCE" "LIBS;INCLUDE_DIRS")
  if(AE_CF_UNPARSED_ARGUMENTS OR NOT AE_CF_SOURCE)
    message(FATAL_ERROR "agentengine_compile_fail_control(${target}): needs SOURCE, got: ${ARGN}")
  endif()
  add_executable(${target} ${AE_CF_SOURCE})
  target_link_libraries(${target} PRIVATE agentengine_warnings ${AE_CF_LIBS})
  if(AE_CF_INCLUDE_DIRS)
    target_include_directories(${target} PRIVATE ${AE_CF_INCLUDE_DIRS})
  endif()
endfunction()

# agentengine_compile_fail_test(<name> SOURCE <src> CONTROL <target> EXPECT <regex>
#                               [LIBS <lib>...] [INCLUDE_DIRS <dir>...] [TIMEOUT <seconds>])
#
# A negative gate: the EXCLUDE_FROM_ALL executable compile_fail_<name> built from SOURCE, and the ctest
# compile_fail_<name>, which builds it and passes only if the compiler rejects it for the expected
# reason (see "Failing for the right reason" above).
#   SOURCE        the file that must NOT compile.
#   CONTROL       the target proving the same shape compiles and links: the gate's positive control, or
#                 the runtime test that serves as one. Built by the fixture setup test
#                 compile_fail_control_<CONTROL minus any compile_fail_ prefix>, which this test
#                 requires.
#   EXPECT        regex that must follow `error:` / `error Cnnnn:` on the same line (grouped, so an
#                 alternation is fine: compilers word the same rejection differently).
#   LIBS          linked PRIVATE: the same libraries as the positive control.
#   INCLUDE_DIRS  target_include_directories(... PRIVATE ...).
#   TIMEOUT       ctest TIMEOUT, default 600 (one cold compile of a session-heavy TU under ASan).
function(agentengine_compile_fail_test name)
  cmake_parse_arguments(PARSE_ARGV 1 AE_CF "" "SOURCE;CONTROL;EXPECT;TIMEOUT" "LIBS;INCLUDE_DIRS")
  if(AE_CF_UNPARSED_ARGUMENTS OR NOT AE_CF_SOURCE OR NOT AE_CF_CONTROL OR NOT AE_CF_EXPECT)
    message(FATAL_ERROR "agentengine_compile_fail_test(${name}): needs SOURCE, CONTROL and EXPECT, got: ${ARGN}")
  endif()
  if(NOT TARGET ${AE_CF_CONTROL})
    message(FATAL_ERROR "agentengine_compile_fail_test(${name}): CONTROL ${AE_CF_CONTROL} is not a target")
  endif()
  if(NOT DEFINED AE_CF_TIMEOUT)
    set(AE_CF_TIMEOUT 600)
  endif()

  set(target compile_fail_${name})
  add_executable(${target} EXCLUDE_FROM_ALL ${AE_CF_SOURCE})
  if(AE_CF_LIBS)
    target_link_libraries(${target} PRIVATE ${AE_CF_LIBS})
  endif()
  if(AE_CF_INCLUDE_DIRS)
    target_include_directories(${target} PRIVATE ${AE_CF_INCLUDE_DIRS})
  endif()

  get_property(ae_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(ae_multi_config)
    set(ae_config_args --config $<CONFIG>)
  else()
    set(ae_config_args)
  endif()

  # One setup test per control, shared by every negative that names it.
  string(REGEX REPLACE "^compile_fail_" "" ae_control_short "${AE_CF_CONTROL}")
  set(ae_fixture compile_fail_control_${ae_control_short})
  get_property(ae_have_setup GLOBAL PROPERTY AE_COMPILE_FAIL_SETUP_${AE_CF_CONTROL})
  if(NOT ae_have_setup)
    set_property(GLOBAL PROPERTY AE_COMPILE_FAIL_SETUP_${AE_CF_CONTROL} TRUE)
    add_test(NAME ${ae_fixture}
      COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${AE_CF_CONTROL} ${ae_config_args})
    set_tests_properties(${ae_fixture} PROPERTIES
      FIXTURES_SETUP ${ae_fixture}
      RESOURCE_LOCK agentengine_build_dir
      TIMEOUT ${AE_CF_TIMEOUT}
      LABELS compile_fail)
  endif()

  add_test(NAME ${target}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${target} ${ae_config_args})
  set_tests_properties(${target} PROPERTIES
    PASS_REGULAR_EXPRESSION "error( C[0-9]+)?:[^\n]*(${AE_CF_EXPECT})"
    FAIL_REGULAR_EXPRESSION
      "Linking CXX executable;LNK[0-9]+;undefined reference;ld returned;ld: ;cannot open (source|include) file;No such file or directory;C1083;C2220;\\[-Werror"
    FIXTURES_REQUIRED ${ae_fixture}
    RESOURCE_LOCK agentengine_build_dir
    TIMEOUT ${AE_CF_TIMEOUT}
    LABELS compile_fail)
endfunction()
