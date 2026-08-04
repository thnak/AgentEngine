# Compiler-version floor (021-Platform-Support-and-Portability.md §5: "CMake >= 3.28, C++23.
# Compilers: MSVC 19.4x, g++ 14+, clang 20+ (clang-cl on Windows)."). A build below the floor fails
# configure with a clear message instead of surfacing as a confusing C++23 feature-support error
# later in the build.
#
# clang-cl reports CMAKE_CXX_COMPILER_ID "Clang" with CMAKE_CXX_COMPILER_FRONTEND_VARIANT "MSVC" —
# it is still Clang for version-floor purposes, so it falls through to the Clang branch below, not
# the MSVC one (MSVC's own compiler id is "MSVC").

if(MSVC)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.40)
    message(FATAL_ERROR
      "MSVC ${CMAKE_CXX_COMPILER_VERSION} is below the 021 §5 floor (19.40, VS 2022 17.10+). "
      "Upgrade the toolchain.")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
    message(FATAL_ERROR
      "g++ ${CMAKE_CXX_COMPILER_VERSION} is below the 021 §5 floor (14.0+). Upgrade the toolchain.")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20.0)
    message(FATAL_ERROR
      "clang ${CMAKE_CXX_COMPILER_VERSION} is below the 021 §5 floor (20.0+). Upgrade the toolchain.")
  endif()
else()
  message(WARNING
    "Unrecognized compiler '${CMAKE_CXX_COMPILER_ID}' ${CMAKE_CXX_COMPILER_VERSION} — 021 §5 only "
    "names MSVC/g++/clang floors, so this build is proceeding unchecked.")
endif()
