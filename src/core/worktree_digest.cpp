// Implements core/worktree.hpp's `compute_digest` -- SHA-256 via Windows CNG/BCrypt (system API,
// not a third-party dependency -- same posture ADR-005's capability_token.cpp already established
// for HMAC-SHA256; this is the unkeyed sibling: BCryptCreateHash with no key argument computes a
// plain hash, not a MAC). Windows-only for now; a Linux backend needs only a different SHA-256
// provider (OpenSSL EVP or libgcrypt) behind this same one function -- a named, tracked gap
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md decision 2), not silently
// assumed available.

#include "agentengine/core/worktree.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <cstdint>

namespace agentengine {

namespace {

result<Digest> bcrypt_error(char const* what, NTSTATUS status) {
    return std::unexpected(error{
        failure_class::fatal,
        std::string(what) + " failed: NTSTATUS 0x" + std::to_string(static_cast<unsigned long>(status)),
        "worktree.digest_failure",
    });
}

char hex_digit(unsigned v) {
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + (v - 10));
}

std::string to_hex(std::span<std::uint8_t const> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(hex_digit(b >> 4));
        out.push_back(hex_digit(b & 0x0F));
    }
    return out;
}

} // namespace

result<Digest> compute_digest(std::span<std::byte const> bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return bcrypt_error("BCryptOpenAlgorithmProvider", status);

    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return bcrypt_error("BCryptCreateHash", status);
    }

    status = BCryptHashData(hash,
                             const_cast<PUCHAR>(reinterpret_cast<UCHAR const*>(bytes.data())),
                             static_cast<ULONG>(bytes.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return bcrypt_error("BCryptHashData", status);
    }

    std::array<std::uint8_t, 32> digest{};
    status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(status)) return bcrypt_error("BCryptFinishHash", status);

    return to_hex(digest);
}

} // namespace agentengine
