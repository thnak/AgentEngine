# Test registration helpers for tests/ (issue #120 S11).
#
# tests/ is split into folders that mirror include/agentengine/ and src/, each with its own
# CMakeLists.txt. tests/CMakeLists.txt adds every folder through agentengine_add_test_folder(), and
# the folder files register most tests through agentengine_add_test().

# agentengine_add_test(<name> [SOURCES <src>...] [LIBS <lib>...] [DEFINITIONS <def>...]
#                      [TIMEOUT <seconds>] [LABELS <label>...])
#
# The plain test: one executable and one ctest entry, both named <name>.
#   SOURCES      defaults to <name>.cpp in the calling folder.
#   LIBS         linked PRIVATE, in the order given (agentengine_warnings is not implied).
#   DEFINITIONS  target_compile_definitions(... PRIVATE ...).
#   TIMEOUT      the ctest TIMEOUT property.
#   LABELS       extra ctest labels. The folder label is added by agentengine_add_test_folder().
# Anything else (include directories, dependencies, ENVIRONMENT, SKIP_RETURN_CODE, ...) is spelled
# out after the call, or the whole test is spelled out.
function(agentengine_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 AE_T "" "TIMEOUT" "SOURCES;LIBS;DEFINITIONS;LABELS")
  if(AE_T_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "agentengine_add_test(${name}): unexpected arguments: ${AE_T_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT AE_T_SOURCES)
    set(AE_T_SOURCES "${name}.cpp")
  endif()
  add_executable(${name} ${AE_T_SOURCES})
  if(AE_T_LIBS)
    target_link_libraries(${name} PRIVATE ${AE_T_LIBS})
  endif()
  if(AE_T_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${AE_T_DEFINITIONS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  if(DEFINED AE_T_TIMEOUT)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${AE_T_TIMEOUT})
  endif()
  if(AE_T_LABELS)
    set_property(TEST ${name} APPEND PROPERTY LABELS ${AE_T_LABELS})
  endif()
endfunction()

# agentengine_add_test_folder(<folder>)
#
# add_subdirectory(<folder>), then, for every test that folder registered however it was written:
#   - WORKING_DIRECTORY is the calling directory's binary dir (<build>/tests), where every test ran
#     before the split; add_test() would otherwise default it to <build>/tests/<folder>.
#   - the folder path ("rt", "core/tools", ...) is appended to LABELS, so `ctest -L core/tools` works.
# Set here rather than in each folder file so no test can miss them.
function(agentengine_add_test_folder folder)
  add_subdirectory(${folder})
  get_property(ae_folder_tests DIRECTORY ${folder} PROPERTY TESTS)
  if(ae_folder_tests)
    set_property(TEST ${ae_folder_tests} DIRECTORY ${folder}
      PROPERTY WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_property(TEST ${ae_folder_tests} DIRECTORY ${folder} APPEND PROPERTY LABELS "${folder}")
  endif()
endfunction()
