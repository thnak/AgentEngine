// Implements trust/capability_registry.hpp. See that header for the ADR this satisfies.

#include "agentengine/trust/capability_registry.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

namespace agentengine::trust {

namespace {

result<std::string> generate_ref() {
    std::array<std::uint8_t, 16> raw{};
    NTSTATUS status = BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(raw.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return std::unexpected(ae::error{failure_class::fatal,
                                          "BCryptGenRandom failed for capability ref",
                                          "capability_registry.rng_failure"});
    }
    std::string hex;
    hex.reserve(32);
    static char const* digits = "0123456789abcdef";
    for (auto byte : raw) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    return hex;
}

} // namespace

result<CapabilityRef> CapabilityRegistry::grant(capability_kind kind, std::string param,
                                                 std::string path_prefix,
                                                 std::chrono::system_clock::time_point expires_at) {
    auto ref = generate_ref();
    if (!ref.has_value()) {
        return std::unexpected(ref.error());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[*ref] = RegistryEntry{kind, std::move(param), std::move(path_prefix), expires_at};
    return *ref;
}

result<void> CapabilityRegistry::check(CapabilityRef const& ref, capability_kind requested_kind,
                                        std::string const& requested_path,
                                        std::chrono::system_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(ref);
    if (it == entries_.end()) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability ref unknown or revoked",
                                          "capability_registry.unknown_ref"});
    }
    RegistryEntry const& entry = it->second;
    if (entry.kind != requested_kind) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability ref kind mismatch",
                                          "capability_registry.kind_mismatch"});
    }
    if (now >= entry.expires_at) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability ref expired",
                                          "capability_registry.expired"});
    }
    if (!entry.path_prefix.empty() &&
        requested_path.compare(0, entry.path_prefix.size(), entry.path_prefix) != 0) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability ref path-prefix not satisfied",
                                          "capability_registry.path_denied"});
    }
    return {};
}

result<CapabilityRef> CapabilityRegistry::derive_attenuated(
    CapabilityRef const& parent, std::string narrower_prefix,
    std::chrono::system_clock::time_point narrower_expiry) {
    RegistryEntry parent_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(parent);
        if (it == entries_.end()) {
            return std::unexpected(ae::error{failure_class::policy,
                                              "cannot derive from unknown or revoked parent ref",
                                              "capability_registry.unknown_parent"});
        }
        parent_copy = it->second;
    }
    // Narrowing is enforced here, host-side, on every derivation -- nothing the caller presents
    // (it never presents anything but the opaque parent ref) can widen past parent_copy's own
    // fields (007 §3 rule 2). A prefix that does not extend the parent's is rejected rather than
    // silently accepted.
    if (!parent_copy.path_prefix.empty() &&
        narrower_prefix.compare(0, parent_copy.path_prefix.size(), parent_copy.path_prefix) != 0) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "derived prefix does not extend parent prefix",
                                          "capability_registry.widen_rejected"});
    }
    if (narrower_expiry > parent_copy.expires_at) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "derived expiry exceeds parent expiry",
                                          "capability_registry.widen_rejected"});
    }
    auto ref = generate_ref();
    if (!ref.has_value()) {
        return std::unexpected(ref.error());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[*ref] = RegistryEntry{parent_copy.kind, parent_copy.param, std::move(narrower_prefix),
                                    narrower_expiry};
    return *ref;
}

void CapabilityRegistry::revoke(CapabilityRef const& ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(ref);
}

std::size_t CapabilityRegistry::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace agentengine::trust
