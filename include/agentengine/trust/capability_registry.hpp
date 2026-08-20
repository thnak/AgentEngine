#pragma once
// Design B of decisions/ADR-005-capability-bearer-tokens-cross-process.md: the status-quo
// alternative to capability_token.hpp's self-verifying bearer token. No cryptographic token crosses
// the process boundary at all -- only an opaque reference id does. All authority state and every
// check stays host-side, so a remote party must call back and ask "is this still valid" instead of
// verifying locally. This is "each remote path invents its own bespoke authority protocol" (007 §10
// Q1) made concrete enough to measure against Design A.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

// Opaque to whoever holds it -- a hex-encoded 128-bit id, carrying no authority itself. Unlike
// CapabilityToken, this type alone proves nothing; every use requires reaching the issuing
// CapabilityRegistry.
// ae-naming-lint: allow CapabilityRef — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using CapabilityRef = std::string;

// ae-naming-lint: allow RegistryEntry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RegistryEntry {
    capability_kind kind;
    std::string param;
    std::string path_prefix; // "" means unconstrained
    std::chrono::system_clock::time_point expires_at;
};

// Host-side only. A remote process never holds this type -- only a CapabilityRef it must present
// back to whatever RPC stub fronts this registry on every use.
// ae-naming-lint: allow CapabilityRegistry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class CapabilityRegistry {
public:
    result<CapabilityRef> grant(capability_kind kind, std::string param, std::string path_prefix,
                                 std::chrono::system_clock::time_point expires_at);

    // The bespoke per-path check a remote process's host-side stub must perform on every use --
    // this is the callback cost ADR-005 §5 measures against capability_token.hpp's local verify().
    result<void> check(CapabilityRef const& ref, capability_kind requested_kind,
                        std::string const& requested_path,
                        std::chrono::system_clock::time_point now) const;

    // Attenuation here means minting a *new* registry entry with a narrower param/prefix/expiry --
    // the host does this bookkeeping; nothing the remote side holds encodes the narrowing itself.
    result<CapabilityRef> derive_attenuated(CapabilityRef const& parent, std::string narrower_prefix,
                                             std::chrono::system_clock::time_point narrower_expiry);

    void revoke(CapabilityRef const& ref);

    std::size_t entry_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RegistryEntry> entries_;
};

} // namespace agentengine::trust
