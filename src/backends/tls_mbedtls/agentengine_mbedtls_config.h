#ifndef AGENTENGINE_MBEDTLS_CONFIG_H
#define AGENTENGINE_MBEDTLS_CONFIG_H
// decisions/archive/ADR-237-async-extension-points-and-io-reactor.md §6.3. The root CMakeLists.txt sets this file
// as MBEDTLS_USER_CONFIG_FILE on the vendored mbedTLS targets (PUBLIC, so every consumer compiles against
// the same configuration -- a mismatched TLS context would be an ABI bug). mbedTLS includes it right after
// its own mbedtls_config.h (include/mbedtls/build_info.h).
//
// MBEDTLS_THREADING_C turns on mbedTLS's internal locking; MBEDTLS_THREADING_ALT picks this project's own
// mutex layer instead of MBEDTLS_THREADING_PTHREAD (POSIX-only; this tree is Windows + Linux).
// threading_alt.h defines the type, agentengine_mbedtls_threading.hpp the implementation; both are found
// through the include directory the same CMake block adds.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

// mbedTLS 3.6.7 ships MBEDTLS_SELF_TEST ENABLED in its own mbedtls_config.h. Disable it: it is the
// library's own *self-test* harness, not a feature this project uses, and enabling it compiles three
// process-global counters into ecp.c (add_count/dbl_count/mul_count, guarded by `#if defined(MBEDTLS_SELF_TEST)`)
// that every EC point operation increments (INC_MUL_COUNT, library/ecp.c:1041). MBEDTLS_THREADING_C does
// NOT guard them, so two concurrent TLS 1.3 handshakes race on mul_count -- the "ECP" race ADR-237 §6.3
// named, which THREADING_C alone left in place (TSan: 777 reports, all `mbedtls_mpi_mul_mod`/mul_count,
// before this line). Undoing the upstream default removes the counters and the self-test entry points,
// which nothing in this tree calls; it is also strictly less work on the handshake path.
#undef MBEDTLS_SELF_TEST

#if defined(__cplusplus)
// Every C++ TU that includes an mbedTLS header arms the threading layer before main. mbedTLS's own C
// sources (library/*.c) see only the defines above and threading_alt.h.
#include "agentengine_mbedtls_threading.hpp"
#endif

#endif  // AGENTENGINE_MBEDTLS_CONFIG_H
