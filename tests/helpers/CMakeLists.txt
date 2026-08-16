# The deliberately-hostile child process the Job Object / AppContainer tests launch and try to
# contain (ADR-004). It lives in its own directory scope for one reason: **it must not be built with
# sanitizer instrumentation**, and CMAKE_CXX_FLAGS is global.
#
# Why it must not be. A sanitizer-instrumented binary imports the sanitizer runtime DLL --
# confirmed by dumpbin on a local /fsanitize=address build:
#
#     job_object_hostile_child.exe -> WS2_32.dll, KERNEL32.dll, MSVCP140.dll,
#                                      clang_rt.asan_dynamic-x86_64.dll, ...
#
# That DLL lives under Program Files. The whole point of this child is that it is launched inside an
# AppContainer holding ZERO capabilities, which by construction has no ACL grant there. So the child
# cannot load its own runtime, never reaches main, and produces no output -- while `create()`
# succeeds, because the jail itself is fine. That is exactly the failure signature seen in CI:
#
#     ok:   C2: create() succeeds given a real mount and ResourceLimits
#     FAIL: C2: a well-behaved exec reports ok
#     FAIL: C2: exec() captures the child's real stdout      <- "SLEEP_DONE" never appears
#
# Reproduced locally under MSVC /fsanitize=address (5 failures, identical to CI), and seen in CI
# under BOTH sanitizer toolchains (MSVC ASan and clang-cl ASanUBSan) while passing in every
# non-sanitizer build. Six tests were affected.
#
# The fix is NOT to grant the AppContainer access to the runtime directory. These tests exist to
# prove containment; widening the sandbox's ACLs to make its own proof pass would hollow out the
# thing being proved.
#
# Building the fixture uninstrumented is also more correct on its own terms, independently of the
# loader problem. ASan replaces the allocator and adds large redzones, so an instrumented child
# changes the very memory behaviour `exceeding memory_bytes reports oom` is measuring. We are not
# hunting memory bugs in a fixture whose job is to fork-bomb, spin and OOM on purpose -- we are
# measuring whether the jail contains it. The product code under test remains fully instrumented;
# only the adversary does not.
foreach(ae_lang C CXX)
  foreach(ae_var CMAKE_${ae_lang}_FLAGS
                  CMAKE_${ae_lang}_FLAGS_RELEASE
                  CMAKE_${ae_lang}_FLAGS_RELWITHDEBINFO
                  CMAKE_${ae_lang}_FLAGS_DEBUG
                  CMAKE_${ae_lang}_FLAGS_MINSIZEREL)
    string(REGEX REPLACE "[-/]fsanitize=[A-Za-z,]+" "" ${ae_var} "${${ae_var}}")
    string(REGEX REPLACE "[-/]fno-sanitize-recover=[A-Za-z,]+" "" ${ae_var} "${${ae_var}}")
    string(REGEX REPLACE "[-/]fno-sanitize=[A-Za-z,]+" "" ${ae_var} "${${ae_var}}")
  endforeach()
endforeach()
foreach(ae_var CMAKE_EXE_LINKER_FLAGS
                CMAKE_EXE_LINKER_FLAGS_RELEASE
                CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
                CMAKE_EXE_LINKER_FLAGS_DEBUG)
  string(REGEX REPLACE "[-/]fsanitize=[A-Za-z,]+" "" ${ae_var} "${${ae_var}}")
endforeach()

# ws2_32: C5's probe_net mode uses raw Winsock.
add_executable(job_object_hostile_child hostile_child.cpp)
target_link_libraries(job_object_hostile_child PRIVATE ws2_32)
