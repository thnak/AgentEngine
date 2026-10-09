#ifndef AGENTENGINE_MBEDTLS_THREADING_ALT_H
#define AGENTENGINE_MBEDTLS_THREADING_ALT_H
// decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3: the mutex type mbedTLS's alternate
// threading layer (MBEDTLS_THREADING_C + MBEDTLS_THREADING_ALT) is built against. mbedTLS compiles this
// as C (library/threading.c), so the type must be plain C: a handle on the std::mutex the C++ side owns
// (agentengine_mbedtls_threading.hpp). threading.c zero-initialises its own static mutexes (MUTEX_INIT is
// empty under THREADING_ALT), i.e. `handle == NULL`; mbedtls_threading_set_alt() arms them before any
// mbedTLS call (mbedtls_client::ensure_threading, and the header's own autostart).
typedef struct mbedtls_threading_mutex_t {
    void *handle;
} mbedtls_threading_mutex_t;
#endif  // AGENTENGINE_MBEDTLS_THREADING_ALT_H
