// Implements trust/hmac.hpp. Extracted verbatim from capability_token.cpp (ADR-005) -- see that
// header's own comment for why this is now a shared primitive rather than two independent copies.

#include "agentengine/trust/hmac.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace agentengine::trust {

namespace {

std::unexpected<ae::error> bcrypt_error(char const* what, NTSTATUS status) {
    return std::unexpected(ae::error{
        failure_class::fatal,
        std::string(what) + " failed: NTSTATUS 0x" + std::to_string(static_cast<unsigned long>(status)),
        "hmac.bcrypt_failure",
    });
}

} // namespace

result<HmacSha256> hmac_sha256(std::uint8_t const* key, std::size_t key_len,
                                std::uint8_t const* data, std::size_t data_len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                   BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) {
        return bcrypt_error("BCryptOpenAlgorithmProvider", status);
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(alg, &hash, nullptr, 0,
                               const_cast<PUCHAR>(key), static_cast<ULONG>(key_len), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return bcrypt_error("BCryptCreateHash", status);
    }

    status = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(data_len), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return bcrypt_error("BCryptHashData", status);
    }

    HmacSha256 out{};
    status = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return bcrypt_error("BCryptFinishHash", status);
    }
    return out;
}

bool constant_time_equal(HmacSha256 const& a, HmacSha256 const& b) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < kHmacSha256Bytes; ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace agentengine::trust
