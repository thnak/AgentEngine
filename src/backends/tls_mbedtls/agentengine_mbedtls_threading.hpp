#pragma once
// decisions/archive/ADR-237-async-extension-points-and-io-reactor.md §6.3: the implementation behind
// threading_alt.h, compiled into every translation unit that includes an mbedTLS header -- this file is
// pulled in by the mbedTLS user-config header (agentengine_mbedtls_config.h, set as
// MBEDTLS_USER_CONFIG_FILE in the root CMakeLists.txt), so a TU cannot use mbedTLS and miss it. It is
// C++-only: mbedTLS's own C sources see only the config macros and threading_alt.h.
//
// WHY THIS EXISTS. mbedTLS 3.6's TLS 1.3 keeps its keys in PSA's process-global store, and several other
// tables are filled lazily on first use; without MBEDTLS_THREADING_C two threads inside mbedTLS race
// (ADR-237 §6.3, TSan). MBEDTLS_THREADING_C makes mbedTLS lock its own globals, but it needs a concrete
// mutex implementation: MBEDTLS_THREADING_ALT + threading_alt.h + this std::mutex layer (MBEDTLS_THREADING_PTHREAD
// is POSIX-only and this tree is Windows + Linux). Every mbedTLS user in the process -- the blocking
// sandbox::TlsClientSession, rt::TlsStream, and any test's loopback peer -- then shares one thread-safe
// mbedTLS without a lock of their own.
//
// ensure_threading() is idempotent and must run before the first mbedTLS call. Two triggers cover every
// user: this header's autostart object (below) runs it before main for any TU that includes mbedTLS, and
// mbedtls_client::Context's constructor calls it again -- a no-op once armed, but it keeps the choke point
// named where the TLS clients are read.

#include "threading_alt.h"

#include <mutex>
#include <new>

// Declared by hand rather than by including <mbedtls/threading.h> / <mbedtls/ssl_ciphersuites.h>: this
// header is reached from inside build_info.h (the user-config include), and re-entering mbedTLS's headers
// mid-configuration is avoidable fragility. Both symbols have C linkage in mbedcrypto.
extern "C" void mbedtls_threading_set_alt(void (*mutex_init)(mbedtls_threading_mutex_t *),
                                          void (*mutex_free)(mbedtls_threading_mutex_t *),
                                          int (*mutex_lock)(mbedtls_threading_mutex_t *),
                                          int (*mutex_unlock)(mbedtls_threading_mutex_t *));
extern "C" int const *mbedtls_ssl_list_ciphersuites(void);

namespace agentengine::detail::mbedtls_client {

namespace threading_detail {

inline void mutex_init(mbedtls_threading_mutex_t *m) noexcept {
    if (m != nullptr) m->handle = new (std::nothrow) std::mutex();
}
inline void mutex_free(mbedtls_threading_mutex_t *m) noexcept {
    if (m == nullptr) return;
    delete static_cast<std::mutex *>(m->handle);
    m->handle = nullptr;
}
// -1 is any nonzero mbedTLS threading error; it can only happen if a mbedTLS call ran before
// ensure_threading(), which the autostart prevents.
inline int mutex_lock(mbedtls_threading_mutex_t *m) noexcept {
    if (m == nullptr || m->handle == nullptr) return -1;
    static_cast<std::mutex *>(m->handle)->lock();
    return 0;
}
inline int mutex_unlock(mbedtls_threading_mutex_t *m) noexcept {
    if (m == nullptr || m->handle == nullptr) return -1;
    static_cast<std::mutex *>(m->handle)->unlock();
    return 0;
}

}  // namespace threading_detail

// Safe to call from any thread, any number of times. Every global mbedTLS mutex is armed once, and
// mbedTLS's one lazily-filled table reachable from the TLS client path -- the ciphersuite list, which
// mbedtls_ssl_config_defaults builds on first use and MBEDTLS_THREADING_C does NOT guard
// (library/ssl_ciphersuites.c builds `supported_ciphersuites` under a plain `static int supported_init`)
// -- is built here, single-threaded, so a later concurrent first handshake cannot race it.
inline void ensure_threading() {
    static std::once_flag once;
    std::call_once(once, [] {
        mbedtls_threading_set_alt(&threading_detail::mutex_init, &threading_detail::mutex_free,
                                  &threading_detail::mutex_lock, &threading_detail::mutex_unlock);
        (void)mbedtls_ssl_list_ciphersuites();
    });
}

namespace threading_detail {

// Runs ensure_threading() before main for every TU that includes any mbedTLS header. Internal linkage
// (const at namespace scope) means one instance per such TU; the call is idempotent, so the cost is a
// guarded no-op. This is what lets a test's raw-mbedTLS loopback peer, or the blocking client, use mbedTLS
// without remembering to arm it.
struct autostart {
    autostart() { ensure_threading(); }
};
[[maybe_unused]] const autostart autostart_instance;

}  // namespace threading_detail

}  // namespace agentengine::detail::mbedtls_client
