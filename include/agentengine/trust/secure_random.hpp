#pragma once
// Implements 011-MCP-Conformance.md §12's "servers MUST generate [task ids] with sufficient
// entropy" and 011 §8a's "handles are high-entropy" clause, and 012-A2A-Conformance.md §4's
// equivalent for A2A task ids.
//
// Closes ADR-023 §7's finding R3: both `protocol/mcp/server.hpp` and `protocol/a2a/server.hpp`
// generated handles from a `std::random_device`-seeded `std::mt19937_64`. MT19937 is not a CSPRNG —
// its internal state is fully recoverable from 312 consecutive 64-bit outputs (156 task creations at
// two draws each), after which every future handle is predictable. A predictable server-minted
// handle is exactly the hazard 011 §8a's own text calls out, and it is a *bearer* value: with
// ADR-023 §7's R1 fix (principal-bound lookup) entropy is defense in depth rather than the sole
// control, but the spec asks for both and mt19937 delivers neither.
//
// Header-only, deliberately. `agentengine::core` is an INTERFACE target and both protocol servers
// are header-only, linking nothing else (`tests/CMakeLists.txt:940`, `:978`); routing this through
// `src/trust/` — which is `if(WIN32)`-gated in the root CMakeLists — would make the MCP and A2A
// server surfaces Windows-only, a portability regression 021's own matrix does not accept for a
// pure-protocol header. Both branches use a system CSPRNG, not a third-party dependency: the same
// posture `src/trust/capability_token.cpp` and `job_object_limits.cpp` already established. The
// root CMakeLists already defines NOMINMAX/WIN32_LEAN_AND_MEAN project-wide (`CMakeLists.txt:21-23`)
// precisely so a TU pulling in <windows.h> transitively gets the lean definition, which is what
// makes including it from a header acceptable here.
//
// Fails closed: every entry point returns `result<T>` and no branch falls back to a weaker source.
// A CSPRNG failure is `failure_class::fatal` — a handle that cannot be generated securely must not
// be generated at all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/error.hpp"

#if defined(_WIN32)
#include <windows.h>
// <bcrypt.h> must follow <windows.h>.
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cerrno>
#include <cstdio>
#include <sys/random.h>
#endif

namespace agentengine::trust {

// Fills `out` with cryptographically-secure random bytes, or fails. Never partially fills on
// success, and never silently substitutes a weaker source on failure.
[[nodiscard]] inline result<void> secure_random_bytes(std::uint8_t* out, std::size_t len) {
    if (len == 0) return {};
    if (out == nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "secure_random_bytes called with a null buffer",
                                          "secure_random.null_buffer"});
    }
#if defined(_WIN32)
    NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return std::unexpected(ae::error{failure_class::fatal, "BCryptGenRandom failed",
                                          "secure_random.rng_failure"});
    }
    return {};
#else
    // `getrandom` can return a short read when interrupted; loop rather than assume it filled the
    // buffer, and treat EINTR as retryable rather than fatal.
    std::size_t filled = 0;
    while (filled < len) {
        ssize_t n = ::getrandom(out + filled, len - filled, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(ae::error{failure_class::fatal, "getrandom failed",
                                              "secure_random.rng_failure"});
        }
        filled += static_cast<std::size_t>(n);
    }
    return {};
#endif
}

// A lowercase-hex handle of `byte_len` random bytes (so `2 * byte_len` characters). 16 bytes / 128
// bits is the default: comfortably past 011 §8a's "high-entropy" bar for an unguessable handle,
// and the same width `trust/capability_registry.cpp`'s own `generate_ref()` already chose.
[[nodiscard]] inline result<std::string> secure_random_hex(std::size_t byte_len = 16) {
    std::vector<std::uint8_t> raw(byte_len);
    if (auto r = secure_random_bytes(raw.data(), raw.size()); !r) {
        return std::unexpected(r.error());
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(byte_len * 2);
    for (std::uint8_t byte : raw) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

} // namespace agentengine::trust
